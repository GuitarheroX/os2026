/*
 * tests/vmtrace.c —— 观察分配器向 OS 要了多少内存（测试专用，不参与提交）
 *
 * 【解决什么问题】
 *
 * M5.md 3.2 节要求"当实际使用的内存超过申请内存的 4 倍时判定为错误"。
 * 这里有两个坑：
 *
 *   坑 1：用 RSS 测是错的。RSS 只统计**真的被碰过的物理页**。如果分配器
 *         一次性 mmap 了 64 MiB 却只用了 1.2 MiB，RSS 只涨 1.2 MiB ——
 *         测出来"放大 1.0x 完美通过"，而虚拟地址空间其实多占了 50 倍。
 *         作业查的是虚拟占用（`size` 命令），不是常驻集。
 *
 *   坑 2：不需要知道"分配器内部哪一块在用"。放大率的分母是
 *         "所有存活分配的总请求量" —— 这是**调用方**知道的，测试自己
 *         记账就有（见 stress.c 里的 live_bytes）。分子是"分配器向 OS
 *         要来的总内存" —— 来源只有 vmalloc/vmfree，拦下来就能算。
 *
 *         两边都是外部可观测的，不用窥探分配器内部。
 *
 * 【怎么拦住的】
 *
 * macOS 的 ld 不支持 GNU 的 --wrap，所以不能用链接器方案。这里改成：
 * 把框架的 start.c **原样包含**进来，包含之前先把 vmalloc/vmfree 两个
 * 名字改掉，于是在本编译单元里它们变成 tk_real_vmalloc/tk_real_vmfree，
 * 然后我们提供自己的 vmalloc/vmfree 去调它们。
 *
 * 这样做的结果是：整个程序里所有对 vmalloc/vmfree 的调用（包括
 * mymalloc.c 里的）都会走到这里的记账代码，而 start.c / mymalloc.c
 * 一行都不用改。
 *
 * 注意：start.c 有 `#ifndef FREESTANDING` 之类的包含保护吗？没有 —— 但
 * 它本身没有 include guard，所以**只能被包含一次**。链接测试时不要
 * 再把 start.c 单独加进去，否则符号重复。见 Makefile 里的 SRCS。
 */

#include <stdatomic.h>
#include <stddef.h>
#include <unistd.h>

/* ---- 把 start.c 里的两个函数改名后再包含进来 ---- */

#define vmalloc tk_real_vmalloc
#define vmfree  tk_real_vmfree
#include "../start.c"          /* <- 顺带把 mymalloc.h 也带进来了 */
#undef vmalloc
#undef vmfree

/* ---- 记账 ---- */

/* 当前仍然 "被分配器握在手里" 的虚拟内存字节数。
 * 分配器一次性要一段大内存，这里就是那一段的大小；要多少段就累加多少。 */
static atomic_llong vmem_reserved;

/* 累计值，用来观察总量（一次性要 vs 分批要，趋势完全不同） */
static atomic_llong vmem_total_ever;
static atomic_int   vmem_calls;

long tk_vmem_reserved(void) {
    return (long)atomic_load(&vmem_reserved);
}

long tk_vmem_total_ever(void) {
    return (long)atomic_load(&vmem_total_ever);
}

int tk_vmem_calls(void) {
    return atomic_load(&vmem_calls);
}

void tk_vmem_reset(void) {
    atomic_store(&vmem_reserved, 0);
    atomic_store(&vmem_total_ever, 0);
    atomic_store(&vmem_calls, 0);
}

/* 页大小必须在这里处理，不能直接用 length 记账。
 *
 * mmap 最少给一页，但分配器完全可能写 vmalloc(NULL, 256) —— 于是"请求
 * 256 字节"实际占掉的是整整一页。**作业查的是虚拟地址占用**，所以这时候
 * 该记的是 16384，不是 256。
 *
 * 这个区别不是小数点后的事：旧实现每次分配都要 256 字节，10000 块就是
 * 10000 次 vmalloc，按请求长度算放大率是 2.56x（看着勉强能过 4x），
 * 按实际页占用算是 163x。差两个数量级，结论完全相反。
 *
 * sysconf 在 freestanding 里没有，所以退回一个保守的 4096 —— 反正
 * freestanding 分支下 vmalloc 根本不会成功，记多少都不影响。
 */
static size_t page_round(size_t length) {
#ifdef FREESTANDING
    const size_t pg = 4096;
#else
    long v = sysconf(_SC_PAGESIZE);
    const size_t pg = (v > 0) ? (size_t)v : 4096;
#endif
    return (length + pg - 1) / pg * pg;
}

/* ---- 被拦截的 vmalloc / vmfree ---- */

void *vmalloc(void *addr, size_t length) {
    void *p = tk_real_vmalloc(addr, length);

    /* 只有真的要到了才记账 —— 失败时分配器没拿到内存，不该算进放大率。
     * 这一点很重要：如果这里无条件累加，那么"分配器疯狂申请又全失败"
     * 会看起来像内存放大。 */
    if (p != NULL) {
        size_t actual = page_round(length);
        atomic_fetch_add(&vmem_reserved, (long long)actual);
        atomic_fetch_add(&vmem_total_ever, (long long)actual);
        atomic_fetch_add(&vmem_calls, 1);
    }
    return p;
}

void vmfree(void *addr, size_t length) {
    atomic_fetch_sub(&vmem_reserved, (long long)page_round(length));
    tk_real_vmfree(addr, length);
}
