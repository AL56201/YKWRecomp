# YKW Recomp - Windows build (MSYS2 UCRT64 clang).
#   make gen    : recompile exefs/main and exefs/sdk from your own extracted game files into generated/
#   make -j     : build build/ykwrecomp.exe
# Game files: extracted/ next to this folder by default (GAME=...); make extract fills it from your NSPs.
CC      := clang
PY      ?= python
GAME    ?= ../extracted
GEN     := generated
B       := build

GENFLAGS := -O1 -g0 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden \
            -mllvm -disable-licm-promotion -march=haswell -Iruntime
RTFLAGS  := -O2 -g -march=haswell -Iruntime -Iruntime/gl -Wall -Wno-unused-function -Wno-unused-variable \
            -Wno-missing-braces -Wno-unused-but-set-variable -Wno-format -D_GNU_SOURCE -DYKW_RECOMP \
            -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 -lSDL3 -lavformat -lavcodec -lavutil -lswscale -lopengl32 -lws2_32 -lwinmm -lole32 -lshell32 -luuid -lbcrypt -ldbghelp

GEN_SRC := $(wildcard $(GEN)/sdk/*.c) $(wildcard $(GEN)/main/*.c)
GEN_OBJ := $(patsubst $(GEN)/%.c,$(B)/gen/%.o,$(GEN_SRC))
RT_SRC  := $(wildcard runtime/*.c) $(wildcard runtime/services/*.c) $(wildcard runtime/hle/*.c) \
           $(wildcard runtime/gl/*.c) runtime/platform/windows.c
RT_OBJ  := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC))

all: $(B)/ykwrecomp.exe

$(B)/ykwrecomp.exe: $(RT_OBJ) $(GEN_OBJ)
	$(CC) -o $@ $(RT_OBJ) $(B)/gen/*/*.o $(LDFLAGS)

$(B)/gen/%.o: $(GEN)/%.c runtime/cpu.h runtime/simd.h runtime/simd_fp.h
	@mkdir -p $(dir $@)
	$(CC) -c $(GENFLAGS) $< -o $@

$(B)/rt/%.o: runtime/%.c $(wildcard runtime/*.h) $(wildcard runtime/gl/*.h)
	@mkdir -p $(dir $@)
	$(CC) -c $(RTFLAGS) $< -o $@

gen-objs: $(GEN_OBJ)
rt-objs: $(RT_OBJ)

runtime/gl/gl_thunks.c: tools/gen_gl_thunks.py
	$(PY) tools/gen_gl_thunks.py runtime/gl/gl_thunks.c

gen:
	cd tools && $(PY) gen.py sdk ../$(GAME)/exefs/sdk 0x7004000000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt --simd c
	cd tools && $(PY) gen.py main ../$(GAME)/exefs/main 0x7000000000 ../$(GEN)/main --simd c

extract:
	$(PY) -I tools/nxextract.py --keys "$(KEYS)" --base "$(BASE_NSP)" --update "$(UPDATE_NSP)" --out $(GAME)

clean:
	rm -rf $(B)

.PHONY: all gen gen-objs rt-objs extract clean
