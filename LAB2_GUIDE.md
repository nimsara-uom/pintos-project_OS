# Pintos Lab 2: User Programs — Engineering Guide

This document explains the Lab 2 implementation in this repository as an
operating-systems engineering exercise. It is intended to be read alongside
the source code, not as a replacement for the Pintos assignment specification.

## 1. What Lab 2 adds

The original Pintos kernel can create kernel threads, but it cannot safely run
normal user programs. Lab 2 adds the boundary between user mode and kernel
mode:

1. Process creation and termination.
2. Command-line argument passing.
3. Safe access to user virtual memory.
4. A complete basic system-call interface.
5. Parent/child synchronization for `exec` and `wait`.
6. File-descriptor management.
7. Write protection for running executables.

The implementation is on the `lab2` branch and builds on the scheduler from
Lab 1.

## 2. The user/kernel boundary

A user program cannot directly call kernel C functions. Instead, the user
library places a system-call number and arguments on the user stack, then
executes `int $0x30`.

The CPU enters `syscall_handler()` in
[`src/userprog/syscall.c`](src/userprog/syscall.c). The handler:

1. Validates the user stack address.
2. Reads the system-call number and arguments.
3. Validates every pointer and buffer before dereferencing it.
4. Performs the requested kernel operation.
5. Stores the return value in `intr_frame.eax`.

The system-call numbers are defined in
[`src/lib/syscall-nr.h`](src/lib/syscall-nr.h), and user-space wrappers are in
`src/lib/user/syscall.c`.

## 3. Process creation

The process-creation path is:

```text
process_execute()
    |
    +-- copy command line into a private page
    +-- create a child_info synchronization record
    +-- create a kernel thread
    |
    +-- start_process()
            |
            +-- load()
            +-- build the initial user stack
            +-- enter user mode through intr_exit
```

### Why copy the command line?

The caller may reuse or modify its command-line buffer immediately after
`process_execute()` returns. The child therefore receives a private page
containing the command line. This also prevents a race between the parent and
the loader.

### Why wait for loading?

`exec()` must return `-1` if loading fails. The parent cannot know that result
until the child has attempted to load the executable. Each child record has a
`load_sema`; the child signals it after `load()` finishes, and the parent
waits before returning from `process_execute()`.

## 4. Parent/child lifetime management

Each process keeps a list of direct-child records. A child record contains:

- The child thread ID.
- Whether loading succeeded.
- The child's exit status.
- A semaphore for load completion.
- A semaphore for child termination.
- Whether the parent is still alive.
- Whether the child is still alive.
- Whether the parent already waited.

This handles all exit orders:

| Event order | Required behavior |
|---|---|
| Child exits first | Keep its status until the parent waits. |
| Parent exits first | Child continues; the shared record is released by the last owner. |
| Parent waits while child runs | Block until the child signals termination. |
| Parent waits twice | Return `-1` on the second call. |
| Non-child PID | Return `-1` immediately. |

`process_wait()` searches only the current thread's child list, so children
are not inherited by grandparents.

## 5. Exit status and cleanup

The current process stores its status in `thread_current()->exit_status`.
The `exit` system call sets this value before terminating. A process killed by
an invalid user pointer or exception reports `-1`.

User processes print:

```text
program-name: exit(status)
```

Kernel-only threads do not print this message.

During `process_exit()` the kernel:

1. Publishes the exit status to the parent record.
2. Wakes a waiting parent.
3. Closes all open file descriptors.
4. Re-enables writes to the executable.
5. Closes the executable.
6. Destroys the process page directory.

Cleanup is deliberately performed before the address space is destroyed.

## 6. Argument passing and the initial stack

For a command such as:

```text
echo hello world
```

the kernel creates:

```text
argc = 3
argv[0] = "echo"
argv[1] = "hello"
argv[2] = "world"
argv[3] = NULL
```

The stack is constructed from high addresses downward:

1. Copy argument strings near `PHYS_BASE`.
2. Align the stack to a 4-byte boundary.
3. Push the null `argv` sentinel.
4. Push argument pointers right-to-left.
5. Push the `argv` pointer.
6. Push `argc`.
7. Push a fake return address of zero.

The user entry point in `lib/user/entry.c` reads this stack layout before
calling `main(argc, argv)`.

Repeated spaces are treated as separators, so multiple spaces do not create
empty arguments.

## 7. Safe user-memory access

User pointers are untrusted. A process may pass:

- `NULL`.
- An address in kernel space.
- An unmapped address.
- A buffer that crosses into an unmapped page.
- A system-call stack pointer that is itself invalid.

`valid_range()` checks every byte of a range using both `is_user_vaddr()` and
`pagedir_get_page()`. This is intentionally conservative and easy to reason
about: the kernel validates the complete range before copying or using it.

`copy_string()` copies user strings into kernel-owned pages. Kernel-owned
copies are important because filesystem operations may block and because a
user process must not be able to change a path while the kernel is using it.

Invalid user input terminates only the offending process with status `-1`;
it must not panic the kernel or corrupt another process.

## 8. Filesystem synchronization

The provided Lab 2 filesystem is not internally synchronized. The global
`filesys_lock` therefore protects:

- Opening and closing files.
- Creating and removing directory entries.
- File length queries.
- Reads, writes, seeks, and tells.
- Executable loading and executable cleanup.

The filesystem implementation itself was not modified. The kernel uses its
public API under one shared lock.

## 9. File descriptors

Each process owns a private descriptor list:

- `0`: standard input.
- `1`: standard output.
- `2` and above: files opened by that process.

Descriptors are not inherited by child processes. Each `open()` creates a new
descriptor and a new file object, so file positions are independent.

On process termination, all descriptors are closed automatically. Invalid
descriptors return the system-call-specific error value or perform no action,
as required by the Pintos interface.

Special console behavior:

- `read(0, buffer, size)` reads keyboard characters.
- `write(1, buffer, size)` writes the complete buffer to the console.

## 10. Running executable protection

After loading an executable, the process keeps its `struct file` open and
calls `file_deny_write()`. This prevents another process from modifying the
binary while it is executing.

The write permission is restored only during final process cleanup with
`file_allow_write()`, immediately before closing the executable.

This lifetime rule is important: closing the executable immediately after
loading would silently re-enable writes too early.

## 11. System-call summary

| System call | Implementation behavior |
|---|---|
| `halt` | Powers off the simulated machine. |
| `exit` | Stores status, prints termination message, and terminates. |
| `exec` | Starts a child and waits for load success/failure. |
| `wait` | Waits for a direct child once and returns its status. |
| `create` | Creates a fixed-size file. |
| `remove` | Removes a directory entry. |
| `open` | Opens a file and returns a process-local descriptor. |
| `filesize` | Returns the length of an open file. |
| `read` | Reads from stdin or an open file. |
| `write` | Writes to stdout or an open file. |
| `seek` | Changes a file's current position. |
| `tell` | Reports a file's current position. |
| `close` | Closes and releases a descriptor. |

## 12. Commit history

The work was deliberately divided into reviewable commits:

```text
0451c33  Add Lab 2 process lifecycle state
ddb542d  Implement Lab 2 user memory and system calls
0c568d4  Fix Lab 2 kernel stack safety and preemption
```

The final fix commit is especially instructive. Pintos kernel stacks are very
small, so page-sized local arrays can overwrite the thread structure and cause
`thread_current()` assertions. Large temporary buffers must be allocated from
the page allocator instead.

## 13. Validation

The full available userprog/filesystem test suite was run with:

```bash
cd src/userprog/build
make check
```

Result:

```text
160 tests passed
0 tests failed
```

The passing tests cover argument passing, invalid pointers, system calls,
process waiting, multiple children, descriptor behavior, executable write
protection, multi-process execution, and the base filesystem tests.

## 14. How to study this implementation

Recommended reading order:

1. [`src/userprog/process.h`](src/userprog/process.h)
2. [`src/threads/thread.h`](src/threads/thread.h)
3. [`src/userprog/process.c`](src/userprog/process.c)
4. [`src/userprog/syscall.c`](src/userprog/syscall.c)
5. [`src/userprog/exception.c`](src/userprog/exception.c)
6. [`src/filesys/file.h`](src/filesys/file.h)
7. [`src/filesys/filesys.h`](src/filesys/filesys.h)

For each system call, trace both directions:

```text
user wrapper -> int $0x30 -> syscall_handler
             -> kernel operation -> intr_frame.eax
             -> user wrapper return value
```

Then read the corresponding test in `src/tests/userprog/`. The tests are
executable specifications: they show the required behavior more precisely
than comments alone.

## 15. Source references

This guide and implementation use:

- The Lab 2 instructions supplied with this project.
- The existing Pintos source tree and public filesystem APIs.
- The official Pintos project documentation available from
  [pintos-os.org](http://pintos-os.org).

No third-party solution code was copied into this repository.
