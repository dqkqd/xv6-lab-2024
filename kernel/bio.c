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
} bcache;

struct hashqueue *gethashqueue(uint);

// buf related functions
int isin_hashqueue(struct buf *);
void remove_from_hashqueue(struct buf *);

// lock 2 locks at the sametime, with reordering to avoid deadlock
void lockboth(struct spinlock *, struct spinlock *);

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < NQUEUE; i++)
    hashqueue_init(&bcache.hashqueue[i]);

  // Create linked list of buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    // make sure this buffer is not exist in any hashqueue
    remove_from_hashqueue(b);
    // blockno < 0 means we haven't cached it yet
    b->blockno = -1;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct hashqueue *hq = 0, *ohq = 0;

  // search from hashqueue first
  // we don't need bcache.lock for this,
  // because those buffers with b->blockno = blockno are protected by `hq.lock`
  hq = gethashqueue(blockno);
  acquire(&hq->lock);
  for(b = hq->head.next; b != &hq->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;

      if(!isin_hashqueue(b))
        panic("bget: buffer must in hashqueue");

      release(&hq->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // drop the lock here
  // so that we can retain the order (bcache.lock then hq.lock)
  release(&hq->lock);

  acquire(&bcache.lock);
  // checking newly buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    if(b->blockno < 0)
      goto found;
  }

  acquire(&hq->lock);
  // Not cached.
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    ohq = gethashqueue(b->blockno);
    // try to lock both of those hashqueue respecting order to avoid deadlock
    lockboth(&hq->lock, &ohq->lock);

    if(b->refcnt == 0)
      goto found;

    // we locked ohq, so need to release it
    if(!hashqueue_eq(ohq, hq))
      release(&ohq->lock);
  }
  panic("bget: no buffers");

found:
  b->refcnt = 1;
  b->valid = 0;
  b->dev = dev;
  b->blockno = blockno;
  hashqueue_addhead(hq, b);

  release(&bcache.lock);

  if(holding(&hq->lock))
    release(&hq->lock);

  if(holding(&ohq->lock))
    release(&ohq->lock);

  acquiresleep(&b->lock);
  return b;
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
  struct hashqueue *hq = gethashqueue(b->blockno);
  acquire(&hq->lock);
  if(!isin_hashqueue(b))
    panic("brelse: buffer must exist in hashqueue");
  b->refcnt--;
  release(&hq->lock);
}

void
bpin(struct buf *b) {
  // this buffer must exist in some hashqueue
  struct hashqueue *hq = gethashqueue(b->blockno);
  acquire(&hq->lock);
  if(!isin_hashqueue(b))
    panic("bpin: buffer must exist in hashqueue");
  b->refcnt++;
  release(&hq->lock);
}

void
bunpin(struct buf *b) {
  // this buffer must exist in some hashqueue
  struct hashqueue *hq = gethashqueue(b->blockno);
  acquire(&hq->lock);
  if(!isin_hashqueue(b))
    panic("bunpin: buffer must exist in hashqueue");
  b->refcnt--;
  release(&hq->lock);
}

int
hashqueue_eq(struct hashqueue *lhs, struct hashqueue *rhs)
{
  return &lhs->head == &rhs->head;
}

void
hashqueue_init(struct hashqueue *hq)
{
  hq->head.prev = &hq->head;
  hq->head.next = &hq->head;
  initlock(&hq->lock, "bcache.hashqueue");
}

void
hashqueue_addhead(struct hashqueue *hq, struct buf *b)
{
  remove_from_hashqueue(b);

  b->next = hq->head.next;
  b->prev = &hq->head;
  hq->head.next->prev = b;
  hq->head.next = b;
}

uint
hash(uint value)
{
  return value;
}

struct hashqueue *
gethashqueue(uint blockno)
{
  if(blockno < 0)
    return 0;
  uint index = hash(blockno) % NQUEUE;
  return &bcache.hashqueue[index];
}

int
isin_hashqueue(struct buf *b)
{
  return b->prev != 0 || b->next != 0;
}

void
remove_from_hashqueue(struct buf *b)
{
  if(b->next && b->prev){
    b->next->prev = b->prev;
    b->prev->next = b->next;
  }
  b->next = b->prev = 0;
}

void
lockboth(struct spinlock *lhs, struct spinlock *rhs)
{
  // reorder lock to avoid deadlock, lhs must hold the lock
  if(!holding(lhs))
    panic("lockboth: lhs must hold the lock");

  // already locked
  if(lhs == rhs)
    return;

  if(lhs > rhs){
    // must reorder
    release(lhs);
    acquire(rhs);
    acquire(lhs);
  } else {
    // already in order, just lock rhs and return
    acquire(rhs);
  }
}

