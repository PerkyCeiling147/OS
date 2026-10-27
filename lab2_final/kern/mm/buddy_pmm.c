#include <pmm.h>
#include <list.h>
#include <stdio.h>
#include <buddy_pmm.h>

/* 可分配的块大小只能是 2 的幂, 最大 2^MAX_ORDER 页;
 * order 的合法取值是 0..MAX_ORDER (order 为 i 的块含 2^i 页). */
#define MAX_ORDER 15

/* 每个 order 维护一条空闲块链表:
 *   - 块首页的 Page.property 记录该块的 order
 *   - 块首页的 PG_property 置位, 表示它是空闲块首页
 *   - nr_free 记录该 order 上的"块数"(不是页数), 页数由 nr_free << order 得到 */
static free_area_t free_area[MAX_ORDER + 1];

/* ------------------------------------------------------------------------- *
 * 小工具
 * ------------------------------------------------------------------------- */

/* 页数 n 对应的 order: 最小的 k 使 2^k >= n, 即 ceil(log2(n));
 * n > 2^MAX_ORDER 时返回 MAX_ORDER + 1, 由调用者拒绝. */
static size_t
order_of(size_t n) {
    size_t order = 0;
    while (order <= MAX_ORDER && ((size_t)1 << order) < n) {
        order++;
    }
    return order;
}

/* 描述符对应的物理页号 (PPN).
 * Page 数组从物理页 nbase 开始: pages[i] 描述页号 nbase + i 的物理页,
 * 所以 PPN = (page - pages) + nbase = page2ppn(page). */
static size_t
ppn_of(struct Page *page) {
    return (size_t)page2ppn(page);
}

/* page 在 "order 大小的块" 中的伙伴.
 * 伙伴关系只看页号的一个二进制位: 同 order 的两个伙伴页号只差第 order 位,
 * 即 buddy_ppn = ppn XOR (1 << order).
 * 伙伴页号落到 [nbase, npage) 之外时没有描述符, 返回 NULL (不得解引用). */
static struct Page *
buddy_of(struct Page *page, size_t order) {
    size_t buddy_ppn = ppn_of(page) ^ ((size_t)1 << order);
    if (buddy_ppn < nbase || buddy_ppn >= npage) {
        return NULL;
    }
    return &pages[buddy_ppn - nbase];
}

/* ------------------------------------------------------------------------- *
 * pmm_manager 接口
 * ------------------------------------------------------------------------- */

static void
buddy_init(void) {
    for (size_t i = 0; i <= MAX_ORDER; i++) {
        list_init(&free_area[i].free_list);
        free_area[i].nr_free = 0;
    }
}

/* 把 [base, base + n) 切成若干自然对齐的 2 的幂块.
 * 当前块从 cur 开始, 能取到多大的 order 同时受三个条件限制:
 *   1) 2^(order+1) <= 剩余页数 left
 *   2) order < MAX_ORDER
 *   3) cur 的页号第 order 位为 0, 即页号能被 2^(order+1) 整除 —— 再翻倍
 *      就会越过它自己的自然边界, 以后永远合并不回去.
 * 因此最终得到的 order 满足: 块首页号能被 2^order 整除(块首按块大小对齐). */
static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p;
    for (p = base; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    struct Page *cur = base;
    size_t left = n;
    while (left > 0) {
        size_t ppn = ppn_of(cur);
        size_t order = 0;
        while (order < MAX_ORDER && ((size_t)1 << (order + 1)) <= left &&
               ((ppn >> order) & 1) == 0) {
            order++;
        }
        struct Page *head = cur;
        head->property = order;
        SetPageProperty(head);
        list_add(&free_area[order].free_list, &head->page_link);
        free_area[order].nr_free++;
        cur += (size_t)1 << order;
        left -= (size_t)1 << order;
    }
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);

    /* 需要 order = ceil(log2(n)) 的块; 超过 MAX_ORDER 无法满足 */
    size_t order = order_of(n);
    if (order > MAX_ORDER) {
        return NULL;
    }

    /* 从 order 往上找第一个非空链表 */
    size_t cur = order;
    while (cur <= MAX_ORDER && list_empty(&free_area[cur].free_list)) {
        cur++;
    }
    if (cur > MAX_ORDER) {
        return NULL;
    }

    /* 取出 order = cur 的整块 */
    struct Page *page = le2page(list_next(&free_area[cur].free_list), page_link);
    list_del(&page->page_link);
    free_area[cur].nr_free--;
    ClearPageProperty(page);

    /* 对半分裂直到 order: 块 [page, page + 2^cur) 劈成两半,
     * 低半 [page, page + 2^(cur-1)) 留作继续分裂, 高半挂回 order-1 链表.
     * 高半的起点就是 page + 2^(cur-1), 它天然是 order-1 的伙伴. */
    while (cur > order) {
        cur--;
        struct Page *buddy = page + ((size_t)1 << cur);
        assert(!PageProperty(buddy));
        buddy->property = cur;
        SetPageProperty(buddy);
        list_add(&free_area[cur].free_list, &buddy->page_link);
        free_area[cur].nr_free++;
    }

    page->property = 0;
    return page;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);

    /* 先把待归还的页清干净: 不能是保留页, 也不能已经是空闲块首页 */
    struct Page *p;
    for (p = base; p != base + n; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    /* n 向上取整到 2^k: 只归还 5 页时, 实际归还是它所在的 2^3 块 */
    size_t order = order_of(n);
    assert(order <= MAX_ORDER);

    /* 先把待归还的块登记成 order 的空闲块首页, 再从底向上反复合并;
     * 合并不发生时这一登记就是最终结果. */
    struct Page *page = base;
    page->property = order;
    SetPageProperty(page);

    /* 自底向上反复合并: 伙伴必须是"同 order 的空闲块首页" */
    while (order < MAX_ORDER) {
        struct Page *buddy = buddy_of(page, order);
        if (buddy == NULL) {
            break;              /* 伙伴页号越界: 没有描述符, 不能合并 */
        }
        if (!PageProperty(buddy) || buddy->property != order) {
            break;              /* 伙伴存在但不是这一档的空闲块首页 */
        }

        /* 合并: 伙伴摘链, 两个 order 块合成一个 order+1 块,
         * 块首取地址较小者 (保证合并后的块按 2^(order+1) 页对齐) */
        list_del(&buddy->page_link);
        free_area[order].nr_free--;
        ClearPageProperty(buddy);
        buddy->property = 0;
        if (buddy < page) {
            ClearPageProperty(page);
            page->property = 0;
            page = buddy;
        }
        order++;
        page->property = order;
        SetPageProperty(page);
    }

    list_add(&free_area[order].free_list, &page->page_link);
    free_area[order].nr_free++;
}

static size_t
buddy_nr_free_pages(void) {
    size_t total = 0;
    for (size_t i = 0; i <= MAX_ORDER; i++) {
        total += (size_t)free_area[i].nr_free << i;
    }
    return total;
}

/* ------------------------------------------------------------------------- *
 * 自检
 *
 * 测试只通过 alloc_pages() / free_pages() / nr_free_pages() 这些全局接口操作
 * 分配器, 不改 pmm_manager; 直接读 free_area[] 和 Page 字段只是为了观察内部
 * 状态. 所有断言都带描述, 失败时先用 cprintf 指出"哪一条行为"出错, 再 panic
 * 打出条件本身和源码行号.
 *
 * 用例分布:
 *   (a) 单页 / 2 的幂 / 非 2 的幂请求的向上取整
 *   (b) 分裂: 剩余部分按 2 的幂挂回各 order
 *   (c) 伙伴判定: 只有"同 order 且页号只差第 order 位"才合并
 *   (d) 合并链: 一对伙伴归还后连环向上合并到 order 4
 *   (e) 耗尽: 全部拿光后必须返回 NULL, 不重复分配
 *   (f) 一致性: sum(块数 * 2^order) == nr_free_pages(), 每步都验
 *   (g) 不重叠: 连续分配 8 块, 返回区间两两不相交
 *
 * 结束前把拿走的页全部还回去, 空闲池恢复到进入 check 前的页数.
 * ------------------------------------------------------------------------- */

/* 带描述的断言: cond 不成立时打印 msg, 再 panic 打出条件与行号 */
#define buddy_assert(cond, msg, ...)                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            cprintf("buddy_check FAILED at %s:%d: ", __FILE__, __LINE__);  \
            cprintf(msg "\n", ##__VA_ARGS__);                              \
            panic("buddy_check: %s", #cond);                               \
        }                                                                  \
    } while (0)

/* order 链表上的空闲块个数 */
static size_t
buddy_blocks(size_t order) {
    size_t n = 0;
    list_entry_t *le = &free_area[order].free_list;
    while ((le = list_next(le)) != &free_area[order].free_list) {
        n++;
    }
    return n;
}

/* order 链表上的第一个空闲块首页; 空链表返回 NULL */
static struct Page *
buddy_head(size_t order) {
    if (list_empty(&free_area[order].free_list)) {
        return NULL;
    }
    return le2page(list_next(&free_area[order].free_list), page_link);
}

/* page 是否挂在 order 的空闲链表上 */
static int
buddy_linked(size_t order, struct Page *page) {
    list_entry_t *le = &free_area[order].free_list;
    while ((le = list_next(le)) != &free_area[order].free_list) {
        if (le2page(le, page_link) == page) {
            return 1;
        }
    }
    return 0;
}

/* (f) 一致性检查, 每个测试步骤之后调用:
 *   - 空闲块首页必须置 PG_property, 且 property == 所在链表的 order
 *   - 块首页必须落在描述符数组内, 且页号按 2^order 对齐
 *   - 同一 order 的两个伙伴不会同时是空闲块 (存在就说明该合并而没合并)
 *   - 链表实际块数 == free_area[order].nr_free
 *   - sum(块数 * 2^order) == nr_free_pages()
 * 最后一条就是要求的"任意时刻两种口径的空闲页数必须相等". */
static void
buddy_assert_consistent(const char *what) {
    size_t total = 0;
    for (size_t i = 0; i <= MAX_ORDER; i++) {
        size_t blocks = 0;
        list_entry_t *le = &free_area[i].free_list;
        while ((le = list_next(le)) != &free_area[i].free_list) {
            struct Page *p = le2page(le, page_link);
            struct Page *b = buddy_of(p, i);
            buddy_assert(PageProperty(p),
                         "%s: order %lu 链上的块首页 0x%lx 没有置 PG_property",
                         what, (unsigned long)i, (unsigned long)(uintptr_t)p);
            buddy_assert(p->property == i,
                         "%s: 块 0x%lx 挂在 order %lu 链上, property 却是 %u",
                         what, (unsigned long)(uintptr_t)p, (unsigned long)i,
                         p->property);
            buddy_assert(p >= pages && p < pages + (npage - nbase),
                         "%s: 块首页 0x%lx 不在描述符数组内",
                         what, (unsigned long)(uintptr_t)p);
            buddy_assert((ppn_of(p) & (((size_t)1 << i) - 1)) == 0,
                         "%s: order %lu 的块页号 0x%lx 没有按 2^%lu 对齐",
                         what, (unsigned long)i, (unsigned long)ppn_of(p),
                         (unsigned long)i);
            buddy_assert(b == NULL || !PageProperty(b) || b->property != i,
                         "%s: order %lu 的伙伴 0x%lx 与 0x%lx 都空闲却没合并",
                         what, (unsigned long)i, (unsigned long)ppn_of(p),
                         (unsigned long)(b == NULL ? 0 : ppn_of(b)));
            blocks++;
        }
        buddy_assert(blocks == free_area[i].nr_free,
                     "%s: order %lu 链表长度 %lu, 但 nr_free = %u",
                     what, (unsigned long)i, (unsigned long)blocks,
                     free_area[i].nr_free);
        total += blocks << i;
    }
    buddy_assert(total == buddy_nr_free_pages(),
                 "%s: sum(块数 * 2^order) = %lu, 但 nr_free_pages() = %lu",
                 what, (unsigned long)total,
                 (unsigned long)buddy_nr_free_pages());
}

static void
buddy_check(void) {
    buddy_assert_consistent("init");
    size_t free0 = buddy_nr_free_pages();
    buddy_assert(free0 > 0, "init: 空闲池是空的, 无法测试");

    /* =====================================================================
     * (a) 单页 / 多页 / 非 2 的幂: 请求必须按 2^k 向上取整.
     *     关键是比 nr_free_pages() 的减少量: 要 1 页少 1, 要 2 页少 2,
     *     要 3 页少 4, 要 5 页少 8. 若实现"按 n 页扣账", 这里的数会对不上.
     * =================================================================== */
    struct Page *p1 = alloc_pages(1);
    buddy_assert(p1 != NULL, "(a) alloc_pages(1) 返回 NULL (空闲 %lu 页)",
                 (unsigned long)free0);
    buddy_assert(!PageProperty(p1),
                 "(a) alloc_pages(1) 返回的块首 0x%lx 仍带着 PG_property",
                 (unsigned long)(uintptr_t)p1);
    buddy_assert(nr_free_pages() == free0 - 1,
                 "(a) alloc_pages(1) 后空闲页 = %lu, 期望 %lu",
                 (unsigned long)nr_free_pages(), (unsigned long)(free0 - 1));
    buddy_assert_consistent("(a) 单页分配后");

    struct Page *p2 = alloc_pages(2);
    buddy_assert(p2 != NULL, "(a) alloc_pages(2) 返回 NULL");
    buddy_assert((ppn_of(p2) & 1) == 0,
                 "(a) order-1 块页号 0x%lx 没有按 2 页对齐",
                 (unsigned long)ppn_of(p2));
    buddy_assert(nr_free_pages() == free0 - 3,
                 "(a) alloc_pages(2) 后空闲页 = %lu, 期望 %lu",
                 (unsigned long)nr_free_pages(), (unsigned long)(free0 - 3));
    buddy_assert_consistent("(a) 两页分配后");

    struct Page *p3 = alloc_pages(3);
    buddy_assert(p3 != NULL, "(a) alloc_pages(3) 返回 NULL");
    buddy_assert((ppn_of(p3) & 3) == 0,
                 "(a) 3 页应取整到 order 2 (4 页对齐), 实际页号 0x%lx",
                 (unsigned long)ppn_of(p3));
    buddy_assert(nr_free_pages() == free0 - 7,
                 "(a) alloc_pages(3) 应取整成 4 页, 空闲页 = %lu, 期望 %lu",
                 (unsigned long)nr_free_pages(), (unsigned long)(free0 - 7));
    buddy_assert_consistent("(a) 三页分配后");

    struct Page *p5 = alloc_pages(5);
    buddy_assert(p5 != NULL, "(a) alloc_pages(5) 返回 NULL");
    buddy_assert((ppn_of(p5) & 7) == 0,
                 "(a) 5 页应取整到 order 3 (8 页对齐), 实际页号 0x%lx",
                 (unsigned long)ppn_of(p5));
    buddy_assert(nr_free_pages() == free0 - 15,
                 "(a) alloc_pages(5) 应取整成 8 页, 空闲页 = %lu, 期望 %lu",
                 (unsigned long)nr_free_pages(), (unsigned long)(free0 - 15));
    buddy_assert_consistent("(a) 五页分配后");

    /* 超过 2^MAX_ORDER 页的请求必须被拒绝, 且不能动空闲池 */
    buddy_assert(alloc_pages(((size_t)1 << MAX_ORDER) + 1) == NULL,
                 "(a) alloc_pages(2^%d + 1) 应该返回 NULL", MAX_ORDER);
    buddy_assert(nr_free_pages() == free0 - 15,
                 "(a) 被拒绝的请求改变了空闲页数: %lu",
                 (unsigned long)nr_free_pages());

    free_pages(p1, 1);
    free_pages(p2, 2);
    free_pages(p3, 3);
    free_pages(p5, 5);
    buddy_assert(nr_free_pages() == free0,
                 "(a) 归还后空闲页 = %lu, 期望恢复到 %lu",
                 (unsigned long)nr_free_pages(), (unsigned long)free0);
    buddy_assert_consistent("(a) 归还后");

    /* =====================================================================
     * (g) 不重叠: 连续分配 8 块不同大小, 每块占用的区间
     *     [首页, 首页 + 2^order) 必须两两不相交; 顺便校验每块的块大小都
     *     取整到了 2 的幂. 分配器若漏摘链表 / 分裂时算错偏移, 这里立刻重叠.
     * =================================================================== */
    static const size_t sizes[8] = {1, 2, 3, 5, 8, 4, 2, 1};
    struct Page *blk[8];
    size_t span[8];
    for (size_t i = 0; i < 8; i++) {
        blk[i] = alloc_pages(sizes[i]);
        span[i] = (size_t)1 << order_of(sizes[i]);
        buddy_assert(blk[i] != NULL,
                     "(g) 第 %lu 次分配 (要 %lu 页) 返回 NULL",
                     (unsigned long)i, (unsigned long)sizes[i]);
        buddy_assert(blk[i] >= pages && blk[i] + span[i] <= pages + (npage - nbase),
                     "(g) 第 %lu 块 [0x%lx, +%lu) 越出描述符数组",
                     (unsigned long)i, (unsigned long)(uintptr_t)blk[i],
                     (unsigned long)span[i]);
    }
    for (size_t i = 0; i < 8; i++) {
        for (size_t j = i + 1; j < 8; j++) {
            buddy_assert(blk[i] + span[i] <= blk[j] || blk[j] + span[j] <= blk[i],
                         "(g) 第 %lu 块 [页号 %lu, +%lu) 与第 %lu 块 [页号 %lu, +%lu) 重叠",
                         (unsigned long)i, (unsigned long)ppn_of(blk[i]),
                         (unsigned long)span[i], (unsigned long)j,
                         (unsigned long)ppn_of(blk[j]), (unsigned long)span[j]);
        }
    }
    buddy_assert_consistent("(g) 8 块同时分配后");

    for (size_t i = 0; i < 8; i++) {
        free_pages(blk[i], sizes[i]);
    }
    buddy_assert(nr_free_pages() == free0,
                 "(g) 归还 8 块后空闲页 = %lu, 期望 %lu",
                 (unsigned long)nr_free_pages(), (unsigned long)free0);
    buddy_assert_consistent("(g) 归还后");

    /* =====================================================================
     * (e) 耗尽: 一次一页把空闲池拿光, 之后必须干净地返回 NULL.
     *     内核里没有 malloc, 借用已分配页自己的 page_link 串一条临时链表,
     *     同时借用 ref 当"这一页已被拿走"的标记, 能抓出"同一页分配两次".
     * =================================================================== */
    list_entry_t allocated;
    list_init(&allocated);
    size_t got = 0;
    while (1) {
        struct Page *p = alloc_pages(1);
        if (p == NULL) {
            break;
        }
        buddy_assert(page_ref(p) == 0,
                     "(e) 页 0x%lx 被分配了两次 (ref 已经是 %d)",
                     (unsigned long)(uintptr_t)p, page_ref(p));
        buddy_assert(!PageProperty(p),
                     "(e) 分配出的页 0x%lx 仍带着 PG_property",
                     (unsigned long)(uintptr_t)p);
        set_page_ref(p, 1);
        p->property = 0;
        list_add_before(&allocated, &p->page_link);
        got++;
    }
    buddy_assert(got == free0,
                 "(e) 只分配出 %lu 页, 空闲池里本来有 %lu 页",
                 (unsigned long)got, (unsigned long)free0);
    buddy_assert(nr_free_pages() == 0,
                 "(e) 耗尽后 nr_free_pages() = %lu, 期望 0",
                 (unsigned long)nr_free_pages());
    buddy_assert(alloc_pages(1) == NULL,
                 "(e) 耗尽后 alloc_pages(1) 必须返回 NULL");
    buddy_assert(alloc_pages(16) == NULL,
                 "(e) 耗尽后 alloc_pages(16) 必须返回 NULL");
    buddy_assert_consistent("(e) 耗尽后");

    /* =====================================================================
     * (b) 分裂: 挑一段 16 页对齐、且全部在我们手上的连续页, 逐页归还让它们
     *     合并成一个 order-4 块; 然后只从里面要 1 页.
     *     此时其余内存全被占着, 剩下的 15 页的归属是唯一确定的:
     *     order 3 剩 A+8, order 2 剩 A+4, order 1 剩 A+2, order 0 剩 A+1.
     *     任何"挂错 order / 挂错半边"的实现都会在这四个断言上现形.
     * =================================================================== */
    struct Page *A = NULL;
    for (size_t i = 0; i + 16 <= npage - nbase; i++) {
        if ((i & 15) != 0 || PageReserved(&pages[i])) {
            continue;
        }
        size_t k = 0;
        while (k < 16 && !PageReserved(&pages[i + k])) {
            k++;
        }
        if (k == 16) {
            A = &pages[i];
            break;
        }
    }
    buddy_assert(A != NULL, "(b) 找不到 16 页对齐的连续可分配页做受控测试");

    for (size_t k = 0; k < 16; k++) {
        buddy_assert(!PageProperty(A + k),
                     "(b) 页 0x%lx 不在我们手上 (PG_property 仍置位)",
                     (unsigned long)(uintptr_t)(A + k));
        list_del(&(A + k)->page_link);          /* 从临时链表摘掉 */
        free_pages(A + k, 1);
    }
    buddy_assert(buddy_blocks(4) == 1 && buddy_head(4) == A,
                 "(b) 连续归还 16 页应合并成 order-4 块 0x%lx, "
                 "实际 order-4 块数 = %lu",
                 (unsigned long)(uintptr_t)A, (unsigned long)buddy_blocks(4));
    buddy_assert(nr_free_pages() == 16,
                 "(b) 归还 16 页后空闲页 = %lu, 期望 16",
                 (unsigned long)nr_free_pages());
    buddy_assert_consistent("(b) 造出 order-4 块后");

    struct Page *s = alloc_pages(1);
    buddy_assert(s == A,
                 "(b) alloc_pages(1) 应返回 order-4 块首页 0x%lx, 实际 0x%lx",
                 (unsigned long)(uintptr_t)A, (unsigned long)(uintptr_t)s);
    buddy_assert(buddy_blocks(3) == 1 && buddy_head(3) == A + 8,
                 "(b) 分裂后 order 3 应剩下高半块 0x%lx, 实际块数 = %lu",
                 (unsigned long)(uintptr_t)(A + 8),
                 (unsigned long)buddy_blocks(3));
    buddy_assert(buddy_blocks(2) == 1 && buddy_head(2) == A + 4,
                 "(b) 分裂后 order 2 应剩下 0x%lx, 实际块数 = %lu",
                 (unsigned long)(uintptr_t)(A + 4),
                 (unsigned long)buddy_blocks(2));
    buddy_assert(buddy_blocks(1) == 1 && buddy_head(1) == A + 2,
                 "(b) 分裂后 order 1 应剩下 0x%lx, 实际块数 = %lu",
                 (unsigned long)(uintptr_t)(A + 2),
                 (unsigned long)buddy_blocks(1));
    buddy_assert(buddy_blocks(0) == 1 && buddy_head(0) == A + 1,
                 "(b) 分裂后 order 0 应剩下 0x%lx, 实际块数 = %lu",
                 (unsigned long)(uintptr_t)(A + 1),
                 (unsigned long)buddy_blocks(0));
    buddy_assert(nr_free_pages() == 15,
                 "(b) 要 1 页后应剩 15 页, 实际 %lu",
                 (unsigned long)nr_free_pages());
    buddy_assert_consistent("(b) 分裂到 order 0 后");

    /* 正好 2/4/8 页的请求应原样整块取走, 不应再分裂 */
    struct Page *s4 = alloc_pages(4);
    buddy_assert(s4 == A + 4,
                 "(b) alloc_pages(4) 应整块拿走 order-2 块 0x%lx, 实际 0x%lx",
                 (unsigned long)(uintptr_t)(A + 4), (unsigned long)(uintptr_t)s4);
    buddy_assert(buddy_blocks(2) == 0,
                 "(b) 正好 4 页的请求不该分裂, order 2 却还有 %lu 块",
                 (unsigned long)buddy_blocks(2));
    struct Page *s2 = alloc_pages(2);
    buddy_assert(s2 == A + 2,
                 "(b) alloc_pages(2) 应整块拿走 order-1 块 0x%lx, 实际 0x%lx",
                 (unsigned long)(uintptr_t)(A + 2), (unsigned long)(uintptr_t)s2);
    buddy_assert(buddy_blocks(1) == 0,
                 "(b) 正好 2 页的请求不该分裂, order 1 却还有 %lu 块",
                 (unsigned long)buddy_blocks(1));
    struct Page *s8 = alloc_pages(8);
    buddy_assert(s8 == A + 8,
                 "(b) alloc_pages(8) 应整块拿走 order-3 块 0x%lx, 实际 0x%lx",
                 (unsigned long)(uintptr_t)(A + 8), (unsigned long)(uintptr_t)s8);
    buddy_assert(buddy_blocks(3) == 0,
                 "(b) 正好 8 页的请求不该分裂, order 3 却还有 %lu 块",
                 (unsigned long)buddy_blocks(3));
    buddy_assert(nr_free_pages() == 1,
                 "(b) 只剩 A+1 一页, 实际 %lu", (unsigned long)nr_free_pages());
    buddy_assert_consistent("(b) 取走各半块后");

    /* =====================================================================
     * (c) 伙伴判定: 只有"同 order 且页号只差第 order 位"才是伙伴.
     *     此时空闲池里只有 A+1 (order 0). 归还 A+2 后, 它与 A+1 地址相邻,
     *     却不是伙伴: order 0 要求两块页号 XOR == 1 (只差第 0 位), 而
     *     A+1 与 A+2 的页号 XOR == 3 (第 0、1 位都不同); 并且 A+2 真正的
     *     伙伴 A+3 此刻仍被占用. 所以两块都必须留在 order 0.
     *     若实现"地址相邻就合并", 这里会冒出一个 order-1 块.
     * =================================================================== */
    free_pages(A + 2, 1);
    buddy_assert(buddy_blocks(0) == 2 && buddy_linked(0, A + 1) &&
                 buddy_linked(0, A + 2),
                 "(c) 相邻但非伙伴的 A+1/A+2 必须各自留在 order 0, "
                 "实际 order-0 块数 = %lu", (unsigned long)buddy_blocks(0));
    buddy_assert(buddy_blocks(1) == 0,
                 "(c) 非伙伴的相邻块不得合并, order 1 却出现了 %lu 块",
                 (unsigned long)buddy_blocks(1));
    buddy_assert(nr_free_pages() == 2,
                 "(c) 归还 A+2 后空闲页 = %lu, 期望 2",
                 (unsigned long)nr_free_pages());
    buddy_assert_consistent("(c) 非伙伴不合并后");

    /* =====================================================================
     * (d) 合并链: 归还 A+3 (它的伙伴 A+2 正空闲) -> 并成 order-1 @ A+2;
     *     归还 A+4..A+7、A+8..A+15 后, 再归还 A: A 与 A+1 并成 order-1,
     *     接着依次撞上 A+2 / A+4 / A+8, 一路 0->1->2->3->4 连环向上合并.
     * =================================================================== */
    free_pages(A + 3, 1);
    buddy_assert(buddy_blocks(0) == 1 && buddy_head(0) == A + 1,
                 "(d) A+2/A+3 应合并成 order-1 块 0x%lx, order 0 只剩 A+1 0x%lx, "
                 "实际 order-0 块数 = %lu",
                 (unsigned long)(uintptr_t)(A + 2),
                 (unsigned long)(uintptr_t)(A + 1),
                 (unsigned long)buddy_blocks(0));
    buddy_assert(buddy_blocks(1) == 1 && buddy_head(1) == A + 2,
                 "(d) 一对伙伴归还后应合并成 order-1 块 0x%lx, 实际块数 = %lu",
                 (unsigned long)(uintptr_t)(A + 2),
                 (unsigned long)buddy_blocks(1));
    buddy_assert(nr_free_pages() == 3,
                 "(d) 归还 A+3 后空闲页 = %lu, 期望 3",
                 (unsigned long)nr_free_pages());
    buddy_assert_consistent("(d) 一对伙伴合并后");

    free_pages(A + 4, 4);
    buddy_assert(buddy_blocks(2) == 1 && buddy_head(2) == A + 4,
                 "(d) 归还 A+4..A+7 应成为 order-2 块 0x%lx "
                 "(它的 order-2 伙伴 A 仍被占用, 不能合并), 实际块数 = %lu",
                 (unsigned long)(uintptr_t)(A + 4),
                 (unsigned long)buddy_blocks(2));
    free_pages(A + 8, 8);
    buddy_assert(buddy_blocks(3) == 1 && buddy_head(3) == A + 8,
                 "(d) 归还 A+8..A+15 应成为 order-3 块 0x%lx, 实际块数 = %lu",
                 (unsigned long)(uintptr_t)(A + 8),
                 (unsigned long)buddy_blocks(3));
    buddy_assert(nr_free_pages() == 15,
                 "(d) 归还各半块后空闲页 = %lu, 期望 15",
                 (unsigned long)nr_free_pages());
    buddy_assert_consistent("(d) 归还各半块后");

    free_pages(A, 1);
    buddy_assert(buddy_blocks(0) == 0 && buddy_blocks(1) == 0 &&
                 buddy_blocks(2) == 0 && buddy_blocks(3) == 0,
                 "(d) 归还 A 后必须连环向上合并, 但 order 0..3 仍有 "
                 "%lu/%lu/%lu/%lu 块",
                 (unsigned long)buddy_blocks(0), (unsigned long)buddy_blocks(1),
                 (unsigned long)buddy_blocks(2), (unsigned long)buddy_blocks(3));
    buddy_assert(buddy_blocks(4) == 1 && buddy_head(4) == A,
                 "(d) 16 页应连环合并成一个 order-4 块 0x%lx, "
                 "实际 order-4 块数 = %lu",
                 (unsigned long)(uintptr_t)A, (unsigned long)buddy_blocks(4));
    buddy_assert(nr_free_pages() == 16,
                 "(d) 连环合并后空闲页 = %lu, 期望 16",
                 (unsigned long)nr_free_pages());
    buddy_assert_consistent("(d) 连环合并到 order 4 后");

    /* =====================================================================
     * 收尾: A..A+15 已经是一个空闲块, 不再重复归还; 把临时链表里剩下的页
     * 逐页还回去, 空闲池必须回到进入 check 时的页数.
     * =================================================================== */
    while (!list_empty(&allocated)) {
        list_entry_t *le = list_next(&allocated);
        struct Page *p = le2page(le, page_link);
        list_del(le);
        free_pages(p, 1);
    }
    buddy_assert(nr_free_pages() == free0,
                 "收尾: 空闲页 = %lu, 期望恢复成 %lu",
                 (unsigned long)nr_free_pages(), (unsigned long)free0);
    buddy_assert_consistent("收尾");
    cprintf("buddy_check: passed (%lu free pages)\n", (unsigned long)free0);
}

/* ------------------------------------------------------------------------- *
 * 管理器
 * ------------------------------------------------------------------------- */

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
