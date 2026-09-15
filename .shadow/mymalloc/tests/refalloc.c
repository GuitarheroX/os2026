/*
 * tests/refalloc.c —— 参考分配器（测试框架的自检用，不参与提交）
 *
 * 【解决什么问题】
 *
 * 我们有 12 个用例，其中若干在真实分配器上失败。问题是：**这些失败
 * 是分配器的真 bug，还是测试框架自己在误报？**
 *
 * 光看失败信息没法回答 —— 除非有一个"必然正确"的分配器作为对照组。
 * 这个文件就是那个对照组：一个教科书式的 buddy 分配器，逻辑简单到
 * 可以逐行手动验算。然后用**完全相同的 12 个用例**去跑它：
 *
 *     参考分配器全过  ->  框架没有误报，真实分配器上的失败都是真 bug
 *     参考分配器也挂  ->  框架有问题，失败信息不可信
 *
 * 这就把"测试可信不可信"从一个主观判断变成了一个可复现的实验。
 *
 * 【为什么必须是"另一个实现"而不是"直接调 libc malloc"】
 *
 * 用 libc malloc 更省事，但要包一层加头部，而包装函数在构造函数阶段
 * 就被调用（testkit 在 main 之前注册用例），那时动态链接器还没就绪，
 * dlsym 会死锁。自己管一段静态内存没有这个问题。
 *
 * 【它到底是不是对的】
 *
 * 它不是"另一个可能有 bug 的实现"，而是 buddy 算法最朴素的展开：
 *   - 空闲块按 order 挂在 bucket[order] 上，bucket[i] 的所有块大小都是 2^i
 *   - 分配：从目标 order 往上找第一个非空 bucket，取块后逐级对半劈开，
 *           每一级的一半留在对应的 bucket 里
 *   - 释放：order 处看 buddy 是否空闲，空闲则合并、order++ 继续
 *
 * 关键不变量（也是真实分配器缺的那条）：
 *   **bucket 里的每一块，地址都是它自身大小的倍数。**
 * 维护方式见 pool_init 的注释 —— 根块从 2^K 对齐的地址开始，
 * 每次对半劈开产生的两半天然满足这条。
 *
 * 【怎么跑】
 *
 *     make approx        # 或下面 Makefile 里对应目标的名字
 *
 * 它把自己的 mymalloc/myfree 编译成**强符号**，把 mymalloc.c 里那对
 * 弱符号压掉。所以跑的是参考实现，而测试框架一个字都不用改。
 */

#include <mymalloc.h>
#include <stdint.h>

/* 来自 start.c：向 OS 要一段连续虚拟内存 */
void *vmalloc(void *addr, size_t length);

/* ------------------------------------------------------------------ *
 * 池子：一段静态内存 + 按 order 分桶的空闲链表
 * ------------------------------------------------------------------ */

/* 和 mymalloc.c 里的段模型保持一致：64 MiB 连续内存，大小固定。
 * 参考实现同样不"按需增长" —— 那样才能和真实实现做同条件对照。 */
#define REF_ORDER   26                    /* 池子 = 2^26 = 64 MiB */
#define REF_POOL    ((size_t)1 << REF_ORDER)
#define REF_NBUCKET (REF_ORDER + 1)       /* bucket[0..26] */

/* 池子的实际起点，由 ref_pool_init 在运行时对齐后设定。
 *
 * 【为什么不能用 `_Alignas(2^27) static char pool[...]`】
 *
 * 试过了，链接器不吃这一套：
 *
 *     ld: warning: reducing alignment of section __DATA,__bss from 0x8000000
 *     to 0x4000 because it exceeds segment maximum alignment
 *
 * 它把对齐**悄悄降到 4 KiB**，池子起点于是不是 2^26 的倍数，根块地址
 * 就不是自身大小的倍数 —— buddy 不变量从第一块起就破了。而且这只是一条
 * warning，不看不出来。
 *
 * 这和 mymalloc.c 里 heap_init 踩的是同一个坑：**不能假设拿到的地址
 * 满足比页更大的对齐**，必须自己对齐。所以这里改成运行时对齐。
 *
 * 对齐到 REF_POOL 的倍数即可，**不要求 2 × REF_POOL**。
 *
 * 为什么这里能比 heap_init 松一档：2 × REF_POOL 对齐是为了让"段内最大块
 * 的 buddy 落在段内"，而这一条只有在**最大块被释放并尝试合并**时才需要。
 * 这个池子的根块一被取出来就立刻对半劈开，永远不会以 2^26 的尺寸回到
 * 桶里；就算真的回去，myfree 里的边界检查也会挡住那一步合并。所以
 * 2^26 对齐足够，而 2 × REF_POOL 的对齐**单次 mmap 不一定给得出来** ——
 * 踩过：raw=0x108bd4000 时向上取整到 0x110000000，但映射只到 0x110bd4000，
 * 池子实际只有 11.7 MiB 可用，写到一半就 EXC_BAD_ACCESS。 */
static char *ref_pool;
static char *ref_pool_raw;

static block_t *bucket[REF_NBUCKET];
static spinlock_t ref_lock;

/* 把 ref_pool 里的一块挂到对应 order 的桶上（头插）。
 * 用块头的 size 反推 order —— 这里 size 恒为 2 的幂，所以 log2 是精确的。 */
static int ref_order_of(size_t size) {
    int o = 0;
    while (((size_t)1 << o) < size) o++;
    return o;
}

static void ref_push(block_t *b) {
    int o = ref_order_of(b->size);
    b->free = 1;
    b->prev = NULL;
    b->next = bucket[o];
    if (bucket[o]) bucket[o]->prev = b;
    bucket[o] = b;
}

static void ref_pop(block_t *b) {
    int o = ref_order_of(b->size);
    if (b->prev) b->prev->next = b->next;
    else         bucket[o] = b->next;
    if (b->next) b->next->prev = b->prev;
    b->next = b->prev = NULL;
    b->free = 0;
}

/* 申请池子：**只做一次**，成功后 ref_pool 非 NULL。
 *
 * 用 vmalloc 而不是 libc malloc：malloc 不保证大块对齐，而且这里要的
 * 本来就是"向 OS 要一段连续虚拟内存"这个语义。
 *
 * 【守卫为什么不能用 `bucket[REF_ORDER] != NULL`】
 *
 * 踩过。根块被劈开之后 bucket[REF_ORDER] 就空了，于是这个守卫会**每次都
 * 成立**，每次分配都重新申请一段新池子 —— 已经发出去的指针全部作废，
 * 第二次分配返回的地址甚至不属于同一段内存。
 *
 * 判据必须是"池子有没有申请过"，而不是"最高 order 的桶空不空"
 * （桶空是很正常的状态）。这里用 ref_pool 自身当判据。 */
static void ref_pool_alloc(void) {
    if (ref_pool != NULL) return;          /* 已经申请过了 */

    ref_pool_raw = vmalloc(NULL, REF_POOL * 2);
    if (ref_pool_raw == NULL) return;      /* 申请失败，调用方会拿到 NULL */

    /* 向上取整到 REF_POOL 的倍数。向上取整最坏浪费 REF_POOL-1 字节，
     * 而上面多要了一整份（2 × REF_POOL），所以池子必然完整落在映射内。 */
    const uintptr_t mask = (uintptr_t)REF_POOL - 1;
    ref_pool = (char *)(((uintptr_t)ref_pool_raw + mask) & ~mask);
}

/* 把整段池子作为一个 2^REF_ORDER 的根块，挂进最高 order 的桶。
 *
 * 这一步要**单独**判"根块是否已经建过"，判据和上面不同：用 ref_root_built
 * 这个显式标志。因为根块一旦被劈开就再也回不到 bucket[REF_ORDER] 里
 * （除非释放到完全合并），拿桶空不空当判据会重复建根块、把整池覆盖掉。 */
static int ref_root_built;

static void ref_pool_init(void) {
    ref_pool_alloc();
    if (ref_pool == NULL || ref_root_built) return;

    block_t *root = (block_t *)ref_pool;
    root->size = REF_POOL;
    root->next = root->prev = NULL;
    root->free = 1;
    bucket[REF_ORDER] = root;
    ref_root_built = 1;
}

/* ------------------------------------------------------------------ *
 * 被测试的接口
 * ------------------------------------------------------------------ */

void *mymalloc(size_t size) {
    spin_lock(&ref_lock);
    ref_pool_init();

    /* 头部本身要占地方。请求 0 字节也得给一个能安全读写的块。
     *
     * 【溢出必须先判】`size + sizeof(block_t)` 会回绕：mymalloc(SIZE_MAX)
     * 加 32 变成 31，够不着下面的最小块下限就被抬到 64，于是这个"比整个池子
     * 还大"的请求会**成功返回一个小块** —— 而要求 5 说这种情况下必须返回
     * NULL。踩过：套件的 sizes_and_alignment 就是拿 SIZE_MAX 试的。 */
    if (size > SIZE_MAX - sizeof(block_t)) {
        spin_unlock(&ref_lock);
        return NULL;
    }
    size_t need = size + sizeof(block_t);
    if (need < sizeof(block_t) * 2) need = sizeof(block_t) * 2;  /* 最小块 64B */

    /* 目标 order：不小于 need 的最小 2 的幂 */
    int want = 0;
    while (((size_t)1 << want) < need) want++;

    if (want > REF_ORDER) {
        spin_unlock(&ref_lock);
        return NULL;                       /* 比整个池子还大，做不到 */
    }

    /* 找一个非空的桶。没有就往上找 —— 大的可以劈开，小的不行。 */
    int o = want;
    while (o <= REF_ORDER && bucket[o] == NULL) o++;
    if (o > REF_ORDER) {
        spin_unlock(&ref_lock);
        return NULL;                       /* 池子用尽 */
    }

    block_t *b = bucket[o];
    ref_pop(b);

    /* 逐级对半劈开，直到尺寸正好是 want。
     *
     * 每一级的另一半地址是 b + half（不是 XOR —— 这里只是算地址，
     * 合并时才用 XOR 找伙伴）。因为 b 的地址是 (2^o) 的倍数，
     * 加 half = 2^(o-1) 之后得到的那一半，地址也是 2^(o-1) 的倍数 ——
     * 这正是 buddy 不变量得以保持的原因。 */
    while (o > want) {
        o--;
        size_t half = (size_t)1 << o;
        /* 注意必须转成 char* 再加 —— block_t* 做加法会按 sizeof(block_t)
         * 缩放，加上 half 等于加了 half * 32 字节。踩过。 */
        block_t *buddy = (block_t *)((char *)b + half);
        buddy->size = half;
        buddy->next = buddy->prev = NULL;
        b->size = half;
        ref_push(buddy);                   /* 另一半留在高 order 桶里 */
    }

    b->free = 0;
    spin_unlock(&ref_lock);
    return (void *)(b + 1);
}

void myfree(void *ptr) {
    if (ptr == NULL) return;

    spin_lock(&ref_lock);
    block_t *b = (block_t *)ptr - 1;

    /* 越界保护：只有落在池内、且 size 合法（2 的幂）的块才处理。
     * 这不影响对照实验的可信度 —— 合法的调用永远走不到这两个分支，
     * 它只是让参考实现在被喂进垃圾指针时不会把池子外的内存踩了。 */
    if ((char *)b < ref_pool || (char *)b >= ref_pool + REF_POOL) {
        spin_unlock(&ref_lock);
        return;
    }
    size_t s = b->size;
    if (s < sizeof(block_t) * 2 || s > REF_POOL || (s & (s - 1)) != 0) {
        spin_unlock(&ref_lock);
        return;
    }

    /* 逐级尝试与 buddy 合并 */
    while (s < REF_POOL) {
        uintptr_t buddy_addr = (uintptr_t)b ^ (uintptr_t)s;
        /* buddy 必须在池内 —— 池内最大块的 buddy 会跑到池外，必须挡住。
         * 这就是"向下取整会崩"那个坑的同一个数学。 */
        if ((char *)buddy_addr < ref_pool ||
            (char *)buddy_addr >= ref_pool + REF_POOL) {
            break;
        }
        block_t *buddy = (block_t *)buddy_addr;
        if (!buddy->free || buddy->size != s) break;   /* 伙伴在用，合不了 */

        ref_pop(buddy);
        /* 低位地址那个当头 */
        if ((uintptr_t)buddy < (uintptr_t)b) b = buddy;
        s <<= 1;
        b->size = s;
    }

    ref_push(b);
    spin_unlock(&ref_lock);
}

/* mymalloc.c 里有这个符号，链接和跳都要。参考构建下没有意义。 */
void dump_free_lists(void) {
}
