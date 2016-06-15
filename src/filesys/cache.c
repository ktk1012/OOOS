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

static struct lock cache_lock;
static struct cache cache;


/* Find cache block */
static struct cache_entry *get_block (disk_sector_t idx);
/* Eviction */
static struct cache_entry *evict_block (void);
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
	lock_init(&cache_lock);
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
		lock_init (&temp->block_lock);
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
		if (!temp->is_victim && temp->is_valid && temp->is_dirty)
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
  lock_acquire (&cache_lock);
	struct cache_entry *temp = get_block (idx);
	lock_release (&cache_lock);
	/* Acquire individual lock in cache block entry */
	// lock_acquire (&temp->block_lock);

	/* If block is not valid, read from disk to buffer cache */
	if (!temp->is_valid)
		disk_read (filesys_disk, idx, temp->buffer);

	memcpy (buffer, temp->buffer + ofs, read_bytes);
	temp->is_valid = true;

	/* Update accessed time stamp */
	temp->time = time_stamp++;
	lock_release (&temp->block_lock);
}

void
cache_write (disk_sector_t idx, const void *buffer,
             off_t ofs, size_t write_bytes)
{
	/* Get block entry */
  lock_acquire (&cache_lock); 
	struct cache_entry *temp = get_block (idx);
	lock_release (&cache_lock);
	/* Acquire individual lock */
	// lock_acquire (&temp->block_lock);

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
	lock_release (&temp->block_lock);
}

static void
cache_add (disk_sector_t idx)
{
	lock_acquire (&cache_lock);
	struct cache_entry *temp = get_block (idx);
	lock_release (&cache_lock);
	/* Read from disk if invalid */

	if (!temp->is_valid)
	{
		disk_read(filesys_disk, idx, temp->buffer);
		temp->is_dirty = false;
	}

	temp->is_valid = true;
	temp->time = time_stamp++;
	lock_release (&temp->block_lock);
}

static struct cache_entry *
get_block (disk_sector_t idx)
{
	/* Find corresponding block entry */
	int i;
	struct cache_entry *temp = NULL;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		/* If there is no entry (all blocks are free) break */
		if (cache.aval_size == CACHE_SIZE)
			break;
		/* Find corresponding entry which is valid */
		temp = &cache.cache_block[i];
		lock_acquire (&temp->block_lock);
		if (temp->is_valid && temp->idx == idx && !temp->is_victim)
		{
			return temp;
		}
		lock_release (&temp->block_lock);
	}

	/* Not found case */
	/* If there is available block */
	if (cache.aval_size != 0)
	{
		size_t entry = bitmap_scan_and_flip(cache.free_map, 0, 1, false);
		temp = &cache.cache_block[entry];
		lock_acquire (&temp->block_lock);
	}
	/* If not eviction occurs */
	else
		temp = evict_block ();

	/* Decrement available block size */
	--cache.aval_size;

	/* As block is changed, wait for previous block works to be done */
	// lock_acquire (&temp->block_lock);

	/* Set index of block sector */
	temp->idx = idx;


	/* Release the cache lock */
	// lock_release (&temp->block_lock);
	return temp;
}

static struct cache_entry *
evict_block (void)
{
	struct cache_entry *temp = NULL;
	struct cache_entry *lru_min = &cache.cache_block[0];
	lock_acquire (&lru_min->block_lock);
	lru_min->is_victim = true;
	int idx_min = 0;

	/* Find lru min */
	int i;
	for (i = 1; i < CACHE_SIZE; ++i)
	{
		temp = &cache.cache_block[i];
		lock_acquire (&temp->block_lock);
		if (lru_min->time < temp->time && !temp->is_victim)
		{
			lru_min->is_victim = false;
			lock_release (&lru_min->block_lock);
			idx_min = i;
			lru_min = temp;
			lru_min->is_victim = true;
			// lock_acquire (&lru_min->block_lock);
			continue;
		}
		lock_release (&temp->block_lock);
	}
	temp = lru_min;
	ASSERT (temp != NULL);

	/* Wait block to be idle (as content of block to be changed) */
	// lock_acquire (&temp->block_lock);

	/* Find victim block, if block is dirty write back */
	if (temp->is_dirty)
		disk_write (filesys_disk, temp->idx, temp->buffer);

	temp->is_valid = false;
	temp->is_dirty = false;
  temp->is_victim = false;

	bitmap_set (cache.free_map, idx_min, false);
	++cache.aval_size;

	/* Set invalid index */
	temp->idx = -1;

	// lock_release (&temp->block_lock);
	return temp;
}

static void
cache_refresh (void)
{
	int i;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		struct cache_entry *temp = &cache.cache_block[i];
    lock_acquire (&temp->block_lock);
		if (!temp->is_victim && temp->is_valid && temp->is_dirty)
		{
			disk_write (filesys_disk, temp->idx, temp->buffer);
			temp->is_dirty = false;
		}
    lock_release (&temp->block_lock);
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
    lock_acquire (&cache_lock);
		cache_refresh ();
    lock_release (&cache_lock);
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
      lock_acquire (&lock_read_ahead);
			struct list_elem *e;
      lock_release (&lock_read_ahead);
			e = list_pop_front (&read_ahead_list);
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
