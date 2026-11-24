// ykwrecomp.ini: key = value lines, unknown keys ignored, written back with every known key.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

Settings g_settings = {.volume = 1.0f, .audio_out = AUDIO_OUT_STEREO, .reverb = true, .movie_audio = true,
                       .window_scale = 1, .fullscreen = false, .vsync = true, .unlock_fps = false, .language = "ja"};

static const char *path(void) {
    const char *e = getenv("YKW_SETTINGS");
    return e && *e ? e : "ykwrecomp.ini";
}

void settings_load(void) {
    FILE *f = fopen(path(), "r");
    if (!f) { settings_save(); return; }
    char line[256], s[64];
    float v;
    int i;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " volume = %f", &v) == 1) g_settings.volume = v < 0 ? 0 : v > 1 ? 1 : v;
        else if (sscanf(line, " audio_out = %d", &i) == 1) g_settings.audio_out = i == AUDIO_OUT_51;
        else if (sscanf(line, " reverb = %d", &i) == 1) g_settings.reverb = i != 0;
        else if (sscanf(line, " movie_audio = %d", &i) == 1) g_settings.movie_audio = i != 0;
        else if (sscanf(line, " window_scale = %d", &i) == 1) g_settings.window_scale = i < 1 ? 1 : i > 4 ? 4 : i;
        else if (sscanf(line, " fullscreen = %d", &i) == 1) g_settings.fullscreen = i != 0;
        else if (sscanf(line, " vsync = %d", &i) == 1) g_settings.vsync = i != 0;
        else if (sscanf(line, " unlock_fps = %d", &i) == 1) g_settings.unlock_fps = i != 0;
        else if (sscanf(line, " language = %7s", s) == 1) snprintf(g_settings.language, sizeof g_settings.language, "%s", s);
    }
    fclose(f);
}

void settings_save(void) {
    FILE *f = fopen(path(), "w");
    if (!f) return;
    fprintf(f, "volume = %.2f\naudio_out = %d\nreverb = %d\nmovie_audio = %d\nwindow_scale = %d\nfullscreen = %d\n"
               "vsync = %d\nunlock_fps = %d\nlanguage = %s\n", g_settings.volume, g_settings.audio_out,
            g_settings.reverb, g_settings.movie_audio, g_settings.window_scale, g_settings.fullscreen, g_settings.vsync,
            g_settings.unlock_fps, g_settings.language);
    fclose(f);
}

const char *settings_language(void) { return g_settings.language; }
