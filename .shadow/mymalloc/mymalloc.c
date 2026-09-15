#include <mymalloc.h>

#define N 30

spinlock_t big_lock;

// We don't need this malloc_count. You can remove it.
long malloc_count;

static block_t *free_lists[N];  // 按块大小分层的空闲链表数组，free_lists[i] 存放大小为 2^i 的空闲块。（全局变量自动初始化为 NULL）
extern spinlock_t big_lock;

int power(int base, int exponent) {
    if (exponent == 0) {
        return 1;
    }
    return base * power(base, exponent - 1);
}

int my_log2(int n) {
    if (n == 1) {
        return 0;
    }
    return 1 + my_log2(n / 2);
}

void insert_blk(block_t *block) {
    int n = my_log2(block->size);
    if (free_lists[n] != NULL) {
        block->next = free_lists[n];
        free_lists[n]->prev = block;
    }
    else {
        block->next = NULL;
    }
    block->prev = NULL;
    free_lists[n] = block;
}

void remove_blk(block_t *block) {
    int n = my_log2(block->size);
    if (block->prev == NULL) {
        free_lists[n] = block->next;
    } else {
        block->prev->next = block->next;
    }
    
    if (block->next != NULL) {
        block->next->prev = block->prev;
    }
}

block_t *divide(size_t size, block_t *block, int n) {
    // 劈开大的 block，将最后合适的小 block 插入到 free_lists 中
    // n 为当前所在 order
    if ((size + sizeof(block_t)) * 2 > block -> size) {
        return block;
    }
    block_t *new_block = (block_t *)((char *)block + block->size / 2);
    new_block->size = block->size / 2;
    new_block->free = 1;
    // new_block 插入 free_lists[n-1]
    insert_blk(new_block);

    block -> size /= 2;
    return divide(size, block, n - 1);
}

void *mymalloc(size_t size) {
    spin_lock(&big_lock);
    
    void *memory = NULL;
    if (size + sizeof(block_t) > power(2, N)) {
        spin_unlock(&big_lock);
        return memory;
    }
    // 计算 exp，使得 2^(exp-1) <= size + sizeof(block_t) <= 2^exp
    int exp = 1;
    for (; exp <= N; exp++) {
        if (size + sizeof(block_t) <= power(2, exp)) {
            break;
        }
    }

    int allocated = 0;
    block_t *curr = NULL;
    int i = exp;  // 记录真正被分配到的 order
    for (; i < N; i++) {
        // 尝试分配给 free_lists[i]
        curr = free_lists[i];
        if (curr != NULL) {
            remove_blk(curr);
            allocated = 1;
            break;
        }
    }

    // 要么是新申请的 block，要么是现有的 block。前者需要 vmalloc 申请内存，后者需要 divide 劈开
    if (!allocated) {
        // 在 free_lists[exp] 的头部追加 block_t
        void *raw = vmalloc(NULL, power(2, exp));
        curr = (block_t *) raw;
        curr -> size = power(2, exp);
        curr -> free = 1;
        curr->next = NULL;
        curr->prev = NULL;
    } else {
        curr = divide(size, curr, i);
    }

    // 赋值 memory 和 free
    curr->free = 0;
    memory = (void *)(curr + 1);
    malloc_count++;
    spin_unlock(&big_lock);

    return memory;
}

void myfree(void *ptr) {
    spin_lock(&big_lock);
    block_t *block = (block_t *)ptr - 1;
    block->free = 1;

    int order = my_log2(block -> size);
    while (order < N) {
        block_t *buddy = (block_t *)((uintptr_t)block ^ ((uintptr_t)1 << order));
        if (buddy->free != 1 || buddy->size != block->size) {
            break;
        }
        remove_blk(buddy);
        if (block > buddy) block = buddy;
        block->size *= 2;
        order++;
    }
    insert_blk(block);
    spin_unlock(&big_lock);
}

#ifndef FREESTANDING
#include <stdio.h>

void dump_free_lists(void) {
    spin_lock(&big_lock);
    int empty = 1;
    for (int order = 0; order < N; order++) {
        if (free_lists[order] == NULL) continue;
        empty = 0;
        printf("order %2d (size=%10d):", order, power(2, order));
        block_t *b = free_lists[order];
        while (b) {
            printf(" [%p", (void *)b);
            if (b->free) printf(" free");
            printf(" sz=%lu]", (unsigned long)b->size);
            b = b->next;
        }
        printf("\n");
    }
    if (empty) printf("(all free_lists empty)\n");
    spin_unlock(&big_lock);
}
#endif
