#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"

struct bitmap;

void inode_init (void);
bool inode_create (disk_sector_t, off_t, bool is_dir, disk_sector_t parent);
struct inode *inode_open (disk_sector_t);
struct inode *inode_reopen (struct inode *);
disk_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (struct inode *);
bool inode_is_dir (struct inode *inode);
bool inode_isroot (struct inode *inode);
bool inode_isremoved (struct inode *inode);
void inode_dir_rdlock (struct inode *inode);
void inode_dir_rdunlock (struct inode *inode);
void inode_dir_wrlock (struct inode *inode);
void inode_dir_wrunlock (struct inode *inode);

disk_sector_t inode_get_parent (struct inode *inode);

#endif /* filesys/inode.h */
