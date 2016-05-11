#include <debug.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"


/**
 * \evict_table
 * \table structure for supporting eviction
 */
struct evict_table
{
	struct list list;       /* list that contains physical frame */

	/* Pointer that indicates current position in list */
	struct list_elem *curr;
};


static struct evict_table evict_table;  /* Eviction table described above */
static struct hash frame_table;         /* Frame hash table for mamnaging frames */


/* Hash function for hash data structures
 * Details are described below */
static unsigned frame_hash (const struct hash_elem *e, void *aux UNUSED);
static bool frame_hash_less (const struct hash_elem *a,
                             const struct hash_elem *b,
                             void *aux UNUSED);


/**
 * \frame_init
 * \Initialize frame table and evict table
 *
 * \param void
 * \retval void
 */
void
frame_init (void)
{
	list_init (&evict_table.list);
	evict_table.curr = NULL;
	hash_init (&frame_table, frame_hash, frame_hash_less, NULL);
}


/**
 * \frame_add_page
 * \Allocate frame entry and add it in frame table and evict table
 *
 * \param paddr   physical address (a.k.a kernel virtual address)
 * \param vaddr   user virtual address
 *
 * \retval true if success.
 * \retval false if fails anyway.
 */
bool
frame_add_page (void *paddr, void *vaddr)
{
	struct frame_entry *fe = malloc (sizeof (struct frame_entry));
	/* If there is no memory for allocate frame entry return false */
	if (fe == NULL)
		return false;

	/* Add page address informations about this physical frame */
	fe->paddr = paddr;
	fe->vaddr = vaddr;

	/* Explicitly describe owner of this frame.
	 * Because there are many same virtual page address in many processes,
	 * owner should be needed */
	fe->owner = thread_current ();

	/* Insert entry into frame table and evict table */
	hash_insert (&frame_table, &fe->elem_hash);
	list_push_back (&evict_table.list, &fe->elem_list);

	/* Successfully installed, return true */
	return true;
}


/**
 * \frame_get_entry
 * \Find and return frame entry corresponding to physical address
 *
 * \param paddr   physical address of physical page
 *
 * \retval        NULL if there is no corresponding entry
 * \retval        struct frame_entry* if found
 */
struct frame_entry *
frame_get_entry (void *paddr)
{
	struct frame_entry fe;
	struct hash_elem *e;
	fe.paddr = paddr;
	e = hash_find (&frame_table, &fe.elem_hash);
	return e != NULL ? hash_entry (e, struct frame_entry, elem_hash) : NULL;
}


/**
 * \frame_free_page
 * \Delete frame entry from frame table and evict table
 *
 * \param     fe  frame_entry which to be free
 *
 * \retval    void
 */
void
frame_free_page (struct frame_entry *fe)
{
	/* Delete entry from frame hash table */
	hash_delete (&frame_table, &fe->elem_hash);

	/* If evict table points parameter's entry,
	 * move to next entry */
	if (&fe->elem_list == evict_table.curr)
		evict_table.curr = list_next (&fe->elem_list);

	/* Remove from evict table */
	list_remove (&fe->elem_list);

	/* Free! */
	free (fe);
}


/**
 * \frame_evict
 * \Using second chance algorithm, find out the frame entry to victim
 *
 * \param   void
 *
 * \retval  frame entry to be evicted
 */
struct frame_entry *
frame_evict (void)
{
	/* If current is not initialized, set it begin of evict table's list */
	if (evict_table.curr == NULL)
		evict_table.curr = list_begin(&evict_table.list);

	/* Using second chance algorithm, find entry to be eliminated */
	while (1) {
		for (; evict_table.curr != list_end(&evict_table.list);
		       list_next(evict_table.curr)) {
			struct frame_entry *temp = list_entry(evict_table.curr,
																						struct frame_entry,
																	          elem_list);
			struct thread *t = temp->owner;

			/* Acquire lock for owner's page lock */
			lock_acquire(&t->page_lock);
			/* If entry is not accessed, imediately return it.
			 * Otherwise, set accessed bit to false and continue */
			if (!pagedir_is_accessed(t->pagedir, temp->vaddr)) {
				lock_release(&t->page_lock);
				return temp;
			}
			else
				pagedir_set_accessed(t->pagedir, temp->vaddr, false);
			lock_release(&t->page_lock);
		}

		/* As fifo queue is circular, rewinds the current position */
		evict_table.curr = list_begin(&evict_table.list);
	}
}


/***** hash function and less function for hash table initialization *****/

/**
 * \internal
 *
 * \frame_hash
 * \Make hash using hash element, in this case,
 * \physical address is unique, identity function of physical address
 * \is proper choice
 *
 * \param   e  hash element of frame entry
 * \param   aux auxiliary data (UNUSED in this function)
 *
 * \retval  hash value (identical to physical address)
 */
static unsigned
frame_hash (const struct hash_elem *e, void *aux UNUSED)
{
	struct frame_entry *fe = hash_entry (e, struct frame_entry, elem_hash);
	return hash_bytes (&fe->paddr, sizeof fe->paddr);
}


/**
 * \internal
 *
 * \frame_hash_less
 * \Compare two hash element of frame entry.
 * \Just naively compares their physical address
 *
 * \param  a_ ,b_  hash entry of frame entry
 *
 * \retval true if a's hash value is less than b's one.
 * \retval false otherwise.
 */
static bool
frame_hash_less (const struct hash_elem *a_,
                 const struct hash_elem *b_,
                 void *aus UNUSED)
{
	struct frame_entry *a = hash_entry (a_, struct frame_entry, elem_hash);
	struct frame_entry *b = hash_entry (b_, struct frame_entry, elem_hash);
	return a->paddr < b->paddr;
}
