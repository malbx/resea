#include <support/printk.h>
#include <arch.h>
#include <support/backtrace.h>

/// The symbol table. tools/link.py embeds it during the build.
extern struct symbol_table __symtable;

/// Resolves a symbol name and the offset from the beginning of symbol.
/// This function returns "(invalid address)" if the symbol does not
/// exist in the symbol table.
static const char *find_symbol(vaddr_t vaddr, size_t *offset) {
    ASSERT(__symtable.magic == SYMBOL_TABLE_MAGIC
           && "invalid symbol table magic");

    struct symbol *symbols = __symtable.symbols;
    uint16_t num_symbols = __symtable.num_symbols;
    const char *strbuf = __symtable.strbuf;

    // Do a binary search. Since `num_symbols` is unsigned 16-bit integer, we
    // need larger signed integer to hold -1 here, namely, int32_t.
    int32_t l = -1;
    int32_t r = num_symbols;
    while (r - l > 1) {
        int32_t mid = (l + r) / 2;
        if (vaddr >= symbols[mid].addr) {
            l = mid;
        } else {
            r = mid;
        }
    }

    if (l == -1) {
        *offset = 0;
        return "(invalid address)";
    }

    *offset = vaddr - symbols[l].addr;
    return &strbuf[symbols[l].offset];
}

/// Prints the stack trace.
void backtrace(void) {
    WARN("Backtrace:");
    struct stack_frame *frame = get_stack_frame();
    for (int i = 0; i < BACKTRACE_MAX; i++) {
        if (frame->return_addr < KERNEL_BASE_ADDR) {
            break;
        }

        size_t offset;
        const char *name = find_symbol(frame->return_addr, &offset);
        WARN("    #%d: %p %s()+0x%x", i, frame->return_addr, name, offset);

        if ((uint64_t) frame->next < KERNEL_BASE_ADDR) {
            break;
        }

        frame = frame->next;
    }
}
