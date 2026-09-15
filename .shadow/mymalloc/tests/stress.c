/*
 * tests/stress.c —— mymalloc/myfree 压力测试 (M5)
 *
 * 这个文件是"测试"：它只通过 mymalloc/myfree 这两个公开接口调用分配器，
 * 不依赖、也不修改分配器的内部结构。你可以用它来定位自己的 bug。
 *
 * 逐条对应作业要求（见 M5.md 第 3 节）：
 *   1. 原子性      —— 多处理器并发分配/释放，含"跨线程释放"
 *   2. 无重叠      —— 所有存活区间两两不相交
 *   3. 8 字节对齐  —— 返回地址低 3 位为 0
 *   4. 无内存泄漏  —— 释放的内存可重用；全部释放后记账归零
 *   5. 错误处理    —— 无法满足时返回 NULL，而不是崩溃
 *   6. 内存放大    —— 实际占用不超过申请量的 4 倍
 *
 * 运行：
 *   gcc -I. -I../testkit -O2 -std=gnu2x mymalloc.c start.c tests/ 下的全部 .c
 *       ../testkit/testkit.c -o test && TK_RUN=1 ./test
 */

#include <testkit.h>

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>

/* ------------------------------------------------------------------ *
 * 让 TK_CHECK 报出来的错误真的算"失败"
 *
 * testkit 判定 UnitTest 通过与否，**只看子进程的退出码**
 * （见 testkit.c 的 check_results()：`succ = t->stest || exit_status == 0`）。
 * 它不读任何断言计数器。而本文件里 failure 是记在 `failures` 变量里的，
 * 于是会出现这样的事：用例报了一堆错误，最后仍然显示 PASS —— 只要没崩。
 *
 * 实测踩到过：memory_amplification 算出放大率 163.84x（上限 4x），
 * TK_CHECK 也报了，结果还是 `[PASS]`。这种"假绿"比不测还危险。
 *
 * 所以这里重定义 UnitTest：用例本体跑完，把退出码设成 `failures != 0`。
 *
 * 【别再自己 fork 了 —— 这里栽过一次】
 *
 * 第一版写成"本用例自己 fork 一个孙进程去跑，父进程 wait 孙进程"，
 * 结果**毫无作用**：testkit.c:209 已经为每个用例 fork 了一层，用例本体
 * 本来就跑在子进程里。再 fork 一层，failures 就记在孙进程里了，孙进程
 * `_exit(1)` 退的只是它自己，testkit 等的那个子进程照样 return 0 —— 照样 PASS。
 *
 * 更糟的是那版在父分支里写了 `if (WIFSIGNALED) { signal(...); raise(...); }`，
 * 让子进程自杀来"保留信号语义"。可子进程自杀会直接跳过 testkit 的
 * run_cleanup() 和 fini 钩子，Timeout / SegFault 的判定链也一并绕过了。
 *
 * 正确做法就是什么都不做：让用例本体在 testkit 自己 fork 出来的子进程里
 * 跑，跑完 `_exit(failures != 0)` 交出真实退出码。信号（段错误、闹钟超时）
 * 由 testkit 照常捕获 —— 它本来就在子进程里设了 alarm()。
 *
 * 放在 #include <testkit.h> 之后，只影响本文件（testkit.h 里的
 * __tk_testcase 定义在前，这里覆盖的是它的调用宏 UnitTest）。
 * ------------------------------------------------------------------ */
/* 这两个间接层是必须的：在嵌套宏里 # 和 ## 不会先展开参数，
 * 直接写 #name_ / run##name_ 只会拼出字面量 "name_" / "runname__行号"。 */
#define TK_STR_(x) #x
#define TK_STR(x)  TK_STR_(x)
#define TK_CAT_(a, b) a##b
#define TK_CAT(a, b)  TK_CAT_(a, b)
/* 拼出"前缀 + 用例名 + 行号"的标识符。
 *
 * 参数名不能用 name —— 它会被替换进 token 里，跟宏外面的参数撞名。
 *
 * 还有一个更阴的坑，试了七八次才试出来：**宏体里的顺序不能随便改**。
 * 如果先引用 TK_FN(__tk_run, ...)，再定义它，中间还夹着别的声明，
 * __LINE__ 会展开成两个不同的值 —— 声明的和引用的名字对不上，
 * 报"use of undeclared identifier"。把 run 的定义挪到 reg 之前就好了。
 * 这大概是因为宏参数在扫描时遇到 `struct` 之类的关键字会重新进入
 * 一轮参数收集，__LINE__ 在那时被重新求值。总之别动这个顺序。
 */
#define TK_FN(pfx, n_) TK_CAT(pfx, TK_CAT(n_, TK_CAT(_, __LINE__)))

#undef UnitTest
#define UnitTest(name_, ...)                                                \
    static void TK_FN(__tk_, name_)(void);                                  \
    static void TK_FN(__tk_run, name_)(void) {                              \
        TK_FN(__tk_, name_)();                                              \
        fflush(stdout);                                                     \
        fflush(stderr);                                                     \
        _exit(failures != 0);                                               \
    }                                                                       \
    __attribute__((constructor))                                            \
    void TK_FN(__tk_reg, name_)(void) {                                     \
        void tk_add_test(struct tk_testcase t);                             \
        tk_add_test((struct tk_testcase){                                   \
            .enabled = 1,                                                   \
            .name = TK_STR(name_),                                          \
            .loc = __FILE__ ":" TK_STR(__LINE__),                           \
            .utest = TK_FN(__tk_run, name_),                                \
            __VA_ARGS__                                                     \
        });                                                                 \
    }                                                                       \
    static void TK_FN(__tk_, name_)(void)

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mymalloc.h>

/* ------------------------------------------------------------------ *
 * 基础工具
 * ------------------------------------------------------------------ */

/* 只有真实的内存错误才计入 failures；"分配失败 / 复用率低" 这类
 * 与分配策略有关的观察值走 expectations，两者分开统计。 */
static int failures;
static int expectations;

static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;

#define PLAYER(fmt, ...)                                                    \
    do {                                                                    \
        pthread_mutex_lock(&report_lock);                                   \
        failures++;                                                         \
        printf("    [%s] " fmt "\n", __func__, ##__VA_ARGS__);              \
        pthread_mutex_unlock(&report_lock);                                 \
    } while (0)

#define NOTE(fmt, ...)                                                      \
    do {                                                                    \
        pthread_mutex_lock(&report_lock);                                   \
        expectations++;                                                     \
        printf("    [%s] (观察) " fmt "\n", __func__, ##__VA_ARGS__);       \
        pthread_mutex_unlock(&report_lock);                                 \
    } while (0)

/* 一个用例出错时可能连续报上千万条相同的话，只报前若干条 */
#define TK_CHECK_LIMIT 12
static atomic_int check_reported;

#define TK_CHECK(cond, fmt, ...)                                            \
    do {                                                                    \
        if (!(cond)) {                                                      \
            if (atomic_fetch_add(&check_reported, 1) < TK_CHECK_LIMIT) {     \
                PLAYER(fmt, ##__VA_ARGS__);                                 \
            } else if (atomic_fetch_add(&check_reported, 0) == TK_CHECK_LIMIT) { \
                PLAYER("（同类错误过多，后续不再重复输出）");                 \
            }                                                               \
        }                                                                   \
    } while (0)

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 确定性 PRNG：不用 rand()，保证同一 seed 下失败可复现 */
static inline uint64_t rng_next(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (*s = x);
}

#define PATTERN(k)  ((unsigned char)(0xA5u ^ (unsigned)(uintptr_t)(k)))
#define DEAD_BYTE   ((unsigned char)0xDE)

/* 估算用：常见 header 大约是 32 字节，只影响打印出来的估算值 */
#define EST_HEADER 32

/* ------------------------------------------------------------------ *
 * 1. 区间模型：重叠检测（要求 2）
 *
 * 维护"当前存活区间"的有序数组；插入时二分定位，再与左右邻居比较。
 * 插入/删除是 O(n) 的 memmove，但常数极小，足以支撑几十万次操作。
 * ------------------------------------------------------------------ */

/*
 * 模型按线程分片：每个线程一份，自己维护自己的存活区间集合。
 * 这样被测代码（分配器）的锁争抢才是唯一的争抢来源 —— 否则测试框架
 * 自己就成了一把全局大锁，既拖慢测试，也掩盖真实性能表现。
 * 分片复用的是同一块静态内存，各线程只是各自记一笔各自的账。
 */
/* 并发压测的规模。ST_THREADS 要与 RANGE_SHARDS 匹配：
 * 每个线程独占一个分片，压测的记账开销就不会变成新的全局锁
 * （第一版就是在这里踩坑的，见文件末尾"踩过的坑"）。 */
#define ST_THREADS 4
#define ST_ITERS 60000
#define ST_SAMPLE 256          /* 每线程最多驻留的"抽样块"数量 */

/* 单线程最多驻留的存活区间数。memory_amplification 的负载扫描峰值有
 * 20000 块，所以这里必须 ≥ 20000，否则 rng_insert 返回 2（容量不足）
 * —— 那个返回值**不是**"重叠"，会从下面的 TK_CHECK 底下溜过去，
 * 然后 rng_remove 找不到区间变成空操作，整张区间表就废了。 */
#define RANGE_CAP 32768
#define RANGE_SHARDS 8

typedef struct {
    uintptr_t base;
    size_t size;
} range_t;

static range_t ranges[RANGE_SHARDS][RANGE_CAP];
static size_t range_n[RANGE_SHARDS];

static atomic_int shard_next;

/* 每个线程一份模型：把 shard 号存成线程局部变量，调用点就不必到处传。
 *
 * 【n 为什么是指针】区间计数只有**一份**，存在 range_n[shard] 里。
 * 原先这里存的是 `size_t n` 这个副本，于是同一次操作就有了两个真相：
 * 插入只更新副本、range_n[] 永远是 0，而 rng_remove_shard 又是按
 * range_n[] 去摘的 —— 它拿着一个空表去删，什么都删不掉，而且一声不吭。
 * 表现出来就是"第 1 轮释放全部静默失败，第 2 轮起每次分配都报 overlap"。
 * 存指针之后，无论从哪个入口进来，动的都是同一份计数。 */
typedef struct {
    range_t *r;
    size_t *n;
} model_t;

static _Thread_local model_t tls_model;
static _Thread_local int tls_shard = -1;

static model_t *model_of_this_thread(void) {
    if (tls_shard < 0) {
        tls_shard = atomic_fetch_add(&shard_next, 1) % RANGE_SHARDS;
        tls_model = (model_t){ ranges[tls_shard], &range_n[tls_shard] };
    }
    return &tls_model;
}

static size_t rng_lower_bound(const range_t *v, size_t n, uintptr_t base) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v[mid].base < base) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* 返回 0 = 插入成功且不与任何存活区间重叠；1 = 重叠；2 = 模型容量不足 */
static int rng_insert(model_t *m, uintptr_t base, size_t size) {
    if (*m->n == RANGE_CAP) return 2;

    size_t at = rng_lower_bound(m->r, *m->n, base);
    if (at > 0 && m->r[at - 1].base + m->r[at - 1].size > base) return 1;
    if (at < *m->n && m->r[at].base < base + size) return 1;

    memmove(&m->r[at + 1], &m->r[at], (*m->n - at) * sizeof(range_t));
    m->r[at].base = base;
    m->r[at].size = size;
    (*m->n)++;
    return 0;
}

static void rng_remove(model_t *m, uintptr_t base) {
    size_t at = rng_lower_bound(m->r, *m->n, base);
    if (at < *m->n && m->r[at].base == base) {
        memmove(&m->r[at], &m->r[at + 1], (*m->n - at - 1) * sizeof(range_t));
        (*m->n)--;
    }
}

/* 按**分片号**摘区间，而不是按"当前线程"。
 *
 * 释放线程未必是分配线程，而区间登记在分配线程的分片里。用
 * model_of_this_thread() 会去找另一张表、找不到、静默失败 —— 那条区间就
 * 永远留在分配线程的账上，此后合法的地址复用会被判成 overlap。
 *
 * 直接用分片号构造一个 model_t 即可：分片的内存是静态的，不需要加锁
 * （每个分片同一时刻只有一个线程在动，见 model_of_this_thread 的说明）。 */
static void rng_remove_shard(int shard, uintptr_t base) {
    model_t m = { ranges[shard], &range_n[shard] };
    rng_remove(&m, base);
}

/* 存活字节数记账：跨线程用原子操作累加。压测结束时必须回到 0，
 * 这是"无内存泄漏"（要求 4）最直接的证据。 */
static atomic_llong live_bytes;
static atomic_llong alloc_calls;

/* 并发压测的进度表 + 看门狗开关，定义在这里是为了让 model_reset_all() 也能复位它们 */
static atomic_long st_progress[ST_THREADS];
static atomic_bool st_watchdog_on;

/* ------------------------------------------------------------------ *
 * 2. 每个分配块的元数据
 * ------------------------------------------------------------------ */

typedef struct {
    void *p;                 /* mymalloc 返回的指针 */
    size_t size;             /* 申请的字节数 */
    unsigned char pattern;   /* 分配时写入的填充值 */
    /* 这个块是在哪个分片上登记的存活区间。
     *
     * 【为什么必须记下来】区间是按 base 精确匹配删除的，而"分配"和"释放"
     * 可以是**两个不同的线程**（cross_thread_free 就是这么设计的）。
     * 如果删除时按"当前线程"的 TLS 分片去找，就会去另一张表里找、找不到、
     * 静默失败 —— 于是这条区间永远留在分配线程的表里。那个用例跑完之后
     * 4 张表里会积压 16384 条"其实早就释放了"的区间，此后任何**合法的地址
     * 复用**都会被判成 overlap（假失败）。现在把登记时的分片号带着走，
     * 归属就唯一确定了。 */
    unsigned char shard;
} blk_t;

/* 静态预分配，避免测试自己调用 libc malloc 干扰内存观测 */
static blk_t pool[1 << 18];
static atomic_int pool_next;

static blk_t *pool_take(void) {
    int i = atomic_fetch_add(&pool_next, 1);
    if (i >= (int)(sizeof(pool) / sizeof(pool[0]))) return NULL;
    return &pool[i];
}

/* 分配 + 立刻做廉价检查（对齐、写读、哨兵、重叠）。
 * 返回 NULL 表示这次分配失败（合法的 NULL 或池子满）。 */
static blk_t *alloc_checked(size_t size, int *pool_full) {
    *pool_full = 0;
    void *p = mymalloc(size);

    if (p == NULL) {
        /* 很大的请求返回 NULL 是正常的（要求 5） */
        TK_CHECK(size > (1u << 24),
                 "mymalloc(%zu) 返回 NULL：请求量不大却分配失败"
                 "（碎片过多或错误处理有问题）", size);
        return NULL;
    }

    /* 要求 3：8 字节对齐
     *
     * 这条一旦触发，别只当成"对齐算错了"。正常路径上 mymalloc 返回的是
     * `(void *)(curr + 1)`，而 curr 必然是 block_t *（32 字节、对齐 8），
     * 所以**干净路径下不可能不对齐** —— 实测 600 次顺序分配 0 次不对齐。
     *
     * 并发用例里看到 `0x…1cd` / `0x…223` 这种**奇数**地址，说明 curr 已经
     * 不是块头了，是从损坏的空闲链表里取出来的垃圾指针。也就是分配器把
     * 同一段内存发给了两个人（和 verify_blk 抓到的"存活块里出现陈旧哨兵"
     * 是同一件事的不同表现）。地址连偶数都不是，是比"内容被改写"更硬的
     * 证据 —— 内容可能被误判，指针低 3 位不会。 */
    TK_CHECK(((uintptr_t)p & 7u) == 0,
             "mymalloc(%zu) 返回 %p，不是 8 字节对齐"
             "（地址为奇数说明这个指针根本不是块头，是空闲链表损坏后的垃圾值）",
             size, p);

    /* 写入 + 读回 */
    unsigned char *q = (unsigned char *)p;
    unsigned char pattern = PATTERN((uintptr_t)p + size);
    memset(q, pattern, size);
    if (size > 0) {
        TK_CHECK(q[size / 2] == pattern,
                 "mymalloc(%zu) 返回的 %p 写入后读回不一致", size, p);
    }

    /* 【为什么没有"块尾哨兵"了 —— 这里踩过一个很贵的坑】
     *
     * 原先在 `q[size]` 写一个哨兵字节。那是**申请范围之外**的第一个字节，
     * 而契约只保证 `[p, p+size)` 可读写 —— p+size 落在哪完全由分配器的
     * 内部布局决定：本项目的 buddy 分配器给 mymalloc(size) 的块是
     * 2^ceil(log2(size+32))，当 size+32 恰好是 2 的幂时（size ∈
     * {0,32,96,224,...}）块没有任何余量，p[size] 就**正好压在相邻块的块头
     * `size` 字段上**。
     *
     * 实测（/tmp/canary2.c）：连续 200 次 mymalloc(32)，
     *
     *     不写哨兵：重复分配 0 次
     *     写哨兵  ：#2 起反复返回同一个地址 0x108000060
     *
     * 因为 size 字段被 0x5A 覆盖后，remove_blk 按 my_log2(0x5A)=6 去
     * free_lists[6] 摘链，而那块挂在 free_lists[7] —— 摘了个寂寞，同一段
     * 内存就反复发给了不同的调用者。
     *
     * 改成写 `q[size-1]` 也不行：那样哨兵落在载荷**里面**，size=1 时
     * 它就等于载荷本身，测试写坏自己的数据再去抱怨"载荷被改写"。
     *
     * 结论是**哨兵这个思路在当前布局下无路可走**：契约以外的字节必然属于
     * 分配器，测试没有权利写。所以这里只写契约范围内的载荷。"分配器给的
     * 块比申请的还小"由 `q[size/2]` 的读回校验和下面的区间模型一起盯 ——
     * 后者登记的是分配器**真正返回**的区间，比一个哨兵字节更直接。
     *
     * 要求 2：与其它存活区间不重叠
     *
     * rng_insert 有三个返回值，必须分开判：0 成功、1 重叠、2 容量不足。
     * 只写 `r != 1` 的话，容量不足（2）会被当成"没有重叠"放过去 ——
     * 而这一次插入根本没发生，之后 rng_remove 也就摘不掉任何东西，
     * 区间表和实际存活集合从此对不上。这种失效是静默的，比崩溃更难查。 */
    model_t *m = model_of_this_thread();
    int r = rng_insert(m, (uintptr_t)p, size + 1);
    TK_CHECK(r != 1, "mymalloc(%zu) 返回 %p，与已分配区间重叠（overlap）",
             size, p);
    TK_CHECK(r != 2, "区间模型容量不足（RANGE_CAP=%d），"
             "本用例的存活块数超过了记账上限，请调大 RANGE_CAP",
             (int)RANGE_CAP);

    atomic_fetch_add(&live_bytes, (long long)size);
    atomic_fetch_add(&alloc_calls, 1);

    blk_t *b = pool_take();
    if (!b) {
        *pool_full = 1;
        /* 池子满了：把这块还回去，避免污染后续检查 */
        rng_remove(m, (uintptr_t)p);
        atomic_fetch_sub(&live_bytes, (long long)size);
        myfree(p);

        /* 【池满必须喊一声】metadata 池是单调递增的（只取不还），
         * concurrent_churn 一个用例就要 240000 个、占掉容量的 91.5%，
         * 余量只有 8.4%。一旦超了，调用方只会看到"分配失败"然后继续跑，
         * 最后照样打印"4 线程 x 60000 次操作完成" —— 整个用例静默降级成
         * 空转，看起来却是通过的。这种假绿比失败更危险，所以在这里报一声。
         * 只报第一次，避免在热路径上刷屏。 */
        static atomic_bool pool_warned;
        if (!atomic_exchange(&pool_warned, true)) {
            NOTE("metadata 池已满（容量 %zu 个），自此以后的分配不再被检查，"
                 "本用例的结论无效 —— 请调大 pool[] 或减小 ST_ITERS",
                 sizeof(pool) / sizeof(pool[0]));
        }
        return NULL;
    }

    b->p = p;
    b->size = size;
    b->pattern = pattern;
    /* 记下这份区间登记在哪个分片上，释放时按它去摘（见 blk_t.shard） */
    b->shard = (unsigned char)tls_shard;
    return b;
}

/* 释放前校验内容完整：抽检首尾字节（便宜），full 时全量扫描。 */
static void verify_blk(blk_t *b, int full) {
    if (!b) return;
    unsigned char *q = (unsigned char *)b->p;

    if (b->size != 0) {
        TK_CHECK(q[0] == b->pattern,
                 "块 %p (size=%zu) 首字节被改写：期望 %02x 实得 %02x"
                 "（可能与他人的分配重叠，或头/负载边界算错）",
                 b->p, b->size, b->pattern, q[0]);
        TK_CHECK(q[b->size - 1] == b->pattern,
                 "块 %p (size=%zu) 末字节被改写：期望 %02x 实得 %02x"
                 "（载荷最后 1 字节都不可写，说明分配器给的块比申请的还小）",
                 b->p, b->size, b->pattern, q[b->size - 1]);
    }

    if (full) {
        for (size_t i = 0; i < b->size; i++) {
            if (q[i] != b->pattern) {
                TK_CHECK(0, "块 %p (size=%zu) 第 %zu 字节被改写：期望 %02x 实得 %02x",
                         b->p, b->size, i, b->pattern, q[i]);
                break;
            }
        }
    }
}

static void free_blk(blk_t *b, int full) {
    if (!b) return;
    verify_blk(b, full);

    /* 置为"已死"填充值：若分配器之后仍把这块内存交给别人，
     * 那次分配的内容校验就会失败 —— 等于抓到了 use-after-free。 */
    memset(b->p, DEAD_BYTE, b->size);

    /* 按**登记时**的分片摘，不能用 model_of_this_thread()。
     * 释放线程未必是分配线程（cross_thread_free 就是故意这么干的），
     * 用当前线程的分片会去另一张表里找、找不到、静默失败，那条区间就
     * 永远留在分配线程的表里，之后合法的地址复用会被误判成 overlap。 */
    rng_remove_shard(b->shard, (uintptr_t)b->p);
    atomic_fetch_sub(&live_bytes, (long long)b->size);

    myfree(b->p);
}

/* ------------------------------------------------------------------ *
 * 3. 并发闸门：让所有线程先各自忙完，再一起冲过起跑线
 * ------------------------------------------------------------------ */

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int arrived;
    int n;
} gate_t;

static void gate_init(gate_t *g, int n) {
    pthread_mutex_init(&g->m, NULL);
    pthread_cond_init(&g->c, NULL);
    g->arrived = 0;
    g->n = n;
}

static void gate_wait(gate_t *g) {
    pthread_mutex_lock(&g->m);
    if (++g->arrived == g->n) pthread_cond_broadcast(&g->c);
    while (g->arrived < g->n) pthread_cond_wait(&g->c, &g->m);
    pthread_mutex_unlock(&g->m);
}

/* ------------------------------------------------------------------ *
 * 测试用例
 * ------------------------------------------------------------------ */

/* 每个用例开头重置记账，保证用例之间互不影响 */
static void model_reset_all(void) {
    for (int i = 0; i < RANGE_SHARDS; i++) range_n[i] = 0;
    atomic_store(&shard_next, 0);
    atomic_store(&pool_next, 0);
    atomic_store(&live_bytes, 0);
    atomic_store(&alloc_calls, 0);
    tls_shard = -1;        /* 本线程重新领一个分片 */
    atomic_store(&check_reported, 0);

    for (int i = 0; i < ST_THREADS; i++) atomic_store(&st_progress[i], 0);
    atomic_store(&st_watchdog_on, false);
}

/* 用例开场白：分配器一崩就是 SIGSEGV/SIGBUS，缓冲区里若一个字都没写，
 * 排查就全靠猜了。先打一行再 flush，崩溃时至少知道跑到了哪个用例。 */
static void banner(const char *what) {
    printf("    [====] %s\n", what);
    fflush(stdout);
}

/* --- 要求 3 + 5：尺寸扫描、边界值、对齐、NULL 语义、无泄漏 --- */
UnitTest(sizes_and_alignment) {
    banner("sizes_and_alignment");
    static const size_t sizes[] = {
        0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 129, 255, 256, 257, 511, 512, 513,
        1023, 1024, 1025, 4095, 4096, 4097,
        8191, 8192, 8193, 16384, 32768, 65536, 131072,
    };
    const int n = (int)(sizeof(sizes) / sizeof(sizes[0]));

    /* 边界尺寸单独走一遍：0 和 1 最容易踩到 order 计算/log2 的坑。
     * 如果这里直接崩掉，说明问题出在分配器内部（例如下标越界）。 */
    blk_t *live[128];
    int nl = 0;
    for (int i = 0; i < n && nl < 128; i++) {
        int full = 0;
        blk_t *b = alloc_checked(sizes[i], &full);
        if (b) live[nl++] = b;
    }

    /* 全量校验：确认没有任何两块互相踩踏 */
    for (int i = 0; i < nl; i++) free_blk(live[i], 1);

    TK_CHECK(atomic_load(&live_bytes) == 0,
             "全部释放后存活字节数 = %lld，应为 0（有内存没有归还）",
             (long long)atomic_load(&live_bytes));

    /* 要求 5：无法满足时应返回 NULL，而不是崩溃 */
    void *huge = mymalloc((size_t)1 << 40);
    TK_CHECK(huge == NULL, "mymalloc(1<<40) = %p，无法满足时应返回 NULL", huge);
    huge = mymalloc((size_t)-1);
    TK_CHECK(huge == NULL, "mymalloc(SIZE_MAX) = %p，无法满足时应返回 NULL", huge);

    printf("    [sizes] 检查了 %d 种尺寸\n", n);
}

/* --- 要求 1 + 2：跨线程释放（一个处理器分配、另一个处理器释放） --- */
#define XT_THREADS 4
#define XT_PER_THREAD 4096

static blk_t *xt_blocks[XT_THREADS][XT_PER_THREAD];
static gate_t xt_gate;

static void *xt_worker(void *arg) {
    long id = (long)arg;
    uint64_t seed = 0x9E3779B97F4A7C15ull * (uint64_t)(id + 1);

    for (int i = 0; i < XT_PER_THREAD; i++) {
        int full = 0;
        size_t size = 1 + (size_t)(rng_next(&seed) % 200);
        xt_blocks[id][i] = alloc_checked(size, &full);
    }

    /* 所有线程在这里汇合，制造最大的并发压力 */
    gate_wait(&xt_gate);

    /* 关键：每个线程释放的是"别的线程"分配的内存 */
    for (int i = 0; i < XT_PER_THREAD; i++) {
        long victim = (id + 1) % XT_THREADS;
        free_blk(xt_blocks[victim][i], (i % 16) == 0);
    }

    return NULL;
}

UnitTest(cross_thread_free) {
    banner("cross_thread_free");
    model_reset_all();
    gate_init(&xt_gate, XT_THREADS);

    pthread_t t[XT_THREADS];
    for (long i = 0; i < XT_THREADS; i++) {
        tk_assert(pthread_create(&t[i], NULL, xt_worker, (void *)i) == 0,
                  "pthread_create 失败");
    }
    for (int i = 0; i < XT_THREADS; i++) pthread_join(t[i], NULL);

    TK_CHECK(atomic_load(&live_bytes) == 0,
             "跨线程释放后存活字节数 = %lld，应为 0",
             (long long)atomic_load(&live_bytes));
    printf("    [cross_thread] %d 线程 x %d 次跨线程 free 完成，存活字节 = %lld\n",
           XT_THREADS, XT_PER_THREAD, (long long)atomic_load(&live_bytes));
}

/* --- 要求 1 + 2 + 4：随机并发压测（主力用例） --- */

/*
 * 看门狗：每隔一秒报一次每个线程走到第几次迭代。
 *
 * 为什么需要它？因为"卡住"和"慢"在 testkit 眼里长得一模一样 ——
 * 都表现为一个 Timeout。但两者的结论完全不同：
 *
 *   进度还在涨            -> 只是慢，调小 ST_ITERS 就行
 *   进度一秒都不动        -> 死锁，函数根本回不来，只能改锁的用法
 *
 * 实测抓到的就是后者：4 个线程在 2 秒内冲到 2000 次左右，然后
 * 进度数字整整 18 秒纹丝不动，直到 testkit 的超时把进程杀掉。
 *
 * 默认不开（免得刷屏），需要时用 TK_CHURN_TRACE=1 打开。
 */
static void *stress_watchdog(void *arg) {
    (void)arg;
    long last[ST_THREADS] = {0};

    for (int tick = 1; tick <= TK_TIME_LIMIT_SEC; tick++) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);

        long cur[ST_THREADS];
        int frozen = 1;
        for (int i = 0; i < ST_THREADS; i++) {
            cur[i] = (long)atomic_load(&st_progress[i]);
            if (cur[i] != last[i]) frozen = 0;
        }

        /* 平时不吭声；一旦全体线程整整一秒没挪窝，就立刻发声 */
        if (atomic_load(&st_watchdog_on) || (frozen && tick > 2)) {
            fprintf(stderr,
                    "      [看门狗 %2ds] 各线程进度:", tick);
            for (int i = 0; i < ST_THREADS; i++) {
                fprintf(stderr, " T%d=%ld(+%ld)", i, cur[i], cur[i] - last[i]);
            }
            fprintf(stderr, "%s\n", frozen ? "  <-- 全部停滞，疑似死锁" : "");
        }

        if (frozen && tick > 2) {
            fprintf(stderr,
                    "      [看门狗] 连续停滞：分配器很可能在 big_lock 上转圈。\n"
                    "      把 TK_CHURN_TRACE 打开可以看到逐秒的完整进度曲线。\n");

            /* 直接退出，不等 testkit 的 alarm 到点。
             *
             * 四个线程卡在 spin_lock 里是**满核空转** —— 实测这个进程
             * 烧掉 100 分钟 CPU 也没自己结束。提前退出把 60 秒的白烧
             * 变成几秒。
             *
             * 用 _exit 而不是 exit：这里已经检测到死锁，四个线程正卡在
             * 锁上，跑 atexit / stdio 清理只会再抓一次大锁，可能永远
             * 回不来。代价是 stdout 缓冲区不 flush，所以要先把
             * stderr（无缓冲）上面那几行诊断打出去 —— 上面已经打了。
             *
             * 退出码 1 由 testkit 记为 "Exited (1)"，日志照常保留。 */
            fflush(NULL);
            _exit(1);
        }

        for (int i = 0; i < ST_THREADS; i++) last[i] = cur[i];
    }
    return NULL;
}

static void *stress_worker(void *arg) {
    long id = (long)arg;
    uint64_t seed = 0xC0FFEEu + 0x1234567u * (uint64_t)id;

    /* 每线程私有的抽样模型：本线程分配的块只由本线程释放，
     * 不存在跨线程共享，所以完全不需要加锁。 */
    blk_t *samples[ST_SAMPLE];
    int nsamp = 0;
    blk_t *tp[1024];           /* 轮转池：制造高频 malloc/free 抖动 */
    int ntp = 0;

    for (int it = 0; it < ST_ITERS; it++) {
        atomic_store_explicit(&st_progress[id], it, memory_order_relaxed);
        /* 尺寸分布刻意偏向小块（贴近真实负载），并混入少量大块 */
        uint64_t r = rng_next(&seed);
        size_t size;
        switch (r % 100) {
            case 0 ... 59:  size = 1 + (size_t)(r / 100) % 64;       break;
            case 60 ... 89: size = 1 + (size_t)(r / 100) % 1024;     break;
            case 90 ... 98: size = 1 + (size_t)(r / 100) % 16384;    break;
            default:        size = 1 + (size_t)(r / 100) % 131072;   break;
        }

        int full = 0;
        blk_t *b = alloc_checked(size, &full);
        if (!b) continue;

        /* 抽样驻留：让一部分块长时间存活，使并发状态更复杂 */
        if ((r >> 20) % 128 == 0) {
            if (nsamp < ST_SAMPLE) {
                samples[nsamp++] = b;
            } else {
                int slot = it % ST_SAMPLE;
                free_blk(samples[slot], 0);
                samples[slot] = b;
            }
        } else if (ntp < 1024) {
            tp[ntp++] = b;
        } else {
            int slot = it % 1024;
            free_blk(tp[slot], 0);
            tp[slot] = b;
        }
    }

    for (int i = 0; i < ntp; i++) free_blk(tp[i], 0);
    for (int i = 0; i < nsamp; i++) free_blk(samples[i], 1);
    return NULL;
}

UnitTest(concurrent_churn) {
    banner("concurrent_churn");
    model_reset_all();

    const char *trace = getenv("TK_CHURN_TRACE");
    atomic_store(&st_watchdog_on, trace != NULL && *trace != '\0' && *trace != '0');
    pthread_t wd;
    pthread_create(&wd, NULL, stress_watchdog, NULL);

    double t0 = now_sec();
    pthread_t t[ST_THREADS];
    for (long i = 0; i < ST_THREADS; i++) {
        tk_assert(pthread_create(&t[i], NULL, stress_worker, (void *)i) == 0,
                  "pthread_create 失败");
    }
    for (int i = 0; i < ST_THREADS; i++) pthread_join(t[i], NULL);
    double dt = now_sec() - t0;

    long long calls = atomic_load(&alloc_calls);
    TK_CHECK(atomic_load(&live_bytes) == 0,
             "压测结束后存活字节数 = %lld，应为 0（泄漏）",
             (long long)atomic_load(&live_bytes));

    printf("    [churn] %d 线程 x %d 次操作，共 %lld 次分配，"
           "耗时 %.2fs (%.1f 万次/秒)，存活字节 = %lld\n",
           ST_THREADS, ST_ITERS, calls, dt,
           dt > 0 ? (double)calls / dt / 1e4 : 0.0,
           (long long)atomic_load(&live_bytes));
}

/* --- 要求 3 + 5：小块高频分配，专打 order 计算与复用路径 --- */
UnitTest(small_block_favorite) {
    banner("small_block_favorite");
    enum { N = 100000, SLOTS = 256 };
    blk_t *v[SLOTS];
    int nv = 0;

    for (int i = 0; i < N; i++) {
        /* 0 和 1 是最容易踩到 order 计算边界（log2(0)、向上取整）的尺寸 */
        size_t size = (i % 3 == 0) ? 0 : (i % 3 == 1 ? 1 : 8);
        int full = 0;
        blk_t *b = alloc_checked(size, &full);
        if (!b) continue;

        if (nv < SLOTS) {
            v[nv++] = b;
        } else {
            int slot = i % SLOTS;
            free_blk(v[slot], 0);
            v[slot] = b;
        }
    }
    for (int i = 0; i < nv; i++) free_blk(v[i], 1);

    TK_CHECK(atomic_load(&live_bytes) == 0,
             "小块循环后存活字节数 = %lld，应为 0",
             (long long)atomic_load(&live_bytes));
    printf("    [small] %d 次小块（0/1/8 字节）分配与释放完成\n", N);
}

/* --- 要求 2 + 4：碎片负载（分配一半、释放一半、用小块填空） --- */
UnitTest(fragmentation_holes) {
    banner("fragmentation_holes");
    model_reset_all();
    enum { M = 4000 };
    static blk_t *v[M];

    for (int i = 0; i < M; i++) {
        int full = 0;
        size_t size = 32 + (size_t)(i % 32) * 8;
        v[i] = alloc_checked(size, &full);
    }

    /* 挖洞：释放奇数下标的块 */
    for (int i = 1; i < M; i += 2) free_blk(v[i], 1);

    /* 用更小的块去填洞；内存压力很小时应当能全部成功 */
    int ok = 0;
    for (int i = 1; i < M; i += 2) {
        void *p = mymalloc(16);
        if (p == NULL) continue;
        TK_CHECK(((uintptr_t)p & 7u) == 0, "填空得到的 %p 未 8 字节对齐", p);
        model_t *m = model_of_this_thread();
        int r = rng_insert(m, (uintptr_t)p, 16);
        TK_CHECK(r != 1, "填空得到的 %p 与存活区间重叠", p);
        /* 和 alloc_checked 一样，容量不足（r==2）必须单独判：只写 r != 1
         * 的话，这次插入根本没发生，后面 rng_remove 也就摘不掉任何东西，
         * 区间表和存活集合从此对不上，而且是静默的。 */
        TK_CHECK(r != 2, "区间模型容量不足（RANGE_CAP=%d）", (int)RANGE_CAP);
        memset(p, PATTERN(i), 16);
        rng_remove(m, (uintptr_t)p);
        myfree(p);
        ok++;
    }

    if (ok < (M / 2) * 9 / 10) {
        NOTE("内存压力很小的碎片负载下，填空失败 %d/%d 次；"
             "频繁分配失败可能导致 hard test failure", M / 2 - ok, M / 2);
    }

    /* 偶数下标的块全程没动过，内容必须完好 */
    for (int i = 0; i < M; i += 2) verify_blk(v[i], 0);
    for (int i = 0; i < M; i += 2) free_blk(v[i], 1);

    TK_CHECK(atomic_load(&live_bytes) == 0,
             "碎片负载后存活字节数 = %lld，应为 0",
             (long long)atomic_load(&live_bytes));
    printf("    [frag] %d 块挖洞后成功填空 %d/%d\n", M, ok, M / 2);
}

/* --- 要求 2：跨 order 混跑，逼出伙伴配对/对齐错误 --- */
UnitTest(mixed_order_churn) {
    banner("mixed_order_churn");
    model_reset_all();
    enum { K = 4000 };
    static blk_t *v[K];

    /* 一轮正序、一轮逆序，检查释放顺序不影响正确性 */
    for (int round = 0; round < 2; round++) {
        for (int j = 0; j < K; j++) {
            int i = (round == 0) ? j : K - 1 - j;
            int full = 0;
            /* 8 B ~ 32 KB，跨越多个 order */
            size_t size = ((size_t)1 << (3 + (i % 12))) - (size_t)(i % 7);
            v[i] = alloc_checked(size, &full);
        }
        for (int i = 0; i < K; i++) {
            free_blk(v[i], round == 1);
        }
    }

    TK_CHECK(atomic_load(&live_bytes) == 0,
             "混合 order 后存活字节数 = %lld，应为 0",
             (long long)atomic_load(&live_bytes));
    printf("    [mixed] %d 块 x 2 轮（正序/逆序）跨 order 分配释放完成\n", K);
}

/* --- 要求 4：内存复用 —— 释放的内存必须能再分配出去 --- */
UnitTest(reuse_after_free) {
    banner("reuse_after_free");
    model_reset_all();
    enum { R = 2000, ROUNDS = 4 };
    static blk_t *v[R];
    int zeroed = 0, counted = 0;

    for (int round = 0; round < ROUNDS; round++) {
        for (int i = 0; i < R; i++) {
            int full = 0;
            blk_t *b = alloc_checked(32, &full);
            if (!b) {
                /* 分配失败是合法的（段用尽 / 池满都会返回 NULL），但
                 * **必须把槽位清空**：v[i] 此刻还指着上一轮已经释放过的块，
                 * 留着它会让下面的释放循环对同一块走两遍 —— verify_blk 读
                 * 已被回收的内存、live_bytes 二次扣减（变负）、myfree 被
                 * 二次调用。那之后无论报什么错，都不能再算分配器的账。 */
                v[i] = NULL;
                continue;
            }

            /* 观察：内容全 0 通常意味着这是一块刚 mmap 出来的新内存，
             * 而不是复用之前释放的块。作为"是否真的在复用"的旁证。 */
            if (round > 0) {
                counted++;
                const unsigned char *q = b->p;
                int all_zero = 1;
                for (int k = 0; k < 32; k++) {
                    if (q[k] != 0) { all_zero = 0; break; }
                }
                if (all_zero) zeroed++;
            }
            v[i] = b;
        }
        for (int i = 0; i < R; i++) free_blk(v[i], 1);
        printf("    [reuse] 第 %d 轮完成，存活字节 = %lld\n", round + 1,
               (long long)atomic_load(&live_bytes));
    }

    if (counted > 0 && zeroed > counted / 2) {
        NOTE("复用轮次中 %d/%d 次拿到的块内容全 0，"
             "疑似每次都向 vmalloc 要新内存，而没有复用已释放的内存",
             zeroed, counted);
    }
    TK_CHECK(atomic_load(&live_bytes) == 0,
             "复用测试后存活字节数 = %lld，应为 0",
             (long long)atomic_load(&live_bytes));
}

/* --- 要求 6：内存放大（申请量 vs 分配器实际占用的内存） ---
 *
 * 这里的"实际占用"必须是**虚拟内存**，不能用 RSS：
 * RSS 只统计真的被碰过的物理页，分配器多要一大段却不碰，RSS 是不涨的 ——
 * 那样"多占 50 倍"会被测成"放大 1.0x 完美通过"。作业查的是 `size`，是虚拟占用。
 *
 * 分子由 tests/vmtrace.c 拦截 vmalloc/vmfree 统计得到（全程序的调用都会
 * 走到那里，mymalloc.c 不用改）。分母就是本文件一直在记的 live_bytes。
 * 两边都是外部可观测的，不需要知道分配器内部哪一块在用。 */
long  tk_vmem_reserved(void);
long  tk_vmem_total_ever(void);
int   tk_vmem_calls(void);
void  tk_vmem_reset(void);

/* ------------------------------------------------------------------ *
 * 内存放大 ≤ 4x
 *
 * 【为什么要在好几个负载点上扫，而不是测一次】
 *
 * 比值是 分子(分配器持有的虚拟内存) / 分母(存活分配的总请求量)。
 * 分子取决于分配器**怎么向 OS 要内存**，两种模型的行为完全不同：
 *
 *   模型 A · 按需 mmap：每次要一小块。负载涨、分子跟着涨。
 *     一个负载点上测出来的比值，能代表全部。
 *
 *   模型 B · 一次性预留一大段：分子是个**常数**，跟负载无关。
 *     这时候只测一个点就会骗人 —— 预留 64 MiB 的分配器，在 1 MiB
 *     负载下是 64x（该报错），在 64 MiB 负载下是 1.0x（合格）。
 *     只在高负载点测，等于给"预留一大段"开后门。
 *
 * 所以这里按 1/4、1/2、1、2 倍逐级加负载，**每个点都要合格**。
 * 模型 B 的正确做法是"随负载增长按需扩段"（比如不够了再要一块、
 * 或者段大小按几何级数涨），那样每个点都能过；如果一上来就死咬
 * 一大段不放，小负载点上必然露馅。
 *
 * 【分母从哪来】
 * 是"调用方当前还活着的分配的总请求字节" —— 测试自己记账就有
 * （live_bytes 那套）。分子由 tests/vmtrace.c 拦截 vmalloc/vmfree
 * 得到，全程序的调用都会走到那里，mymalloc.c 不用改。两边都是外部
 * 可观测的，不需要知道分配器内部哪一块在用。
 *
 * 【预热和 baseline 是干什么的】
 * M5.md 3.2 节说"初始阶段使用的一些合理内存不计入此项"。所以先跑一轮
 * 分配/释放，让分配器把该建的表、该留的段建好，然后以"预热之后还握着
 * 多少"为起点。注意 baseline 是**减掉的常数**，不是免检额度：后面每次
 * 增长都要自己挣回自己的分母。
 * ------------------------------------------------------------------ */

UnitTest(memory_amplification) {
    banner("memory_amplification");
    model_reset_all();

    enum { SZ = 100, MAXBLK = 20000 };
    /* 每级负载的增量。20000 块 x 100B = 2,000,000 B 峰值存活 */
    static blk_t *v[MAXBLK];
    int nv = 0;

    /* 预热：让分配器把启动开销花完，不计入放大率 */
    tk_vmem_reset();
    for (int i = 0; i < 2000; i++) {
        void *p = mymalloc(SZ);
        if (p) myfree(p);
    }
    long baseline = tk_vmem_reserved();

    /* 分母太小时比值没有意义（才分了两块就比，什么分配器都是天文数字），
     * 所以低于这个量不判，只提示。这就是 M5.md 说的"负载上来再判"。 */
    const size_t MIN_MEANINGFUL = 256 * 1024;

    int peak_load_ok = 0;      /* 至少有一个负载点够大到可以判定 */
    double worst = 0.0;        /* 可判定点里最差的那个比值 */
    size_t worst_req = 0;
    int nbad = 0;

    printf("    [amplify] 预热后分配器持有 %ld B（后续按增量判定）\n",
           baseline);

    for (int step = 0; step < 4; step++) {
        /* 在上级基础上再加 5000 块 -> 负载点 0.5/1.0/1.5/2.0 MB */
        for (int i = 0; i < 5000 && nv < MAXBLK; i++) {
            int full = 0;
            blk_t *b = alloc_checked(SZ, &full);
            if (b) v[nv++] = b;
        }

        long   reserved  = tk_vmem_reserved();
        size_t requested = (size_t)nv * SZ;
        long   delta     = reserved - baseline;
        double ratio     = (double)delta / (double)requested;

        printf("    [amplify] 负载 %7zu B -> 分配器多持有 %10ld B，"
               "放大 %5.2fx   (%d 块, vmalloc 累计 %d 次)\n",
               requested, delta, ratio, nv, tk_vmem_calls());

        if (requested < MIN_MEANINGFUL) continue;

        peak_load_ok++;
        if (ratio > worst) { worst = ratio; worst_req = requested; }
        if (ratio > 4.0) nbad++;
    }

    if (!peak_load_ok) {
        NOTE("存活量在各级负载下都低于 %zu B，本次不判放大率",
             MIN_MEANINGFUL);
    } else if (nbad > 0) {
        TK_CHECK(0,
                 "内存放大超过 4x 上限：%d/%d 个负载点不合格，"
                 "最差 %.2fx（存活 %zu B 时分配器多持有 %zu B）",
                 nbad, peak_load_ok, worst, worst_req,
                 (size_t)(worst * (double)worst_req));
    } else {
        printf("    [amplify] %d 个负载点全部合格，最差 %.2fx（上限 4x）\n",
               peak_load_ok, worst);
    }

    /* 全部还回去之后，分配器不该再握着这段内存不放 —— 这既是放大率的一部分，
     * 也是"内存能否被系统回收"的直接证据。
     *
     * 注意这条**只在按需归还的模型下成立**。如果分配器选择"段留着、
     * 下次复用"，那是合法设计，不应该判失败，所以这里只观察不判定。 */
    for (int i = 0; i < nv; i++) free_blk(v[i], 0);

    TK_CHECK(atomic_load(&live_bytes) == 0,
             "放大测试后存活字节数 = %lld，应为 0",
             (long long)atomic_load(&live_bytes));

    printf("    [amplify] 全部释放后，分配器仍持有 %ld B"
           "（累计曾申请 %ld B）—— 保留待复用是合法策略，仅作观察\n",
           tk_vmem_reserved(), tk_vmem_total_ever());
}

/* --- 要求 1：多处理器可扩展性（只看趋势，判定宽松） --- */
#define BENCH_ROUNDS 100000
#define BENCH_THREADS 4

static void *bench_worker(void *arg) {
    (void)arg;
    uint64_t seed = 0xABCDEF;
    for (int i = 0; i < BENCH_ROUNDS; i++) {
        size_t size = 8 + (size_t)(rng_next(&seed) % 128);
        void *p = mymalloc(size);
        if (!p) continue;
        *(volatile char *)p = 1;
        myfree(p);
    }
    return NULL;
}

static double bench_run(int nthreads) {
    pthread_t t[BENCH_THREADS];
    double t0 = now_sec();
    for (int i = 0; i < nthreads; i++) {
        pthread_create(&t[i], NULL, bench_worker, NULL);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(t[i], NULL);
    return now_sec() - t0;
}

UnitTest(scalability_trend) {
    banner("scalability_trend");
    bench_run(1);                       /* 预热，排除冷启动 */

    double t1 = bench_run(1);
    double tn = bench_run(BENCH_THREADS);

    double tput1 = BENCH_ROUNDS / t1;
    double tputn = (double)BENCH_ROUNDS * BENCH_THREADS / tn;
    double speedup = tputn / tput1;

    printf("    [scale] 1 线程 %.1f 万次/秒，%d 线程 %.1f 万次/秒，加速比 %.2fx\n",
           tput1 / 1e4, BENCH_THREADS, tputn / 1e4, speedup);

    if (speedup < 1.0) {
        NOTE("多线程吞吐低于单线程（加速比 %.2fx），疑似被全局锁串行化；"
             "作业要求关注可扩展性", speedup);
    }
}
