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
#include "icache.h"   // struct icache definition, inode_operations, iget proto
#include "ext2fs.h"   // struct ext2fs_addrs (needed by iget for slot allocation)

// File-scope externs resolved by ext2fs.c at link time.
extern struct inode_operations ext2fs_inode_ops;
extern struct ext2fs_addrs     ext2fs_addrs[NINODE];

#define min(a, b) ((a) < (b) ? (a) : (b))

// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;
  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init xv6 fs (only used if ROOTDEV is xv6fs, not ext2).
void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
  ireclaim(dev);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;
  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// returns 0 if out of disk space.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){
        bp->data[bi/8] |= m;
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  printf("balloc: out of blocks\n");
  return 0;
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// ---------------------------------------------------------------------------
// Inode cache
// Named "icache" (was "itable") so ext2fs.c and icache.h share one definition.
// ---------------------------------------------------------------------------
struct icache icache;

void
iinit()
{
  int i = 0;
  initlock(&icache.lock, "itable");
  for(i = 0; i < NINODE; i++)
    initsleeplock(&icache.inode[i].lock, "inode");
}

// iget() is non-static so ext2fs.c can call it directly.
struct inode *iget(uint dev, uint inum);

struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  printf("ialloc: no inodes\n");
  return 0;
}

void
iupdate(struct inode *ip)
{
  // Dispatch to filesystem-specific update if iops is set.
  if(ip->iops){
    ip->iops->iupdate(ip);
    return;
  }
  struct buf *bp;
  struct dinode *dip;
  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type  = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size  = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// ---------------------------------------------------------------------------
// iget: find or allocate inode table entry for (dev, inum).
// For ROOTDEV, pre-sets iops and allocates an ext2fs_addrs slot so that
// ext2fs_ilock can fill in block addresses on first lock.
// ---------------------------------------------------------------------------
struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)
      empty = ip;
  }

  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev   = dev;
  ip->inum  = inum;
  ip->ref   = 1;
  ip->valid = 0;
  ip->iops  = 0;
  ip->ext_addrs = 0;

  // If this inode belongs to the ext2 root device, wire up ext2 ops and
  // grab a slot from the ext2fs_addrs pool.
  if(dev == ROOTDEV){
    ip->iops = &ext2fs_inode_ops;
    // Find a free addrs slot.
    for(int i = 0; i < NINODE; i++){
      if(ext2fs_addrs[i].busy == 0){
        ext2fs_addrs[i].busy = 1;
        ip->ext_addrs = &ext2fs_addrs[i];
        break;
      }
    }
    if(ip->ext_addrs == 0)
      panic("iget: no ext2fs_addrs slots");
  }

  release(&icache.lock);
  return ip;
}

struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock an inode. Dispatches to ext2fs_ilock if iops is set.
void
ilock(struct inode *ip)
{
  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  if(ip->iops){
    ip->iops->ilock(ip);
    return;
  }

  struct buf *bp;
  struct dinode *dip;

  acquiresleep(&ip->lock);
  if(ip->valid == 0){
    bp  = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type  = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size  = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");
  releasesleep(&ip->lock);
}

// Drop a reference. Dispatches to ext2fs_iput if iops is set.
void
iput(struct inode *ip)
{
  if(ip->iops){
    ip->iops->iput(ip);
    return;
  }

  acquire(&icache.lock);
  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    acquiresleep(&ip->lock);
    release(&icache.lock);
    itrunc(ip);
    ip->type  = 0;
    iupdate(ip);
    ip->valid = 0;
    releasesleep(&ip->lock);
    acquire(&icache.lock);
  }
  ip->ref--;
  release(&icache.lock);
}

void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

void
ireclaim(int dev)
{
  for(int inum = 1; inum < sb.ninodes; inum++){
    struct inode *ip = 0;
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode *)bp->data + inum % IPB;
    if(dip->type != 0 && dip->nlink == 0){
      printf("ireclaim: orphaned inode %d\n", inum);
      ip = iget(dev, inum);
    }
    brelse(bp);
    if(ip){
      begin_op();
      ilock(ip);
      iunlock(ip);
      iput(ip);
      end_op();
    }
  }
}

static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    if((addr = ip->addrs[NDIRECT]) == 0){
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[NDIRECT] = addr;
    }
    bp = bread(ip->dev, addr);
    a  = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      addr = balloc(ip->dev);
      if(addr){
        a[bn] = addr;
        log_write(bp);
      }
    }
    brelse(bp);
    return addr;
  }
  panic("bmap: out of range");
}

void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a  = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++)
      if(a[j]) bfree(ip->dev, a[j]);
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }
  ip->size = 0;
  iupdate(ip);
}

void
stati(struct inode *ip, struct stat *st)
{
  // Dispatch to ext2 stati if iops set.
  if(ip->iops){
    ip->iops->stati(ip, st);
    return;
  }
  st->dev   = ip->dev;
  st->ino   = ip->inum;
  st->type  = ip->type;
  st->nlink = ip->nlink;
  st->size  = ip->size;
}

// Read from inode. Dispatches to ext2fs_readi if iops set.
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  if(ip->iops)
    return ip->iops->readi(ip, user_dst, dst, off, n);

  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    bp = bread(ip->dev, addr);
    m  = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1){
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write to inode. Dispatches to ext2fs_writei if iops set.
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  if(ip->iops)
    return ip->iops->writei(ip, user_src, src, off, n);

  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    bp = bread(ip->dev, addr);
    m  = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1){
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }
  if(off > ip->size)
    ip->size = off;
  iupdate(ip);
  return tot;
}

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// dirlookup: dispatches to ext2 dirlookup if iops set.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  if(dp->iops)
    return dp->iops->dirlookup(dp, name, poff);

  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }
  return 0;
}

// dirlink: dispatches to ext2 dirlink if iops set.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  if(dp->iops)
    return dp->iops->dirlink(dp, name, inum);

  int off;
  struct dirent de;
  struct inode *ip;

  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;
  return 0;
}

static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}