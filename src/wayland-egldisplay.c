/*
 * Copyright (c) 2014-2022, NVIDIA CORPORATION. All rights reserved.
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

#include "wayland-egldisplay.h"
#include "wayland-eglstream-client-protocol.h"
#include "wayland-eglstream-controller-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wayland-eglstream-server.h"
#include "wayland-thread.h"
#include "wayland-eglsurface-internal.h"
#include "wayland-eglhandle.h"
#include "wayland-eglutils.h"
#include "wayland-drm-client-protocol.h"
#include "wayland-drm.h"
#include "presentation-time-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <dlfcn.h>
#include <stdio.h>

typedef struct WlServerProtocolsRec {
    EGLBoolean hasEglStream;
    EGLBoolean hasDmaBuf;
    struct zwp_linux_dmabuf_v1 *wlDmaBuf;
    dev_t devId;

    struct wl_drm *wlDrm;
    char *drm_name;
} WlServerProtocols;

/* TODO: Make global display lists hang off platform data */
static struct wl_list wlEglDisplayList = WL_LIST_INITIALIZER(&wlEglDisplayList);

static bool getDeviceFromDevIdInitialised = false;
static int (*getDeviceFromDevId)(dev_t dev_id, uint32_t flags, drmDevice **device) = NULL;

EGLBoolean wlEglIsWaylandDisplay(void *nativeDpy)
{
    if (!wlEglMemoryIsReadable(nativeDpy, sizeof (void *))) {
        return EGL_FALSE;
    }

    return wlEglCheckInterfaceType(nativeDpy, "wl_display");
}

EGLBoolean wlEglIsValidNativeDisplayExport(void *data, void *nativeDpy)
{
    char       *val      = getenv("EGL_PLATFORM");
    (void)data;

    if (val && !strcasecmp(val, "wayland")) {
        return EGL_TRUE;
    }

    return wlEglIsWaylandDisplay(nativeDpy);
}

#include <stdio.h>
#include <EGL/egl.h>
#include "wayland-egldisplay.h" // Assuming the required headers are included here

EGLBoolean wlEglBindDisplaysHook(void *data, EGLDisplay dpy, void *nativeDpy)
{
    /* Retrieve extension string and device name before taking external API lock */
    fprintf(stderr, "Entering wlEglBindDisplaysHook: data=%p, dpy=%p, nativeDpy=%p\n", data, dpy, nativeDpy);

    const char *exts = NULL;
    const char *dev_name = NULL;
    EGLBoolean res = EGL_FALSE;

    // Validate inputs
    if (!data || !dpy || !nativeDpy) {
        fprintf(stderr, "Invalid arguments passed to wlEglBindDisplaysHook: data=%p, dpy=%p, nativeDpy=%p\n", data, dpy, nativeDpy);
        return EGL_FALSE;
    }

    // Retrieve EGL extensions
    exts = ((WlEglPlatformData *)data)->egl.queryString(dpy, EGL_EXTENSIONS);
    if (!exts) {
        fprintf(stderr, "EGL queryString failed: extensions string is NULL\n");
        return EGL_FALSE;
    }
    fprintf(stderr, "EGL extensions retrieved: %s\n", exts);

    // Retrieve DRM device name
    dev_name = wl_drm_get_dev_name(data, dpy);
    if (!dev_name) {
        fprintf(stderr, "Failed to get DRM device name from wl_drm\n");
        return EGL_FALSE;
    }
    fprintf(stderr, "DRM device name: %s\n", dev_name);

    // Lock API and attempt binding
    wlExternalApiLock();
    res = wl_eglstream_display_bind((WlEglPlatformData *)data,
                                    (struct wl_display *)nativeDpy,
                                    dpy, exts, dev_name);
    if (!res) {
        fprintf(stderr, "Failed to bind EGL display to Wayland display\n");
    } else {
        fprintf(stderr, "Successfully bound EGL display to Wayland display\n");
    }
    wlExternalApiUnlock();

    fprintf(stderr, "Exiting wlEglBindDisplaysHook: result=%d\n", res);
    return res;
}

EGLBoolean wlEglUnbindDisplaysHook(EGLDisplay dpy, void *nativeDpy)
{
    fprintf(stderr, "Entering wlEglUnbindDisplaysHook: dpy=%p, nativeDpy=%p\n", dpy, nativeDpy);

    struct wl_eglstream_display *wlStreamDpy;
    EGLBoolean res = EGL_FALSE;

    // Validate inputs
    if (!dpy || !nativeDpy) {
        fprintf(stderr, "Invalid arguments passed to wlEglUnbindDisplaysHook: dpy=%p, nativeDpy=%p\n", dpy, nativeDpy);
        return EGL_FALSE;
    }

    // Lock API and attempt unbinding
    wlExternalApiLock();
    wlStreamDpy = wl_eglstream_display_get(dpy);
    if (!wlStreamDpy) {
        fprintf(stderr, "No EGLStream display found for EGLDisplay: %p\n", dpy);
    } else if (wlStreamDpy->wlDisplay != (struct wl_display *)nativeDpy) {
        fprintf(stderr, "Mismatch between Wayland displays during unbind\n");
    } else {
        fprintf(stderr, "Unbinding EGLStream display: %p\n", wlStreamDpy);
        wl_eglstream_display_unbind(wlStreamDpy);
        res = EGL_TRUE;
    }
    wlExternalApiUnlock();

    fprintf(stderr, "Exiting wlEglUnbindDisplaysHook: result=%d\n", res);
    return res;
}


static void
wlEglDestroyFormatSet(WlEglDmaBufFormatSet *set)
{
    for (unsigned int i = 0; i < set->numFormats; i++) {
        free(set->dmaBufFormats[i].modifiers);
    }

    free(set->dmaBufFormats);
}

static void
wlEglFeedbackResetTranches(WlEglDmaBufFeedback *feedback)
{
    if (feedback->numTranches == 0)
        return;

    wlEglDestroyFormatSet(&feedback->tmpTranche.formatSet);
    for (int i = 0; i < feedback->numTranches; i++) {
        wlEglDestroyFormatSet(&feedback->tranches[i].formatSet);
    }
    free(feedback->tranches);
    feedback->tranches = NULL;
    feedback->numTranches = 0;
}

#if defined(NV_SUNOS)
// Solaris uses the obsolete 'caddr_t' type unless _X_OPEN_SOURCE is set.
// Unfortunately, setting that flag breaks a lot of other code so instead
// just cast the pointer to the appropriate type.
//
// caddr_t is defined in sys/types.h to be char*.
typedef caddr_t pointer_t;
#else
typedef void *pointer_t;
#endif

void
wlEglDestroyFeedback(WlEglDmaBufFeedback *feedback)
{
    wlEglFeedbackResetTranches(feedback);
    munmap((pointer_t)feedback->formatTable.entry,
           sizeof(feedback->formatTable.entry[0]) * feedback->formatTable.len);

    if (feedback->wlDmaBufFeedback) {
        zwp_linux_dmabuf_feedback_v1_destroy(feedback->wlDmaBufFeedback);
    }
}

static void
wlEglDmaBufFormatAddModifier(WlEglDmaBufFormat *format, const uint64_t modifier)
{
    uint64_t *newModifiers;
    uint32_t m;

    for (m = 0; m < format->numModifiers; m++) {
        if (format->modifiers[m] == modifier) {
            return;
        }
    }

    newModifiers = realloc(format->modifiers,
                           sizeof(format->modifiers[0]) *
                           (format->numModifiers + 1));

    if (!newModifiers) {
        return;
    }

    newModifiers[format->numModifiers] = modifier;

    format->modifiers = newModifiers;
    format->numModifiers++;
}

static void
wlEglFormatSetAdd(WlEglDmaBufFormatSet *set, uint32_t format, const uint64_t modifier)
{
    uint32_t f;
    WlEglDmaBufFormat *newFormats;

    for (f = 0; f < set->numFormats; f++) {
        if (set->dmaBufFormats[f].format == format) {
            wlEglDmaBufFormatAddModifier(&set->dmaBufFormats[f], modifier);
            return;
        }
    }

    newFormats = realloc(set->dmaBufFormats,
            sizeof(set->dmaBufFormats[0]) * (set->numFormats + 1));

    if (!newFormats) {
        return;
    }

    newFormats[set->numFormats].format = format;
    newFormats[set->numFormats].numModifiers = 0;
    newFormats[set->numFormats].modifiers = NULL;
    wlEglDmaBufFormatAddModifier(&newFormats[set->numFormats], modifier);

    set->dmaBufFormats = newFormats;
    set->numFormats++;
}

static void
dmabuf_handle_format(void *data,
                     struct zwp_linux_dmabuf_v1 *dmabuf,
                     uint32_t format)
{
    (void)data;
    (void)dmabuf;
    (void)format;
    /* Only use formats that include an associated modifier */
}

static void
dmabuf_handle_modifier(void *data,
                       struct zwp_linux_dmabuf_v1 *dmabuf,
                       uint32_t format,
                       uint32_t mod_hi,
                       uint32_t mod_lo)
{
    WlEglDisplay *display = data;
    const uint64_t modifier = ((uint64_t)mod_hi << 32ULL) | (uint64_t)mod_lo;

    (void)dmabuf;

    wlEglFormatSetAdd(&display->formatSet, format, modifier);
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_handle_format,
    .modifier = dmabuf_handle_modifier,
};

/*
 * We need to check if the compositor is resending all of the tranche
 * information. Each tranche event will call this method to see
 * if the existing format info should be cleared before refilling.
 */
static void
dmabuf_feedback_check_reset_tranches(WlEglDmaBufFeedback *feedback)
{
    if (!feedback->feedbackDone)
        return;

    feedback->feedbackDone = false;
    wlEglFeedbackResetTranches(feedback);
}

static void
dmabuf_feedback_main_device(void *data,
                            struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                            struct wl_array *dev)
{
    WlEglDmaBufFeedback *feedback = data;
    dev_t devid;
    (void) dmabuf_feedback;

    dmabuf_feedback_check_reset_tranches(feedback);

    assert(dev->size == sizeof(dev_t));
    memcpy(&devid, dev->data, sizeof(dev_t));

    feedback->mainDev = devid;
}

static void
dmabuf_feedback_tranche_target_device(void *data,
                                      struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                      struct wl_array *dev)
{
    WlEglDmaBufFeedback *feedback = data;
    (void) dmabuf_feedback;

    dmabuf_feedback_check_reset_tranches(feedback);

    assert(dev->size == sizeof(dev_t));
    memcpy(&feedback->tmpTranche.drmDev, dev->data, sizeof(dev_t));
}

static void
dmabuf_feedback_tranche_flags(void *data,
                              struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                              uint32_t flags)
{
    WlEglDmaBufFeedback *feedback = data;
    (void) dmabuf_feedback;

    dmabuf_feedback_check_reset_tranches(feedback);

    if (flags & ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT)
        feedback->tmpTranche.supportsScanout = true;
}

static void
dmabuf_feedback_tranche_formats(void *data,
                                struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                struct wl_array *indices)
{
    WlEglDmaBufFeedback *feedback = data;
    WlEglDmaBufTranche *tmp = &feedback->tmpTranche;
    uint16_t *index;
    WlEglDmaBufFormatTableEntry *entry;
    (void) dmabuf_feedback;

    dmabuf_feedback_check_reset_tranches(feedback);

    wl_array_for_each(index, indices) {
        if (*index >= feedback->formatTable.len) {
            /*
             * Index given to us by the compositor is too large to fit in the format table.
             * This is a compositor bug, just skip it.
             */
            continue;
        }

        /* Look up this format/mod in the format table */
        entry = &feedback->formatTable.entry[*index];

        /* Add it to the in-progress */
        wlEglFormatSetAdd(&tmp->formatSet, entry->format, entry->modifier);
    }
}

static void
dmabuf_feedback_tranche_done(void *data,
                             struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
    WlEglDmaBufFeedback *feedback = data;
    (void) dmabuf_feedback;

    /*
     * No need to call dmabuf_feedback_check_reset_tranches, the other events should have been
     * triggered first
     */

    feedback->numTranches++;
    feedback->tranches = realloc(feedback->tranches,
            sizeof(WlEglDmaBufTranche) * feedback->numTranches);
    assert(feedback->tranches);

    /* copy the temporary tranche into the official array */
    memcpy(&feedback->tranches[feedback->numTranches - 1],
           &feedback->tmpTranche, sizeof(WlEglDmaBufTranche));

    /* reset the tranche */
    memset(&feedback->tmpTranche, 0, sizeof(WlEglDmaBufTranche));
}

static void
dmabuf_feedback_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
    WlEglDmaBufFeedback *feedback = data;
    (void) dmabuf_feedback;

    feedback->feedbackDone = feedback->unprocessedFeedback = true;
}

_Static_assert(sizeof(WlEglDmaBufFormatTableEntry) == 16,
        "Validate that this struct's layout wasn't modified by the compiler");

static void
dmabuf_feedback_format_table(void *data,
                             struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                             int32_t fd, uint32_t size)
{
    WlEglDmaBufFeedback *feedback = data;
    (void) dmabuf_feedback;

    assert(size % sizeof(WlEglDmaBufFormatTableEntry) == 0);
    feedback->formatTable.len = size / sizeof(WlEglDmaBufFormatTableEntry);

    feedback->formatTable.entry =
        (WlEglDmaBufFormatTableEntry *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (feedback->formatTable.entry == MAP_FAILED) {
        /*
         * Could not map the format table: Compositor bug or out of resources
         */
        feedback->formatTable.len = 0;
    }
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
    .done = dmabuf_feedback_done,
    .format_table = dmabuf_feedback_format_table,
    .main_device = dmabuf_feedback_main_device,
    .tranche_done = dmabuf_feedback_tranche_done,
    .tranche_target_device = dmabuf_feedback_tranche_target_device,
    .tranche_formats = dmabuf_feedback_tranche_formats,
    .tranche_flags = dmabuf_feedback_tranche_flags,
};

int
WlEglRegisterFeedback(WlEglDmaBufFeedback *feedback)
{
    fprintf(stderr, "Registering DMA-BUF feedback: %p\n", feedback->wlDmaBufFeedback);

    int result = zwp_linux_dmabuf_feedback_v1_add_listener(feedback->wlDmaBufFeedback,
                                                           &dmabuf_feedback_listener,
                                                           feedback);
    if (result != 0) {
        fprintf(stderr, "Failed to register DMA-BUF feedback listener\n");
    } else {
        fprintf(stderr, "Successfully registered DMA-BUF feedback listener\n");
    }
    return result;
}

static void
registry_handle_global(void *data,
                       struct wl_registry *registry,
                       uint32_t name,
                       const char *interface,
                       uint32_t version)
{
    WlEglDisplay *display = (WlEglDisplay *)data;

    fprintf(stderr, "Global registry: interface=%s, name=%u, version=%u\n", interface, name, version);

    if (strcmp(interface, "wl_eglstream_display") == 0) {
        fprintf(stderr, "Binding wl_eglstream_display\n");
        display->wlStreamDpy = wl_registry_bind(registry,
                                                name,
                                                &wl_eglstream_display_interface,
                                                1);
        if (!display->wlStreamDpy) {
            fprintf(stderr, "Failed to bind wl_eglstream_display\n");
        }
    } else if (strcmp(interface, "wl_eglstream_controller") == 0) {
        fprintf(stderr, "Binding wl_eglstream_controller, version=%u\n", version);
        display->wlStreamCtl = wl_registry_bind(registry,
                                                name,
                                                &wl_eglstream_controller_interface,
                                                version > 1 ? 2 : 1);
        display->wlStreamCtlVer = version;
        if (!display->wlStreamCtl) {
            fprintf(stderr, "Failed to bind wl_eglstream_controller\n");
        }
    } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
        fprintf(stderr, "Binding zwp_linux_dmabuf_v1, version=%u\n", version);
        if (version >= 3) {
            display->wlDmaBuf = wl_registry_bind(registry,
                                                 name,
                                                 &zwp_linux_dmabuf_v1_interface,
                                                 version > 3 ? 4 : 3);
            if (!display->wlDmaBuf) {
                fprintf(stderr, "Failed to bind zwp_linux_dmabuf_v1\n");
            }
        } else {
            fprintf(stderr, "Skipping zwp_linux_dmabuf_v1: version < 3\n");
        }
        display->dmaBufProtocolVersion = version;
    } else if (strcmp(interface, "wp_presentation") == 0) {
        fprintf(stderr, "Binding wp_presentation\n");
        display->wpPresentation = wl_registry_bind(registry,
                                                   name,
                                                   &wp_presentation_interface,
                                                   version);
        if (!display->wpPresentation) {
            fprintf(stderr, "Failed to bind wp_presentation\n");
        }
    } else if (strcmp(interface, "wp_linux_drm_syncobj_manager_v1") == 0 &&
               display->supports_native_fence_sync &&
               display->supports_explicit_sync) {
        fprintf(stderr, "Binding wp_linux_drm_syncobj_manager_v1\n");
        display->wlDrmSyncobj = wl_registry_bind(registry,
                                                 name,
                                                 &wp_linux_drm_syncobj_manager_v1_interface,
                                                 1);
        if (!display->wlDrmSyncobj) {
            fprintf(stderr, "Failed to bind wp_linux_drm_syncobj_manager_v1\n");
        }
    } else {
        fprintf(stderr, "Unknown interface: %s\n", interface);
    }
}


static void
registry_handle_global_remove(void *data,
                              struct wl_registry *registry,
                              uint32_t name)
{
    (void) data;
    (void) registry;
    (void) name;
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

static void wl_drm_device(void *data, struct wl_drm *wl_drm, const char *name)
{
    WlServerProtocols *protocols = (WlServerProtocols *)data;
    (void) wl_drm;

    free(protocols->drm_name);
    protocols->drm_name = strdup(name);
}

static void wl_drm_authenticated(void *data, struct wl_drm *wl_drm)
{
    (void) data;
    (void) wl_drm;
}
static void wl_drm_format(void *data, struct wl_drm *wl_drm, uint32_t format)
{
    (void) data;
    (void) wl_drm;
    (void) format;
}
static void wl_drm_capabilities(void *data, struct wl_drm *wl_drm, uint32_t value)
{
    (void) data;
    (void) wl_drm;
    (void) value;
}

static const struct wl_drm_listener drmListener = {
    .device = wl_drm_device,
    .authenticated = wl_drm_authenticated,
    .format = wl_drm_format,
    .capabilities = wl_drm_capabilities,
};


static void
dmabuf_feedback_check_main_device(void *data,
                            struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                            struct wl_array *dev)
{
    WlServerProtocols *protocols = (WlServerProtocols *)data;
    (void) dmabuf_feedback;

    assert(dev->size == sizeof(dev_t));
    memcpy(&protocols->devId, dev->data, sizeof(dev_t));
}

static void
dmabuf_feedback_check_tranche_target_device(void *data,
                                      struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                      struct wl_array *dev)
{
    (void) data;
    (void) dmabuf_feedback;
    (void) dev;
}

static void
dmabuf_feedback_check_tranche_flags(void *data,
                              struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                              uint32_t flags)
{
    (void) data;
    (void) dmabuf_feedback;
    (void) flags;
}

static void
dmabuf_feedback_check_tranche_formats(void *data,
                                struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                struct wl_array *indices)
{
    (void) data;
    (void) dmabuf_feedback;
    (void) indices;
}

static void
dmabuf_feedback_check_tranche_done(void *data,
                             struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
    (void) data;
    (void) dmabuf_feedback;
}

static void
dmabuf_feedback_check_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
    WlServerProtocols *protocols = (WlServerProtocols *)data;
    drmDevice *drm_device;

    (void) dmabuf_feedback;

    assert(getDeviceFromDevId);
    if (getDeviceFromDevId(protocols->devId, 0, &drm_device) == 0) {
        if (drm_device->available_nodes & (1 << DRM_NODE_RENDER)) {
            free(protocols->drm_name);
            protocols->drm_name = strdup(drm_device->nodes[DRM_NODE_RENDER]);
        }

        drmFreeDevice(&drm_device);
    }
}

static void
dmabuf_feedback_check_format_table(void *data,
                             struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                             int32_t fd, uint32_t size)
{
    (void) data;
    (void) dmabuf_feedback;
    (void) fd;
    (void) size;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_check_listener = {
        .done = dmabuf_feedback_check_done,
        .format_table = dmabuf_feedback_check_format_table,
        .main_device = dmabuf_feedback_check_main_device,
        .tranche_done = dmabuf_feedback_check_tranche_done,
        .tranche_target_device = dmabuf_feedback_check_tranche_target_device,
        .tranche_formats = dmabuf_feedback_check_tranche_formats,
        .tranche_flags = dmabuf_feedback_check_tranche_flags,
};


static void
registry_handle_global_check_protocols(
                       void *data,
                       struct wl_registry *registry,
                       uint32_t name,
                       const char *interface,
                       uint32_t version){
    WlServerProtocols *protocols = (WlServerProtocols *)data;
    fprintf(stderr, "Registry event: interface=%s, version=%u\n", interface, version);

    if (strcmp(interface, "wl_eglstream_display") == 0) {
        protocols->hasEglStream = EGL_TRUE;
        fprintf(stderr, "Found wl_eglstream_display protocol\n");
    }

    if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0 && version >= 3) {
        protocols->hasDmaBuf = EGL_TRUE;
        fprintf(stderr, "Found zwp_linux_dmabuf_v1 protocol (version %u)\n", version);
        if (version >= 4) {
            protocols->wlDmaBuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 4);
            if (protocols->wlDmaBuf) {
                fprintf(stderr, "Bound zwp_linux_dmabuf_v1 interface (version 4)\n");
            } else {
                fprintf(stderr, "Failed to bind zwp_linux_dmabuf_v1 interface\n");
            }
        }
    }

    if (strcmp(interface, "wl_drm") == 0 && version >= 2) {
        protocols->wlDrm = wl_registry_bind(registry, name, &wl_drm_interface, 2);
        if (protocols->wlDrm) {
            fprintf(stderr, "Bound wl_drm interface\n");
            wl_drm_add_listener(protocols->wlDrm, &drmListener, protocols);
        } else {
            fprintf(stderr, "Failed to bind wl_drm interface\n");
        }
    }
}

static void
eglstream_display_handle_caps(void *data,
                              struct wl_eglstream_display *wlStreamDpy,
                              int32_t caps)
{
    WlEglDisplay *dpy = (WlEglDisplay *)data;
    (void) wlStreamDpy;

#define IS_CAP_SET(CAPS, CAP) (((CAPS)&(CAP)) != 0)

    dpy->caps.stream_fd     = IS_CAP_SET(caps,
                                         WL_EGLSTREAM_DISPLAY_CAP_STREAM_FD);
    dpy->caps.stream_inet   = IS_CAP_SET(caps,
                                         WL_EGLSTREAM_DISPLAY_CAP_STREAM_INET);
    dpy->caps.stream_socket = IS_CAP_SET(caps,
                                         WL_EGLSTREAM_DISPLAY_CAP_STREAM_SOCKET);

#undef IS_CAP_SET
}

static void
eglstream_display_handle_swapinterval_override(
                                    void *data,
                                    struct wl_eglstream_display *wlStreamDpy,
                                    int32_t swapinterval,
                                    struct wl_buffer *streamResource)
{
    WlEglDisplay *dpy = (WlEglDisplay *)data;
    WlEglSurface *surf = NULL;
    (void) wlStreamDpy;

    wl_list_for_each(surf, &dpy->wlEglSurfaceList, link) {
        if (surf->ctx.wlStreamResource == streamResource) {
            WlEglPlatformData *pData = surf->wlEglDpy->data;
            EGLDisplay         dpy1   = surf->wlEglDpy->devDpy->eglDisplay;

            if (pData->egl.swapInterval(dpy1, swapinterval)) {
                surf->swapInterval = swapinterval;
            }

            break;
        }
    }
}


static const struct wl_eglstream_display_listener eglstream_display_listener = {
    eglstream_display_handle_caps,
    eglstream_display_handle_swapinterval_override,
};

/* On wayland, when a wl_display backed EGLDisplay is created and then
 * wl_display is destroyed without terminating EGLDisplay first, some
 * driver allocated resources associated with wl_display could not be
 * destroyed properly during EGL teardown.
 * Per EGL spec: Termination of a display that has already been terminated,
 * or has not yet been initialized, is allowed, but the only effect of such
 * a call is to return EGL_TRUE, since there are no EGL resources associated
 * with the display to release.
 * However, in our wayland egl driver, we do allocate some resources
 * which are associated with wl_display even eglInitialize is not called.
 * If the app does not terminate EGLDisplay before closing wl_display,
 * it can hit assertion or hang in pthread_mutex_lock during EGL teardown.
 * To WAR the issue, in case wl_display has been destroyed, we skip
 * destroying some resources during EGL system termination, only when
 * terminateDisplay is called from wlEglDestroyAllDisplays.
 */
static EGLBoolean terminateDisplay(WlEglDisplay *display, EGLBoolean globalTeardown)
{
    if (display->initCount == 0) {
        return EGL_TRUE;
    }

    /* If globalTeardown is true, then ignore the refcount and terminate the
       display. That's used when the library is unloaded. */
    if (display->initCount > 1 && !globalTeardown) {
        display->initCount--;
        return EGL_TRUE;
    }

    if (!wlInternalTerminate(display->devDpy)) {
        if (!globalTeardown) {
            return EGL_FALSE;
        }
    }
    display->initCount = 0;

    /* First, destroy any surface associated to the given display. Then
     * destroy the display connection itself */
    wlEglDestroyAllSurfaces(display);

    if (!globalTeardown || display->ownNativeDpy) {
        wlEglDestroyFormatSet(&display->formatSet);
        wlEglDestroyFeedback(&display->defaultFeedback);

        if (display->wlRegistry) {
            wl_registry_destroy(display->wlRegistry);
            display->wlRegistry = NULL;
        }
        if (display->wlStreamDpy) {
            wl_eglstream_display_destroy(display->wlStreamDpy);
            display->wlStreamDpy = NULL;
        }
        if (display->wlStreamCtl) {
            wl_eglstream_controller_destroy(display->wlStreamCtl);
            display->wlStreamCtl = NULL;
        }
        if (display->wpPresentation) {
            wp_presentation_destroy(display->wpPresentation);
            display->wpPresentation = NULL;
        }
        if (display->wlDrmSyncobj) {
            wp_linux_drm_syncobj_manager_v1_destroy(display->wlDrmSyncobj);
            display->wlDrmSyncobj = NULL;
        }
        if (display->wlDmaBuf) {
            zwp_linux_dmabuf_v1_destroy(display->wlDmaBuf);
            display->wlDmaBuf = NULL;
        }
        /* all proxies using the queue must be destroyed first! */
        if (display->wlEventQueue) {
            wl_event_queue_destroy(display->wlEventQueue);
            display->wlEventQueue = NULL;
        }
    }

    return EGL_TRUE;
}

EGLBoolean wlEglTerminateHook(EGLDisplay dpy)
{
    WlEglDisplay *display = wlEglAcquireDisplay(dpy);
    EGLBoolean res;

    if (!display) {
        return EGL_FALSE;
    }
    pthread_mutex_lock(&display->mutex);
    res = terminateDisplay(display, EGL_FALSE);
    pthread_mutex_unlock(&display->mutex);
    wlEglReleaseDisplay(display);

    return res;
}

static bool getServerProtocolsInfo(struct wl_display *nativeDpy,
                                   WlServerProtocols *protocols)
{
    struct wl_display     *wrapper      = NULL;
    struct wl_registry    *wlRegistry   = NULL;
    struct wl_event_queue *queue        = wl_display_create_queue(nativeDpy);
    int                    ret          = 0;
    bool                   result       = false;
    const struct wl_registry_listener registryListener = {
        registry_handle_global_check_protocols,
        registry_handle_global_remove
    };

    if (queue == NULL) {
        goto done;
    }

    wrapper = wl_proxy_create_wrapper(nativeDpy);
    if (wrapper == NULL) {
        goto done;
    }
    wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);

    /* Listen to wl_registry events and make a roundtrip in order to find the
     * wl_eglstream_display global object.
     */
    wlRegistry = wl_display_get_registry(wrapper);
    if (wlRegistry == NULL) {
        goto done;
    }
    ret = wl_registry_add_listener(wlRegistry,
                                   &registryListener,
                                   protocols);
    if (ret == 0) {
        wl_display_roundtrip_queue(nativeDpy, queue);
        /* use a second roundtrip to handle any wl_drm events triggered by binding the protocol */
        wl_display_roundtrip_queue(nativeDpy, queue);

        if (!getDeviceFromDevIdInitialised) {
            getDeviceFromDevId = dlsym(RTLD_DEFAULT, "drmGetDeviceFromDevId");
            getDeviceFromDevIdInitialised = true;
        }

        /*
         * if dmabuf feedback is available then use that. This will potentially
         * replace the drm_name provided by wl_drm, assuming the feedback provides
         * a valid dev_t.
         */
        if (protocols->wlDmaBuf && getDeviceFromDevId) {
            struct zwp_linux_dmabuf_feedback_v1 *default_feedback
                    = zwp_linux_dmabuf_v1_get_default_feedback(protocols->wlDmaBuf);
            if (default_feedback) {
                zwp_linux_dmabuf_feedback_v1_add_listener(default_feedback, &dmabuf_feedback_check_listener, protocols);
                wl_display_roundtrip_queue(nativeDpy, queue);
                zwp_linux_dmabuf_feedback_v1_destroy(default_feedback);
            }
        }

        /* Check that one of our two protocols provided the device name */
        result = protocols->drm_name != NULL;

        if (protocols->wlDmaBuf) {
            zwp_linux_dmabuf_v1_destroy(protocols->wlDmaBuf);
        }
        if (protocols->wlDrm) {
            wl_drm_destroy(protocols->wlDrm);
        }
    }

done:
    if (wrapper) {
        wl_proxy_wrapper_destroy(wrapper);
    }
    if (wlRegistry) {
        wl_registry_destroy(wlRegistry);
    }
    if (queue) {
        wl_event_queue_destroy(queue);
    }
    return result;
}

static EGLBoolean checkNvidiaDrmDevice(WlServerProtocols *protocols)
{
    int fd = -1;
    EGLBoolean result = EGL_FALSE;
    drmVersion *version = NULL;
    drmDevice *dev = NULL;


    fprintf(stderr, "Checking NVIDIA DRM device...\n");

    if (protocols->drm_name == NULL) {
        fprintf(stderr, "DRM name is NULL. Skipping check.\n");
        goto done;
    }

    fprintf(stderr, "Opening DRM device: %s\n", protocols->drm_name);
    fd = open(protocols->drm_name, O_RDWR);
    if (fd < 0) {
        perror("Failed to open DRM device");
        goto done;
    }

    fprintf(stderr, "Calling drmGetDevice...\n");
    if (drmGetDevice(fd, &dev) == 0) {
        fprintf(stderr, "drmGetDevice succeeded. Available nodes: %d\n", dev->available_nodes);
        if (dev->available_nodes & (1 << DRM_NODE_RENDER)) {
            fprintf(stderr, "Render node found: %s\n", dev->nodes[DRM_NODE_RENDER]);
            // Ensure protocols->drm_name points to the render node
            if (strcmp(protocols->drm_name, dev->nodes[DRM_NODE_RENDER]) != 0) {
                fprintf(stderr, "Updating DRM name to render node: %s\n", dev->nodes[DRM_NODE_RENDER]);
                free(protocols->drm_name);
                protocols->drm_name = strdup(dev->nodes[DRM_NODE_RENDER]);
                if (protocols->drm_name == NULL) {
                    fprintf(stderr, "Failed to update DRM name to render node\n");
                    goto done;
                }
            }
        }

        fprintf(stderr, "Checking if device is NVIDIA PCI...\n");
        if (dev->bustype == DRM_BUS_PCI && dev->deviceinfo.pci->vendor_id == 0x10de) {
            fprintf(stderr, "NVIDIA PCI device detected. Vendor ID: 0x10de\n");
            result = EGL_TRUE;
        }
    } else {
        fprintf(stderr, "drmGetDevice failed.\n");
    }

    if (!result) {
        fprintf(stderr, "Checking DRM version for NVIDIA drivers...\n");
        version = drmGetVersion(fd);
        if (version != NULL && version->name != NULL) {
            fprintf(stderr, "DRM version name: %s\n", version->name);
            if (strcmp(version->name, "nvidia-drm") == 0
                || strcmp(version->name, "tegra-udrm") == 0
                || strcmp(version->name, "tegra") == 0) {
                fprintf(stderr, "NVIDIA DRM driver detected: %s\n", version->name);
                result = EGL_TRUE;
            } else {
                fprintf(stderr, "Driver is not NVIDIA. Detected: %s\n", version->name);
            }
        } else {
            fprintf(stderr, "Failed to retrieve DRM version.\n");
        }
    }


done:
    if (version != NULL) {
        drmFreeVersion(version);
    }
    if (dev != NULL) {
        drmFreeDevice(&dev);
    }
    if (fd >= 0) {
        close(fd);
    }
    return result;
}

EGLDisplay wlEglGetPlatformDisplayExport(void *data,
                                         EGLenum platform,
                                         void *nativeDpy,
                                         const EGLAttrib *attribs){
    fprintf(stderr, "[DEBUG] Entrando no método wlEglGetPlatformDisplayExport\n");

    WlEglPlatformData     *pData           = (WlEglPlatformData *)data;
    WlEglDisplay          *display         = NULL;
    WlServerProtocols      protocols       = {};
    EGLint                 numDevices      = 0;
    int                    i               = 0;
    EGLDeviceEXT          *eglDeviceList   = NULL;
    EGLDeviceEXT           eglDevice       = NULL;
    EGLint                 err             = EGL_SUCCESS;
    EGLBoolean             useInitRefCount = EGL_FALSE;
    const char *primeRenderOffloadStr;

    EGLDeviceEXT requestedDevice = EGL_NO_DEVICE_EXT;
    EGLBoolean usePrimeRenderOffload = EGL_FALSE;
    EGLBoolean isServerNV;
    const char *drmName = NULL;

    if (platform != EGL_PLATFORM_WAYLAND_EXT) {
        fprintf(stderr, "[ERROR] Plataforma inválida recebida: %d\n", platform);
        wlEglSetError(data, EGL_BAD_PARAMETER);
        return EGL_NO_DISPLAY;
    }

    /* Check the attribute list */
    if (attribs) {
        for (i = 0; attribs[i] != EGL_NONE; i += 2) {


            if (attribs[i] == EGL_TRACK_REFERENCES_KHR) {
                if (attribs[i + 1] == EGL_TRUE || attribs[i + 1] == EGL_FALSE) {
                    useInitRefCount = (EGLBoolean) attribs[i + 1];
                } else {
                    fprintf(stderr, "[ERROR] Atributo inválido para EGL_TRACK_REFERENCES_KHR\n");
                    wlEglSetError(data, EGL_BAD_ATTRIBUTE);
                    return EGL_NO_DISPLAY;
                }
            } else if (attribs[i] == EGL_DEVICE_EXT) {
                requestedDevice = (EGLDeviceEXT) attribs[i + 1];
                if (requestedDevice == EGL_NO_DEVICE_EXT) {
                    fprintf(stderr, "[ERROR] Dispositivo inválido recebido\n");
                    wlEglSetError(data, EGL_BAD_DEVICE_EXT);
                    return EGL_NO_DISPLAY;
                }
            } else {

                wlEglSetError(data, EGL_BAD_ATTRIBUTE);
                return EGL_NO_DISPLAY;
            }
        }
    }

    wlExternalApiLock();
    fprintf(stderr, "[DEBUG] Bloqueio externo aplicado\n");

    /* Check for existing displays */
    wl_list_for_each(display, &wlEglDisplayList, link) {
        if ((display->nativeDpy == nativeDpy || (!nativeDpy && display->ownNativeDpy)) &&
            display->useInitRefCount == useInitRefCount &&
            display->requestedDevice == requestedDevice) {
            fprintf(stderr, "[DEBUG] Display existente encontrado\n");
            wlExternalApiUnlock();
            return (EGLDisplay)display;
        }
    }

    display = calloc(1, sizeof(*display));
    if (!display) {
        fprintf(stderr, "[ERROR] Falha ao alocar memória para display\n");
        err = EGL_BAD_ALLOC;
        goto fail;
    }

    display->data = pData;
    display->nativeDpy = nativeDpy;
    display->useInitRefCount = useInitRefCount;
    display->requestedDevice = requestedDevice;

    if (!display->nativeDpy) {
        display->nativeDpy = wl_display_connect(NULL);
        if (!display->nativeDpy) {
            fprintf(stderr, "[ERROR] Falha ao conectar ao display Wayland\n");
            err = EGL_BAD_ALLOC;
            goto fail;
        }

        display->ownNativeDpy = EGL_TRUE;
        wl_display_dispatch_pending(display->nativeDpy);
    }

    fprintf(stderr, "[DEBUG] Conexão Wayland estabelecida\n");

    primeRenderOffloadStr = getenv("__NV_PRIME_RENDER_OFFLOAD");
    if (primeRenderOffloadStr && !strcmp(primeRenderOffloadStr, "1")) {
        usePrimeRenderOffload = EGL_TRUE;
        fprintf(stderr, "[DEBUG] PRIME Render Offload ativado\n");
    }

    if (!getServerProtocolsInfo(display->nativeDpy, &protocols)) {
        fprintf(stderr, "[ERROR] Falha ao obter informações dos protocolos do servidor\n");
        err = EGL_BAD_ALLOC;
        goto fail;
    }

    fprintf(stderr, "[DEBUG] Protocolos do servidor obtidos\n");

    isServerNV = checkNvidiaDrmDevice(&protocols);
    if (!usePrimeRenderOffload && requestedDevice == EGL_NO_DEVICE_EXT) {
        if (!isServerNV) {
            fprintf(stderr, "[DEBUG] O servidor não está em uma GPU NVIDIA\n");
            err = EGL_SUCCESS;
            goto fail;
        }
    }

    if (!protocols.hasEglStream && !protocols.hasDmaBuf) {
        fprintf(stderr, "[ERROR] O compositor não suporta EGLStream nem DMA-BUF\n");
        goto fail;
    }

    if (!pData->egl.queryDevices(-1, NULL, &numDevices) || numDevices == 0) {
        fprintf(stderr, "[ERROR] Falha ao consultar dispositivos EGL\n");
        goto fail;
    }

    fprintf(stderr, "[DEBUG] Número de dispositivos EGL disponíveis: %d\n", numDevices);

    eglDeviceList = calloc(numDevices, sizeof(*eglDeviceList));
    if (!eglDeviceList) {
        fprintf(stderr, "[ERROR] Falha ao alocar memória para lista de dispositivos\n");
        goto fail;
    }

    if (!pData->egl.queryDevices(numDevices, eglDeviceList, &numDevices) || numDevices == 0) {
        fprintf(stderr, "[ERROR] Falha ao consultar dispositivos EGL\n");
        goto fail;
    }

    for (i = 0; i < numDevices; i++) {
        const char *dev_name = pData->egl.queryDeviceString(eglDeviceList[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        fprintf(stderr, "[DEBUG] Dispositivo EGL encontrado: %s\n", dev_name);
    }

    display->devDpy = wlGetInternalDisplay(pData, eglDevice);
    if (display->devDpy == NULL) {
        fprintf(stderr, "[ERROR] Falha ao obter display interno\n");
        goto fail;
    }

    drmName = display->data->egl.queryDeviceString(display->devDpy->eglDevice, EGL_DRM_DEVICE_FILE_EXT);
    if (!drmName) {
        fprintf(stderr, "[ERROR] Falha ao obter nome do dispositivo DRM\n");
        goto fail;
    }

    fprintf(stderr, "[DEBUG] Dispositivo DRM em uso: %s\n", drmName);

    display->drmFd = open(drmName, O_RDWR | O_CLOEXEC);
    if (display->drmFd < 0) {
        fprintf(stderr, "[ERROR] Falha ao abrir o dispositivo DRM\n");
        goto fail;
    }

    wl_list_insert(&wlEglDisplayList, &display->link);

    fprintf(stderr, "[DEBUG] Display criado com sucesso\n");

    free(eglDeviceList);
    if (protocols.drm_name) {
        free(protocols.drm_name);
    }
    wlExternalApiUnlock();
    return display;

    fail:
    wlExternalApiUnlock();
    free(eglDeviceList);
    free(protocols.drm_name);

    if (display && display->ownNativeDpy) {
        wl_display_disconnect(display->nativeDpy);
    }
    free(display);

    if (err != EGL_SUCCESS) {
        wlEglSetError(data, err);
    }

    fprintf(stderr, "[ERROR] Saindo do método wlEglGetPlatformDisplayExport com falha\n");
    return EGL_NO_DISPLAY;
}
static void wlEglCheckDriverSyncSupport(WlEglDisplay *display)
{
    EGLSyncKHR  eglSync = EGL_NO_SYNC_KHR;
    int         syncFd  = -1;
    EGLDisplay  dpy     = display->devDpy->eglDisplay;
    EGLint      attribs[5];
    uint32_t    tmpSyncobj;
    const char *disableExplicitSyncStr = getenv("__NV_DISABLE_EXPLICIT_SYNC");

    /*
     * Don't enable explicit sync if requested by the user or if we do not have
     * the necessary EGL extensions.
     */
    if ((disableExplicitSyncStr && !strcmp(disableExplicitSyncStr, "1")) ||
        !display->supports_native_fence_sync) {
        return;
    }

    /* make a dummy fd to pass in */
    if (drmSyncobjCreate(display->drmFd, 0, &tmpSyncobj) != 0) {
        return;
    }

    if (drmSyncobjHandleToFD(display->drmFd, tmpSyncobj, &syncFd)) {
        goto destroy;
    }

    /*
     * This call is supposed to fail if the driver is new enough to support
     * Explicit Sync. Since we don't have an easy way to detect the driver
     * version number at the moment, we check for some error conditions added
     * as part of the EGL driver support. Here we check that specifying a valid
     * fd and a sync object status returns EGL_BAD_ATTRIBUTE.
     */
    attribs[0] = EGL_SYNC_NATIVE_FENCE_FD_ANDROID;
    attribs[1] = syncFd;
    attribs[2] = EGL_SYNC_STATUS;
    attribs[3] = EGL_SIGNALED;
    attribs[4] = EGL_NONE;
    eglSync = display->data->egl.createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID,
                                            attribs);

    /* If the call failed then the driver version is recent enough */
    if (eglSync == EGL_NO_SYNC_KHR &&
        display->data->egl.getError() == EGL_BAD_ATTRIBUTE) {
        display->supports_explicit_sync = true;
    }
fprintf(stderr, "[ERROR] Saindo do método wlEglGetPlatformDisplayExport com falha\n");
destroy:
    if (eglSync != EGL_NO_SYNC_KHR) {
        display->data->egl.destroySync(dpy, eglSync);
    }
    drmSyncobjDestroy(display->drmFd, tmpSyncobj);
}

EGLBoolean wlEglInitializeHook(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    WlEglDisplay      *display = wlEglAcquireDisplay(dpy);
    WlEglPlatformData *data    = NULL;
    struct wl_display *wrapper = NULL;
    EGLint             err     = EGL_SUCCESS;
    int                ret     = 0;
    const char *dev_exts = NULL;

    if (!display) {
        return EGL_FALSE;
    }
    pthread_mutex_lock(&display->mutex);

    data = display->data;

    if (display->initCount > 0) {
        // This display has already been initialized.
        if (major) {
                *major = display->devDpy->major;
        }
        if (minor) {
                *minor = display->devDpy->minor;
        }
        if (display->useInitRefCount) {
            display->initCount++;
        }
        pthread_mutex_unlock(&display->mutex);
        wlEglReleaseDisplay(display);
        return EGL_TRUE;
    }

    if (!wlInternalInitialize(display->devDpy)) {
        pthread_mutex_unlock(&display->mutex);
        wlEglReleaseDisplay(display);
        return EGL_FALSE;
    }

    dev_exts = display->data->egl.queryString(display->devDpy->eglDisplay, EGL_EXTENSIONS);
    if (dev_exts && wlEglFindExtension("EGL_ANDROID_native_fence_sync", dev_exts)) {
        display->supports_native_fence_sync = true;
    }

    /* Check if we support explicit sync */
    wlEglCheckDriverSyncSupport(display);

    // Set the initCount to 1. If something goes wrong, then terminateDisplay
    // will clean up and set it back to zero.
    display->initCount = 1;

    display->wlEventQueue =  wl_display_create_queue(display->nativeDpy);;
    if (display->wlEventQueue == NULL) {
        err = EGL_BAD_ALLOC;
        goto fail;
    }

    wrapper = wl_proxy_create_wrapper(display->nativeDpy);
    wl_proxy_set_queue((struct wl_proxy *)wrapper, display->wlEventQueue);

    /* Listen to wl_registry events and make a roundtrip in order to find the
     * wl_eglstream_display and/or zwp_linux_dmabuf_v1 global object
     */
    display->wlRegistry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper); /* Done with wrapper */
    ret = wl_registry_add_listener(display->wlRegistry,
                                   &registry_listener,
                                   display);
    if (ret == 0) {
        ret = wl_display_roundtrip_queue(display->nativeDpy, display->wlEventQueue);
    }
    if (ret < 0) {
        err = EGL_BAD_ALLOC;
        goto fail;
    }

    if (display->wlStreamDpy) {
        /* Listen to wl_eglstream_display events */
        ret = wl_eglstream_display_add_listener(display->wlStreamDpy,
                                                &eglstream_display_listener,
                                                display);
    } else if (display->wlDmaBuf) {
        ret = zwp_linux_dmabuf_v1_add_listener(display->wlDmaBuf,
                                               &dmabuf_listener,
                                               display);

        if (ret == 0 && display->dmaBufProtocolVersion >= 4) {
            /* Since the compositor supports it, opt into surface format feedback */
            display->defaultFeedback.wlDmaBufFeedback =
                zwp_linux_dmabuf_v1_get_default_feedback(display->wlDmaBuf);
            if (display->defaultFeedback.wlDmaBufFeedback) {
                ret = WlEglRegisterFeedback(&display->defaultFeedback);
            }
        }
    }

    if (ret < 0 || !(display->wlStreamDpy || display->wlDmaBuf)) {
        /* This library requires either the EGLStream or dma-buf protocols to
         * present content to the Wayland compositor.
         */
        err = EGL_BAD_ALLOC;
        goto fail;
    }

    /*
     * Make another roundtrip so we catch any bind-related event (e.g. server capabilities)
     */
    ret = wl_display_roundtrip_queue(display->nativeDpy, display->wlEventQueue);
    if (ret < 0) {
        err = EGL_BAD_ALLOC;
        goto fail;
    }

    /* We haven't created any surfaces yet, so no need to reallocate. */
    display->defaultFeedback.unprocessedFeedback = false;

    if (major != NULL) {
        *major = display->devDpy->major;
    }
    if (minor != NULL) {
        *minor = display->devDpy->minor;
    }

    pthread_mutex_unlock(&display->mutex);
    wlEglReleaseDisplay(display);
    return EGL_TRUE;

fail:
    terminateDisplay(display, EGL_FALSE);
    if (err != EGL_SUCCESS) {
        wlEglSetError(data, err);
    }
    pthread_mutex_unlock(&display->mutex);
    wlEglReleaseDisplay(display);
    return EGL_FALSE;
}

EGLBoolean wlEglIsWlEglDisplay(WlEglDisplay *display)
{
    WlEglDisplay *dpy;

    wl_list_for_each(dpy, &wlEglDisplayList, link) {
        if (dpy == display) {
            return EGL_TRUE;
        }
    }

    return EGL_FALSE;
}

WlEglDisplay *wlEglAcquireDisplay(EGLDisplay dpy) {
    WlEglDisplay *display = (WlEglDisplay *)dpy;
    wlExternalApiLock();
    if (wlEglIsWlEglDisplay(display)) {
        ++display->refCount;
    } else {
        display = NULL;
    }
    wlExternalApiUnlock();
    return display;
}

static void wlEglUnrefDisplay(WlEglDisplay *display) {
    if (--display->refCount == 0) {
        wlEglMutexDestroy(&display->mutex);
        close(display->drmFd);
        free(display);
    }
}

void wlEglReleaseDisplay(WlEglDisplay *display) {
    wlExternalApiLock();
    wlEglUnrefDisplay(display);
    wlExternalApiUnlock();
}

EGLBoolean wlEglChooseConfigHook(EGLDisplay dpy,
                                 EGLint const *attribs,
                                 EGLConfig *configs,
                                 EGLint configSize,
                                 EGLint *numConfig)
{
    WlEglDisplay      *display       = (WlEglDisplay *)dpy;
    WlEglPlatformData *data          = display->data;
    EGLint            *attribs2      = NULL;
    EGLint             nAttribs      = 0;
    EGLint             nTotalAttribs = 0;
    EGLBoolean         surfType      = EGL_FALSE;
    EGLint             err           = EGL_SUCCESS;
    EGLBoolean         ret;

    /* Save the internal EGLDisplay handle, as it's needed by the actual
     * eglChooseConfig() call */
    dpy = display->devDpy->eglDisplay;

    /* Calculate number of attributes in attribs */
    if (attribs) {
        while (attribs[nAttribs] != EGL_NONE) {
            surfType = surfType || (attribs[nAttribs] == EGL_SURFACE_TYPE);
            nAttribs += 2;
        }
    }

    /* If not SURFACE_TYPE provided, we need convert the default WINDOW_BIT to a
     * default EGL_STREAM_BIT */
    nTotalAttribs += (surfType ? nAttribs : (nAttribs + 2));

    /* Make attributes list copy */
    attribs2 = (EGLint *)malloc((nTotalAttribs + 1) * sizeof(*attribs2));
    if (!attribs2) {
        err = EGL_BAD_ALLOC;
        goto done;
    }

    if (nAttribs > 0) {
        memcpy(attribs2, attribs, nAttribs * sizeof(*attribs2));
    }
    attribs2[nTotalAttribs] = EGL_NONE;

    /* Replace all WINDOW_BITs by EGL_STREAM_BITs */
    if (surfType) {
        nAttribs = 0;
        while (attribs2[nAttribs] != EGL_NONE) {
            if ((attribs2[nAttribs] == EGL_SURFACE_TYPE) &&
                (attribs2[nAttribs + 1] != EGL_DONT_CARE) &&
                (attribs2[nAttribs + 1] & EGL_WINDOW_BIT)) {
                attribs2[nAttribs + 1] &= ~EGL_WINDOW_BIT;
                attribs2[nAttribs + 1] |= EGL_STREAM_BIT_KHR;
            }
            nAttribs += 2;
        }
    } else {
        attribs2[nTotalAttribs - 2] = EGL_SURFACE_TYPE;
        attribs2[nTotalAttribs - 1] = EGL_STREAM_BIT_KHR;
    }

    /* Actual eglChooseConfig() call */
    ret = data->egl.chooseConfig(dpy,
                                 attribs2,
                                 configs,
                                 configSize,
                                 numConfig);

done:
    /* Cleanup */
    free(attribs2);

    if (err != EGL_SUCCESS) {
        wlEglSetError(data, err);
        return EGL_FALSE;
    }

    return ret;
}

EGLBoolean wlEglGetConfigAttribHook(EGLDisplay dpy,
                                    EGLConfig config,
                                    EGLint attribute,
                                    EGLint *value)
{
    WlEglDisplay      *display = (WlEglDisplay *)dpy;
    WlEglPlatformData *data    = display->data;
    EGLBoolean         ret     = EGL_FALSE;

    /* Save the internal EGLDisplay handle, as it's needed by the actual
     * eglGetConfigAttrib() call */
    dpy = display->devDpy->eglDisplay;

    ret = data->egl.getConfigAttrib(dpy, config, attribute, value);
    if (ret && (attribute == EGL_SURFACE_TYPE)) {
        /* We only support window configurations through EGLStreams */
        if (*value & EGL_STREAM_BIT_KHR) {
            *value |= EGL_WINDOW_BIT;
        } else {
            *value &= ~EGL_WINDOW_BIT;
        }
    }

    return ret;
}

EGLBoolean wlEglQueryDisplayAttribHook(EGLDisplay dpy,
                                       EGLint name,
                                       EGLAttrib *value)
{
    WlEglDisplay *display = wlEglAcquireDisplay(dpy);
    WlEglPlatformData *data = NULL;
    EGLBoolean ret = EGL_TRUE;

    if (!display) {
        return EGL_FALSE;
    }
    pthread_mutex_lock(&display->mutex);

    data = display->data;

    if (value == NULL) {
        wlEglSetError(data, EGL_BAD_PARAMETER);
        pthread_mutex_unlock(&display->mutex);
        wlEglReleaseDisplay(display);
        return EGL_FALSE;
    }

    if (display->initCount == 0) {
        wlEglSetError(data, EGL_NOT_INITIALIZED);
        pthread_mutex_unlock(&display->mutex);
        wlEglReleaseDisplay(display);
        return EGL_FALSE;
    }

    switch (name) {
    case EGL_DEVICE_EXT:
        *value = (EGLAttrib) display->devDpy->eglDevice;
        break;
    case EGL_TRACK_REFERENCES_KHR:
        *value = (EGLAttrib) display->useInitRefCount;
        break;
    default:
        ret = data->egl.queryDisplayAttrib(display->devDpy->eglDisplay, name, value);
        break;
    }

    pthread_mutex_unlock(&display->mutex);
    wlEglReleaseDisplay(display);
    return ret;
}

EGLBoolean wlEglDestroyAllDisplays(WlEglPlatformData *data)
{
    WlEglDisplay *display, *next;

    EGLBoolean res = EGL_TRUE;

    wlExternalApiLock();

    wl_list_for_each_safe(display, next, &wlEglDisplayList, link) {
        if (display->data == data) {
            pthread_mutex_lock(&display->mutex);
            res = terminateDisplay(display, EGL_TRUE) && res;
            if (display->ownNativeDpy) {
                wl_display_disconnect(display->nativeDpy);
            }
            display->devDpy = NULL;
            pthread_mutex_unlock(&display->mutex);
            wl_list_remove(&display->link);
            /* Unref the external display */
            wlEglUnrefDisplay(display);
        }
    }

    wlFreeAllInternalDisplays(data);

    wlExternalApiUnlock();

    return res;
}

const char* wlEglQueryStringExport(void *data,
                                   EGLDisplay dpy,
                                   EGLExtPlatformString name)
{
    WlEglPlatformData *pData   = (WlEglPlatformData *)data;
    EGLBoolean         isEGL15 = (pData->egl.major > 1) ||
                                 ((pData->egl.major == 1) &&
                                  (pData->egl.minor >= 5));
    const char        *res     = NULL;

    switch (name) {
    case EGL_EXT_PLATFORM_PLATFORM_CLIENT_EXTENSIONS:
        res = isEGL15 ? "EGL_KHR_platform_wayland EGL_EXT_platform_wayland EGL_EXT_explicit_device" :
                        "EGL_EXT_platform_wayland";
        break;

    case EGL_EXT_PLATFORM_DISPLAY_EXTENSIONS:
        if (dpy == EGL_NO_DISPLAY) {
            /* This should return all client extensions, which for now is
             * equivalent to EXTERNAL_PLATFORM_CLIENT_EXTENSIONS */
            res = isEGL15 ? "EGL_KHR_platform_wayland EGL_EXT_platform_wayland EGL_EXT_explicit_device" :
                            "EGL_EXT_platform_wayland";
        } else {
            /*
             * Check whether the given display supports EGLStream
             * extensions. For Wayland support over EGLStreams, at least the
             * following extensions must be supported by the underlying
             * driver:
             *
             *  - EGL_KHR_stream
             *  - EGL_KHR_stream_producer_eglsurface
             *  - EGL_KHR_stream_cross_process_fd
             *
             * For Wayland support via dma-buf, at least the following
             * extensions must be supported by the underlying driver:
             *
             *  - EGL_KHR_stream
             *  - EGL_KHR_stream_producer_eglsurface
             *  - EGL_NV_stream_consumer_eglimage
             *  - EGL_MESA_image_dma_buf_export
             */
            const char *exts = pData->egl.queryString(dpy, EGL_EXTENSIONS);

            if (wlEglFindExtension("EGL_KHR_stream", exts) &&
                wlEglFindExtension("EGL_KHR_stream_producer_eglsurface",
                                   exts)) {
                if (wlEglFindExtension("EGL_KHR_stream_cross_process_fd",
                                       exts)) {
                    res = "EGL_EXT_present_opaque EGL_WL_bind_wayland_display "
                        "EGL_WL_wayland_eglstream";
                } else if (wlEglFindExtension("EGL_NV_stream_consumer_eglimage",
                                              exts) &&
                           wlEglFindExtension("EGL_MESA_image_dma_buf_export",
                                              exts)) {
                    res = "EGL_EXT_present_opaque EGL_WL_bind_wayland_display";
                }
            }
        }
        break;

    default:
        break;
    }

    return res;
}
