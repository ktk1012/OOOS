#include <string.h>
#include <bitmap.h>
#include "devices/timer.h"
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* For debugging */
#include <stdio.h>

static uint64_t time_stamp;  /* Time stamp for LRU eviction */

/* Cache structures that contains cache blocks */
struct cache
{
	/* Cache entries */
	struct cache_entry cache_block[CACHE_SIZE];
	struct bitmap *free_map;  /* Bitmap of available cache blocks */
	int aval_size;            /* Available cache block size */

};

/* lock for cache_lock */
static struct lock cache_lock;

/* Cache */
static struct cache cache;


/* Find cache block */
static struct cache_entry *get_block (disk_sector_t idx);
/* Flush all dirty blocks */
static void cache_refresh (void);
/* Periodically flushes all dirty blocks */
static void cache_periodic_refresh (void *aux UNUSED);

/* For read ahead code */
static struct list read_ahead_list;

/* Condition variable and lock for waiting read ahead structure */
static struct condition cond_read_ahead;
static struct lock lock_read_ahead;

/* Read ahead demon, it asynchronously check read ahead request */
static void cache_read_ahead_demon (void *aux UNUSED);
/* Add data in sector 'idx' into cache */
static void cache_add (disk_sector_t idx);


/*
 * \cache_init
 * \initialize cache and each cache block when file system starts up
 *
 * \param void
 *
 * \retval void
 */
void
cache_init (void)
{
	/* Initialize lock for cache */
	lock_init(&cache_lock);

	/* Initialize available cache block size and bitmap */
	cache.aval_size = CACHE_SIZE;
	cache.free_map = bitmap_create (CACHE_SIZE);

	/* Initialize all cache block */
	int i;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		struct cache_entry *temp = cache.cache_block + i;
		/* Clear buffer to 0 */
		memset (temp->buffer, 0, DISK_SECTOR_SIZE);
		/* Set invalid index to block */
		temp->idx = -1;

		/* Initialize all informations about cache blocks */
		temp->is_dirty = false;
		temp->is_valid = false;
		temp->time = 0;
		temp->is_victim = false;

		/* Initialize r/w lock for each block */
		rw_init (&temp->rwl);
	}
	time_stamp = 0;

	struct semaphore sem;
	sema_init (&sem, 0);

	/* Make new thread for periodically refresh the dirty cache blocks */
	thread_create ("refresh", PRI_DEFAULT, cache_periodic_refresh, (void *)&sem);
	sema_down (&sem);

	/* Initialize read_ahead data structures */
	list_init (&read_ahead_list);
	lock_init (&lock_read_ahead);
	cond_init (&cond_read_ahead);

	/* Initialize read ahead demon */
	thread_create ("read_ahead", PRI_DEFAULT, cache_read_ahead_demon, (void *)&sem);
	sema_down (&sem);
}


/*
 * \cache_done
 * \Clear all cache block when filesystem is done.
 *
 * \param void
 *
 * \retval void
 */
void
cache_done (void)
{
	bitmap_destroy (cache.free_map);
  /* Clear all resources */
	int i;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		struct cache_entry *temp = &cache.cache_block[i];
		/* If blok is valid, write it back to disk */
		if (!temp->is_victim && temp->is_valid && temp->is_dirty)
		{
			disk_write (filesys_disk, temp->idx, temp->buffer);
			temp->is_dirty = false;
		}
	}
}


/*
 * \cache_read
 * \Read data with sector number 'idx' to buffer.
 *
 * \param idx index of block to read
 * \param buffer buffer for read data
 * \param ofs offset from start of block sector (0~DISK_SECTOR_SIZE)
 * \param read_byte bytes to read from the data sector
 *
 * \retval void
 */
void
cache_read (disk_sector_t idx, void *buffer, off_t ofs, size_t read_bytes)
{
	/* Get cache block entry */
  lock_acquire (&cache_lock);
	struct cache_entry *temp = get_block (idx);
	/* Acquire individual lock in cache block entry */
	rw_rd_lock (&temp->rwl);
	lock_release (&cache_lock);

	/* If block is not valid, read from disk to buffer cache */
	if (!temp->is_valid)
		disk_read (filesys_disk, idx, temp->buffer);

	memcpy (buffer, temp->buffer + ofs, read_bytes);
	temp->is_valid = true;

	/* Update accessed time stamp */
	temp->time = time_stamp++;
	rw_rd_unlock (&temp->rwl);
}


/*
 * \cache_write
 * \Write content in buffer to cache
 *
 * \param idx index of disk sector
 * \param buffer content to be written
 * \param ofs offset to write
 * \param write_bytes bytes to be written to cache
 *
 * \retval void
 */
void
cache_write (disk_sector_t idx, const void *buffer,
             off_t ofs, size_t write_bytes)
{
	/* Get block entry */
  lock_acquire (&cache_lock); 
	struct cache_entry *temp = get_block (idx);
	/* Acquire individual lock */
	rw_wr_lock (&temp->rwl);
	lock_release (&cache_lock);

	/* If block is not valid, read from disk to buffer cache */
	if (!temp->is_valid)
		disk_read (filesys_disk, idx, temp->buffer);

	/* Copy to the cache block */
	memcpy (temp->buffer + ofs, buffer, write_bytes);

	/* Set dirty bit and valid bit, and update lru time */
	temp->is_dirty = true;
	temp->is_valid = true;

	/* Update accessed time stamp */
	temp->time = time_stamp++;

	/* Release lock */
	rw_wr_unlock (&temp->rwl);
}


/*
 * \Internal
 * \cache_add
 * \Caching the data in block 'idx'
 *
 * \param idx Setor number to caching
 *
 * \retval void
 */
static void
cache_add (disk_sector_t idx)
{
	/* Get block and get read lock */
	lock_acquire (&cache_lock);
	struct cache_entry *temp = get_block (idx);
	rw_rd_lock (&temp->rwl);
	lock_release (&cache_lock);

	/* Read from disk if invalid */
	if (!temp->is_valid)
	{
		disk_read(filesys_disk, idx, temp->buffer);
		temp->is_dirty = false;
	}

	/* Set block to be valid and increment time stamp */
	temp->is_valid = true;
	temp->time = time_stamp++;
	rw_rd_unlock (&temp->rwl);
}


/*
 * \Internal
 * \get_block
 * \Get cache block for given idx, if cache is full, evict
 * \with LRU policy
 *
 * \param idx block index for new cache block
 *
 * \retval Pointer of cache entry
 */
static struct cache_entry *
get_block (disk_sector_t idx)
{
	/* Find corresponding block entry */
	int i;
	struct cache_entry *temp = NULL;
	/* Check least recent used time */
	uint64_t lru_min = -1;
	/* Index of least recent used block */
	int lru_min_idx = 0;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		/* If there is no entry (all blocks are free) break */
		if (cache.aval_size == CACHE_SIZE)
			break;
		/* Find corresponding entry which is valid */
		temp = &cache.cache_block[i];
		/* If block is valid and not to be evicted */
		if (temp->is_valid && !temp->is_victim)
		{
			/* If index of block is matched, return it */
			if (temp->idx == idx)
				return temp;
			/* Otherwise, check this block is candidate for eviction */
			if (lru_min > temp->time)
			{
				lru_min = temp->time;
				lru_min_idx = i;
			}
		}
	}

	/* Not found case */
	/* If there is available block */
	if (cache.aval_size != 0)
	{
		size_t entry = bitmap_scan_and_flip(cache.free_map, 0, 1, false);
		temp = &cache.cache_block[entry];
		--cache.aval_size;
	}
	/* If there is no available block -> evict! */
	else
	{
		/* Get least recent used block */
		temp = &cache.cache_block[lru_min_idx];
		/* Set this block to be evict */
		temp->is_victim = true;
		/* Wait until previous request is done */
		rw_evict_lock (&temp->rwl);
		/* If victim block is dirty, write back */
		if (temp->is_dirty)
			disk_write (filesys_disk, temp->idx, temp->buffer);
		/* Unlock evict lock */
		rw_evict_unlock (&temp->rwl);
	}


	/* Set index of block sector */
	temp->idx = idx;

	/* Initialize informations about cache block */
	temp->is_valid = false;
	temp->is_dirty = false;
	temp->is_victim = false;


	/* Release the cache lock */
	return temp;
}


/*
 * \Internal
 * \cache_refresh
 * \refresh the cache block
 *
 * \param void
 * \retval void
 */
static void
cache_refresh (void)
{
	int i;
	/* Traverse all cache block */
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		struct cache_entry *temp = &cache.cache_block[i];
		/* Try to acquire i-th cache block */
		bool status = rw_wr_lock (&temp->rwl);
		/* If lock acquired */
		if (status)
		{
			/* Check block validity and write back to disk if dirty */
			if (!temp->is_victim && temp->is_valid && temp->is_dirty) {
				disk_write(filesys_disk, temp->idx, temp->buffer);
				temp->is_dirty = false;
			}
			/* Unlock the writers lock */
			rw_wr_unlock(&temp->rwl);
		}
	}
}


/*
 * \Internal
 * \cache_periodic_refresh
 * \Periodically refresh the cache block
 *
 * \param aux semaphore for thead synch
 *
 * \retval void
 */
static void
cache_periodic_refresh (void *aux UNUSED)
{
	struct semaphore *sem = aux;
	sema_up ((struct semaphore *)sem);
	/* This is refresh demon, run until program done. */
	while (1)
	{
		/* Sleep for a while */
		timer_usleep (10000);

		/* Refresh the cache */
    lock_acquire (&cache_lock);
		cache_refresh ();
    lock_release (&cache_lock);
	}
}


/* Structures for read ahead requests */
struct ahead_entry
{
	disk_sector_t idx;
	struct list_elem elem;
};


/*
 * \Internal
 * \cache_read_ahead_demon
 * \Demon thread for read ahead
 *
 * \param aux semaphore for thread creation synch
 *
 * \retval void
 */
static void
cache_read_ahead_demon (void *aux UNUSED)
{
	struct semaphore *sem = aux;
	sema_up ((struct semaphore *)sem);
	while (1)
	{
		/* sleep for some periodic times */
		timer_usleep (10000);
		lock_acquire (&lock_read_ahead);

		/* Wait until list is filled */
		while (list_empty (&read_ahead_list))
			cond_wait (&cond_read_ahead, &lock_read_ahead);
		lock_release (&lock_read_ahead);

		/* Perform the all read ahead requests */
		while (!list_empty (&read_ahead_list))
		{
			struct list_elem *e;
      lock_acquire (&lock_read_ahead);
			e = list_pop_front (&read_ahead_list);
      lock_release (&lock_read_ahead);
			struct ahead_entry *temp = list_entry (e, struct ahead_entry, elem);
			/* Add cache block */
			cache_add (temp->idx);
			free (temp);
		}
	}
}


/*
 * \cache_read_ahead_append
 * \Append new read ahead request and dispatch the read ahead demon
 *
 * \param idx index for disk sector to be added
 *
 * \retval void
 */
void cache_read_ahead_append (disk_sector_t idx)
{
	if (!idx)
		return;

	/* Acquire read ahead lock */
	lock_acquire (&lock_read_ahead);

	/* Allocate new wait entry */
	struct ahead_entry *wait_entry = malloc (sizeof (struct ahead_entry));

	if (wait_entry == NULL)
		return;

	wait_entry->idx = idx;
	/* Push entry into read ahead list and dispatch the demon */
	list_push_back (&read_ahead_list, &wait_entry->elem);
	cond_signal (&cond_read_ahead, &lock_read_ahead);
	lock_release (&lock_read_ahead);
}
