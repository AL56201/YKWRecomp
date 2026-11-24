// YKW Recomp entry: reserve the guest window, load the modules, set up the kernel, run nn::init::Start on the main
// thread, and run the window loop (runtime/ui.c) on this one.
// Usage: ykwrecomp [game folder with exefs/ and romfs/]   (default: extracted)
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gamelib.h"
#include "hle.h"
#include "kernel.h"
#include "mods.h"
#include "settings.h"

const char *g_romfs_dir;

// rtld normally passes these to nn::init::Start: run a module's DT_INIT (registers it and its TLS with musl, then
// runs its init_array), or the array itself without one.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}
static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }

// The guest address range is claimed before anything else can map there; on a collision, start again.
static bool g_guest_reserved;
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) { g_guest_reserved = true; return; }
    const char *tries = getenv("YKW_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("YKW_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// in-place reboot isn't supported here: a request just quits
static _Atomic int g_reboot;
void rt_request_reboot(int kind) { g_reboot = kind; }
int rt_reboot_requested(void) { return g_reboot; }

static void boot(const char *root) {
    if (!g_guest_reserved) FATAL("guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
    static char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    g_romfs_dir = romfs;
    char why[512];
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    // the generated code was made from one main; refuse another (its addresses would be wrong)
    extern const char ykw_main_build_id[];
    char id[GAMELIB_FULL_ID_SIZE];
    if (gamelib_full_id_for("", root, id, sizeof id) && strncasecmp(id + 1, ykw_main_build_id, 16))
        FATAL("exefs/main in %s is build %s, but this executable was generated from %.16s: run make gen and make\n",
              root, id + 1, ykw_main_build_id);
    mods_scan();
    kernel_init();
    dispatch_init();

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t fini_fn = hle_register("<rtld_init_main>", rtld_init_main);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_fn, fini_fn);  // nn::init::Start(thread_handle, args_region, init, fini)
}

int main(int argc, char **argv) {
    plat_init();
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    const char *root = "extracted";
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-') root = argv[i];
    LOG("[info] YKW Recomp, game files: %s\n", root);
    settings_load();
    extern void ui_init(void);
    ui_init();  // the window and the GL root context, before any game thread asks for a context
    boot(root);
    ui_run();
    LOG("window closed\n");
    _exit(0);
}
