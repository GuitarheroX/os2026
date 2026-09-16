# mymalloc 代码审查记录

> 审查时间：2026-09-15
> 审查对象：`mymalloc.c` / `mymalloc.h` / `start.c` / `Makefile`
> 平台：macOS (Apple Silicon, arm64, 页大小 16384)
> 结论摘要：**当前实现有 1 个致命 UB（必崩）+ 5 个正确性缺陷 + 2 个性能硬伤 + 4 个工程问题。**

---

## 〇、最重要的结论：不是 macOS 的锅

「同样的代码 Linux 能跑、macOS 崩」——这是**链接器彩票**，不是平台差异。

同一台 macOS 上编译的两个二进制，一个正常一个崩溃，这本身就排除了操作系统的原因：

| 二进制 | `free_lists` 结束地址 | 后面 8 字节是什么 | 结果 |
|---|---|---|---|
| `./mymalloc`（trivial demo） | `0x1000080F0` | `.bss` 填充 = 0 | 读到 NULL，正常退出 ✅ |
| 诊断程序 | `0x100c0c0f0` | `big_lock` = 1 | 野指针，SIGSEGV ❌ |

Linux 上能跑，几乎可以肯定是那边 `free_lists` 后面恰好是 0。换个优化级别、加一个全局变量、
或换到 OJ 的编译环境，随时会崩。**修它只能靠修代码。**

---

## 一、正确性缺陷（必须修）

### 1. `free_lists[N]` 越界读 —— 致命，macOS 上必崩

**位置**：`mymalloc.c:88-90`

```c
block_t *free_lists[N];          // N = 30，合法下标 0..29
...
for (; i <= N; i++) {            // i 会取到 30
    curr = free_lists[i];        // ← 越界读 free_lists[30]
```

**证据**：AddressSanitizer 在两个二进制里都报同一个错（**包括那个「跑通」的**）

```
ERROR: AddressSanitizer: global-buffer-overflow ... READ of size 8
    #0 mymalloc mymalloc.c:90
0x...c3f0 is located 0 bytes after global variable 'free_lists' ... of size 240
```

**崩溃机理**（用 probe 程序实测）：

```
free_lists 数组占 240 字节 (30 × 8)
&free_lists[29]  = 0x100c0c0e8
&free_lists[30]  = 0x100c0c0f0   <-- 越界
&big_lock        = 0x100c0c0f0   <-- 同一个地址！
持锁时 free_lists[30] 读到的值 = 0x1  ->  非 NULL，会被当成 block_t* 解引用！
```

`free_lists[30]` 正好落在紧邻的 `big_lock` 上（4 字节 `status` + 4 字节 `malloc_count`）。
`mymalloc` 此刻正持有自旋锁，所以 `status == LOCKED == 1`，拼出来就是 `0x1`：

```c
curr = (block_t *)0x1;      // 非 NULL，于是 if (curr != NULL) 成立
remove_blk(curr);           // → my_log2(curr->size) → 解引用地址 0x1 → SIGSEGV
```

**而且是必崩**，因为锁一定是 LOCKED 状态，不是概率问题。

**修法方向**：循环边界改成 `i < N`。同时注意 `insert_blk` 里若 `my_log2(size)` 返回 30
（即 `size == 2^30`）也会越界**写**，见第 6 条。

---

### 2. `vmalloc` 返回值未检查 NULL

**位置**：`mymalloc.c:101-104`

```c
void *raw = vmalloc(NULL, power(2, exp));
curr = (block_t *) raw;
curr -> size = power(2, exp);   // ← raw 可能是 NULL
```

`start.c` 在 `mmap` 失败（返回 `MAP_FAILED`）时会返回 `NULL`，此时直接空指针解引用。
作业明确要求「无法满足请求时返回 NULL」，而不是崩溃。

**修法方向**：`if (raw == NULL) { spin_unlock(&big_lock); return NULL; }`。

---

### 3. buddy 合并假设地址按块大小对齐

**位置**：`myfree`，`mymalloc.c:127`

```c
block_t *buddy = (block_t *)((uintptr_t)block ^ ((uintptr_t)1 << order));
```

buddy 算法的经典写法，但它有个**前提**：块的地址必须是 `2^order` 的倍数。
`vmalloc` 只保证**页对齐**，不保证更大的对齐：

```
macOS page size = 16384
vmalloc len=2^15 = 32768 -> 0x10490c000  aligned to len? NO
vmalloc len=2^16 = 65536 -> 0x10490c000  aligned to len? NO
```

块大于页大小时，buddy 地址会算错 → 读到未映射内存，或 `remove_blk` 一个野指针。

**注意**：这个假设**在 Linux 上同样不成立**（4K 页下 32K 的块也不保证 32K 对齐），
只是目前没测到那么大的块。所以这仍然不是平台问题。

**修法方向**：要么让分配器自己保证对齐（例如按最大块大小对齐切分堆区），
要么放弃 XOR 算法、改用记录 buddy 指针的方式。

---

### 4. `myfree` 读 buddy 的 header，而那块内存可能从未被初始化

**位置**：`mymalloc.c:128-131`

```c
if (buddy->free != 1 || buddy->size != block->size) {
    break;
}
```

`buddy` 指向的内存可能：
- 在 `vmalloc` 拿到的页内但从未被使用过 → 读到**未初始化的垃圾值**；
- 已在页边界之外 → **未映射，直接段错误**。

当前 trivial demo 能过，纯粹是因为 `mmap` 给的是整页 4096 字节，
buddy 落在页内且内容恰好是 0。垃圾值凑巧满足 `free == 1 && size 匹配` 时，
就会 `remove_blk` 一个野指针。

**修法方向**：需要一个能判断「这块内存是否属于本分配器管理范围」的方式，
比如维护堆区边界，或给每个块加魔数（magic）校验。

---

### 5. `my_log2(0)` 无限递归；且只对 2 的幂正确

**位置**：`mymalloc.c:20-25`

```c
int my_log2(int n) {
    if (n == 1) return 0;
    return 1 + my_log2(n / 2);   // n == 0 时永远递归不到 1
}
```

- `my_log2(0)` → 栈溢出。
- `my_log2` 返回的是 `floor(log2(n))`，对非 2 的幂会**静默算错**：
  `my_log2(3) == 1`，但 `3` 不属于任何一个 order 链表。

目前块大小恒为 2 的幂所以恰好成立，但 `insert_blk` / `remove_blk` 都靠它算下标，
一旦某处 size 不是 2 的幂就会索引错链表，且不会有任何报错。

**修法方向**：改成用 `__builtin_ctz` 或位运算直接算 order，并在 debug 断言里校验
`size` 是 2 的幂。

---

### 6. `insert_blk` 在 order 30 时会越界写

**位置**：`mymalloc.c:27-38`（`insert_blk`）+ `mymalloc.c:136`

`myfree` 的合并循环是 `while (order < N)`，即 `order` 最大到 29，
合并后 `order` 变成 30、`size` 变成 `2^30`，然后 `insert_blk` 调用
`my_log2(2^30) == 30` → **写** `free_lists[30]`，越界。

这是第 1 条的孪生问题（那条是读，这条是写）。越界写更危险，会直接踩坏相邻全局变量。

**修法方向**：`insert_blk` / `remove_blk` 内部对 `n` 做边界检查，
并且明确「最大可管理块」与数组长度 `N` 的关系（当前 `2^N` 这个上界和 `N` 个槽位是矛盾的）。

---

### 7. `mymalloc.h` 没有 include guard

重复包含会直接编译报错（`typedef redefinition` / `redefinition of 'spin_lock'`）。
只要有一个 `.c` 同时 `#include <mymalloc.h>` 和 `#include "mymalloc.c"`（调试时很常见）就会炸。

**修法方向**：加 `#pragma once` 或 `#ifndef MYMALLOC_H` 包裹。

---

## 二、性能问题（Hard Test 会挂）

### 8. 可扩展性基本为零

- **一把全局大锁**保护所有空闲链表（`mymalloc.c:70` / `mymalloc.c:121`）。
  所有处理器上的分配/释放完全串行化。
- 分配时还要**线性扫描 order** 找可用块（`mymalloc.c:88`）。
- 没有 fast path，没有线程本地缓存，没有任何 per-CPU 结构。

作业原文点名批评这种做法：

> 性能过低（如全局锁保护空闲链表并遍历查找）可能导致 hard test failure。

这是 hard test 扣分的主因，也是最需要重构的部分。

---

### 9. 内存放大风险

`sizeof(block_t) == 32`（实测）。每个块的头开销是 32 字节：

| 请求 | 实际占用块 | 放大倍数 |
|---|---|---|
| `mymalloc(0)` | 32 B（order 5） | 无穷 |
| `mymalloc(1)` | 64 B（order 6） | 64× |
| `mymalloc(33)` | 128 B（order 7） | 3.9× |

作业有硬性规则：

> 当实际使用的内存超过申请内存的 4 倍时，Online Judge 将会判定为错误；
> 初始阶段的合理内存不计入。

`mymalloc(33)` 的 3.9× 已经贴着红线。像 `tests-trivial.c` 里
「4 线程 × 100000 次 `mymalloc(0)` 且从不 free」这种负载，
会为 0 字节的请求吃掉 400000 × 32 B = **12.8 MB**。

---

## 三、工程与测试问题

### 10. 测试几乎为空

- `tests/main.c`：手写 demo，靠 `printf` 肉眼看输出，**不是** testkit 用例，
  没有断言、没有并发、没有边界。
- `tests-trivial.c`：框架给的示例，只覆盖了最平凡的情况。

缺失的测试类型：并发压力测试、use-after-free / double-free 检查、
碎片测试、大块（> 页大小）测试、边界（size = 0 / 超大）测试。

作业指南原文：

> 单测不够，需要检查常见内存错误：use-after-free、double-allocation 等；
> 注意日志的「probe effect」。

### 11. freestanding 分支没有实现

`start.c` 在 `FREESTANDING` 下 `vmalloc` 直接 `return NULL`，
作业提示的「划定一块 `static char[]` 自己实现简单 vmalloc/vmfree」没有做。

OJ 会覆盖 `start.c`，所以**不影响提交**，但意味着**本地完全无法验证 freestanding 环境**——
而 OJ 恰恰就是在 freestanding 下跑的。

### 12. `make check` 在 macOS 上失败

```
Undefined symbols for architecture arm64:
  "start", referenced from: <initial-undefines>
```

这是 Makefile 的平台差异（macOS 符号名带下划线、入口点处理与 Linux 不同），
在 Linux / OJ 上正常。但如果你只在 Mac 上开发，这个自检就一直处于失效状态，
等于少了一道防线。

### 13. `malloc_count` 调试变量未清理

`mymalloc.c:8` 的 `malloc_count` 是调试用的全局变量（代码注释里也说了可以删），
`tests-trivial.c` 里依赖它的用例框架也标注了可以删。

除了冗余，它还会影响 `.bss` 布局——第 1 条的崩溃位置就和全局变量排布直接相关。

---

## 四、已符合作业要求的部分（做得对的地方）

- ✅ `mymalloc` / `myfree` 接口签名正确
- ✅ 使用框架的 `spinlock_t` 自旋锁，未调用 pthreads，函数体内未使用任何库函数
- ✅ **8 字节对齐达标**：返回 `block + 1`，`block` 至少页对齐，+32 后仍是 8 的倍数
- ✅ **失败返回 NULL**：实测 `mymalloc(2^40)` 返回 `0x0`
- ✅ 通过 `vmalloc` / `vmfree` 申请大块内存
- ✅ 分裂（`divide`）与合并（buddy coalescing）都已实现，且 demo 中合并正确工作

---

## 五、修复优先级建议

| 优先级 | 项目 | 说明 |
|---|---|---|
| P0 | #1 越界读 | 一行改动，改完至少不崩了 |
| P0 | #2 NULL 检查 | 一行改动，作业硬性要求 |
| P0 | #6 越界写 | 与 #1 同源 |
| P1 | #3 对齐假设<br>#4 未初始化 buddy | 大块分配的正确性，需要设计上的处理 |
| P1 | #5 my_log2 | 健壮性 |
| P2 | #7 include guard<br>#13 清理调试变量 | 顺手 |
| **P0** | **#8 可扩展性** | **拿 hard test 分的关键，需要重构为 fast/slow path** |
| P1 | #9 内存放大 | 需要更细的 size class |
| P1 | #10 测试 | 补并发与边界测试 |
| P3 | #11 freestanding<br>#12 make check | 本地验证能力 |

---

## 附：本次审查用到的复现命令

```bash
# 1. 用 ASan 定位越界（对任意测试都成立）
cc -fsanitize=address -g -O1 -I. tests/main.c start.c mymalloc.c -o /tmp/asan_trivial
ASAN_OPTIONS=detect_leaks=0 /tmp/asan_trivial

# 2. 看全局变量布局，确认 free_lists[30] 落在了谁头上
nm -n ./mymalloc | grep -E "free_lists|big_lock|malloc_count"

# 3. 看页大小
python3 -c "import os; print(os.sysconf('SC_PAGE_SIZE'))"

# 4. 验证 vmalloc 的大块对齐
#    vmalloc(32768) 返回的地址不满足 32K 对齐 → buddy 算法前提被破坏
```
