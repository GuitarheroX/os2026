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

# 覆盖 SRCS，在 oslab.mk 之后定义才能生效。
# tests/repro.c 自带 main()，与 tests/main.c 冲突，所以这里逐个列出、
# 而不是 find 一整个目录（见文件末尾的 repro 目标）。
#
# 注意这里是 tests/vmtrace.c 而不是 start.c —— vmtrace.c 自己
# #include 了 start.c（改名后拦截 vmalloc/vmfree），两个都写上会符号重复。
SRCS := tests/main.c tests/stress.c tests/tests-trivial.c tests/vmtrace.c mymalloc.c

# ---------------------------------------------------------------------------
# 压力测试相关（本项目新增）
#
#   make test  —— 编译并运行 testkit 用例（含 tests/stress.c 的压力测试）
#   make repro —— 单独编译最小复现程序 ./repro
#
# testkit 用例只在设置了 TK_RUN / TK_VERBOSE 时才注册，所以要带 TK_RUN 跑。
# 加 TK_VERBOSE=1 可以把失败用例的日志打出来。
# ---------------------------------------------------------------------------
.PHONY: test repro approx

test: $(NAME)
	@TK_RUN=1 TK_VERBOSE=1 ./$(NAME)

repro:
	$(CC) $(CFLAGS) -O2 -Wall mymalloc.c start.c tests/repro.c -o repro

# ---------------------------------------------------------------------------
# approx —— 测试框架的自检（"approximate oracle"，不参与提交）
#
# 用同一套 12 个用例去跑一个**必然正确**的参考 buddy 分配器
# （tests/refalloc.c），用来回答"失败到底是分配器的 bug，还是测试框架
# 在误报"。我的 mymalloc/myfree 是弱符号，会被 refalloc.c 里的强符号压掉。
#
#   approx 全过 -> 框架可信，真实分配器上的失败都是真 bug
#   approx 也挂 -> 框架有问题，得先修框架
# ---------------------------------------------------------------------------
approx:
	$(CC) $(CFLAGS) -O2 -Wall tests/main.c tests/stress.c tests/tests-trivial.c \
	    tests/vmtrace.c mymalloc.c tests/refalloc.c ../testkit/testkit.c -o approx
	@TK_RUN=1 TK_VERBOSE=1 ./approx
