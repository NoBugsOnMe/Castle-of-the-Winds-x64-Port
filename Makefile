# Cross-build of the Castle of the Winds Windows port (clang + lld, Wine headers).
#
#   make            -> build/x64/CASTLE1.EXE and build/x64/CASTLE2.EXE
#   make ARCH=x86   -> 32-bit build in build/x86/ (builds, but has not been tested)

# local overrides (not part of the distribution)
-include config.mk

WINE_SRC   ?= ../wine
WINE_BUILD ?= ../wine-build
GAME_DIR   ?= ../CotW
ARCH       ?= x64

ifeq ($(ARCH),x86)
TRIPLE  = i686-pc-windows-msvc
DLLARCH = i386
MACH    = i386
else
TRIPLE  = x86_64-pc-windows-msvc
DLLARCH = x86_64
MACH    = i386:x86-64
endif

OUT  = build/$(ARCH)
OBJ  = $(OUT)/obj
LIBD = $(OUT)/lib

CC = clang
CFLAGS = -target $(TRIPLE) --no-default-config -O2 -g0 -fno-strict-aliasing -fwrapv \
         -Wall -Wno-unused-function -Wno-missing-braces -Wno-pointer-sign -Wno-format-truncation \
         -I$(WINE_SRC)/include/msvcrt -I$(WINE_SRC)/include -I$(WINE_BUILD)/include -Isrc \
         -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -D_MSVCR_VER=0 -mno-stack-arg-probe $(EXTRA_CFLAGS)
LDFLAGS = -target $(TRIPLE) --no-default-config -fuse-ld=lld -nostdlib \
          -Wl,-subsystem:windows -Wl,-entry:WinMainCRTStartup -Wl,-dynamicbase -Wl,-nxcompat \
          -Wl,-largeaddressaware -Wl,-stack:0x400000

DLLS = kernel32 user32 gdi32 comdlg32 shell32 msvcrt advapi32
ifeq ($(ARCH),x86)
DLLS += ntdll   # 64-bit arithmetic helpers (_alldiv, _aulldiv)
endif
LIBS = $(addprefix $(LIBD)/,$(addsuffix .lib,$(DLLS)))

SRCS = src/cpu.c src/mem.c src/ne.c src/native.c src/dos.c src/kernel.c src/user.c \
       src/msg.c src/res.c src/gdi.c src/commdlg.c src/help.c src/patch.c src/zoom.c src/scale.c src/dark.c src/settings.c src/hud.c src/msgbox.c src/main.c src/crt.c gen/apitab.c
OBJS = $(patsubst %.c,$(OBJ)/%.o,$(notdir $(SRCS)))

all: $(OUT)/CASTLE1.EXE $(OUT)/CASTLE2.EXE

$(OBJ)/%.o: src/%.c src/*.h
	@mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/apitab.o: gen/apitab.c src/*.h
	@mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBD)/%.lib: tools/gen_implibs.py
	@mkdir -p $(LIBD)
	python3 -I tools/gen_implibs.py $(WINE_SRC) $(LIBD) $(DLLARCH) $* > /dev/null
	llvm-dlltool -m $(MACH) $(if $(filter x86,$(ARCH)),-k,) -d $(LIBD)/$*.def -l $@

$(OUT)/help%.bin: $(GAME_DIR)/CASTLE%.HLP tools/hlp.py tools/hlp2bin.py
	@mkdir -p $(OUT)
	python3 -I tools/hlp2bin.py $< $@

$(OUT)/castle%.res: res/castle%.rc res/*.manifest $(GAME_DIR)/CASTLE%.EXE $(OUT)/help%.bin tools/extract_icon.py
	@mkdir -p $(OUT)
	cp $(GAME_DIR)/CASTLE$*.EXE $(OUT)/neimage$*.bin
	python3 -I tools/extract_icon.py $(GAME_DIR)/CASTLE$*.EXE 1 $(OUT)/castle$*.ico
	llvm-rc -I res -I $(OUT) -fo $@ res/castle$*.rc

$(OUT)/CASTLE%.EXE: $(OBJS) $(LIBS) $(OUT)/castle%.res
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(OUT)/castle$*.res $(LIBS)

clean:
	rm -rf build

.PRECIOUS: $(OUT)/castle%.res $(LIBD)/%.lib $(OUT)/help%.bin
.SECONDARY: $(OBJS)
.PHONY: all clean
