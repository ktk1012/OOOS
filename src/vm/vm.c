#include <hash.h>
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/vm.h"

#include <stdio.h>

static struct lock vm_frame_lock;

// static void page_destroy_action (struct hash_elem *e, void *aux UNUSED);
static bool vm_swap_in (struct page_entry *spte);
static bool vm_load_demand (struct page_entry *spte);


void
vm_init (void)
{
	lock_init (&vm_frame_lock);
	frame_init ();
	swap_init ();
}

bool
vm_load (void *fault_addr, void *esp)
{
	void *upage = pg_round_down (fault_addr);
	void *kpage = NULL;

	struct thread *curr = thread_current ();
	lock_acquire (&curr->page_lock);
	struct page_entry *spte = page_get_entry (&curr->page_table, upage);
	lock_release (&curr->page_lock);
	if (spte)
	{
		switch (spte->type)
		{
			case MEM:
				break;
			case DISK:
				return vm_swap_in (spte);
			case FILE:
				return vm_load_demand (spte);
		}
		return true;
	}
	else
	{
		if (fault_addr >= esp - 32)
		{
			kpage = vm_get_page (PAL_USER | PAL_ZERO, upage);
			if (kpage == NULL)
				return false;
			return vm_install_page (upage, kpage, true, false, PAL_USER | PAL_ZERO, MEM);
		}
		else
			return false;
	}
}


void
vm_init_page (void)
{
	page_init_page ();
}

void *
vm_get_page (enum palloc_flags flags, void *vaddr)
{
	if (!(flags & PAL_USER))
		return NULL;

	lock_acquire (&vm_frame_lock);
	void *paddr = palloc_get_page (flags);

	if (paddr != NULL)
	{
		if (!frame_add_page (paddr, vaddr))
		{
			palloc_free_page (paddr);
			lock_release (&vm_frame_lock);
			return NULL;
		}
		lock_release (&vm_frame_lock);
		return paddr;
	}
	else
	{
		struct frame_entry *fe = frame_evict ();

		lock_acquire (&fe->owner->page_lock);

		struct page_entry *spte = page_get_entry (&fe->owner->page_table, fe->vaddr);

		if (spte->type != FILE ||
				pagedir_is_dirty (fe->owner->pagedir, fe->vaddr))
		{
			size_t swap_idx = swap_write (fe->paddr);
			spte->block_idx = swap_idx;
			spte->type = DISK;
		}
		spte->is_loaded = false;
		pagedir_clear_page (fe->owner->pagedir, fe->vaddr);
		lock_release (&fe->owner->page_lock);
		palloc_free_page (fe->paddr);
		frame_free_page (fe);

		paddr = palloc_get_page (flags);
		if (!frame_add_page (paddr, vaddr))
		{
			palloc_free_page (paddr);
			lock_release (&vm_frame_lock);
			return NULL;
		}
		lock_release (&vm_frame_lock);
		return paddr;
		// PANIC ("eviction will be implemented\n");
	}
}

bool vm_install_page (void *upage, void *kpage, bool writable, bool lazy,
                      enum palloc_flags flags, enum page_type type)
{
	return page_install_page (upage, kpage, writable, lazy, flags, type);
}


void
vm_free_page (void *paddr)
{
	lock_acquire (&vm_frame_lock);
	struct frame_entry *fe = frame_get_entry (paddr);
	if (fe == NULL)
	{
		lock_release (&vm_frame_lock);
		return;
	}
	struct page_entry *spte = page_get_entry (&fe->owner->page_table, fe->vaddr);
	page_delete_entry (&fe->owner->page_table, spte);
	palloc_free_page (fe->paddr);
	frame_free_page (fe);
	lock_release (&vm_frame_lock);
}


void
vm_destroy_page_table (struct hash *table)
{
	lock_acquire (&vm_frame_lock);
	page_destroy_table (table);
	lock_release (&vm_frame_lock);
}

bool
vm_load_lazy (struct file *file, off_t ofs, void *vaddr,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	struct thread *curr = thread_current ();
	lock_acquire (&curr->page_lock);
	bool result = page_load_lazy (&curr->page_table, file, ofs, vaddr,
																read_bytes, zero_bytes, writable);
	lock_release (&curr->page_lock);
	return result;
}

static bool
vm_swap_in (struct page_entry *spte)
{
	void *paddr = vm_get_page (spte->flags, spte->vaddr);
	struct frame_entry *fe = frame_get_entry (paddr);
	if (paddr == NULL)
		return false;

	lock_acquire (&fe->owner->page_lock);
	swap_read (spte->block_idx, paddr);
	spte->is_loaded = true;
	if (!pagedir_set_page (fe->owner->pagedir, spte->vaddr, paddr, spte->writable))
	{
		lock_release (&fe->owner->page_lock);
		vm_free_page (paddr);
		return false;
	}
	lock_release (&fe->owner->page_lock);
	return true;
}

static bool
vm_load_demand (struct page_entry *spte)
{
	void *paddr = vm_get_page (spte->flags, spte->vaddr);
	if (paddr == NULL)
		return false;

	struct thread *curr = thread_current ();
	lock_acquire (&curr->page_lock);

	if (!page_load_demand (spte, paddr))
	{
		vm_free_page (paddr);
		lock_release (&curr->page_lock);
		return false;
	}
	lock_release (&curr->page_lock);
	return true;
}
