NAME := $(shell basename $(PWD))
export MODULE := M5

CFLAGS := -I.

all: $(NAME) check

check:
	gcc -DFREESTANDING -I. \
	    -ffreestanding -static -nostdlib -nostartfiles -no-pie \
	    -o malloc-check start.c mymalloc.c && \
	        rm -f malloc-check

include ../oslab.mk
include ../.shadow/oslab.mk

# 覆盖 SRCS，在 oslab.mk 之后定义才能生效
SRCS := $(shell find tests/ -maxdepth 1 -name "*.c") start.c mymalloc.c
