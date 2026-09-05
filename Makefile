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
$(BINDIR) :
	mkdir -p $@
.PHONY: all clean clean-all run fmt
## jonboy
all :: $(BINDIR)/jonboy
clean ::
	$(RM) $(BINDIR)/jonboy

$(BINDIR)/jonboy : cpu.c dma.c gpu.c main.c mem.c pad.c rom.c timer.c vram.c | $(BINDIR)
	$(CC) -o $@ -MMD $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $^ $(LDLIBS)
$(BINDIR)/jonboy : CFLAGS:=$(CFLAGS) $(shell pkg-config --cflags sdl3)
$(BINDIR)/jonboy : LDLIBS:=$(LDLIBS) $(shell pkg-config --libs sdl3)
## testcarts
TESTS=test1 test2
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
