/**************************************************************************
 * 
 * Copyright (c) 2006-2007 Tungsten Graphics, Inc., Cedar Park, TX., USA
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
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR 
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE 
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/
/*
 * Authors: Thomas Hellstr�m <thomas-at-tungstengraphics-dot-com>
 */

#include "drmP.h"

/*
 * Locking may look a bit complicated but isn't really:
 *
 * The buffer usage atomic_t needs to be protected by dev->struct_mutex
 * when there is a chance that it can be zero before or after the operation.
 * 
 * dev->struct_mutex also protects all lists and list heads. Hash tables and hash
 * heads.
 *
 * bo->mutex protects the buffer object itself excluding the usage field.
 * bo->mutex does also protect the buffer list heads, so to manipulate those, we need
 * both the bo->mutex and the dev->struct_mutex.
 *
 * Locking order is bo->mutex, dev->struct_mutex. Therefore list traversal is a bit
 * complicated. When dev->struct_mutex is released to grab bo->mutex, the list
 * traversal will, in general, need to be restarted.
 *
 */

static void drm_bo_destroy_locked(drm_buffer_object_t * bo);
static int drm_bo_setup_vm_locked(drm_buffer_object_t * bo);
static void drm_bo_takedown_vm_locked(drm_buffer_object_t * bo);
static void drm_bo_unmap_virtual(drm_buffer_object_t * bo);

static inline uint32_t drm_bo_type_flags(unsigned type)
{
	return (1 << (24 + type));
}

/*
 * bo locked. dev->struct_mutex locked.
 */

void drm_bo_add_to_pinned_lru(drm_buffer_object_t * bo)
{
	drm_mem_type_manager_t *man;

	man = &bo->dev->bm.man[bo->pinned_mem_type];
	list_add_tail(&bo->pinned_lru, &man->pinned);
}

void drm_bo_add_to_lru(drm_buffer_object_t * bo)
{
	drm_mem_type_manager_t *man;

	if (bo->mem.mm_node != bo->pinned_node) {
		man = &bo->dev->bm.man[bo->mem.mem_type];
		list_add_tail(&bo->lru, &man->lru);
	} else
		INIT_LIST_HEAD(&bo->lru);
}

static int drm_bo_vm_pre_move(drm_buffer_object_t * bo, int old_is_pci)
{
#ifdef DRM_ODD_MM_COMPAT
	int ret;

	ret = drm_bo_lock_kmm(bo);
	if (ret)
		return ret;
	drm_bo_unmap_virtual(bo);
	if (old_is_pci)
		drm_bo_finish_unmap(bo);
#else
	drm_bo_unmap_virtual(bo);
#endif
	return 0;
}

static void drm_bo_vm_post_move(drm_buffer_object_t * bo)
{
#ifdef DRM_ODD_MM_COMPAT
	int ret;

	ret = drm_bo_remap_bound(bo);
	if (ret) {
		DRM_ERROR("Failed to remap a bound buffer object.\n"
			  "\tThis might cause a sigbus later.\n");
	}
	drm_bo_unlock_kmm(bo);
#endif
}

/*
 * Call bo->mutex locked.
 */

static int drm_bo_add_ttm(drm_buffer_object_t * bo)
{
	drm_device_t *dev = bo->dev;
	int ret = 0;
	bo->ttm = NULL;

	switch (bo->type) {
	case drm_bo_type_dc:
		bo->ttm = drm_ttm_init(dev, bo->mem.num_pages << PAGE_SHIFT);
		if (!bo->ttm)
			ret = -ENOMEM;
		break;
	case drm_bo_type_user:
	case drm_bo_type_fake:
		break;
	default:
		DRM_ERROR("Illegal buffer object type\n");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int drm_bo_handle_move_mem(drm_buffer_object_t * bo,
				  drm_bo_mem_reg_t * mem,
				  int evict, int no_wait)
{
	drm_device_t *dev = bo->dev;
	drm_buffer_manager_t *bm = &dev->bm;
	int old_is_pci = drm_mem_reg_is_pci(dev, &bo->mem);
	int new_is_pci = drm_mem_reg_is_pci(dev, mem);
	drm_mem_type_manager_t *old_man = &bm->man[bo->mem.mem_type];
	drm_mem_type_manager_t *new_man = &bm->man[mem->mem_type];
	int ret = 0;

	if (old_is_pci || new_is_pci)
		ret = drm_bo_vm_pre_move(bo, old_is_pci);
	if (ret)
		return ret;

	/*
	 * Create and bind a ttm if required.
	 */

	if (!(new_man->flags & _DRM_FLAG_MEMTYPE_FIXED) && (bo->ttm == NULL)) {
		ret = drm_bo_add_ttm(bo);
		if (ret)
			goto out_err;

		if (mem->mem_type != DRM_BO_MEM_LOCAL) {
			ret = drm_bind_ttm(bo->ttm, new_man->flags &
					   DRM_BO_FLAG_CACHED,
					   mem->mm_node->start);
			if (ret)
				goto out_err;
		}
	}

	if ((bo->mem.mem_type == DRM_BO_MEM_LOCAL) && bo->ttm == NULL) {

		drm_bo_mem_reg_t *old_mem = &bo->mem;
		uint32_t save_flags = old_mem->flags;
		uint32_t save_mask = old_mem->mask;

		*old_mem = *mem;
		mem->mm_node = NULL;
		old_mem->mask = save_mask;
		DRM_FLAG_MASKED(save_flags, mem->flags, DRM_BO_MASK_MEMTYPE);

	} else if (!(old_man->flags & _DRM_FLAG_MEMTYPE_FIXED) &&
		   !(new_man->flags & _DRM_FLAG_MEMTYPE_FIXED)) {

		ret = drm_bo_move_ttm(bo, evict, no_wait, mem);

	} else if (dev->driver->bo_driver->move) {
		ret = dev->driver->bo_driver->move(bo, evict, no_wait, mem);

	} else {

		ret = drm_bo_move_memcpy(bo, evict, no_wait, mem);

	}

	if (ret)
		goto out_err;

	if (old_is_pci || new_is_pci)
		drm_bo_vm_post_move(bo);

	if (bo->priv_flags & _DRM_BO_FLAG_EVICTED) {
		ret =
		    dev->driver->bo_driver->invalidate_caches(dev,
							      bo->mem.flags);
		if (ret)
			DRM_ERROR("Can not flush read caches\n");
	}

	DRM_FLAG_MASKED(bo->priv_flags,
			(evict) ? _DRM_BO_FLAG_EVICTED : 0,
			_DRM_BO_FLAG_EVICTED);

	if (bo->mem.mm_node)
		bo->offset = bo->mem.mm_node->start << PAGE_SHIFT;

	return 0;

      out_err:
	if (old_is_pci || new_is_pci)
		drm_bo_vm_post_move(bo);

	new_man = &bm->man[bo->mem.mem_type];
	if ((new_man->flags & _DRM_FLAG_MEMTYPE_FIXED) && bo->ttm) {
		drm_ttm_unbind(bo->ttm);
		drm_destroy_ttm(bo->ttm);
		bo->ttm = NULL;
	}

	return ret;
}

/*
 * Call bo->mutex locked.
 * Wait until the buffer is idle.
 */

int drm_bo_wait(drm_buffer_object_t * bo, int lazy, int ignore_signals,
		int no_wait)
{

	drm_fence_object_t *fence = bo->fence;
	int ret;

	if (fence) {
		drm_device_t *dev = bo->dev;
		if (drm_fence_object_signaled(fence, bo->fence_type)) {
			drm_fence_usage_deref_unlocked(dev, fence);
			bo->fence = NULL;
			return 0;
		}
		if (no_wait) {
			return -EBUSY;
		}
		ret =
		    drm_fence_object_wait(dev, fence, lazy, ignore_signals,
					  bo->fence_type);
		if (ret)
			return ret;

		drm_fence_usage_deref_unlocked(dev, fence);
		bo->fence = NULL;

	}
	return 0;
}

static int drm_bo_expire_fence(drm_buffer_object_t * bo, int allow_errors)
{
	drm_device_t *dev = bo->dev;
	drm_buffer_manager_t *bm = &dev->bm;

	if (bo->fence) {
		if (bm->nice_mode) {
			unsigned long _end = jiffies + 3 * DRM_HZ;
			int ret;
			do {
				ret = drm_bo_wait(bo, 0, 1, 0);
				if (ret && allow_errors)
					return ret;

			} while (ret && !time_after_eq(jiffies, _end));

			if (bo->fence) {
				bm->nice_mode = 0;
				DRM_ERROR("Detected GPU lockup or "
					  "fence driver was taken down. "
					  "Evicting buffer.\n");
			}
		}
		if (bo->fence) {
			drm_fence_usage_deref_unlocked(dev, bo->fence);
			bo->fence = NULL;
		}
	}
	return 0;
}

/*
 * Call dev->struct_mutex locked.
 * Attempts to remove all private references to a buffer by expiring its
 * fence object and removing from lru lists and memory managers.
 */

static void drm_bo_cleanup_refs(drm_buffer_object_t * bo, int remove_all)
{
	drm_device_t *dev = bo->dev;
	drm_buffer_manager_t *bm = &dev->bm;

	atomic_inc(&bo->usage);
	mutex_unlock(&dev->struct_mutex);
	mutex_lock(&bo->mutex);

	DRM_FLAG_MASKED(bo->priv_flags, 0, _DRM_BO_FLAG_UNFENCED);

	if (bo->fence && drm_fence_object_signaled(bo->fence, bo->fence_type)) {
		drm_fence_usage_deref_locked(dev, bo->fence);
		bo->fence = NULL;
	}

	if (bo->fence && remove_all)
		(void)drm_bo_expire_fence(bo, 0);

	mutex_lock(&dev->struct_mutex);

	if (!atomic_dec_and_test(&bo->usage)) {
		goto out;
	}

	if (!bo->fence) {
		list_del_init(&bo->lru);
		if (bo->mem.mm_node) {
			drm_mm_put_block(bo->mem.mm_node);
			bo->mem.mm_node = NULL;
		}
		list_del_init(&bo->pinned_lru);
		if (bo->pinned_node) {
			drm_mm_put_block(bo->pinned_node);
			bo->pinned_node = NULL;
		}
		list_del_init(&bo->ddestroy);
		mutex_unlock(&bo->mutex);
		drm_bo_destroy_locked(bo);
		return;
	}

	if (list_empty(&bo->ddestroy)) {
		drm_fence_object_flush(dev, bo->fence, bo->fence_type);
		list_add_tail(&bo->ddestroy, &bm->ddestroy);
		schedule_delayed_work(&bm->wq,
				      ((DRM_HZ / 100) < 1) ? 1 : DRM_HZ / 100);
	}

      out:
	mutex_unlock(&bo->mutex);
	return;
}

/*
 * Verify that refcount is 0 and that there are no internal references
 * to the buffer object. Then destroy it.
 */

static void drm_bo_destroy_locked(drm_buffer_object_t * bo)
{
	drm_device_t *dev = bo->dev;
	drm_buffer_manager_t *bm = &dev->bm;

	if (list_empty(&bo->lru) && bo->mem.mm_node == NULL &&
	    list_empty(&bo->pinned_lru) && bo->pinned_node == NULL &&
	    list_empty(&bo->ddestroy) && atomic_read(&bo->usage) == 0) {
		BUG_ON(bo->fence != NULL);

#ifdef DRM_ODD_MM_COMPAT
		BUG_ON(!list_empty(&bo->vma_list));
		BUG_ON(!list_empty(&bo->p_mm_list));
#endif

		if (bo->ttm) {
			drm_ttm_unbind(bo->ttm);
			drm_destroy_ttm(bo->ttm);
			bo->ttm = NULL;
		}

		atomic_dec(&bm->count);

		drm_ctl_free(bo, sizeof(*bo), DRM_MEM_BUFOBJ);

		return;
	}

	/*
	 * Some stuff is still trying to reference the buffer object.
	 * Get rid of those references.
	 */

	drm_bo_cleanup_refs(bo, 0);

	return;
}

/*
 * Call dev->struct_mutex locked.
 */

static void drm_bo_delayed_delete(drm_device_t * dev, int remove_all)
{
	drm_buffer_manager_t *bm = &dev->bm;

	drm_buffer_object_t *entry, *nentry;
	struct list_head *list, *next;

	list_for_each_safe(list, next, &bm->ddestroy) {
		entry = list_entry(list, drm_buffer_object_t, ddestroy);

		nentry = NULL;
		if (next != &bm->ddestroy) {
			nentry = list_entry(next, drm_buffer_object_t,
					    ddestroy);
			atomic_inc(&nentry->usage);
		}

		drm_bo_cleanup_refs(entry, remove_all);

		if (nentry) {
			atomic_dec(&nentry->usage);
		}
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
static void drm_bo_delayed_workqueue(void *data)
#else
static void drm_bo_delayed_workqueue(struct work_struct *work)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	drm_device_t *dev = (drm_device_t *) data;
	drm_buffer_manager_t *bm = &dev->bm;
#else
	drm_buffer_manager_t *bm =
	    container_of(work, drm_buffer_manager_t, wq.work);
	drm_device_t *dev = container_of(bm, drm_device_t, bm);
#endif

	DRM_DEBUG("Delayed delete Worker\n");

	mutex_lock(&dev->struct_mutex);
	if (!bm->initialized) {
		mutex_unlock(&dev->struct_mutex);
		return;
	}
	drm_bo_delayed_delete(dev, 0);
	if (bm->initialized && !list_empty(&bm->ddestroy)) {
		schedule_delayed_work(&bm->wq,
				      ((DRM_HZ / 100) < 1) ? 1 : DRM_HZ / 100);
	}
	mutex_unlock(&dev->struct_mutex);
}

void drm_bo_usage_deref_locked(drm_buffer_object_t * bo)
{
	if (atomic_dec_and_test(&bo->usage)) {
		drm_bo_destroy_locked(bo);
	}
}

static void drm_bo_base_deref_locked(drm_file_t * priv, drm_user_object_t * uo)
{
	drm_buffer_object_t *bo =
	    drm_user_object_entry(uo, drm_buffer_object_t, base);

	drm_bo_takedown_vm_locked(bo);
	drm_bo_usage_deref_locked(bo);
}

static void drm_bo_usage_deref_unlocked(drm_buffer_object_t * bo)
{
	drm_device_t *dev = bo->dev;

	if (atomic_dec_and_test(&bo->usage)) {
		mutex_lock(&dev->struct_mutex);
		if (atomic_read(&bo->usage) == 0)
			drm_bo_destroy_locked(bo);
		mutex_unlock(&dev->struct_mutex);
	}
}

/*
 * Note. The caller has to register (if applicable) 
 * and deregister fence object usage.
 */

int drm_fence_buffer_objects(drm_file_t * priv,
			     struct list_head *list,
			     uint32_t fence_flags,
			     drm_fence_object_t * fence,
			     drm_fence_object_t ** used_fence)
{
	drm_device_t *dev = priv->head->dev;
	drm_buffer_manager_t *bm = &dev->bm;

	drm_buffer_object_t *entry;
	uint32_t fence_type = 0;
	int count = 0;
	int ret = 0;
	struct list_head *l;
	LIST_HEAD(f_list);

	mutex_lock(&dev->struct_mutex);

	if (!list)
		list = &bm->unfenced;

	list_for_each_entry(entry, list, lru) {
		BUG_ON(!(entry->priv_flags & _DRM_BO_FLAG_UNFENCED));
		fence_type |= entry->fence_type;
		if (entry->fence_class != 0) {
			DRM_ERROR("Fence class %d is not implemented yet.\n",
				  entry->fence_class);
			ret = -EINVAL;
			goto out;
		}
		count++;
	}

	if (!count) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Transfer to a local list before we release the dev->struct_mutex;
	 * This is so we don't get any new unfenced objects while fencing 
	 * the ones we already have..
	 */

	list_splice_init(list, &f_list);

	if (fence) {
		if ((fence_type & fence->type) != fence_type) {
			DRM_ERROR("Given fence doesn't match buffers "
				  "on unfenced list.\n");
			ret = -EINVAL;
			goto out;
		}
	} else {
		mutex_unlock(&dev->struct_mutex);
		ret = drm_fence_object_create(dev, fence_type,
					      fence_flags | DRM_FENCE_FLAG_EMIT,
					      &fence);
		mutex_lock(&dev->struct_mutex);
		if (ret)
			goto out;
	}

	count = 0;
	l = f_list.next;
	while (l != &f_list) {
		entry = list_entry(l, drm_buffer_object_t, lru);
		atomic_inc(&entry->usage);
		mutex_unlock(&dev->struct_mutex);
		mutex_lock(&entry->mutex);
		mutex_lock(&dev->struct_mutex);
		list_del_init(l);
		if (entry->priv_flags & _DRM_BO_FLAG_UNFENCED) {
			count++;
			if (entry->fence)
				drm_fence_usage_deref_locked(dev, entry->fence);
			entry->fence = fence;
			DRM_FLAG_MASKED(entry->priv_flags, 0,
					_DRM_BO_FLAG_UNFENCED);
			DRM_WAKEUP(&entry->event_queue);
			drm_bo_add_to_lru(entry);
		}
		mutex_unlock(&entry->mutex);
		drm_bo_usage_deref_locked(entry);
		l = f_list.next;
	}
	atomic_add(count, &fence->usage);
	DRM_DEBUG("Fenced %d buffers\n", count);
      out:
	mutex_unlock(&dev->struct_mutex);
	*used_fence = fence;
	return ret;
}

EXPORT_SYMBOL(drm_fence_buffer_objects);

/*
 * bo->mutex locked 
 */

static int drm_bo_evict(drm_buffer_object_t * bo, unsigned mem_type,
			int no_wait)
{
	int ret = 0;
	drm_device_t *dev = bo->dev;
	drm_bo_mem_reg_t evict_mem;

	/*
	 * Someone might have modified the buffer before we took the buffer mutex.
	 */

	if (bo->priv_flags & _DRM_BO_FLAG_UNFENCED)
		goto out;
	if (bo->mem.mem_type != mem_type)
		goto out;

	ret = drm_bo_wait(bo, 0, 0, no_wait);

	if (ret && ret != -EAGAIN) {
		DRM_ERROR("Failed to expire fence before "
			  "buffer eviction.\n");
		goto out;
	}

	evict_mem = bo->mem;
	evict_mem.mm_node = NULL;

	if (bo->type == drm_bo_type_fake) {
		bo->mem.mem_type = DRM_BO_MEM_LOCAL;
		bo->mem.mm_node = NULL;
		bo->pinned_mem_type = DRM_BO_MEM_LOCAL;
		bo->pinned_node = NULL;
		goto out1;
	}

	evict_mem = bo->mem;
	evict_mem.mask = dev->driver->bo_driver->evict_flags(dev, mem_type);
	ret = drm_bo_mem_space(bo, &evict_mem, no_wait);

	if (ret) {
		if (ret != -EAGAIN)
			DRM_ERROR("Failed to find memory space for "
				  "buffer eviction.\n");
		goto out;
	}

	if (bo->pinned_node)
		DRM_ERROR("Evicting pinned buffer\n");

	ret = drm_bo_handle_move_mem(bo, &evict_mem, 1, no_wait);

	if (ret) {
		if (ret != -EAGAIN)
			DRM_ERROR("Buffer eviction failed\n");
		goto out;
	}

      out1:
	mutex_lock(&dev->struct_mutex);
	if (evict_mem.mm_node) {
		drm_mm_put_block(evict_mem.mm_node);
		evict_mem.mm_node = NULL;
	}
	list_del(&bo->lru);
	drm_bo_add_to_lru(bo);
	mutex_unlock(&dev->struct_mutex);

	DRM_FLAG_MASKED(bo->priv_flags, _DRM_BO_FLAG_EVICTED,
			_DRM_BO_FLAG_EVICTED);

      out:
	return ret;
}

static int drm_bo_mem_force_space(drm_device_t * dev,
				  drm_bo_mem_reg_t * mem,
				  uint32_t mem_type, int no_wait)
{
	drm_mm_node_t *node;
	drm_buffer_manager_t *bm = &dev->bm;
	drm_buffer_object_t *entry;
	drm_mem_type_manager_t *man = &bm->man[mem_type];
	struct list_head *lru;
	unsigned long num_pages = mem->num_pages;
	int ret;

	mutex_lock(&dev->struct_mutex);
	do {
		node = drm_mm_search_free(&man->manager, num_pages,
					  mem->page_alignment, 1);
		if (node)
			break;

		lru = &man->lru;
		if (lru->next == lru)
			break;

		entry = list_entry(lru->next, drm_buffer_object_t, lru);
		atomic_inc(&entry->usage);
		mutex_unlock(&dev->struct_mutex);
		mutex_lock(&entry->mutex);
		BUG_ON(entry->mem.
		       flags & (DRM_BO_FLAG_NO_MOVE | DRM_BO_FLAG_NO_EVICT));

		ret = drm_bo_evict(entry, mem_type, no_wait);
		mutex_unlock(&entry->mutex);
		drm_bo_usage_deref_unlocked(entry);
		if (ret)
			return ret;
		mutex_lock(&dev->struct_mutex);
	} while (1);

	if (!node) {
		mutex_unlock(&dev->struct_mutex);
		return -ENOMEM;
	}

	node = drm_mm_get_block(node, num_pages, mem->page_alignment);
	mutex_unlock(&dev->struct_mutex);
	mem->mm_node = node;
	mem->mem_type = mem_type;
	return 0;
}

static int drm_bo_mt_compatible(drm_mem_type_manager_t * man,
				uint32_t mem_type,
				uint32_t mask, uint32_t * res_mask)
{
	uint32_t cur_flags = drm_bo_type_flags(mem_type);
	uint32_t flag_diff;

	if (man->flags & _DRM_FLAG_MEMTYPE_CACHED)
		cur_flags |= DRM_BO_FLAG_CACHED;
	if (man->flags & _DRM_FLAG_MEMTYPE_MAPPABLE)
		cur_flags |= DRM_BO_FLAG_MAPPABLE;
	if (man->flags & _DRM_FLAG_MEMTYPE_CSELECT)
		DRM_FLAG_MASKED(cur_flags, mask, DRM_BO_FLAG_CACHED);

	if ((cur_flags & mask & DRM_BO_MASK_MEM) == 0) {
		return 0;
	}
	flag_diff = (mask ^ cur_flags);
	if ((flag_diff & DRM_BO_FLAG_CACHED) &&
	    (mask & DRM_BO_FLAG_FORCE_CACHING)) {
		return 0;
	}
	if ((flag_diff & DRM_BO_FLAG_MAPPABLE) &&
	    (mask & DRM_BO_FLAG_FORCE_MAPPABLE)) {
		return 0;
	}

	*res_mask = cur_flags;
	return 1;
}

int drm_bo_mem_space(drm_buffer_object_t * bo,
		     drm_bo_mem_reg_t * mem, int no_wait)
{
	drm_device_t *dev = bo->dev;
	drm_buffer_manager_t *bm = &dev->bm;
	drm_mem_type_manager_t *man;

	uint32_t num_prios = dev->driver->bo_driver->num_mem_type_prio;
	const uint32_t *prios = dev->driver->bo_driver->mem_type_prio;
	uint32_t i;
	uint32_t mem_type = DRM_BO_MEM_LOCAL;
	uint32_t cur_flags;
	int type_found = 0;
	int type_ok = 0;
	int has_eagain = 0;
	drm_mm_node_t *node = NULL;
	int ret;

	for (i = 0; i < num_prios; ++i) {
		mem_type = prios[i];
		man = &bm->man[mem_type];

		type_ok = drm_bo_mt_compatible(man, mem_type, mem->mask,
					       &cur_flags);

		if (!type_ok)
			continue;

		if (mem_type == DRM_BO_MEM_LOCAL)
			break;

		if ((mem_type == bo->pinned_mem_type) &&
		    (bo->pinned_node != NULL)) {
			DRM_ERROR("Choosing pinned region\n");
			node = bo->pinned_node;
			break;
		}

		mutex_lock(&dev->struct_mutex);
		if (man->has_type && man->use_type) {
			type_found = 1;
			node = drm_mm_search_free(&man->manager, mem->num_pages,
						  mem->page_alignment, 1);
			if (node)
				node = drm_mm_get_block(node, mem->num_pages,
							mem->page_alignment);
		}
		mutex_unlock(&dev->struct_mutex);
		if (node)
			break;
	}

	if ((type_ok && (mem_type == DRM_BO_MEM_LOCAL)) || node) {
		mem->mm_node = node;
		mem->mem_type = mem_type;
		mem->flags = cur_flags;
		return 0;
	}

	if (!type_found)
		return -EINVAL;

	num_prios = dev->driver->bo_driver->num_mem_busy_prio;
	prios = dev->driver->bo_driver->mem_busy_prio;

	for (i = 0; i < num_prios; ++i) {
		mem_type = prios[i];
		man = &bm->man[mem_type];

		if (!drm_bo_mt_compatible(man, mem_type, mem->mask, &cur_flags))
			continue;

		ret = drm_bo_mem_force_space(dev, mem, mem_type, no_wait);

		if (ret == 0) {
			mem->flags = cur_flags;
			return 0;
		}

		if (ret == -EAGAIN)
			has_eagain = 1;
	}

	ret = (has_eagain) ? -EAGAIN : -ENOMEM;
	return ret;
}

EXPORT_SYMBOL(drm_bo_mem_space);

static int drm_bo_new_mask(drm_buffer_object_t * bo,
			   uint32_t new_mask, uint32_t hint)
{
	uint32_t new_props;

	if (bo->type == drm_bo_type_user) {
		DRM_ERROR("User buffers are not supported yet\n");
		return -EINVAL;
	}
	if (bo->type == drm_bo_type_fake &&
	    !(new_mask & (DRM_BO_FLAG_NO_MOVE | DRM_BO_FLAG_NO_EVICT))) {
		DRM_ERROR("Fake buffers must be pinned.\n");
		return -EINVAL;
	}

	if ((new_mask & DRM_BO_FLAG_NO_EVICT) && !DRM_SUSER(DRM_CURPROC)) {
		DRM_ERROR
		    ("DRM_BO_FLAG_NO_EVICT is only available to priviliged "
		     "processes\n");
		return -EPERM;
	}

	new_props = new_mask & (DRM_BO_FLAG_EXE | DRM_BO_FLAG_WRITE |
				DRM_BO_FLAG_READ);

	if (!new_props) {
		DRM_ERROR("Invalid buffer object rwx properties\n");
		return -EINVAL;
	}

	bo->mem.mask = new_mask;
	return 0;
}

/*
 * Call dev->struct_mutex locked.
 */

drm_buffer_object_t *drm_lookup_buffer_object(drm_file_t * priv,
					      uint32_t handle, int check_owner)
{
	drm_user_object_t *uo;
	drm_buffer_object_t *bo;

	uo = drm_lookup_user_object(priv, handle);

	if (!uo || (uo->type != drm_buffer_type)) {
		DRM_ERROR("Could not find buffer object 0x%08x\n", handle);
		return NULL;
	}

	if (check_owner && priv != uo->owner) {
		if (!drm_lookup_ref_object(priv, uo, _DRM_REF_USE))
			return NULL;
	}

	bo = drm_user_object_entry(uo, drm_buffer_object_t, base);
	atomic_inc(&bo->usage);
	return bo;
}

/*
 * Call bo->mutex locked.
 * Returns 1 if the buffer is currently rendered to or from. 0 otherwise.
 * Doesn't do any fence flushing as opposed to the drm_bo_busy function.
 */

static int drm_bo_quick_busy(drm_buffer_object_t * bo)
{
	drm_fence_object_t *fence = bo->fence;

	BUG_ON(bo->priv_flags & _DRM_BO_FLAG_UNFENCED);
	if (fence) {
		drm_device_t *dev = bo->dev;
		if (drm_fence_object_signaled(fence, bo->fence_type)) {
			drm_fence_usage_deref_unlocked(dev, fence);
			bo->fence = NULL;
			return 0;
		}
		return 1;
	}
	return 0;
}

/*
 * Call bo->mutex locked.
 * Returns 1 if the buffer is currently rendered to or from. 0 otherwise.
 */

static int drm_bo_busy(drm_buffer_object_t * bo)
{
	drm_fence_object_t *fence = bo->fence;

	BUG_ON(bo->priv_flags & _DRM_BO_FLAG_UNFENCED);
	if (fence) {
		drm_device_t *dev = bo->dev;
		if (drm_fence_object_signaled(fence, bo->fence_type)) {
			drm_fence_usage_deref_unlocked(dev, fence);
			bo->fence = NULL;
			return 0;
		}
		drm_fence_object_flush(dev, fence, DRM_FENCE_TYPE_EXE);
		if (drm_fence_object_signaled(fence, bo->fence_type)) {
			drm_fence_usage_deref_unlocked(dev, fence);
			bo->fence = NULL;
			return 0;
		}
		return 1;
	}
	return 0;
}

static int drm_bo_read_cached(drm_buffer_object_t * bo)
{
	int ret = 0;

	BUG_ON(bo->priv_flags & _DRM_BO_FLAG_UNFENCED);
	if (bo->mem.mm_node)
		ret = drm_bo_evict(bo, DRM_BO_MEM_TT, 1);
	return ret;
}

/*
 * Wait until a buffer is unmapped.
 */

static int drm_bo_wait_unmapped(drm_buffer_object_t * bo, int no_wait)
{
	int ret = 0;

	if ((atomic_read(&bo->mapped) >= 0) && no_wait)
		return -EBUSY;

	DRM_WAIT_ON(ret, bo->event_queue, 3 * DRM_HZ,
		    atomic_read(&bo->mapped) == -1);

	if (ret == -EINTR)
		ret = -EAGAIN;

	return ret;
}

static int drm_bo_check_unfenced(drm_buffer_object_t * bo)
{
	int ret;

	mutex_lock(&bo->mutex);
	ret = (bo->priv_flags & _DRM_BO_FLAG_UNFENCED);
	mutex_unlock(&bo->mutex);
	return ret;
}

/*
 * Wait until a buffer, scheduled to be fenced moves off the unfenced list.
 * Until then, we cannot really do anything with it except delete it.
 * The unfenced list is a PITA, and the operations
 * 1) validating
 * 2) submitting commands
 * 3) fencing
 * Should really be an atomic operation. 
 * We now "solve" this problem by keeping
 * the buffer "unfenced" after validating, but before fencing.
 */

static int drm_bo_wait_unfenced(drm_buffer_object_t * bo, int no_wait,
				int eagain_if_wait)
{
	int ret = (bo->priv_flags & _DRM_BO_FLAG_UNFENCED);
	unsigned long _end = jiffies + 3 * DRM_HZ;

	if (ret && no_wait)
		return -EBUSY;
	else if (!ret)
		return 0;

	do {
		mutex_unlock(&bo->mutex);
		DRM_WAIT_ON(ret, bo->event_queue, 3 * DRM_HZ,
			    !drm_bo_check_unfenced(bo));
		mutex_lock(&bo->mutex);
		if (ret == -EINTR)
			return -EAGAIN;
		if (ret) {
			DRM_ERROR
			    ("Error waiting for buffer to become fenced\n");
			return ret;
		}
		ret = (bo->priv_flags & _DRM_BO_FLAG_UNFENCED);
	} while (ret && !time_after_eq(jiffies, _end));
	if (ret) {
		DRM_ERROR("Timeout waiting for buffer to become fenced\n");
		return ret;
	}
	if (eagain_if_wait)
		return -EAGAIN;

	return 0;
}

/*
 * Fill in the ioctl reply argument with buffer info.
 * Bo locked. 
 */

static void drm_bo_fill_rep_arg(drm_buffer_object_t * bo,
				drm_bo_arg_reply_t * rep)
{
	rep->handle = bo->base.hash.key;
	rep->flags = bo->mem.flags;
	rep->size = bo->mem.num_pages * PAGE_SIZE;
	rep->offset = bo->offset;
	rep->arg_handle = bo->map_list.user_token;
	rep->mask = bo->mem.mask;
	rep->buffer_start = bo->buffer_start;
	rep->fence_flags = bo->fence_type;
	rep->rep_flags = 0;
	rep->page_alignment = bo->mem.page_alignment;

	if ((bo->priv_flags & _DRM_BO_FLAG_UNFENCED) || drm_bo_quick_busy(bo)) {
		DRM_FLAG_MASKED(rep->rep_flags, DRM_BO_REP_BUSY,
				DRM_BO_REP_BUSY);
	}
}

/*
 * Wait for buffer idle and register that we've mapped the buffer.
 * Mapping is registered as a drm_ref_object with type _DRM_REF_TYPE1, 
 * so that if the client dies, the mapping is automatically 
 * unregistered.
 */

static int drm_buffer_object_map(drm_file_t * priv, uint32_t handle,
				 uint32_t map_flags, unsigned hint,
				 drm_bo_arg_reply_t * rep)
{
	drm_buffer_object_t *bo;
	drm_device_t *dev = priv->head->dev;
	int ret = 0;
	int no_wait = hint & DRM_BO_HINT_DONT_BLOCK;

	mutex_lock(&dev->struct_mutex);
	bo = drm_lookup_buffer_object(priv, handle, 1);
	mutex_unlock(&dev->struct_mutex);

	if (!bo)
		return -EINVAL;

	mutex_lock(&bo->mutex);
	if (!(hint & DRM_BO_HINT_ALLOW_UNFENCED_MAP)) {
		ret = drm_bo_wait_unfenced(bo, no_wait, 0);
		if (ret)
			goto out;
	}

	/*
	 * If this returns true, we are currently unmapped.
	 * We need to do this test, because unmapping can
	 * be done without the bo->mutex held.
	 */

	while (1) {
		if (atomic_inc_and_test(&bo->mapped)) {
			if (no_wait && drm_bo_busy(bo)) {
				atomic_dec(&bo->mapped);
				ret = -EBUSY;
				goto out;
			}
			ret = drm_bo_wait(bo, 0, 0, no_wait);
			if (ret) {
				atomic_dec(&bo->mapped);
				goto out;
			}

			if ((map_flags & DRM_BO_FLAG_READ) &&
			    (bo->mem.flags & DRM_BO_FLAG_READ_CACHED) &&
			    (!(bo->mem.flags & DRM_BO_FLAG_CACHED))) {
				drm_bo_read_cached(bo);
			}
			break;
		} else if ((map_flags & DRM_BO_FLAG_READ) &&
			   (bo->mem.flags & DRM_BO_FLAG_READ_CACHED) &&
			   (!(bo->mem.flags & DRM_BO_FLAG_CACHED))) {

			/*
			 * We are already mapped with different flags.
			 * need to wait for unmap.
			 */

			ret = drm_bo_wait_unmapped(bo, no_wait);
			if (ret)
				goto out;

			continue;
		}
		break;
	}

	mutex_lock(&dev->struct_mutex);
	ret = drm_add_ref_object(priv, &bo->base, _DRM_REF_TYPE1);
	mutex_unlock(&dev->struct_mutex);
	if (ret) {
		if (atomic_add_negative(-1, &bo->mapped))
			DRM_WAKEUP(&bo->event_queue);

	} else
		drm_bo_fill_rep_arg(bo, rep);
      out:
	mutex_unlock(&bo->mutex);
	drm_bo_usage_deref_unlocked(bo);
	return ret;
}

static int drm_buffer_object_unmap(drm_file_t * priv, uint32_t handle)
{
	drm_device_t *dev = priv->head->dev;
	drm_buffer_object_t *bo;
	drm_ref_object_t *ro;
	int ret = 0;

	mutex_lock(&dev->struct_mutex);

	bo = drm_lookup_buffer_object(priv, handle, 1);
	if (!bo) {
		ret = -EINVAL;
		goto out;
	}

	ro = drm_lookup_ref_object(priv, &bo->base, _DRM_REF_TYPE1);
	if (!ro) {
		ret = -EINVAL;
		goto out;
	}

	drm_remove_ref_object(priv, ro);
	drm_bo_usage_deref_locked(bo);
      out:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

/*
 * Call struct-sem locked.
 */

static void drm_buffer_user_object_unmap(drm_file_t * priv,
					 drm_user_object_t * uo,
					 drm_ref_t action)
{
	drm_buffer_object_t *bo =
	    drm_user_object_entry(uo, drm_buffer_object_t, base);

	/*
	 * We DON'T want to take the bo->lock here, because we want to
	 * hold it when we wait for unmapped buffer.
	 */

	BUG_ON(action != _DRM_REF_TYPE1);

	if (atomic_add_negative(-1, &bo->mapped))
		DRM_WAKEUP(&bo->event_queue);
}

/*
 * bo->mutex locked. 
 * Note that new_mem_flags are NOT transferred to the bo->mem.mask.
 */

int drm_bo_move_buffer(drm_buffer_object_t * bo, uint32_t new_mem_flags,
		       int no_wait, int move_unfenced)
{
	drm_device_t *dev = bo->dev;
	drm_buffer_manager_t *bm = &dev->bm;
	int ret = 0;
	drm_bo_mem_reg_t mem;
	/*
	 * Flush outstanding fences.
	 */

	drm_bo_busy(bo);

	/*
	 * Wait for outstanding fences.
	 */

	ret = drm_bo_wait(bo, 0, 0, no_wait);
	if (ret)
		return ret;

	mem.num_pages = bo->mem.num_pages;
	mem.size = mem.num_pages << PAGE_SHIFT;
	mem.mask = new_mem_flags;
	mem.page_alignment = bo->mem.page_alignment;

	mutex_lock(&bm->evict_mutex);
	mutex_lock(&dev->struct_mutex);
	list_del(&bo->lru);
	list_add_tail(&bo->lru, &bm->unfenced);
	DRM_FLAG_MASKED(bo->priv_flags, _DRM_BO_FLAG_UNFENCED,
			_DRM_BO_FLAG_UNFENCED);
	mutex_unlock(&dev->struct_mutex);

	/*
	 * Determine where to move the buffer.
	 */
	ret = drm_bo_mem_space(bo, &mem, no_wait);

	if (ret)
		goto out_unlock;

	ret = drm_bo_handle_move_mem(bo, &mem, 0, no_wait);

      out_unlock:
	if (ret || !move_unfenced) {
		mutex_lock(&dev->struct_mutex);
		if (mem.mm_node) {
			drm_mm_put_block(mem.mm_node);
			mem.mm_node = NULL;
		}
		DRM_FLAG_MASKED(bo->priv_flags, 0, _DRM_BO_FLAG_UNFENCED);
		DRM_WAKEUP(&bo->event_queue);
		list_del(&bo->lru);
		drm_bo_add_to_lru(bo);
		mutex_unlock(&dev->struct_mutex);
	}

	mutex_unlock(&bm->evict_mutex);
	return ret;
}

static int drm_bo_mem_compat(drm_bo_mem_reg_t * mem)
{
	uint32_t flag_diff = (mem->mask ^ mem->flags);

	if ((mem->mask & mem->flags & DRM_BO_MASK_MEM) == 0)
		return 0;
	if ((flag_diff & DRM_BO_FLAG_CACHED) &&
	    (mem->mask & DRM_BO_FLAG_FORCE_CACHING))
		return 0;
	if ((flag_diff & DRM_BO_FLAG_MAPPABLE) &&
	    (mem->mask & DRM_BO_FLAG_FORCE_MAPPABLE))
		return 0;
	return 1;
}

static int drm_bo_check_fake(drm_device_t * dev, drm_bo_mem_reg_t * mem)
{
	drm_buffer_manager_t *bm = &dev->bm;
	drm_mem_type_manager_t *man;
	uint32_t num_prios = dev->driver->bo_driver->num_mem_type_prio;
	const uint32_t *prios = dev->driver->bo_driver->mem_type_prio;
	uint32_t i;
	int type_ok = 0;
	uint32_t mem_type = 0;
	uint32_t cur_flags;

	if (drm_bo_mem_compat(mem))
		return 0;

	BUG_ON(mem->mm_node);

	for (i = 0; i < num_prios; ++i) {
		mem_type = prios[i];
		man = &bm->man[mem_type];
		type_ok = drm_bo_mt_compatible(man, mem_type, mem->mask,
					       &cur_flags);
		if (type_ok)
			break;
	}

	if (type_ok) {
		mem->mm_node = NULL;
		mem->mem_type = mem_type;
		mem->flags = cur_flags;
		DRM_FLAG_MASKED(mem->flags, mem->mask, ~DRM_BO_MASK_MEMTYPE);
		return 0;
	}

	DRM_ERROR("Illegal fake buffer flags 0x%08x\n", mem->mask);
	return -EINVAL;
}

/*
 * bo locked.
 */

static int drm_buffer_object_validate(drm_buffer_object_t * bo,
				      int move_unfenced, int no_wait)
{
	drm_device_t *dev = bo->dev;
	drm_buffer_manager_t *bm = &dev->bm;
	drm_bo_driver_t *driver = dev->driver->bo_driver;
	int ret;

	DRM_DEBUG("New flags 0x%08x, Old flags 0x%08x\n", bo->mem.mask,
		  bo->mem.flags);
	ret =
	    driver->fence_type(bo->mem.mask, &bo->fence_class, &bo->fence_type);
	if (ret) {
		DRM_ERROR("Driver did not support given buffer permissions\n");
		return ret;
	}

	ret = drm_bo_wait_unmapped(bo, no_wait);
	if (ret)
		return ret;

	if (bo->type == drm_bo_type_fake) {
		ret = drm_bo_check_fake(dev, &bo->mem);
		if (ret)
			return ret;
	}

	/*
	 * Check whether we need to move buffer.
	 */

	if (!drm_bo_mem_compat(&bo->mem)) {
		ret = drm_bo_move_buffer(bo, bo->mem.mask & DRM_BO_MASK_MEMTYPE,
					 no_wait, move_unfenced);
		if (ret) {
			if (ret != -EAGAIN)
				DRM_ERROR("Failed moving buffer.\n");
			return ret;
		}
	}

	/*
	 * Pinned buffers.
	 */

	if (bo->mem.mask & (DRM_BO_FLAG_NO_EVICT | DRM_BO_FLAG_NO_MOVE)) {

		bo->pinned_mem_type = bo->mem.mem_type;
		mutex_lock(&dev->struct_mutex);
		list_del_init(&bo->pinned_lru);
		drm_bo_add_to_pinned_lru(bo);

		if (bo->pinned_node != bo->mem.mm_node) {
			drm_mm_put_block(bo->pinned_node);
			bo->pinned_node = bo->mem.mm_node;
		}

		mutex_unlock(&dev->struct_mutex);

	} else if (bo->pinned_node != NULL) {

		mutex_lock(&dev->struct_mutex);
		drm_mm_put_block(bo->pinned_node);
		list_del_init(&bo->pinned_lru);
		bo->pinned_node = NULL;
		mutex_unlock(&dev->struct_mutex);

	}

	/*
	 * We might need to add a TTM.
	 */

	if (bo->mem.mem_type == DRM_BO_MEM_LOCAL && bo->ttm == NULL) {
		ret = drm_bo_add_ttm(bo);
		if (ret)
			return ret;
	}
	DRM_FLAG_MASKED(bo->mem.flags, bo->mem.mask, ~DRM_BO_MASK_MEMTYPE);

	/*
	 * Finally, adjust lru to be sure.
	 */

	mutex_lock(&dev->struct_mutex);
	list_del(&bo->lru);
	if (move_unfenced) {
		list_add_tail(&bo->lru, &bm->unfenced);
		DRM_FLAG_MASKED(bo->priv_flags, _DRM_BO_FLAG_UNFENCED,
				_DRM_BO_FLAG_UNFENCED);
	} else {
		drm_bo_add_to_lru(bo);
		if (bo->priv_flags & _DRM_BO_FLAG_UNFENCED) {
			DRM_WAKEUP(&bo->event_queue);
			DRM_FLAG_MASKED(bo->priv_flags, 0,
					_DRM_BO_FLAG_UNFENCED);
		}
	}
	mutex_unlock(&dev->struct_mutex);

	return 0;
}

static int drm_bo_handle_validate(drm_file_t * priv, uint32_t handle,
				  uint32_t flags, uint32_t mask, uint32_t hint,
				  drm_bo_arg_reply_t * rep)
{
	drm_buffer_object_t *bo;
	int ret;
	int no_wait = hint & DRM_BO_HINT_DONT_BLOCK;

	bo = drm_lookup_buffer_object(priv, handle, 1);
	if (!bo) {
		return -EINVAL;
	}

	mutex_lock(&bo->mutex);
	ret = drm_bo_wait_unfenced(bo, no_wait, 0);

	if (ret)
		goto out;

	DRM_FLAG_MASKED(flags, bo->mem.mask, ~mask);
	ret = drm_bo_new_mask(bo, flags, hint);
	if (ret)
		goto out;

	ret =
	    drm_buffer_object_validate(bo, !(hint & DRM_BO_HINT_DONT_FENCE),
				       no_wait);
	drm_bo_fill_rep_arg(bo, rep);

      out:

	mutex_unlock(&bo->mutex);

	drm_bo_usage_deref_unlocked(bo);
	return ret;
}

static int drm_bo_handle_info(drm_file_t * priv, uint32_t handle,
			      drm_bo_arg_reply_t * rep)
{
	drm_buffer_object_t *bo;

	bo = drm_lookup_buffer_object(priv, handle, 1);
	if (!bo) {
		return -EINVAL;
	}
	mutex_lock(&bo->mutex);
	if (!(bo->priv_flags & _DRM_BO_FLAG_UNFENCED))
		(void)drm_bo_busy(bo);
	drm_bo_fill_rep_arg(bo, rep);
	mutex_unlock(&bo->mutex);
	drm_bo_usage_deref_unlocked(bo);
	return 0;
}

static int drm_bo_handle_wait(drm_file_t * priv, uint32_t handle,
			      uint32_t hint, drm_bo_arg_reply_t * rep)
{
	drm_buffer_object_t *bo;
	int no_wait = hint & DRM_BO_HINT_DONT_BLOCK;
	int ret;

	bo = drm_lookup_buffer_object(priv, handle, 1);
	if (!bo) {
		return -EINVAL;
	}

	mutex_lock(&bo->mutex);
	ret = drm_bo_wait_unfenced(bo, no_wait, 0);
	if (ret)
		goto out;
	ret = drm_bo_wait(bo, hint & DRM_BO_HINT_WAIT_LAZY, 0, no_wait);
	if (ret)
		goto out;

	drm_bo_fill_rep_arg(bo, rep);

      out:
	mutex_unlock(&bo->mutex);
	drm_bo_usage_deref_unlocked(bo);
	return ret;
}

int drm_buffer_object_create(drm_file_t * priv,
			     unsigned long size,
			     drm_bo_type_t type,
			     uint32_t mask,
			     uint32_t hint,
			     uint32_t page_alignment,
			     unsigned long buffer_start,
			     drm_buffer_object_t ** buf_obj)
{
	drm_device_t *dev = priv->head->dev;
	drm_buffer_manager_t *bm = &dev->bm;
	drm_buffer_object_t *bo;
	int ret = 0;
	unsigned long num_pages;

	if ((buffer_start & ~PAGE_MASK) && (type != drm_bo_type_fake)) {
		DRM_ERROR("Invalid buffer object start.\n");
		return -EINVAL;
	}
	num_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (num_pages == 0) {
		DRM_ERROR("Illegal buffer object size.\n");
		return -EINVAL;
	}

	bo = drm_ctl_calloc(1, sizeof(*bo), DRM_MEM_BUFOBJ);

	if (!bo)
		return -ENOMEM;

	mutex_init(&bo->mutex);
	mutex_lock(&bo->mutex);

	atomic_set(&bo->usage, 1);
	atomic_set(&bo->mapped, -1);
	DRM_INIT_WAITQUEUE(&bo->event_queue);
	INIT_LIST_HEAD(&bo->lru);
	INIT_LIST_HEAD(&bo->pinned_lru);
	INIT_LIST_HEAD(&bo->ddestroy);
#ifdef DRM_ODD_MM_COMPAT
	INIT_LIST_HEAD(&bo->p_mm_list);
	INIT_LIST_HEAD(&bo->vma_list);
#endif
	bo->dev = dev;
	bo->type = type;
	bo->mem.mem_type = DRM_BO_MEM_LOCAL;
	bo->mem.num_pages = num_pages;
	bo->mem.mm_node = NULL;
	bo->mem.page_alignment = page_alignment;
	if (bo->type == drm_bo_type_fake) {
		bo->offset = buffer_start;
		bo->buffer_start = 0;
	} else {
		bo->buffer_start = buffer_start;
	}
	bo->priv_flags = 0;
	bo->mem.flags = 0;
	bo->mem.mask = 0;
	atomic_inc(&bm->count);
	ret = drm_bo_new_mask(bo, mask, hint);

	if (ret)
		goto out_err;

	if (bo->type == drm_bo_type_dc) {
		mutex_lock(&dev->struct_mutex);
		ret = drm_bo_setup_vm_locked(bo);
		mutex_unlock(&dev->struct_mutex);
		if (ret)
			goto out_err;
	}
	ret = drm_buffer_object_validate(bo, 0, hint & DRM_BO_HINT_DONT_BLOCK);
	if (ret)
		goto out_err;

	mutex_unlock(&bo->mutex);
	*buf_obj = bo;
	return 0;

      out_err:
	mutex_unlock(&bo->mutex);

	drm_bo_usage_deref_unlocked(bo);
	return ret;
}

static int drm_bo_add_user_object(drm_file_t * priv, drm_buffer_object_t * bo,
				  int shareable)
{
	drm_device_t *dev = priv->head->dev;
	int ret;

	mutex_lock(&dev->struct_mutex);
	ret = drm_add_user_object(priv, &bo->base, shareable);
	if (ret)
		goto out;

	bo->base.remove = drm_bo_base_deref_locked;
	bo->base.type = drm_buffer_type;
	bo->base.ref_struct_locked = NULL;
	bo->base.unref = drm_buffer_user_object_unmap;

      out:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

static int drm_bo_lock_test(drm_device_t * dev, struct file *filp)
{
	LOCK_TEST_WITH_RETURN(dev, filp);
	return 0;
}

int drm_bo_ioctl(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;
	drm_bo_arg_t arg;
	drm_bo_arg_request_t *req = &arg.d.req;
	drm_bo_arg_reply_t rep;
	unsigned long next;
	drm_user_object_t *uo;
	drm_buffer_object_t *entry;

	if (!dev->bm.initialized) {
		DRM_ERROR("Buffer object manager is not initialized.\n");
		return -EINVAL;
	}

	do {
		DRM_COPY_FROM_USER_IOCTL(arg, (void __user *)data, sizeof(arg));

		if (arg.handled) {
			data = arg.next;
			continue;
		}

		rep.ret = 0;
		switch (req->op) {
		case drm_bo_create:
			rep.ret =
			    drm_buffer_object_create(priv, req->size,
						     req->type,
						     req->mask,
						     req->hint,
						     req->page_alignment,
						     req->buffer_start, &entry);
			if (rep.ret)
				break;

			rep.ret =
			    drm_bo_add_user_object(priv, entry,
						   req->
						   mask &
						   DRM_BO_FLAG_SHAREABLE);
			if (rep.ret)
				drm_bo_usage_deref_unlocked(entry);

			if (rep.ret)
				break;

			mutex_lock(&entry->mutex);
			drm_bo_fill_rep_arg(entry, &rep);
			mutex_unlock(&entry->mutex);
			break;
		case drm_bo_unmap:
			rep.ret = drm_buffer_object_unmap(priv, req->handle);
			break;
		case drm_bo_map:
			rep.ret = drm_buffer_object_map(priv, req->handle,
							req->mask,
							req->hint, &rep);
			break;
		case drm_bo_destroy:
			mutex_lock(&dev->struct_mutex);
			uo = drm_lookup_user_object(priv, req->handle);
			if (!uo || (uo->type != drm_buffer_type)
			    || uo->owner != priv) {
				mutex_unlock(&dev->struct_mutex);
				rep.ret = -EINVAL;
				break;
			}
			rep.ret = drm_remove_user_object(priv, uo);
			mutex_unlock(&dev->struct_mutex);
			break;
		case drm_bo_reference:
			rep.ret = drm_user_object_ref(priv, req->handle,
						      drm_buffer_type, &uo);
			if (rep.ret)
				break;
			mutex_lock(&dev->struct_mutex);
			uo = drm_lookup_user_object(priv, req->handle);
			entry =
			    drm_user_object_entry(uo, drm_buffer_object_t,
						  base);
			atomic_dec(&entry->usage);
			mutex_unlock(&dev->struct_mutex);
			mutex_lock(&entry->mutex);
			drm_bo_fill_rep_arg(entry, &rep);
			mutex_unlock(&entry->mutex);
			break;
		case drm_bo_unreference:
			rep.ret = drm_user_object_unref(priv, req->handle,
							drm_buffer_type);
			break;
		case drm_bo_validate:
			rep.ret = drm_bo_lock_test(dev, filp);

			if (rep.ret)
				break;
			rep.ret =
			    drm_bo_handle_validate(priv, req->handle, req->mask,
						   req->arg_handle, req->hint,
						   &rep);
			break;
		case drm_bo_fence:
			rep.ret = drm_bo_lock_test(dev, filp);
			if (rep.ret)
				break;
			 /**/ break;
		case drm_bo_info:
			rep.ret = drm_bo_handle_info(priv, req->handle, &rep);
			break;
		case drm_bo_wait_idle:
			rep.ret = drm_bo_handle_wait(priv, req->handle,
						     req->hint, &rep);
			break;
		case drm_bo_ref_fence:
			rep.ret = -EINVAL;
			DRM_ERROR("Function is not implemented yet.\n");
		default:
			rep.ret = -EINVAL;
		}
		next = arg.next;

		/*
		 * A signal interrupted us. Make sure the ioctl is restartable.
		 */

		if (rep.ret == -EAGAIN)
			return -EAGAIN;

		arg.handled = 1;
		arg.d.rep = rep;
		DRM_COPY_TO_USER_IOCTL((void __user *)data, arg, sizeof(arg));
		data = next;
	} while (data);
	return 0;
}

static int drm_bo_leave_list(drm_buffer_object_t * bo,
			     uint32_t mem_type,
			     int free_pinned, int allow_errors)
{
	drm_device_t *dev = bo->dev;
	int ret = 0;

	atomic_inc(&bo->usage);
	mutex_unlock(&dev->struct_mutex);
	mutex_lock(&bo->mutex);

	ret = drm_bo_expire_fence(bo, allow_errors);
	if (ret)
		goto out;

	DRM_FLAG_MASKED(bo->priv_flags, 0, _DRM_BO_FLAG_UNFENCED);

	if (free_pinned) {
		DRM_FLAG_MASKED(bo->mem.flags, 0, DRM_BO_FLAG_NO_MOVE);
		mutex_lock(&dev->struct_mutex);
		list_del_init(&bo->pinned_lru);
		if (bo->pinned_node == bo->mem.mm_node)
			bo->pinned_node = NULL;
		if (bo->pinned_node != NULL) {
			drm_mm_put_block(bo->pinned_node);
			bo->pinned_node = NULL;
		}
		mutex_unlock(&dev->struct_mutex);
	}

	if (bo->mem.flags & DRM_BO_FLAG_NO_EVICT) {
		DRM_ERROR("A DRM_BO_NO_EVICT buffer present at "
			  "cleanup. Removing flag and evicting.\n");
		bo->mem.flags &= ~DRM_BO_FLAG_NO_EVICT;
		bo->mem.mask &= ~DRM_BO_FLAG_NO_EVICT;
	}

	if (bo->mem.mem_type == mem_type)
		ret = drm_bo_evict(bo, mem_type, 0);

	if (ret) {
		if (allow_errors) {
			goto out;
		} else {
			ret = 0;
			DRM_ERROR("Cleanup eviction failed\n");
		}
	}

      out:
	mutex_unlock(&bo->mutex);
	mutex_lock(&dev->struct_mutex);
	drm_bo_usage_deref_locked(bo);
	return ret;
}

/*
 * dev->struct_sem locked.
 */

static int drm_bo_force_list_clean(drm_device_t * dev,
				   struct list_head *head,
				   unsigned mem_type,
				   int free_pinned,
				   int allow_errors, int pinned_list)
{
	struct list_head *list, *next;
	drm_buffer_object_t *entry;
	int ret;
	int do_retry;

	/*
	 * We need to 
	 * restart if a node disappears from under us.
	 * Nodes cannot be added since the hardware lock is needed
	 * For this operation.
	 */

      retry:
	list_for_each_safe(list, next, head) {
		if (pinned_list)
			entry = list_entry(list, drm_buffer_object_t,
					   pinned_lru);
		else
			entry = list_entry(list, drm_buffer_object_t, lru);
		atomic_inc(&entry->usage);
		ret = drm_bo_leave_list(entry, mem_type, free_pinned,
					allow_errors);

		do_retry = list->next != next;
		drm_bo_usage_deref_locked(entry);

		if (ret)
			return ret;

		if (do_retry)
			goto retry;
	}
	return 0;
}

int drm_bo_clean_mm(drm_device_t * dev, unsigned mem_type)
{
	drm_buffer_manager_t *bm = &dev->bm;
	drm_mem_type_manager_t *man = &bm->man[mem_type];
	int ret = -EINVAL;

	if (mem_type >= DRM_BO_MEM_TYPES) {
		DRM_ERROR("Illegal memory type %d\n", mem_type);
		return ret;
	}

	if (!man->has_type) {
		DRM_ERROR("Trying to take down uninitialized "
			  "memory manager type\n");
		return ret;
	}
	man->use_type = 0;
	man->has_type = 0;

	ret = 0;
	if (mem_type > 0) {

		drm_bo_force_list_clean(dev, &bm->unfenced, mem_type, 1, 0, 0);
		drm_bo_force_list_clean(dev, &man->lru, mem_type, 1, 0, 0);
		drm_bo_force_list_clean(dev, &man->pinned, mem_type, 1, 0, 1);

		if (drm_mm_clean(&man->manager)) {
			drm_mm_takedown(&man->manager);
		} else {
			ret = -EBUSY;
		}
	}

	return ret;
}

/**
 *Evict all buffers of a particular mem_type, but leave memory manager
 *regions for NO_MOVE buffers intact. New buffers cannot be added at this
 *point since we have the hardware lock.
 */

static int drm_bo_lock_mm(drm_device_t * dev, unsigned mem_type)
{
	int ret;
	drm_buffer_manager_t *bm = &dev->bm;
	drm_mem_type_manager_t *man = &bm->man[mem_type];

	if (mem_type == 0 || mem_type >= DRM_BO_MEM_TYPES) {
		DRM_ERROR("Illegal memory manager memory type %u,\n", mem_type);
		return -EINVAL;
	}

	ret = drm_bo_force_list_clean(dev, &bm->unfenced, mem_type, 0, 1, 0);
	if (ret)
		return ret;
	ret = drm_bo_force_list_clean(dev, &man->lru, mem_type, 0, 1, 0);

	return ret;
}

static int drm_bo_init_mm(drm_device_t * dev,
			  unsigned type,
			  unsigned long p_offset, unsigned long p_size)
{
	drm_buffer_manager_t *bm = &dev->bm;
	int ret = -EINVAL;
	drm_mem_type_manager_t *man;

	if (type >= DRM_BO_MEM_TYPES) {
		DRM_ERROR("Illegal memory type %d\n", type);
		return ret;
	}

	man = &bm->man[type];
	if (man->has_type) {
		DRM_ERROR("Memory manager already initialized for type %d\n",
			  type);
		return ret;
	}

	ret = dev->driver->bo_driver->init_mem_type(dev, type, man);
	if (ret)
		return ret;

	ret = 0;
	if (type != DRM_BO_MEM_LOCAL) {
		if (!p_size) {
			DRM_ERROR("Zero size memory manager type %d\n", type);
			return ret;
		}
		ret = drm_mm_init(&man->manager, p_offset, p_size);
		if (ret)
			return ret;
	}
	man->has_type = 1;
	man->use_type = 1;

	INIT_LIST_HEAD(&man->lru);
	INIT_LIST_HEAD(&man->pinned);

	return 0;
}

/*
 * This is called from lastclose, so we don't need to bother about
 * any clients still running when we set the initialized flag to zero.
 */

int drm_bo_driver_finish(drm_device_t * dev)
{
	drm_buffer_manager_t *bm = &dev->bm;
	int ret = 0;
	unsigned i = DRM_BO_MEM_TYPES;
	drm_mem_type_manager_t *man;

	mutex_lock(&dev->bm.init_mutex);
	mutex_lock(&dev->struct_mutex);

	if (!bm->initialized)
		goto out;
	bm->initialized = 0;

	while (i--) {
		man = &bm->man[i];
		if (man->has_type) {
			man->use_type = 0;
			if ((i != DRM_BO_MEM_LOCAL) && drm_bo_clean_mm(dev, i)) {
				ret = -EBUSY;
				DRM_ERROR("DRM memory manager type %d "
					  "is not clean.\n", i);
			}
			man->has_type = 0;
		}
	}
	mutex_unlock(&dev->struct_mutex);

	if (!cancel_delayed_work(&bm->wq)) {
		flush_scheduled_work();
	}
	mutex_lock(&dev->struct_mutex);
	drm_bo_delayed_delete(dev, 1);
	if (list_empty(&bm->ddestroy)) {
		DRM_DEBUG("Delayed destroy list was clean\n");
	}
	if (list_empty(&bm->man[0].lru)) {
		DRM_DEBUG("Swap list was clean\n");
	}
	if (list_empty(&bm->man[0].pinned)) {
		DRM_DEBUG("NO_MOVE list was clean\n");
	}
	if (list_empty(&bm->unfenced)) {
		DRM_DEBUG("Unfenced list was clean\n");
	}
      out:
	mutex_unlock(&dev->struct_mutex);
	mutex_unlock(&dev->bm.init_mutex);
	return ret;
}

int drm_bo_driver_init(drm_device_t * dev)
{
	drm_bo_driver_t *driver = dev->driver->bo_driver;
	drm_buffer_manager_t *bm = &dev->bm;
	int ret = -EINVAL;

	mutex_lock(&dev->bm.init_mutex);
	mutex_lock(&dev->struct_mutex);
	if (!driver)
		goto out_unlock;

	/*
	 * Initialize the system memory buffer type.
	 * Other types need to be driver / IOCTL initialized.
	 */

	ret = drm_bo_init_mm(dev, 0, 0, 0);
	if (ret)
		goto out_unlock;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	INIT_WORK(&bm->wq, &drm_bo_delayed_workqueue, dev);
#else
	INIT_DELAYED_WORK(&bm->wq, drm_bo_delayed_workqueue);
#endif
	bm->initialized = 1;
	bm->nice_mode = 1;
	atomic_set(&bm->count, 0);
	bm->cur_pages = 0;
	INIT_LIST_HEAD(&bm->unfenced);
	INIT_LIST_HEAD(&bm->ddestroy);
      out_unlock:
	mutex_unlock(&dev->struct_mutex);
	mutex_unlock(&dev->bm.init_mutex);
	return ret;
}

EXPORT_SYMBOL(drm_bo_driver_init);

int drm_mm_init_ioctl(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;

	int ret = 0;
	drm_mm_init_arg_t arg;
	drm_buffer_manager_t *bm = &dev->bm;
	drm_bo_driver_t *driver = dev->driver->bo_driver;

	if (!driver) {
		DRM_ERROR("Buffer objects are not supported by this driver\n");
		return -EINVAL;
	}

	DRM_COPY_FROM_USER_IOCTL(arg, (void __user *)data, sizeof(arg));

	switch (arg.req.op) {
	case mm_init:
		ret = -EINVAL;
		mutex_lock(&dev->bm.init_mutex);
		mutex_lock(&dev->struct_mutex);
		if (!bm->initialized) {
			DRM_ERROR("DRM memory manager was not initialized.\n");
			break;
		}
		if (arg.req.mem_type == 0) {
			DRM_ERROR
			    ("System memory buffers already initialized.\n");
			break;
		}
		ret = drm_bo_init_mm(dev, arg.req.mem_type,
				     arg.req.p_offset, arg.req.p_size);
		break;
	case mm_takedown:
		LOCK_TEST_WITH_RETURN(dev, filp);
		mutex_lock(&dev->bm.init_mutex);
		mutex_lock(&dev->struct_mutex);
		ret = -EINVAL;
		if (!bm->initialized) {
			DRM_ERROR("DRM memory manager was not initialized\n");
			break;
		}
		if (arg.req.mem_type == 0) {
			DRM_ERROR("No takedown for System memory buffers.\n");
			break;
		}
		ret = 0;
		if (drm_bo_clean_mm(dev, arg.req.mem_type)) {
			DRM_ERROR("Memory manager type %d not clean. "
				  "Delaying takedown\n", arg.req.mem_type);
		}
		break;
	case mm_lock:
		LOCK_TEST_WITH_RETURN(dev, filp);
		mutex_lock(&dev->bm.init_mutex);
		mutex_lock(&dev->struct_mutex);
		ret = drm_bo_lock_mm(dev, arg.req.mem_type);
		break;
	case mm_unlock:
		LOCK_TEST_WITH_RETURN(dev, filp);
		mutex_lock(&dev->bm.init_mutex);
		mutex_lock(&dev->struct_mutex);
		ret = 0;
		break;
	default:
		DRM_ERROR("Function not implemented yet\n");
		return -EINVAL;
	}

	mutex_unlock(&dev->struct_mutex);
	mutex_unlock(&dev->bm.init_mutex);
	if (ret)
		return ret;

	DRM_COPY_TO_USER_IOCTL((void __user *)data, arg, sizeof(arg));
	return 0;
}

/*
 * buffer object vm functions.
 */

int drm_mem_reg_is_pci(drm_device_t * dev, drm_bo_mem_reg_t * mem)
{
	drm_buffer_manager_t *bm = &dev->bm;
	drm_mem_type_manager_t *man = &bm->man[mem->mem_type];

	if (!(man->flags & _DRM_FLAG_MEMTYPE_FIXED)) {
		if (mem->mem_type == DRM_BO_MEM_LOCAL)
			return 0;

		if (man->flags & _DRM_FLAG_MEMTYPE_CMA)
			return 0;

		if (mem->flags & DRM_BO_FLAG_CACHED)
			return 0;
	}
	return 1;
}

EXPORT_SYMBOL(drm_mem_reg_is_pci);

/**
 * \c Get the PCI offset for the buffer object memory.
 *
 * \param bo The buffer object.
 * \param bus_base On return the base of the PCI region
 * \param bus_offset On return the byte offset into the PCI region
 * \param bus_size On return the byte size of the buffer object or zero if
 *     the buffer object memory is not accessible through a PCI region.
 * \return Failure indication.
 * 
 * Returns -EINVAL if the buffer object is currently not mappable.
 * Otherwise returns zero.
 */

int drm_bo_pci_offset(drm_device_t * dev,
		      drm_bo_mem_reg_t * mem,
		      unsigned long *bus_base,
		      unsigned long *bus_offset, unsigned long *bus_size)
{
	drm_buffer_manager_t *bm = &dev->bm;
	drm_mem_type_manager_t *man = &bm->man[mem->mem_type];

	*bus_size = 0;
	if (!(man->flags & _DRM_FLAG_MEMTYPE_MAPPABLE))
		return -EINVAL;

	if (drm_mem_reg_is_pci(dev, mem)) {
		*bus_offset = mem->mm_node->start << PAGE_SHIFT;
		*bus_size = mem->num_pages << PAGE_SHIFT;
		*bus_base = man->io_offset;
	}

	return 0;
}

/**
 * \c Kill all user-space virtual mappings of this buffer object.
 *
 * \param bo The buffer object.
 *
 * Call bo->mutex locked.
 */

void drm_bo_unmap_virtual(drm_buffer_object_t * bo)
{
	drm_device_t *dev = bo->dev;
	loff_t offset = ((loff_t) bo->map_list.hash.key) << PAGE_SHIFT;
	loff_t holelen = ((loff_t) bo->mem.num_pages) << PAGE_SHIFT;

	unmap_mapping_range(dev->dev_mapping, offset, holelen, 1);
}

static void drm_bo_takedown_vm_locked(drm_buffer_object_t * bo)
{
	drm_map_list_t *list = &bo->map_list;
	drm_local_map_t *map;
	drm_device_t *dev = bo->dev;

	if (list->user_token) {
		drm_ht_remove_item(&dev->map_hash, &list->hash);
		list->user_token = 0;
	}
	if (list->file_offset_node) {
		drm_mm_put_block(list->file_offset_node);
		list->file_offset_node = NULL;
	}

	map = list->map;
	if (!map)
		return;

	drm_ctl_free(map, sizeof(*map), DRM_MEM_BUFOBJ);
	list->map = NULL;
	list->user_token = 0ULL;
	drm_bo_usage_deref_locked(bo);
}

static int drm_bo_setup_vm_locked(drm_buffer_object_t * bo)
{
	drm_map_list_t *list = &bo->map_list;
	drm_local_map_t *map;
	drm_device_t *dev = bo->dev;

	list->map = drm_ctl_calloc(1, sizeof(*map), DRM_MEM_BUFOBJ);
	if (!list->map)
		return -ENOMEM;

	map = list->map;
	map->offset = 0;
	map->type = _DRM_TTM;
	map->flags = _DRM_REMOVABLE;
	map->size = bo->mem.num_pages * PAGE_SIZE;
	atomic_inc(&bo->usage);
	map->handle = (void *)bo;

	list->file_offset_node = drm_mm_search_free(&dev->offset_manager,
						    bo->mem.num_pages, 0, 0);

	if (!list->file_offset_node) {
		drm_bo_takedown_vm_locked(bo);
		return -ENOMEM;
	}

	list->file_offset_node = drm_mm_get_block(list->file_offset_node,
						  bo->mem.num_pages, 0);

	list->hash.key = list->file_offset_node->start;
	if (drm_ht_insert_item(&dev->map_hash, &list->hash)) {
		drm_bo_takedown_vm_locked(bo);
		return -ENOMEM;
	}

	list->user_token = ((drm_u64_t) list->hash.key) << PAGE_SHIFT;

	return 0;
}
