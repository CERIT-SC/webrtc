/* GStreamer
 *
 * Copyright (C) 2006 Zaheer Merali <zaheerabbas at merali dot org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-nvimagesrc
 * @title: nvimagesrc
 *
 * This element captures your X Display and creates raw RGB video.  It uses
 * the XDamage extension if available to only capture areas of the screen that
 * have changed since the last frame.  It uses the XFixes extension if
 * available to also capture your mouse pointer.  By default it will fixate to
 * 25 frames per second.
 *
 * ## Example pipelines
 * |[
 * gst-launch-1.0 nvimagesrc ! video/x-raw,framerate=5/1 ! videoconvert ! theoraenc ! oggmux ! filesink location=desktop.ogg
 * ]| Encodes your X display to an Ogg theora video at 5 frames per second.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstnvimagesrc.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <gst/gst.h>
#include <gst/gst-i18n-plugin.h>
#include <gst/video/video.h>

#include "gst/glib-compat-private.h"

GST_DEBUG_CATEGORY_STATIC (gst_debug_nvimage_src);
#define GST_CAT_DEFAULT gst_debug_nvimage_src

static GstStaticPadTemplate t =
GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265, "
        "framerate = (fraction) [ 0, MAX ], "
        "width = (int) [ 145, 4096 ], " "height = (int) [ 49, 4095 ], "
	"stream-format = (string) byte-stream, "
	"alignment = (string) au, "
	"profile = (string) { main, high, high-4:4:4, baseline }"));

enum
{
        PROP_0,
        PROP_DISPLAY_NAME,
        PROP_SHOW_POINTER,
        PROP_BITRATE,
        PROP_FPS,
};

#define gst_nvimage_src_parent_class parent_class
G_DEFINE_TYPE (GstNVimageSrcHEVC, gst_nvimage_src, GST_TYPE_PUSH_SRC);

static GstCaps *gst_nvimage_src_fixate (GstBaseSrc * bsrc, GstCaps * caps);

static gboolean
gst_nvimage_src_open_display (GstNVimageSrcHEVC * s, const gchar * name)
{
        g_return_val_if_fail (GST_IS_NVIMAGE_SRC (s), FALSE);

        if (s->xcontext != NULL)
                return TRUE;

        s->xcontext = nvimageutil_xcontext_get_r (GST_ELEMENT (s), name);
        if (s->xcontext == NULL) {
                GST_ELEMENT_ERROR (s, RESOURCE, OPEN_READ,
                                   ("Could not open X display for reading"),
                                   ("NULL returned from getting xcontext"));
                return FALSE;
        }
        s->width = s->xcontext->width;
        s->height = s->xcontext->height;

        if (s->xcontext == NULL)
                return FALSE;

        return TRUE;
}

static gboolean
gst_nvimage_src_start (GstBaseSrc * basesrc)
{
        GstNVimageSrcHEVC *s = GST_NVIMAGE_SRC (basesrc);

        s->last_frame_no = -1;
        s->frame = 0;
        return gst_nvimage_src_open_display (s, s->display_name);
}

static gboolean
gst_nvimage_src_stop (GstBaseSrc * basesrc)
{
        GstNVimageSrcHEVC *src = GST_NVIMAGE_SRC (basesrc);

        src->frame = 0;
        nvimageutil_xcontext_clear_r (src->xcontext);
        src->xcontext = NULL;
        return TRUE;
}

static gboolean
gst_nvimage_src_unlock (GstBaseSrc * basesrc)
{
        GstNVimageSrcHEVC *src = GST_NVIMAGE_SRC (basesrc);

        /* Awaken the create() func if it's waiting on the clock */
        GST_OBJECT_LOCK (src);
        if (src->clock_id) {
                GST_DEBUG_OBJECT (src, "Waking up waiting clock");
                gst_clock_id_unschedule (src->clock_id);
        }
        GST_OBJECT_UNLOCK (src);

        return TRUE;
}

static GstFlowReturn
gst_nvimage_src_create (GstPushSrc * bs, GstBuffer ** buf)
{
        GstNVimageSrcHEVC *s = GST_NVIMAGE_SRC (bs);
        GstBuffer *image;
        GstClockTime base_time;
        GstClockTime next_capture_ts, pts;
        GstClockTime dur;
        gint64 next_frame_no;

        if (s->fps_n <= 0 || s->fps_d <= 0)
                return GST_FLOW_NOT_NEGOTIATED;     /* FPS must be > 0 */

        if (s->frame == 0) {
                sleep(5);
        }

        /* Now, we might need to wait for the next multiple of the fps
         * before capturing */

        GST_OBJECT_LOCK (s);
        if (GST_ELEMENT_CLOCK (s) == NULL) {
                GST_OBJECT_UNLOCK (s);
                GST_ELEMENT_ERROR (s, RESOURCE, FAILED,
                                        (_("Cannot operate without a clock")), (NULL));
                return GST_FLOW_ERROR;
        }

        base_time = GST_ELEMENT_CAST (s)->base_time;
        pts = next_capture_ts = gst_clock_get_time (GST_ELEMENT_CLOCK (s));
        next_capture_ts -= base_time;

        /* Figure out which 'frame number' position we're at, based on the cur time
         * and frame rate */
        next_frame_no = gst_util_uint64_scale (next_capture_ts, s->fps_n, GST_SECOND * s->fps_d);
        if (next_frame_no == s->last_frame_no) {
                GstClockID id;
                GstClockReturn ret;

                /* Need to wait for the next frame */
                next_frame_no += 1;

                /* Figure out what the next frame time is */
                next_capture_ts = gst_util_uint64_scale (next_frame_no,
                        s->fps_d * GST_SECOND, s->fps_n);

                id = gst_clock_new_single_shot_id (GST_ELEMENT_CLOCK (s), next_capture_ts + base_time);
                s->clock_id = id;

                /* release the object lock while waiting */
                GST_OBJECT_UNLOCK (s);

                GST_DEBUG_OBJECT (s, "Waiting for next frame time %" G_GUINT64_FORMAT, next_capture_ts);
                ret = gst_clock_id_wait (id, NULL);
                GST_OBJECT_LOCK (s);

                gst_clock_id_unref (id);
                s->clock_id = NULL;
                if (ret == GST_CLOCK_UNSCHEDULED) {
                        /* Got woken up by the unlock function */
                        GST_OBJECT_UNLOCK (s);
                        return GST_FLOW_FLUSHING;
                }
                /* Duration is a complete 1/fps frame duration */
                dur = gst_util_uint64_scale_int (GST_SECOND, s->fps_d, s->fps_n);
        } else {
                GstClockTime next_frame_ts;

                GST_DEBUG_OBJECT (s, "No need to wait for next frame time %"
                        G_GUINT64_FORMAT " next frame = %" G_GINT64_FORMAT " prev = %"
                        G_GINT64_FORMAT, next_capture_ts, next_frame_no, s->last_frame_no);
                next_frame_ts = gst_util_uint64_scale (next_frame_no + 1, s->fps_d * GST_SECOND, s->fps_n);
                /* Frame duration is from now until the next expected capture time */
                dur = next_frame_ts - next_capture_ts;
        }
        //dur = gst_util_uint64_scale_int (GST_SECOND, s->fps_d, s->fps_n);
        s->last_frame_no = next_frame_no;
        GST_OBJECT_UNLOCK (s);

        image = gst_nvimageutil_nvimage_new_r(s->xcontext, GST_ELEMENT(s), 
                                            s->fps_n, s->fps_d, s->bitrate, s->show_pointer, s->keyframe, 
                                            next_frame_no, next_capture_ts);

        if(s->keyframe) {
                s->keyframe = 0;
        }

        if (!image)
                return GST_FLOW_ERROR;

        *buf = image;
        GST_BUFFER_DTS (*buf) = GST_CLOCK_TIME_NONE; //pts+s->last_frame_no;
        GST_BUFFER_PTS (*buf) = next_capture_ts; //pts+s->last_frame_no; // next_capture_ts;
        GST_BUFFER_DURATION (*buf) = dur;

        GST_DEBUG_OBJECT (s, "Sending frame time %"
                        GST_TIME_FORMAT " duration %ld next frame = %" G_GINT64_FORMAT " prev = %"
                        G_GINT64_FORMAT, GST_TIME_ARGS(pts+s->last_frame_no), dur, next_frame_no, s->last_frame_no);

        s->frame++;

        return GST_FLOW_OK;
}

static void
gst_nvimage_src_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
        GstNVimageSrcHEVC *src = GST_NVIMAGE_SRC (object);
        gdouble fps;

        switch (prop_id) {
                case PROP_DISPLAY_NAME:
                        g_free (src->display_name);
                        src->display_name = g_strdup (g_value_get_string (value));
                        break;
                case PROP_SHOW_POINTER:
                        src->show_pointer = g_value_get_boolean (value);
                        break;
                case PROP_BITRATE:
                        src->bitrate = g_value_get_uint (value);
                        break;
                case PROP_FPS:
                        fps = g_value_get_double(value);
                        if (fps == (guint)fps) {
                                src->fps_n = (guint)fps;
                                src->fps_d = 1;
                        } else {
                                src->fps_n = fps*1000;
                                src->fps_d = 1000;
                        }
                        break;
                default:
                        g_warning("Unknown property %d", prop_id);
                        break;
        }
}

static void
gst_nvimage_src_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
        GstNVimageSrcHEVC *src = GST_NVIMAGE_SRC (object);

        switch (prop_id) {
                case PROP_DISPLAY_NAME:
                        if (src->xcontext)
                                g_value_set_string (value, DisplayString (src->xcontext->disp));
                        else
                                g_value_set_string (value, src->display_name);

                        break;
                case PROP_SHOW_POINTER:
                        g_value_set_boolean (value, src->show_pointer);
                        break;
                case PROP_BITRATE:
                        g_value_set_uint (value, src->bitrate);
                        break;
                case PROP_FPS:
                        g_value_set_double(value, ((double)src->fps_n) / src->fps_d);
                        break;
                default:
                        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                        break;
        }
}

static void
gst_nvimage_src_dispose (GObject * object)
{
        G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_nvimage_src_finalize (GObject * object)
{
        GstNVimageSrcHEVC *src = GST_NVIMAGE_SRC (object);

        if (src->xcontext)
                nvimageutil_xcontext_clear_r (src->xcontext);

        G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstCaps *
gst_nvimage_src_get_caps (GstBaseSrc * bs, GstCaps * filter)
{
        GstNVimageSrcHEVC *s = GST_NVIMAGE_SRC (bs);
        gint width, height;

        if ((!s->xcontext) || (!gst_nvimage_src_open_display (s, s->display_name)))
                return gst_pad_get_pad_template_caps (GST_BASE_SRC (s)->srcpad);

        width = s->xcontext->width;
        height = s->xcontext->height;

        GST_DEBUG ("width = %d, height=%d", width, height);

        return gst_caps_new_simple ("video/x-h265",
                "width", G_TYPE_INT, width,
                "height", G_TYPE_INT, height,
                "framerate", GST_TYPE_FRACTION_RANGE, 1, G_MAXINT, G_MAXINT, 1,
                "stream-format", G_TYPE_STRING, "byte-stream",
                "alignment", G_TYPE_STRING, "au",
                "profile", G_TYPE_STRING, "high",
                NULL);
}

static gboolean
gst_nvimage_src_set_caps (GstBaseSrc * bs, GstCaps * caps)
{
        GstNVimageSrcHEVC *s = GST_NVIMAGE_SRC (bs);
        GstStructure *structure;
        const GValue *new_fps;

        /* If not yet opened, disallow setcaps until later */
        if (!s->xcontext)
                return FALSE;

        /* The only thing that can change is the framerate downstream wants */
        structure = gst_caps_get_structure (caps, 0);
        new_fps = gst_structure_get_value (structure, "framerate");
        if (!new_fps)
                return FALSE;

        /* Store this FPS for use when generating buffers */
        s->fps_n = gst_value_get_fraction_numerator (new_fps);
        s->fps_d = gst_value_get_fraction_denominator (new_fps);

        GST_DEBUG_OBJECT (s, "peer wants %d/%d fps", s->fps_n, s->fps_d);

        return TRUE;
}

static GstCaps *
gst_nvimage_src_fixate (GstBaseSrc * bsrc, GstCaps * caps)
{
        gint i;
        GstStructure *structure;

        caps = gst_caps_make_writable (caps);

        for (i = 0; i < gst_caps_get_size (caps); ++i) {
                structure = gst_caps_get_structure (caps, i);

                gst_structure_fixate_field_nearest_fraction (structure, "framerate", 25, 1);
        }
        caps = GST_BASE_SRC_CLASS (parent_class)->fixate (bsrc, caps);

        return caps;
}

static gboolean
gst_nvimage_src_event (GstBaseSrc * bsrc, GstEvent * event) {
        if(event) {
                const GstStructure *s = gst_event_get_structure(event);
                GstNVimageSrcHEVC *nvs = GST_NVIMAGE_SRC (bsrc);
                if(s && s->name) {
                        const char *p = g_quark_to_string(s->name);
                        if(p) {
                                g_warning("got event: %d, subtype: %s", event->type, g_quark_to_string(s->name));

                                if (event->type == GST_EVENT_CUSTOM_UPSTREAM) {
                                        if (gst_structure_has_name (s, "GstForceKeyUnit") && nvs) {
                                                nvs->keyframe = 1;
                                        }
                                }
                        }
                }
        }
        return TRUE;
}

static void
gst_nvimage_src_class_init (GstNVimageSrcHEVCClass * klass)
{
        GObjectClass *gc = G_OBJECT_CLASS (klass);
        GstElementClass *ec = GST_ELEMENT_CLASS (klass);
        GstBaseSrcClass *bc = GST_BASE_SRC_CLASS (klass);
        GstPushSrcClass *push_class = GST_PUSH_SRC_CLASS (klass);

        gc->set_property = gst_nvimage_src_set_property;
        gc->get_property = gst_nvimage_src_get_property;
        gc->dispose = gst_nvimage_src_dispose;
        gc->finalize = gst_nvimage_src_finalize;

        g_object_class_install_property (gc, PROP_DISPLAY_NAME,
                                                g_param_spec_string ("display-name", "Display", "X Display Name",
                                                NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

        g_object_class_install_property (gc, PROP_SHOW_POINTER,
                                                g_param_spec_boolean ("show-pointer", "Show Mouse Pointer",
                                                "Show mouse pointer (if XFixes extension enabled)", TRUE,
                                                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

        g_object_class_install_property (gc, PROP_BITRATE,
                                                g_param_spec_uint ("bitrate", "videobitrate", "Desired video bitrate",
                                                0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

        g_object_class_install_property (gc, PROP_FPS,
                                                g_param_spec_double ("fps", "fps", "Desired grabbing fps",
                                                0, 1000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

        gst_element_class_set_static_metadata (ec, "NVimage video source",
                                              "Source/Video",
                                              "Creates a screenshot video stream to h265",
                                              "Lukas Hejtmanek <xhejtman@gmail.com>");
        gst_element_class_add_static_pad_template (ec, &t);

        bc->fixate = gst_nvimage_src_fixate;
        bc->get_caps = gst_nvimage_src_get_caps;
        bc->set_caps = gst_nvimage_src_set_caps;
        bc->start = gst_nvimage_src_start;
        bc->stop = gst_nvimage_src_stop;
        bc->unlock = gst_nvimage_src_unlock;
        bc->event = gst_nvimage_src_event;
        push_class->create = gst_nvimage_src_create;
}

static void
gst_nvimage_src_init (GstNVimageSrcHEVC * nvimagesrc)
{
        gst_base_src_set_format (GST_BASE_SRC (nvimagesrc), GST_FORMAT_TIME);
        gst_base_src_set_live (GST_BASE_SRC (nvimagesrc), TRUE);

        nvimagesrc->show_pointer = TRUE;
        nvimagesrc->bitrate = 2000000;
        nvimagesrc->keyframe = TRUE;
        nvimagesrc->frame = 0;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
        gboolean ret;

        GST_DEBUG_CATEGORY_INIT (gst_debug_nvimage_src, "nvimagesrchevc", 0,
                                        "nvimagesrchevc element debug");

        ret = gst_element_register (plugin, "nvimagesrchevc", GST_RANK_NONE, GST_TYPE_NVIMAGE_SRC);

        return ret;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nvimagesrchevc,
    "X11 video input plugin using standard NVIDIA FBC calls",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
