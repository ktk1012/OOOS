#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

static void syscall_handler (struct intr_frame *);
static int syscall_write (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  //printf ("system call!\n");
  int syscall_num = *(int *)(f->esp);
  switch (*(int *) f->esp)
    {
      case SYS_EXIT:
        thread_exit (0);
        break;

      case SYS_WRITE:
        syscall_write (f);
        break;

      default:
        thread_exit (0);
    }
}


static int
syscall_write (struct intr_frame *f)
{
  int fd = *(int *) (f->esp + 4);
  char *buf = *(char **) (f->esp + 8);
  unsigned len = *(unsigned *) (f->esp + 12);
  if (fd == STDOUT_FILENO)
    {
      putbuf (buf, (size_t) len);
      return len;
    }
  return -1;
}
