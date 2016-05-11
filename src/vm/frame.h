#ifndef VM_FRAME_H
#define VM_FRAME_H
#include <hash.h>
#include <list.h>
#include "threads/thread.h"

/**
 * \frame entry
 * \Entry for frame table hash & evict table
 *
 */
struct frame_entry
{
	struct thread *owner;       /* Owner of this physical frame */
	void *paddr;                /* Kernel virtual address (a.k.a physical address) */
	void *vaddr;                /* User virtual address (a.k.a virtual address) */
	struct hash_elem elem_hash; /* Hash element for frame hash table */

	/* list element for list that implement fifo clock algorithm */
	struct list_elem elem_list;
};


void frame_init (void);
bool frame_add_page (void *paddr, void *vaddr);
struct frame_entry *frame_get_entry (void *paddr);
void frame_free_page (struct frame_entry *fe);
struct frame_entry *frame_evict (void);

#endif //OOOS_FRAME_H
