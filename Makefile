BINDIR=_out
# Optimization level is chosen per target architecture. On ARM (Raspberry Pi and
# other Cortex targets) build optimized and tuned for the in-order Cortex-A53
# pipeline. Everywhere else (x86-64 dev machines) build -O2: the interpreter and
# per-instruction hardware ticks are the bottleneck, and -Og runs the core about
# 1.4x slower, which is enough to starve real-time audio. Pass OPT= to override,
# for example OPT=-Og for a debug build. The target is read from the compiler's
# own triple ($(CC) -dumpmachine), so this stays correct under a cross-compiler.
TARGET:=$(shell $(CC) -dumpmachine)
ifneq (,$(filter aarch64% arm%,$(TARGET)))
OPT?=-O2 -mcpu=cortex-a53 -fomit-frame-pointer
else
OPT?=-O2
endif
CFLAGS=-Wall -W -g $(OPT)
# Cross-toolchain for the MIPS test carts and the loader PS-EXE. Different distros
# and crosstool-NG builds name the mipsel bare-metal toolchain differently, so
# probe PATH for the first candidate gcc that exists rather than hardcoding one,
# then derive the as/ld/objcopy prefix from its own triple ($(CC) -dumpmachine).
# Override the probe by passing MIPSEL_CC=<name>-gcc on the make command line.
MIPSEL_CANDIDATES=mipsel-none-elf-gcc mipsel-unknown-elf-gcc mipsel-linux-gnu-gcc mipsel-elf-gcc
MIPSEL_CC?=
ifeq ($(MIPSEL_CC),)
MIPSEL_CC:=$(firstword $(foreach c,$(MIPSEL_CANDIDATES),$(if $(shell command -v $(c) 2>/dev/null),$(c))))
endif
MIPSEL_ARCH:=$(shell $(MIPSEL_CC) -dumpmachine 2>/dev/null)
all ::
clean ::
clean-all : clean
	$(RM) *.d
run : $(BINDIR)/jonboy
	./$<
fmt : # WARNING: this modifies every file without confirmation
	astyle --style=linux -xB --indent=spaces=4 -f -p *.[ch]
# Run the PeterLemon per-opcode CPU tests as a conformance suite. The reference
# ROMs live under REF/ (not part of this repository) and each prints a PASS or
# FAIL verdict read back from the framebuffer.
CPUTESTS=$(sort $(wildcard REF/PSX/CPUTest/CPU/*/*.exe REF/PSX/CPUTest/CPU/*/*/*.exe))
conformance : $(BINDIR)/jonboy
	@pass=0; fail=0; \
	for exe in $(CPUTESTS); do \
	    v=$$(./$(BINDIR)/jonboy --exe "$$exe" 2>/dev/null | grep VERDICT | awk '{print $$2}'); \
	    printf "%-14s %s\n" "$$(basename $$exe .exe)" "$$v"; \
	    if [ "$$v" = PASS ]; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo "-----------------"; echo "PASS=$$pass FAIL=$$fail"
# Save-state determinism regression. It boots the BIOS headless, saves state,
# then proves a reload reproduces the next run bit-for-bit (see run_savestate_selftest
# in main.c). Needs a retail BIOS, which is external, so pass it in; a disc and a
# deeper step budget exercise more subsystems (FMV: SPU, XA, MDEC, CD):
#   make savestate-test BIOS=path/to/bios.bin [CD=game.cue] [STEPS=300000000]
STEPS ?= 20000000
savestate-test : $(BINDIR)/jonboy
	@test -n "$(BIOS)" || { echo "set BIOS=path/to/bios.bin"; exit 1; }
	./$(BINDIR)/jonboy --savestate-selftest --steps $(STEPS) --bios "$(BIOS)" $(if $(CD),--cd "$(CD)")
# Run the PeterLemon GTE (coprocessor 2) tests, the same verdict scheme as the
# CPU conformance suite. The ROMs live under REF/ and are not part of the repo.
GTETESTS=$(sort $(wildcard REF/PSX/CPUTest/GTE/*/*.exe REF/PSX/CPUTest/GTE/*/*/*.exe))
gte-conformance : $(BINDIR)/jonboy
	@pass=0; fail=0; \
	for exe in $(GTETESTS); do \
	    v=$$(./$(BINDIR)/jonboy --exe "$$exe" 2>/dev/null | grep VERDICT | awk '{print $$2}'); \
	    printf "%-14s %s\n" "$$(basename $$exe .exe)" "$$v"; \
	    if [ "$$v" = PASS ]; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo "-----------------"; echo "PASS=$$pass FAIL=$$fail"
$(BINDIR) :
	mkdir -p $@
.PHONY: all clean clean-all run fmt conformance gte-conformance savestate-test amalg cdda-test spu-test xa-test mc-test loader-test pad-test script-test mdec-test gte-test gpu-thread-test gpu-readback-test mdec-oracle macos-app install install-man uninstall
## jonboy
EMU_SRC=cdrom.c cpu.c dma.c gdb.c gpu.c gte.c inject.c main.c mc.c mdec.c mem.c pad.c psio.c rom.c sched.c savestate.c script.c spu.c timer.c vram.c xa.c
all :: $(BINDIR)/jonboy
clean ::
	$(RM) $(BINDIR)/jonboy

$(BINDIR)/jonboy : $(EMU_SRC) | $(BINDIR)
	$(CC) -o $@ -MMD $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $^ $(LDLIBS)
$(BINDIR)/jonboy : CFLAGS:=$(CFLAGS) $(shell pkg-config --cflags sdl3)
$(BINDIR)/jonboy : LDLIBS:=$(LDLIBS) -lpthread $(shell pkg-config --libs sdl3)
## amalgamation (single self-contained C file)
# tools/amalgamate.awk inlines every local include once, so $(BINDIR)/jonboy.c
# is a single file that builds with only the system and SDL3 headers. The
# jonboy-amalg target compiles it, proving the amalgamation stays self-contained.
amalg : $(BINDIR)/jonboy-amalg
all :: $(BINDIR)/jonboy.c
clean ::
	$(RM) $(BINDIR)/jonboy.c $(BINDIR)/jonboy-amalg
$(BINDIR)/jonboy.c : $(EMU_SRC) $(wildcard *.h) tools/amalgamate.awk | $(BINDIR)
	awk -f tools/amalgamate.awk $(EMU_SRC) > $@
$(BINDIR)/jonboy-amalg : $(BINDIR)/jonboy.c | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) $(shell pkg-config --cflags sdl3) $< -lpthread $(shell pkg-config --libs sdl3)
## host unit tests (built without SDL: cdrom.c/spu.c with stub globals)
all :: $(BINDIR)/test_cdda $(BINDIR)/test_spu $(BINDIR)/test_xa $(BINDIR)/test_mc $(BINDIR)/test_loader $(BINDIR)/test_pad $(BINDIR)/test_script $(BINDIR)/test_mdec $(BINDIR)/test_gte $(BINDIR)/test_gpu_thread $(BINDIR)/test_gpu_readback
clean ::
	$(RM) $(BINDIR)/test_cdda $(BINDIR)/test_spu $(BINDIR)/test_xa $(BINDIR)/test_mc $(BINDIR)/test_loader $(BINDIR)/test_pad $(BINDIR)/test_script $(BINDIR)/test_mdec $(BINDIR)/test_gte $(BINDIR)/test_gpu_thread $(BINDIR)/test_gpu_readback
$(BINDIR)/test_cdda : test_cdda.c cdrom.c xa.c cdrom.h cpu.h mem.h spu.h xa.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_cdda.c cdrom.c xa.c -lpthread
$(BINDIR)/test_spu : test_spu.c spu.c spu.h cpu.h lebytes.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_spu.c spu.c
$(BINDIR)/test_xa : test_xa.c xa.c xa.h spu.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_xa.c xa.c
$(BINDIR)/test_mc : test_mc.c mc.c mc.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_mc.c mc.c
$(BINDIR)/test_loader : test_loader.c psio.c psio.h lebytes.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_loader.c psio.c
$(BINDIR)/test_pad : test_pad.c pad.c mc.c pad.h cpu.h mc.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_pad.c pad.c mc.c
$(BINDIR)/test_script : test_script.c script.c script.h pad.h cdrom.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_script.c script.c
$(BINDIR)/test_mdec : test_mdec.c mdec.c mdec.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_mdec.c mdec.c
$(BINDIR)/test_gte : test_gte.c gte.c gte.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_gte.c gte.c
$(BINDIR)/test_gpu_thread : test_gpu_thread.c gpu.c vram.c gpu.h vram.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_gpu_thread.c gpu.c vram.c -lpthread
$(BINDIR)/test_gpu_readback : test_gpu_readback.c gpu.c vram.c gpu.h vram.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_gpu_readback.c gpu.c vram.c -lpthread
cdda-test : $(BINDIR)/test_cdda
	./$(BINDIR)/test_cdda
spu-test : $(BINDIR)/test_spu
	./$(BINDIR)/test_spu
xa-test : $(BINDIR)/test_xa
	./$(BINDIR)/test_xa
mc-test : $(BINDIR)/test_mc
	./$(BINDIR)/test_mc
loader-test : $(BINDIR)/test_loader
	./$(BINDIR)/test_loader
pad-test : $(BINDIR)/test_pad
	./$(BINDIR)/test_pad
script-test : $(BINDIR)/test_script
	./$(BINDIR)/test_script
mdec-test : $(BINDIR)/test_mdec
	./$(BINDIR)/test_mdec
gte-test : $(BINDIR)/test_gte
	./$(BINDIR)/test_gte
gpu-thread-test : $(BINDIR)/test_gpu_thread
	./$(BINDIR)/test_gpu_thread
gpu-readback-test : $(BINDIR)/test_gpu_readback
	./$(BINDIR)/test_gpu_readback

# Byte-exact cross-check of the MDEC decoder against the real mednafen/Beetle
# mdec.c. That source is not part of this repository; fetch it under
# REF/beetle-mdec (git-ignored, like the conformance ROMs) to enable the check.
# The Beetle object is built with warnings off since it is third-party code.
BEETLE_MDEC = REF/beetle-mdec/psx/mdec.c
mdec-oracle : test_mdec_oracle.c mdec.c mdec.h | $(BINDIR)
	@if [ ! -f $(BEETLE_MDEC) ]; then \
	    echo "mdec-oracle: $(BEETLE_MDEC) not present; skipping."; \
	    echo "  Fetch the beetle-psx mednafen/psx MDEC source under REF/beetle-mdec to enable."; \
	else \
	    $(CC) -c -O2 -w -I REF/beetle-mdec/libretro-common/include \
	        -o $(BINDIR)/beetle_mdec.o $(BEETLE_MDEC) && \
	    $(CC) -o $(BINDIR)/test_mdec_oracle $(CFLAGS) \
	        test_mdec_oracle.c mdec.c $(BINDIR)/beetle_mdec.o && \
	    ./$(BINDIR)/test_mdec_oracle; \
	fi
## testcarts
TESTS=test1 test2 test3 test4 test5 test6 test7 test8 test9
TEST_CARTS=$(TESTS:%=$(BINDIR)/%.cart)
all :: $(TEST_CARTS)
clean ::
	$(RM) $(TESTS:%=$(BINDIR)/%.cart) $(TESTS:%=$(BINDIR)/%.elf) $(TESTS:%=$(BINDIR)/%.o)
$(BINDIR)/%.cart : $(BINDIR)/%.elf
	$(MIPSEL_ARCH)-objcopy -O binary -j .text -j .data $< $@
$(BINDIR)/%.o : %.asm | $(BINDIR)
	$(MIPSEL_ARCH)-as -march=r3000 -mabi=32 -o $@ $<
$(BINDIR)/%.elf : $(BINDIR)/%.o
	$(MIPSEL_ARCH)-ld -Ttext 0x0 -e __start -o $@ $<
## loader (guest menu program, a PS-EXE sideloaded at kernel-up)
LOADER_CC=$(MIPSEL_ARCH)-gcc
LOADER_CFLAGS=-march=r3000 -mabi=32 -G0 -fno-pic -mno-abicalls -O2 -ffreestanding -Wall
LOADER_SRC=loader/crt0.s loader/main.c
all :: $(BINDIR)/loader.exe
clean ::
	$(RM) $(BINDIR)/loader.exe $(BINDIR)/loader.elf $(BINDIR)/loader.bin $(BINDIR)/mkexe
$(BINDIR)/mkexe : loader/mkexe.c | $(BINDIR)
	$(CC) -O2 -o $@ $<
$(BINDIR)/loader.elf : $(LOADER_SRC) loader/loader.ld | $(BINDIR)
	$(LOADER_CC) $(LOADER_CFLAGS) -nostdlib -T loader/loader.ld -o $@ $(LOADER_SRC)
$(BINDIR)/loader.exe : $(BINDIR)/loader.elf $(BINDIR)/mkexe
	$(MIPSEL_ARCH)-objcopy -O binary $< $(BINDIR)/loader.bin
	$(BINDIR)/mkexe $(BINDIR)/loader.bin $@
## version (git describe; falls back to a bare sha, then "dev")
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
## install (PREFIX defaults to the XDG user prefix; pass PREFIX=/usr/local for a
## system install, and DESTDIR= to stage into a packaging root)
PREFIX ?= $(HOME)/.local
BINDIR_INSTALL ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1
install : $(BINDIR)/jonboy install-man
	install -d "$(DESTDIR)$(BINDIR_INSTALL)"
	install -m 755 $(BINDIR)/jonboy "$(DESTDIR)$(BINDIR_INSTALL)/jonboy"
install-man : jonboy.1
	install -d "$(DESTDIR)$(MANDIR)"
	install -m 644 jonboy.1 "$(DESTDIR)$(MANDIR)/jonboy.1"
uninstall :
	$(RM) "$(DESTDIR)$(BINDIR_INSTALL)/jonboy" "$(DESTDIR)$(MANDIR)/jonboy.1"
## macOS app bundle
# Package _out/JonBoy.app with the SDL3 dylib copied in and the binary repointed
# at it, so the bundle runs on a Mac without Homebrew. Native macOS only (uses
# otool/install_name_tool). The build is unsigned; clear the download quarantine
# with: xattr -dr com.apple.quarantine _out/JonBoy.app
macos-app : $(BINDIR)/jonboy packaging/Info.plist.in
	@command -v install_name_tool >/dev/null 2>&1 || { echo "macos-app: macOS only (needs install_name_tool)"; exit 1; }
	@app="$(BINDIR)/JonBoy.app"; fw="$$app/Contents/Frameworks"; \
	rm -rf "$$app"; \
	mkdir -p "$$app/Contents/MacOS" "$$fw" "$$app/Contents/Resources"; \
	cp $(BINDIR)/jonboy "$$app/Contents/MacOS/jonboy"; \
	rec=$$(otool -L $(BINDIR)/jonboy | awk '/libSDL3/ {print $$1; exit}'); \
	test -n "$$rec" || { echo "macos-app: no SDL3 dependency in binary"; exit 1; }; \
	base=$$(basename "$$rec"); src="$$rec"; \
	case "$$rec" in @*) src="$$(pkg-config --variable=libdir sdl3)/$$base";; esac; \
	test -f "$$src" || src="$$(pkg-config --variable=libdir sdl3)/$$base"; \
	cp "$$src" "$$fw/$$base"; chmod u+w "$$fw/$$base"; \
	install_name_tool -id "@executable_path/../Frameworks/$$base" "$$fw/$$base"; \
	install_name_tool -change "$$rec" "@executable_path/../Frameworks/$$base" "$$app/Contents/MacOS/jonboy"; \
	sed 's/@VERSION@/$(VERSION)/g' packaging/Info.plist.in > "$$app/Contents/Info.plist"; \
	echo "built $$app ($(VERSION), bundled $$base)"
clean ::
	$(RM) -r $(BINDIR)/JonBoy.app
## end
-include $(wildcard *.d)
