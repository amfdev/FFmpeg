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

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
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

#include "AMF/components/VideoConverter.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#if CONFIG_DXVA2
#include <d3d9.h>
#endif

#if CONFIG_D3D11VA
#include <d3d11.h>
#endif

#define AMFAV_RETURN_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
    if (!(exp)) { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        return ret_value; \
    }


typedef struct FormatMap {
    enum AVPixelFormat       av_format;
    enum AMF_SURFACE_FORMAT  amf_format;
} FormatMap;

static const FormatMap format_map[] =
{
    { AV_PIX_FMT_NONE,       AMF_SURFACE_UNKNOWN },
    { AV_PIX_FMT_NV12,       AMF_SURFACE_NV12 },
    { AV_PIX_FMT_BGR0,       AMF_SURFACE_BGRA },
    { AV_PIX_FMT_RGB0,       AMF_SURFACE_RGBA },
    { AV_PIX_FMT_GRAY8,      AMF_SURFACE_GRAY8 },
    { AV_PIX_FMT_YUV420P,    AMF_SURFACE_YUV420P },
    { AV_PIX_FMT_YUYV422,    AMF_SURFACE_YUY2 },
};

static enum AMF_SURFACE_FORMAT amf_av_to_amf_format(enum AVPixelFormat fmt)
{
    int i;
    for (i = 0; i < amf_countof(format_map); i++) {
        if (format_map[i].av_format == fmt) {
            return format_map[i].amf_format;
        }
    }
    return AMF_SURFACE_UNKNOWN;
}

typedef struct AMFScaleContext AMFScaleContext;

typedef struct AMFDataAllocatorImpl {
    const AMFDataAllocatorCBVtbl    *vtbl;
    AMFScaleContext                 *ctx;
    AMFSurface                      *surface;
} AMFDataAllocatorImpl;

static amf_long            AMF_STD_CALL amf_allocator_acquire(AMFDataAllocatorCB* pThis)
{
    return 0;
}
static amf_long            AMF_STD_CALL amf_allocator_release(AMFDataAllocatorCB* pThis)
{
    return 0;
}
static enum AMF_RESULT     AMF_STD_CALL amf_allocator_query_interface(AMFDataAllocatorCB* pThis, const struct AMFGuid *interfaceID, void** ppInterface)
{
    return AMF_NO_INTERFACE;
}
static AMF_RESULT AMF_STD_CALL amf_allocator_alloc_buffer(AMFDataAllocatorCB* pThis, AMF_MEMORY_TYPE type, amf_size size, AMFBuffer** ppBuffer)
{
    return AMF_NOT_SUPPORTED;
}

static AMF_RESULT AMF_STD_CALL amf_allocator_alloc_surface(AMFDataAllocatorCB* pThis, AMF_MEMORY_TYPE type, AMF_SURFACE_FORMAT format,
    amf_int32 width, amf_int32 height, amf_int32 hPitch, amf_int32 vPitch, AMFSurface** ppSurface)
{
    AMFDataAllocatorImpl* this = (AMFDataAllocatorImpl*)pThis;
    *ppSurface = this->surface;
    (*ppSurface)->pVtbl->Acquire(*ppSurface);
    return AMF_OK;
}


static const AMFDataAllocatorCBVtbl dataAllocatorCBVtbl =
{
    .Acquire=amf_allocator_acquire,
    .Release=amf_allocator_release,
    .QueryInterface=amf_allocator_query_interface,
    .AllocBuffer=amf_allocator_alloc_buffer,
    .AllocSurface=amf_allocator_alloc_surface,
};

typedef struct AMFScaleContext {
    const AVClass *class;


    /**
     * New dimensions. Special values are:
     *   0 = original width/height
     *  -1 = keep original aspect
     */
    int width, height;

    /**
     * Output sw format. AV_PIX_FMT_NONE for no conversion.
     */
    enum AVPixelFormat format;

    char *w_expr;               ///< width  expression string
    char *h_expr;               ///< height expression string
    char *format_str;

    AMFComponent         *converter; ///< AMF encoder object
    AMFDataAllocatorImpl  allocator;

    AVBufferRef        *amf_device_ctx;

    AVBufferRef       *hwframes_in_ref;//do we need to keep it?

    AVBufferRef       *hwdevice_ref;
    //AVHWDeviceContext *hwdevice;


    AVBufferRef       *hwframes_out_ref;
    AVHWFramesContext *hwframes;

    AMFContext         *context;
    AMFFactory         *factory;

} AMFScaleContext;

static int amf_scale_init(AVFilterContext *avctx)
{
    AMFScaleContext     *ctx = avctx->priv;
    
    ctx->allocator.vtbl = &dataAllocatorCBVtbl;
    ctx->allocator.ctx = ctx;

    if (!strcmp(ctx->format_str, "same")) {
        ctx->format = AV_PIX_FMT_NONE;
    } else {
        ctx->format = av_get_pix_fmt(ctx->format_str);
        if (ctx->format == AV_PIX_FMT_NONE) {
            av_log(avctx, AV_LOG_ERROR, "Unrecognized pixel format: %s\n", ctx->format_str);
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

static int amf_copy_surface(AVFilterContext *avctx, const AVFrame *frame,
    AMFSurface* surface)
{
    AMFPlane *plane;
    uint8_t  *dst_data[4];
    int       dst_linesize[4];
    int       planes;
    int       i;

    planes = surface->pVtbl->GetPlanesCount(surface);
    av_assert0(planes < FF_ARRAY_ELEMS(dst_data));

    for (i = 0; i < planes; i++) {
        plane = surface->pVtbl->GetPlaneAt(surface, i);
        dst_data[i] = plane->pVtbl->GetNative(plane);
        dst_linesize[i] = plane->pVtbl->GetHPitch(plane);
    }
    av_image_copy(dst_data, dst_linesize,
        (const uint8_t**)frame->data, frame->linesize, frame->format,
        frame->width, frame->height);

    return 0;
}


static int amf_scale_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink   *inlink = avctx->inputs[0];
    AMFScaleContext  *ctx = avctx->priv;
    AVAMFDeviceContext *amf_ctx;
    enum AVPixelFormat pix_fmt_in;

    int err;
    AMF_RESULT res;

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

        ctx->hwframes_in_ref = av_buffer_ref(inlink->hw_frames_ctx);
        if (!ctx->hwframes_in_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes_out_ref = av_hwframe_ctx_alloc(frames_ctx->device_ref);
        if (!ctx->hwframes_out_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes = (AVHWFramesContext*)ctx->hwframes_out_ref->data;
        ctx->hwframes->format    = outlink->format;
        ctx->hwframes->sw_format = frames_ctx->sw_format;
        pix_fmt_in = frames_ctx->sw_format;

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
        pix_fmt_in = inlink->format;

    } else {
        err = av_hwdevice_ctx_create(&ctx->amf_device_ctx, AV_HWDEVICE_TYPE_AMF, NULL, NULL, 0);
        if (err < 0)
            return err;
        pix_fmt_in = inlink->format;
    }

    outlink->w = inlink->w*2;
    outlink->h = inlink->h*2;

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
    amf_ctx = ((AVHWDeviceContext*)ctx->amf_device_ctx->data)->hwctx;
    ctx->context = amf_ctx->context;
    ctx->factory = amf_ctx->factory;


    res = ctx->factory->pVtbl->CreateComponent(ctx->factory, ctx->context, AMFVideoConverter, &ctx->converter);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", AMFVideoConverter, res);

    ctx->converter->pVtbl->SetOutputDataAllocatorCB(ctx->converter, (AMFDataAllocatorCB*)&ctx->allocator);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "SetOutputDataAllocatorCB() failed with error %d\n", res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->converter, AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, (amf_int32)amf_av_to_amf_format(ctx->hwframes->sw_format));
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->converter, AMF_VIDEO_CONVERTER_MEMORY_TYPE, (amf_int32)AMF_MEMORY_DX11);//FIX ME
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);

    AMFSize out_sz = { outlink->w, outlink->h };
    AMF_ASSIGN_PROPERTY_SIZE(res, ctx->converter, AMF_VIDEO_CONVERTER_OUTPUT_SIZE, out_sz);

    res = ctx->converter->pVtbl->Init(ctx->converter, amf_av_to_amf_format(pix_fmt_in), inlink->w, inlink->h);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-Init() failed with error %d\n", res);


    AMFVariantStruct vs;
    res = ctx->converter->pVtbl->GetProperty(ctx->converter, AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, &vs);

    return 0;

fail:
    //av_log(NULL, AV_LOG_ERROR,
    //       "Error when evaluating the expression '%s'\n", expr);
    return err;
}
static int amf_avframe_to_amfsurface(AVFilterContext *avctx, const AVFrame *frame, AMFSurface** ppSurface)
{
    AMFScaleContext *ctx = avctx->priv;
    AMFSurface *surface;
    AMF_RESULT  res;
    int hw_surface = 0;

    switch (frame->format) {
#if CONFIG_D3D11VA
    case AV_PIX_FMT_D3D11:
        {
            static const GUID AMFTextureArrayIndexGUID = { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };
            ID3D11Texture2D *texture = (ID3D11Texture2D*)frame->data[0]; // actual texture
            int index = (intptr_t)frame->data[1]; // index is a slice in texture array is - set to tell AMF which slice to use

            //av_assert0(frame->hw_frames_ctx       && ctx->hwframes_in_ref &&
            //            frame->hw_frames_ctx->data == ctx->hwframes_in_ref->data);

            texture->lpVtbl->SetPrivateData(texture, &AMFTextureArrayIndexGUID, sizeof(index), &index);

            res = ctx->context->pVtbl->CreateSurfaceFromDX11Native(ctx->context, texture, &surface, NULL); // wrap to AMF surface
            AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX11Native() failed  with error %d\n", res);
            hw_surface = 1;
        }
        break;
#endif
#if CONFIG_DXVA2
    case AV_PIX_FMT_DXVA2_VLD:
        {
            IDirect3DSurface9 *texture = (IDirect3DSurface9 *)frame->data[3]; // actual texture

            res = ctx->context->pVtbl->CreateSurfaceFromDX9Native(ctx->context, texture, &surface, NULL); // wrap to AMF surface
            AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX9Native() failed  with error %d\n", res);

            hw_surface = 1;
        }
        break;
#endif
    default:
        {
            res = ctx->context->pVtbl->AllocSurface(ctx->context, AMF_MEMORY_HOST, ctx->format, ctx->width, ctx->height, &surface);
            AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AllocSurface() failed  with error %d\n", res);
            amf_copy_surface(avctx, frame, surface);
        }
        break;
    }

    if (hw_surface) {
        // input HW surfaces can be vertically aligned by 16; tell AMF the real size
        surface->pVtbl->SetCrop(surface, 0, 0, frame->width, frame->height);
    }

    surface->pVtbl->SetPts(surface, frame->pts);
    *ppSurface = surface;
    return 0;
}

static int amf_scale_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext             *avctx = link->dst;
    AMFScaleContext             *ctx = avctx->priv;
    AVFilterLink                *outlink = avctx->outputs[0];
    AMF_RESULT  res;
    AMFSurface *surface_in;
    AMFSurface *surface_out;
    AMFData *data_out;

    AVFrame *out = NULL;
    int ret = 0;

    if (!ctx->converter)
        return AVERROR(EINVAL);

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = amf_avframe_to_amfsurface(avctx, in, &surface_in);
    if (ret < 0)
        goto fail;
    ret = amf_avframe_to_amfsurface(avctx, out, &ctx->allocator.surface);
    if (ret < 0)
        goto fail;

//kjhjkjgkjgjhkgjhhjgjkhgjkhgyuf

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        goto fail;

    out->width  = outlink->w;
    out->height = outlink->h;

    AMF_MEMORY_TYPE mti = surface_in->pVtbl->GetMemoryType(surface_in);
    AMF_SURFACE_FORMAT sfi = surface_in->pVtbl->GetFormat(surface_in);

    AMF_MEMORY_TYPE mto= ctx->allocator.surface->pVtbl->GetMemoryType(ctx->allocator.surface);
    AMF_SURFACE_FORMAT sfo = ctx->allocator.surface->pVtbl->GetFormat(ctx->allocator.surface);

    res = ctx->converter->pVtbl->SubmitInput(ctx->converter, (AMFData*)surface_in);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "SubmitInput() failed with error %d\n", res);

    res = ctx->converter->pVtbl->QueryOutput(ctx->converter, &data_out);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "SubmitInput() failed with error %d\n", res);

    if (data_out) {
        // copy data to packet
        AMFGuid guid = IID_AMFSurface();
        data_out->pVtbl->QueryInterface(data_out, &guid, (void**)&surface_out); // query for buffer interface
    }

    AMF_MEMORY_TYPE mtro= surface_out->pVtbl->GetMemoryType(surface_out);
    AMF_SURFACE_FORMAT sfro = surface_out->pVtbl->GetFormat(surface_out);

    ctx->allocator.surface->pVtbl->Release(ctx->allocator.surface);
    data_out->pVtbl->Release(data_out);
    surface_in->pVtbl->Release(surface_in);
    surface_out->pVtbl->Release(surface_out);

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
