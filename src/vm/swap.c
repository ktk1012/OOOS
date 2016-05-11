#include <stdio.h>
#include "swap.h"
/* Not swapped index is set false,
 * if in use set true */


static struct swap_table swap_table;


/**
 * \swap_init
 * \Initialize swap table
 *
 * \param void
 * \retval void
 */
void swap_init (void)
{
	/* Get swap disk */
	swap_table.swap_disk = disk_get (1, 1);

	/* Determine the number of pages to be swapped out.
	 * Note that disk size is 512 byte, but page is 4096 byte. */
	size_t num_pages = disk_size (swap_table.swap_disk) / 8;

	/* Create pool with num_pages */
	swap_table.swap_pool = bitmap_create (num_pages);

	/* Init lock */
	lock_init (&swap_table.swap_lock);
}


/**
 * \swap_write
 * \Write kpage to disk block
 *
 * \param kpage  kernal virtual page to be swapped out
 *
 * \retval index of swap_pool (location of saved disk)
 */
size_t swap_write (void *kpage)
{
	/* Find available swap block and set to unavailable */
	size_t swap_idx = bitmap_scan_and_flip (swap_table.swap_pool, 10, 1, false);

	/* If there is no block to write, PANIC :( */
	if (swap_idx == BITMAP_ERROR)
		PANIC ("Swap disk is full :(");

	/* As consecutive 8 block is 1 page, page is divided into 8 blocks
	 * and written into disk */
	int i;
	for (i = 0; i < 8; ++i)
	{
		disk_write (swap_table.swap_disk, 8 * swap_idx + i,
		            kpage + i * DISK_SECTOR_SIZE);
	}

	/* Return index of swap pool */
	return swap_idx;
}


/**
 * \swap_read
 * \Read the data from disk
 *
 * \param kpage destination for swap in
 *
 * \retval true if successful
 * \retval false if read fails
 */
bool swap_read (size_t idx, void *kpage)
{
	lock_acquire (&swap_table.swap_lock);
	if (!bitmap_test(swap_table.swap_pool, idx))
	{
		lock_release (&swap_table.swap_lock);
		return false;
	}

	/* Read 8 consecutive blocks into physical memory */
	int i;
	for (i = 0; i < 8; ++i)
	{
		disk_read (swap_table.swap_disk, 8 * idx + i,
		           kpage + i * DISK_SECTOR_SIZE);
	}

	/* Flip the bit located idx */
	bitmap_flip (swap_table.swap_pool, idx);

	lock_release (&swap_table.swap_lock);

	return true;
}


/**
 * \swap_delete
 * \Delete swap block, just flip the bit
 *
 * \param idx index to be deleted
 *
 * \retval  void
 */
void swap_delete (size_t idx)
{
	bitmap_flip (swap_table.swap_pool, idx);
}
