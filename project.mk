# Project
_ := jonboy
ALL_PROJECTS += $_
EXEC.$_ := $(OUTDIR)/$_
SRCS.$_ := $(wildcard *.c)
OBJS.$_ := $(addprefix $(OUTDIR)/,$(SRCS.$_:.c=.o))
LDLIBS.$_ :=-ldl -lm $(shell pkg-config --libs gl glfw3)
$(EXEC.$_) : $(OBJS.$_) ; $(link.c)
