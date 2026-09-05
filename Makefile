BINDIR=_out
CFLAGS=-Wall -W -g -Og
all : $(BINDIR)/jonboy
clean :
	$(RM) $(BINDIR)/jonboy
clean-all : clean
	$(RM) *.d
$(BINDIR)/jonboy : main.c | $(BINDIR)
	$(CC) -o $@ -MMD $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $< $(LDLIBS)
$(BINDIR)/jonboy : CFLAGS:=$(CFLAGS) $(shell pkg-config --cflags sdl3)
$(BINDIR)/jonboy : LDLIBS:=$(LDLIBS) $(shell pkg-config --libs sdl3)
-include $(wildcard *.d)
$(BINDIR) :
	mkdir -p $@
