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

#define NQUEUE 29

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
int  hashqueue_eq(struct hashqueue *, struct hashqueue *);
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

  for(int i = 0; i < NQUEUE; i++)
    hashqueue_init(&bcache.hashqueue[i]);

  // Create linked list of buffers
  freelist_init(&bcache.freelist);
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    freelist_addhead(&bcache.freelist, b);
    // make sure this buffer is not exist in any hashqueue
    remove_from_hashqueue(b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // search from hashqueue first
  struct hashqueue* hq = gethashqueue(blockno);

  acquire(&hq->lock);
  for(b = hq->head.hnext; b != &hq->head; b = b->hnext){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&hq->lock);

      // this buffer is currently in freelist, we should remove it from freelist
      // to avoid others accessing it
      if(b->refcnt == 1){
        // it must exist in freelist
        if(!isin_freelist(b))
          panic("bget: b must exist in freelist");

        acquire(&bcache.freelist.lock);
        remove_from_freelist(b);
        release(&bcache.freelist.lock);
      }

      acquiresleep(&b->lock);
      return b;
    }
  }

  // need to get new buffer from freelist
  acquire(&bcache.freelist.lock);
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.freelist.head.lprev; b != &bcache.freelist.head; b = b->lprev){
    if(b->refcnt == 0) {
      // remove this buffer from current freelist
      remove_from_freelist(b);
      // don't need to lock anymore
      release(&bcache.freelist.lock);

      // this buffer is currently in other's hashqueue, we should remove it from there
      struct hashqueue *oldhq = gethashqueue(b->blockno);
      if(isin_hashqueue(b)){
        if (!hashqueue_eq(hq, oldhq)) {
          remove_from_hashqueue(b);
          hashqueue_addhead(hq, b);
        }
      } else {
        // add to new hq
        hashqueue_addhead(hq, b);
      }
      release(&hq->lock);

      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;

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

  // this buffer must exist in some hashqueue
  if(!isin_hashqueue(b))
    panic("brelse: buffer must exist in hashqueue");

  b->refcnt--;
  if(b->refcnt == 0){
    // no one is waiting for it.
    acquire(&bcache.freelist.lock);
    freelist_addhead(&bcache.freelist, b);
    release(&bcache.freelist.lock);
  }
}

void
bpin(struct buf *b) {
  // this buffer must exist in some hashqueue
  if(!isin_hashqueue(b))
    panic("brelse: buffer must exist in hashqueue");
  struct hashqueue *hq = gethashqueue(b->blockno);
  acquire(&hq->lock);
  b->refcnt++;
  release(&hq->lock);
}

void
bunpin(struct buf *b) {
  // this buffer must exist in some hashqueue
  if(!isin_hashqueue(b))
    panic("brelse: buffer must exist in hashqueue");
  struct hashqueue *hq = gethashqueue(b->blockno);
  acquire(&hq->lock);
  b->refcnt--;
  release(&hq->lock);
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


int
hashqueue_eq(struct hashqueue *lhs, struct hashqueue *rhs)
{
  return &lhs->head == &rhs->head;
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

uint
hash(uint value)
{
  uint64 FNV_OFFSET = 0xcbf29ce484222325ULL;
  uint64 FNV_PRIME = 0x100000001b3ULL;

  uint64 hash = FNV_OFFSET;
  for (int i = 0; i < sizeof(value); i++) {
    hash ^= (value >> (i * 8)) & 0xFF;  // Extract each byte
    hash *= FNV_PRIME;        // Multiply by the prime
  }

  return hash;
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
  if(b->lnext && b->lprev){
    b->lnext->lprev = b->lprev;
    b->lprev->lnext = b->lnext;
  }
  b->lnext = b->lprev = 0;
}

void
remove_from_hashqueue(struct buf *b)
{
  if(b->hnext && b->hprev){
    b->hnext->hprev = b->hprev;
    b->hprev->hnext = b->hnext;
  }
  b->hnext = b->hprev = 0;
}
