// ext2fs.c - ext2 filesystem driver ported from xv6-x86 to xv6-riscv
//
// Key differences from the x86 version:
//   - mmu.h removed (not present in RISC-V xv6; types come from riscv.h/stat.h)
//   - icache is aliased from itable in fs.c (see icache.h)
//   - cprintf() replaced with printf()
//   - iops function signatures updated to RISC-V xv6 VFS conventions
//     (readi/writei take int user_flag + uint64 addr instead of char*)
//   - device read/write calls updated to RISC-V devsw signature
//   - ip->addrs replaced with ip->ext_addrs (void* to ext2fs_addrs pool)

#include "types.h"
#include "param.h"
#include "riscv.h"           // defines uint64, pagetable_t, pte_t - must be first
#include "spinlock.h"        // defines struct spinlock
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"            // after riscv.h so pagetable_t/uint64 are known
#include "stat.h"
#include "fs.h"
#include "ext2fs.h"
#include "buf.h"
#include "file.h"
#include "icache.h"          // inode_operations, struct icache, iget()

// ---------------------------------------------------------------------------
// Forward declarations of all ext2fs functions so the ops table can be
// filled in at the top of the file (matches the x86 structure).
// ---------------------------------------------------------------------------
int          ext2fs_dirlink   (struct inode*, char*, uint);
struct inode*ext2fs_dirlookup (struct inode*, char*, uint*);
struct inode*ext2fs_ialloc    (uint, short);
void         ext2fs_iinit     (int);
void         ext2fs_ilock     (struct inode*);
void         ext2fs_iput      (struct inode*);
void         ext2fs_iunlock   (struct inode*);
void         ext2fs_iunlockput(struct inode*);
void         ext2fs_iupdate   (struct inode*);
int          ext2fs_readi     (struct inode*, int, uint64, uint, uint);
void         ext2fs_stati     (struct inode*, struct stat*);
int          ext2fs_writei    (struct inode*, int, uint64, uint, uint);

struct inode_operations ext2fs_inode_ops = {
  ext2fs_dirlink,
  ext2fs_dirlookup,
  ext2fs_ialloc,
  ext2fs_iinit,
  ext2fs_ilock,
  ext2fs_iput,
  ext2fs_iunlock,
  ext2fs_iunlockput,
  ext2fs_iupdate,
  ext2fs_readi,
  ext2fs_stati,
  ext2fs_writei,
};

#define min(a,b) ((a) < (b) ? (a) : (b))

static void  ext2fs_bzero(int dev, int bno);
static uint  ext2fs_balloc(uint dev, uint inum);
static void  ext2fs_bfree(int dev, uint b);
static uint  ext2fs_bmap(struct inode *ip, uint bn);
static void  ext2fs_itrunc(struct inode *ip);

// Per-inode address pool (replaces per-inode addrs[] for ext2 inodes).
struct ext2fs_addrs ext2fs_addrs[NINODE];
struct ext2_super_block ext2_sb;

// ---------------------------------------------------------------------------
// Helper: cast ip->ext_addrs to the typed pointer used throughout.
// ---------------------------------------------------------------------------
static inline struct ext2fs_addrs *
ip_ad(struct inode *ip)
{
  return (struct ext2fs_addrs *)ip->ext_addrs;
}

// ---------------------------------------------------------------------------
// Read the ext2 superblock from disk (always at block 1).
// ---------------------------------------------------------------------------
void
ext2fs_readsb(int dev, struct ext2_super_block *sb)
{
  struct buf *bp;
  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// ---------------------------------------------------------------------------
// Zero a disk block.
// ---------------------------------------------------------------------------
static void
ext2fs_bzero(int dev, int bno)
{
  struct buf *bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  bwrite(bp);
  brelse(bp);
}

// ---------------------------------------------------------------------------
// Scan a block bitmap and return the index of the first free bit,
// marking it as used.  Returns -1 if all bits are set.
// ---------------------------------------------------------------------------
static uint
ext2fs_free_block(char *bitmap)
{
  int i, j, mask;
  for (i = 0; i < (int)(ext2_sb.s_blocks_per_group * 8); i++) {
    for (j = 0; j < 8; j++) {
      mask = 1 << (7 - j);
      if ((bitmap[i] & mask) == 0) {
        bitmap[i] |= mask;
        return i * 8 + j;
      }
    }
  }
  return (uint)-1;
}

// ---------------------------------------------------------------------------
// Allocate a zeroed disk block in the block group containing inode inum.
// ---------------------------------------------------------------------------
static uint
ext2fs_balloc(uint dev, uint inum)
{
  int gno, zbno;
  uint fbit;
  struct ext2_group_desc bgdesc;
  struct buf *bp1, *bp2;

  gno  = GET_GROUP_NO(inum, ext2_sb);
  bp1  = bread(dev, 2);
  memmove(&bgdesc, bp1->data + gno * sizeof(bgdesc), sizeof(bgdesc));
  brelse(bp1);

  bp2  = bread(dev, bgdesc.bg_block_bitmap);
  fbit = ext2fs_free_block((char *)bp2->data);
  if (fbit != (uint)-1) {
    zbno = bgdesc.bg_block_bitmap + fbit;
    bwrite(bp2);
    ext2fs_bzero(dev, zbno);
    brelse(bp2);
    return zbno;
  }
  brelse(bp2);
  panic("ext2_balloc: out of blocks");
}

// ---------------------------------------------------------------------------
// Free disk block b.
// ---------------------------------------------------------------------------
static void
ext2fs_bfree(int dev, uint b)
{
  int gno, iindex, mask;
  struct ext2_group_desc bgdesc;
  struct buf *bp1, *bp2;

  gno    = GET_GROUP_NO(b, ext2_sb);
  iindex = GET_INODE_INDEX(b, ext2_sb);
  bp1    = bread(dev, 2);
  memmove(&bgdesc, bp1->data + gno * sizeof(bgdesc), sizeof(bgdesc));
  bp2    = bread(dev, bgdesc.bg_block_bitmap);
  iindex -= bgdesc.bg_block_bitmap;
  mask   = 1 << (iindex % 8);

  if ((bp2->data[iindex / 8] & mask) == 0)
    panic("ext2fs_bfree: block already free");
  bp2->data[iindex / 8] &= ~mask;
  bwrite(bp2);
  brelse(bp2);
  brelse(bp1);
}

// ---------------------------------------------------------------------------
// Filesystem initialisation: read the superblock and print a summary.
// ---------------------------------------------------------------------------
void
ext2fs_iinit(int dev)
{
  ext2fs_readsb(dev, &ext2_sb);
  printf("ext2_sb: magic_number %x size %d nblocks %d ninodes %d "
         "inodes_per_group %d inode_size %d\n",
         ext2_sb.s_magic,
         1024 << ext2_sb.s_log_block_size,
         ext2_sb.s_blocks_count,
         ext2_sb.s_inodes_count,
         ext2_sb.s_inodes_per_group,
         ext2_sb.s_inode_size);
}

// ---------------------------------------------------------------------------
// Allocate a new inode of the given type on device dev.
// Returns an unlocked, allocated, referenced inode.
// ---------------------------------------------------------------------------
struct inode *
ext2fs_ialloc(uint dev, short type)
{
  int i, bno, iindex, bgcount, inum;
  uint fbit;
  struct buf *bp1, *bp2, *bp3;
  struct ext2_inode *din;
  struct ext2_group_desc bgdesc;

  bgcount = ext2_sb.s_blocks_count / ext2_sb.s_blocks_per_group;
  for (i = 0; i <= bgcount; i++) {
    bp1 = bread(dev, 2);
    memmove(&bgdesc, bp1->data + i * sizeof(bgdesc), sizeof(bgdesc));
    brelse(bp1);

    bp2  = bread(dev, bgdesc.bg_inode_bitmap);
    fbit = ext2fs_free_block((char *)bp2->data);
    if (fbit == (uint)-1) {
      brelse(bp2);
      continue;
    }

    bno    = bgdesc.bg_inode_table +
             fbit / (EXT2_BSIZE / sizeof(struct ext2_inode));
    iindex = fbit % (EXT2_BSIZE / sizeof(struct ext2_inode));
    bp3    = bread(dev, bno);
    din    = (struct ext2_inode *)bp3->data + iindex;
    memset(din, 0, sizeof(*din));
    if (type == T_DIR)
      din->i_mode = S_IFDIR;
    else if (type == T_FILE)
      din->i_mode = S_IFREG;
    bwrite(bp3);
    bwrite(bp2);
    brelse(bp3);
    brelse(bp2);

    inum = i * ext2_sb.s_inodes_per_group + fbit + 1;
    return iget(dev, inum);
  }
  panic("ext2_ialloc: no inodes");
}

// ---------------------------------------------------------------------------
// Write the in-memory inode back to disk.
// Caller must hold ip->lock.
// ---------------------------------------------------------------------------
void
ext2fs_iupdate(struct inode *ip)
{
  struct buf *bp, *bp1;
  struct ext2_group_desc bgdesc;
  struct ext2_inode din;
  struct ext2fs_addrs *ad;
  int gno, ioff, bno, iindex;

  gno  = GET_GROUP_NO(ip->inum, ext2_sb);
  ioff = GET_INODE_INDEX(ip->inum, ext2_sb);
  bp   = bread(ip->dev, 2);
  memmove(&bgdesc, bp->data + gno * sizeof(bgdesc), sizeof(bgdesc));
  brelse(bp);
  bno    = bgdesc.bg_inode_table +
           ioff / (EXT2_BSIZE / ext2_sb.s_inode_size);
  iindex = ioff % (EXT2_BSIZE / ext2_sb.s_inode_size);
  bp1    = bread(ip->dev, bno);
  memmove(&din, bp1->data + iindex * ext2_sb.s_inode_size, sizeof(din));

  if (ip->type == T_DIR)  din.i_mode = S_IFDIR;
  if (ip->type == T_FILE) din.i_mode = S_IFREG;
  din.i_links_count = ip->nlink;
  din.i_size        = ip->size;
  din.i_dtime       = 0;
  din.i_faddr       = 0;
  din.i_file_acl    = 0;
  din.i_flags       = 0;
  din.i_generation  = 0;
  din.i_gid         = 0;
  din.i_mtime       = 0;
  din.i_uid         = 0;
  din.i_atime       = 0;

  ad = ip_ad(ip);
  memmove(din.i_block, ad->addrs, sizeof(ad->addrs));
  memmove(bp1->data + iindex * ext2_sb.s_inode_size, &din, sizeof(din));
  bwrite(bp1);
  brelse(bp1);
}

// ---------------------------------------------------------------------------
// Lock ip and, if not yet valid, read the inode from disk.
// ---------------------------------------------------------------------------
void
ext2fs_ilock(struct inode *ip)
{
  struct buf *bp, *bp1;
  struct ext2_group_desc bgdesc;
  struct ext2_inode din;
  struct ext2fs_addrs *ad;
  int gno, ioff, bno, iindex;

  if (ip == 0 || ip->ref < 1)
    panic("ext2fs_ilock");

  acquiresleep(&ip->lock);
  ad = ip_ad(ip);

  if (ip->valid == 0) {
    gno  = GET_GROUP_NO(ip->inum, ext2_sb);
    ioff = GET_INODE_INDEX(ip->inum, ext2_sb);
    bp   = bread(ip->dev, 2);
    memmove(&bgdesc, bp->data + gno * sizeof(bgdesc), sizeof(bgdesc));
    brelse(bp);
    bno    = bgdesc.bg_inode_table +
             ioff / (EXT2_BSIZE / ext2_sb.s_inode_size);
    iindex = ioff % (EXT2_BSIZE / ext2_sb.s_inode_size);
    bp1    = bread(ip->dev, bno);
    memmove(&din, bp1->data + iindex * ext2_sb.s_inode_size, sizeof(din));
    brelse(bp1);

    ip->type  = S_ISDIR(din.i_mode) ? T_DIR : T_FILE;
    ip->major = 0;
    ip->minor = 0;
    ip->nlink = din.i_links_count;
    ip->size  = din.i_size;
    ip->iops  = &ext2fs_inode_ops;
    memmove(ad->addrs, din.i_block, sizeof(ad->addrs));

    ip->valid = 1;
    if (ip->type == 0)
      panic("ext2fs_ilock: no type");
  }
}

// ---------------------------------------------------------------------------
// Release the sleep-lock on ip.
// ---------------------------------------------------------------------------
void
ext2fs_iunlock(struct inode *ip)
{
  if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("ext2fs_iunlock");
  releasesleep(&ip->lock);
}

// ---------------------------------------------------------------------------
// Free the on-disk inode bitmap entry for ip.
// ---------------------------------------------------------------------------
static void
ext2fs_ifree(struct inode *ip)
{
  int gno, index, mask;
  struct ext2_group_desc bgdesc;
  struct buf *bp1, *bp2;

  gno  = GET_GROUP_NO(ip->inum, ext2_sb);
  bp1  = bread(ip->dev, 2);
  memmove(&bgdesc, bp1->data + gno * sizeof(bgdesc), sizeof(bgdesc));
  brelse(bp1);
  bp2   = bread(ip->dev, bgdesc.bg_inode_bitmap);
  index = (ip->inum - 1) % ext2_sb.s_inodes_per_group;
  mask  = 1 << (index % 8);

  if ((bp2->data[index / 8] & mask) == 0)
    panic("ext2fs_ifree: inode already free");
  bp2->data[index / 8] &= ~mask;
  bwrite(bp2);
  brelse(bp2);
}

// ---------------------------------------------------------------------------
// Drop a reference to ip; free inode + data if ref hits 0 and nlink == 0.
// ---------------------------------------------------------------------------
void
ext2fs_iput(struct inode *ip)
{
  struct ext2fs_addrs *ad;

  acquiresleep(&ip->lock);
  ad = ip_ad(ip);
  if (ip->valid && ip->nlink == 0) {
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if (r == 1) {
      ext2fs_ifree(ip);
      ext2fs_itrunc(ip);
      ip->type  = 0;
      ip->iops->iupdate(ip);
      ip->valid = 0;
      ip->iops  = 0;
      ip->ext_addrs = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  if (ip->ref == 0) {
    ad->busy    = 0;
    ip->ext_addrs = 0;
  }
  release(&icache.lock);
}

// ---------------------------------------------------------------------------
// Unlock then put (common idiom).
// ---------------------------------------------------------------------------
void
ext2fs_iunlockput(struct inode *ip)
{
  ip->iops->iunlock(ip);
  ip->iops->iput(ip);
}

// ---------------------------------------------------------------------------
// Fill in stat from inode metadata.
// ---------------------------------------------------------------------------
void
ext2fs_stati(struct inode *ip, struct stat *st)
{
  st->dev   = ip->dev;
  st->ino   = ip->inum;
  st->type  = ip->type;
  st->nlink = ip->nlink;
  st->size  = ip->size;
}

// ---------------------------------------------------------------------------
// Block map: return the disk block number for logical block bn in inode ip,
// allocating blocks as needed.
//
// EXT2 block layout:
//   0..EXT2_NDIR_BLOCKS-1   : direct
//   EXT2_IND_BLOCK           : singly-indirect  (128 entries)
//   EXT2_DIND_BLOCK          : doubly-indirect  (128*128)
//   EXT2_TIND_BLOCK          : triply-indirect  (128*128*128)
// ---------------------------------------------------------------------------
static uint
ext2fs_bmap(struct inode *ip, uint bn)
{
  uint addr, *a, *b, *c;
  struct buf *bp, *bp1, *bp2;
  struct ext2fs_addrs *ad = ip_ad(ip);

  // Direct blocks
  if (bn < EXT2_NDIR_BLOCKS) {
    if ((addr = ad->addrs[bn]) == 0)
      ad->addrs[bn] = addr = ext2fs_balloc(ip->dev, ip->inum);
    return addr;
  }
  bn -= EXT2_NDIR_BLOCKS;

  // Singly-indirect
  if (bn < EXT2_INDIRECT) {
    if ((addr = ad->addrs[EXT2_IND_BLOCK]) == 0)
      ad->addrs[EXT2_IND_BLOCK] = addr = ext2fs_balloc(ip->dev, ip->inum);
    bp = bread(ip->dev, addr);
    a  = (uint *)bp->data;
    if ((addr = a[bn]) == 0)
      a[bn] = addr = ext2fs_balloc(ip->dev, ip->inum);
    brelse(bp);
    return addr;
  }
  bn -= EXT2_INDIRECT;

  // Doubly-indirect
  if (bn < EXT2_DINDIRECT) {
    if ((addr = ad->addrs[EXT2_DIND_BLOCK]) == 0)
      ad->addrs[EXT2_DIND_BLOCK] = addr = ext2fs_balloc(ip->dev, ip->inum);
    bp = bread(ip->dev, addr);
    a  = (uint *)bp->data;
    if ((addr = a[bn / EXT2_INDIRECT]) == 0)
      a[bn / EXT2_INDIRECT] = addr = ext2fs_balloc(ip->dev, ip->inum);
    bp1 = bread(ip->dev, addr);
    b   = (uint *)bp1->data;
    if ((addr = b[bn % EXT2_INDIRECT]) == 0)
      b[bn % EXT2_INDIRECT] = addr = ext2fs_balloc(ip->dev, ip->inum);
    brelse(bp);
    brelse(bp1);
    return addr;
  }
  bn -= EXT2_DINDIRECT;

  // Triply-indirect
  if (bn < EXT2_TINDIRECT) {
    if ((addr = ad->addrs[EXT2_TIND_BLOCK]) == 0)
      ad->addrs[EXT2_TIND_BLOCK] = addr = ext2fs_balloc(ip->dev, ip->inum);
    bp = bread(ip->dev, addr);
    a  = (uint *)bp->data;
    if ((addr = a[bn / EXT2_DINDIRECT]) == 0)
      a[bn / EXT2_DINDIRECT] = addr = ext2fs_balloc(ip->dev, ip->inum);
    bp1 = bread(ip->dev, addr);
    b   = (uint *)bp1->data;
    if ((addr = b[(bn % EXT2_DINDIRECT) / EXT2_INDIRECT]) == 0)
      b[(bn % EXT2_DINDIRECT) / EXT2_INDIRECT] =
        addr = ext2fs_balloc(ip->dev, ip->inum);
    bp2 = bread(ip->dev, addr);
    c   = (uint *)bp2->data;
    if ((addr = c[bn % EXT2_INDIRECT]) == 0)
      c[bn % EXT2_INDIRECT] = addr = ext2fs_balloc(ip->dev, ip->inum);
    brelse(bp);
    brelse(bp1);
    brelse(bp2);
    return addr;
  }
  panic("ext2_bmap: block number out of range");
}

// ---------------------------------------------------------------------------
// Truncate inode: free all data blocks, then clear the block pointers.
// Caller must hold ip->lock.
// ---------------------------------------------------------------------------
static void
ext2fs_itrunc(struct inode *ip)
{
  int i, j, k;
  struct buf *bp1, *bp2, *bp3;
  uint *a, *b, *c;
  struct ext2fs_addrs *ad = ip_ad(ip);

  // Direct blocks
  for (i = 0; i < EXT2_NDIR_BLOCKS; i++) {
    if (ad->addrs[i]) {
      ext2fs_bfree(ip->dev, ad->addrs[i]);
      ad->addrs[i] = 0;
    }
  }

  // Singly-indirect
  if (ad->addrs[EXT2_IND_BLOCK]) {
    bp1 = bread(ip->dev, ad->addrs[EXT2_IND_BLOCK]);
    a   = (uint *)bp1->data;
    for (i = 0; i < (int)EXT2_INDIRECT; i++) {
      if (a[i]) { ext2fs_bfree(ip->dev, a[i]); a[i] = 0; }
    }
    brelse(bp1);
    ext2fs_bfree(ip->dev, ad->addrs[EXT2_IND_BLOCK]);
    ad->addrs[EXT2_IND_BLOCK] = 0;
  }

  // Doubly-indirect
  if (ad->addrs[EXT2_DIND_BLOCK]) {
    bp1 = bread(ip->dev, ad->addrs[EXT2_DIND_BLOCK]);
    a   = (uint *)bp1->data;
    for (i = 0; i < (int)EXT2_INDIRECT; i++) {
      if (a[i]) {
        bp2 = bread(ip->dev, a[i]);
        b   = (uint *)bp2->data;
        for (j = 0; j < (int)EXT2_INDIRECT; j++) {
          if (b[j]) { ext2fs_bfree(ip->dev, b[j]); b[j] = 0; }
        }
        brelse(bp2);
        ext2fs_bfree(ip->dev, a[i]);
        a[i] = 0;
      }
    }
    brelse(bp1);
    ext2fs_bfree(ip->dev, ad->addrs[EXT2_DIND_BLOCK]);
    ad->addrs[EXT2_DIND_BLOCK] = 0;
  }

  // Triply-indirect
  if (ad->addrs[EXT2_TIND_BLOCK]) {
    bp1 = bread(ip->dev, ad->addrs[EXT2_TIND_BLOCK]);
    a   = (uint *)bp1->data;
    for (i = 0; i < (int)EXT2_INDIRECT; i++) {
      if (a[i]) {
        bp2 = bread(ip->dev, a[i]);
        b   = (uint *)bp2->data;
        for (j = 0; j < (int)EXT2_INDIRECT; j++) {
          if (b[j]) {
            bp3 = bread(ip->dev, b[j]);
            c   = (uint *)bp3->data;
            for (k = 0; k < (int)EXT2_INDIRECT; k++) {
              if (c[k]) { ext2fs_bfree(ip->dev, c[k]); c[k] = 0; }
            }
            brelse(bp3);
            ext2fs_bfree(ip->dev, b[j]);
            b[j] = 0;
          }
        }
        brelse(bp2);
        ext2fs_bfree(ip->dev, a[i]);
        a[i] = 0;
      }
    }
    brelse(bp1);
    ext2fs_bfree(ip->dev, ad->addrs[EXT2_TIND_BLOCK]);
    ad->addrs[EXT2_TIND_BLOCK] = 0;
  }

  ip->size = 0;
  ip->iops->iupdate(ip);
}

// ---------------------------------------------------------------------------
// Read n bytes from inode ip starting at offset off.
// If user_dst == 1, dst is a user virtual address; otherwise kernel address.
// Returns number of bytes read, or -1 on error.
// ---------------------------------------------------------------------------
int
ext2fs_readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if (ip->type == T_DEVICE) {
    // Delegate to the registered device driver (RISC-V devsw signature).
    if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(user_dst, dst, n);
  }

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > ip->size)
    n = ip->size - off;

  for (tot = 0; tot < n; tot += m, off += m, dst += m) {
    bp = bread(ip->dev, ext2fs_bmap(ip, off / EXT2_BSIZE));
    m  = min(n - tot, EXT2_BSIZE - off % EXT2_BSIZE);
    if (either_copyout(user_dst, dst, bp->data + off % EXT2_BSIZE, m) == -1) {
      brelse(bp);
      return -1;
    }
    brelse(bp);
  }
  return n;
}

// ---------------------------------------------------------------------------
// Write n bytes from src to inode ip starting at offset off.
// If user_src == 1, src is a user virtual address; otherwise kernel address.
// Returns number of bytes written, or -1 on error.
// ---------------------------------------------------------------------------
int
ext2fs_writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if (ip->type == T_DEVICE) {
    if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(user_src, src, n);
  }

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > EXT2_MAXFILE * EXT2_BSIZE)
    return -1;

  for (tot = 0; tot < n; tot += m, off += m, src += m) {
    bp = bread(ip->dev, ext2fs_bmap(ip, off / EXT2_BSIZE));
    m  = min(n - tot, EXT2_BSIZE - off % EXT2_BSIZE);
    if (either_copyin(bp->data + off % EXT2_BSIZE, user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    bwrite(bp);
    brelse(bp);
  }

  if (n > 0 && off > ip->size) {
    ip->size = off;
    ip->iops->iupdate(ip);
  }
  return n;
}

// ---------------------------------------------------------------------------
// Directory helpers
// ---------------------------------------------------------------------------

static int
ext2fs_namecmp(const char *s, const char *t)
{
  return strncmp(s, t, EXT2_NAME_LEN);
}

struct inode *
ext2fs_dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off;
  struct ext2_dir_entry_2 de;
  char file_name[EXT2_NAME_LEN + 1];

  for (off = 0; off < dp->size; off += de.rec_len) {
    if (dp->iops->readi(dp, 0, (uint64)&de, off, sizeof(de)) != (int)sizeof(de))
      panic("ext2fs_dirlookup: read error");
    if (de.inode == 0)
      continue;
    strncpy(file_name, de.name, de.name_len);
    file_name[de.name_len] = '\0';
    if (ext2fs_namecmp(name, file_name) == 0) {
      if (poff)
        *poff = off;
      return iget(dp->dev, de.inode);
    }
  }
  return 0;
}

int
ext2fs_dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct ext2_dir_entry_2 de;
  struct inode *ip;

  if ((ip = dp->iops->dirlookup(dp, name, 0)) != 0) {
    ip->iops->iput(ip);
    return -1;
  }

  for (off = 0; off < (int)dp->size; off += sizeof(de)) {
    if (dp->iops->readi(dp, 0, (uint64)&de, off, sizeof(de)) != (int)sizeof(de))
      panic("ext2fs_dirlink read");
    if (de.inode == 0)
      break;
  }

  memset(&de, 0, sizeof(de));
  strncpy(de.name, name, EXT2_NAME_LEN);
  de.inode    = inum;
  de.rec_len  = EXT2_BSIZE;
  de.name_len = strlen(name);
  dp->size    = off + de.rec_len;
  dp->iops->iupdate(dp);
  dp->iops->writei(dp, 0, (uint64)&de, off, de.rec_len);

  return 0;
}