#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <defs.h>

struct kmem_cache;

void slub_init(void);
struct kmem_cache *kmem_cache_create(const char *name, size_t size);
void *kmem_cache_alloc(struct kmem_cache *cache);
void kmem_cache_free(struct kmem_cache *cache, void *obj);

void *kmalloc(size_t size);
void kfree(void *obj);

/* Runs a self-contained page-backed object-allocation test. */
void slub_check(void);

#endif /* ! __KERN_MM_SLUB_H__ */
