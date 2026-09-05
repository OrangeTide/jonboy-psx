BINDIR=_out
CFLAGS=-Wall -W -g -Og
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
.PHONY: all clean clean-all run fmt conformance
## jonboy
all :: $(BINDIR)/jonboy
clean ::
	$(RM) $(BINDIR)/jonboy

$(BINDIR)/jonboy : cpu.c dma.c gpu.c inject.c main.c mem.c pad.c psio.c rom.c sched.c spu.c timer.c vram.c | $(BINDIR)
	$(CC) -o $@ -MMD $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $^ $(LDLIBS)
$(BINDIR)/jonboy : CFLAGS:=$(CFLAGS) $(shell pkg-config --cflags sdl3)
$(BINDIR)/jonboy : LDLIBS:=$(LDLIBS) $(shell pkg-config --libs sdl3)
## testcarts
TESTS=test1 test2 test3 test4 test5 test6 test7 test8
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
## end
-include $(wildcard *.d)
