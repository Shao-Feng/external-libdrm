/* mach64_drv.c -- mach64 (Rage Pro) driver -*- linux-c -*-
 * Created: Fri Nov 24 18:34:32 2000 by gareth@valinux.com
 *
 * Copyright 2000 Gareth Hughes
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * GARETH HUGHES BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Gareth Hughes <gareth@valinux.com>
 *    Leif Delgass <ldelgass@retinalburn.net>
 */

#include <linux/config.h>
#include "drmP.h"
#include "drm.h"
#include "mach64_drm.h"
#include "mach64_drv.h"

#include "drm_pciids.h"

static int postinit(struct drm_device *dev, unsigned long flags)
{
	DRM_INFO("Initialized %s %d.%d.%d %s on minor %d: %s\n",
		 DRIVER_NAME,
		 DRIVER_MAJOR,
		 DRIVER_MINOR,
		 DRIVER_PATCHLEVEL,
		 DRIVER_DATE, dev->minor, pci_pretty_name(dev->pdev)
	    );
	return 0;
}

static int version(drm_version_t * version)
{
	int len;

	version->version_major = DRIVER_MAJOR;
	version->version_minor = DRIVER_MINOR;
	version->version_patchlevel = DRIVER_PATCHLEVEL;
	DRM_COPY(version->name, DRIVER_NAME);
	DRM_COPY(version->date, DRIVER_DATE);
	DRM_COPY(version->desc, DRIVER_DESC);
	return 0;
}

static struct pci_device_id pciidlist[] = {
	mach64_PCI_IDS
};

/* Interface history:
 *
 * 1.0 - Initial mach64 DRM
 *
 */
static drm_ioctl_desc_t ioctls[] = {
	[DRM_IOCTL_NR(DRM_MACH64_INIT)] = {mach64_dma_init, 1, 1},
	[DRM_IOCTL_NR(DRM_MACH64_CLEAR)] = {mach64_dma_clear, 1, 0},
	[DRM_IOCTL_NR(DRM_MACH64_SWAP)] = {mach64_dma_swap, 1, 0},
	[DRM_IOCTL_NR(DRM_MACH64_IDLE)] = {mach64_dma_idle, 1, 0},
	[DRM_IOCTL_NR(DRM_MACH64_RESET)] = {mach64_engine_reset, 1, 0},
	[DRM_IOCTL_NR(DRM_MACH64_VERTEX)] = {mach64_dma_vertex, 1, 0},
	[DRM_IOCTL_NR(DRM_MACH64_BLIT)] = {mach64_dma_blit, 1, 0},
	[DRM_IOCTL_NR(DRM_MACH64_FLUSH)] = {mach64_dma_flush, 1, 0},
	[DRM_IOCTL_NR(DRM_MACH64_GETPARAM)] = {mach64_get_param, 1, 0},
};

static struct drm_driver driver = {
	.driver_features =
	    DRIVER_USE_AGP | DRIVER_USE_MTRR | DRIVER_PCI_DMA | DRIVER_HAVE_DMA
	    | DRIVER_HAVE_IRQ | DRIVER_IRQ_SHARED | DRIVER_IRQ_VBL,
	.pretakedown = mach64_driver_pretakedown,
	.vblank_wait = mach64_driver_vblank_wait,
	.irq_preinstall = mach64_driver_irq_preinstall,
	.irq_postinstall = mach64_driver_irq_postinstall,
	.irq_uninstall = mach64_driver_irq_uninstall,
	.irq_handler = mach64_driver_irq_handler,
	.reclaim_buffers = drm_core_reclaim_buffers,
	.get_map_ofs = drm_core_get_map_ofs,
	.get_reg_ofs = drm_core_get_reg_ofs,
	.postinit = postinit,
	.version = version,
	.ioctls = ioctls,
	.num_ioctls = DRM_ARRAY_SIZE(ioctls),
	.dma_ioctl = mach64_dma_buffers,
	.fops = {
		 .owner = THIS_MODULE,
		 .open = drm_open,
		 .release = drm_release,
		 .ioctl = drm_ioctl,
		 .mmap = drm_mmap,
		 .fasync = drm_fasync,
		 },
};

static int probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	return drm_probe(pdev, ent, &driver);
}

static struct pci_driver pci_driver = {
	.name = DRIVER_NAME,
	.id_table = pciidlist,
	.probe = probe,
	.remove = __devexit_p(drm_cleanup_pci),
};

static int __init mach64_init(void)
{
	return drm_init(&pci_driver, pciidlist, &driver);
}

static void __exit mach64_exit(void)
{
	drm_exit(&pci_driver);
}

module_init(mach64_init);
module_exit(mach64_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL and additional rights");
