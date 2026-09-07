BINDIR=_out
# Optimization level is chosen per target architecture. On ARM (Raspberry Pi and
# other Cortex targets) the interpreter and software rasterizer are the
# bottleneck, so build optimized and tuned for the in-order Cortex-A53 pipeline.
# Everywhere else (x86-64 dev machines) keep the debug-friendly -Og. The target
# is read from the compiler's own triple ($(CC) -dumpmachine), so this stays
# correct under a cross-compiler. Override by passing OPT= on the command line.
TARGET:=$(shell $(CC) -dumpmachine)
ifneq (,$(filter aarch64% arm%,$(TARGET)))
OPT?=-O2 -mcpu=cortex-a53 -fomit-frame-pointer
else
OPT?=-Og
endif
CFLAGS=-Wall -W -g $(OPT)
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
$(BINDIR) :
	mkdir -p $@
.PHONY: all clean clean-all run fmt conformance amalg cdda-test spu-test xa-test
## jonboy
EMU_SRC=cdrom.c cpu.c dma.c gpu.c inject.c main.c mem.c pad.c psio.c rom.c sched.c spu.c timer.c vram.c xa.c
all :: $(BINDIR)/jonboy
clean ::
	$(RM) $(BINDIR)/jonboy

$(BINDIR)/jonboy : $(EMU_SRC) | $(BINDIR)
	$(CC) -o $@ -MMD $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $^ $(LDLIBS)
$(BINDIR)/jonboy : CFLAGS:=$(CFLAGS) $(shell pkg-config --cflags sdl3)
$(BINDIR)/jonboy : LDLIBS:=$(LDLIBS) $(shell pkg-config --libs sdl3)
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
	$(CC) -o $@ $(CFLAGS) $(shell pkg-config --cflags sdl3) $< $(shell pkg-config --libs sdl3)
## host unit tests (built without SDL: cdrom.c/spu.c with stub globals)
all :: $(BINDIR)/test_cdda $(BINDIR)/test_spu $(BINDIR)/test_xa
clean ::
	$(RM) $(BINDIR)/test_cdda $(BINDIR)/test_spu $(BINDIR)/test_xa
$(BINDIR)/test_cdda : test_cdda.c cdrom.c xa.c cdrom.h cpu.h mem.h spu.h xa.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_cdda.c cdrom.c xa.c
$(BINDIR)/test_spu : test_spu.c spu.c spu.h lebytes.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_spu.c spu.c
$(BINDIR)/test_xa : test_xa.c xa.c xa.h spu.h | $(BINDIR)
	$(CC) -o $@ $(CFLAGS) test_xa.c xa.c
cdda-test : $(BINDIR)/test_cdda
	./$(BINDIR)/test_cdda
spu-test : $(BINDIR)/test_spu
	./$(BINDIR)/test_spu
xa-test : $(BINDIR)/test_xa
	./$(BINDIR)/test_xa
## testcarts
TESTS=test1 test2 test3 test4 test5 test6 test7 test8 test9
TEST_CARTS=$(TESTS:%=$(BINDIR)/%.cart)
all :: $(TEST_CARTS)
clean ::
	$(RM) $(TESTS:%=$(BINDIR)/%.cart) $(TESTS:%=$(BINDIR)/%.elf) $(TESTS:%=$(BINDIR)/%.o)
$(BINDIR)/%.cart : $(BINDIR)/%.elf
	mipsel-unknown-elf-objcopy -O binary -j .text -j .data $< $@
$(BINDIR)/%.o : %.asm | $(BINDIR)
	mipsel-unknown-elf-as -march=r3000 -mabi=32 -o $@ $<
$(BINDIR)/%.elf : $(BINDIR)/%.o
	mipsel-unknown-elf-ld -Ttext 0x0 -e __start -o $@ $<
## loader (guest menu program, a PS-EXE sideloaded at kernel-up)
LOADER_CC=mipsel-unknown-elf-gcc
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
	mipsel-unknown-elf-objcopy -O binary $< $(BINDIR)/loader.bin
	$(BINDIR)/mkexe $(BINDIR)/loader.bin $@
## end
-include $(wildcard *.d)
