// RomFS mods (LayeredFS): files in a mod's romfs/ folder replace or add to the game's RomFS (services/fs.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define YKW_TITLE_ID "0100C0000CEEA000"

void mods_scan(void);                     // (idempotent) find the enabled mods; fs.c calls it before indexing RomFS
int mods_count(void);                     // enabled mods with a romfs folder, lowest priority first
const char *mods_romfs_root(int i);       // host path of mod i's romfs folder
const char *mods_name(int i);
// Host file that replaces RomFS file `path` ("dir/file", no leading slash), from the highest-priority mod that has it.
bool mods_romfs_override(const char *path, char *host, size_t cap);
