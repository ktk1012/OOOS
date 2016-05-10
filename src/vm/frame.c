#include <debug.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"

#include <stdio.h>

struct evict_table
{
	struct list list;
	struct list_elem *curr;
};

static struct evict_table evict_table;
static struct hash frame_table;


static unsigned frame_hash (const struct hash_elem *e, void *aux UNUSED);
static bool frame_hash_less (const struct hash_elem *a,
                             const struct hash_elem *b,
                             void *aux UNUSED);

void
frame_init (void)
{
	list_init (&evict_table.list);
	evict_table.curr = NULL;
	hash_init (&frame_table, frame_hash, frame_hash_less, NULL);
}

bool
frame_add_page (void *paddr, void *vaddr)
{
	struct frame_entry *fe = malloc (sizeof (struct frame_entry));
	if (fe == NULL)
		return false;

	fe->paddr = paddr;
	fe->vaddr = vaddr;
	fe->owner = thread_current ();
	hash_insert (&frame_table, &fe->elem_hash);
	list_push_back (&evict_table.list, &fe->elem_list);
	return true;
}

struct frame_entry *
frame_get_entry (void *paddr)
{
	struct frame_entry fe;
	struct hash_elem *e;
	fe.paddr = paddr;
	e = hash_find (&frame_table, &fe.elem_hash);
	return e != NULL ? hash_entry (e, struct frame_entry, elem_hash) : NULL;
}

void
frame_free_page (struct frame_entry *fe)
{
	hash_delete (&frame_table, &fe->elem_hash);
	if (&fe->elem_list == evict_table.curr)
		evict_table.curr = list_next (&fe->elem_list);

	list_remove (&fe->elem_list);
	free (fe);
}

struct frame_entry *
frame_evict (void)
{
	if (evict_table.curr == NULL)
		evict_table.curr = list_begin(&evict_table.list);

	while (1) {
		for (; evict_table.curr != list_end(&evict_table.list);
		       list_next(evict_table.curr)) {
			struct frame_entry *temp = list_entry(evict_table.curr,
																						struct frame_entry,
																	          elem_list);
			struct thread *t = temp->owner;
			// printf ("owner: %s, %p\n", t->name, t->pagedir);
			lock_acquire(&t->page_lock);
			if (!pagedir_is_accessed(t->pagedir, temp->vaddr)) {
				lock_release(&t->page_lock);
				return temp;
			}
			else
				pagedir_set_accessed(t->pagedir, temp->vaddr, false);
			lock_release(&t->page_lock);
		}
		evict_table.curr = list_begin(&evict_table.list);
	}
}

/* Hash function for physical address,
 * as physical address is unique, use paddr */
static unsigned
frame_hash (const struct hash_elem *e, void *aux UNUSED)
{
	struct frame_entry *fe = hash_entry (e, struct frame_entry, elem_hash);
	return hash_bytes (&fe->paddr, sizeof fe->paddr);
}

/* Hash less function, It compares physical address */
static bool
frame_hash_less (const struct hash_elem *a,
                 const struct hash_elem *b,
                 void *aus UNUSED)
{
	struct frame_entry *fe1 = hash_entry (a, struct frame_entry, elem_hash);
	struct frame_entry *fe2 = hash_entry (b, struct frame_entry, elem_hash);
	return fe1->paddr < fe2->paddr;
}
