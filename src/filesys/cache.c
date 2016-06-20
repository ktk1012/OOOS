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

struct cache
{
	/* Cache entries */
	struct cache_entry cache_block[CACHE_SIZE];
	struct bitmap *free_map;  /* Bitmap of available cache blocks */
	int aval_size;            /* Available cache block size */

};

static struct rw_lock cache_lock;
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

static void cache_read_ahead_demon (void *aux UNUSED);
static void cache_add (disk_sector_t idx);


void
cache_init (void)
{
	rw_init (&cache_lock);
	cache.aval_size = 64;
	cache.free_map = bitmap_create (CACHE_SIZE);
	/* Initialize all cache block */
	int i;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		struct cache_entry *temp = cache.cache_block + i;
		memset (temp->buffer, 0, DISK_SECTOR_SIZE);
		temp->idx = -1;
		temp->is_dirty = false;
		temp->is_valid = false;
		temp->time = 0;
		temp->is_victim = false;
		rw_init (&temp->rwl);
	}
	time_stamp = 0;

	/* Make new thread for periodically refresh the dirty cache blocks */
	list_init (&read_ahead_list);
	lock_init (&lock_read_ahead);
	cond_init (&cond_read_ahead);
	struct semaphore sem;
	sema_init (&sem, 0);
	thread_create ("refresh", PRI_DEFAULT, cache_periodic_refresh, (void *)&sem);
	sema_down (&sem);
	/* Initialize read ahead demon */
	thread_create ("read_ahead", PRI_DEFAULT, cache_read_ahead_demon, (void *)&sem);
	sema_down (&sem);
}


void
cache_done (void)
{
	bitmap_destroy (cache.free_map);
  /* Clear all resources */
	int i;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		struct cache_entry *temp = &cache.cache_block[i];
		if (temp->is_valid && temp->is_dirty)
		{
			disk_write (filesys_disk, temp->idx, temp->buffer);
			temp->is_dirty = false;
		}
	}
}


void
cache_read (disk_sector_t idx, void *buffer, off_t ofs, size_t read_bytes)
{
	/* Get cache block entry */
	struct cache_entry *temp = get_block (idx);
	/* Acquire individual lock in cache block entry */
	rw_rd_lock (&temp->rwl);

	/* If block is not valid, read from disk to buffer cache */
	if (!temp->is_valid)
		disk_read (filesys_disk, idx, temp->buffer);

	memcpy (buffer, temp->buffer + ofs, read_bytes);
	temp->is_valid = true;

	/* Update accessed time stamp */
	temp->time = time_stamp++;
	rw_rd_unlock (&temp->rwl);
}

void
cache_write (disk_sector_t idx, const void *buffer,
             off_t ofs, size_t write_bytes)
{
	/* Get block entry */
	struct cache_entry *temp = get_block (idx);
	rw_wr_lock (&temp->rwl);
	/* Acquire individual lock */

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
	rw_wr_unlock (&temp->rwl);
}

static void
cache_add (disk_sector_t idx)
{
	struct cache_entry *temp = get_block (idx);
	rw_rd_lock (&temp->rwl);
	/* Read from disk if invalid */

	if (!temp->is_valid)
	{
		disk_read(filesys_disk, idx, temp->buffer);
		temp->is_dirty = false;
	}

	temp->is_valid = true;
	temp->time = time_stamp++;
	rw_rd_unlock (&temp->rwl);
}

static struct cache_entry *
get_block (disk_sector_t idx)
{
	/* Find corresponding block entry */
	int i;
	struct cache_entry *temp = NULL;
	uint64_t lru_min = -1;
	int lru_min_idx = 0;
	rw_rd_lock (&cache_lock);
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		/* If there is no entry (all blocks are free) break */
		if (cache.aval_size == CACHE_SIZE)
			break;
		/* Find corresponding entry which is valid */
		temp = &cache.cache_block[i];
		if (temp->is_valid && !temp->is_victim)
		{
			/* If index of block is matched, return it */
			if (temp->idx == idx)
      {
        rw_rd_unlock (&cache_lock);
				return temp;
      }
			/* Otherwise, check this block is candidate for eviction */
			if (lru_min > temp->time)
			{
				lru_min = temp->time;
				lru_min_idx = i;
			}
		}
	}
  rw_rd_unlock (&cache_lock);
  rw_wr_lock (&cache_lock);

	/* Not found case */
	/* If there is available block */
	if (cache.aval_size != 0)
	{
		size_t entry = bitmap_scan_and_flip(cache.free_map, 0, 1, false);
		temp = &cache.cache_block[entry];
		--cache.aval_size;
	}
	/* If not eviction occurs */
	else
	{
		/* Get least recent used block */
		temp = &cache.cache_block[lru_min_idx];
		temp->is_victim = true;
		rw_evict_lock (&temp->rwl);
		/* If victim block is dirty, write back */
		if (temp->is_dirty)
			disk_write (filesys_disk, temp->idx, temp->buffer);
		rw_evict_unlock (&temp->rwl);
	}


	/* Decrement available block size */

	/* As block is changed, wait for previous block works to be done */

	/* Set index of block sector */
	temp->idx = idx;

	temp->is_valid = false;
	temp->is_dirty = false;
	temp->is_victim = false;


	/* Release the cache lock */
  rw_wr_unlock (&cache_lock);

	return temp;
}

static void
cache_refresh (void)
{
	int i;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		struct cache_entry *temp = &cache.cache_block[i];
		bool status = rw_wr_lock (&temp->rwl);
		if (status)
		{
			if (!temp->is_victim && temp->is_valid && temp->is_dirty) {
				disk_write(filesys_disk, temp->idx, temp->buffer);
				temp->is_dirty = false;
			}
			rw_wr_unlock(&temp->rwl);
		}
	}
}

static void
cache_periodic_refresh (void *aux UNUSED)
{
	struct semaphore *sem = aux;
	sema_up ((struct semaphore *)sem);
	while (1)
	{
		timer_usleep (10000);
		// printf ("periodic !\n");
    // lock_acquire (&cache_lock);
    rw_rd_lock (&cache_lock);
		cache_refresh ();
    rw_rd_unlock (&cache_lock);
    // lock_release (&cache_lock);
	}
}

struct ahead_entry
{
	disk_sector_t idx;
	struct list_elem elem;
};


static void
cache_read_ahead_demon (void *aux UNUSED)
{
	struct semaphore *sem = aux;
	sema_up ((struct semaphore *)sem);
	while (1)
	{
		timer_usleep (10000);
		// printf ("read_ahead demon!!\n");
		/* sleep for some periodic times */
		lock_acquire (&lock_read_ahead);

		while (list_empty (&read_ahead_list))
			cond_wait (&cond_read_ahead, &lock_read_ahead);

		lock_release (&lock_read_ahead);

		while (!list_empty (&read_ahead_list))
		{
			struct list_elem *e;
      lock_acquire (&lock_read_ahead);
			e = list_pop_front (&read_ahead_list);
      lock_release (&lock_read_ahead);
			struct ahead_entry *temp = list_entry (e, struct ahead_entry, elem);
			cache_add (temp->idx);
			free (temp);
		}
	}
}

void cache_read_ahead_append (disk_sector_t idx)
{
	if (!idx)
		return;

	lock_acquire (&lock_read_ahead);

	/* Allocate new wait entry */
	struct ahead_entry *wait_entry = malloc (sizeof (struct ahead_entry));

	if (wait_entry == NULL)
		return;

	wait_entry->idx = idx;
	list_push_back (&read_ahead_list, &wait_entry->elem);
	cond_signal (&cond_read_ahead, &lock_read_ahead);
	lock_release (&lock_read_ahead);
}
