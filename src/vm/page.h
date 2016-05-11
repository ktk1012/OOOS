#ifndef VM_PAGE_H
#define VM_PAGE_H
#include <hash.h>
#include "threads/palloc.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "filesys/file.h"
#include "filesys/filesys.h"

enum page_type
{
	MEM,
	DISK,
	FILE
};

struct page_entry
{
	void *vaddr;
	enum page_type type;

	bool is_loaded;
	bool writable;

	enum palloc_flags flags;

	/* For swapped item */
	size_t block_idx;

	/* For lazy loading for file */
	struct file *file;
	off_t ofs;
	uint32_t read_bytes;
	uint32_t zero_bytes;

	struct hash_elem elem;
};

void page_init_page (void);
bool page_install_page (void *upage, void *kpage, bool writable, bool lazy,
                        enum palloc_flags flags, enum page_type type);
struct page_entry *page_get_entry (struct hash *table, void *vaddr);
void page_delete_entry (struct hash *table, struct page_entry *spte);

void page_destroy_table (struct hash *table);

bool page_load_lazy (struct hash *table, struct file *file, off_t ofs,
                     void *vaddr, uint32_t read_bytes, uint32_t zero_bytes,
                     bool writable);

bool page_load_demand (struct page_entry *spte, void *paddr);

#endif //VM_PAGE_H
