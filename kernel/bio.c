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

#define NQUEUE 13

struct freelist {
  struct spinlock lock;
  struct buf head;
};
void freelist_init(struct freelist *);
void freelist_addhead(struct freelist *, struct buf *);

struct hashqueue {
  struct spinlock lock;
  struct buf head;
};
void hashqueue_init(struct hashqueue *);
void hashqueue_addhead(struct hashqueue *, struct buf *);
uint hash(uint);

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  struct hashqueue hashqueue[NQUEUE];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct freelist freelist;
} bcache;

struct hashqueue *gethashqueue(uint);

// buf related functions
int isin_freelist(struct buf *);
int isin_hashqueue(struct buf *);
void remove_from_freelist(struct buf *);
void remove_from_hashqueue(struct buf *);

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  for(int i = 0; i < NQUEUE; i++)
    hashqueue_init(&bcache.hashqueue[i]);

  // Create linked list of buffers
  freelist_init(&bcache.freelist);
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    freelist_addhead(&bcache.freelist, b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.freelist.head.lnext; b != &bcache.freelist.head; b = b->lnext){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.freelist.head.lprev; b != &bcache.freelist.head; b = b->lprev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
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

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    remove_from_freelist(b);
    freelist_addhead(&bcache.freelist, b);
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}

void
freelist_init(struct freelist *fl)
{
  fl->head.lprev = &fl->head;
  fl->head.lnext = &fl->head;
  initlock(&fl->lock, "bcache");
}

void
freelist_addhead(struct freelist *fl, struct buf *b)
{
    b->lnext = fl->head.lnext;
    b->lprev = &fl->head;
    fl->head.lnext->lprev = b;
    fl->head.lnext = b;
}

void
hashqueue_init(struct hashqueue *hq)
{
  hq->head.hprev = &hq->head;
  hq->head.hnext = &hq->head;
  initlock(&hq->lock, "bcache");
}

void
hashqueue_addhead(struct hashqueue *hq, struct buf *b)
{
    b->hnext = hq->head.hnext;
    b->hprev = &hq->head;
    hq->head.hnext->hprev = b;
    hq->head.hnext = b;
}

void
hashqueue_remove(struct buf *b)
{
    b->hnext->hprev = b->hprev;
    b->hprev->hnext = b->hnext;
}

uint
hash(uint value)
{
  value++; // avoid unused warning
  return 0;
}

struct hashqueue *
gethashqueue(uint blockno)
{
  uint index = hash(blockno) % NQUEUE;
  return &bcache.hashqueue[index];
}

int
isin_freelist(struct buf *b)
{
  return b->lprev != 0 || b->lnext != 0;
}

int
isin_hashqueue(struct buf *b)
{
  return b->hprev != 0 || b->hnext != 0;
}

void
remove_from_freelist(struct buf *b)
{
  b->lnext->lprev = b->lprev;
  b->lprev->lnext = b->lnext;
  b->lnext = b->lprev = 0;
}

void
remove_from_hashqueue(struct buf *b)
{
  b->hnext->hprev = b->hprev;
  b->hprev->hnext = b->hnext;
  b->hnext = b->hprev = 0;
}
