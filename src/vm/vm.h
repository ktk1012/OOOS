#ifndef VM_VM_H
#define VM_VM_H
#include <user/syscall.h>
#include <list.h>
#include "threads/palloc.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"


struct mmap_entry
{
	struct file *file;
	mapid_t mid;
	struct list map_list;
	struct list fd_list;
	struct list_elem elem;
};


void vm_init (void);
void vm_init_page (void);

bool vm_load (void *fault_addr, void *esp);

void *vm_get_page (enum palloc_flags flags, void *vaddr);
void vm_free_page (void *paddr);
bool vm_install_page (void *upage, void *kpage, bool writable,
                      enum palloc_flags flags, enum page_type type);

void vm_destroy_page_table (struct hash *table);

bool vm_load_lazy (struct file *file, off_t ofs, void *vaddr,
								   uint32_t read_bytes, uint32_t zero_bytes, bool writable);

struct mmap_entry *vm_add_mmap (struct file *file,
                                void *start_addr,
                                size_t file_size);
void vm_munmap (struct mmap_entry *me);
#endif //VM_VM_H
