#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "devices/disk.h"
#include "threads/thread.h"
#include "threads/malloc.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  filesys_disk = disk_get (0, 1);
  if (filesys_disk == NULL)
    PANIC ("hd0:1 (hdb) not present, file system initialization failed");

  inode_init ();
  free_map_init ();
  /* Initialize cache */
  cache_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  /* Clear cache */
  cache_done ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, bool is_dir)
{
  disk_sector_t inode_sector = 0;
  /* Open file's containing directory and its file name */
  struct dir *dir = dir_open_path (name);
  char *file_name = dir_parse_name (name);
  bool success = false;

  /* Check impossible cases */
  if (file_name == NULL || !strlen (file_name)
      || !strcmp (file_name, ".") || !strcmp (file_name, ".."))
  {
    success = false;
    goto done;
  }

  /* Check directory is removed */
  if (inode_isremoved (dir->inode))
  {
    success =false;
    goto done;
  }

  disk_sector_t parent_sector = 0;
  /* If create is directory, remember the patent's inode number */
  if (is_dir)
    parent_sector = inode_get_inumber (dir->inode);

  success = (dir != NULL
             && free_map_allocate (1, &inode_sector)
             && inode_create (inode_sector, initial_size, is_dir, parent_sector)
             && dir_add (dir, file_name, inode_sector));


  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);

  /* If directory, add '.' and '..' */
  if (success && inode_sector != 0 && is_dir)
  {
    struct dir *dir_own = dir_open (inode_open (inode_sector));
    dir_add (dir_own, ".", inode_sector);
    dir_add (dir_own, "..", parent_sector);
    dir_close (dir_own);
  }

done:
  dir_close (dir);
  free (file_name);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir = dir_open_path (name);
  char *file_name = dir_parse_name (name);
  struct inode *inode = NULL;

  if (dir != NULL)
  {
    /* Check directory is removed */
    if (inode_isremoved (dir->inode))
    {
      dir_close (dir);
      free (file_name);
      return NULL;
    }
    dir_lookup (dir, file_name, &inode);
  }

  dir_close (dir);
  free (file_name);

  return file_open (inode);
}

bool
filesys_chdir (const char *name)
{
  struct dir *dir = dir_open_path (name);
  char *file_name = dir_parse_name (name);
  struct inode *inode = NULL;
  bool success = false;

  if (dir != NULL)
    dir_lookup (dir, file_name, &inode);

  if (inode == NULL)
  {
    dir_close (dir);
    return success;
  }

  /* Check inode is directory */
  if (inode_is_dir (inode))
  {
    thread_current ()->cwd = inode_get_inumber (inode);
    success = true;
  }

  dir_close (dir);
  free (file_name);
  return success;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir = dir_open_path (name);
  char *file_name = dir_parse_name (name);

  bool success = dir != NULL && dir_remove (dir, file_name);
  dir_close (dir);
  free (file_name);

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  /* Open root directory and add '.' and '..' entry */
  struct dir *root_dir = dir_open (inode_open (ROOT_DIR_SECTOR));
  dir_add (root_dir, ".", ROOT_DIR_SECTOR);
  dir_add (root_dir, "..", ROOT_DIR_SECTOR);
  free_map_close ();
  printf ("done.\n");
}
