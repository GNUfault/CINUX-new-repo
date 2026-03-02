//
// icache.h - inode cache and inode_operations for the ext2 driver (RISC-V xv6)
//
// Include order requirements (include these BEFORE icache.h):
//   types.h  param.h  riscv.h  spinlock.h  sleeplock.h  fs.h  file.h
//

#ifndef ICACHE_H
#define ICACHE_H

// inode_operations - per-filesystem dispatch table.
// All signatures follow RISC-V xv6 VFS conventions.
// This must be defined before file.h so that struct inode can embed *iops.
struct stat;
struct inode;

struct inode_operations {
  int           (*dirlink)   (struct inode *, char *, uint);
  struct inode *(*dirlookup) (struct inode *, char *, uint *);
  struct inode *(*ialloc)    (uint dev, short type);
  void          (*iinit)     (int dev);
  void          (*ilock)     (struct inode *);
  void          (*iput)      (struct inode *);
  void          (*iunlock)   (struct inode *);
  void          (*iunlockput)(struct inode *);
  void          (*iupdate)   (struct inode *);
  int           (*readi)     (struct inode *, int user_dst, uint64 dst,
                               uint off, uint n);
  void          (*stati)     (struct inode *, struct stat *);
  int           (*writei)    (struct inode *, int user_src, uint64 src,
                               uint off, uint n);
};

// Named struct tag "icache" matches the extern in ext2fs.h.
// fs.c defines this as "struct icache icache" (renamed from itable).
struct icache {
  struct spinlock lock;
  struct inode    inode[NINODE];
};

extern struct icache icache;

// iget() is non-static in fs.c so ext2fs.c can call it.
struct inode *iget(uint dev, uint inum);

#endif // ICACHE_H