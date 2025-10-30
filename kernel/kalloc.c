// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
	char name[7]; // assume the #cores is up to 99, kmem##.
} kmem[NCPU];

void
kinit()
{
	int i;
	for (i = 0; i < NCPU; i++) {
		kmem[i].freelist = 0;		

		kmem[i].name[0] = 'k'; kmem[i].name[1] = 'm';
		kmem[i].name[2] = 'e'; kmem[i].name[3] = 'm';
		kmem[i].name[4] = '0' + (i / 10); kmem[i].name[5] = '0' + (i % 10);
		kmem[i].name[6] = 0;
		
		initlock(&(kmem[i].lock), kmem[i].name);
  }
	freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
		kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
	int from;
	
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
	
	push_off();
	from = cpuid();
	pop_off();
  
	memset(pa, 1, PGSIZE);
  
	r = (struct run*)pa;
  
	acquire(&(kmem[from].lock));
  
	r->next = kmem[from].freelist;
  kmem[from].freelist = r;
  release(&(kmem[from].lock));
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
	int core_idx;

	push_off();
	core_idx = cpuid();
	pop_off();
  acquire(&(kmem[core_idx].lock));
  r = kmem[core_idx].freelist;

  if (r) {
	  kmem[core_idx].freelist = r->next;
		release(&(kmem[core_idx].lock));
	}
	else {
		int i = (core_idx + 1) % NCPU;
		struct run* diter, *half;

		release(&(kmem[core_idx].lock));
		for (; i != core_idx; i = (i + 1) % NCPU) {
			acquire(&(kmem[i].lock));
			r = kmem[i].freelist;

			if (r) {
				diter = half = r;
				while (!diter && !diter->next && !diter->next->next) {
					half = diter->next;
					diter = diter->next->next;
				}
				kmem[i].freelist = half->next;
				release(&(kmem[i].lock));
				
				acquire(&(kmem[core_idx].lock));
				half->next = kmem[core_idx].freelist;
				kmem[core_idx].freelist = r->next;
				release(&(kmem[core_idx].lock));
				break;			
			}
			release(&(kmem[i].lock));
		}
	}

	if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
	return (void*)r;
}
