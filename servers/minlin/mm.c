#include <resea/io.h>
#include <resea/malloc.h>
#include <cstring.h>
#include "elf.h"
#include "fs.h"
#include "mm.h"
#include "proc.h"

struct mchunk *mm_resolve(struct mm *mm, vaddr_t vaddr) {
    LIST_FOR_EACH(mchunk, &mm->mchunks, struct mchunk, next) {
        if (mchunk->vaddr <= vaddr && vaddr < mchunk->vaddr + mchunk->len) {
            return mchunk;
        }
    }

    return NULL;
}

struct mchunk *mm_alloc_mchunk(struct mm *mm, vaddr_t vaddr, size_t len) {
    DEBUG_ASSERT(IS_ALIGNED(len, PAGE_SIZE));
    paddr_t paddr;
    void *buf = (void *) io_alloc_pages(len / PAGE_SIZE, 0, &paddr);
    struct mchunk *mchunk = malloc(sizeof(*mchunk));
    mchunk->vaddr = vaddr;
    mchunk->paddr = paddr;
    mchunk->buf = buf;
    mchunk->len = len;
    list_push_back(&mm->mchunks, &mchunk->next);
    return mchunk;
}

struct mchunk *mm_clone_mchunk(struct proc *child, struct mchunk *src) {
    struct mchunk *dst = mm_alloc_mchunk(&child->mm, src->vaddr, src->len);
    memcpy(dst->buf, src->buf, dst->len);
    return dst;
}

errno_t mm_fork(struct proc *parent, struct proc *child) {
    list_init(&child->mm.mchunks);
    if (parent) {
        LIST_FOR_EACH(mchunk, &parent->mm.mchunks, struct mchunk, next) {
            mm_clone_mchunk(child, mchunk);
        }
    }

    return 0;
}

error_t copy_from_user(struct proc *proc, void *dst, vaddr_t src,
                       size_t len) {
    size_t remaining = len;
    while (remaining > 0) {
        size_t copy_len = MIN(remaining, PAGE_SIZE - (src % PAGE_SIZE));
        vaddr_t src_aligned = ALIGN_DOWN(src, PAGE_SIZE);
        struct mchunk *mchunk = mm_resolve(&proc->mm, src_aligned);
        if (!mchunk) {
            if (!handle_page_fault(proc, src, PF_USER)) {
                return ERR_NOT_PERMITTED;
            }

            mchunk = mm_resolve(&proc->mm, src);
            DEBUG_ASSERT(mchunk);
        }

        memcpy(dst, &mchunk->buf[src - mchunk->vaddr], copy_len);
        dst += copy_len;
        src += copy_len;
        remaining -= copy_len;
    }

    return OK;
}

error_t copy_to_user(struct proc *proc, vaddr_t dst, const void *src,
                     size_t len) {
    size_t remaining = len;
    while (remaining > 0) {
        size_t copy_len = MIN(remaining, PAGE_SIZE - (dst % PAGE_SIZE));
        vaddr_t dst_aligned = ALIGN_DOWN(dst, PAGE_SIZE);
        struct mchunk *mchunk = mm_resolve(&proc->mm, dst_aligned);
        if (!mchunk) {
            if (!handle_page_fault(proc, dst, PF_USER)) {
                return ERR_NOT_PERMITTED;
            }

            mchunk = mm_resolve(&proc->mm, dst);
            DEBUG_ASSERT(mchunk);
        }

        memcpy(&mchunk->buf[dst - mchunk->vaddr], src, copy_len);
        dst += copy_len;
        src += copy_len;
        remaining -= copy_len;
    }

    return OK;
}

size_t strncpy_from_user(struct proc *proc, char *dst, vaddr_t src,
                         size_t max_len) {
    size_t read_len = 0;
    while (read_len < max_len) {
        char ch;
        error_t err;
        if ((err = copy_from_user(proc, &ch, src, sizeof(ch))) != OK) {
            return 0;
        }

        dst[read_len] = ch;
        if (!ch) {
            return read_len;
        }

        read_len++;
        src++;
    }

    return read_len;
}

paddr_t handle_page_fault(struct proc *proc, vaddr_t vaddr, pagefault_t fault) {
    vaddr_t aligned_vaddr = ALIGN_DOWN(vaddr, PAGE_SIZE);

    if (fault & PF_PRESENT) {
        // Invalid access. For instance the user thread has tried to write to
        // readonly area.
        WARN("%s: invalid memory access at %p (perhaps segfault?)", proc->name,
             vaddr);
        return 0;
    }

    // Look for the associated mchunk.
    struct mchunk *mchunk = mm_resolve(&proc->mm, vaddr);
    if (mchunk) {
        return mchunk->paddr + (aligned_vaddr - mchunk->vaddr);
    }

    // Allocate heap.
    if (HEAP_ADDR <= vaddr && vaddr < proc->current_brk) {
        struct mchunk *new_mchunk = mm_alloc_mchunk(&proc->mm, aligned_vaddr, PAGE_SIZE);
        if (!new_mchunk) {
            return 0;
        }

        memset(new_mchunk->buf, 0, new_mchunk->len);
        DBG("allocated heap at %p %p", new_mchunk->vaddr, new_mchunk);
        return new_mchunk->paddr + (aligned_vaddr - new_mchunk->vaddr);
    }

    // Look for the associated program header.
    struct elf64_phdr *phdr = NULL;
    for (unsigned i = 0; i < proc->ehdr->e_phnum; i++) {
        // TODO: .bss section

        // Ignore GNU_STACK
        if (!proc->phdrs[i].p_vaddr) {
            continue;
        }

        vaddr_t start = proc->phdrs[i].p_vaddr;
        vaddr_t end = start + proc->phdrs[i].p_memsz;
        if (start <= vaddr && vaddr < end) {
            phdr = &proc->phdrs[i];
            break;
        }
    }

    if (!phdr) {
        WARN("invalid memory access (addr=%p), killing %s...", vaddr, proc->name);
        return 0;
    }

    // Allocate a page and fill it with the file data.
    struct mchunk *new_mchunk = mm_alloc_mchunk(&proc->mm, aligned_vaddr, PAGE_SIZE);
    if (!new_mchunk) {
        return 0;
    }

    memset(new_mchunk->buf, 0, PAGE_SIZE);
    // Copy file contents.
    size_t offset_in_file;
    size_t offset_in_page;
    size_t copy_len;
    if (aligned_vaddr < phdr->p_vaddr) {
        offset_in_file = phdr->p_offset;
        offset_in_page = phdr->p_vaddr % PAGE_SIZE;
        copy_len = MIN(PAGE_SIZE - offset_in_page, phdr->p_filesz);
    } else {
        size_t offset_in_segment = aligned_vaddr - phdr->p_vaddr;
        if (offset_in_segment >= phdr->p_filesz) {
            // The entire page should be and have already been filled with
            // zeroes (.bss section).
            copy_len = 0;
            offset_in_file = 0;
            offset_in_page = 0;
        } else {
            offset_in_file = offset_in_segment + phdr->p_offset;
            offset_in_page = 0;
            copy_len = MIN(phdr->p_filesz - offset_in_segment, PAGE_SIZE);
        }
    }

    fs_read_pos(proc->exec, &new_mchunk->buf[offset_in_page],
                offset_in_file, copy_len);

    return new_mchunk->paddr;
}