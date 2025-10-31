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
#define BKSIZE 17
#define HASH(d, b) (((d) * (b)) % BKSIZE)

struct {
  struct spinlock lock[BKSIZE];
  struct buf buf[NBUF];
  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[BKSIZE]; // dummy head
	
	char hlk_name[BKSIZE][9];
	char blk_name[NBUF][6];
} bcache;


void
binit(void)
{
  struct buf *b;
	int i;

	for (i = 0; i < BKSIZE; i++) {
		bcache.hlk_name[i][0] = 'b'; bcache.hlk_name[i][1] = 'c';
		bcache.hlk_name[i][2] = 'a'; bcache.hlk_name[i][3] = 'c';
		bcache.hlk_name[i][4] = 'h'; bcache.hlk_name[i][5] = 'e';
		bcache.hlk_name[i][6] = '0' + (i / 10);
		bcache.hlk_name[i][7] = '0' + (i % 10);
		bcache.hlk_name[i][8] = 0;
		
		initlock(&bcache.lock[i], bcache.hlk_name[i]);

		acquire(&bcache.lock[i]);
		bcache.head[i].prev = &bcache.head[i];
		bcache.head[i].next = &bcache.head[i];
		release(&bcache.lock[i]);
	}

	i = 0;
	acquire(&bcache.lock[0]);
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
		// buf field will be init by kernel as 0.
    b->next = bcache.head[0].next;
    b->prev = &bcache.head[0];
		
		bcache.blk_name[i][0] = 'b'; bcache.blk_name[i][1] = 'l';
		bcache.blk_name[i][2] = 'k';
		bcache.blk_name[i][3] = '0' + (i / 10);
		bcache.blk_name[i][4] = '0' + (i % 10);
		bcache.blk_name[i][5] = 0;
	
    initsleeplock(&b->lock, bcache.blk_name[i]);
    bcache.head[0].next->prev = b;
    bcache.head[0].next = b;
  	
		i++;
	}
	release(&bcache.lock[0]);
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
	uint bi, hi = HASH(dev, blockno);

	// prevent structure revising during traversal.
  acquire(&bcache.lock[hi]);
	for (b = bcache.head[hi].next; b != &bcache.head[hi]; b = b->next) {
		if (b->dev == dev && b->blockno == blockno) {
			b->refcnt++;
			release(&bcache.lock[hi]);
			acquiresleep(&b->lock); // cache hit
			return b;	
		}
	}
	release(&bcache.lock[hi]);
	
	// cache miss
	for (bi = 0; bi < BKSIZE; bi++) {
		// another cache hit case allow a race b/w a miss's bi and many hi's.
		// but there is nothing to do since cache hit is more important to access rapidly to data.
		acquire(&bcache.lock[bi]); // prevent structure revising during traversal.
		// after bi lock, cache hit feels a race but it will be ok soon.
		for (b = bcache.head[bi].next; b != &bcache.head[bi]; b = b->next) {
			if (!b->refcnt) {
				// cut buffer
				b->next->prev = b->prev;
				b->prev->next = b->next;

				// assign field		
				b->dev = dev;
				b->blockno = blockno;
				b->valid = 0;
				b->refcnt = 1;
				release(&bcache.lock[bi]);
				// there is no nested locks so that deadlocks cannot occur instead some races.	
				acquire(&bcache.lock[hi]);
				// connect buffer				
				b->next = bcache.head[hi].next;
				b->prev = &bcache.head[hi];
				bcache.head[hi].next->prev = b;
				bcache.head[hi].next = b;
				release(&bcache.lock[hi]);
	
				acquiresleep(&b->lock);
				return b;
			}
		}
		release(&bcache.lock[bi]);
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

	uint hi = HASH(b->dev, b->blockno);

  releasesleep(&b->lock);

	acquire(&bcache.lock[hi]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head[hi].next;
    b->prev = &bcache.head[hi];
    bcache.head[hi].next->prev = b;
    bcache.head[hi].next = b;
  }
  
  release(&bcache.lock[hi]);
}

void
bpin(struct buf *b) {
	uint hi = HASH(b->dev, b->blockno);
	acquire(&bcache.lock[hi]);
  b->refcnt++;
  release(&bcache.lock[hi]);
}

void
bunpin(struct buf *b) {
	uint hi = HASH(b->dev, b->blockno);
	acquire(&bcache.lock[hi]);
  b->refcnt--;
  release(&bcache.lock[hi]);
}


