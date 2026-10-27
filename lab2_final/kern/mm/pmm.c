#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <slub.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void check_alloc_page(void);
static void page_init(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void) {
    pmm_manager = &best_fit_pmm_manager;
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

//lab2 function1 2411559

/* page_init - build the Page descriptor array from the physical memory range
 * reported by the device tree, then hand every page that is not occupied by
 * OpenSBI, the kernel image or the descriptor array itself to the current
 * pmm_manager.
 *
 * Four kinds of "address" are involved, do not mix them up:
 *   virtual address : what a C pointer holds, always >= KERNBASE
 *   physical address: VA - va_pa_offset, what the hardware / DTB talk about
 *   page number     : PPN(pa) = pa >> PGSHIFT, the page's index in RAM
 *   Page descriptor : pages[PPN(pa) - nbase], exactly one struct Page per page
 */
static void
page_init(void) {
    /* PADDR() and pa2page() are built on va_pa_offset, so it has to be set
     * before any address conversion below. */
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    /* (1) Physical memory range probed from the device tree, both PAs. */
    uint64_t mem_base = get_memory_base();
    uint64_t mem_size = get_memory_size();
    if (mem_base == 0 || mem_size == 0) {
        panic("page_init: dtb reported no usable memory (base=0x%lx, size=0x%lx)",
              mem_base, mem_size);
    }
    uint64_t mem_end = mem_base + mem_size;
    cprintf("phys memory: [0x%016lx, 0x%016lx), size = 0x%016lx\n",
            mem_base, mem_end, mem_size);

    /* (2) npage is the page number one past the end of memory.  The descriptor
     *     array has to cover every page in [nbase, npage), where
     *     nbase = PPN(DRAM_BASE) is the lowest physical page the kernel knows
     *     about, so the array holds npage - nbase descriptors. */
    npage = PPN(mem_end);
    /* nbase 是 DRAM 起始页号，npage 必须比它大，否则下面 npage - nbase
     * 会在 size_t 上下溢成天文数字，描述符循环就会写到数组外面去。 */
    if (npage <= nbase) {
        panic("page_init: no manageable physical pages (npage=%lu, nbase=%lu)",
              (unsigned long)npage, (unsigned long)nbase);
    }
    size_t nr_pages = npage - nbase;
    cprintf("page_init: npage = %lu, nbase = %lu, descriptors = %lu\n",
            (unsigned long)npage, (unsigned long)nbase,
            (unsigned long)nr_pages);

    /* (3) `end` is the linker symbol that marks the end of the kernel image,
     *     and it is a *virtual* address; PADDR(end) is the matching physical
     *     address.  va_pa_offset is page aligned, so rounding the VA up to
     *     PGSIZE rounds the PA up to PGSIZE as well. */
    extern char end[];
    uintptr_t end_va = (uintptr_t)end;
    uintptr_t end_pa = PADDR(end_va);

    pages = (struct Page *)ROUNDUP(end_va, PGSIZE);  /* array start, VA */
    uintptr_t pages_pa = PADDR((uintptr_t)pages);    /* array start, PA */
    uintptr_t desc_pa_end = pages_pa + nr_pages * sizeof(struct Page);

    /* (4) Reserve all descriptors of [nbase, npage) up front: flags is reset to
     *     PG_reserved only, ref = 0, property = 0.  init_memmap() only
     *     un-reserves the pages it is actually given, so OpenSBI's pages, the
     *     kernel image and the descriptor array itself keep PG_reserved
     *     afterwards. */
    for (size_t i = 0; i < nr_pages; i++) {
        struct Page *p = pages + i;
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
        SetPageReserved(p);
    }
    cprintf("page_init: pages = 0x%016lx (pa 0x%016lx), %lu bytes, "
            "kernel end pa 0x%016lx\n",
            (unsigned long)(uintptr_t)pages, (unsigned long)pages_pa,
            (unsigned long)(nr_pages * sizeof(struct Page)),
            (unsigned long)end_pa);

    /* (5) Free region: right after the descriptor array (its own pages must
     *     stay reserved), up to the last complete page of memory.  Both bounds
     *     are physical addresses. */
    uintptr_t free_start_pa = ROUNDUP(desc_pa_end, PGSIZE);
    uintptr_t free_end_pa = ROUNDDOWN(mem_end, PGSIZE);

    if (free_end_pa > free_start_pa) {
        /* pa2page(pa) == &pages[PPN(pa) - nbase], i.e. the descriptor of the
         * first free page; the length is counted in pages. */
        init_memmap(pa2page(free_start_pa), (free_end_pa - free_start_pa) / PGSIZE);
        cprintf("page_init: free memory [0x%016lx, 0x%016lx), %lu pages\n",
                (unsigned long)free_start_pa, (unsigned long)free_end_pa,
                (unsigned long)((free_end_pa - free_start_pa) / PGSIZE));
    } else {
        cprintf("page_init: no free memory left after the kernel image\n");
    }
}

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();

#ifdef ucore_test
    slub_check();
#endif

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
