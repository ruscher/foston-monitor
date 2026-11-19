/*
 * boot.c - start a Linux kernel (or any bare-metal payload) on the FOSTON
 * FS-707DC from Windows CE 5, without writing to flash (HaRET-style).
 *
 * Sequence:
 *   1. Read the payload files into pages committed in the shared large-
 *      allocation area (VA >= 0x42000000, never remapped by the FCSE).
 *   2. In kernel mode, translate every page to its physical address by
 *      walking the live page tables (TTB from CP15, read through the
 *      uncached static kernel window 0xAC000000 + PA, RAM < 128 MB).
 *      Only pages at PA >= BOOT_SAFE_MIN are used: the destinations of the
 *      copy all lie below it, so nothing used by the copy is overwritten.
 *   3. Build a copy list (dst, src, len) chained across list pages, place
 *      the position-independent stub in its own page, map that page
 *      VA == PA with a 1 MB section in the live L1 table.
 *   4. IRQ/FIQ off, clean + invalidate caches, jump to the stub. The stub
 *      turns the MMU and caches off, copies, and enters the payload with
 *      r0 = 0, r1 = machine type, r2 = DTB physical address.
 *
 * Windows CE never runs again after step 4; a reset (pin hole) brings it
 * back unchanged, since nothing was written to the NAND.
 *
 * Measured facts this relies on (docs/01-hardware.md): ARM1176JZ-S, RAM at
 * PA 0, TTB at PA 0x00710000, v5-format page tables (CP15 control XP = 0),
 * kernel static map 0x8C000000/0xAC000000 -> PA 0.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "boot.h"

/* this coredll has _snwprintf (validated on the device); narrow _snprintf is unproven */
#define BERR(...) do { WCHAR w_[240]; _snwprintf(w_, 239, __VA_ARGS__); w_[239] = 0; \
                       WideCharToMultiByte(CP_UTF8, 0, w_, -1, err, (int)err_len, NULL, NULL); } while (0)

#define PAGE 4096u
#define RAM_SIZE 0x08000000u          /* 128 MB at PA 0 */
#define RAM_UNCACHED 0xAC000000u      /* CE static uncached map of PA 0 */
#define BOOT_SAFE_MIN 0x02000000u     /* pages used by the copy must be above every destination */
#define FB_START 0x03E00000u          /* framebuffer + CE video memory: never used as a source */
#define FB_END   0x04000000u
#define LIST_ENTRIES ((PAGE / 12u) - 1u)  /* last entry links to the next list page */

typedef BOOL (WINAPI *SetKMode_t)(BOOL);
typedef BOOL (WINAPI *LockPages_t)(LPVOID, DWORD, PDWORD, int);

/* ---------------------------------------------------------------- stub
 * Runs at its physical address (identity section), MMU still on at entry.
 *   r0 = PA of the first copy-list page
 *   r1 = PA of the parameter block { entry, r0, r1, r2 }
 * List entries: { dst, src, len }; dst == 0xFFFFFFFF -> continue at src
 * (next list page); len == 0 -> end of list. */
__asm__(
    ".text\n"
    ".align 5\n"
    ".global bf_stub_start\n"
    ".global bf_stub_end\n"
    "bf_stub_start:\n"
    "    mov   r10, r1\n"
    /* Windows CE leaves the FCSE PID of the calling process in c13: every
     * address below 32 MB would still be relocated by it (with the MMU on or
     * off), so the copy below and the kernel would land somewhere else. This
     * code and its data all sit above 32 MB, where the FCSE does not apply. */
    "    mov   r2, #0\n"
    "    mcr   p15, 0, r2, c13, c0, 0\n"   /* FCSE PID = 0 */
    "    mcr   p15, 0, r2, c13, c0, 1\n"   /* context ID = 0 */
    "    mcr   p15, 0, r2, c7, c5, 4\n"    /* prefetch flush */
    "    mrc   p15, 0, r2, c1, c0, 0\n"
    "    bic   r2, r2, #0x1000\n"          /* I cache off */
    "    bic   r2, r2, #0x0800\n"          /* branch prediction off */
    "    bic   r2, r2, #0x0005\n"          /* MMU and D cache off */
    "    mcr   p15, 0, r2, c1, c0, 0\n"
    "    nop\n"
    "    nop\n"
    "    nop\n"
    "    mov   r2, #0\n"
    "    mcr   p15, 0, r2, c7, c5, 0\n"    /* invalidate I cache */
    "    mcr   p15, 0, r2, c7, c5, 6\n"    /* flush branch target cache */
    "    mcr   p15, 0, r2, c8, c7, 0\n"    /* invalidate TLBs */
    "    mcr   p15, 0, r2, c7, c10, 4\n"   /* data synchronisation barrier */
    "1:  ldr   r3, [r0], #4\n"             /* dst */
    "    ldr   r4, [r0], #4\n"             /* src */
    "    ldr   r5, [r0], #4\n"             /* len */
    "    cmn   r3, #1\n"
    "    moveq r0, r4\n"
    "    beq   1b\n"
    "    cmp   r5, #0\n"
    "    beq   3f\n"
    "2:  ldr   r6, [r4], #4\n"
    "    str   r6, [r3], #4\n"
    "    subs  r5, r5, #4\n"
    "    bne   2b\n"
    "    b     1b\n"
    "3:  ldr   r3, [r10]\n"                /* entry */
    "    ldr   r0, [r10, #4]\n"
    "    ldr   r1, [r10, #8]\n"
    "    ldr   r2, [r10, #12]\n"
    "    mov   pc, r3\n"
    "bf_stub_end:\n");

extern const unsigned char bf_stub_start[], bf_stub_end[];

/* ----------------------------------------------------------- page pool */

typedef struct {
    uint8_t *va;       /* committed region in the large-allocation area */
    DWORD pages;
    DWORD *pa;         /* physical address of each page */
    DWORD next;        /* next unused page index */
} pool_t;

static DWORD read_ttb(void)
{
    DWORD ttb;
    __asm__ __volatile__("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttb));
    return ttb & ~0x3FFFu;
}

/* VA -> PA through the live page tables; kernel mode only. 0 on failure. */
static DWORD va_to_pa(DWORD ttb, DWORD va)
{
    volatile DWORD *l1 = (volatile DWORD *)(RAM_UNCACHED + ttb);
    DWORD d1 = l1[va >> 20], d2;
    switch (d1 & 3) {
    case 2:                                   /* section */
        return (d1 & 0xFFF00000u) | (va & 0x000FFFFFu);
    case 1: {                                 /* coarse */
        DWORD table = d1 & 0xFFFFFC00u;
        if (table >= RAM_SIZE)
            return 0;
        d2 = ((volatile DWORD *)(RAM_UNCACHED + table))[(va >> 12) & 0xFF];
        break;
    }
    case 3: {                                 /* fine */
        DWORD table = d1 & 0xFFFFF000u;
        if (table >= RAM_SIZE)
            return 0;
        d2 = ((volatile DWORD *)(RAM_UNCACHED + table))[(va >> 10) & 0x3FF];
        break;
    }
    default:
        return 0;
    }
    switch (d2 & 3) {
    case 1: return (d2 & 0xFFFF0000u) | (va & 0xFFFFu);     /* large 64 KB */
    case 2: return (d2 & 0xFFFFF000u) | (va & 0xFFFu);      /* small 4 KB */
    case 3: return (d2 & 0xFFFFFC00u) | (va & 0x3FFu);      /* tiny 1 KB */
    default: return 0;
    }
}

static int pool_create(pool_t *pool, DWORD pages, SetKMode_t setk, char *err, size_t err_len)
{
    DWORD i, ttb, pid;
    BOOL previous;
    memset(pool, 0, sizeof(*pool));
    /* >= 2 MB: CE 5 places it in the shared large-allocation area (no FCSE remapping) */
    if (pages * PAGE < 0x200000u)
        pages = 0x200000u / PAGE;
    /* CE 5 puts reservations > 2 MB made with MEM_RESERVE + PAGE_NOACCESS in the
     * shared large-allocation area; commit afterwards */
    pool->va = (uint8_t *)VirtualAlloc(NULL, pages * PAGE, MEM_RESERVE, PAGE_NOACCESS);
    if (pool->va && !VirtualAlloc(pool->va, pages * PAGE, MEM_COMMIT, PAGE_READWRITE))
        pool->va = NULL;
    pool->pa = (DWORD *)malloc(pages * sizeof(DWORD));
    if (!pool->va || !pool->pa) {
        BERR(L"allocation of %lu pages failed (%lu)", pages, GetLastError());
        return 0;
    }
    pool->pages = pages;
    for (i = 0; i < pages; i++)
        pool->va[i * PAGE] = 0;               /* commit physically */
    {
        LockPages_t lock = (LockPages_t)GetProcAddressW(LoadLibraryW(L"coredll.dll"), L"LockPages");
        if (lock)
            lock(pool->va, pages * PAGE, NULL, 2 /* LOCKFLAG_WRITE */);
    }
    previous = setk(TRUE);
    ttb = read_ttb();
    __asm__ __volatile__("mrc p15, 0, %0, c13, c0, 0" : "=r"(pid));
    pid &= 0xFE000000u;
    for (i = 0; i < pages; i++) {
        DWORD va = (DWORD)(pool->va + i * PAGE);
        if (va < 0x02000000u)
            va |= pid;                        /* FCSE: the MMU sees the modified VA */
        pool->pa[i] = va_to_pa(ttb, va);
    }
    setk(previous);
    return 1;
}

static int page_usable(DWORD pa)
{
    return pa >= BOOT_SAFE_MIN && pa < RAM_SIZE && !(pa >= FB_START && pa < FB_END) && !(pa & (PAGE - 1));
}

/* next usable page: returns its VA and PA */
static uint8_t *pool_take(pool_t *pool, DWORD *pa)
{
    while (pool->next < pool->pages) {
        DWORD i = pool->next++;
        if (page_usable(pool->pa[i])) {
            *pa = pool->pa[i];
            return pool->va + i * PAGE;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------- loading */

typedef struct {
    DWORD *list;       /* current list page (VA) */
    DWORD used;        /* entries used in it */
    DWORD first_pa;
} list_t;

static int list_add(pool_t *pool, list_t *l, DWORD dst, DWORD src, DWORD len)
{
    if (!l->list || l->used == LIST_ENTRIES) {
        DWORD pa;
        DWORD *page = (DWORD *)pool_take(pool, &pa);
        if (!page)
            return 0;
        if (l->list) {                        /* link the full page to the new one */
            l->list[l->used * 3] = 0xFFFFFFFFu;
            l->list[l->used * 3 + 1] = pa;
            l->list[l->used * 3 + 2] = 0;
        } else {
            l->first_pa = pa;
        }
        l->list = page;
        l->used = 0;
    }
    l->list[l->used * 3] = dst;
    l->list[l->used * 3 + 1] = src;
    l->list[l->used * 3 + 2] = len;
    l->used++;
    return 1;
}

/* Read a file into pool pages and add copy entries to dst. Returns bytes or -1. */
static long load_file(pool_t *pool, list_t *l, const WCHAR *path, DWORD dst, DWORD limit, char *err, size_t err_len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, done = 0;
    if (f == INVALID_HANDLE_VALUE) {
        BERR(L"cannot open %ls (%lu)", path, GetLastError());
        return -1;
    }
    size = GetFileSize(f, NULL);
    if (size == 0xFFFFFFFFu || size > limit) {
        CloseHandle(f);
        BERR(L"%ls: size %lu over the limit %lu", path, size, limit);
        return -1;
    }
    while (done < size) {
        DWORD pa, got = 0, want = size - done > PAGE ? PAGE : size - done;
        uint8_t *page = pool_take(pool, &pa);
        if (!page) {
            CloseHandle(f);
            BERR(L"out of usable pages loading %ls", path);
            return -1;
        }
        memset(page, 0, PAGE);
        if (!ReadFile(f, page, want, &got, NULL) || got != want) {
            CloseHandle(f);
            BERR(L"read error in %ls at %lu", path, done);
            return -1;
        }
        if (!list_add(pool, l, dst + done, pa, (want + 3) & ~3u)) {
            CloseHandle(f);
            BERR(L"out of pages for the copy list");
            return -1;
        }
        done += want;
    }
    CloseHandle(f);
    return (long)size;
}

/* DTB: patch /chosen? Not here: the DTB is built with its bootargs and
 * initrd range on the PC (tools/bigfoston/mkdtb), so the loader stays dumb. */

static DWORD file_size(const WCHAR *path)
{
    HANDLE f;
    DWORD size;
    if (!path[0])
        return 0;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    size = GetFileSize(f, NULL);
    CloseHandle(f);
    return size == 0xFFFFFFFFu ? 0 : size;
}

int bf_boot(const bf_boot_args *args, char *err, size_t err_len)
{
    SetKMode_t setk = (SetKMode_t)GetProcAddressW(LoadLibraryW(L"coredll.dll"), L"SetKMode");
    pool_t pool;
    list_t list;
    DWORD stub_pa, params_pa, need, ttb;
    uint8_t *stub, *params_page;
    DWORD *params;
    long n;
    BOOL previous;
    if (!setk) {
        BERR(L"SetKMode unavailable");
        return 0;
    }
    /* pages for the files + lists + stub; extra because pages below BOOT_SAFE_MIN are skipped */
    need = (file_size(args->kernel) + file_size(args->dtb) + file_size(args->initrd)) / PAGE + 64;
    if (!pool_create(&pool, need + need / 2 + 2048, setk, err, err_len))
        return 0;
    memset(&list, 0, sizeof(list));
    stub = pool_take(&pool, &stub_pa);
    params_page = pool_take(&pool, &params_pa);
    if (!stub || !params_page) {
        BERR(L"no usable page for the stub");
        return 0;
    }
    n = load_file(&pool, &list, args->kernel, args->kernel_pa, args->kernel_max, err, err_len);
    if (n < 0)
        return 0;
    if (args->dtb[0] && load_file(&pool, &list, args->dtb, args->dtb_pa, args->dtb_max, err, err_len) < 0)
        return 0;
    if (args->initrd[0] && load_file(&pool, &list, args->initrd, args->initrd_pa, args->initrd_max, err, err_len) < 0)
        return 0;
    if (!list_add(&pool, &list, 0, 0, 0)) {
        BERR(L"out of pages for the copy list");
        return 0;
    }
    if ((DWORD)(bf_stub_end - bf_stub_start) > PAGE) {
        BERR(L"stub too large");
        return 0;
    }
    memcpy(stub, bf_stub_start, bf_stub_end - bf_stub_start);
    params = (DWORD *)params_page;
    params[0] = args->kernel_pa;
    params[1] = 0;
    params[2] = args->machine;
    params[3] = args->dtb[0] ? args->dtb_pa : 0;

    if (args->dry_run) {
        BERR(L"dry run ok: stub 0x%08lx params 0x%08lx list 0x%08lx kernel %ld bytes",
                  stub_pa, params_pa, list.first_pa, n);
        return 1;
    }

    /* point of no return */
    previous = setk(TRUE);
    (void)previous;
    __asm__ __volatile__(
        "mrs   r3, cpsr\n\t"
        "orr   r3, r3, #0xC0\n\t"             /* IRQ and FIQ off: Windows CE stops here */
        "msr   cpsr_c, r3\n\t" ::: "r3", "memory");
    ttb = read_ttb();
    {
        DWORD first = list.first_pa, prm = params_pa, target = stub_pa;
        volatile DWORD *l1 = (volatile DWORD *)(RAM_UNCACHED + ttb);
        /* identity section for the stub: AP = 3 (privileged rw), uncached, domain 0 */
        l1[stub_pa >> 20] = (stub_pa & 0xFFF00000u) | (3u << 10) | 2u;
        __asm__ __volatile__(
            "mov   r3, #0\n\t"
            "mcr   p15, 0, r3, c7, c14, 0\n\t"  /* clean + invalidate D cache */
            "mcr   p15, 0, r3, c7, c5, 0\n\t"   /* invalidate I cache */
            "mcr   p15, 0, r3, c7, c10, 4\n\t"  /* DSB */
            "mcr   p15, 0, r3, c8, c7, 0\n\t"   /* invalidate TLBs (new identity entry) */
            "mcr   p15, 0, r3, c7, c5, 4\n\t"   /* prefetch flush */
            "mov   r0, %0\n\t"
            "mov   r1, %1\n\t"
            "mov   pc, %2\n\t"
            :
            : "r"(first), "r"(prm), "r"(target)
            : "r0", "r1", "r3", "memory");
    }
    for (;;)
        ;
}
