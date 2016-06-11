#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "devices/input.h"
#include "vm/vm.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

struct lock exit_lock;
struct lock filesys_lock;


/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;
  bool is_child_loaded = true;
  struct shared_status *st;
  void *args[3];


  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  st = malloc (sizeof (struct shared_status));
  st->parent = thread_tid ();
  sema_init (&st->synch, 0);
  st->exit_status = 0;
  st->is_child_exit = false;
  st->p_status = PARENT_RUNNING;
  list_push_back (&thread_current ()->list_child, &st->elem);
  args[0] = (void *) fn_copy;
  args[1] = (void *) st;
  args[2] = (void *) &is_child_loaded;


  /* Create a new thread to execute FILE_NAME. 
   * I set thread_name 'process' temporaly, It would set it's command name,
   * after loaded */
  tid = thread_create (fn_copy, PRI_DEFAULT, start_process, (void *) args);
  /* Wait for initialize child process */
  sema_down (&st->synch);
  if (tid == TID_ERROR)
    {
      list_remove (&st->elem);
      free (st);
      palloc_free_page (fn_copy); 
    }
  st->child = tid;
  return is_child_loaded ? tid : -1;
}

/* A thread function that loads a user process and makes it start
   running. */
static void
start_process (void *f_name)
{
  char *file_name = ((char **)f_name)[0];
  struct shared_status *st = ((struct shared_status **)f_name)[1];
  bool *is_load_success = ((bool **)f_name)[2];
  struct intr_frame if_;
  bool success;

  
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  /* Loads supplemental page table */
  vm_init_page ();

//  /* If inherit NULL open root */
//  if (thread_current ()->cwd == NULL)
//    dir_init ();

  success = load (file_name, &if_.eip, &if_.esp);
  *is_load_success = success;
  sema_up (&st->synch);

  /* If load failed, quit. */
  palloc_free_page (file_name);
  if (!success) 
    {
      thread_exit (-1);
    }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct thread *curr = thread_current ();
  struct shared_status *st = NULL;
  struct list_elem *e;
  int status;
  for (e = list_begin (&curr->list_child); e != list_end (&curr->list_child);
       e = list_next (e))
    {
      st = list_entry (e, struct shared_status, elem);
      if (child_tid == st->child)
        break;
    }
  if (st != NULL)
    {
      if (st->is_child_exit) 
        {
          status = st->exit_status;
          list_remove (&st->elem);
          free (st);
          return status;
        }
      else 
        {
          st->p_status = PARENT_WAITING;
          sema_down (&st->synch);
          status = st->exit_status;
          list_remove (&st->elem);
          free (st);
          return status;
        }
    }
  return -1;
}

/* Utility function that close all process's open files 
 * See further belows for implementation */
static void clear_resources (void);

/* Free the current process's resources. */
void
process_exit (int status)
{
  lock_acquire (&exit_lock);
  struct thread *curr = thread_current ();
  struct shared_status *st = curr->child_shared_status;
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = curr->pagedir;
  clear_resources ();
  vm_destroy_page_table (&curr->page_table);
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      curr->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);

    }
  printf ("%s: exit(%d)\n", thread_name (), status);
  /* Set shared_status field for wait synchronization */
  if (st->p_status == PARENT_WAITING)
  {
    st->exit_status = status;
    st->is_child_exit = true;
    sema_up (&st->synch);
  }
  else if (st->p_status == PARENT_RUNNING)
  {
    st->exit_status = status;
    st->is_child_exit = true;
  }
  else
  {
    free (st);
  }
  file_close (curr->excutable);
  lock_release (&exit_lock);
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, void **arg_);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  char *save_ptr;
  char *cmd;
  void *args[2];
  cmd = strtok_r ((char *)file_name, " ", &save_ptr);
  args[0] = cmd;
  args[1] = save_ptr;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, args))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  /* Set thread's name using (argv[0]) */
  strlcpy (t->name, file_name, sizeof t->name);

  if (success)
    {
      t->excutable = file;
      file_deny_write (file);
    }
  else
    file_close (file);
  return success;
}

/* load() helpers. */

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Do calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      /* uint8_t *kpage = vm_get_page (PAL_USER, upage);
      if (kpage == NULL)
        return false;
      */
      /* Load this page. */
      /* if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          vm_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);
      */

      if (!vm_load_lazy (file, ofs, upage, page_read_bytes, page_zero_bytes, writable)) {
        return false;
      }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      ofs += page_read_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp, void **arg_) 
{
  /****************   Memory layout   ***********************
   *          PHYS_BASE -> **************
   *          argv[][]     * 'string'   *
   *          argv[][]     * 'string'   *
   *          ....         * 'string'   *
   *                       * 'string..' *
   *      args_origin ->   * 'string..' *
   * Word_align & sentinel *  0 0 0 0   *
   *  And pushing argv & argc & fake return address ...
   **********************************************************/

  uint8_t *kpage;
  bool success = false;
  int i;                    /* arg_lenoral variable for for loop */
  int argc = 0;             /* argc value */
  size_t arg_len;           /* Length of each argument */            
  char *token;              /* Parsed string token */
  void *args_origin;        /* Origin point of argv pushing */

  kpage = vm_get_page (PAL_USER | PAL_ZERO, PHYS_BASE - PGSIZE);
  if (kpage != NULL) 
    {
      success = vm_install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true,
                                 PAL_USER | PAL_ZERO, MEM);
      if (success)
        *esp = PHYS_BASE;
      else
        {
          vm_free_page (kpage);
          return success;
        }
    }
  for (token = (char *)arg_[0]; token != NULL;
       token = strtok_r (NULL, " ", (char **)&arg_[1]))
    {
      arg_len = strlen (token) + 1;
      *esp -= arg_len;
      strlcpy ((char *) *esp, token, arg_len);
      argc++;
    }

  args_origin = *esp;       /* Save starting point of argument strings */


  /* Word alignment & Set null pointer sentinel */
  i = ((size_t) *esp % 4);
  *esp -= 4 + i;
  memset (*esp, 0, 4 + i);

  /* Push argv & free all argument elements */
  for (i = 0; i < argc; i++)
    {
      arg_len = strlen ((char *) args_origin) + 1;
      *esp -= 4;
      memcpy (*esp, &args_origin, sizeof (char *));
      args_origin += arg_len;
    }

  /* Push argv starting address */
  memcpy (*esp - 4, esp, 4);
  *esp -= 4;

  /* Push argc */
  *esp -= 4;
  memcpy (*esp, &argc, sizeof (int));

  /* Push fake return address */
  *esp -= 4;
  memset (*esp, 0, 4);

  /* For debugging */
  //hex_dump ((uintptr_t)*esp, 
  //          *esp, (int) ((size_t) PHYS_BASE - (size_t) *esp),
  //          true);

  /* Setup argument stack */
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}


/*************** For supporting system calls ******************/
static struct fd_entry *get_fd_entry (int fd);
static struct mmap_entry *get_mmap_entry (mapid_t mid);

static struct fd_entry * 
get_fd_entry (int fd)
{
  struct list_elem *e;
  struct thread *curr = thread_current ();
  for (e = list_begin (&curr->files); e != list_end (&curr->files);
       e = list_next (e))
    {
      struct fd_entry *fe = list_entry (e, struct fd_entry, elem);
      if (fe->fd == fd)
        return fe;
    }
  return NULL;
}

static void
clear_resources (void)
{
  struct thread *curr = thread_current ();
  struct fd_entry *fe;
  struct shared_status *st;
  struct list_elem *e;
  struct mmap_entry *me;
  while (!list_empty (&curr->mmap_list))
  {
    e = list_front (&curr->mmap_list);
    me = list_entry (e, struct mmap_entry, elem);
    // vm_munmap (me);
    list_remove (e);
    free (me);
  }
  while (!list_empty (&curr->files))
  {
    fe = list_entry (list_pop_front (&curr->files),
                     struct fd_entry, elem);
    if (fe->dir)
      dir_close (fe->dir);
    file_close (fe->file);
    free (fe);
  }

  for (e = list_begin (&curr->list_child); e != list_end (&curr->list_child);)
  {
    st = list_entry (e, struct shared_status, elem);
    e = list_next (e);
    if (st->is_child_exit)
    {
      free (st);
    }
    else
    {
      st->p_status = PARENT_EXITED;
    }
  }
}



int process_open (const char *file)
{
  struct file *f = filesys_open (file);
  struct thread *curr = thread_current ();
  if (f == NULL)
  {
    return -1;
  }
  struct fd_entry *fe = malloc (sizeof (struct fd_entry));
  fe->file = f;
  fe->fd = curr->fd_next++;
  if (file_isdir (f))
    fe->dir = dir_open (file_get_inode (f));
  else
    fe->dir = NULL;

  list_push_back (&curr->files, &fe->elem);
  return fe->fd;
}

int process_filesize (int fd)
{
  struct fd_entry *fe = get_fd_entry (fd);
  if (fe == NULL)
    return -1;

  return file_length (fe->file);
}

int process_read (int fd, void *buffer, unsigned size)
{
  if (fd == STDIN_FILENO)
    {
      uint8_t c;
      unsigned cnt = size;
      uint8_t *buf = buffer;
      while (cnt > 1 && (c = input_getc ()) != 0)
        {
          *buf = c;
          buffer++;
          cnt--;
        }
      *buf = 0;
      return size - cnt;
    }
  struct fd_entry *fe = get_fd_entry (fd);
  if (fe == NULL)
    return -1;
  return file_read (fe->file, buffer, (off_t) size);
}

int process_write (int fd, void *buffer, unsigned size)
{
  if (fd == STDOUT_FILENO)
    {
      putbuf (buffer, size);
      return size;
    }
  struct fd_entry *fe = get_fd_entry (fd);
  if (fe == NULL)
    return -1;
  return file_write (fe->file, buffer, (off_t) size);
}

int process_seek (int fd, unsigned position)
{
  struct fd_entry *fe = get_fd_entry (fd);
  if (fe == NULL)
    return -1;
  file_seek (fe->file, (off_t) position);
  return 0;
}

int process_tell (int fd)
{
  struct fd_entry *fe = get_fd_entry (fd);
  if (fe == NULL)
    return -1;
  return file_tell (fe->file);
}

int process_close (int fd)
{
  struct fd_entry *fe = get_fd_entry (fd);
  if (fe == NULL)
    return -1;
  file_close (fe->file);
  if (fe->dir)
    dir_close (fe->dir);
  list_remove (&fe->elem);
  free (fe);
  return 0;
}

static struct mmap_entry *
get_mmap_entry (mapid_t mid)
{
  struct list_elem *e;
  struct thread *curr = thread_current ();
  for (e = list_begin (&curr->mmap_list); e != list_end (&curr->mmap_list);
       e = list_next (e))
  {
    struct mmap_entry *me = list_entry (e, struct mmap_entry, elem);
    if (me->mid == mid)
      return me;
  }
  return NULL;
}

int process_mmap (int fd, void *addr)
{
  if (addr == NULL || pg_ofs (addr))
    return MAP_FAILED;

  struct fd_entry *fe = get_fd_entry (fd);
  if (fe == NULL)
    return MAP_FAILED;

  size_t file_size = file_length (fe->file);
  if (file_size == 0)
    return MAP_FAILED;

  /* Reopen the file */
  //lock_acquire (&filesys_lock)
  struct file *file = file_reopen (fe->file);
  //lock_release (&filesys_lock);

  struct mmap_entry *me = vm_add_mmap (file, addr, file_size);
  if (me == NULL)
    return MAP_FAILED;
  else
    return me->mid;
}

int process_munmap (mapid_t mid)
{
  struct mmap_entry *me = get_mmap_entry (mid);
  if (me == NULL)
    return -1;
  struct file *file = me->file;
  vm_munmap (me);
  //lock_acquire (&filesys_lock)
  file_close (file);
  //lock_release (&filesys_lock);
  list_remove (&me->elem);
  free (me);
  return 0;
}

bool process_readdir (int fd, char *name)
{
  struct fd_entry *fe = get_fd_entry (fd);

  if (fe == NULL)
    return false;

  if (!file_isdir (fe->file))
    return false;

  return dir_readdir (fe->dir, name);
}

bool process_isdir (int fd)
{
  struct fd_entry *fe = get_fd_entry (fd);

  if (fe == NULL)
    return false;

  return file_isdir (fe->file);
}

int process_inumber (int fd)
{
  struct fd_entry *fe = get_fd_entry (fd);

  if (fe == NULL)
    return -1;

  return file_get_inumber (fe->file);
}
