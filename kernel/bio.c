// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13

struct
{
    struct spinlock bcache_locks[NBUCKETS];
    struct buf buf[NBUF];
    struct buf bcache_heads[NBUCKETS];
} bcache;

static inline int hash(uint blockno)
{
    return blockno % NBUCKETS;
}

void binit(void)
{
    struct buf *b;
    char lock_name[16];

    for (int i = 0; i < NBUCKETS; i++)
    {
        snprintf(lock_name, sizeof(lock_name), "bcache_%d", i);
        initlock(&bcache.bcache_locks[i], lock_name);
        // Create linked list of buffers
        bcache.bcache_heads[i].prev = &bcache.bcache_heads[i];
        bcache.bcache_heads[i].next = &bcache.bcache_heads[i];
    }

    for (b = bcache.buf; b < bcache.buf + NBUF; b++)
    {
        b->next = bcache.bcache_heads[0].next;
        b->prev = &bcache.bcache_heads[0];
        initsleeplock(&b->lock, "buffer");
        bcache.bcache_heads[0].next->prev = b;
        bcache.bcache_heads[0].next = b;
    }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
    struct buf *b;
    int bucket = hash(blockno);

    acquire(&bcache.bcache_locks[bucket]);

    // Is the block already cached?
    for (b = bcache.bcache_heads[bucket].next; b != &bcache.bcache_heads[bucket]; b = b->next)
    {
        if (b->dev == dev && b->blockno == blockno)
        {
            b->refcnt++;
            release(&bcache.bcache_locks[bucket]);
            acquiresleep(&b->lock);
            return b;
        }
    }

    // Not cached.
    // 从当前桶中查找空闲缓冲区
    for (b = bcache.bcache_heads[bucket].prev; b != &bcache.bcache_heads[bucket]; b = b->prev)
    {
        if (b->refcnt == 0)
        {
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;
            b->refcnt = 1;
            release(&bcache.bcache_locks[bucket]);
            acquiresleep(&b->lock);
            return b;
        }
    }

    release(&bcache.bcache_locks[bucket]);

    // 如果当前桶没有空闲缓冲区，则从其他桶中查找空闲缓冲区
    for (int i = 0; i < NBUCKETS; i++)
    {
        if (i == bucket)
            continue;

        acquire(&bcache.bcache_locks[i]);
        for (b = bcache.bcache_heads[i].prev; b != &bcache.bcache_heads[i]; b = b->prev)
        {
            if (b->refcnt == 0)
            {
                // 窃取这个缓冲区，移动到目标桶
                // 避免死锁，按顺序获取锁
                // 释放当前桶的锁，然后按顺序获取两个桶的锁
                int first = i < bucket ? i : bucket;
                int second = i < bucket ? bucket : i;

                release(&bcache.bcache_locks[i]);

                acquire(&bcache.bcache_locks[first]);
                if (first != second)
                    acquire(&bcache.bcache_locks[second]);

                // 再次检查缓冲区是否仍然空闲
                if (b->refcnt == 0)
                {
                    // 从旧桶中移除
                    b->prev->next = b->next;
                    b->next->prev = b->prev;

                    // 添加到新桶
                    b->next = bcache.bcache_heads[bucket].next;
                    b->prev = &bcache.bcache_heads[bucket];
                    bcache.bcache_heads[bucket].next->prev = b;
                    bcache.bcache_heads[bucket].next = b;

                    b->dev = dev;
                    b->blockno = blockno;
                    b->valid = 0;
                    b->refcnt = 1;

                    if (first != second)
                        release(&bcache.bcache_locks[second]);
                    release(&bcache.bcache_locks[first]);

                    acquiresleep(&b->lock);
                    return b;
                }

                if (first != second)
                    release(&bcache.bcache_locks[second]);
                release(&bcache.bcache_locks[first]);

                // 如果缓冲区不再空闲，从头开始继续查找
                return bget(dev, blockno);
            }
        }
        release(&bcache.bcache_locks[i]);
    }

    panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
    struct buf *b;

    b = bget(dev, blockno);
    if (!b->valid)
    {
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
    if (!holdingsleep(&b->lock))
        panic("bwrite");
    virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
    if (!holdingsleep(&b->lock))
        panic("brelse");

    releasesleep(&b->lock);

    int bucket = hash(b->blockno);
    acquire(&bcache.bcache_locks[bucket]);
    b->refcnt--;
    if (b->refcnt == 0)
    {
        // no one is waiting for it.
        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->next = bcache.bcache_heads[bucket].next;
        b->prev = &bcache.bcache_heads[bucket];
        bcache.bcache_heads[bucket].next->prev = b;
        bcache.bcache_heads[bucket].next = b;
    }

    release(&bcache.bcache_locks[bucket]);
}

void bpin(struct buf *b)
{
    int bucket = hash(b->blockno);
    acquire(&bcache.bcache_locks[bucket]);
    b->refcnt++;
    release(&bcache.bcache_locks[bucket]);
}

void bunpin(struct buf *b)
{
    int bucket = hash(b->blockno);
    acquire(&bcache.bcache_locks[bucket]);
    b->refcnt--;
    release(&bcache.bcache_locks[bucket]);
}
