#ifndef VM_SWAP_H
#define VM_SWAP_H
#include <bitmap.h>
#include "threads/synch.h"
#include "devices/disk.h"

#define SWAP_ERROR -1

/* Swap table structure */
struct swap_table
{
	struct disk *swap_disk;
	struct bitmap *swap_pool;
	struct lock swap_lock;
};

void swap_init (void);
bool swap_read (size_t idx, void *kapge);
size_t swap_write (void *kpage);
void swap_delete (size_t idx);

#endif
