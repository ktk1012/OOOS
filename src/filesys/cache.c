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

struct cache
{
	/* Cache entries */
	struct cache_entry cache_block[CACHE_SIZE];
	struct bitmap *free_map;  /* Bitmap of available cache blocks */
	int aval_size;            /* Available cache block size */
	int curr;                 /* Pointer to current block */
};

static struct lock cache_lock;
static struct cache cache;

/* Find cache block */
static struct cache_entry *get_block (disk_sector_t idx);
/* Eviction */
static struct cache_entry *evict_block (void);
/* Refresher */
static void cache_refresh (void *aux UNUSED);


void
cache_init (void)
{
	lock_init(&cache_lock);
	cache.aval_size = 64;
	cache.curr = 0;
	cache.free_map = bitmap_create (CACHE_SIZE);
	memset(cache.cache_block, 0, CACHE_SIZE * sizeof(struct cache_entry));
	struct semaphore sem;
	sema_init (&sem, 0);
	thread_create ("refresh", PRI_MIN, cache_refresh, (void *)&sem);
	sema_down (&sem);
}


void
cache_done (void)
{
	lock_acquire (&cache_lock);
	bitmap_destroy (cache.free_map);
	int i;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		struct cache_entry *temp = &cache.cache_block[i];
		if (temp->is_valid && temp->is_dirty)
			disk_write (filesys_disk, temp->idx, temp->buffer);
	}
	lock_release (&cache_lock);
}

void cache_read (disk_sector_t idx, void *buffer)
{
	lock_acquire (&cache_lock);
	struct cache_entry *temp = get_block (idx);
	if (temp->is_valid)
		memcpy (buffer, temp->buffer, DISK_SECTOR_SIZE);
	else
	{
		disk_read (filesys_disk, idx, temp->buffer);
		memcpy (buffer, temp->buffer, DISK_SECTOR_SIZE);
	}
	temp->is_valid = true;
	temp->is_accessed = true;
	lock_release (&cache_lock);
}

void cache_write (disk_sector_t idx, const void *buffer)
{
	lock_acquire (&cache_lock);
	struct cache_entry *temp = get_block (idx);
	if (temp->is_valid)
	{
		memcpy(temp->buffer, buffer, DISK_SECTOR_SIZE);
		temp->is_dirty = true;
	}
	else
	{
		disk_write(filesys_disk, idx, buffer);
		memcpy (temp->buffer, buffer, DISK_SECTOR_SIZE);
		temp->is_dirty = false;
	}
	temp->is_valid = true;
	temp->is_accessed = true;
	lock_release (&cache_lock);
}

static struct cache_entry *
get_block (disk_sector_t idx)
{
	int i;
	struct cache_entry *temp = NULL;
	for (i = 0; i < CACHE_SIZE; ++i)
	{
		/* If there is no entry (all blocks are free) break */
		if (cache.aval_size == CACHE_SIZE)
			break;
		/* Find corresponding entry which is valid */
		temp = &cache.cache_block[i];
		if (temp->is_valid && temp->idx == idx)
			return temp;
	}

	/* Not found case */
	/* If there is available block */
	if (cache.aval_size != 0)
	{
		size_t entry = bitmap_scan_and_flip(cache.free_map, 0, 1, false);
		temp = &cache.cache_block[entry];
	}
	/* If not eviction occurs */
	else
		temp = evict_block ();

	/* Decrement available block size */
	--cache.aval_size;
	temp->idx = idx;

	/* Initialization */
	// temp->is_valid = true;
	// temp->is_accessed = false;
	// temp->is_dirty = false;
	/* If zero filled creation, not request disk immediately */
	// if (is_create)
		// memset (temp->buffer, 0, DISK_SECTOR_SIZE);
	// else
		// disk_read (filesys_disk, temp->idx, temp->buffer);

	return temp;
}

static struct cache_entry *
evict_block (void)
{
	bool is_find = false;
	struct cache_entry *temp = NULL;
	while (1)
	{
		for (; cache.curr < CACHE_SIZE; ++cache.curr)
		{
			temp = &cache.cache_block[cache.curr];

			if (!temp->is_valid)
				continue;

			if (!temp->is_accessed)
			{
				is_find = true;
				break;
			}
			else
				temp->is_accessed = false;
		}
		if (is_find)
			break;
		cache.curr = 0;
	}
	ASSERT (temp != NULL);

	/* Find victim block, if block is dirty write back */
	if (temp->is_dirty)
		disk_write (filesys_disk, temp->idx, temp->buffer);
	temp->is_valid = false;
	temp->is_dirty = false;
	bitmap_set (cache.free_map, cache.curr, false);
	temp->idx = -1;
	++cache.aval_size;
	return temp;
}

static void
cache_refresh (void *aux UNUSED)
{
	lock_acquire (&cache_lock);
	struct seamphore *sem = aux;
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
	sema_up ((struct semaphore *) sem);
	lock_release (&cache_lock);
	timer_sleep (10 * TIMER_FREQ);
}
