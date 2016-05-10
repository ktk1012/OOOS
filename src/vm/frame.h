#ifndef VM_FRAME_H
#define VM_FRAME_H
#include <hash.h>
#include <list.h>
#include "threads/thread.h"

struct frame_entry
{
	struct thread *owner;
	void *paddr;
	void *vaddr;
	struct hash_elem elem_hash;
	struct list_elem elem_list;
};


void frame_init (void);
bool frame_add_page (void *paddr, void *vaddr);
struct frame_entry *frame_get_entry (void *paddr);
void frame_free_page (struct frame_entry *fe);
struct frame_entry *frame_evict (void);

#endif //OOOS_FRAME_H
