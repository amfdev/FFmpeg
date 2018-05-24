/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * scale video filter - AMF
 */

#include <stdio.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_amf.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "libavutil/hwcontext_amf.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"


typedef struct AMFScaleContext {
    const AVClass *class;


    /**
     * New dimensions. Special values are:
     *   0 = original width/height
     *  -1 = keep original aspect
     */
    int w, h;

    /**
     * Output sw format. AV_PIX_FMT_NONE for no conversion.
     */
    enum AVPixelFormat format;

    char *w_expr;               ///< width  expression string
    char *h_expr;               ///< height expression string
    char *format_str;

    AVBufferRef        *amf_device_ctx;

    AVBufferRef       *hwframes_in_ref;//do we need to keep it?

    AVBufferRef       *hwdevice_ref;
    //AVHWDeviceContext *hwdevice;


    AVBufferRef       *hwframes_out_ref;
    AVHWFramesContext *hwframes;

    AMFContext         *context;
    AMFFactory         *factory;

} AMFScaleContext;

static int amf_scale_init(AVFilterContext *ctx)
{
    AMFScaleContext     *s = ctx->priv;

    if (!strcmp(s->format_str, "same")) {
        s->format = AV_PIX_FMT_NONE;
    } else {
        s->format = av_get_pix_fmt(s->format_str);
        if (s->format == AV_PIX_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Unrecognized pixel format: %s\n", s->format_str);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static void amf_scale_uninit(AVFilterContext *avctx)
{
    AMFScaleContext *ctx = avctx->priv;

//    av_buffer_unref(&ctx->amf_device_ctx);
    av_buffer_unref(&ctx->hwdevice_ref);
    av_buffer_unref(&ctx->hwframes_in_ref);
    av_buffer_unref(&ctx->hwframes_out_ref);
}

static int amf_scale_query_formats(AVFilterContext *avctx)
{
    static const enum AVPixelFormat pixel_formats_in[] = {
        AV_PIX_FMT_DXVA2_VLD,
        AV_PIX_FMT_D3D11, 
        AV_PIX_FMT_NV12, 
        AV_PIX_FMT_0RGB, 
        AV_PIX_FMT_BGR0, 
        AV_PIX_FMT_RGB0, 
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_NONE,
    };
    static const enum AVPixelFormat pixel_formats_out[] = {
        AV_PIX_FMT_DXVA2_VLD, 
        AV_PIX_FMT_D3D11, 
        AV_PIX_FMT_NONE,
    };
    AVFilterFormats *pix_fmts_in  = ff_make_format_list(pixel_formats_in);
    AVFilterFormats *pix_fmts_out  = ff_make_format_list(pixel_formats_out);
    int err;


    if ((err = ff_formats_ref(pix_fmts_in,  &avctx->inputs[0]->out_formats)) < 0 ||
        (err = ff_formats_ref(pix_fmts_out, &avctx->outputs[0]->in_formats)) < 0)
        return err;


    return 0;
}

static int amf_scale_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink   *inlink = avctx->inputs[0];
    AMFScaleContext  *ctx = avctx->priv;
    AVAMFDeviceContext *amf_ctx;
    int err;

    av_buffer_unref(&ctx->hwframes_in_ref);
    av_buffer_unref(&ctx->hwframes_out_ref);

    if (inlink->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)inlink->hw_frames_ctx->data;

/*        if (amf_av_to_amf_format(frames_ctx->sw_format) == AMF_SURFACE_UNKNOWN) {
            av_log(avctx, AV_LOG_ERROR, "Format of input frames context (%s) is not supported by AMF.\n",
                   av_get_pix_fmt_name(frames_ctx->sw_format));
            return AVERROR(EINVAL);
        }*/

        err = av_hwdevice_ctx_create_derived(&ctx->amf_device_ctx, AV_HWDEVICE_TYPE_AMF, frames_ctx->device_ref, 0);
        if (err < 0)
            return err;
        av_buffer_unref(&ctx->amf_device_ctx);

        ctx->hwframes_in_ref = av_buffer_ref(inlink->hw_frames_ctx);
        if (!ctx->hwframes_in_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes_out_ref = av_hwframe_ctx_alloc(frames_ctx->device_ref);
        if (!ctx->hwframes_out_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes = (AVHWFramesContext*)ctx->hwframes_out_ref->data;
        ctx->hwframes->format    = outlink->format;
        ctx->hwframes->sw_format = frames_ctx->sw_format;

    } else if (avctx->hw_device_ctx) {
        err = av_hwdevice_ctx_create_derived(&ctx->amf_device_ctx, AV_HWDEVICE_TYPE_AMF, avctx->hw_device_ctx, 0);
        if (err < 0)
            return err;

        ctx->hwdevice_ref = av_buffer_ref(avctx->hw_device_ctx);
        if (!ctx->hwdevice_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes_out_ref = av_hwframe_ctx_alloc(ctx->hwdevice_ref);
        if (!ctx->hwframes_out_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes = (AVHWFramesContext*)ctx->hwframes_out_ref->data;
        ctx->hwframes->format    = outlink->format;
        ctx->hwframes->sw_format = inlink->format;

    } else {
        err = av_hwdevice_ctx_create(&ctx->amf_device_ctx, AV_HWDEVICE_TYPE_AMF, NULL, NULL, 0);
        if (err < 0)
            return err;
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;

    ctx->hwframes->width     = outlink->w;
    ctx->hwframes->height    = outlink->h;

    //if (avctx->extra_hw_frames >= 0)
    //    ctx->hwframes->initial_pool_size = 2 + avctx->extra_hw_frames;

    err = av_hwframe_ctx_init(ctx->hwframes_out_ref);
    if (err < 0)
        goto fail;

    outlink->hw_frames_ctx = av_buffer_ref(ctx->hwframes_out_ref);
    if (!outlink->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

//AMF
//    amf_ctx = ((AVHWDeviceContext*)ctx->amf_device_ctx->data)->hwctx;
//    ctx->context = amf_ctx->context;
//    ctx->factory = amf_ctx->factory;

    return 0;

fail:
    //av_log(NULL, AV_LOG_ERROR,
    //       "Error when evaluating the expression '%s'\n", expr);
    return err;
}

static int amf_scale_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext             *ctx = link->dst;
    AMFScaleContext               *s = ctx->priv;
    AVFilterLink            *outlink = ctx->outputs[0];

    AVFrame *out = NULL;
    int ret = 0;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

//kjhjkjgkjgjhkgjhhjgjkhgjkhgyuf

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        goto fail;

    out->width  = outlink->w;
    out->height = outlink->h;

    av_reduce(&out->sample_aspect_ratio.num, &out->sample_aspect_ratio.den,
              (int64_t)in->sample_aspect_ratio.num * outlink->h * link->w,
              (int64_t)in->sample_aspect_ratio.den * outlink->w * link->h,
              INT_MAX);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

#define OFFSET(x) offsetof(AMFScaleContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption options[] = {
    { "w",      "Output video width",  OFFSET(w_expr),     AV_OPT_TYPE_STRING, { .str = "iw"   }, .flags = FLAGS },
    { "h",      "Output video height", OFFSET(h_expr),     AV_OPT_TYPE_STRING, { .str = "ih"   }, .flags = FLAGS },
    { "format", "Output pixel format", OFFSET(format_str), AV_OPT_TYPE_STRING, { .str = "same" }, .flags = FLAGS },

    { NULL },
};

static const AVClass amf_scale_class = {
    .class_name = "amf_scale",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad amf_scale_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = amf_scale_filter_frame,
    },
    { NULL }
};

static const AVFilterPad amf_scale_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = amf_scale_config_output,
    },
    { NULL }
};

AVFilter ff_vf_scale_amf = {
    .name      = "scale_amf",
    .description = NULL_IF_CONFIG_SMALL("AMF video scaling and format conversion"),

    .init          = amf_scale_init,
    .uninit        = amf_scale_uninit,
    .query_formats = amf_scale_query_formats,

    .priv_size = sizeof(AMFScaleContext),
    .priv_class = &amf_scale_class,

    .inputs    = amf_scale_inputs,
    .outputs   = amf_scale_outputs,

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
