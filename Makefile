#
# simple makefile!
#

TARGET := incgc
CSRC := $(shell find . -iname '*.c')
LIBS :=
CFLAGS := -O2 -pipe -Wall -Wextra -Werror
LDFLAGS := 

include common.mk
