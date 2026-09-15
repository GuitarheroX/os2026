/*
 * tests/repro.c —— 最小复现（用 `make repro` 单独编译运行）
 *
 * tests/stress.c 里的失败是"随机踩雷"，输出一大堆不好定位。这里把已经
 * 查实的几个最小触发条件单独拎出来，一个一个看。
 *
 * 运行：
 *   make repro && ./repro
 *
 * 注意：这个文件自带 main()，与 tests/main.c 冲突，所以它**不**参与
 * testkit 那个 test 可执行文件的链接（见 Makefile 里的 SRCS）。
 *
 * 为什么有的现象每次都能复现、有的却时灵时不灵？
 * 因为 myfree 找伙伴用的是"块头地址 XOR 块大小"，算出来的地址落在哪
 * 完全取决于 mmap 这次把内存给了谁 —— 也就是取决于 ASLR 和之前的分配
 * 历史。所以容易漏的那些，用"重复很多轮"把概率变成必然。
 */

#include <mymalloc.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static uintptr_t hdr_of(void *p) {
    /* 头部紧贴在负载之前；M5 没规定头部大小，本实现是 32 字节 */
    return (uintptr_t)p - sizeof(block_t);
}

static size_t hdr_size_of(void *p) {
    return *(const size_t *)hdr_of(p);
}

static int order_of(size_t size) {
    int o = 0;
    while (size > 1) { size >>= 1; o++; }
    return o;
}

/* 跑在子进程里的那些函数，不要打总结行（父进程会打） */
static int in_child;

/* 让每个尺寸各跑一个子进程，这样崩掉的那个不会把后面的结果一起带走 */
static void for_each_size(void (*fn)(size_t), const size_t *sizes, int n) {
    for (int i = 0; i < n; i++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            in_child = 1;
            fn(sizes[i]);
            fflush(stdout);
            _exit(0);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) { printf("    waitpid 失败\n"); continue; }
        if (WIFSIGNALED(status)) {
            printf("    >>> mymalloc(%zu) 这一轮被信号 %d (%s) 终止 <<<\n",
                   sizes[i], WTERMSIG(status), strsignal(WTERMSIG(status)));
        }
    }
}

/* ---------------------------------------------------------------- *
 * 现象 1：块头里记的 size 大于 vmalloc 实际要来的内存（必现）
 *
 * mymalloc 算出 order 以后向 vmalloc 要了 2^exp 字节，但 divide() 只在
 * "(size + 头部) × 2 > 本块大小" 时才继续劈分 —— 于是整块 2^exp 的
 * header.size 被原样留给了调用者。之后 myfree 按这个偏大的 size 去找
 * 伙伴，就直接算到别的区域头上去了。
 * ---------------------------------------------------------------- */
static void one_size_mismatch(size_t size) {
    void *p = mymalloc(size);
    if (!p) { printf("    mymalloc(%6zu) -> NULL\n", size); return; }

    uintptr_t hdr = hdr_of(p);
    size_t hsize = hdr_size_of(p);

    printf("    mymalloc(%6zu) -> 块头 %p 自报 size = %7zu"
           "（多出 %zu 字节，不在本段 mmap 之内）\n",
           size, (void *)hdr, hsize, hsize > size ? hsize - size : 0);
    myfree(p);
}

static void repro_size_mismatch(void) {
    static const size_t sizes[] = {4096, 8192, 16384, 32768, 65536, 131072};
    printf("\n=== 现象 1：块头 size 与 vmalloc 实际请求量不一致（必现）===\n");
    for_each_size(one_size_mismatch, sizes,
                  (int)(sizeof(sizes) / sizeof(*sizes)));
}

/* ---------------------------------------------------------------- *
 * 现象 2：XOR 算出的伙伴地址落在 mmap 区域之外（必现）
 *
 * buddy = 块头地址 ^ 块大小。这个式子成立的前提是"两块都来自同一段
 * 按块大小对齐的内存"，而 vmalloc 只保证页对齐。各自的块头地址由
 * mmap 随便给，XOR 出来的地址经常根本不属于本分配器 —— myfree 却会
 * 直接去读它的 header。
 * ---------------------------------------------------------------- */
static void one_buddy_check(size_t size) {
    void *p = mymalloc(size);
    if (!p) { printf("    mymalloc(%6zu) -> NULL\n", size); return; }

    uintptr_t hdr = hdr_of(p);
    size_t hsize = hdr_size_of(p);
    uintptr_t buddy = hdr ^ ((uintptr_t)1 << order_of(hsize));

    int inside = (buddy >= hdr && buddy < hdr + size);
    printf("    mymalloc(%6zu)：块头 %p，本段 mmap 覆盖 [%p, %p)\n"
           "                  buddy = %p -> %s\n",
           size, (void *)hdr, (void *)hdr, (void *)(hdr + size),
           (void *)buddy,
           inside ? "在本段之内" : "不在本段之内！myfree 会直接解引用它");
    myfree(p);
}

static void repro_buddy_outside_region(void) {
    static const size_t sizes[] = {4096, 8192, 16384, 32768, 65536, 131072};
    printf("\n=== 现象 2：XOR 算出的伙伴地址落在 mmap 区域之外（必现）===\n");
    for_each_size(one_buddy_check, sizes,
                  (int)(sizeof(sizes) / sizeof(*sizes)));
}

/* ---------------------------------------------------------------- *
 * 现象 3：反复做最小的分配/释放，几十轮内必然崩一次
 *
 * 每一轮都是"分配 4K / 8K / 16K / 32K，各自立刻释放"，单看一轮毫无
 * 特别之处。但只要 mmap 恰好把某两段地址排布成"本不该是伙伴、XOR
 * 却互相指到对方"，myfree 就会去读一段不属于自己的内存。
 * ---------------------------------------------------------------- */
static void repro_crash_within_rounds(int max_rounds) {
    printf("\n=== 现象 3：最小序列反复执行，看第几轮崩（必现，只是轮数不定）===\n");

    for (int round = 0; round < max_rounds; round++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            size_t sizes[] = {4096, 8192, 16384, 32768};
            for (int i = 0; i < 4; i++) {
                void *p = mymalloc(sizes[i]);
                if (p) myfree(p);
            }
            _exit(0);
        }

        int status = 0;
        if (waitpid(pid, &status, 0) < 0) { printf("  waitpid 失败\n"); return; }

        if (status != 0) {
            printf("  第 %d 轮崩溃（status = %d）。"
                   "崩溃点在 myfree 读伙伴的 header。\n", round, status);
            return;
        }
    }

    printf("  %d 轮都没崩 —— 这次运气不错，多跑几遍。\n", max_rounds);
}

static void run_crash_rounds(void) {
    repro_crash_within_rounds(60);
}

/* ---------------------------------------------------------------- *
 * 现象 4：新分配的地址落在还在使用的块身上（要求 2 直接违反）
 * ---------------------------------------------------------------- */
static void repro_overlap(void) {
    printf("\n=== 现象 4：新分配的地址落在存活块身上 ===\n");

    enum { N = 24 };
    static void *live[N];

    for (int i = 0; i < N; i++) {
        live[i] = mymalloc(4096);
        if (live[i]) memset(live[i], 0xA5, 4096);
    }
    /* 释放偶数下标的，制造空洞并触发合并 */
    for (int i = 0; i < N; i += 2) {
        if (live[i]) { myfree(live[i]); live[i] = NULL; }
    }

    int hits = 0;
    for (int round = 0; round < 400 && hits < 6; round++) {
        size_t size = (size_t)32u << (round % 9);
        void *p = mymalloc(size);
        if (!p) continue;

        for (int i = 1; i < N; i += 2) {
            if (!live[i]) continue;
            if ((char *)p < (char *)live[i] + 4096 &&
                (char *)live[i] < (char *)p + size) {
                printf("  mymalloc(%zu) 返回 %p，但 [%p, %p) 仍在使用中\n",
                       size, p, live[i], (char *)live[i] + 4096);
                hits++;
                break;
            }
        }
        if ((uintptr_t)p & 7u) {
            printf("  mymalloc(%zu) 返回 %p，低 3 位 = %zu，不是 8 字节对齐\n",
                   size, p, (uintptr_t)p & 7u);
            hits++;
        }
        myfree(p);
    }

    printf("  命中 %d 次%s\n", hits,
           hits ? " —— 分配器把正在使用的内存交给了调用者"
                : "（这类错误与 mmap 给到的地址有关，换次运行可能就复现了）");

    for (int i = 1; i < N; i += 2) {
        if (live[i]) myfree(live[i]);
    }
}

typedef void (*stage_fn)(void);

/* 每个现象都在自己的子进程里跑：分配器崩起来是 SIGSEGV/SIGBUS，
 * 放在同一个进程里，第一个崩了后面的就都看不到了。 */
static void run_stage(const char *title, stage_fn fn) {
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        fn();
        fflush(stdout);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        printf("  [%s] waitpid 失败\n", title);
        return;
    }
    if (WIFSIGNALED(status)) {
        printf("  >>> [%s] 被信号 %d (%s) 终止 <<<\n",
               title, WTERMSIG(status), strsignal(WTERMSIG(status)));
    }
}

int main(void) {
    /* 分配器一崩就是 SIGSEGV/SIGBUS，缓冲区里的内容会跟着一起丢。
     * 一定要关掉缓冲，否则很可能什么都看不到。 */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("mymalloc 最小复现\n");
    printf("页大小 = %ld 字节\n", sysconf(_SC_PAGESIZE));

    run_stage("size_mismatch", repro_size_mismatch);
    run_stage("buddy_outside_region", repro_buddy_outside_region);
    run_stage("crash_rounds", run_crash_rounds);
    run_stage("overlap", repro_overlap);

    printf("\n完成。每个现象都单独跑在子进程里，"
           "上面被信号终止的就是当时崩掉的那个。\n");
    return 0;
}
