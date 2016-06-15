#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"
#include "threads/synch.h"


/* Maximum numbers of entry in cache table (i.e cache size) */
#define CACHE_SIZE 64

/* Cache entry */
struct cache_entry
{
	char buffer[DISK_SECTOR_SIZE];    /* Saved data from disk */
	disk_sector_t idx;                /* Location of buffered item */
	bool is_dirty;                    /* Dirty check bit */
	bool is_valid;                    /* Valid check bit */
	uint64_t time;                    /* Last accessed time */
	struct lock block_lock;           /* per entry lock */
	bool is_victim;                   /* This block to be evicted */
};

/* Functions for caching */

void cache_init (void);
void cache_done (void);
void cache_read (disk_sector_t idx, void *buffer, off_t ofs, size_t read_bytes);
void cache_write (disk_sector_t idx, const void *buffer,
                  off_t ofs, size_t write_bytes);

void cache_read_ahead_append (disk_sector_t idx);

// void cache_remove (disk_sector_t idx);

#endif //FILESYS_CACHE_H
