// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

// 这里仅列出修改的两个函数
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a, *b;
  struct buf *bp, *db_bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;

  if (bn < ND_INDIRECT) {
    // 加载二级间接块，在需要用到时分配
    uint indirect_index = bn / NINDIRECT;   // 哪个一级间接块（0~255）
    uint direct_index   = bn % NINDIRECT;   // 在该一级间接块中的偏移（0~255）
    // 分配/读取二级间接块
    if ((addr = ip->addrs[NDIRECT+1]) == 0) {
      ip->addrs[NDIRECT+1] = addr = balloc(ip->dev);
    }
    db_bp = bread(ip->dev, addr);
    b = (uint*)db_bp->data;
    // 分配/读取对应的一级间接块
    if ((addr = b[indirect_index]) == 0) {
      b[indirect_index] = addr = balloc(ip->dev);
      log_write(db_bp);
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    // 修改一级间接块
    if ((addr = a[direct_index]) == 0) {
      a [direct_index] = addr = balloc(ip->dev);
      log_write(bp);
    }
    // 做了两次bread，执行两次brelse
    brelse(bp);
    brelse(db_bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp, *db_bp;
  uint *a, *b;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  // 修改与bmap的修改对应，比较简单
  if (ip->addrs[NDIRECT+1]) {
    db_bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
    b = (uint*)db_bp->data;
    for (i = 0; i < NINDIRECT; ++i) {
      if (b[i]) {
        bp = bread(ip->dev, b[i]);
        a = (uint*)bp->data;
        for (j = 0; j < NINDIRECT; ++j) {
          if (a[j])
            bfree(ip->dev, a[j]);
        }
        brelse(bp);
        bfree(ip->dev, b[i]);
      }
    }
    brelse(db_bp);
    bfree(ip->dev, ip->addrs[NDIRECT+1]);
    ip->addrs[NDIRECT+1] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}
