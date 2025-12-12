# Makefile for jonboy
## Detection Routines
ifeq ($(OS),)
	OS:=$(shell uname -o | tr 'A-Z/' 'a-z_')
endif
ifeq ($(ARCH),)
	ARCH:=$(shell uname -m)
endif
# Build Settings
TOP := $(wildcard $(dir $(lastword $(MAKEFILE_LIST))))
ALL_PROJECTS :=
# TODO: realpath or abspath?
OUTDIR := $(TOP)out/$(ARCH)-$(OS)
$(shell mkdir -p $(OUTDIR))
# Command Macros
link.c = $(CC) -o $@ $(LDFLAGS) $(LDFLAGS.$(PROJECT)) $(if $(OUTDIR),-L$(OUTDIR)) $^ $(LDLIBS) $(LDLIBS.$(PROJECT))
link.cxx = $(CXX) -o $@ $(LDFLAGS) $(LDFLAGS.$(PROJECT)) $(if $(OUTDIR),-L$(OUTDIR)) $^ $(LDLIBS) $(LDLIBS.$(PROJECT))
link.a = $(if $?,,$(error Empty))$(AR) $(ARFLAGS) $(ARFLAGS.$(PROJECT)) $@ $?
compile.c = $(CC) -c -o $@ $< -MMD $(CFLAGS) $(CFLAGS.$(PROJECT)) $(CPPFLAGS) $(CPPFLAGS.$(PROJECT))
# Top-level rules
.SECONDEXPANSION:
all :: $$(addprefix build_,$$(ALL_PROJECTS))
# pattern-based dependency with no steps in rule
build_% : $$(EXEC.$$*) ;
build_% : PROJECT:=$*
clean :: $$(addprefix clean_,$$(ALL_PROJECTS))
clean_% : ; $(RM) $(OBJS.$*)
clean-all :: clean $$(addprefix cleanall_,$$(ALL_PROJECTS))
cleanall_% :: ; $(RM) $(EXEC.$*)
.PHONY: all clean clean-all build_% clean_% cleanall_%
% : %.c # delete
% : %.o # delete
%.o : %.c # delete
$(OUTDIR)/%.o : %.c ; $(compile.c)
# Project
_ := jonboy
ALL_PROJECTS += $_
EXEC.$_ := $(OUTDIR)/$_
SRCS.$_ := $(wildcard *.c)
OBJS.$_ := $(addprefix $(OUTDIR)/,$(SRCS.$_:.c=.o))
LDLIBS.jonboy:=-ldl -lm $(shell pkg-config --libs gl glfw3)
$(EXEC.$_) : $(OBJS.$_) ; $(link.c)
-include $(OBJS.$_:.o=.d)
