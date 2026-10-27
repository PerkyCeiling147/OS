#include <slub.h>
#include <pmm.h>
#include <string.h>
#include <stdio.h>

#define SLUB_MIN_ALIGN       16
#define SLUB_MAX_CACHES      16
#define SLUB_MAX_SLABS       64
#define SLUB_CACHE_NAME_MAX  24

struct kmem_cache {
    char name[SLUB_CACHE_NAME_MAX];
    size_t object_size;
    size_t objects_per_slab;
    size_t slab_count;
    struct slub_slab *slabs;
    int in_use;
};

struct slub_slab {
    struct Page *page;
    struct kmem_cache *cache;
    void *free_list;
    size_t free_count;
    struct slub_slab *next;
    int in_use;
};

static struct kmem_cache cache_pool[SLUB_MAX_CACHES];
static struct slub_slab slab_pool[SLUB_MAX_SLABS];
static struct kmem_cache *kmalloc_cache_table[9];
static const size_t kmalloc_sizes[9] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};
static int slub_ready;

void
slub_init(void) {
    memset(cache_pool, 0, sizeof(cache_pool));
    memset(slab_pool, 0, sizeof(slab_pool));
    memset(kmalloc_cache_table, 0, sizeof(kmalloc_cache_table));
    slub_ready = 1;
}

static struct slub_slab *
slub_find_slab(struct kmem_cache *cache, struct Page *page) {
    for (struct slub_slab *slab = cache->slabs;
         slab != NULL; slab = slab->next) {
        if (slab->page == page) {
            return slab;
        }
    }
    return NULL;
}

static void *
slub_page_to_kva(struct Page *page) {
    return (void *)(page2pa(page) + va_pa_offset);
}

static struct slub_slab *
slub_new_slab(struct kmem_cache *cache) {
    struct slub_slab *slab = NULL;
    for (size_t i = 0; i < SLUB_MAX_SLABS; i++) {
        if (!slab_pool[i].in_use) {
            slab = &slab_pool[i];
            break;
        }
    }
    if (slab == NULL) {
        return NULL;
    }

    struct Page *page = alloc_pages(1);
    if (page == NULL) {
        return NULL;
    }

    slab->page = page;
    slab->cache = cache;
    slab->free_list = NULL;
    slab->free_count = cache->objects_per_slab;
    slab->in_use = 1;
    set_page_ref(page, 1);

    char *base = slub_page_to_kva(page);
    for (size_t i = cache->objects_per_slab; i > 0; i--) {
        void *obj = base + (i - 1) * cache->object_size;
        *(void **)obj = slab->free_list;
        slab->free_list = obj;
    }

    slab->next = cache->slabs;
    cache->slabs = slab;
    cache->slab_count++;
    return slab;
}

static void
slub_release_slab(struct kmem_cache *cache, struct slub_slab *slab) {
    assert(slab->cache == cache);
    assert(slab->free_count == cache->objects_per_slab);

    struct slub_slab **link = &cache->slabs;
    while (*link != slab) {
        assert(*link != NULL);
        link = &(*link)->next;
    }
    *link = slab->next;
    cache->slab_count--;

    set_page_ref(slab->page, 0);
    free_pages(slab->page, 1);

    slab->page = NULL;
    slab->cache = NULL;
    slab->free_list = NULL;
    slab->free_count = 0;
    slab->next = NULL;
    slab->in_use = 0;
}

struct kmem_cache *
kmem_cache_create(const char *name, size_t size) {
    if (!slub_ready) {
        slub_init();
    }
    if (size == 0 || size > PGSIZE) {
        return NULL;
    }

    size_t object_size = ROUNDUP(size, SLUB_MIN_ALIGN);
    if (object_size < sizeof(void *)) {
        object_size = sizeof(void *);
    }
    if (object_size > PGSIZE) {
        return NULL;
    }

    for (size_t i = 0; i < SLUB_MAX_CACHES; i++) {
        if (cache_pool[i].in_use &&
            cache_pool[i].object_size == object_size) {
            return &cache_pool[i];
        }
    }
    for (size_t i = 0; i < SLUB_MAX_CACHES; i++) {
        if (!cache_pool[i].in_use) {
            struct kmem_cache *cache = &cache_pool[i];
            memset(cache, 0, sizeof(*cache));
            if (name != NULL) {
                strncpy(cache->name, name, SLUB_CACHE_NAME_MAX - 1);
            }
            cache->object_size = object_size;
            cache->objects_per_slab = PGSIZE / object_size;
            cache->in_use = 1;
            return cache;
        }
    }
    return NULL;
}

void *
kmem_cache_alloc(struct kmem_cache *cache) {
    assert(cache != NULL && cache->in_use);

    struct slub_slab *slab = cache->slabs;
    while (slab != NULL && slab->free_count == 0) {
        slab = slab->next;
    }
    if (slab == NULL) {
        slab = slub_new_slab(cache);
        if (slab == NULL) {
            return NULL;
        }
    }

    void *obj = slab->free_list;
    assert(obj != NULL);
    slab->free_list = *(void **)obj;
    slab->free_count--;
    return obj;
}

static struct slub_slab *
slub_slab_from_object(void *obj) {
    uintptr_t va = (uintptr_t)obj;
    if (va < KERNBASE) {
        return NULL;
    }

    uintptr_t pa = PADDR(va);
    if (pa < DRAM_BASE || PPN(pa) >= npage) {
        return NULL;
    }
    struct Page *page = pa2page(pa);

    for (size_t i = 0; i < SLUB_MAX_SLABS; i++) {
        if (slab_pool[i].in_use && slab_pool[i].page == page) {
            return &slab_pool[i];
        }
    }
    return NULL;
}

static int
slub_object_is_free(struct kmem_cache *cache, struct slub_slab *slab,
                    void *obj) {
    size_t left = cache->objects_per_slab;
    for (void *p = slab->free_list; p != NULL && left > 0; left--) {
        if (p == obj) {
            return 1;
        }
        p = *(void **)p;
    }
    assert(left > 0);
    return 0;
}

void
kmem_cache_free(struct kmem_cache *cache, void *obj) {
    assert(cache != NULL && cache->in_use && obj != NULL);

    struct slub_slab *slab = slub_slab_from_object(obj);
    assert(slab != NULL && slab->cache == cache);

    char *base = slub_page_to_kva(slab->page);
    size_t offset = (char *)obj - base;
    assert(offset < cache->objects_per_slab * cache->object_size);
    assert(offset % cache->object_size == 0);
    assert(!slub_object_is_free(cache, slab, obj));

    *(void **)obj = slab->free_list;
    slab->free_list = obj;
    slab->free_count++;
    assert(slab->free_count <= cache->objects_per_slab);

    if (slab->free_count == cache->objects_per_slab &&
        cache->slab_count > 1) {
        slub_release_slab(cache, slab);
    }
}

static struct kmem_cache *
slub_kmalloc_cache(size_t size) {
    if (size == 0 || size > PGSIZE) {
        return NULL;
    }
    for (size_t i = 0; i < 9; i++) {
        if (size <= kmalloc_sizes[i]) {
            if (kmalloc_cache_table[i] == NULL) {
                kmalloc_cache_table[i] =
                    kmem_cache_create("kmalloc", kmalloc_sizes[i]);
            }
            return kmalloc_cache_table[i];
        }
    }
    return NULL;
}

void *
kmalloc(size_t size) {
    struct kmem_cache *cache = slub_kmalloc_cache(size);
    return cache == NULL ? NULL : kmem_cache_alloc(cache);
}

void
kfree(void *obj) {
    if (obj == NULL) {
        return;
    }
    struct slub_slab *slab = slub_slab_from_object(obj);
    assert(slab != NULL);
    kmem_cache_free(slab->cache, obj);
}

static void
slub_cache_drain(struct kmem_cache *cache) {
    while (cache->slabs != NULL) {
        slub_release_slab(cache, cache->slabs);
    }
    assert(cache->slab_count == 0);
}

void
slub_check(void) {
    if (!slub_ready) {
        slub_init();
    }

    size_t free_before = nr_free_pages();
    struct kmem_cache *cache = kmem_cache_create("slub-check-64", 64);
    assert(cache != NULL);
    assert(cache->object_size == 64);
    assert(cache->objects_per_slab == PGSIZE / 64);
    assert(cache->slab_count == 0);

    enum { N = 80 };
    static void *objects[N];
    for (size_t i = 0; i < N; i++) {
        objects[i] = kmem_cache_alloc(cache);
        assert(objects[i] != NULL);
        assert(((uintptr_t)objects[i] % SLUB_MIN_ALIGN) == 0);
    }
    assert(cache->slab_count >= 2);

    for (size_t i = 0; i < N; i++) {
        for (size_t j = i + 1; j < N; j++) {
            assert(objects[i] != objects[j]);
        }
    }

    for (size_t i = 0; i < N; i += 2) {
        kmem_cache_free(cache, objects[i]);
    }
    for (size_t i = 0; i < N; i += 2) {
        objects[i] = kmem_cache_alloc(cache);
        assert(objects[i] != NULL);
    }
    for (size_t i = 0; i < N; i++) {
        kmem_cache_free(cache, objects[i]);
    }
    assert(cache->slab_count == 1);
    slub_cache_drain(cache);
    assert(nr_free_pages() == free_before);

    static void *generic[32];
    for (size_t i = 0; i < 32; i++) {
        generic[i] = kmalloc(1 + i * 97);
        assert(generic[i] != NULL);
    }
    for (size_t i = 0; i < 32; i++) {
        kfree(generic[i]);
    }
    for (size_t i = 0; i < 9; i++) {
        if (kmalloc_cache_table[i] != NULL) {
            slub_cache_drain(kmalloc_cache_table[i]);
        }
    }
    assert(nr_free_pages() == free_before);

    cprintf("slub_check: passed (%u object cache test)\n",
            (unsigned)N);
}
