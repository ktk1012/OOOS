#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include <user/syscall.h>

/* Open file entry */
struct fd_entry
  {
    struct file * file;
    int fd;
    struct list_elem elem;
    bool is_mmaped;              /* Indicates mmapped or not */
    struct list_elem elem_mmap;  /* Element in mmap entry */
  };

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (int);
void process_activate (void);

int process_open (const char *file);
int process_filesize (int fd);
int process_read (int fd, void *buffer, unsigned size);
int process_write (int fd, void *buffer, unsigned size);
int process_seek (int fd, unsigned position);
int process_tell (int fd);
int process_close (int fd);
int process_mmap (int fd, void *addr);
int process_munmap (mapid_t mid);

/* Load helpers */
bool install_page (void *upage, void *kpage, bool writable);

#endif /* userprog/process.h */
