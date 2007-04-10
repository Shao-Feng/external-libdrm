/*
 * \file xf86drmMode.h
 * Header for DRM modesetting interface.
 *
 * \author Jakob Bornecrantz <wallbraker@gmail.com>
 *
 * \par Acknowledgements:
 * Feb 2007, Dave Airlie <airlied@linux.ie>
 */

/*
 * Copyright (c) <year> <copyright holders>
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <drm.h>
#include "xf86mm.h"

/*
 * This is the interface for modesetting for drm.
 *
 * In order to use this interface you must include either <stdint.h> or another
 * header defining uint32_t, int32_t and uint16_t.
 *
 * It aims to provide a randr compatible interface for modesettings in the
 * kernel, the interface is also ment to be used by libraries like EGL.
 *
 * More information can be found in randrproto.txt which can be found here:
 * http://gitweb.freedesktop.org/?p=xorg/proto/randrproto.git
 *
 * All framebuffer, crtc and output ids start at 1 while 0 is either an invalid
 * parameter or used to indicate that the command should disconnect from the
 * currently bound target, as with drmModeMapOutput.
 *
 * Currently only one framebuffer exist and has a id of 1, which is also the
 * default framebuffer and should allways be avaible to the client, unless
 * it is locked/used or any other limiting state is applied on it.
 *
 */

typedef struct _drmModeGammaTriple {
	uint16_t r, g, b;
} drmModeGammaTriple, *drmModeGammaTriplePtr;

typedef struct _drmModeRes {

	uint32_t frameBufferId;

	int count_crtcs;
	uint32_t *crtcs;

	int count_outputs;
	uint32_t *outputs;

	int count_modes;
	struct drm_mode_modeinfo *modes;

} drmModeRes, *drmModeResPtr;

typedef struct _drmModeFrameBuffer {

        uint32_t width;
        uint32_t height;
        uint32_t pitch;
        uint8_t bpp;

} drmModeFrameBuffer, *drmModeFrameBufferPtr;

typedef struct _drmModeCrtc {

	unsigned int bufferId; /**< Buffer currently connected to */

	uint32_t x, y; /**< Position on the frameuffer */
	uint32_t width, height;
	uint32_t mode; /**< Current mode used */

	int count_outputs;
	uint32_t outputs; /**< Outputs that are connected */

	int count_possibles;
	uint32_t possibles; /**< Outputs that can be connected */

	int gamma_size; /**< Number of gamma stops */

} drmModeCrtc, *drmModeCrtcPtr;

typedef enum {
	DRM_MODE_CONNECTED         = 1,
	DRM_MODE_DISCONNECTED      = 2,
	DRM_MODE_UNKNOWNCONNECTION = 3
} drmModeConnection;

typedef enum {
	DRM_MODE_SUBPIXEL_UNKNOWN        = 1,
	DRM_MODE_SUBPIXEL_HORIZONTAL_RGB = 2,
	DRM_MODE_SUBPIXEL_HORIZONTAL_BGR = 3,
	DRM_MODE_SUBPIXEL_VERTICAL_RGB   = 4,
	DRM_MODE_SUBPIXEL_VERTICAL_BGR   = 5,
	DRM_MODE_SUBPIXEL_NONE           = 6
} drmModeSubPixel;

typedef struct _drmModeOutput {

	unsigned int crtc; /**< Crtc currently connected to */

	drmModeConnection connection;
	uint32_t mmWidth, mmHeight; /**< HxW in millimeters */
	drmModeSubPixel subpixel;

	int count_crtcs;
	uint32_t crtcs; /**< Possible crtc to connect to */

	int count_clones;
	uint32_t clones; /**< Mask of clones */

	int count_modes;
	uint32_t *modes; /**< List of modes ids */

} drmModeOutput, *drmModeOutputPtr;

/*
 * RRSetScreenConfig o
 * RRGetScreenInfo o
 *
 * RRGetScreenSizeRange - see frameBuffer info
 * RRSetScreenSize
 * RRGetScreenResources
 *
 * RRGetOutputInfo
 *
 * RRListOutputProperties *
 * RRQueryOutputProperty *
 * RRConfigureOutputProperty *
 * RRChangeOutputProperty *
 * RRDeleteOutputProperty *
 * RRGetOutputProperty *
 *
 * RRCreateMode
 * RRDestroyMode
 * RRAddOutputMode
 * RRDeleteOutputMode
 *
 * RRGetCrtcInfo
 * RRSetCrtcConfig
 *
 * RRGetCrtcGammaSize - see crtc info
 * RRGetCrtcGamma
 * RRSetCrtcGamma
 *
 * drmModeGetResources
 * drmModeForceProbe
 *
 * drmModeGetFrameBufferInfo
 * drmModeSetFrameBufferSize
 *
 * drmModeGetCrtcInfo
 * drmModeSetCrtcConfig
 * drmModeGetCrtcGamma
 * drmModeSetCrtcGamma
 *
 * drmModeGetOutputInfo
 *
 * drmModeAddMode
 * drmModeDestroyMode
 * drmModeAddOutputMode
 * drmModeDeleteOutputMode
 */

extern void drmModeFreeModeInfo( struct drm_mode_modeinfo *ptr );
extern void drmModeFreeResources( drmModeResPtr ptr );
extern void drmModeFreeFrameBuffer( drmModeFrameBufferPtr ptr );
extern void drmModeFreeCrtc( drmModeCrtcPtr ptr );
extern void drmModeFreeOutput( drmModeOutputPtr ptr );

/**
 * Retrives all of the resources associated with a card.
 */
extern drmModeResPtr drmModeGetResources(int fd);

/**
 * Forces a probe of the give output outputId, on 0 all will be probed.
 */
extern int drmModeForceProbe(int fd, uint32_t outputId);


/*
 * FrameBuffer manipulation.
 */

/**
 * Retrive information about framebuffer bufferId
 */
extern drmModeFrameBufferPtr drmModeGetFramebuffer(int fd,
		uint32_t bufferId);

/**
 * Creates a new framebuffer with an buffer object as its scanout buffer.
 */
extern uint32_t drmModeNewFrameBuffer(int fd,
		uint32_t width, uint32_t height,
		uint8_t bbp, uint32_t pitch, drmBO *bo);

/**
 * Destroies the given framebuffer.
 */
extern int drmModeDesFrameBuffer(int fd, uint32_t bufferId);

/**
 * Changes the scanout buffer to the given buffer object.
 */
extern int drmModeFlipFrameBuffer(int fd, uint32_t bufferId, drmBO *bo);

/*
 * Crtc function.
 */

/**
 * Retrive information about the ctrt crtcId
 */
extern drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtcId);

/**
 * Set the mode on a crtc crtcId with the given mode modeId.
 */
extern int drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId,
		uint32_t x, uint32_t y, uint32_t modeId,
		uint32_t *outputs, int count);

/**
 * Gets the gamma from a crtc
 */
extern drmModeGammaTriplePtr drmModeGetCrtcGamma(int fd, uint32_t crtcId,
		int *count);

/**
 * Sets the gamma on a crtc
 */
extern int drmModeSetCrtcGamma(int fd, uint32_t crtcId,
		drmModeGammaTriplePtr ptr, int count);



/*
 * Output manipulation
 */

/**
 * Retrive information about the output outputId.
 */
extern drmModeOutputPtr drmModeGetOutput(int fd,
		uint32_t outputId);

/**
 * Creates a new mode from the given mode info.
 * Name must be unique.
 */
extern uint32_t drmModeNewMode(int fd, struct drm_mode_modeinfo *modeInfo);

/**
 * Destroys a mode created with CreateMode, must be unused.
 */
extern int drmModeDesMode(int fd, uint32_t modeId);

/**
 * Adds the given mode to an output.
 */
extern int drmModeAddMode(int fd, uint32_t outputId, uint32_t modeId);

/**
 * Deletes a mode Added with AddOutputMode from the output,
 * must be unused, by the given mode.
 */
extern int drmModeDelMode(int fd, uint32_t outputId, uint32_t modeId);

