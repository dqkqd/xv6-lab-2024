// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define KNCPUS 1

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct kmem {
  int index;

  uint64 pa_start;      // start address of freelist
  uint64 pa_end;        // end address of freelist

  struct spinlock lock;
  struct run *freelist;
};

void  kmeminit(struct kmem*);
void* kmemalloc(struct kmem*);
void  kmemfree(struct kmem*, void*);
void  kmemfreerange(struct kmem*);

static struct kmem kmems[KNCPUS];

struct kmem * currentkmem(void);

void
kinit()
{
  for(int i = 0; i < KNCPUS; ++i){
    kmems[i].index = i;
    kmeminit(&kmems[i]);
    kmemfreerange(&kmems[i]);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  kmemfree(currentkmem(), pa);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  return kmemalloc(currentkmem());
}

void
kmeminit(struct kmem *kmem)
{
  uint64 first = PGROUNDUP((uint64)end);
  uint64 last = (uint64)PHYSTOP;
  uint64 total = last - first;
  uint64 npages = total / PGSIZE;


  uint64 npages_per_cpu = npages / KNCPUS;
  uint64 pa_start = first + kmem->index * npages_per_cpu * PGSIZE;
  uint64 pa_end = first + (kmem->index + 1) * npages_per_cpu * PGSIZE;
  if(pa_end > last)
    pa_end = last;

  kmem->pa_start = pa_start;
  kmem->pa_end = pa_end;
  initlock(&kmem->lock, "kmem");
}

void *
kmemallocraw(struct kmem *kmem)
{
  struct run *r;

  acquire(&kmem->lock);
  r = kmem->freelist;
  if(r)
    kmem->freelist = r->next;
  release(&kmem->lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void *
kmemalloc(struct kmem *kmem)
{
  struct run *r;

  // get pages from current kmem
  r = kmemallocraw(kmem);

  int searched = 1;
  while(r == 0 && searched < KNCPUS){
    int index = (kmem->index + searched) % KNCPUS;
    r = kmemallocraw(&kmems[index]);
    searched++;
  }

  return (void*)r;
}

void
kmemfree(struct kmem *kmem, void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kmemfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem->lock);
  r->next = kmem->freelist;
  kmem->freelist = r;
  release(&kmem->lock);
}

void
kmemfreerange(struct kmem *kmem)
{
  char *p = (char*)kmem->pa_start;
  for(; p + PGSIZE <= (char*)kmem->pa_end; p += PGSIZE)
    kmemfree(kmem, p);
}

int
getcpuindex(void)
{
  return (1 % KNCPUS);
}

struct kmem*
currentkmem(void)
{
  int cpuid = getcpuindex();
  return &kmems[cpuid];
}
