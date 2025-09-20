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

#define BUCKETSIZE 13 // number of hashing buckets
#define BUFFERSIZE 5 // number of available buckets per bucket

extern uint ticks; // system time clock
struct spinlock evict_lock;

struct {
  struct spinlock lock;      
  struct buf head;          // 每个桶是一个 buf 链表头
  struct buf buf[BUFFERSIZE];
} bcache[BUCKETSIZE];

int
hash(uint blockno)
{
  return blockno % BUCKETSIZE;
}

void
binit(void)
{
  struct buf *b;
  // 初始化时：
  initlock(&evict_lock, "bcache evict");
  for (int i = 0; i < BUCKETSIZE; ++i) {
    initlock(&bcache[i].lock, "bcache");
    bcache[i].head.prev = &bcache[i].head;
    bcache[i].head.next = &bcache[i].head;
    for (b = bcache[i].buf; b < bcache[i].buf+BUFFERSIZE; ++b) {
      b->next = bcache[i].head.next;
      b->prev = &bcache[i].head;
      initsleeplock(&b->lock, "buffer");
      bcache[i].head.next->prev = b;
      bcache[i].head.next = b;
    }
  }
}

// 未命中时利用LRU查找空闲块。成功返回查找到的块，失败报错；若查找到的块被其他进程使用了，返回0重新查找。
struct buf*
select_victim(int buckno)
{
  // 如果buckno的桶已满，把ticks最小的那个refcnt=0的块中的从桶中偷走
  uint least = 0xffffffff;
  struct buf * least_b = 0;  // 要偷的块
  int least_buck = -1;       // 要偷的块所在的桶
  for(int i = 0; i < BUCKETSIZE; ++i) {
    if (i == buckno) {
      continue;
    }
    else {
      acquire(&bcache[i].lock);
      for(struct buf *b = bcache[i].head.prev; b != &bcache[i].head; b = b->prev) {
        if(b->refcnt == 0 && b->ticks < least) {
          least_b = b;
          least_buck = i;
          least = b->ticks;
        }
      }
      release(&bcache[i].lock);
    }
  }
  // 如果没有找到空位，那么报错
  if (!least_b)  {
    panic("bget: no buffers");
    return 0;
  }

  // 再次获取 victim 所在桶锁，预占它！
  acquire(&bcache[least_buck].lock);
  if (least_b->refcnt != 0) {
    release(&bcache[least_buck].lock);
    return 0;  // 被抢了，返回空，让调用者重试
  }
  // 预占：标记为“正在驱逐中”
  least_b->refcnt = -1;  // 其他线程看到 -1 会跳过
  release(&bcache[least_buck].lock);

  return least_b;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int buckno = hash(blockno);
  acquire(&bcache[buckno].lock);

  // Is the block already cached?
  for (b = bcache[buckno].head.next; b != &bcache[buckno].head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      if (b->refcnt == -1) {
        // 正在被驱逐，跳过
        continue;
      }
      ++b->refcnt;
      b->ticks = ticks;
      release(&bcache[buckno].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache[buckno].head.prev; b != &bcache[buckno].head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache[buckno].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 如果buckno的桶已满，把ticks最小的那个refcnt=0的块中的从桶中偷走
  // 先从其他桶中查找块
  release(&bcache[buckno].lock);
  acquire(&evict_lock);
  struct buf* least_b = 0;
  while (least_b == 0) {
    least_b = select_victim(buckno);
  }

  int least_buck = hash(least_b->blockno);
  release(&evict_lock);  // 安全释放
  

  // 开始偷桶，把要偷的块从桶中摘下来，放到buckno桶中head->prev的位置
  acquire(&bcache[least_buck].lock);
  least_b->prev->next = least_b->next;
  least_b->next->prev = least_b->prev;
  release(&bcache[least_buck].lock);

  acquire(&bcache[buckno].lock);
  least_b->dev = dev;
  least_b->blockno = blockno;
  least_b->valid = 0;
  least_b->refcnt= 1;
  least_b->ticks = ticks;
  least_b->prev = bcache[buckno].head.prev;
  least_b->next = &bcache[buckno].head;
  bcache[buckno].head.prev->next = least_b;
  bcache[buckno].head.prev = least_b;
  release(&bcache[buckno].lock);
  acquiresleep(&least_b->lock);
  return least_b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  int buckno = hash(b->blockno);
  acquire(&bcache[buckno].lock);
  if (b->refcnt == -1) {
    release(&bcache[buckno].lock);
    panic("brelse: refcnt == -1");
  }
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache[buckno].head.next;
    b->prev = &bcache[buckno].head;
    bcache[buckno].head.next->prev = b;
    bcache[buckno].head.next = b;
  }
  
  release(&bcache[buckno].lock);
}

void
bpin(struct buf *b) {

  int bukno = hash(b->blockno);
  acquire(&bcache[bukno].lock);
  b->refcnt++;
  release(&bcache[bukno].lock);
}

void
bunpin(struct buf *b) {
  int bukno = hash(b->blockno);
  acquire(&bcache[bukno].lock);
  b->refcnt--;
  release(&bcache[bukno].lock);
}


