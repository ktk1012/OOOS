#include <hash.h>
#include <list.h>
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "vm/vm.h"

#include <stdio.h>

static struct lock vm_frame_lock; /* Lock for synch vm system */
static struct lock vm_mmap_lock;

struct lock filesys_lock;


/* Internal function for swap in */
static bool vm_swap_in (struct page_entry *spte);
static bool vm_load_demand (struct page_entry *spte);


/**
 * \vm_init
 * \Initialize frame table and swap table.
 *
 * \param void
 *
 * \retval void
 */
void
vm_init (void)
{
	/* Init frame lock and call frame_init and swap_init in
	 * frame.c and swap.c, respectively */
	lock_init (&vm_frame_lock);
	lock_init (&vm_mmap_lock);
	frame_init ();
	swap_init ();
}


/**
 * \vm_load
 * \Handles page fault in various cases
 *
 * \param fault_addr  address that fault occurred
 * \param esp  stack pointer for determine stack growth
 *
 * \retval  true if page fault is properly handled.
 * \retval  false otherwise.
 */
bool
vm_load (void *fault_addr, void *esp)
{
	/* Find user page address from fault addr */
	void *upage = pg_round_down (fault_addr);
	void *kpage = NULL;

	/* Find supplenetal page entry for determining fault case */
	struct thread *curr = thread_current ();
	lock_acquire (&curr->page_lock);
	struct page_entry *spte = page_get_entry (&curr->page_table, upage);
	lock_release (&curr->page_lock);

	/* If there is virtual page, handle */
	if (spte)
	{
		switch (spte->type)
		{
			case MEM:
				/* Impossible case */
				break;
			case DISK:
				/* If data is swapped out, swap in */
				return vm_swap_in (spte);
			case FILE:
			case MMAP:
				return vm_load_demand (spte);
		}
		/* Properly handled */
		return true;
	}
	else
	{
		/* If fault address is below the stack pointer, determine
		 * whether fault is stack growth or not */
		if (fault_addr >= esp - 32)
		{
			/* If fault addr is above the esp - 32 (WORD), it is stack growth */
			kpage = vm_get_page (PAL_USER | PAL_ZERO, upage);
			if (kpage == NULL)
				return false;

			/* Install grown stack page to page table */
			return vm_install_page (upage, kpage, true, PAL_USER | PAL_ZERO, MEM);
		}
		else    /* It is not proper case, return false to page fault handler */
			return false;
	}
}


/**
 * \vm_init_page
 * \Init supplemental page table for user process
 *
 * \param void
 *
 * \retval void
 */
void
vm_init_page (void)
{
	page_init_page ();
}


/**
 * \vm_get_page
 * \Get physical page and map with virtual page address
 *
 * \param flags palloc flag
 * \param vaddr virtual page address
 *
 * \retval address of physical page
 * \retval NULL if allocation failed
 */
void *
vm_get_page (enum palloc_flags flags, void *vaddr)
{

	/* If not User page allocation request, return NULL */
	if (!(flags & PAL_USER))
		return NULL;

	lock_acquire (&vm_frame_lock);
	/* Get page from user pool */
	void *paddr = palloc_get_page (flags);

	if (paddr != NULL)
	{
		/* If allocation available, add it to frame table and return */
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
		/* palloc is not available, eviction occurs */
		/* Find entry to be evicted */
		struct frame_entry *fe = frame_evict ();

		lock_acquire (&fe->owner->page_lock);

		struct page_entry *spte = page_get_entry (&fe->owner->page_table, fe->vaddr);

		if (spte->type == MMAP)
		{
			if (pagedir_is_dirty(fe->owner->pagedir, fe->vaddr))
			{
				//lock_acquire (&filesys_lock)
				file_write_at (spte->file, fe->paddr, spte->read_bytes, spte->ofs);
				//lock_release (&filesys_lock);
			}
		}
		else if (spte->type != FILE ||
				pagedir_is_dirty (fe->owner->pagedir, fe->vaddr))
		{
			size_t swap_idx = swap_write (fe->paddr);
			spte->block_idx = swap_idx;
			spte->type = DISK;
		}

		/* Set sup.page entry loaded flag to false */
		spte->is_loaded = false;
		/* Clear page table entry (set present bit invalid */
		pagedir_clear_page (fe->owner->pagedir, fe->vaddr);
		lock_release (&fe->owner->page_lock);

		/* Free page */
		palloc_free_page (fe->paddr);

		/* Remove frame entry corresponding to paddr */
		frame_free_page (fe);

		/* Re allocate for request */
		paddr = palloc_get_page (flags);

		/* Add it to frame table */
		if (!frame_add_page (paddr, vaddr))
		{
			palloc_free_page (paddr);
			lock_release (&vm_frame_lock);
			return NULL;
		}
		lock_release (&vm_frame_lock);
		return paddr;
	}
}


/**
 * \vm_install_page
 * \Simple wrapper for page_install_page
 *
 * \param  same as page_install_page (refer page.c)
 *
 * \reval  same as page_install_page (refer page.c too)
 */
bool vm_install_page (void *upage, void *kpage, bool writable,
                      enum palloc_flags flags, enum page_type type)
{
	return page_install_page (upage, kpage, writable, flags, type);
}


/**
 * \vm_free_page
 * \Free allocated page
 *
 * \param paddr physical page
 *
 * \retval void
 */
void
vm_free_page (void *paddr)
{
	lock_acquire (&vm_frame_lock);
	/* Find frame entry corresponding to paddr */
	struct frame_entry *fe = frame_get_entry (paddr);
	if (fe == NULL)
	{
		lock_release (&vm_frame_lock);
		return;
	}

	/* Find supplemental page entry and remove it from table */
	lock_acquire (&fe->owner->page_lock);
	struct page_entry *spte = page_get_entry (&fe->owner->page_table, fe->vaddr);
	page_delete_entry (&fe->owner->page_table, spte);
	lock_release (&fe->owner->page_lock);

	/* Remove frame entry and free physical page */
	palloc_free_page (fe->paddr);
	frame_free_page (fe);
	lock_release (&vm_frame_lock);
}


/**
 * \vm_destroy_page_table
 * \Destroy supplemental page table
 *
 * \param table supplemental page table to be destroyed
 *
 * \retval void
 */
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
	struct page_entry *result = page_load_lazy (&curr->page_table, file, ofs,
	                                            vaddr, read_bytes, zero_bytes,
	                                            writable, FILE);
	lock_release (&curr->page_lock);
	return result != NULL ? true : false;
}


/**
 * \internal
 *
 * \vm_swap_in
 * \Swap data into corresponding process's user page
 *
 * \param spte  supplemental page entry for swap in
 *
 * \retval true if swap in success
 * \retval false otherwise
 */
static bool
vm_swap_in (struct page_entry *spte)
{
	/* Allocate physical page */
	void *paddr = vm_get_page (spte->flags, spte->vaddr);
	struct frame_entry *fe = frame_get_entry (paddr);

	/* If allocation fail, return false */
	if (paddr == NULL)
		return false;

	/* Read data from swap disk */
	lock_acquire (&fe->owner->page_lock);
	swap_read (spte->block_idx, paddr);
	/* Now data is loaded in memory, so set is_loaded to true */
	spte->is_loaded = true;

	/* Enable page entry corresponding to virtual page address */
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


struct mmap_entry *
vm_add_mmap (struct file *file, void *start_addr, size_t file_size)
{
	struct thread *curr = thread_current ();
	struct hash *sup_pt = &curr->page_table;
	struct mmap_entry *me = malloc (sizeof (struct mmap_entry));
	if (me == NULL)
		return NULL;
	lock_acquire (&vm_mmap_lock);
	lock_acquire (&curr->page_lock);
	/* Initialize mmap entry */
	me->file = file;
	list_init (&me->map_list);
	list_init (&me->fd_list);

	bool ret = true;
	// return ret;
	off_t ofs = 0;
	while (file_size > 0)
	{
		size_t read_bytes = file_size < PGSIZE ? file_size : PGSIZE;
		size_t zero_bytes = PGSIZE - read_bytes;

		if (page_get_entry (sup_pt, start_addr))
		{
			ret = false;
			break;
		}

		/* Add supplemental page entry */
		struct page_entry *temp = page_load_lazy (sup_pt, file, ofs, start_addr,
																						  read_bytes, zero_bytes, true,
																							MMAP);
		if (temp == NULL)
		{
			ret = false;
			break;
		}
		list_push_back (&me->map_list, &temp->elem_mmap);

		file_size -= read_bytes;
		ofs += read_bytes;
		start_addr += PGSIZE;
	}

	if (ret == false)
	{
		struct list_elem *e;
		for (e = list_begin (&me->map_list); e != list_end (&me->map_list);
			   e = list_next (e))
			page_delete_entry (sup_pt,
			                   list_entry (e, struct page_entry, elem_mmap));
		free (me);
		return NULL;
	}
	list_push_back (&curr->mmap_list, &me->elem);
	lock_release (&vm_mmap_lock);
	lock_release (&curr->page_lock);

	me->mid = curr->mapid_next++;
	return me;
}


void
vm_munmap (struct mmap_entry *me)
{
	struct thread *curr = thread_current ();
	struct page_entry *spte;
	lock_acquire (&vm_mmap_lock);
	while (!list_empty (&me->map_list))
	{
		spte = list_entry (list_pop_front (&me->map_list),
											 struct page_entry,
										   elem_mmap);
		if (spte->is_loaded)
		{
			void *paddr = pagedir_get_page (curr->pagedir, spte->vaddr);
			struct frame_entry *fe = frame_get_entry (paddr);
			if (pagedir_is_dirty (curr->pagedir, spte->vaddr))
			{
				//lock_acquire (&filesys_lock)
				file_write_at(spte->file, fe->paddr, spte->read_bytes, spte->ofs);
				//lock_release (&filesys_lock);
			}
			vm_free_page (paddr);
		}
		else
			page_delete_entry (&curr->page_table, spte);
	}
	lock_release (&vm_mmap_lock);
	return;
}
