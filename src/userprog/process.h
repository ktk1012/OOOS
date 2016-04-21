#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

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

#endif /* userprog/process.h */
