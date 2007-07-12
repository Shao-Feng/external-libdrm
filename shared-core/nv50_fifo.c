/*
 * Copyright (C) 2007 Ben Skeggs.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "drmP.h"
#include "drm.h"
#include "nouveau_drv.h"

typedef struct {
	nouveau_gpuobj_ref_t *thingo;
	nouveau_gpuobj_ref_t *dummyctx;
} nv50_fifo_priv;

#define IS_G80 ((dev_priv->chipset & 0xf0) == 0x50)

static void
nv50_fifo_init_thingo(drm_device_t *dev)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;
	nv50_fifo_priv *priv = dev_priv->Engine.fifo.priv;
	nouveau_gpuobj_ref_t *thingo = priv->thingo;
	int i, fi=2;

	DRM_DEBUG("\n");

	INSTANCE_WR(thingo->gpuobj, 0, 0x7e);
	INSTANCE_WR(thingo->gpuobj, 1, 0x7e);
	for (i = 0; i <NV_MAX_FIFO_NUMBER; i++, fi) {
		if (dev_priv->fifos[i]) {
			INSTANCE_WR(thingo->gpuobj, fi, i);
			fi++;
		}
	}

	NV_WRITE(0x32f4, thingo->instance >> 12);
	NV_WRITE(0x32ec, fi);
	NV_WRITE(0x2500, 0x101);
}

static int
nv50_fifo_channel_enable(drm_device_t *dev, int channel)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;
	struct nouveau_fifo *chan = dev_priv->fifos[channel];

	DRM_DEBUG("ch%d\n", channel);

	if (IS_G80) {
		if (!chan->ramin)
			return DRM_ERR(EINVAL);

		NV_WRITE(NV50_PFIFO_CTX_TABLE(channel),
			 (chan->ramin->instance >> 12) |
			 NV50_PFIFO_CTX_TABLE_CHANNEL_ENABLED);
	} else {
		if (!chan->ramfc)
			return DRM_ERR(EINVAL);

		NV_WRITE(NV50_PFIFO_CTX_TABLE(channel),
			 (chan->ramfc->instance >> 8) |
			 NV50_PFIFO_CTX_TABLE_CHANNEL_ENABLED);
	}

	nv50_fifo_init_thingo(dev);
	return 0;
}

static void
nv50_fifo_channel_disable(drm_device_t *dev, int channel, int nt)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;

	DRM_DEBUG("ch%d, nt=%d\n", channel, nt);

	if (IS_G80) {
		NV_WRITE(NV50_PFIFO_CTX_TABLE(channel),
			 NV50_PFIFO_CTX_TABLE_INSTANCE_MASK_G80);
	} else {
		NV_WRITE(NV50_PFIFO_CTX_TABLE(channel),
			 NV50_PFIFO_CTX_TABLE_INSTANCE_MASK_G84);
	}

	if (!nt) nv50_fifo_init_thingo(dev);
}

static void
nv50_fifo_init_reset(drm_device_t *dev)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;
	uint32_t pmc_e;

	DRM_DEBUG("\n");

	pmc_e = NV_READ(NV03_PMC_ENABLE);
	NV_WRITE(NV03_PMC_ENABLE, pmc_e & ~NV_PMC_ENABLE_PFIFO);
	pmc_e = NV_READ(NV03_PMC_ENABLE);
	NV_WRITE(NV03_PMC_ENABLE, pmc_e |  NV_PMC_ENABLE_PFIFO);
}

static void
nv50_fifo_init_context_table(drm_device_t *dev)
{
	int i;

	DRM_DEBUG("\n");

	for (i = 0; i < NV50_PFIFO_CTX_TABLE__SIZE; i++)
		nv50_fifo_channel_disable(dev, i, 1);
	nv50_fifo_init_thingo(dev);
}

static void
nv50_fifo_init_regs__nv(drm_device_t *dev)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;

	DRM_DEBUG("\n");

	NV_WRITE(0x250c, 0x6f3cfc34);
}

static int
nv50_fifo_init_regs(drm_device_t *dev)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;
	nv50_fifo_priv *priv = dev_priv->Engine.fifo.priv;
	int ret;

	DRM_DEBUG("\n");

	if ((ret = nouveau_gpuobj_new_ref(dev, -1, -1, 0, 0x1000,
					  0x1000,
					  NVOBJ_FLAG_ZERO_ALLOC |
					  NVOBJ_FLAG_ZERO_FREE,
					  &priv->dummyctx)))
		return ret;

	NV_WRITE(0x2500, 0);
	NV_WRITE(0x3250, 0);
	NV_WRITE(0x3220, 0);
	NV_WRITE(0x3204, 0);
	NV_WRITE(0x3210, 0);
	NV_WRITE(0x3270, 0);

	if (IS_G80) {
		NV_WRITE(0x2600, (priv->dummyctx->instance>>8) | (1<<31));
		NV_WRITE(0x27fc, (priv->dummyctx->instance>>8) | (1<<31));
	} else {
		NV_WRITE(0x2600, (priv->dummyctx->instance>>12) | (1<<31));
		NV_WRITE(0x27fc, (priv->dummyctx->instance>>12) | (1<<31));
	}

	return 0;
}

int
nv50_fifo_init(drm_device_t *dev)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;
	nv50_fifo_priv *priv;
	int ret;

	DRM_DEBUG("\n");

	priv = drm_calloc(1, sizeof(*priv), DRM_MEM_DRIVER);
	if (!priv)
		return DRM_ERR(ENOMEM);
	dev_priv->Engine.fifo.priv = priv;

	nv50_fifo_init_reset(dev);

	if ((ret = nouveau_gpuobj_new_ref(dev, -1, -1, 0, (128+2)*4, 0x1000,
				   NVOBJ_FLAG_ZERO_ALLOC,
				   &priv->thingo))) {
		DRM_ERROR("error creating thingo: %d\n", ret);
		return ret;
	}
	nv50_fifo_init_context_table(dev);

	nv50_fifo_init_regs__nv(dev);
	if ((ret = nv50_fifo_init_regs(dev)))
		return ret;

	return 0;
}

void
nv50_fifo_takedown(drm_device_t *dev)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;
	nv50_fifo_priv *priv = dev_priv->Engine.fifo.priv;

	DRM_DEBUG("\n");

	if (!priv)
		return;

	nouveau_gpuobj_ref_del(dev, &priv->thingo);
	nouveau_gpuobj_ref_del(dev, &priv->dummyctx);

	dev_priv->Engine.fifo.priv = NULL;
	drm_free(priv, sizeof(*priv), DRM_MEM_DRIVER);
}

int
nv50_fifo_create_context(drm_device_t *dev, int channel)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;
	struct nouveau_fifo *chan = dev_priv->fifos[channel];
	nouveau_gpuobj_t *ramfc = NULL;
	int ret;

	DRM_DEBUG("ch%d\n", channel);

	if (IS_G80) {
		uint32_t ramfc_offset = chan->ramin->gpuobj->im_pramin->start;
		if ((ret = nouveau_gpuobj_new_fake(dev, ramfc_offset, 0x100,
						   NVOBJ_FLAG_ZERO_ALLOC |
						   NVOBJ_FLAG_ZERO_FREE,
						   &ramfc, &chan->ramfc)))
				return ret;
	} else {
		if ((ret = nouveau_gpuobj_new_ref(dev, channel, -1, 0, 0x100,
						  256,
						  NVOBJ_FLAG_ZERO_ALLOC |
						  NVOBJ_FLAG_ZERO_FREE,
						  &chan->ramfc)))
			return ret;
		ramfc = chan->ramfc->gpuobj;
	}

	INSTANCE_WR(ramfc, 0x48/4, chan->pushbuf->instance >> 4);
	INSTANCE_WR(ramfc, 0x80/4, (0xc << 24) | (chan->ramht->instance >> 4));
	INSTANCE_WR(ramfc, 0x3c/4, 0x000f0078); /* fetch? */
	INSTANCE_WR(ramfc, 0x44/4, 0x2101ffff);
	INSTANCE_WR(ramfc, 0x60/4, 0x7fffffff);
	INSTANCE_WR(ramfc, 0x10/4, 0x00000000);
	INSTANCE_WR(ramfc, 0x08/4, 0x00000000);
	INSTANCE_WR(ramfc, 0x40/4, 0x00000000);
	INSTANCE_WR(ramfc, 0x50/4, 0x2039b2e0);
	INSTANCE_WR(ramfc, 0x54/4, 0x000f0000);
	INSTANCE_WR(ramfc, 0x7c/4, 0x30000001);
	INSTANCE_WR(ramfc, 0x78/4, 0x00000000);
	INSTANCE_WR(ramfc, 0x4c/4, 0x00007fff);

	if (!IS_G80) {
		INSTANCE_WR(chan->ramin->gpuobj, 0, channel);
		INSTANCE_WR(chan->ramin->gpuobj, 1, chan->ramfc->instance);

		INSTANCE_WR(ramfc, 0x88/4, 0x3d520); /* some vram addy >> 10 */
		INSTANCE_WR(ramfc, 0x98/4, chan->ramin->instance >> 12);
	}

	if ((ret = nv50_fifo_channel_enable(dev, channel))) {
		DRM_ERROR("error enabling ch%d: %d\n", channel, ret);
		nouveau_gpuobj_ref_del(dev, &chan->ramfc);
		return ret;
	}

	return 0;
}

void
nv50_fifo_destroy_context(drm_device_t *dev, int channel)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;
	struct nouveau_fifo *chan = dev_priv->fifos[channel];

	DRM_DEBUG("ch%d\n", channel);

	nv50_fifo_channel_disable(dev, channel, 0);
	nouveau_gpuobj_ref_del(dev, &chan->ramfc);
}

int
nv50_fifo_load_context(drm_device_t *dev, int channel)
{
	drm_nouveau_private_t *dev_priv = dev->dev_private;
	struct nouveau_fifo *chan = dev_priv->fifos[channel];
	nouveau_gpuobj_t *ramfc = chan->ramfc->gpuobj;

	DRM_DEBUG("ch%d\n", channel);

	/*XXX: incomplete, only touches the regs that NV does */

	NV_WRITE(0x3244, 0);
	NV_WRITE(0x3240, 0);

	NV_WRITE(0x3224, INSTANCE_RD(ramfc, 0x3c/4));
	NV_WRITE(NV04_PFIFO_CACHE1_DMA_INSTANCE, INSTANCE_RD(ramfc, 0x48/4));
	NV_WRITE(0x3234, INSTANCE_RD(ramfc, 0x4c/4));
	NV_WRITE(0x3254, 1);
	NV_WRITE(NV03_PFIFO_RAMHT, INSTANCE_RD(ramfc, 0x80/4));

	if (!IS_G80) {
		NV_WRITE(0x340c, INSTANCE_RD(ramfc, 0x88/4));
		NV_WRITE(0x3410, INSTANCE_RD(ramfc, 0x98/4));
	}

	NV_WRITE(NV03_PFIFO_CACHE1_PUSH1, channel | (1<<16));
	return 0;
}

int
nv50_fifo_save_context(drm_device_t *dev, int channel)
{
	DRM_DEBUG("ch%d\n", channel);
	DRM_ERROR("stub!\n");
	return 0;
}

