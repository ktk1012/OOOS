#include <stdio.h>
#include "swap.h"

/* Not swapped index is set false,
 * if in use set true */

static struct swap_table swap_table;

void swap_init (void)
{
	swap_table.swap_disk = disk_get (1, 1);    /* Get swap disk structure */
	size_t num_pages = disk_size (swap_table.swap_disk) / 8;
	swap_table.swap_pool = bitmap_create (num_pages);
	lock_init (&swap_table.swap_lock);
}

size_t swap_write (void *kpage)
{
	size_t swap_idx = bitmap_scan_and_flip (swap_table.swap_pool, 10, 1, false);

	if (swap_idx == BITMAP_ERROR)
		PANIC ("Swap disk is full :(");

	int i;
	for (i = 0; i < 8; ++i)
	{
		disk_write (swap_table.swap_disk, 8 * swap_idx + i,
		            kpage + i * DISK_SECTOR_SIZE);
	}

	return swap_idx;
}

bool swap_read (size_t idx, void *kpage)
{
	if (!bitmap_test(swap_table.swap_pool, idx))
	{
		lock_release (&swap_table.swap_lock);
		return false;
	}

	int i;
	for (i = 0; i < 8; ++i)
	{
		disk_read (swap_table.swap_disk, 8 * idx + i,
		           kpage + i * DISK_SECTOR_SIZE);
	}

	/* Flip the bit located idx */
	bitmap_flip (swap_table.swap_pool, idx);

	return true;
}

void swap_delete (size_t idx)
{
	bitmap_flip (swap_table.swap_pool, idx);
}
