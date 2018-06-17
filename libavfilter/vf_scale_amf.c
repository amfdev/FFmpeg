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
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_amf.h"

#include "AMF/components/VideoConverter.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "scale.h"

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

typedef struct AMFScaleContext {
    const AVClass *class;

    int width, height;
    enum AVPixelFormat format;

    char *w_expr;
    char *h_expr;
    char *format_str;

    int rect_left;
    int rect_right;
    int rect_top;
    int rect_bottom;
    
    int scale_type;
    int color_profile;
    
    int keep_aspect_ratio;
    int fill;
    uint8_t fill_color[4];

    AMFComponent        *converter;
    AVBufferRef         *amf_device_ref;

    AVBufferRef         *hwframes_in_ref;
    AVBufferRef         *hwframes_out_ref;
    AVBufferRef         *hwdevice_ref;

    AMFContext          *context;
    AMFFactory          *factory;

} AMFScaleContext;


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

static void amf_free_amfsurface(void *opaque, uint8_t *data)
{
    AMFSurface *surface = (AMFSurface*)(opaque);
    surface->pVtbl->Release(surface);
}

static AVFrame *amf_amfsurface_to_avframe(AVFilterContext *avctx, AMFSurface* pSurface)
{
    AVFrame *frame = av_frame_alloc();

    if (!frame)
        return NULL;

    switch (pSurface->pVtbl->GetMemoryType(pSurface))
    {
#if CONFIG_D3D11VA
        case AMF_MEMORY_DX11:
        {
            AMFPlane *plane0 = pSurface->pVtbl->GetPlaneAt(pSurface, 0);
            frame->data[0] = plane0->pVtbl->GetNative(plane0);
            frame->data[1] = 0;

            frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     amf_free_amfsurface,
                                     pSurface,
                                     AV_BUFFER_FLAG_READONLY);
            pSurface->pVtbl->Acquire(pSurface);
        }
        break;
#endif
#if CONFIG_DXVA2
        case AMF_MEMORY_DX9:
        {
            AMFPlane *plane0 = pSurface->pVtbl->GetPlaneAt(pSurface, 0);
            frame->data[3] = plane0->pVtbl->GetNative(plane0);

            frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     amf_free_amfsurface,
                                     pSurface,
                                     AV_BUFFER_FLAG_READONLY);
            pSurface->pVtbl->Acquire(pSurface);
        }
        break;
#endif
    default:
        {
            av_assert0(0);//should not happen
        }
    }

    return frame;
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
            AMF_SURFACE_FORMAT amf_fmt = amf_av_to_amf_format(frame->format);
            res = ctx->context->pVtbl->AllocSurface(ctx->context, AMF_MEMORY_HOST, amf_fmt, frame->width, frame->height, &surface);
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

static int amf_scale_init(AVFilterContext *avctx)
{
    AMFScaleContext     *ctx = avctx->priv;

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

    if (ctx->converter) {
        ctx->converter->pVtbl->Terminate(ctx->converter);
        ctx->converter->pVtbl->Release(ctx->converter);
        ctx->converter = NULL;
    }

    av_buffer_unref(&ctx->amf_device_ref);
    av_buffer_unref(&ctx->hwdevice_ref);
    av_buffer_unref(&ctx->hwframes_in_ref);
    av_buffer_unref(&ctx->hwframes_out_ref);
}

static int amf_scale_query_formats(AVFilterContext *avctx)
{
    AVBufferRef *device_ref = NULL;
    AVHWDeviceContext *device_ctx = NULL;
    AVHWFramesConstraints *constraints = NULL;
    const enum AVPixelFormat *output_pix_fmts;
    AVFilterFormats *input_formats = NULL;
    int err;
    int i;
    static const enum AVPixelFormat input_pix_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_0RGB,
        AV_PIX_FMT_BGR0,
        AV_PIX_FMT_RGB0,
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_NONE,
    };

    if (avctx->inputs[0]->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->inputs[0]->hw_frames_ctx->data;
        device_ref = frames_ctx->device_ref;
    } else if (avctx->hw_device_ctx) {
        device_ref = avctx->hw_device_ctx;
    } else {
        av_log(avctx, AV_LOG_ERROR, "A hardware device reference is required to initialise AMF Scaler.\n");
        return AVERROR(EINVAL);
    }

    device_ctx = (AVHWDeviceContext*)device_ref->data;

    switch (device_ctx->type) {
#if CONFIG_D3D11VA
        case AV_HWDEVICE_TYPE_D3D11VA:
        {
            static const enum AVPixelFormat output_pix_fmts_d3d11[] = {
                AV_PIX_FMT_D3D11,
                AV_PIX_FMT_NONE,
            };
            output_pix_fmts = output_pix_fmts_d3d11;
        }
#endif
#if CONFIG_DXVA2
        case AV_HWDEVICE_TYPE_DXVA2:
        {
            static const enum AVPixelFormat output_pix_fmts_dxva2[] = {
                AV_PIX_FMT_DXVA2_VLD,
                AV_PIX_FMT_NONE,
            };
            output_pix_fmts = output_pix_fmts_dxva2;
        }
        break;
#endif
        default:
        {
            av_log(avctx, AV_LOG_ERROR, "Device %s is not supported by amf.\n", av_hwdevice_get_type_name(device_ctx->type));
            return AVERROR(EINVAL);
        }
        break;
    }

    input_formats = ff_make_format_list(output_pix_fmts);
    if (!input_formats) {
        err = AVERROR(ENOMEM);
        return err;
    }

    for (i = 0; input_pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
        err = ff_add_format(&input_formats, input_pix_fmts[i]);
        if (err < 0)
            return err;
    }

    if ((err = ff_formats_ref(input_formats, &avctx->inputs[0]->out_formats)) < 0 ||
        (err = ff_formats_ref(ff_make_format_list(output_pix_fmts),
                              &avctx->outputs[0]->in_formats)) < 0)
        return err;

    av_hwframe_constraints_free(&constraints);
    return 0;
}

static int amf_scale_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink   *inlink = avctx->inputs[0];
    AMFScaleContext  *ctx = avctx->priv;
    AVAMFDeviceContext *amf_ctx;
    AVHWFramesContext *hwframes_out;
    enum AVPixelFormat pix_fmt_in;
    AMFSize out_size;
    AMFRect out_rect;
    AMFColor fill_color;
    int err;
    AMF_RESULT res;

    if ((err = ff_scale_eval_dimensions(avctx,
                                        ctx->w_expr, ctx->h_expr,
                                        inlink, outlink,
                                        &ctx->width, &ctx->height)) < 0)
        return err;

    av_buffer_unref(&ctx->hwframes_in_ref);
    av_buffer_unref(&ctx->hwframes_out_ref);

    if (inlink->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)inlink->hw_frames_ctx->data;

        if (amf_av_to_amf_format(frames_ctx->sw_format) == AMF_SURFACE_UNKNOWN) {
            av_log(avctx, AV_LOG_ERROR, "Format of input frames context (%s) is not supported by AMF.\n",
                   av_get_pix_fmt_name(frames_ctx->sw_format));
            return AVERROR(EINVAL);
        }

        err = av_hwdevice_ctx_create_derived(&ctx->amf_device_ref, AV_HWDEVICE_TYPE_AMF, frames_ctx->device_ref, 0);
        if (err < 0)
            return err;

        ctx->hwframes_in_ref = av_buffer_ref(inlink->hw_frames_ctx);
        if (!ctx->hwframes_in_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes_out_ref = av_hwframe_ctx_alloc(frames_ctx->device_ref);
        if (!ctx->hwframes_out_ref)
            return AVERROR(ENOMEM);

        hwframes_out = (AVHWFramesContext*)ctx->hwframes_out_ref->data;
        hwframes_out->format    = outlink->format;
        hwframes_out->sw_format = frames_ctx->sw_format;
        pix_fmt_in = frames_ctx->sw_format;

    } else if (avctx->hw_device_ctx) {
        err = av_hwdevice_ctx_create_derived(&ctx->amf_device_ref, AV_HWDEVICE_TYPE_AMF, avctx->hw_device_ctx, 0);
        if (err < 0)
            return err;

        ctx->hwdevice_ref = av_buffer_ref(avctx->hw_device_ctx);
        if (!ctx->hwdevice_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes_out_ref = av_hwframe_ctx_alloc(ctx->hwdevice_ref);
        if (!ctx->hwframes_out_ref)
            return AVERROR(ENOMEM);

        hwframes_out = (AVHWFramesContext*)ctx->hwframes_out_ref->data;
        hwframes_out->format    = outlink->format;
        hwframes_out->sw_format = inlink->format;
        pix_fmt_in = inlink->format;

    } else {
        av_log(ctx, AV_LOG_ERROR, "A hardware device reference to init hwcontext_amf.\n");
        return AVERROR(EINVAL);
    }

    outlink->w = ctx->width;
    outlink->h = ctx->height;

    hwframes_out->width = outlink->w;
    hwframes_out->height = outlink->h;

    err = av_hwframe_ctx_init(ctx->hwframes_out_ref);
    if (err < 0)
        return err;

    outlink->hw_frames_ctx = av_buffer_ref(ctx->hwframes_out_ref);
    if (!outlink->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        return err;
    }

    amf_ctx = ((AVHWDeviceContext*)ctx->amf_device_ref->data)->hwctx;
    ctx->context = amf_ctx->context;
    ctx->factory = amf_ctx->factory;

    res = ctx->factory->pVtbl->CreateComponent(ctx->factory, ctx->context, AMFVideoConverter, &ctx->converter);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", AMFVideoConverter, res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->converter, AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, (amf_int32)amf_av_to_amf_format(hwframes_out->sw_format));
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);

    out_size.width = outlink->w;
    out_size.height = outlink->h;
    AMF_ASSIGN_PROPERTY_SIZE(res, ctx->converter, AMF_VIDEO_CONVERTER_OUTPUT_SIZE, out_size);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);

    out_rect.left = ctx->rect_left;
    out_rect.top = ctx->rect_top;
    out_rect.right = ctx->rect_right;
    out_rect.bottom = ctx->rect_bottom;
    AMF_ASSIGN_PROPERTY_RECT(res, ctx->converter, AMF_VIDEO_CONVERTER_OUTPUT_RECT, out_rect);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);

    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->converter, AMF_VIDEO_CONVERTER_KEEP_ASPECT_RATIO, ctx->keep_aspect_ratio);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);

    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->converter, AMF_VIDEO_CONVERTER_FILL, ctx->fill);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);

    fill_color.r = ctx->fill_color[0];
    fill_color.g = ctx->fill_color[1];
    fill_color.b = ctx->fill_color[2];
    AMF_ASSIGN_PROPERTY_COLOR(res, ctx->converter, AMF_VIDEO_CONVERTER_FILL_COLOR, fill_color);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->converter, AMF_VIDEO_CONVERTER_SCALE, (amf_int32)ctx->scale_type);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);

    if(ctx->color_profile != AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN) {
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->converter, AMF_VIDEO_CONVERTER_COLOR_PROFILE, (amf_int32)ctx->color_profile);
        AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-SetProperty() failed with error %d\n", res);
    }

    res = ctx->converter->pVtbl->Init(ctx->converter, amf_av_to_amf_format(pix_fmt_in), inlink->w, inlink->h);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AMFConverter-Init() failed with error %d\n", res);

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

    ret = amf_avframe_to_amfsurface(avctx, in, &surface_in);
    if (ret < 0)
        goto fail;

    res = ctx->converter->pVtbl->SubmitInput(ctx->converter, (AMFData*)surface_in);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "SubmitInput() failed with error %d\n", res);

    res = ctx->converter->pVtbl->QueryOutput(ctx->converter, &data_out);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "QueryOutput() failed with error %d\n", res);

    if (data_out) {
        AMFGuid guid = IID_AMFSurface();
        data_out->pVtbl->QueryInterface(data_out, &guid, (void**)&surface_out); // query for buffer interface
        data_out->pVtbl->Release(data_out);
    }

    out = amf_amfsurface_to_avframe(avctx, surface_out);

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        goto fail;

    out->format = outlink->format;
    out->width  = outlink->w;
    out->height = outlink->h;

    out->hw_frames_ctx = av_buffer_ref(ctx->hwframes_out_ref);
    if (!out->hw_frames_ctx)
        return AVERROR(ENOMEM);

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
    { "w",      "Output video width",               OFFSET(w_expr),     AV_OPT_TYPE_STRING, { .str = "iw"   }, .flags = FLAGS },
    { "h",      "Output video height",              OFFSET(h_expr),     AV_OPT_TYPE_STRING, { .str = "ih"   }, .flags = FLAGS },
    { "format", "Output pixel format",              OFFSET(format_str), AV_OPT_TYPE_STRING, { .str = "same" }, .flags = FLAGS },

    { "scale_type",    "Scale Type",                OFFSET(scale_type),      AV_OPT_TYPE_INT,   { .i64 = AMF_VIDEO_CONVERTER_SCALE_BILINEAR },
        AMF_VIDEO_CONVERTER_SCALE_BILINEAR, AMF_VIDEO_CONVERTER_SCALE_BICUBIC, FLAGS, "scale_type" },
    { "bilinear",      "Bilinear",      0,                       AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_CONVERTER_SCALE_BILINEAR },       0, 0, FLAGS, "scale_type" },
    { "bicubic",       "Bicubic",       0,                       AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_CONVERTER_SCALE_BICUBIC },        0, 0, FLAGS, "scale_type" },

    { "color_profile", "Color Profile",             OFFSET(color_profile),   AV_OPT_TYPE_INT,    { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN },
        AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN, AMF_VIDEO_CONVERTER_COLOR_PROFILE_JPEG, FLAGS, "color_profile" },
    { "auto",      "Auto",       0, AV_OPT_TYPE_CONST,  { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN },    0, 0, FLAGS, "color_profile" },
    { "601",       "601",        0, AV_OPT_TYPE_CONST,  { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_601  },       0, 0, FLAGS, "color_profile" },
    { "709",       "709",        0, AV_OPT_TYPE_CONST,  { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_709  },       0, 0, FLAGS, "color_profile" },
    { "2020",      "2020",       0, AV_OPT_TYPE_CONST,  { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020  },      0, 0, FLAGS, "color_profile" },
    { "full",      "Full Range", 0, AV_OPT_TYPE_CONST,  { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_JPEG  },      0, 0, FLAGS, "color_profile" },

    { "keep_aspect_ratio", "Keep Aspect Ratio",     OFFSET(keep_aspect_ratio), AV_OPT_TYPE_BOOL,  { .i64 = 0 }, 0, 1, FLAGS },

    { "fill",       "Enable fill area out of ROI",  OFFSET(fill),           AV_OPT_TYPE_BOOL,  { .i64 = 0 }, 0, 1, FLAGS },
    { "fill_color", "Fill color out of ROI",        OFFSET(fill_color),     AV_OPT_TYPE_COLOR,  {.str = "black"}, CHAR_MIN, CHAR_MAX, FLAGS },

    { "rect_left",  "Output video rect.left",       OFFSET(rect_left),    AV_OPT_TYPE_INT, { .i64 = 0   }, 0, INT_MAX, .flags = FLAGS },
    { "rect_right", "Output video rect.right",      OFFSET(rect_right),   AV_OPT_TYPE_INT, { .i64 = 0   }, 0, INT_MAX, .flags = FLAGS },
    { "rect_top",   "Output video rect.top",        OFFSET(rect_top),     AV_OPT_TYPE_INT, { .i64 = 0   }, 0, INT_MAX, .flags = FLAGS },
    { "rect_bottom","Output video rect.bottom",     OFFSET(rect_bottom),  AV_OPT_TYPE_INT, { .i64 = 0   }, 0, INT_MAX, .flags = FLAGS },

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
