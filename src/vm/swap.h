#ifndef VM_SWAP_H
#define VM_SWAP_H
#include <bitmap.h>
#include "threads/synch.h"
#include "devices/disk.h"


/**
 * \swap_table
 * \Table for swap disk management
 */
struct swap_table
{
	struct disk *swap_disk;   /* Indicate swap disk */
	struct bitmap *swap_pool; /* Indicate available block */
	struct lock swap_lock;    /* Lock for pool synchronization */
};


void swap_init (void);
bool swap_read (size_t idx, void *kapge);
size_t swap_write (void *kpage);
void swap_delete (size_t idx);


#endif
