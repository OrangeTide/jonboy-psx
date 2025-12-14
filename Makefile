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
link.c = $(CC) -o $@ $(LDFLAGS) $(LDFLAGS.$(PROJECT)) $(if $(OUTDIR),-L$(OUTDIR)) $^ $(LDLIBS) $(LDLIBS.$(PROJECT)) -DPROJECT=$(PROJECT)
link.cxx = $(CXX) -o $@ $(LDFLAGS) $(LDFLAGS.$(PROJECT)) $(if $(OUTDIR),-L$(OUTDIR)) $^ $(LDLIBS) $(LDLIBS.$(PROJECT))
link.a = $(if $?,,$(error Empty))$(AR) $(ARFLAGS) $(ARFLAGS.$(PROJECT)) $@ $?
compile.c = $(CC) -c -o $@ $< -MMD $(CFLAGS) $(CFLAGS.$(PROJECT)) $(CPPFLAGS) $(CPPFLAGS.$(PROJECT))
# Top-level rules
.SECONDEXPANSION:
all :: $$(addprefix build_,$$(ALL_PROJECTS))
# pattern-based dependency with no steps in rule
build_% : $$(EXEC.$$*) ;
build_% : PROJECT:=webrpg
clean :: $$(addprefix clean_,$$(ALL_PROJECTS))
clean_% : ; $(RM) $(OBJS.$*)
clean-all :: clean $$(addprefix cleanall_,$$(ALL_PROJECTS))
cleanall_% :: ; $(RM) $(EXEC.$*)
.PHONY: all clean clean-all
% : %.c # delete
% : %.o # delete
%.o : %.c # delete
$(OUTDIR)/%.o : %.c ; $(compile.c)
include project.mk
-include $(foreach p,$(OBJS.$p:.o=.d),$(ALL_PROJECTS))
