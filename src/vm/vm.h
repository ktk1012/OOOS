#ifndef VM_VM_H
#define VM_VM_H
#include "threads/palloc.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"


void vm_init (void);
void vm_init_page (void);

bool vm_load (void *fault_addr, void *esp);

void *vm_get_page (enum palloc_flags flags, void *vaddr);
void vm_free_page (void *paddr);
bool vm_install_page (void *upage, void *kpage, bool writable, bool lazy,
                      enum palloc_flags flags, enum page_type type);

void vm_destroy_page_table (struct hash *table);

bool vm_load_lazy (struct file *file, off_t ofs, void *vaddr,
								   uint32_t read_bytes, uint32_t zero_bytes, bool writable);
#endif //VM_VM_H
