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
#include "drm.h"
#include "i915_drm.h"
#include "i915_drv.h"

/*
 * Implements an intel sync flush operation.
 */

static void i915_perform_flush(drm_device_t * dev)
{
	drm_i915_private_t *dev_priv = (drm_i915_private_t *) dev->dev_private;
	drm_fence_manager_t *fm = &dev->fm;
	drm_fence_driver_t *driver = dev->driver->fence_driver;
	int flush_completed = 0;
	uint32_t flush_flags = 0;
	uint32_t flush_sequence = 0;
	uint32_t i_status;
	uint32_t diff;
	uint32_t sequence;

	if (fm->pending_exe_flush) {
		sequence = READ_BREADCRUMB(dev_priv);
		diff = sequence - fm->last_exe_flush;
		if (diff < driver->wrap_diff && diff != 0) {
			drm_fence_handler(dev, sequence, DRM_FENCE_EXE);
			diff = sequence - fm->exe_flush_sequence;
			if (diff < driver->wrap_diff) {
				fm->pending_exe_flush = 0;
				i915_user_irq_off(dev_priv);
			} else {
			        i915_user_irq_on(dev_priv);
			}
		}
	}
	if (dev_priv->flush_pending) {
		i_status = READ_HWSP(dev_priv, 0);
		if ((i_status & (1 << 12)) !=
		    (dev_priv->saved_flush_status & (1 << 12))) {
			flush_completed = 1;
			flush_flags = dev_priv->flush_flags;
			flush_sequence = dev_priv->flush_sequence;
			dev_priv->flush_pending = 0;
		} else {
		}
	}
	if (flush_completed) {
		drm_fence_handler(dev, flush_sequence, flush_flags);
	}
	if (fm->pending_flush && !dev_priv->flush_pending) {
		dev_priv->flush_sequence = (uint32_t) READ_BREADCRUMB(dev_priv);
		dev_priv->flush_flags = fm->pending_flush;
		dev_priv->saved_flush_status = READ_HWSP(dev_priv, 0);
		DRM_ERROR("Saved flush status is 0x%08x\n",
			  dev_priv->saved_flush_status);
		I915_WRITE(I915REG_INSTPM, (1 << 5) | (1 << 21));
		dev_priv->flush_pending = 1;
		fm->pending_flush = 0;
	}
}

void i915_poke_flush(drm_device_t * dev)
{
	drm_fence_manager_t *fm = &dev->fm;
	unsigned long flags;

	write_lock_irqsave(&fm->lock, flags);
	i915_perform_flush(dev);
	write_unlock_irqrestore(&fm->lock, flags);
}

int i915_fence_emit_sequence(drm_device_t * dev, uint32_t * sequence)
{
	drm_i915_private_t *dev_priv = (drm_i915_private_t *) dev->dev_private;
	i915_emit_irq(dev);
	*sequence = (uint32_t) dev_priv->counter;
	return 0;
}

void i915_fence_handler(drm_device_t * dev)
{
	drm_fence_manager_t *fm = &dev->fm;

	write_lock(&fm->lock);
	i915_perform_flush(dev);
	i915_perform_flush(dev);
	write_unlock(&fm->lock);
}

