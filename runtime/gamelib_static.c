// The recompiled game code is linked into the executable (make gen + make), so its tables are plain symbols.
#include "gamelib.h"

extern const uint64_t main_fn_addrs[], sdk_fn_addrs[];
extern const GuestFn main_fn_ptrs[], sdk_fn_ptrs[];
extern const uint64_t main_fn_count, sdk_fn_count;

static const GameLibTables g_tables = {
    GAMELIB_ABI, "static", main_fn_addrs, sdk_fn_addrs, main_fn_ptrs, sdk_fn_ptrs, &main_fn_count, &sdk_fn_count,
};

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    (void)game_dir, (void)why, (void)why_size;
    return true;
}
const GameLibTables *gamelib_tables(void) { return &g_tables; }
bool gamelib_present(const char *game_dir) { (void)game_dir; return true; }
bool gamelib_built_for(char *out, size_t size) { (void)out, (void)size; return false; }
