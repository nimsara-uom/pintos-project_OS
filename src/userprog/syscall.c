#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"

struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
static void terminate_bad_user (void) NO_RETURN;
static bool valid_range (const void *, size_t);
static bool copy_string (const char *, char *, size_t);
static uint32_t argument (struct intr_frame *, unsigned);
static struct file_desc *find_fd (int);

void
syscall_init (void)
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
terminate_bad_user (void)
{
  thread_current ()->exit_status = -1;
  thread_exit ();
}

static bool
valid_range (const void *buffer, size_t size)
{
  const uint8_t *start = buffer;
  size_t i;
  if (buffer == NULL)
    return false;
  for (i = 0; i < size; i++)
    if (!is_user_vaddr (start + i)
        || pagedir_get_page (thread_current ()->pagedir, start + i) == NULL)
      return false;
  return true;
}

static bool
copy_string (const char *user, char *kernel, size_t size)
{
  size_t i;
  if (size == 0)
    return false;
  for (i = 0; i < size; i++)
    {
      if (!valid_range (user + i, 1))
        return false;
      kernel[i] = user[i];
      if (kernel[i] == '\0')
        return true;
    }
  return false;
}

static uint32_t
argument (struct intr_frame *f, unsigned index)
{
  uint32_t value;
  uint8_t *address = (uint8_t *) f->esp + index * sizeof value;
  if (!valid_range (address, sizeof value))
    terminate_bad_user ();
  value = *(uint32_t *) address;
  return value;
}

static struct file_desc *
find_fd (int fd)
{
  struct list_elem *e;
  for (e = list_begin (&thread_current ()->file_descriptors);
       e != list_end (&thread_current ()->file_descriptors);
       e = list_next (e))
    {
      struct file_desc *entry = list_entry (e, struct file_desc, elem);
      if (entry->fd == fd)
        return entry;
    }
  return NULL;
}

static void
syscall_handler (struct intr_frame *f)
{
  unsigned call;
  call = argument (f, 0);
  switch (call)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;
    case SYS_EXIT:
      thread_current ()->exit_status = (int) argument (f, 1);
      thread_exit ();
      break;
    case SYS_EXEC:
      {
        char *cmd = palloc_get_page (0);
        if (cmd == NULL || !copy_string ((const char *) argument (f, 1),
                                         cmd, PGSIZE))
          {
            if (cmd != NULL) palloc_free_page (cmd);
            terminate_bad_user ();
          }
        f->eax = process_execute (cmd);
        palloc_free_page (cmd);
      }
      break;
    case SYS_WAIT:
      f->eax = process_wait ((tid_t) argument (f, 1));
      break;
    case SYS_CREATE:
      {
        char *name = palloc_get_page (0);
        if (name == NULL || !copy_string ((const char *) argument (f, 1),
                                          name, PGSIZE))
          {
            if (name != NULL) palloc_free_page (name);
            terminate_bad_user ();
          }
        lock_acquire (&filesys_lock);
        f->eax = filesys_create (name, argument (f, 2));
        lock_release (&filesys_lock);
        palloc_free_page (name);
      }
      break;
    case SYS_REMOVE:
      {
        char *name = palloc_get_page (0);
        if (name == NULL || !copy_string ((const char *) argument (f, 1),
                                          name, PGSIZE))
          {
            if (name != NULL) palloc_free_page (name);
            terminate_bad_user ();
          }
        lock_acquire (&filesys_lock);
        f->eax = filesys_remove (name);
        lock_release (&filesys_lock);
        palloc_free_page (name);
      }
      break;
    case SYS_OPEN:
      {
        char *name = palloc_get_page (0);
        struct file_desc *entry;
        struct file *file;
        if (name == NULL || !copy_string ((const char *) argument (f, 1),
                                          name, PGSIZE))
          {
            if (name != NULL) palloc_free_page (name);
            terminate_bad_user ();
          }
        lock_acquire (&filesys_lock);
        file = filesys_open (name);
        lock_release (&filesys_lock);
        palloc_free_page (name);
        if (file == NULL)
          {
            f->eax = -1;
            break;
          }
        entry = malloc (sizeof *entry);
        if (entry == NULL)
          {
            lock_acquire (&filesys_lock);
            file_close (file);
            lock_release (&filesys_lock);
            f->eax = -1;
            break;
          }
        entry->fd = thread_current ()->next_fd++;
        entry->file = file;
        list_push_back (&thread_current ()->file_descriptors, &entry->elem);
        f->eax = entry->fd;
      }
      break;
    case SYS_FILESIZE:
      {
        struct file_desc *entry = find_fd ((int) argument (f, 1));
        if (entry == NULL)
          f->eax = -1;
        else
          {
            lock_acquire (&filesys_lock);
            f->eax = file_length (entry->file);
            lock_release (&filesys_lock);
          }
      }
      break;
    case SYS_READ:
      {
        int fd = (int) argument (f, 1);
        void *buffer = (void *) argument (f, 2);
        unsigned size = argument (f, 3);
        unsigned i;
        struct file_desc *entry;
        if (!valid_range (buffer, size))
          terminate_bad_user ();
        if (fd == 0)
          {
            for (i = 0; i < size; i++)
              ((uint8_t *) buffer)[i] = input_getc ();
            f->eax = size;
          }
        else if ((entry = find_fd (fd)) != NULL)
          {
            lock_acquire (&filesys_lock);
            f->eax = file_read (entry->file, buffer, size);
            lock_release (&filesys_lock);
          }
        else
          f->eax = -1;
      }
      break;
    case SYS_WRITE:
      {
        int fd = (int) argument (f, 1);
        const void *buffer = (const void *) argument (f, 2);
        unsigned size = argument (f, 3);
        struct file_desc *entry;
        if (!valid_range (buffer, size))
          terminate_bad_user ();
        if (fd == 1)
          {
            putbuf (buffer, size);
            f->eax = size;
          }
        else if ((entry = find_fd (fd)) != NULL)
          {
            lock_acquire (&filesys_lock);
            f->eax = file_write (entry->file, buffer, size);
            lock_release (&filesys_lock);
          }
        else
          f->eax = -1;
      }
      break;
    case SYS_SEEK:
      {
        struct file_desc *entry = find_fd ((int) argument (f, 1));
        if (entry != NULL)
          {
            lock_acquire (&filesys_lock);
            file_seek (entry->file, argument (f, 2));
            lock_release (&filesys_lock);
          }
      }
      break;
    case SYS_TELL:
      {
        struct file_desc *entry = find_fd ((int) argument (f, 1));
        if (entry == NULL)
          f->eax = -1;
        else
          {
            lock_acquire (&filesys_lock);
            f->eax = file_tell (entry->file);
            lock_release (&filesys_lock);
          }
      }
      break;
    case SYS_CLOSE:
      {
        struct file_desc *entry = find_fd ((int) argument (f, 1));
        if (entry != NULL)
          {
            list_remove (&entry->elem);
            lock_acquire (&filesys_lock);
            file_close (entry->file);
            lock_release (&filesys_lock);
            free (entry);
          }
      }
      break;
    default:
      terminate_bad_user ();
    }
}
