/**************************************************************************
 * 
 * Copyright 2006 Tungsten Graphics, Inc., Bismarck, ND., USA
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR 
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE 
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * 
 **************************************************************************/

#include "drmP.h"

static void drm_ttm_ipi_handler(void *null)
{
	flush_agp_cache();
}

static void drm_ttm_cache_flush(void) 
{
	if (on_each_cpu(drm_ttm_ipi_handler, NULL, 1, 1) != 0)
		DRM_ERROR("Timed out waiting for drm cache flush.\n");
}


/*
 * Use kmalloc if possible. Otherwise fall back to vmalloc.
 */

static void ttm_alloc_pages(drm_ttm_t *ttm)
{
	unsigned long size = ttm->num_pages * sizeof(*ttm->pages);
	ttm->pages = NULL;

	if (drm_alloc_memctl(size))
		return;

	if (size <= PAGE_SIZE) {
		ttm->pages = drm_calloc(1, size, DRM_MEM_TTM);
	}
	if (!ttm->pages) {
		ttm->pages = vmalloc_user(size);
		if (ttm->pages)
			ttm->page_flags |= DRM_TTM_PAGE_VMALLOC;
	}
	if (!ttm->pages) {
		drm_free_memctl(size);
	}
}

static void ttm_free_pages(drm_ttm_t *ttm)
{
	unsigned long size = ttm->num_pages * sizeof(*ttm->pages);

	if (ttm->page_flags & DRM_TTM_PAGE_VMALLOC) {
		vfree(ttm->pages);
		ttm->page_flags &= ~DRM_TTM_PAGE_VMALLOC;
	} else {
		drm_free(ttm->pages, size, DRM_MEM_TTM);
	}
	drm_free_memctl(size);
	ttm->pages = NULL;
}


struct page *drm_ttm_alloc_page(void)
{
	struct page *page;

	if (drm_alloc_memctl(PAGE_SIZE)) {
		return NULL;
	}
	page = alloc_page(GFP_KERNEL | __GFP_ZERO | GFP_DMA32);
	if (!page) {
		drm_free_memctl(PAGE_SIZE);
		return NULL;
	}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15))
	SetPageLocked(page);
#else
	SetPageReserved(page);
#endif
	return page;
}


/*
 * Change caching policy for the linear kernel map 
 * for range of pages in a ttm.
 */

static int drm_set_caching(drm_ttm_t * ttm, int noncached)
{
	int i;
	struct page **cur_page;
	int do_tlbflush = 0;

	if ((ttm->page_flags & DRM_TTM_PAGE_UNCACHED) == noncached)
		return 0;

	if (noncached) 
		drm_ttm_cache_flush();

	for (i = 0; i < ttm->num_pages; ++i) {
		cur_page = ttm->pages + i;
		if (*cur_page) {
			if (!PageHighMem(*cur_page)) {
				if (noncached) {
					map_page_into_agp(*cur_page);
				} else {
					unmap_page_from_agp(*cur_page);
				}
				do_tlbflush = 1;
			}
		}
	}
	if (do_tlbflush)
		flush_agp_mappings();

	DRM_MASK_VAL(ttm->page_flags, DRM_TTM_PAGE_UNCACHED, noncached);

	return 0;
}

/*
 * Free all resources associated with a ttm.
 */

int drm_destroy_ttm(drm_ttm_t * ttm)
{

	int i;
	struct page **cur_page;
	drm_ttm_backend_t *be;

	if (!ttm)
		return 0;

	be = ttm->be;
	if (be) {
		be->destroy(be);
		ttm->be = NULL;
	}

	if (ttm->pages) {
		drm_buffer_manager_t *bm = &ttm->dev->bm;
		if (ttm->page_flags & DRM_TTM_PAGE_UNCACHED)
			drm_set_caching(ttm, 0);

		for (i = 0; i < ttm->num_pages; ++i) {
			cur_page = ttm->pages + i;
			if (*cur_page) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15))
				unlock_page(*cur_page);
#else
				ClearPageReserved(*cur_page);
#endif
				if (page_count(*cur_page) != 1) {
					DRM_ERROR("Erroneous page count. "
						  "Leaking pages.\n");
				}
				if (page_mapped(*cur_page)) {
					DRM_ERROR("Erroneous map count. "
						  "Leaking page mappings.\n");
				}
				__free_page(*cur_page);
				drm_free_memctl(PAGE_SIZE);
				--bm->cur_pages;
			}
		}
		ttm_free_pages(ttm);
	}

	drm_ctl_free(ttm, sizeof(*ttm), DRM_MEM_TTM);
	return 0;
}

static int drm_ttm_populate(drm_ttm_t * ttm)
{
	struct page *page;
	unsigned long i;
	drm_buffer_manager_t *bm;
	drm_ttm_backend_t *be;

	if (ttm->state != ttm_unpopulated)
		return 0;

	bm = &ttm->dev->bm;
	be = ttm->be;
	for (i = 0; i < ttm->num_pages; ++i) {
		page = ttm->pages[i];
		if (!page) {
			page = drm_ttm_alloc_page();
			if (!page)
				return -ENOMEM;
			ttm->pages[i] = page;
			++bm->cur_pages;
		}
	}
	be->populate(be, ttm->num_pages, ttm->pages);
	ttm->state = ttm_unbound;
	return 0;
}

/*
 * Initialize a ttm.
 */

drm_ttm_t *drm_ttm_init(struct drm_device *dev, unsigned long size)
{
	drm_bo_driver_t *bo_driver = dev->driver->bo_driver;
	drm_ttm_t *ttm;

	if (!bo_driver)
		return NULL;

	ttm = drm_ctl_calloc(1, sizeof(*ttm), DRM_MEM_TTM);
	if (!ttm)
		return NULL;

	ttm->dev = dev;
	atomic_set(&ttm->vma_count, 0);

	ttm->destroy = 0;
	ttm->num_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;

	ttm->page_flags = 0;

	/*
	 * Account also for AGP module memory usage.
	 */

	ttm_alloc_pages(ttm);
	if (!ttm->pages) {
		drm_destroy_ttm(ttm);
		DRM_ERROR("Failed allocating page table\n");
		return NULL;
	}
	ttm->be = bo_driver->create_ttm_backend_entry(dev);
	if (!ttm->be) {
		drm_destroy_ttm(ttm);
		DRM_ERROR("Failed creating ttm backend entry\n");
		return NULL;
	}
	ttm->state = ttm_unpopulated;
	return ttm;
}

/*
 * Unbind a ttm region from the aperture.
 */

void drm_ttm_evict(drm_ttm_t * ttm)
{
	drm_ttm_backend_t *be = ttm->be;
	int ret;

	if (ttm->state == ttm_bound) {
		ret = be->unbind(be);
		BUG_ON(ret);
	}

	ttm->state = ttm_evicted;
}

void drm_ttm_fixup_caching(drm_ttm_t * ttm)
{

	if (ttm->state == ttm_evicted) {
		drm_ttm_backend_t *be = ttm->be;
		if (be->needs_ub_cache_adjust(be)) {
			drm_set_caching(ttm, 0);
		}
		ttm->state = ttm_unbound;
	}
}

void drm_ttm_unbind(drm_ttm_t * ttm)
{
	if (ttm->state == ttm_bound)
		drm_ttm_evict(ttm);

	drm_ttm_fixup_caching(ttm);
}

int drm_bind_ttm(drm_ttm_t * ttm, int cached, unsigned long aper_offset)
{

	int ret = 0;
	drm_ttm_backend_t *be;

	if (!ttm)
		return -EINVAL;
	if (ttm->state == ttm_bound)
		return 0;

	be = ttm->be;

	ret = drm_ttm_populate(ttm);
	if (ret)
		return ret;

	if (ttm->state == ttm_unbound && !cached) {
		drm_set_caching(ttm, DRM_TTM_PAGE_UNCACHED);
	}

	if ((ret = be->bind(be, aper_offset, cached))) {
		ttm->state = ttm_evicted;
		DRM_ERROR("Couldn't bind backend.\n");
		return ret;
	}

	ttm->aper_offset = aper_offset;
	ttm->state = ttm_bound;

	return 0;
}
