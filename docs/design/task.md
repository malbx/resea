# Tasks
A *task* is a unit of execution just like *process* in other operating systems.
It contains a CPU context (registers) and its own virtual address space.

The biggest difference is that **Resea does not have kernel-level threading**:
in other words, only single thread can exist in a task [^1].

## Server
*Server* is a task which provides services like device driver, file system,
TCP/IP, etc. While we use the term *server* in documentation and code comments, the kernel
does not distinguish between server tasks and client (non-server) tasks.

## Pager
Each tasks (except the very first task created by the kernel) is associated a
*pager*, a task which is responsible for handling exceptions occurred in the
task. When the following events (called *exceptions*) occur, the kernel
sends a message to the associated pager to handle them:

- **Page fault:** The pager task is responsible for mapping a memory page by the `map` system call (or kills the task if it's a so-called segmentation fault).
  Specifically, a pager allocate a physical memory page for the task, copy the file
  contents into the page, map the page, and reply the message to resume the task.
- **When a task exits:** Because of invalid opcode exception, divide by zero, etc.
- **ABI Emulation Hook:** If ABI emulation is enabled for the task, the kernel
  asks the pager to handle system calls, etc.

This *pager* mechanism is introduced for achieving [the separation of mechanism and policy](https://en.wikipedia.org/wiki/Separation_of_mechanism_and_policy)
and it suprisingly improves the flexibility of the operating system.

[^1]: Note that you can still implement *threads* in Resea by simply mapping *same* physical memory pages in your pager. I suppose the size of page table is negligible.
