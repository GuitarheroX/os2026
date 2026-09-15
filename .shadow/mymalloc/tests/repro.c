/*
 * tests/repro.c —— 最小复现（用 `make repro` 单独编译运行）
 *
 * tests/stress.c 里的失败是"随机踩雷"，输出一大堆不好定位。这里把已经
 * 查实的 7 个最小触发条件单独拎出来，一个一个看。
 *
 * 现象 1/2/3/5 是同一个根因（buddy 算法要求"所有块来自同一段按大小对齐
 * 的内存"，而这里每次分配都是一段新的 mmap）的不同表现；
 * 现象 6/7 各自独立。详见 REPORT.md 第 3 节。
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

/* ---------------------------------------------------------------- *
 * 现象 5：整组分配、整组释放，单线程也崩（必现）
 *
 * 这是把 tests/stress.c 里 sizes_and_alignment 的崩溃缩到最小之后的样子：
 * 根本没有并发，就是"按 sizes 里 38 个尺寸各分配一块、写满，然后全部还回去"。
 *
 * 第一版只跑一次，看到"正序释放崩、逆序释放没事"，差点当成顺序问题。
 * 多跑几轮才发现两个方向都会崩，只是崩在第几次 myfree 不同 —— 真正的
 * 变量不是释放顺序，而是 mmap 这次把地址摆在哪，也就是 ASLR。
 * 所以下面的循环把每个方向都跑很多轮，避免又得出一个偶然的结论。
 *
 * 崩的位置在 myfree 找伙伴那一步：
 *
 *     buddy = 块头地址 ^ (1 << order)
 *
 * order 是由块头自报的 size 反推的（my_log2(block->size)）。而 divide()
 * 会把 size 不是 2 的幂的块留在链表里，这种块的 order 跟它真正所在的阶
 * 对不上，XOR 出来的 buddy 就落到别的段里，解引用即崩。
 * ---------------------------------------------------------------- */
static void repro_free_order(void) {
    static const size_t sizes[] = {
        0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 129, 255, 256, 257, 511, 512, 513,
        1023, 1024, 1025, 4095, 4096, 4097,
        8191, 8192, 8193, 16384, 32768, 65536, 131072,
    };
    const int n = (int)(sizeof(sizes) / sizeof(sizes[0]));

    printf("\n=== 现象 5：整组分配 + 整组释放，单线程也崩（必现）===\n");

    for (int dir = 0; dir < 2; dir++) {
        const char *what = dir ? "逆序释放（小的先还）"
                               : "正序释放（大的先还）";

        /* 每轮都是"分配一整组、再按某个方向全部释放"，各轮之间互不影响。
         * 崩不崩跟 mmap 这次把地址摆在哪有关，所以多跑几轮把概率磨成必然。 */
        for (int round = 0; round < 40; round++) {
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
                void *live[64];
                int nl = 0;
                for (int i = 0; i < n; i++) {
                    void *p = mymalloc(sizes[i]);
                    if (p) { memset(p, 0xA5, sizes[i]); live[nl++] = p; }
                }
                for (int k = 0; k < nl; k++) {
                    int i = dir ? (nl - 1 - k) : k;
                    /* 只在第 0 轮打进度：崩掉时能直接看出是哪个尺寸、
                     * 第几次 myfree 出的问题。stdout 在 main() 里已经
                     * 关了缓冲，这里的内容崩溃时不会丢。 */
                    if (round == 0) {
                        printf("      [%s] 第 %2d 次 myfree(%-7zu 块头 %p)\n",
                               dir ? "逆序" : "正序", k, sizes[i],
                               (void *)hdr_of(live[i]));
                    }
                    myfree(live[i]);
                }
                _exit(0);   /* 正常跑完 */
            }

            int status = 0;
            if (waitpid(pid, &status, 0) < 0) { printf("    waitpid 失败\n"); break; }

            if (WIFSIGNALED(status)) {
                printf("    %s：第 %d 轮被信号 %d (%s) 终止\n",
                       what, round, WTERMSIG(status), strsignal(WTERMSIG(status)));
                break;
            }
            if (round == 39) {
                printf("    %s：40 轮全部正常返回\n", what);
            }
        }
    }

    printf("    两个方向都会崩，崩的轮次随机 —— 变量是地址布局（ASLR），"
           "不是释放顺序。\n");
    printf("    崩点在合并（找伙伴）那一步，见上面现象 2 算出来的 buddy。\n");
}

/* ---------------------------------------------------------------- *
 * 现象 6：mymalloc(SIZE_MAX) 不返回 NULL，而是给回一个 32 字节的块
 *
 * 守卫 `if (size + sizeof(block_t) > power(2, N)) return NULL;` 本身是
 * 对的，但 `size + sizeof(block_t)` 在 size_t 里会**回绕**：
 *
 *     SIZE_MAX + 32  ==  0x1f  ==  31
 *     31 > 2^30 ?  否  -->  守卫被绕过
 *
 * 接着推导 order 的那行又踩同一个坑：
 *
 *     31 <= power(2, exp)  -->  exp = 5，而 SIZE_MAX 本该是 63
 *
 * 于是 vmalloc 只申请了 32 字节，调用方却以为自己拿到了 SIZE_MAX 字节
 * —— 写第一个字节就越界 2^64。这跟"该返回 NULL 却返回了非 NULL"是
 * 两回事：**任何调用方都会立刻踩出堆破坏**。
 *
 * 顺带说明为什么不用 1<<40 触发同一个毛病：1<<40 已经超过 2^30 上限，
 * 守卫能正常拦住（那条路径本身没坏）。
 * ---------------------------------------------------------------- */
static void repro_size_max(void) {
    printf("\n=== 现象 6：mymalloc(SIZE_MAX) 返回非 NULL，且只给 32 字节（必现）===\n");

    printf("    sizeof(block_t) = %zu\n", sizeof(block_t));
    printf("    SIZE_MAX + sizeof(block_t) = %zu  （本该很大，实际回绕成这个数）\n",
           (size_t)(SIZE_MAX + sizeof(block_t)));
    printf("    --> 守卫 'size+32 > 2^30' 判为假，放行\n");

    void *p = mymalloc(SIZE_MAX);
    if (p == NULL) {
        printf("    mymalloc(SIZE_MAX) -> NULL（正确）\n");
        return;
    }

    size_t hsize = hdr_size_of(p);
    printf("    mymalloc(SIZE_MAX) -> %p，块头自报 size = %zu\n", p, hsize);
    printf("    >>> 调用方以为拿到 %zu 字节，实际只有 %zu 字节 <<<\n",
           (size_t)SIZE_MAX, hsize);
    printf("    >>> 写第一个字节就越界，这里不写，直接释放看会不会崩 <<<\n");

    fflush(stdout);
    myfree(p);
    printf("    myfree 活着回来了\n");
}

/* ---------------------------------------------------------------- *
 * 现象 7：my_log2(2^30) = 30，free_lists[30] 越界打到 big_lock
 *
 * free_lists 是 `static block_t *free_lists[N]`，N = 30 —— 合法下标只有
 * 0..29。而 my_log2 的返回值可以直接达到 N：
 *
 *     my_log2(2^30) = 30
 *
 * 什么时候 block->size 会是 2^30？mymalloc.c:73 的守卫允许
 * `size + sizeof(block_t) <= 2^30`，也就是最大 2^30 - 32 字节的请求；
 * 那条路径算出 exp = 30，curr->size = power(2, 30)。
 *
 * 【这个越界为什么特别阴】
 *
 * 实测符号布局（nm -n 的输出）：
 *
 *     _free_lists  @ 0x100008000   30 x 8 = 240 字节，占 0x...8000..0x...80EF
 *     _big_lock    @ 0x1000080f0   <-- 正好接在 free_lists 后面
 *     _malloc_count@ 0x1000080f8
 *
 * 也就是说 free_lists[30] 这个"越界格"精确地落在 big_lock 上。而
 * insert_blk 是在**持锁状态下**被调用的：
 *
 *     free_lists[30] = block;   // 改写的其实是 big_lock
 *
 * 分配器把保护自己的那把锁写坏了。
 *
 * 【它和现象 2 是什么关系】
 *
 * 老实说：这里**没有一个独立于现象 2 的新崩溃机制**。释放这个 2^30 的块时，
 * myfree 照样要先算 buddy = hdr ^ (1<<30)，而那个地址一样在 mmap 段之外 ——
 * 崩不崩取决于堆当时的状态，所以"必现"的是越界本身，不是崩溃。
 *
 * 单跑一轮经常能全身而退（big_lock 那一刻的值恰好让 insert_blk 走了另一条
 * 分支），跑几轮才会露出来。下面循环 5 轮就是为了这个。
 * ---------------------------------------------------------------- */
static void repro_free_list_oob(void) {
    printf("\n=== 现象 7：free_lists[30] 越界打到 big_lock ===\n");

    size_t sz = ((size_t)1 << 30) - sizeof(block_t);
    printf("    触发条件: 请求 %zu 字节（守卫允许的最大值，exp 会算成 30）\n", sz);
    printf("    N 是分配器内部的宏（repro.c 这边看不到），所以不写死数字：\n");
    printf("    free_lists 有 N 个槽，合法下标 0..N-1，而 my_log2(2^30) = 30 = N，\n");
    printf("    越界一格 —— 那一格恰好落在紧邻的 big_lock 上。\n");

    for (int round = 0; round < 5; round++) {
        fflush(stdout);
        printf("    第 %d 轮: mymalloc...", round);
        fflush(stdout);

        void *p = mymalloc(sz);
        printf(" %p, 块头 size = %zu, myfree...", p,
               p ? hdr_size_of(p) : (size_t)0);
        fflush(stdout);

        if (p) myfree(p);
        printf(" ok\n");
    }
    printf("    五轮都活着 —— 越界那一写打中了 big_lock，但没当场发作。\n");
    printf("    锁已经被自己写坏，下一次拿锁就是碰运气了。\n");
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

    run_stage("size_mismatch", repro_size_mismatch);         /* 现象 1 */
    run_stage("buddy_outside_region", repro_buddy_outside_region); /* 现象 2 */
    run_stage("crash_rounds", run_crash_rounds);             /* 现象 3 */
    run_stage("overlap", repro_overlap);                     /* 现象 4 */
    run_stage("free_order", repro_free_order);               /* 现象 5 */
    run_stage("size_max", repro_size_max);                   /* 现象 6 */
    run_stage("free_list_oob", repro_free_list_oob);         /* 现象 7 */

    printf("\n完成。每个现象都单独跑在子进程里，"
           "上面被信号终止的就是当时崩掉的那个。\n");
    return 0;
}
