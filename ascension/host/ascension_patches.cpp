// Game patches (RPCS3's, and the ones ported to 1.00) switched when the game
// starts, not built in.
//
// RPCS3 patches a game in memory as it runs. This port translates the code
// ahead of time, so a patch used to mean writing it into EBOOT.ELF and
// translating and building again. Now tools/rpcs3_patch.py runtime lists every
// patch's writes (host/ascension_patch_table.inc: guest address, original and
// patched bytes), the PPU instructions they change are lifted both ways
// (ppu_lifter.py --patch-sites: the word in guest memory picks the
// translation), and the SPU programs they change are lifted in both versions
// (matched by their bytes when the game loads them). Before the game runs,
// ascension_after_load() writes every patch over the loaded executable: the
// patched bytes for a patch that is on, the original bytes for one that is
// off, so the switch holds whether the disc's EBOOT.ELF was patched or not.
//
//   PS3_PATCHES=<name>|<name>...  the patches on, by name (the launcher's
//                                 list); "none" or empty: none. Unset: the
//                                 build's defaults (the table's default flags).
//   PS3_PATCH_LIST=1              print the patches this build can switch (JSON
//                                 between PATCH-LIST-BEGIN/END lines) and exit,
//                                 before anything starts: the launcher's
//                                 Patches page, where no patch files exist.
//   PS3_MP_MODE=<mode>            with "Offline Multiplayer (experimental)" on:
//                                 the mode Quick Play starts: trial1 (default),
//                                 trial2, favor4, favor8, team4, team8, ctf.
//   PS3_MP_MAP=<n>|cycle          the map of the first match: 1 = the first of
//                                 the mode's list, 2 = the second, ...; cycle
//                                 (default) = the game's own shuffled order.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "ppu_recomp.h"   /* the generated header: vm_base and the guest loads */

namespace {

struct ascension_patch_write { uint32_t addr, len, orig, patched; };   // offsets into k_patch_bytes
struct ascension_patch {
    const char* name; const char* kind; int default_on; uint32_t first, count;
    const char* author; const char* file; const char* notes;
};
#include "ascension_patch_table.inc"

constexpr int kPatches = (int)(sizeof k_patches / sizeof k_patches[0]);

// PS3_PATCHES from the OS environment, not getenv(): the defaults code asks
// during static initialisation, before the boot harness's getenv cache exists
// (ascension_defaults.cpp reads its variables the same way).
bool patches_env(std::string& out)
{
#ifdef _WIN32
    SetLastError(0);
    const DWORD n = GetEnvironmentVariableA("PS3_PATCHES", nullptr, 0);
    if (!n) return GetLastError() != ERROR_ENVVAR_NOT_FOUND;   // set but empty
    out.resize(n);
    out.resize(GetEnvironmentVariableA("PS3_PATCHES", &out[0], n));
    return true;
#else
    const char* e = getenv("PS3_PATCHES");
    if (e) out = e;
    return e != nullptr;
#endif
}

// Which patches are on, decided once (the defaults code asks before main for
// Unlock FPS; the loader asks after the image is in memory).
const bool* patch_state()
{
    static bool on[kPatches];
    static bool done;
    if (done) return on;
    done = true;
    std::string list;
    const bool set = patches_env(list);
    for (int i = 0; i < kPatches; i++) on[i] = set ? false : k_patches[i].default_on != 0;
    if (!set) return on;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t bar = list.find('|', pos);
        if (bar == std::string::npos) bar = list.size();
        std::string name = list.substr(pos, bar - pos);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        while (!name.empty() && (name[0] == ' ' || name[0] == '\t')) name.erase(0, 1);
        if (!name.empty() && name != "none") {
            int found = -1;
            for (int i = 0; i < kPatches; i++)
                if (name == k_patches[i].name) found = i;
            if (found >= 0) on[found] = true;
            else printf("[patches] unknown patch \"%s\" (not in this build's list)\n", name.c_str());
        }
        pos = bar + 1;
    }
    return on;
}

} // namespace

extern "C" int ascension_patch_on(const char* name)
{
    const bool* on = patch_state();
    for (int i = 0; i < kPatches; i++)
        if (!strcmp(name, k_patches[i].name)) return on[i] ? 1 : 0;
    return 0;
}

// The in-game settings menu (ps3kit libs/video/rsx_ingame_menu.h): one patch
// switched in the RUNNING game. Only a PPU patch that is a single instruction
// word: the lifted code reads that word each time it runs there (--patch-sites:
// "the word in guest memory picks the translation"), and one aligned 4-byte
// store is seen whole by the game's threads. A patch of several writes could be
// caught half switched, and an SPU patch is matched when its program loads, so
// both are refused. on: 1 / 0 = switch it; -1 = only ask whether it can be
// switched. Returns 1 when done (or possible), -1 when not.
// Used for "Disable Motion Blur": AMD FSR reads the moving objects from the
// game's own motion-blur draws, so the menu turns the blur on with FSR.
extern "C" int ascension_patch_set(const char* name, int on)
{
    if (!vm_base) return -1;
    bool* state = const_cast<bool*>(patch_state());
    for (int i = 0; i < kPatches; i++) {
        const ascension_patch& p = k_patches[i];
        if (strcmp(name, p.name)) continue;
        if (strcmp(p.kind, "PPU") || p.count != 1) return -1;
        const ascension_patch_write& pw = k_patch_writes[p.first];
        if (pw.len != 4 || (pw.addr & 3u)) return -1;
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + pw.addr, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mi.Protect & PAGE_GUARD))
            return -1;
#endif
        if (on < 0) return 1;
        uint32_t word;
        memcpy(&word, k_patch_bytes + (on ? pw.patched : pw.orig), 4);
#ifdef _WIN32
        InterlockedExchange(reinterpret_cast<volatile LONG*>(vm_base + pw.addr), (LONG)word);
#else
        memcpy(vm_base + pw.addr, &word, 4);
#endif
        state[i] = on != 0;
        printf("[patches] %s switched %s in the running game (0x%08X)\n", p.name, on ? "on" : "off", pw.addr);
        fflush(stdout);
        return 1;
    }
    return -1;
}

namespace {
void json_str(const char* s)
{
    putchar('"');
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);   // UTF-8 passes through
    }
    putchar('"');
}
} // namespace

// ascension_pre_main (ascension_defaults.cpp), before anything starts:
// PS3_PATCH_LIST=1 prints the table and returns 0 (the process exits), else -1.
extern "C" int ascension_patch_list_main(void)
{
#ifdef _WIN32
    char v[8];
    const DWORD n = GetEnvironmentVariableA("PS3_PATCH_LIST", v, sizeof v);
    if (!n || n >= sizeof v || v[0] != '1') return -1;
#else
    const char* v = getenv("PS3_PATCH_LIST");
    if (!v || v[0] != '1') return -1;
#endif
    printf("PATCH-LIST-BEGIN\n[");
    for (int i = 0; i < kPatches; i++) {
        const ascension_patch& p = k_patches[i];
        printf("%s\n {\"name\": ", i ? "," : "");
        json_str(p.name);
        printf(", \"kind\": ");
        json_str(p.kind);
        printf(", \"default\": %s, \"author\": ", p.default_on ? "true" : "false");
        json_str(p.author);
        printf(", \"file\": ");
        json_str(p.file);
        printf(", \"notes\": ");
        json_str(p.notes);
        printf("}");
    }
    printf("\n]\nPATCH-LIST-END\n");
    fflush(stdout);
    return 0;
}

// Offline Multiplayer: the launcher's choice of mode and map, written once into
// the three words the patch reads (patches-rpcs3/ascension-1.00.yml, "Offline
// Multiplayer (experimental)"): mode 0x819960, map 0x819964, shuffle 0x819968.
// They sit in alignment padding of the executable's data (zero on disc, used by
// nothing else) and are written here, after the patch table and before the
// game's first instruction; nothing changes them while the game runs. With the
// patch off nothing is written (the table has put the disc's zeros back).
namespace {
struct mp_mode_row { const char* key; uint32_t mode; uint32_t maps; const char* name; };
// mode = the game's own index (GameModes.bin); maps = levels in its built-in cycle
const mp_mode_row k_mp_modes[] = {
    { "trial1", 9, 3, "Trial of the Gods, 1 player" },
    { "trial2", 8, 3, "Trial of the Gods, 2 players co-op" },
    { "favor4", 4, 4, "Favor of the Gods, 4 players" },
    { "favor8", 5, 4, "Favor of the Gods, 8 players" },
    { "team4",  6, 4, "Team Favor of the Gods, 2v2" },
    { "team8",  7, 4, "Team Favor of the Gods, 4v4" },
    { "ctf",    3, 4, "Capture the Flag, 4v4" },
};
void mp_put32(uint32_t addr, uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    memcpy(vm_base + addr, b, 4);
}
// From the OS environment, as PS3_PATCHES is read (no getenv cache this early).
std::string mp_env(const char* name)
{
    std::string out;
#ifdef _WIN32
    const DWORD n = GetEnvironmentVariableA(name, nullptr, 0);
    if (n) { out.resize(n); out.resize(GetEnvironmentVariableA(name, &out[0], n)); }
#else
    const char* e = getenv(name);
    if (e) out = e;
#endif
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    while (!out.empty() && (out[0] == ' ' || out[0] == '\t')) out.erase(0, 1);
    return out;
}
void mp_choice_apply()
{
    if (!ascension_patch_on("Offline Multiplayer (experimental)")) return;
    const std::string mode = mp_env("PS3_MP_MODE"), map = mp_env("PS3_MP_MAP");
    const mp_mode_row* row = &k_mp_modes[0];
    if (!mode.empty()) {
        const mp_mode_row* found = nullptr;
        for (const mp_mode_row& r : k_mp_modes)
            if (mode == r.key) found = &r;
        if (found) row = found;
        else printf("[mp] unknown PS3_MP_MODE \"%s\": %s is used\n", mode.c_str(), row->key);
    }
    // map: 1..maps = that level of the mode's list, kept in its data order;
    // anything else = the game's own shuffled cycle from its first level
    uint32_t pick = 0;
    if (!map.empty() && map != "cycle") {
        char* end = nullptr;
        const long v = strtol(map.c_str(), &end, 10);
        if (end != map.c_str() && *end == 0 && v >= 1 && v <= (long)row->maps) pick = (uint32_t)v;
        else printf("[mp] PS3_MP_MAP \"%s\" is not 1..%u or cycle: cycle is used\n", map.c_str(), row->maps);
    }
    mp_put32(0x819960, row->mode);
    mp_put32(0x819964, pick ? pick - 1 : 0);
    mp_put32(0x819968, pick ? 0x100 : 1);
    if (pick) printf("[mp] Quick Play: %s (mode %u), first map %u of %u, list not shuffled\n", row->name, row->mode, pick, row->maps);
    else printf("[mp] Quick Play: %s (mode %u), maps in the game's shuffled order\n", row->name, row->mode);
    fflush(stdout);
}
} // namespace

// boot_main.cpp, right after ppu_load_elf: the switches into guest memory.
extern "C" void ascension_after_load(void)
{
    if (!vm_base) return;
    const bool* on = patch_state();
    std::string list_on, list_off;
    // The patches that are off first, then the ones that are on (each in table
    // order): where two patches write the same data bytes (the Offline
    // Multiplayer mode word), a patch that is on always wins over one that is
    // off, and of two that are on the later one. Code words are never shared
    // (rpcs3_patch.py refuses that), so for them the order does not matter.
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < kPatches; i++) {
            if (on[i] != (pass == 1)) continue;
            const ascension_patch& p = k_patches[i];
            for (uint32_t w = p.first; w < p.first + p.count; w++) {
                const ascension_patch_write& pw = k_patch_writes[w];
                memcpy(vm_base + pw.addr, k_patch_bytes + (on[i] ? pw.patched : pw.orig), pw.len);
            }
        }
    for (int i = 0; i < kPatches; i++) {
        const ascension_patch& p = k_patches[i];
        std::string& l = on[i] ? list_on : list_off;
        l += l.empty() ? "" : ", ";
        l += p.name;
    }
    std::string env;
    printf("[patches] on: %s; off: %s%s\n", list_on.empty() ? "none" : list_on.c_str(),
           list_off.empty() ? "none" : list_off.c_str(), patches_env(env) ? "" : " (build defaults)");
    mp_choice_apply();
}
