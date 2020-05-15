#ifndef __MM_CMA_H__
#define __MM_CMA_H__

#define IS_GCMA ((struct gcma *)(void *)0xFF)

struct cma {
#if 0
	bool is_rbin;
#endif
	unsigned long   base_pfn;
	unsigned long   count;
	unsigned long   *bitmap;
	unsigned int order_per_bit; /* Order of pages represented by one bit */
	struct mutex    lock;
	struct gcma	*gcma;
#ifdef CONFIG_CMA_DEBUGFS
	const char	*name;
	struct hlist_head mem_head;
	spinlock_t mem_head_lock;
#endif
	const char *name;
};

extern struct cma cma_areas[MAX_CMA_AREAS];
extern unsigned cma_area_count;

static inline unsigned long cma_bitmap_maxno(struct cma *cma)
{
	return cma->count >> cma->order_per_bit;
}

#endif
