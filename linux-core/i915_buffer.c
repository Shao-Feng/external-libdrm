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
/*
 * Authors: Thomas Hellstr�m <thomas-at-tungstengraphics-dot-com>
 */

#include "drmP.h"
#include "i915_drm.h"
#include "i915_drv.h"


drm_ttm_backend_t *i915_create_ttm_backend_entry(drm_device_t * dev)
{
	return drm_agp_init_ttm(dev, NULL);
}

int i915_fence_types(uint32_t buffer_flags, uint32_t * class, uint32_t * type)
{
	*class = 0;
	if (buffer_flags & (DRM_BO_FLAG_READ | DRM_BO_FLAG_WRITE))
		*type = 3;
	else
		*type = 1;
	return 0;
}

int i915_invalidate_caches(drm_device_t * dev, uint32_t flags)
{
	/*
	 * FIXME: Only emit once per batchbuffer submission.
	 */

	uint32_t flush_cmd = MI_NO_WRITE_FLUSH;

	if (flags & DRM_BO_FLAG_READ)
		flush_cmd |= MI_READ_FLUSH;
	if (flags & DRM_BO_FLAG_EXE)
		flush_cmd |= MI_EXE_FLUSH;

	return i915_emit_mi_flush(dev, flush_cmd);
}

int i915_init_mem_type(drm_device_t *dev, uint32_t type, 
		       drm_mem_type_manager_t *man)
{
	switch(type) {
	case DRM_BO_MEM_LOCAL:
		man->flags = _DRM_FLAG_MEMTYPE_MAPPABLE |
			_DRM_FLAG_MEMTYPE_CACHED;
		break;
	case DRM_BO_MEM_TT:
		if (!(drm_core_has_AGP(dev) && dev->agp)) {
			DRM_ERROR("AGP is not enabled for memory type %u\n", 
				  (unsigned) type);
			return -EINVAL;
		}
		man->io_offset = dev->agp->agp_info.aper_base;
		man->io_size = dev->agp->agp_info.aper_size * 1024 * 1024;
		man->io_addr = NULL;
		man->flags = _DRM_FLAG_MEMTYPE_MAPPABLE |
			_DRM_FLAG_MEMTYPE_CSELECT |
			_DRM_FLAG_NEEDS_IOREMAP;
		break;
	case DRM_BO_MEM_PRIV0:
		if (!(drm_core_has_AGP(dev) && dev->agp)) {
			DRM_ERROR("AGP is not enabled for memory type %u\n", 
				  (unsigned) type);
			return -EINVAL;
		}
		man->io_offset = dev->agp->agp_info.aper_base;
		man->io_size = dev->agp->agp_info.aper_size * 1024 * 1024;
		man->io_addr = NULL;
		man->flags = _DRM_FLAG_MEMTYPE_MAPPABLE |
			_DRM_FLAG_MEMTYPE_FIXED |
			_DRM_FLAG_NEEDS_IOREMAP;

		break;
	default:
		DRM_ERROR("Unsupported memory type %u\n", (unsigned) type);
		return -EINVAL;
	}
	return 0;
}

uint32_t i915_evict_flags(drm_device_t *dev, uint32_t type)
{
	switch(type) {
	case DRM_BO_MEM_LOCAL:
	case DRM_BO_MEM_TT:
		return DRM_BO_FLAG_MEM_LOCAL;
	default:
		return DRM_BO_FLAG_MEM_TT;
	}
}

static void i915_emit_copy_blit(drm_device_t *dev,
			 uint32_t src_offset,
			 uint32_t dst_offset,
			 uint32_t pages,
			 int direction)
{
	uint32_t cur_pages;
	uint32_t stride = PAGE_SIZE;
	drm_i915_private_t *dev_priv = dev->dev_private;
	RING_LOCALS;

	if (!dev_priv)
		return;
	
	i915_kernel_lost_context(dev);
	while(pages > 0) {
		cur_pages = pages;
		if (cur_pages > 2048)
			cur_pages = 2048;
		pages -= cur_pages;

		BEGIN_LP_RING(6);
		OUT_RING(SRC_COPY_BLT_CMD | XY_SRC_COPY_BLT_WRITE_ALPHA |
			 XY_SRC_COPY_BLT_WRITE_RGB);
		OUT_RING((stride & 0xffff) | ( 0xcc << 16) | (1 << 24) | 
			 (1 << 25) | (direction ? (1 << 30) : 0));
		OUT_RING((cur_pages << 16) | PAGE_SIZE);
		OUT_RING(dst_offset);
		OUT_RING(stride & 0xffff);
		OUT_RING(src_offset);
		ADVANCE_LP_RING();
	}
	return;
}

static int i915_move_blit(drm_buffer_object_t *bo,
			    int evict,
			    int no_wait,
			    drm_bo_mem_reg_t *new_mem)
{
	drm_bo_mem_reg_t *old_mem = &bo->mem;
	int dir = 0;

	if ((old_mem->mem_type == new_mem->mem_type) && 
	    (new_mem->mm_node->start < 
	     old_mem->mm_node->start +  old_mem->mm_node->size)) {
		dir = 1;
	}

	i915_emit_copy_blit(bo->dev,
			    old_mem->mm_node->start << PAGE_SHIFT,
			    new_mem->mm_node->start << PAGE_SHIFT,
			    new_mem->num_pages,
			    dir);

	i915_emit_mi_flush(bo->dev, MI_READ_FLUSH | MI_EXE_FLUSH);

	return drm_bo_move_accel_cleanup(bo, evict, no_wait,
					 DRM_FENCE_TYPE_EXE |
					 DRM_I915_FENCE_TYPE_RW, 
					 DRM_I915_FENCE_FLAG_FLUSHED, 
					 new_mem);
}

/*
 * Flip destination ttm into cached-coherent AGP, 
 * then blit and subsequently move out again.
 */


static int i915_move_flip(drm_buffer_object_t *bo,
			  int evict,
			  int no_wait,
			  drm_bo_mem_reg_t *new_mem)
{
	drm_device_t *dev = bo->dev;
	drm_bo_mem_reg_t tmp_mem;
	int ret;

	tmp_mem = *new_mem;
	tmp_mem.mm_node = NULL;
	tmp_mem.mask = DRM_BO_FLAG_MEM_TT |
	        DRM_BO_FLAG_CACHED  |
		DRM_BO_FLAG_FORCE_CACHING;
	
	ret = drm_bo_mem_space(dev, &tmp_mem, no_wait);
	if (ret) 
		return ret;
	
	ret = drm_bind_ttm(bo->ttm, 1, tmp_mem.mm_node->start);
	if (ret) 
		goto out_cleanup;

	ret = i915_move_blit(bo, 1, no_wait, &tmp_mem);
	if (ret) 
		goto out_cleanup;
	
	ret = drm_bo_move_ttm(bo, evict, no_wait, new_mem);
out_cleanup:
	if (tmp_mem.mm_node) {
		mutex_lock(&dev->struct_mutex);
		drm_mm_put_block(tmp_mem.mm_node);
		tmp_mem.mm_node = NULL;
		mutex_unlock(&dev->struct_mutex);
	}
	return ret;
}

	
int i915_move(drm_buffer_object_t *bo,
	      int evict,
	      int no_wait,
	      drm_bo_mem_reg_t *new_mem)
{
	drm_bo_mem_reg_t *old_mem = &bo->mem;

	if (old_mem->mem_type == DRM_BO_MEM_LOCAL) {
		return drm_bo_move_memcpy(bo, evict, no_wait, new_mem);
	} else if (new_mem->mem_type == DRM_BO_MEM_LOCAL) {
		if (i915_move_flip(bo, evict, no_wait, new_mem)) 
			return drm_bo_move_memcpy(bo, evict, no_wait, new_mem);
	} else {
		if (i915_move_blit(bo, evict, no_wait, new_mem)) 
			return drm_bo_move_memcpy(bo, evict, no_wait, new_mem);
	}
	return 0;
}
