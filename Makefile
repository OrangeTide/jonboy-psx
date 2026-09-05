BINDIR=_out
CFLAGS=-Wall -W -g -Og
all ::
clean ::
clean-all : clean
	$(RM) *.d
run : $(BINDIR)/jonboy
	./$<
$(BINDIR) :
	mkdir -p $@
.PHONY: all clean clean-all run
## jonboy
all :: $(BINDIR)/jonboy
clean ::
	$(RM) $(BINDIR)/jonboy

$(BINDIR)/jonboy : cart.c cpu.c dma.c main.c mem.c | $(BINDIR)
	$(CC) -o $@ -MMD $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $^ $(LDLIBS)
$(BINDIR)/jonboy : CFLAGS:=$(CFLAGS) $(shell pkg-config --cflags sdl3)
$(BINDIR)/jonboy : LDLIBS:=$(LDLIBS) $(shell pkg-config --libs sdl3)
## testcart
all :: $(BINDIR)/test1.cart
clean ::
	$(RM) $(BINDIR)/test1.cart $(BINDIR)/test1.elf $(BINDIR)/test1.o
$(BINDIR)/test1.cart : $(BINDIR)/test1.elf
	mipsel-unknown-elf-objcopy -O binary $< $@
$(BINDIR)/test1.o : test1.asm
	mipsel-unknown-elf-as -march=r3000 -mabi=32 -o $@ $<
$(BINDIR)/test1.elf : $(BINDIR)/test1.o
	mipsel-unknown-elf-ld -Ttext 0x0 -o $@ $<
## end
-include $(wildcard *.d)
