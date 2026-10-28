// NFSMWDrawAll.asi — Need for Speed: Most Wanted, speed.exe 1.3
// Config: NFSMWDrawAll.ini  |  Log: NFSMWDrawAll.log
// Build with the x86 Native Tools prompt; see build.bat.
// PreloadWholeCity requires Large Address Aware (4GB patch) on speed.exe.

#include "AsiKit.h"   // shared\asi: Log, ini, byte-patch groups, crash log
#include <ctype.h>

using namespace asikit;

// Live state
// Exported for tools that inspect or modify the plugin state.
#pragma pack(push, 1)
struct ModState {
    char magic[8];          // "NFSMWDA"
    uint32_t version;       // layout version
    uint32_t drawAll;       // 1 = main view draws every loaded section (live toggle)
    int32_t lastZone;       // id of the zone the camera is in, -1 = none
    uint8_t skip[352];      // bit per section id (2800): never drawn by DrawAll
    uint8_t home[352];      // bit per section id: hidden while the camera is in its district
};
#pragma pack(pop)
extern "C" __declspec(dllexport) ModState g_state = {"NFSMWDA", 1, 0, -1, {0}, {0}};

// ------------------------------------------------------------------ game addresses
static const uintptr_t STREAMER = 0x9AA9D0;        // track streamer (this of every hook below)
static const uintptr_t ZONES = 0x9A3B50;           // zone manager + scenery registry
static const uintptr_t ZONE_LIST = ZONES + 0x10;   // sentinel of the 0x34153 zone-record list
static const uintptr_t SCENERY_TABLE = ZONES + 0x60;   // id -> registry entry (entry+4 = section)
static const unsigned SCENERY_IDS = 0xAF0;         // 2800
static const unsigned SCENERY_FREE = 0x6424;       // registry free list (512 entries of 0x1C)
static const uintptr_t GAME_STATE = 0x925E90;      // 6 = in the world
static const uintptr_t CAMERA_ZONE = 0x90214C;     // zone id of the main view, set by VisibleList
static const unsigned EXTRA_NODES = 4096;          // extra pool block descriptors (orig 0xC0)
static const unsigned EXTRA_SCENERY = 1536;        // extra scenery registry entries (orig 512)
static const unsigned POOL_OBJ_SIZE = 0x2750;      // "Track Streaming" pool object
static const unsigned RENDER_LIST_ENTRIES = 60000; // orig static list holds 5000
static const uint32_t FRAME_POOL_BYTES = 0x400000; // per-frame matrix pool (orig 0x64000)
static const uint32_t RENDER_QUEUE = 0x20000;      // D3D mesh queue entries of 0x44 bytes (orig 0x1000)

// absolute targets used from inline asm (indirect jmp/call through these)
static DWORD kStreamerService = 0x730370;   // per-frame streamer update
static DWORD kUnloadAll = 0x728730;
static DWORD kDrawVisibleResume = 0x731B16;

typedef void(__fastcall *StartLoading_t)(void *streamer, int, short *ids);   // thiscall, ret 4
typedef void(__fastcall *Thiscall0_t)(void *streamer);
typedef int(__stdcall *VisibleList_t)(short *ids, int max, void *view);
typedef void(__fastcall *SectionDraw_t)(void *section, int, void *view);     // thiscall, ret 4
static const StartLoading_t StartLoading = (StartLoading_t)0x730460;
static const Thiscall0_t WaitLoaded = (Thiscall0_t)0x730390;
static const Thiscall0_t ForceRefresh = (Thiscall0_t)0x7317A0;   // determine sections from cars, load, wait
static const VisibleList_t VisibleList = (VisibleList_t)0x731870;
static const SectionDraw_t SectionDraw = (SectionDraw_t)0x729640;

// ------------------------------------------------------------------ buffers
static DWORD g_preloadedFor;                                          // section array already preloaded
static __declspec(align(16)) uint8_t g_sceneryBuf[EXTRA_SCENERY * 0x1C];
static __declspec(align(16)) uint8_t g_renderList[RENDER_LIST_ENTRIES * 12];

static bool Bit(const uint8_t *bits, unsigned i) { return (bits[i >> 3] >> (i & 7)) & 1; }

// ------------------------------------------------------------------ slot pools
// Fixed-size pools (bNewSlotPool FUN_004662E0, cdecl(slotSize, count, name, flags)); the
// fast allocator FUN_00463C70 returns NULL when one is full and the callers crash
// (first whole-city run: TexturePackSlotPool, 128 texture packs, at 0x5060B0).
// Pool header: +0xC name, +0x18 used, +0x1C peak used, +0x24 count, +0x28 slot size.
struct SlotPoolSize { const char *name; int count; };
static const SlotPoolSize SLOT_POOLS[] = {
    {"TexturePackSlotPool", 2048},          // 128
    {"g_textureHeaderPool", 16384},         // 1024
    {"g_vertexBufferHeaderPool", 16384},    // 1024
    {"WorldModelSlotPool", 1024},           // 128
    {"eLightMaterialPlatInfoSlotPool", 1760},   // 220
    {"eAnimTextureSlotPool", 2048},         // 256
    {"SpaceNodeSlotPool", 640},             // 80
    {"ShadowMapMeshSlotPool", 160},         // 40
    {"WorldAnimEntity_SlotPool", 852},      // 213
    {"WorldAnimEntityTree_SlotPool", 264},  // 66
    {"WorldAnimEntityTreeInfo_SlotPool", 156},  // 39
    {"WorldAnimCtrl_SlotPool", 596},        // 149
    {"WorldAnimInstanceEntry_SlotPool", 264},   // 66
    {"eMeshRender", (int)RENDER_QUEUE},     // 0x1000, sized like the mesh queue
};
typedef uint8_t *(__cdecl *NewPool_t)(int slotSize, int count, const char *name, int flags);
static const NewPool_t NewPool = (NewPool_t)0x465D80;
static uint8_t *g_pools[256];
static unsigned g_poolCount;

static uint8_t *__cdecl NewSlotPool(int slotSize, int count, const char *name, int flags)
{
    for (auto &p : SLOT_POOLS) {
        if (name && strcmp(name, p.name) == 0 && count < p.count) {
            Log("  %s: %d -> %d slots", name, count, p.count);
            count = p.count;
        }
    }
    uint8_t *pool = NewPool((slotSize + 3) & ~3, count, name, flags);
    if (!pool)
        return nullptr;
    if (*(int *)0x910EF8 == 1) {                 // the game's own pool list (sentinel 0x910EFC)
        uint8_t **tail = (uint8_t **)0x910F00;
        *(uint8_t **)*tail = pool;
        ((uint8_t **)pool)[1] = *tail;
        *tail = pool;
        *(uintptr_t *)pool = 0x910EFC;
    }
    if (g_poolCount < _countof(g_pools))
        g_pools[g_poolCount++] = pool;
    return pool;
}

// pools at >= minPercent of their size (peak use)
static void LogPools(unsigned minPercent)
{
    for (unsigned i = 0; i < g_poolCount; i++) {
        uint32_t h[11];
        if (!SafeRead((uintptr_t)g_pools[i], h, sizeof h))
            continue;
        char name[40] = "?";
        SafeRead(h[3], name, sizeof name - 1);
        if (h[9] && h[7] * 100 >= h[9] * minPercent)
            Log("  pool %-34s used %u peak %u of %u", name, h[6], h[7], h[9]);
    }
}

// ------------------------------------------------------------------ streaming hooks
// complete(this): phase (+0x188) == 0 && (#needed sections not active) <= postponed (+0x28).
// Replaces every "active + postponed >= needed" test: pinned sections stay active without
// being on the needed list (+0x2C0 count, +0x2C4 array). Keeps ecx.
static __declspec(naked) void Complete()
{
    __asm {
        push esi
        push edi
        xor edi, edi
        cmp dword ptr [ecx+0x188], 0
        jne c_no
        mov edx, [ecx+0x2C0]
        lea eax, [ecx+0x2C4]
    c_loop:
        test edx, edx
        jle c_chk
        mov esi, [eax]
        cmp dword ptr [esi+0xC], 4
        je c_next
        inc edi
    c_next:
        add eax, 4
        dec edx
        jmp c_loop
    c_chk:
        cmp edi, [ecx+0x28]
        jg c_no
        mov eax, 1
        pop edi
        pop esi
        ret
    c_no:
        xor eax, eax
        pop edi
        pop esi
        ret
    }
}

// 0x7303D0: returns "loading still pending" (the game's loop there never runs)
static __declspec(naked) void Pending()
{
    __asm {
        call Complete
        test eax, eax
        sete al
        ret
    }
}

// needed-list reset (0x723960): keep the pin bit 0x80 instead of zeroing the mask
static __declspec(naked) void ClearNeeded()
{
    __asm {
        mov esi, [ecx]
        and byte ptr [esi+0xB], 0x80
        ret
    }
}

// ActivateSection (0x727E30): state 4 + pin bit
static __declspec(naked) void Activate()
{
    __asm {
        mov dword ptr [esi+0xC], 4
        or byte ptr [esi+0xB], 0x80
        ret
    }
}

// FreeSection (0x727EBF, ebx = 0): really freed -> unpin
static __declspec(naked) void FreeSection()
{
    __asm {
        mov [esi+0x4C], ebx
        mov [esi+0xC], ebx
        mov [esi+0x40], ebx
        and byte ptr [esi+0xB], 0x7F
        ret
    }
}

// UnloadAll call when the section chunk goes away (0x72BF10): forget the preloaded region
static __declspec(naked) void UnloadAllHook()
{
    __asm {
        mov g_preloadedFor, 0
        jmp dword ptr [kUnloadAll]
    }
}

// Visit every zone record once: load what it needs, then return to the car position.
static void Preload(uint8_t *streamer)
{
    unsigned zones = 0;
    for (uint8_t *rec = *(uint8_t **)ZONE_LIST; rec && (uintptr_t)rec != ZONE_LIST; rec = *(uint8_t **)rec) {
        short ids[2] = {*(short *)(rec + 0xC), 0};
        if (ids[0] <= 0)
            continue;
        StartLoading(streamer, 0, ids);
        WaitLoaded(streamer);
        zones++;
    }
    ForceRefresh(streamer);
    unsigned active = 0, n = *(unsigned *)(streamer + 4);
    uint8_t *sec = *(uint8_t **)streamer;
    for (unsigned i = 0; i < n; i++)
        active += *(int *)(sec + i * 0x5C + 0xC) == 4;
    Log("  %u zones visited, %u of %u sections active; slot pools over 50%%:", zones, active, n);
    LogPools(50);
}

// Main loop (FUN_00663D30, call at 0x663EA9 to the per-frame streamer update): once the
// game is in the world (state 6) preload the region from here, at the top level of a frame
static void __cdecl MainLoopService()
{
    uint8_t *s = (uint8_t *)STREAMER;
    if (*(int *)GAME_STATE == 6 && s[0x2B1] == 0 && *(void **)(s + 0x838) &&   // in game, streaming on, pool
        *(DWORD *)s && *(DWORD *)s != g_preloadedFor) {                       // new region
        g_preloadedFor = *(DWORD *)s;
        DWORD t = GetTickCount();
        Preload(s);
        Log("preloaded region (section array %08X) in %u ms", g_preloadedFor, GetTickCount() - t);
    }
}

static __declspec(naked) void MainLoopHook()
{
    __asm {
        push ecx
        call MainLoopService
        pop ecx
        jmp dword ptr [kStreamerService]
    }
}

// Pool init (0x72DA34): append EXTRA_NODES block descriptors (0x34 bytes) to the free list
// (+0x20 sentinel, +0x24 tail) of the enlarged pool object. Keeps ecx (SEH frame).
static __declspec(naked) void PoolNodes()
{
    __asm {
        mov [esi+0x844], eax             // displaced
        test eax, eax
        je n_done
        push ecx
        push edx
        push edi
        push ebx
        lea edi, [eax+0x2750]            // POOL_OBJ_SIZE
        mov ebx, 4096                    // EXTRA_NODES
    n_loop:
        mov edx, [eax+0x24]
        mov [edx], edi
        mov [eax+0x24], edi
        lea ecx, [eax+0x20]
        mov [edi], ecx
        mov [edi+4], edx
        add edi, 0x34
        dec ebx
        jne n_loop
        pop ebx
        pop edi
        pop edx
        pop ecx
    n_done:
        ret
    }
}

// Scenery registry: append EXTRA_SCENERY free entries (0x1C bytes) to the free list
static void __cdecl AppendScenery(uint8_t *registry)
{
    uintptr_t *head = (uintptr_t *)(registry + SCENERY_FREE);
    for (unsigned i = 0; i < EXTRA_SCENERY; i++) {
        uintptr_t *e = (uintptr_t *)(g_sceneryBuf + i * 0x1C);
        uintptr_t *tail = (uintptr_t *)head[1];
        tail[0] = (uintptr_t)e;
        head[1] = (uintptr_t)e;
        e[0] = (uintptr_t)head;
        e[1] = (uintptr_t)tail;
    }
}

// registry ctor tail (0x72EACA), when the ctor has not run yet at plugin load
static __declspec(naked) void SceneryCtorTail()
{
    __asm {
        push esi
        call AppendScenery
        add esp, 4
        pop edi                          // displaced tail
        mov eax, esi
        pop esi
        pop ebp
        pop ebx
        ret
    }
}

// ------------------------------------------------------------------ draw-all
// Main view: every loaded scenery section (FUN_00729640 still frustum-culls each one).
static void __stdcall DrawAllMain(uint8_t *view)
{
    short ids[128];
    VisibleList(ids, 128, view);                       // keeps the game's camera-zone tracking
    g_state.lastZone = *(int *)CAMERA_ZONE;
    unsigned district = (unsigned)g_state.lastZone / 100;     // -1 -> no district
    uint8_t **table = (uint8_t **)SCENERY_TABLE;
    for (unsigned i = 0; i < SCENERY_IDS; i++) {
        if (Bit(g_state.skip, i) || (Bit(g_state.home, i) && i / 100 == district))
            continue;
        uint8_t *entry = table[i];
        uint8_t *section = entry ? *(uint8_t **)(entry + 4) : nullptr;
        if (section && *(void **)(section + 0x28))
            SectionDraw(section, 0, view);
    }
}

// DrawVisibleSections (0x731B10) entry
static __declspec(naked) void DrawVisibleHook()
{
    __asm {
        cmp g_state.drawAll, 0
        je d_orig
        mov eax, [esp+4]
        mov eax, [eax+0x80]
        cmp dword ptr [eax+4], 1         // main player view
        jne d_orig
        jmp DrawAllMain                  // __stdcall(view), ret 4
    d_orig:
        sub esp, 0x100                   // displaced
        jmp dword ptr [kDrawVisibleResume]
    }
}

// ------------------------------------------------------------------ settings
// "A99, C97 ..." -> bits
static void IniSections(const char *sec, const char *key, const char *def, uint8_t *bits, const char *what)
{
    char buf[4096];
    IniString(sec, key, def, buf, sizeof buf);
    char list[4096] = "";
    char *ctx = nullptr;
    for (char *t = strtok_s(buf, " ,\t", &ctx); t; t = strtok_s(nullptr, " ,\t", &ctx)) {
        char c = (char)(t[0] & ~0x20);
        size_t len = strlen(t);
        bool digits = (len == 2 || len == 3) && isdigit((uint8_t)t[1]) && (len == 2 || isdigit((uint8_t)t[2]));
        int n = digits ? atoi(t + 1) : -1;
        if (c < 'A' || c > 'Z' || n < 0 || n > 99) {
            Log("  %s: ignored '%s'", what, t);
            continue;
        }
        unsigned id = (unsigned)(c - 'A' + 1) * 100 + n;
        if (id < SCENERY_IDS) {
            bits[id >> 3] |= 1 << (id & 7);
            sprintf_s(list + strlen(list), sizeof list - strlen(list), " %c%02d", c, n);
        }
    }
    Log("  %s:%s", what, list[0] ? list : " none");
}

// Large Address Aware flag of speed.exe (4GB patch) and the address space it really got
static bool LargeAddressAware()
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    bool flag = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    MEMORYSTATUSEX ms = {sizeof ms};
    GlobalMemoryStatusEx(&ms);
    Log("speed.exe Large Address Aware: %s, address space %u MB", flag ? "yes" : "NO",
        (unsigned)(ms.ullTotalVirtual >> 20));
    return flag && ms.ullTotalVirtual > 0xA0000000ull;
}

// ------------------------------------------------------------------ crash diagnostics
static void CrashDetails(EXCEPTION_POINTERS *)
{
    uint32_t s[4] = {};
    SafeRead(STREAMER + 0x24, &s[0], 8);          // active, postponed
    SafeRead(STREAMER + 0x188, &s[2], 4);         // loading phase
    SafeRead(STREAMER + 0x2C0, &s[3], 4);         // needed
    Log("  drawAll %u lastZone %d preloaded %08X | streamer active %u postponed %u phase %u needed %u",
        g_state.drawAll, g_state.lastZone, g_preloadedFor, s[0], s[1], s[2], s[3]);
    Log("  slot pools over 90%%:");
    LogPools(90);
}

static const char *DEFAULT_SKIP = "";
static const char *DEFAULT_HOME = "";

static void Install(HMODULE self)
{
    Init(self, "NFSMWDrawAll");
    if (IniInt("Debug", "CrashLog", 1))
        InstallCrashLog(0x401000, 0x88F2A5, CrashDetails);   // speed.exe .text

    
    if (!IsCommitted(0x731B10) || memcmp((void *)0x731B10, "\x81\xEC\x00\x01\x00\x00\x53", 7) != 0) {
        Log("speed.exe is not the analysed v1.3 build (or already patched): nothing done.");
        return;
    }

    // ---- 1. whole city in memory
    if (IniInt("Streaming", "PreloadWholeCity", 1)) {
        int poolMB = IniInt("Streaming", "PoolSizeMB", 640);
        if (!LargeAddressAware()) {
            Log("[PreloadWholeCity] NOT applied: speed.exe needs the 4GB patch (Large Address Aware)"
                " for a %d MB pool. Apply it to speed.exe and restart.", poolMB);
        } else {
            Group g("PreloadWholeCity");
            if (poolMB >= 128 && poolMB <= 1536) {
                std::vector<uint8_t> a = {0xB8};
                U32(a, (uint32_t)poolMB << 20);
                g.Add(0x664A35, "b80040ef07", a);                                // pool size
                g.Add(0x664A42, "b80040eb07", a);                                // variant
            }
            {
                std::vector<uint8_t> n = {0x68};
                U32(n, POOL_OBJ_SIZE + EXTRA_NODES * 0x34);
                g.Add(0x72D9ED, "6850270000", n);                                // bigger pool object
            }
            g.Add(Branch(0x72DA34, "898644080000", 0xE8, PoolNodes));
            g.Add(Branch(0x4662E0, "8b442410 8b4c240c", 0xE9, NewSlotPool));     // bigger slot pools
            g.Add(Branch(0x727E30, "c7460c04000000", 0xE8, Activate));
            g.Add(Branch(0x723960, "8b31 c6460b00", 0xE8, ClearNeeded));
            g.Add(Branch(0x727EBF, "895e4c 895e0c 895e40", 0xE8, FreeSection));
            g.Add(Branch(0x723A80, "8b8188010000", 0xE9, Complete));           // IsLoadingComplete
            {   // WaitLoaded loop condition
                Patch p{0x730393, Hex("8b8688010000 85c0 7512 8b4628 8b5624 8b8ec0020000 03c2 3bc1 7d18"), Hex("8BCE E8")};
                Rel32(p.repl, 0x73039A, Complete);
                auto t = Hex("85C0 7529");                                       // done -> 0x7303C7
                p.repl.insert(p.repl.end(), t.begin(), t.end());
                g.Add(p);
            }
            g.Add(Branch(0x7303D0, "53 56 8bf1 8b8e88010000", 0xE9, Pending));
            g.Add(Branch(0x663EA9, "e8c2c40c00", 0xE8, MainLoopHook));          // main loop: streamer update
            g.Add(Branch(0x72BF10, "e81bc8ffff", 0xE8, UnloadAllHook));
            bool built = *(uintptr_t *)(ZONES + SCENERY_FREE) != 0;              // static ctor already ran?
            if (!built)
                g.Add(Branch(0x72EACA, "5f 8bc6 5e 5d 5b c3", 0xE9, SceneryCtorTail));
            if (g.Apply()) {
                if (built)
                    AppendScenery((uint8_t *)ZONES);
                Log("  pool %d MB, +%u pool descriptors, +%u scenery entries", poolMB, EXTRA_NODES, EXTRA_SCENERY);
            }
        }
    }

    // ---- 2. draw every loaded section
    if (IniInt("Render", "DrawAll", 1)) {
        IniSections("Render", "HideSections", DEFAULT_SKIP, g_state.skip, "HideSections");
        IniSections("Render", "HideInOwnDistrict", DEFAULT_HOME, g_state.home, "HideInOwnDistrict");
        Group g("DrawAll");
        g.Add(Branch(0x731B10, "81ec00010000", 0xE9, DrawVisibleHook));
        std::vector<uint8_t> a = {0xB8};                                         // render list -> our buffer
        U32(a, (uint32_t)(uintptr_t)g_renderList);
        g.Add(0x731B86, "b868439900", a);
        std::vector<uint8_t> b = Hex("C785CC090000");
        U32(b, (uint32_t)(uintptr_t)(g_renderList + sizeof g_renderList));
        g.Add(0x731BA1, "c785cc090000c82d9a00", b);
        g.Add(0x731CCE, "7426", Hex("eb26"));                                    // preculler off
        std::vector<uint8_t> push = {0x68}, add = {0x05}, mov = Hex("C7057C5F9100");
        U32(push, FRAME_POOL_BYTES), U32(add, FRAME_POOL_BYTES), U32(mov, FRAME_POOL_BYTES);
        g.Add(0x5009D1, "6800400600", push);                                     // matrix pool malloc #1
        g.Add(0x5009DB, "6800400600", push);                                     // malloc #2
        g.Add(0x500A00, "0500400600", add);                                      // end
        g.Add(0x500A0C, "c7057c5f910000400600", mov);                            // size DAT_00915F7C
        // D3D mesh queue (FUN_006E0AB0 & co.): static 0x1000 x 0x44 at 0x93E878 dropped every
        // mesh past 4096 per frame, so most of the city vanished depending on the view angle
        uint8_t *queue = (uint8_t *)VirtualAlloc(nullptr, RENDER_QUEUE * 0x44, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!queue) {
            Log("[DrawAll] NOT applied: no memory for the mesh queue");
            return;
        }
        std::vector<uint8_t> cmp = Hex("81FE"), base = Hex("81C6"), reserve = {0x68};
        U32(cmp, RENDER_QUEUE), U32(base, (uint32_t)(uintptr_t)queue), U32(reserve, RENDER_QUEUE);
        for (uintptr_t va : {0x6DFB71u, 0x6DFEC1u, 0x6E0799u, 0x6E0AD2u})
            g.Add(va, "81fe00100000", cmp);                                     // count < 0x1000
        for (uintptr_t va : {0x6DFB8Du, 0x6DFEDDu, 0x6E0249u, 0x6E07B1u, 0x6E0AEAu})
            g.Add(va, "81c678e89300", base);                                    // + 0x93E878
        g.Add(0x6E42F5, "6800100000", reserve);                                  // sort keys vector reserve
        if (g.Apply())
            g_state.drawAll = 1;
    }

    // ---- 3. minimum on-screen size of scenery objects (FUN_00729120: cmp ebx,imm8)
    int px = IniInt("Render", "MinObjectPixels", 11);
    if (px >= 2 && px <= 127) {
        Group g("MinObjectPixels");
        g.Add(0x729285, "83fb17", {0x83, 0xFB, (uint8_t)px});                   // high world detail
        g.Add(0x729296, "83fb12", {0x83, 0xFB, (uint8_t)px});                   // lower detail
        if (g.Apply())
            Log("  %d px", px);
    } else {
        Log("[MinObjectPixels] %d: keeping the game's 23/18", px);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Install(module);
    }
    return TRUE;
}
