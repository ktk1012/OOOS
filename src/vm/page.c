#include <string.h>
#include "page.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"

#include <stdio.h>


struct lock filesys_lock;


/* Hash function for hash data structures
 * Details are described below */
static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED);
static bool page_hash_less (const struct hash_elem *_a,
                            const struct hash_elem *_b,
                            void *aux UNUSED);
/* Hash action function for destroying hash table */
static void page_destroy_action (struct hash_elem *e, void *aux UNUSED);


/**
 * \page_init_page
 * \Initialize user process's supplemental page table
 *
 * \param   void
 *
 * \retval  void
 */
void
page_init_page (void)
{
	/* Initialize hash table and lock for page table */
	struct thread *curr =thread_current ();
	hash_init (&curr->page_table, page_hash, page_hash_less, NULL);
	lock_init (&curr->page_lock);
}


/**
 * \page_install_page
 * \Install page_entry structure into page table.
 * \Note that this function immediately install physical memory (not lazy)
 *
 * \param upage virtual address of user process
 * \param kpage kernal virtual address (physical address) to be installed
 * \param writable indication of read only or not
 * \param flags palloc flags (to be used for swap in)
 * \param type  type of page (MEM, FILE, ...)
 *
 * \retval true on success
 * \retval fail on fail
 */
bool
page_install_page (void *upage, void *kpage, bool writable,
									 enum palloc_flags flags, enum page_type type)
{
	struct thread *curr = thread_current ();

	/* Allocate memory, return false if not successfully allocated */
	lock_acquire (&curr->page_lock);
	struct page_entry *pe = malloc (sizeof (struct page_entry));
	if (pe == NULL)
	{
		lock_release (&curr->page_lock);
		return false;
	}

	/* Write user page address */
	pe->vaddr = upage;

	/* Write type and flags */
	pe->type = type;
	pe->flags = flags;

	/* Write protection bit */
	pe->writable = writable;

	/* As this function load page not lazily, set is_loaded value to true */
	pe->is_loaded = true;

	/* Insert page entry to sup page table */
	hash_insert (&curr->page_table, &pe->elem);

	/* Add address mapping to original page table.
	 * Install page is implemented in userprog/process.c */
	bool result = install_page (upage, kpage, writable);
	lock_release (&curr->page_lock);

	/* Return result of install_page */
	return result;
}


/**
 * \page_get_entry
 * \Find and return page entry that corresponding to
 * \user virtual address with corresponding sup page table
 * \(Synchronization is managed by vm.c)
 *
 * \param table supplemental page table
 * \param vaddr virtual address to find entry
 *
 * \retval page_entry if found
 * \retval NULL if not found
 */
struct page_entry *
page_get_entry (struct hash *table, void *vaddr)
{
	struct page_entry spte;

	spte.vaddr = vaddr;
	struct hash_elem *e = hash_find (table, &spte.elem);

	return e != NULL ? hash_entry (e, struct page_entry, elem) : NULL;
}


/**
 * \page_delete_entry
 * \Delete page table entry from table
 *
 * \param table sup page table
 * \param spte  supplemental page entry to be deleted
 *
 * \retval void
 */
void
page_delete_entry (struct hash *table, struct page_entry *spte)
{
	hash_delete (table, &spte->elem);
	free (spte);
}


/**
 * \page_destroy_table
 * \Destroy sup page table, just call hash_destroy with action
 *
 * \param table sup page table to be destroyed
 *
 * \retval void
 */
void
page_destroy_table (struct hash *table)
{
	hash_destroy (table, page_destroy_action);
}


/**
 * \page_load_lazy
 * \Add supplemental page lazily, not load file immediately
 *
 * \param   table corresponding sup page table
 * \param   file  file to load lazily
 * \param   ofs   offset of file to be read
 * \param   vaddr user virtual address of current context
 * \param   read_bytes  bytes to be read from offset
 * \param   zero_bytes  bytes of zero padding
 * \param   writable    indicates that this region is writable or not
 * \param   enum page_type  determine this region is mmap or file
 *
 * \retval  supplemental page table entry if success
 * \retval  Null if failed
 */
struct page_entry *
page_load_lazy (struct hash *table, struct file *file, off_t ofs,
                void *vaddr, uint32_t read_bytes, uint32_t zero_bytes,
                bool writable, enum page_type type)
{
	/* Allocate page entry */
	struct page_entry *spte = malloc (sizeof (struct page_entry));
	if (spte == NULL)
		return NULL;

	/* Set status of this page entry */
	spte->vaddr = vaddr;
	spte->type = type;
	/* Set this page is not in memory */
	spte->is_loaded = false;
	spte->flags = PAL_USER;
	spte->file = file;
	spte->ofs = ofs;
	spte->read_bytes = read_bytes;
	spte->zero_bytes = zero_bytes;
	spte->writable = writable;

	/* Insert entry into supplemetal page table */
	hash_insert (table, &spte->elem);
	return spte;
}


/**
 * \page_load_demand
 * \Load page on demand (call only fault occurred)
 *
 * \param   spte  Supplemental page table that fault occurred
 * \param   paddr physical address to be mapped
 *
 * \retval  true  if success
 * \retval  false if failed
 */
bool
page_load_demand (struct page_entry *spte, void *paddr)
{
	/* Read read_bytes from offset written in spte */
	if (file_read_at (spte->file, paddr, spte->read_bytes, spte->ofs)
	    != (int) spte->read_bytes)
		return false;

	/* Add zero padding */
	memset (paddr + spte->read_bytes, 0, spte->zero_bytes);

	/* Set this page is loaded in memory */
	spte->is_loaded = true;

	/* Install vaddr and paddr mapping into pagedir */
	return install_page (spte->vaddr, paddr, spte->writable);
}

/**
 * \page_munmap
 * \unmap the page
 *
 * \param   table supplemental page table
 * \param   spte  supplemental page table entry
 * \param   pagedir pagedirectory of corresponding process
 *
 * \retval  void
 */
void
page_munmap (struct hash *table, struct page_entry *spte, void *pagedir)
{
	/* If page is loaded in memory, free it */
	if (spte->is_loaded)
	{
		void *paddr = pagedir_get_page (pagedir, spte->vaddr);
		/* If this page is dirty, write back to file */
		if (pagedir_is_dirty (pagedir, spte->vaddr))
		{
			file_write_at (spte->file, paddr, spte->read_bytes, spte->ofs);
		}
		palloc_free_page (paddr);
	}

	/* Delete entry and set vaddr regin to be not present */
	hash_delete (table, &spte->elem);
	pagedir_clear_page (pagedir, spte->vaddr);
	free (spte);
}



/***** hash function and less function for hash table initialization *****/

/**
 * \internal
 *
 * \page_hash
 * \Hash function for page entry
 * \Use user virtual address
 *
 * \param e hash element of page entry
 * \param aux auxiliary data (UNUSED)
 *
 * \retval  hash value
 */
static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED)
{
	struct page_entry *pe = hash_entry (e, struct page_entry, elem);
	return hash_bytes (&pe->vaddr, sizeof pe->vaddr);
}


/**
 * \internal
 *
 * \page_hash_less
 * \Compare two hash element of pate entry.
 * \Just naively compares their user virtual address
 *
 * \param  a_, b_  hash entry of page entry
 *
 * \retval true if a's hash value is less than b's one.
 * \retval false otherwise.
 */
static bool page_hash_less (const struct hash_elem *a_,
                            const struct hash_elem *b_,
                            void *aux UNUSED)
{
	struct page_entry *a = hash_entry (a_, struct page_entry, elem);
	struct page_entry *b = hash_entry (b_, struct page_entry, elem);
	return a->vaddr < b->vaddr;
}


/**
 * \internal
 *
 * \page_destroy_action
 * \Action for destroy hash table
 *
 * \param e hash entry of page entry
 * \param aux auxiliary data (UNUSED)
 *
 * \retval void
 */
static void
page_destroy_action (struct hash_elem *e, void *aux UNUSED)
{
	struct page_entry *pe = hash_entry (e, struct page_entry, elem);
	struct thread *t = thread_current ();
	void *paddr;

	/* If page is loaded in memory, call palloc_free */
	if (pe->is_loaded)
	{
		paddr = pagedir_get_page (t->pagedir, pe->vaddr);
    if (pe->type == MMAP)
    {
      list_remove (&pe->elem_mmap);
      if (pagedir_is_dirty (t->pagedir, pe->vaddr))
      {
        //lock_acquire (&filesys_lock)
        file_write_at (pe->file, paddr, pe->read_bytes, pe->ofs);
        //lock_release (&filesys_lock);
      }
    }
		struct frame_entry *fe = frame_get_entry (paddr);
		pagedir_clear_page (t->pagedir, pe->vaddr);
		palloc_free_page (fe->paddr);
		frame_free_page (fe);
		free (pe);
		return;
	}

	/* For each page type, remove resources */
	switch (pe->type)
	{
		case MEM:
			/* MEM case is all ways loaded in memory, impossible case */
			break;
		case DISK:
			/* If page is swapped out, delete corresponding swap block */
			swap_delete (pe->block_idx);
			break;
		case FILE:
			/* Nothing to do in unloaded file pages */
			break;
		case MMAP:
			break;
	}

	/* Free!! */
	free (pe);
}


