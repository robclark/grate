/*
 * Copyright (c) 2012, 2013 Erik Faye-Lund
 * Copyright (c) 2013 Thierry Reding
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "host1x-private.h"

#include "nvhost-gr2d.h"
#include "nvhost-gr3d.h"
#include "nvhost.h"

static int nvhost_bo_mmap(struct host1x_bo *bo)
{
	struct nvhost_bo *nbo = to_nvhost_bo(bo);
	int err;

	err = nvmap_handle_mmap(nbo->nvmap, nbo->handle);
	if (err < 0)
		return err;

	bo->ptr = nbo->handle->ptr;

	return 0;
}

static int nvhost_bo_invalidate(struct host1x_bo *bo, loff_t offset,
				size_t length)
{
	struct nvhost_bo *nbo = to_nvhost_bo(bo);

	return nvmap_handle_invalidate(nbo->nvmap, nbo->handle, offset,
				       length);
}

static int nvhost_bo_flush(struct host1x_bo *bo, loff_t offset, size_t length)
{
	struct nvhost_bo *nbo = to_nvhost_bo(bo);

	return nvmap_handle_writeback_invalidate(nbo->nvmap, nbo->handle,
						 offset, length);
}

static void nvhost_bo_free(struct host1x_bo *bo)
{
	struct nvhost_bo *nbo = to_nvhost_bo(bo);

	nvmap_handle_free(nbo->nvmap, nbo->handle);

	free(nbo);
}

static struct host1x_bo *nvhost_bo_create(struct host1x *host1x, size_t size,
					  unsigned long flags)
{
	struct nvhost *nvhost = to_nvhost(host1x);
	unsigned long heap_mask, align;
	struct nvhost_bo *bo;
	int err;

	bo = calloc(1, sizeof(*bo));
	if (!bo)
		return NULL;

	bo->nvmap = nvhost->nvmap;

	bo->handle = nvmap_handle_create(nvhost->nvmap, size);
	if (!bo->handle) {
		free(bo);
		return NULL;
	}

	switch (flags) {
	case 1: /* framebuffer */
		heap_mask = 1 << 0;
		flags = 1 << 0;
		align = 0x100;
		break;

	case 2: /* command buffer */
		heap_mask = 1 << 0;
		flags = 0x0a000001;
		align = 0x20;
		break;

	case 3: /* scratch */
		heap_mask = 1 << 30;
		flags = 1 << 0;
		align = 0x20;
		break;

	case 4: /* attributes */
		heap_mask = 1 << 30;
		flags = 0x3d000001;
		align = 0x4;
		break;

	default:
		heap_mask = 1 << 30;
		flags = 0x3d000001;
		align = 0x4;
		break;
	}

	/* XXX what to use for flags and heap_mask? */
	err = nvmap_handle_alloc(nvhost->nvmap, bo->handle, heap_mask, flags,
				 align);
	if (err < 0) {
		nvmap_handle_free(nvhost->nvmap, bo->handle);
		free(bo);
		return NULL;
	}

	bo->base.handle = bo->handle->id;
	bo->base.size = bo->handle->size;

	bo->base.mmap = nvhost_bo_mmap;
	bo->base.invalidate = nvhost_bo_invalidate;
	bo->base.flush = nvhost_bo_flush;
	bo->base.free = nvhost_bo_free;

	return &bo->base;
}

static void host1x_nvhost_close(struct host1x *host1x)
{
	struct nvhost *nvhost = to_nvhost(host1x);

	nvhost_gr3d_close(nvhost->gr3d);
	nvhost_gr2d_close(nvhost->gr2d);
	nvhost_ctrl_close(nvhost->ctrl);
	nvmap_close(nvhost->nvmap);
}

struct host1x *host1x_nvhost_open(void)
{
	struct nvhost *nvhost;

	nvhost = calloc(1, sizeof(*nvhost));
	if (!nvhost)
		return NULL;

	nvhost->nvmap = nvmap_open();
	if (!nvhost->nvmap)
		return NULL;

	nvhost->ctrl = nvhost_ctrl_open();
	if (!nvhost->ctrl)
		return NULL;

	nvhost->base.bo_create = nvhost_bo_create;
	nvhost->base.close = host1x_nvhost_close;

	nvhost->gr2d = nvhost_gr2d_open(nvhost);
	if (!nvhost->gr2d)
		return NULL;

	nvhost->gr3d = nvhost_gr3d_open(nvhost);
	if (!nvhost->gr3d)
		return NULL;

	nvhost->base.gr2d = &nvhost->gr2d->base;
	nvhost->base.gr3d = &nvhost->gr3d->base;

	return &nvhost->base;
}
