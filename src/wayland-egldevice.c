/*
 * Copyright (c) 2014-2019, NVIDIA CORPORATION. All rights reserved.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "wayland-egldevice.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

#include "wayland-eglhandle.h"
#include "wayland-eglutils.h"

WlEglDeviceDpy *wlGetInternalDisplay(WlEglPlatformData *data, EGLDeviceEXT device)
{
  static const EGLint TRACK_REFS_ATTRIBS[] = {
    EGL_TRACK_REFERENCES_KHR,
    EGL_TRUE,
    EGL_NONE
  };

  fprintf(stderr, "Entering wlGetInternalDisplay for device: %p\n", device);

  WlEglDeviceDpy *devDpy = NULL;
  const EGLint *attribs = NULL;
  const char *drmName = NULL, *renderName = NULL;
  struct stat sb, render_sb;

  // First, see if we've already created an EGLDisplay for this device.
  wl_list_for_each(devDpy, &data->deviceDpyList, link) {
    if (devDpy->data == data && devDpy->eglDevice == device) {
      fprintf(stderr, "devDpy: %p\n", devDpy);
      fprintf(stderr, "devDpy->data: %p\n", devDpy->data);
      fprintf(stderr, "devDpy->eglDevice: %p\n", devDpy->eglDevice);
      return devDpy;
    }
  }

  // We didn't find a matching display, so create one.
  if (data->supportsDisplayReference) {
    // Always use EGL_KHR_display_reference if the driver supports it.
    // We'll do our own refcounting so that we can work without it, but
    // setting EGL_TRACK_REFERENCES_KHR means that it's less likely that
    // something else might grab the same EGLDevice-based display and
    // call eglTerminate on it.
    fprintf(stderr, "Creating a new EGLDisplay, using EGL_KHR_display_reference.\n");
    attribs = TRACK_REFS_ATTRIBS;
  } else {
    fprintf(stderr, "Creating a new EGLDisplay without display reference support.\n");
  }

  devDpy = calloc(1, sizeof(WlEglDeviceDpy));
  if (devDpy == NULL) {
    fprintf(stderr, "Failed to allocate memory for WlEglDeviceDpy.\n");
    return NULL;
  }

  devDpy->eglDevice = device;
  devDpy->data = data;
  devDpy->eglDisplay = data->egl.getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, device, attribs);
  if (devDpy->eglDisplay == EGL_NO_DISPLAY) {
    fprintf(stderr, "Failed to create EGLDisplay for device: %p\n", device);
    goto fail;
  }

  /* Get the device in use,
     * calling eglQueryDeviceStringEXT(EGL_DRM_RENDER_NODE_FILE_EXT) to get drm fd.
     * We will be getting the dev_t for the render node and the normal node, since
     * we don't know for sure which one the compositor will happen to use.
     */
  drmName = data->egl.queryDeviceString(devDpy->eglDevice, EGL_DRM_DEVICE_FILE_EXT);
  if (!drmName) {
    fprintf(stderr, "Failed to query DRM device file name for device: %p\n", device);
    goto fail;
  }
  fprintf(stderr, "DRM device file: %s\n", drmName);

  // Use stat to get the dev_t for this device
  if (stat(drmName, &sb) != 0) {
    fprintf(stderr, "stat failed for DRM device file: %s\n", drmName);
    goto fail;
  }

  renderName = data->egl.queryDeviceString(devDpy->eglDevice, EGL_DRM_RENDER_NODE_FILE_EXT);
  if (!renderName) {
    fprintf(stderr, "Failed to query render node file name for device: %p\n", device);
    goto fail;
  }
  fprintf(stderr, "Render node file: %s\n", renderName);

  if (stat(renderName, &render_sb) != 0) {
    fprintf(stderr, "stat failed for render node file: %s\n", renderName);
    goto fail;
  }

  devDpy->dev = sb.st_rdev;
  devDpy->renderNode = render_sb.st_rdev;

  wl_list_insert(&data->deviceDpyList, &devDpy->link);
  fprintf(stderr, "Inserted new WlEglDeviceDpy into deviceDpyList for device: %p\n", device);

  return devDpy;

  fail:
  free(devDpy);
  return NULL;
}

static void wlFreeInternalDisplay(WlEglDeviceDpy *devDpy)
{
  fprintf(stderr, "Freeing internal display: %p\n", devDpy);

  if (devDpy->initCount > 0) {
    fprintf(stderr, "Terminating EGL display for device: %p\n", devDpy->eglDevice);
    devDpy->data->egl.terminate(devDpy->eglDisplay);
  }

  wl_list_remove(&devDpy->link);
  fprintf(stderr, "Removed internal display from list: %p\n", devDpy);

  free(devDpy);
}

void wlFreeAllInternalDisplays(WlEglPlatformData *data)
{
  WlEglDeviceDpy *devDpy, *devNext;
  wl_list_for_each_safe(devDpy, devNext, &data->deviceDpyList, link) {
    assert(devDpy->data == data);
    fprintf(stderr, "Freeing internal display: %p\n", devDpy);
    wlFreeInternalDisplay(devDpy);
  }
}

EGLBoolean wlInternalInitialize(WlEglDeviceDpy *devDpy)
{
  if (devDpy->initCount == 0) {
    const char *exts;

    // Log the beginning of the initialization
    fprintf(stderr, "Initializing EGL display for device: %p\n", devDpy);

    // Attempt to initialize the EGL display
    if (!devDpy->data->egl.initialize(devDpy->eglDisplay, &devDpy->major, &devDpy->minor)) {
      fprintf(stderr, "Failed to initialize EGL display: %p\n", devDpy->eglDisplay);
      return EGL_FALSE;
    }

    // Log the EGL version after initialization
    fprintf(stderr, "Initialized EGL display %p with version %d.%d\n",
            devDpy->eglDisplay, devDpy->major, devDpy->minor);

    // Query and log the EGL extensions
    exts = devDpy->data->egl.queryString(devDpy->eglDisplay, EGL_EXTENSIONS);
    if (exts) {
      fprintf(stderr, "EGL Extensions: %s\n", exts);
    } else {
      fprintf(stderr, "Failed to query EGL extensions.\n");
    }

#define CACHE_EXT(_PREFIX_, _NAME_)                                      \
        devDpy->exts._NAME_ =                                           \
            !!wlEglFindExtension("EGL_" #_PREFIX_ "_" #_NAME_, exts)

    // Log caching of each extension
    CACHE_EXT(KHR, stream);
    fprintf(stderr, "Cached KHR_stream: %d\n", devDpy->exts.stream);

    CACHE_EXT(NV, stream_attrib);
    fprintf(stderr, "Cached NV_stream_attrib: %d\n", devDpy->exts.stream_attrib);

    CACHE_EXT(KHR, stream_cross_process_fd);
    fprintf(stderr, "Cached KHR_stream_cross_process_fd: %d\n", devDpy->exts.stream_cross_process_fd);

    CACHE_EXT(NV, stream_remote);
    fprintf(stderr, "Cached NV_stream_remote: %d\n", devDpy->exts.stream_remote);

    CACHE_EXT(KHR, stream_producer_eglsurface);
    fprintf(stderr, "Cached KHR_stream_producer_eglsurface: %d\n", devDpy->exts.stream_producer_eglsurface);

    CACHE_EXT(NV, stream_fifo_synchronous);
    fprintf(stderr, "Cached NV_stream_fifo_synchronous: %d\n", devDpy->exts.stream_fifo_synchronous);

    CACHE_EXT(NV, stream_sync);
    fprintf(stderr, "Cached NV_stream_sync: %d\n", devDpy->exts.stream_sync);

    CACHE_EXT(NV, stream_flush);
    fprintf(stderr, "Cached NV_stream_flush: %d\n", devDpy->exts.stream_flush);

    CACHE_EXT(NV, stream_consumer_eglimage);
    fprintf(stderr, "Cached NV_stream_consumer_eglimage: %d\n", devDpy->exts.stream_consumer_eglimage);

    CACHE_EXT(MESA, image_dma_buf_export);
    fprintf(stderr, "Cached MESA_image_dma_buf_export: %d\n", devDpy->exts.image_dma_buf_export);

#undef CACHE_EXT
  }

  devDpy->initCount++;
  fprintf(stderr, "EGL display initialized count incremented: %d\n", devDpy->initCount);
  return EGL_TRUE;
}

EGLBoolean wlInternalTerminate(WlEglDeviceDpy *devDpy)
{
  if (devDpy->initCount > 0) {
    fprintf(stderr, "Terminating EGL display for device: %p, current initCount: %d\n", devDpy, devDpy->initCount);

    if (devDpy->initCount == 1) {
      fprintf(stderr, "Final termination of EGL display: %p\n", devDpy->eglDisplay);
      if (!devDpy->data->egl.terminate(devDpy->eglDisplay)) {
        fprintf(stderr, "Failed to terminate EGL display: %p\n", devDpy->eglDisplay);
        return EGL_FALSE;
      }
      fprintf(stderr, "Successfully terminated EGL display: %p\n", devDpy->eglDisplay);
    }

    devDpy->initCount--;
    fprintf(stderr, "Decrementing initCount: %d\n", devDpy->initCount);
  } else {
    fprintf(stderr, "No active initializations to terminate for device: %p\n", devDpy);
  }

  return EGL_TRUE;
}