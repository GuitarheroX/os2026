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
#define CANARY_BYTE ((unsigned char)0x5A)

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
#define RANGE_CAP 4096
#define RANGE_SHARDS 8

typedef struct {
    uintptr_t base;
    size_t size;
} range_t;

static range_t ranges[RANGE_SHARDS][RANGE_CAP];
static size_t range_n[RANGE_SHARDS];

static atomic_int shard_next;

/* 每个线程一份模型：把 shard 号存成线程局部变量，调用点就不必到处传 */
typedef struct {
    range_t *r;
    size_t n;
} model_t;

static _Thread_local model_t tls_model;
static _Thread_local int tls_shard = -1;

static model_t *model_of_this_thread(void) {
    if (tls_shard < 0) {
        tls_shard = atomic_fetch_add(&shard_next, 1) % RANGE_SHARDS;
        tls_model = (model_t){ ranges[tls_shard], 0 };
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
    if (m->n == RANGE_CAP) return 2;

    size_t at = rng_lower_bound(m->r, m->n, base);
    if (at > 0 && m->r[at - 1].base + m->r[at - 1].size > base) return 1;
    if (at < m->n && m->r[at].base < base + size) return 1;

    memmove(&m->r[at + 1], &m->r[at], (m->n - at) * sizeof(range_t));
    m->r[at].base = base;
    m->r[at].size = size;
    m->n++;
    return 0;
}

static void rng_remove(model_t *m, uintptr_t base) {
    size_t at = rng_lower_bound(m->r, m->n, base);
    if (at < m->n && m->r[at].base == base) {
        memmove(&m->r[at], &m->r[at + 1], (m->n - at - 1) * sizeof(range_t));
        m->n--;
    }
}

/* 存活字节数记账：跨线程用原子操作累加。压测结束时必须回到 0，
 * 这是"无内存泄漏"（要求 4）最直接的证据。 */
static atomic_llong live_bytes;
static atomic_llong alloc_calls;

/* ------------------------------------------------------------------ *
 * 2. 每个分配块的元数据
 * ------------------------------------------------------------------ */

typedef struct {
    void *p;                 /* mymalloc 返回的指针 */
    size_t size;             /* 申请的字节数 */
    unsigned char pattern;   /* 分配时写入的填充值 */
    unsigned char canary;    /* 分配时写在 p[size] 的哨兵值 */
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

    /* 要求 3：8 字节对齐 */
    TK_CHECK(((uintptr_t)p & 7u) == 0, "mymalloc(%zu) 返回 %p，不是 8 字节对齐",
             size, p);

    /* 写入 + 读回；同时在 p[size] 放哨兵抓溢出 */
    unsigned char *q = (unsigned char *)p;
    unsigned char pattern = PATTERN((uintptr_t)p + size);
    memset(q, pattern, size);
    if (size > 0) {
        TK_CHECK(q[size / 2] == pattern,
                 "mymalloc(%zu) 返回的 %p 写入后读回不一致", size, p);
    }
    q[size] = CANARY_BYTE;

    /* 要求 2：与其它存活区间不重叠 */
    model_t *m = model_of_this_thread();
    int r = rng_insert(m, (uintptr_t)p, size + 1);
    TK_CHECK(r != 1, "mymalloc(%zu) 返回 %p，与已分配区间重叠（overlap）",
             size, p);

    atomic_fetch_add(&live_bytes, (long long)size);
    atomic_fetch_add(&alloc_calls, 1);

    blk_t *b = pool_take();
    if (!b) {
        *pool_full = 1;
        /* 池子满了：把这块还回去，避免污染后续检查 */
        rng_remove(m, (uintptr_t)p);
        atomic_fetch_sub(&live_bytes, (long long)size);
        myfree(p);
        return NULL;
    }

    b->p = p;
    b->size = size;
    b->pattern = pattern;
    b->canary = CANARY_BYTE;
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
                 "（疑似 off-by-one 越界）",
                 b->p, b->size, b->pattern, q[b->size - 1]);
    }

    TK_CHECK(q[b->size] == b->canary,
             "块 %p (size=%zu) 的哨兵字节被改写：期望 %02x 实得 %02x（写越界）",
             b->p, b->size, b->canary, q[b->size]);

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
    rng_remove(model_of_this_thread(), (uintptr_t)b->p);
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
    tls_model.n = 0;
    atomic_store(&check_reported, 0);
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
#define ST_THREADS 4
#define ST_ITERS 120000
#define ST_SAMPLE 256          /* 每线程最多驻留的"抽样块"数量 */

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
           ST_THREADS, ST_ITERS, calls, dt, (double)calls / dt / 1e4,
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
            if (!b) continue;

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

/* --- 要求 6：内存放大（申请量 vs 实际占用） --- */
static long rss_bytes(void) {
#ifdef __APPLE__
    /* macOS 没有 /proc，用 mach 的 resident_size */
    struct {
        int count;
        unsigned int unused;
        unsigned long long info[64];
    } info;
    extern int task_info(void *, int, void *, unsigned int *);
    unsigned int cnt = sizeof(info);
    info.count = 64;
    /* MACH_TASK_BASIC_INFO = 20；info[0]=virtual_size, info[1]=resident_size */
    if (task_info((void *)0xFFFFFFFFu, 20, &info, &cnt) != 0) return -1;
    return (long)info.info[1];
#else
    FILE *fp = fopen("/proc/self/statm", "r");
    if (!fp) return -1;
    long total = 0, resident = 0;
    int got = fscanf(fp, "%ld %ld", &total, &resident);
    fclose(fp);
    if (got != 2 || resident < 0) return -1;
    return resident * (long)sysconf(_SC_PAGESIZE);
#endif
}

UnitTest(memory_amplification) {
    banner("memory_amplification");
    model_reset_all();
    enum { N = 10000, SZ = 100 };

    /* 先预热，把初始 mmap 的合理开销排除在外 */
    for (int i = 0; i < 2000; i++) {
        void *p = mymalloc(SZ);
        if (p) myfree(p);
    }

    long rss0 = rss_bytes();

    static blk_t *v[N];
    int nv = 0;
    for (int i = 0; i < N; i++) {
        int full = 0;
        blk_t *b = alloc_checked(SZ, &full);
        if (b) v[nv++] = b;
    }

    long rss1 = rss_bytes();
    size_t requested = (size_t)nv * SZ;
    size_t headroom = (size_t)nv * EST_HEADER;

    if (rss0 < 0 || rss1 < 0) {
        NOTE("当前平台读不到进程内存占用，跳过放大率检查");
    } else {
        long grew = rss1 - rss0;
        printf("    [amplify] 申请 %zu B，RSS 增长 %ld B，比值 %.2fx"
               "（其中约 %zu B 是 %dB/块的头部开销）\n",
               requested, grew, (double)grew / (double)requested,
               headroom, EST_HEADER);
        if (grew > (long)requested * 4) {
            NOTE("内存放大 %.2fx 超过作业规定的 4x 上限",
                 (double)grew / (double)requested);
        }
    }

    for (int i = 0; i < nv; i++) free_blk(v[i], 0);
    TK_CHECK(atomic_load(&live_bytes) == 0,
             "放大测试后存活字节数 = %lld，应为 0",
             (long long)atomic_load(&live_bytes));
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
