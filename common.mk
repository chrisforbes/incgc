#
# Common makefile plumbing.
# Supported variables:
# 	- TARGET: the name of the binary to produce
# 	- CSRC: the list of C sources to use
# 	- LIBS: the list of pkg-config libs to use
# 	- CFLAGS: additional flags to pass to the C compiler
# 	- LDFLAGS: additional flags to pass to the linker
#

CC := gcc
COBJ := $(CSRC:.c=.o)
XCFLAGS := $(shell pkg-config --cflags $(LIBS))
XLDFLAGS := $(shell pkg-config --libs $(LIBS))

$(TARGET): $(COBJ) Makefile
	@echo LINK $(COBJ) -> $@
	@$(CC) -o $@ $(COBJ) $(XLDFLAGS) $(LDFLAGS)

clean:
	-rm $(TARGET) $(COBJ) $(CSRC:.c=.d)

%.o: %.c Makefile
	@echo CC $< '->' $@
	@$(CC) -o $@ -c $< $(XCFLAGS) $(CFLAGS)

%.d: %.c Makefile
	@echo DEP $<
	@$(CC) -fsyntax-only -MM -MF $@ $< $(XCFLAGS) $(CFLAGS)

-include $(CSRC:.c=.d)
