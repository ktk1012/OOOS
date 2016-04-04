#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/init.h"
#include "userprog/process.h"
#include "filesys/file.h"
#include "filesys/filesys.h"

static struct lock filesys_lock;
static struct lock exec_lock;

static void syscall_handler (struct intr_frame *);
static int get_user (uint8_t *uaddr);
static bool check_arguments (void *esp, int bytes);
static bool check_buffer (void *buf, int size);
static bool check_str (char *arg, int size);

static bool
check_str (char *arg, int size)
{
  return check_arguments ((void *) arg, size);
}

static bool
check_buffer (void *buf, int size)
{
  return check_arguments ((uint8_t *) buf, 1) 
         && check_arguments ((uint8_t *)buf + size, 1);
}

/*****************************************************/
static void syscall_exit (struct intr_frame *f);
static int syscall_wait (struct intr_frame *f);
static int syscall_exec (struct intr_frame *f);
static int syscall_create (struct intr_frame *f, int *status);
static int syscall_remove (struct intr_frame *f, int *status);
static int syscall_open (struct intr_frame *f, int *status);
static int syscall_filesize (struct intr_frame *f);
static int syscall_read (struct intr_frame *f, int *status);
static int syscall_write (struct intr_frame *f, int *status);
static int syscall_seek (struct intr_frame *f);
static int syscall_tell (struct intr_frame *f);
static int syscall_close (struct intr_frame *f);
/***************************************************/

static int syscall_wait (struct intr_frame *f)
{
  tid_t child_tid = *(tid_t *) (f->esp + 4);
  //printf ("%s want to wait %d\n", thread_name (), child_tid);
  lock_acquire (&exec_lock);
  //printf ("wait: %d\n", child_tid);
  int result = process_wait (child_tid);
  //printf ("wait done: %d\n", child_tid);
  lock_release (&exec_lock);
  return result;
}

static int syscall_exec (struct intr_frame *f)
{
  char *cmd = *(char **) (f->esp + 4);
  int result;
  if (cmd == NULL || !check_str (cmd, PGSIZE))
    return -1;
  if (strlen (cmd) > PGSIZE)
    return -1;
  lock_acquire (&exec_lock);
  //printf ("%s execute %s\n", thread_name (), cmd);
  result = process_execute ((const char *) cmd);
  //printf ("%s execute %s done\n", thread_name (), cmd);
  lock_release (&exec_lock);
  //printf ("%s released lock\n", thread_name ());
  return result;
}


static int syscall_create (struct intr_frame *f, int *status)
{
  char *file = *(char **) (f->esp + 4);
  unsigned initial_size = *(unsigned *) (f->esp + 8);
  if (file == NULL || !check_str (file, 14))
    {
      *status = -1;
      return 0;
    }
  if (strlen (file) > 14)
    return 0;
  return filesys_create (file, initial_size);
}

static int syscall_remove (struct intr_frame *f, int *status)
{
  char *file = *(char **) (f->esp + 4);
  if (file == NULL || !check_str (file, 14))
    {
      *status = -1;
      return 0;
    }
  if (strlen (file) > 14)
    return 0;
  return filesys_remove (file);
}

static void syscall_exit (struct intr_frame *f)
{
  int status = *(int *) (f->esp + 4);
  thread_exit (status);
}

static int syscall_open (struct intr_frame *f, int *status)
{
  char *file_name = *(char **) (f->esp + 4);
  if (file_name == NULL || !check_str (file_name, 14))
    {
      *status = -1;
      return -1;
    }
  if (strlen (file_name) > 14)
    return -1;
  return process_open (file_name);
}

static int syscall_filesize (struct intr_frame *f)
{
  int fd = *(int *) (f->esp + 4);
  return process_filesize (fd);
}

static int syscall_read (struct intr_frame *f, int *status)
{
  int fd = *(int *) (f->esp + 4);
  void *buf = *(void **) (f->esp + 8);
  unsigned size = *(unsigned *) (f->esp + 12);
  if (!check_buffer ((uint8_t *) buf, size))
    {
      *status = -1;
      return -1;
    }
  return process_read (fd, buf, size);
}

static int syscall_write (struct intr_frame *f, int *status)
{
  int fd = *(int *) (f->esp + 4);
  void *buf = *(void **) (f->esp + 8);
  unsigned size = *(unsigned *) (f->esp + 12);
  if (!check_str ((uint8_t *)buf, size))
    {
      *status = -1;
      return -1;
    }
  return process_write (fd, buf, size);
}

static int syscall_seek (struct intr_frame *f)
{
  int fd = *(int *) (f->esp + 4);
  unsigned pos = *(unsigned *) (f->esp + 8);
  return process_seek (fd, pos);
}

static int syscall_tell (struct intr_frame *f)
{
  int fd = *(int *) (f->esp + 4);
  return process_tell (fd);
}

static int syscall_close (struct intr_frame *f)
{
  int fd = *(int *) (f->esp + 4);
  return process_close (fd);
}

static int get_user (uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

static bool check_arguments (void *esp, int bytes)
{
  int i;
  for (i = 0; i < bytes; i++)
  {
    if (get_user ((uint8_t *) (esp + i)) == -1 || 
        !is_user_vaddr (esp + i))
      return false;
  }
  return true;
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
  lock_init (&exec_lock);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int result = -1;
  int return_status = 0;
  if (!check_arguments (f->esp, 4))
    {
      thread_exit (-1);
    }
  switch (*(int *) f->esp)
    {
      case SYS_HALT:
        power_off ();
        break;

      case SYS_EXIT:
        if (!check_arguments (f->esp + 4, 4))
          goto bad_arg;
        syscall_exit (f);
        break;

      case SYS_EXEC:
        if (!check_arguments (f->esp + 4, 4))
          goto bad_arg;
        result = syscall_exec (f);
        break;

      case SYS_WAIT:
        if (!check_arguments (f->esp + 4, 4))
          goto bad_arg;
        result = syscall_wait (f);
        break;

      case SYS_CREATE:
        if (!check_arguments (f->esp + 4, 8))
          goto bad_arg;
        lock_acquire (&filesys_lock);
        result = syscall_create (f, &return_status);
        lock_release (&filesys_lock);
        break;

      case SYS_REMOVE:
        if (!check_arguments (f->esp + 4, 4))
          goto bad_arg;
        lock_acquire (&filesys_lock);
        result = syscall_remove (f, &return_status);
        lock_release (&filesys_lock);
        break;

      case SYS_OPEN:
        if (!check_arguments (f->esp + 4, 4))
          goto bad_arg;
        lock_acquire (&filesys_lock);
        result = syscall_open (f, &return_status);
        lock_release (&filesys_lock);
        break;

      case SYS_FILESIZE:
        if (!check_arguments (f->esp + 4, 4))
          goto bad_arg;
        lock_acquire (&filesys_lock);
        result = syscall_filesize (f);
        lock_release (&filesys_lock);
        break;

      case SYS_READ:
        if (!check_arguments (f->esp + 4, 12))
          goto bad_arg;
        lock_acquire (&filesys_lock);
        result = syscall_read (f, &return_status);
        lock_release (&filesys_lock);
        break;

      case SYS_WRITE:
        if (!check_arguments (f->esp + 4, 12))
            goto bad_arg;
        lock_acquire (&filesys_lock);
        result = syscall_write (f, &return_status);
        lock_release (&filesys_lock);
        break;

      case SYS_SEEK:
        if (!check_arguments (f->esp + 4, 8))
          goto bad_arg;
        lock_acquire (&filesys_lock);
        result = syscall_seek (f);
        lock_release (&filesys_lock);
        break;

      case SYS_TELL:
        if (!check_arguments (f->esp + 4, 8))
          goto bad_arg;
        lock_acquire (&filesys_lock);
        result = syscall_tell (f);
        lock_release (&filesys_lock);
        break;

      case SYS_CLOSE:
        if (!check_arguments (f->esp + 4, 4))
          goto bad_arg;
        lock_acquire (&filesys_lock);
        result = syscall_close (f);
        lock_release (&filesys_lock);
        break;

    }
  if (return_status == -1)
    thread_exit (-1);
  f->eax = result;
  return;
bad_arg:
  thread_exit (-1);
  return;
}

