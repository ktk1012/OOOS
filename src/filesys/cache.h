#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"

/* Maximum numbers of entry in cache table (i.e cache size) */
#define CACHE_SIZE 64

/* Cache entry */
struct cache_entry
{
	char buffer[DISK_SECTOR_SIZE];    /* Saved data from disk */
	disk_sector_t idx;                /* Location of buffered item */
	bool is_dirty;                    /* Dirty check bit */
	bool is_accessed;                 /* Access check bit */
	bool is_valid;                    /* Valid check bit */
	uint64_t time;                    /* Last accessed time */
};

/* Functions for caching */

void cache_init (void);
void cache_done (void);
void cache_read (disk_sector_t idx, void *buffer);
void cache_write (disk_sector_t idx, const void *buffer);

#endif //FILESYS_CACHE_H
