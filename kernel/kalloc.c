// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run
{
    struct run *next;
};

struct kmem
{
    struct spinlock lock;
    struct run *freelist;
};

static struct kmem kmems[NCPU];

// 将一页放入指定 CPU 的 freelist（供 freerange 和 kfree 共用）
static void kfree_cpu(void *pa, int id)
{
    struct run *r;

    // 填充垃圾以捕获悬空引用。
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    acquire(&kmems[id].lock);
    r->next = kmems[id].freelist;
    kmems[id].freelist = r;
    release(&kmems[id].lock);
}

// 初始化每 CPU 的 kmem 锁与 freelist，并将所有页分配给各 CPU（轮转）
void kinit()
{
    for (int i = 0; i < NCPU; i++)
    {
        // 锁名以 "kmem" 开头
        initlock(&kmems[i].lock, "kmem");
        kmems[i].freelist = 0;
    }
    freerange(end, (void *)PHYSTOP);
}

// 将 [pa_start, pa_end) 区间内的页，按 CPU 轮转分发到各 CPU freelist
void freerange(void *pa_start, void *pa_end)
{
    char *p;
    int id = 0;

    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    {
        // 基本健壮性检查（与原 kfree 同步）
        if (((uint64)p % PGSIZE) != 0 || (char *)p < end || (uint64)p >= PHYSTOP)
            panic("freerange");
        // 轮转投递到各 CPU
        kfree_cpu((void *)p, id);
        id = (id + 1) % NCPU;
    }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 释放：放入当前 CPU freelist
void kfree(void *pa)
{
    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

    // 使用 cpuid() 必须在关中断期间
    push_off();
    int id = cpuid();
    pop_off();

    kfree_cpu(pa, id);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
    struct run *r = 0;

    // 获取当前 CPU id（关中断使用）
    push_off();
    int id = cpuid();
    pop_off();

    // 先尝试本地 freelist
    acquire(&kmems[id].lock);
    r = kmems[id].freelist;
    if (r)
        kmems[id].freelist = r->next;
    release(&kmems[id].lock);

    // 本地为空则尝试从其他 CPU 窃取
    if (r == 0)
    {
        for (int j = 0; j < NCPU; j++)
        {
            if (j == id)
                continue;
            acquire(&kmems[j].lock);
            if (kmems[j].freelist)
            {
                r = kmems[j].freelist;
                kmems[j].freelist = r->next;
                release(&kmems[j].lock);
                break;
            }
            release(&kmems[j].lock);
        }
    }

    if (r)
        memset((char *)r, 5, PGSIZE); // 填充垃圾

    return (void *)r;
}
