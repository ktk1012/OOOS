#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "threads/thread.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"



/* Parse given path, and return file name and containing directory */
struct dir *
dir_open_path (const char *path)
{
  char dir_copy[strlen (path) + 1];
  /* If path end with '/', strip it */
  if (path[strlen(path)] == '/')
    strlcpy (dir_copy, path, strlen (path));
  else
    strlcpy (dir_copy, path, strlen (path) + 1);

  char *token, *save_ptr, *path_next;
  struct dir *dir;
  struct inode *inode_temp;

  /* Find cwd */
  struct dir *cwd = thread_current ()->cwd;
  if (cwd)
    cwd = dir_reopen (cwd);
  else
    cwd = dir_open_root ();

  /* If path start with '/', open root directory, else open relatively */
  if (dir_copy[0] == '/')
  {
    dir = dir_open_root ();
    /* Make relative path to root directory */
    strlcpy (dir_copy, path + 1, strlen (path));
  }
  else
    dir = dir_reopen (cwd);

  path_next = strtok_r (dir_copy, "/", &save_ptr);

  for (token = strtok_r (NULL, "/", &save_ptr); token != NULL;)
  {
    //printf ("path_next: %s\n", path_next);
    if (strlen (path_next) == 0)
      continue;

    if (!strcmp (path_next, "./"))
      continue;

    if (!strcmp (path_next, "../"))
    {
      // Open parent directory
    }


    if (!dir_lookup (dir, path_next, &inode_temp) ||
        !inode_is_dir (inode_temp))
    {
      dir_close (dir);
      inode_close (inode_temp);
      return NULL;
    }
    dir_close (dir);
    dir = dir_open (inode_temp);
    path_next = token;
    token = strtok_r (NULL, "/", &save_ptr);
  }

  return dir;
}

char *
dir_parse_name (const char *path)
{
  char *token, *save_ptr, *f_name = NULL;
  char path_copy[strlen (path) + 1];
  if (path[strlen(path)] == '/')
  {
    strlcpy (path_copy, path, strlen (path));
  }
  else
    strlcpy (path_copy, path, strlen (path) + 1);

  for (token = strtok_r (path_copy, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
    f_name = token;
  char *name = malloc (strlen (f_name) + 1);
  if (!name)
    return NULL;
  else
    strlcpy (name, f_name, strlen (f_name) + 1);
  return name;
}

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (disk_sector_t sector, size_t entry_cnt, struct dir *parent)
{
  disk_sector_t parent_sector;
  if (parent)
    parent_sector = inode_get_parent (dir_get_inode(parent));
  else
    parent_sector = sector;   /* Root directory */

  return inode_create (sector, entry_cnt * sizeof (struct dir_entry),
                       true, parent_sector);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  //printf ("lookup: %s\n", name);
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) {
//    printf ("e: %s, %d\n", e.name, e.in_use ? 1 : 0);
    if (e.in_use && !strcmp(name, e.name)) {
      if (ep != NULL)
        *ep = e;
      if (ofsp != NULL)
        *ofsp = ofs;
      return true;
    }
  }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, disk_sector_t inode_sector) 
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* If inode is directory and not empry, reject it */
  if (inode_is_dir (inode))
  {
    struct dir *temp = dir_open (inode);
    if (!dir_is_empty (temp))
    {
      dir_close (temp);
      goto done;
    }
    dir_close (temp);
  }

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  return false;
}

/* Initialization */
void dir_init (void)
{
  thread_current ()->cwd = dir_open_root ();
}

bool
dir_is_empty (struct dir *dir)
{
  struct dir_entry e;
  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e)
  {
    dir->pos += sizeof e;
    if (e.in_use)
      return false;
  }
  return true;
}
