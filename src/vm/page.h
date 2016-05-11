#ifndef VM_PAGE_H
#define VM_PAGE_H
#include <hash.h>
#include "threads/palloc.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "filesys/file.h"
#include "filesys/filesys.h"


/**
 * \page_type
 * \Indicate type of memeory page
 */
enum page_type
{
	MEM,
	DISK,
	FILE
};


/**
 * \page_entry
 * \page entry for supplemental page table
 */
struct page_entry
{
	void *vaddr;         /* User virtual address */
	enum page_type type; /* Type of page */

	bool is_loaded;      /* Indicate that data is loaded or not */
	bool writable;       /* Indicate that this page is writable or not */

	/* Flags for palloc */
	enum palloc_flags flags;

	/* For swapped item */
	size_t block_idx;    /* Indicate the location of swap disk, if swapped out */

	/* For lazy loading for file */
	struct file *file;
	off_t ofs;
	uint32_t read_bytes;
	uint32_t zero_bytes;

	/* Hash element for supplemental page table */
	struct hash_elem elem;
};


void page_init_page (void);
bool page_install_page (void *upage, void *kpage, bool writable,
                        enum palloc_flags flags, enum page_type type);
struct page_entry *page_get_entry (struct hash *table, void *vaddr);
void page_delete_entry (struct hash *table, struct page_entry *spte);

void page_destroy_table (struct hash *table);

bool page_load_lazy (struct hash *table, struct file *file, off_t ofs,
                     void *vaddr, uint32_t read_bytes, uint32_t zero_bytes,
                     bool writable);

bool page_load_demand (struct page_entry *spte, void *paddr);

#endif //VM_PAGE_H
