// Player settings (ykwrecomp.ini in the data folder; runtime/settings.c).
#pragma once
#include <stdbool.h>

enum { AUDIO_OUT_STEREO, AUDIO_OUT_51, N_AUDIO_OUTS };

typedef struct {
    float volume;        // master volume, 0..1
    int audio_out;       // AUDIO_OUT_*
    bool reverb;         // the audio renderer's reverb effect
    bool movie_audio;    // sound in movies
    int window_scale;    // window size: 1280x720 times this
    bool fullscreen;
    bool vsync;
    bool interp;         // frame interpolation: in-between images on the display's refreshes (gl/interp.c)
    double fps;          // frame rate (hle/framerate.c): 30 = the game's own, 0 = the display's refresh, or a number
    char language[8];    // the system language the game is told ("ja": the game's only text; a RomFS mod may translate)
} Settings;

extern Settings g_settings;
void settings_load(void);
void settings_save(void);
const char *settings_language(void);
