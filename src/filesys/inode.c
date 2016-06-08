#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "threads/synch.h"

/* For debugging */
#include <stdio.h>

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Number of direct block */
#define DIRECT_BLOCK_CNT 120

/* Number of block in indirect block */
#define INDIRECT_CNT 128

/* On-disk inode.
   Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    disk_sector_t direct_idx[DIRECT_BLOCK_CNT];      /* Direct block list */
    disk_sector_t indirect_idx;         /* Indirect block pointer */
    disk_sector_t db_indirect_idx;      /* Doubly indirect block pointer */

    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    bool is_dir;                    /* Is directory or not */
    /* If inode is directory, indicate parent directory's sector */
    disk_sector_t parent;
    uint32_t unused[1];                 /* Reserved region */
    uint8_t dummy[3];                   /* Reserved region */
  };

/* On-disk indirect block,
 * Initially, each entry is 0 (invalid),
 * Otherwise, indicate the block index */
struct indirect_block
{
  disk_sector_t sector[INDIRECT_CNT];
};


/* enum type of index type */
enum idx_type
{
  DIRECT,
  INDIRECT,
  DOUBLE_INDIRECT
};

/* Sector index structure
 * To use for indicate direct / indirect/ doubly indirect block */
struct idx_entry
{
  enum idx_type type;
  /* First hierarchy index of block
   * (index of direct index, index of indirect index,
   * index pointer for doubly indirect index) */
  disk_sector_t first_idx;
  /* Second hierarchy index of block
   * (index of block of first index of indirect block,
   * index of doubly indirect block) */
  disk_sector_t second_idx;
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    disk_sector_t sector;               /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
    struct lock inode_lock;             /* Inode lock */
  };



/* Returns the disk sector that contains index offset idx within
 * inode_disk */
static disk_sector_t
idx_to_sector (const struct inode_disk *inode_disk, size_t idx)
{
  /* First, check index is in direct block region */
  if (idx < DIRECT_BLOCK_CNT)
    return inode_disk->direct_idx[idx];
  else if (idx < DIRECT_BLOCK_CNT + INDIRECT_CNT)
  {
    /* Second, check index is in indirect block region */
    /* Read indirect block from disk */
    idx -= DIRECT_BLOCK_CNT;
    /* Read index position in cache block */
    disk_sector_t result;
    cache_read (inode_disk->indirect_idx, &result, idx * 4, 4);
    return result;
  }
  else
  {
    /* Last, indirect block case, first, count the first index
     * of indirect block and count offset in second indirect block */
    idx -= DIRECT_BLOCK_CNT + INDIRECT_CNT;
    disk_sector_t indirect_idx = idx / INDIRECT_CNT;
    disk_sector_t indirect_ofs = idx % INDIRECT_CNT;
    /* Read first indirect block and, corresponding second block */
    disk_sector_t temp, result;
    cache_read (inode_disk->db_indirect_idx, &temp, indirect_idx * 4, 4);
    cache_read (temp, &result, indirect_ofs * 4, 4);
    return result;
  }
}


/* Returns the disk sector that contains byte offset POS within
   INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (0 <= pos && pos < inode->data.length)
  {
    size_t idx = pos / DISK_SECTOR_SIZE;
    return idx_to_sector (&inode->data, idx);
  }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

static off_t inode_set_length (struct inode *inode, off_t size);

/* internal functions for handling indexed file structure */
static bool inode_idxed_create (struct inode_disk *disk_inode);
static void inode_idxed_remove (struct inode_disk *disk_inode, size_t size);
static bool inode_extend (struct inode_disk *disk_inode, size_t size);


/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   disk.
   Returns true if successful.
   Returns false if memory or disk allocation fails.
   is_dir indicate creation is directory creation,
   with parent directory, if parent is 0 it means root dir or
   regular file */
bool
inode_create (disk_sector_t sector, off_t length,
              bool is_dir, disk_sector_t parent)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      /* Set parent's inode if it is directory */
      if (is_dir)
        disk_inode->parent = parent;
      if (inode_idxed_create (disk_inode))
        {
          // disk_write (filesys_disk, sector, disk_inode);
          cache_write (sector, disk_inode, 0, DISK_SECTOR_SIZE);
          success = true;
        } 
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) 
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->inode_lock);
  cache_read (inode->sector, &inode->data, 0, DISK_SECTOR_SIZE);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          /* free_map_release (inode->data.start,
                            bytes_to_sectors (inode->data.length)); */
          inode_idxed_remove (&inode->data, inode->data.length);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  off_t len = inode_length (inode);

  while (size > 0)
  {
      /* Disk sector to read, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = len - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Read data from buffer cache */
      cache_read (sector_idx, buffer + bytes_read, sector_ofs, chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
  }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  off_t len = inode_length (inode);

  if (inode->deny_write_cnt)
    return 0;

  /* If size + offset is larger than original size, extend it */
  if (size + offset > inode->data.length)
  {
    inode_extend (&inode->data, offset + size);
    len = inode_set_length (inode, offset + size);
    cache_write (inode->sector, &inode->data, 0, DISK_SECTOR_SIZE);
  }

  while (size > 0)
  {
      /* Sector to write, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = len - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Write buffer to buffer cache */
      cache_write (sector_idx, buffer + bytes_written,
                   sector_ofs, chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
  }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (struct inode *inode)
{
  lock_acquire (&inode->inode_lock);
  off_t size = inode->data.length;
  lock_release (&inode->inode_lock);
  return size;
}


/* Set the length and return, in bytes, of inode's data. */
static off_t
inode_set_length (struct inode *inode, off_t size)
{
  lock_acquire (&inode->inode_lock);
  inode->data.length = size;
  lock_release (&inode->inode_lock);
  return size;
}


static bool
inode_idxed_create (struct inode_disk *disk_inode)
{
  size_t size = disk_inode->length;
  return inode_extend (disk_inode, size);
}


static void
inode_idxed_remove (struct inode_disk *disk_inode, size_t size)
{
  /* Take lareger on between size and inode_disk's length */
  size = size < (size_t) disk_inode->length ?
         size : (size_t) disk_inode->length;
  size_t cnt = bytes_to_sectors (size);
  /* Check actual allocation sectors in direct block */
  size_t alloc_block_cnt = cnt < DIRECT_BLOCK_CNT ? cnt : DIRECT_BLOCK_CNT;

  /* Index for for-loop */
  size_t i, j;

  for (i = 0; i < alloc_block_cnt; ++i)
  {
    /* If corresponding region is allocated, release it */
    if (disk_inode->direct_idx[i] == 0)
      free_map_release (disk_inode->direct_idx[i], 1);
    --cnt;
  }
  if (!cnt)
    return;

  /* Indirect block case */

  /* Check actual releasing sectors in indirect block */
  alloc_block_cnt = cnt < INDIRECT_CNT ? cnt : INDIRECT_CNT;

  struct indirect_block temp;

  /* Check indirect block is allocated or not */
  if (disk_inode->indirect_idx == 0)
    return;

  cache_read (disk_inode->indirect_idx, &temp, 0, DISK_SECTOR_SIZE);
  for (i = 0; i < alloc_block_cnt; ++i)
  {
    if (temp.sector[i] != 0)
      free_map_release (temp.sector[i], 1);
    --cnt;
  }

  /* Release indirect block */
  free_map_release (disk_inode->indirect_idx, 1);

  if (!cnt)
    return;

  /* Doubly indirect block case,
   * now remains cnt is same as actual release size */

  size_t indirect_idx = cnt / DISK_SECTOR_SIZE;
  size_t indirect_ofs = cnt % DISK_SECTOR_SIZE;

  /* Check doubly indirect block is allocated or not */
  if (disk_inode->db_indirect_idx == 0)
    return;

  cache_read (disk_inode->db_indirect_idx, &temp, 0, DISK_SECTOR_SIZE);

  for (i = 0; i <= indirect_idx; ++i)
  {
    struct indirect_block temp2;
    if (temp.sector[i] == 0)
      return;

    cache_read (temp.sector[i], &temp2, 0, DISK_SECTOR_SIZE);


    size_t indirect_alloc_cnt = i == indirect_idx ?
                                indirect_ofs : INDIRECT_CNT;

    for (j = 0; j < indirect_alloc_cnt; ++j)
    {
      if (temp2.sector[j] != 0)
        free_map_release (temp.sector[j], 1);
    }

    /* Release indirect block */
    free_map_release (temp.sector[i], 1);
  }


  /* release doubly indrect block */
  free_map_release (disk_inode->db_indirect_idx, 1);

  return;
}


/* Extend file with given size, if size is less than original size,
 * just return true.
 * Extension is just scan the block index and check if corresponding
 * index is 0 (which means not allocated yet), allocate it.
 * If anyway fail, return false */
static bool
inode_extend (struct inode_disk *disk_inode, size_t size)
{
  /* Zero filled buffer */
  static char zeros[DISK_SECTOR_SIZE];
  size_t cnt = bytes_to_sectors (size);
  // size_t cnt_prev = bytes_to_sectors (disk_inode->length);
  /* Check actual allocation sectors in direct block */

  size_t alloc_block_cnt = cnt <= DIRECT_BLOCK_CNT ? cnt : DIRECT_BLOCK_CNT;

  /* Index for for-loop */
  size_t i, j;
  for (i = 0; i < alloc_block_cnt; ++i)
  {
    /* If corresponding region is not allocated, allocate it */
    if (disk_inode->direct_idx[i] == 0)
    {
      /* Allocation failed */
      if (!free_map_allocate (1, &disk_inode->direct_idx[i]))
      {
        // inode_idxed_remove (disk_inode, size);
        return false;
      }
      cache_write (disk_inode->direct_idx[i], zeros, 0, DISK_SECTOR_SIZE);
    }
    --cnt;
  }
  if (!cnt)
    return true;

  /* Indirect block case */

  /* Check actual allocation sectors in indirect block */
  alloc_block_cnt = cnt <= INDIRECT_CNT ? cnt : INDIRECT_CNT;
  struct indirect_block temp;
  /* Check indirect block is allocated or not */
  if (disk_inode->indirect_idx == 0)
  {
    if (!free_map_allocate (1, &disk_inode->indirect_idx))
    {
      // inode_idxed_remove (disk_inode, size);
      return false;
    }
    cache_write (disk_inode->indirect_idx, zeros, 0, DISK_SECTOR_SIZE);
  }
  cache_read (disk_inode->indirect_idx, &temp, 0, DISK_SECTOR_SIZE);
  for (i = 0; i < alloc_block_cnt; ++i)
  {
    if (temp.sector[i] == 0)
    {
      if (!free_map_allocate (1, &temp.sector[i]))
      {
        return false;
      }
      cache_write (temp.sector[i], zeros, 0, DISK_SECTOR_SIZE);
    }
    --cnt;
  }
  /* Update indirect block */
  cache_write (disk_inode->indirect_idx, &temp, 0, DISK_SECTOR_SIZE);
  if (!cnt)
    return true;

  /* Doubly indirect block case,
   * now remains cnt is same as actual allocation size */

  size_t indirect_idx = cnt / INDIRECT_CNT;
  size_t indirect_ofs = cnt % INDIRECT_CNT;


  /* Check doubly indirect block is allocated or not */
  if (disk_inode->db_indirect_idx == 0)
  {
    if (!free_map_allocate (1, &disk_inode->db_indirect_idx))
    {
      // inode_idxed_remove (disk_inode, size);
      return false;
    }
    cache_write (disk_inode->db_indirect_idx, zeros, 0, DISK_SECTOR_SIZE);
  }
  cache_read (disk_inode->db_indirect_idx, &temp, 0, DISK_SECTOR_SIZE);

  /* Loop for inner tail (fit in INDIRECT_CNT) */
  for (i = 0; i <= indirect_idx; ++i)
  {
    struct indirect_block temp2;
    if (temp.sector[i] == 0)
    {
      if (!free_map_allocate (1, &temp.sector[i]))
      {
        return false;
      }
      cache_write (temp.sector[i], zeros, 0, DISK_SECTOR_SIZE);
    }
    cache_read (temp.sector[i], &temp2, 0, DISK_SECTOR_SIZE);

    size_t indirect_alloc_cnt = i == indirect_idx ?
                                indirect_ofs : INDIRECT_CNT;

    for (j = 0; j < indirect_alloc_cnt; ++j)
    {
      if (temp2.sector[j] == 0)
      {
        if (!free_map_allocate (1, &temp2.sector[j]))
        {
          return false;
        }
        cache_write (temp2.sector[j], zeros, 0, DISK_SECTOR_SIZE);
      }
    }

    cache_write (temp.sector[i], &temp2, 0, DISK_SECTOR_SIZE);
  }

  /* Update doubly indrect block */
  cache_write (disk_inode->db_indirect_idx, &temp, 0, DISK_SECTOR_SIZE);

  return true;
}
