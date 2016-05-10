#include "page.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"


static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED);
static bool page_hash_less (const struct hash_elem *_a,
                            const struct hash_elem *_b,
                            void *aux UNUSED);
static void page_destroy_action (struct hash_elem *e, void *aux UNUSED);


void
page_init_page (void)
{
	struct thread *curr =thread_current ();
	hash_init (&curr->page_table, page_hash, page_hash_less, NULL);
	lock_init (&curr->page_lock);
}

bool
page_install_page (void *upage, void *kpage, bool writable, bool lazy,
									 enum palloc_flags flags, enum page_type type)
{
	struct thread *curr = thread_current ();
	lock_acquire (&curr->page_lock);
	struct page_entry *pe = malloc (sizeof (struct page_entry));
	if (pe == NULL)
	{
		lock_release (&curr->page_lock);
		return false;
	}
	pe->vaddr = upage;
	pe->type = type;
	pe->flags = flags;
	pe->writable = writable;
	pe->is_loaded = !lazy;
	hash_insert (&curr->page_table, &pe->elem);
	bool result = install_page (upage, kpage, writable);
	lock_release (&curr->page_lock);
	return result;
}

struct page_entry *
page_get_entry (struct hash *table, void *vaddr)
{
	struct page_entry spte;

	spte.vaddr = vaddr;
	struct hash_elem *e = hash_find (table, &spte.elem);

	return e != NULL ? hash_entry (e, struct page_entry, elem) : NULL;
}

void
page_delete_entry (struct hash *table, struct page_entry *spte)
{
	hash_delete (table, &spte->elem);
	free (spte);
}

void page_destroy_table (struct hash *table)
{
	hash_destroy (table, page_destroy_action);
}


static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED)
{
	struct page_entry *pe = hash_entry (e, struct page_entry, elem);
	return hash_bytes (&pe->vaddr, sizeof pe->vaddr);
}

static bool page_hash_less (const struct hash_elem *_a,
                            const struct hash_elem *_b,
                            void *aux UNUSED)
{
	struct page_entry *a = hash_entry (_a, struct page_entry, elem);
	struct page_entry *b = hash_entry (_b, struct page_entry, elem);
	return a->vaddr < b->vaddr;
}

static void
page_destroy_action (struct hash_elem *e, void *aux UNUSED)
{
	struct page_entry *pe = hash_entry (e, struct page_entry, elem);
	struct thread *t = thread_current ();
	void *paddr;
	if (pe->is_loaded)
	{
		paddr = pagedir_get_page (t->pagedir, pe->vaddr);
		struct frame_entry *fe = frame_get_entry (paddr);
		pagedir_clear_page (t->pagedir, pe->vaddr);
		palloc_free_page (fe->paddr);
		frame_free_page (fe);
		free (pe);
		return;
	}
	switch (pe->type)
	{
		case MEM:
			break;
		case DISK:
			swap_delete (pe->block_idx);
			break;
		case FILE:
			break;
	}
	free (pe);
}
