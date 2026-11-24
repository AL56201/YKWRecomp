"""One-off: adapt LAS Recomp's runtime/hle/movie.c (nn::movie on FFmpeg) to YKW Recomp (kept for the record)."""
import sys

p = sys.argv[1]
s = open(p, encoding='utf-8').read()


def rep(a, b):
    global s
    if a not in s:
        sys.exit('missing: ' + a[:70])
    s = s.replace(a, b)


rep('''    { extern void pace_hitch(const char *why);  // nvn/pace.c
      pace_hitch("a movie"); }
''', '')
rep('''    e->skippable = rel && strstr(rel, "movie/opening.webm");''',
    '''    e->skippable = false;  // (LAS's opening skip; Yo-kai Watch's movies keep the game's own skip handling)''')
rep('''    if (rel && strstr(rel, "movie/ending.webm")) { extern void run_timer_ending_movie(void); run_timer_ending_movie(); }  // (the run's end)
    if (rel && strstr(rel, "movie/title.webm")) { extern void run_timer_title(void); run_timer_title(); }  // (a new attempt next)
''', '')
# the game draws movies with GL from the decoder's NV12 buffer: no NVN texture output mode
a = s.index('    if (d->video && b->base && b->capacity == 8) {')
b = s.index('    memset(b, 0, sizeof *b);', a)
s = s[:a] + s[b:]
a = s.index('typedef struct NvnTexture NvnTexture;')
b = s.index('\n', s.index('bool renderer_upload_nv12', a)) + 1
s = s[:a] + s[b:]
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
