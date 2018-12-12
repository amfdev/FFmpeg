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
#include "config.h"

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext.h"

#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"


#include "amfenc.h"
#include "internal.h"

#if CONFIG_DXVA2
#include <d3d9.h>
#endif

#if CONFIG_D3D11VA
#include <d3d11.h>
#endif

#define FFMPEG_AMF_WRITER_ID L"ffmpeg_amf"

#define PTS_PROP L"PtsProp"

static int amf_init_context(AVCodecContext *avctx)
{
    AvAmfEncoderContext *ctx = avctx->priv_data;
    AVAMFDeviceContext *amf_ctx;
    int ret;

    ctx->delayed_frame = av_frame_alloc();
    if (!ctx->delayed_frame) {
        return AVERROR(ENOMEM);
    }

    // hardcoded to current HW queue size - will realloc in timestamp_queue_enqueue() if too small
    ctx->timestamp_list = av_fifo_alloc((avctx->max_b_frames + 16) * sizeof(int64_t));
    if (!ctx->timestamp_list) {
        return AVERROR(ENOMEM);
    }
    ctx->dts_delay = 0;

    ctx->hwsurfaces_in_queue = 0;
    ctx->hwsurfaces_in_queue_max = 16;

    // If a device was passed to the encoder, try to initialise from that.
    if (avctx->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;

        if (amf_av_to_amf_format(frames_ctx->sw_format) == AMF_SURFACE_UNKNOWN) {
            av_log(avctx, AV_LOG_ERROR, "Format of input frames context (%s) is not supported by AMF.\n",
                   av_get_pix_fmt_name(frames_ctx->sw_format));
            return AVERROR(EINVAL);
        }

        ret = av_hwdevice_ctx_create_derived(&ctx->amf_device_ctx, AV_HWDEVICE_TYPE_AMF, frames_ctx->device_ref, 0);
        if (ret < 0)
            return ret;

        ctx->hw_frames_ctx = av_buffer_ref(avctx->hw_frames_ctx);
        if (!ctx->hw_frames_ctx)
            return AVERROR(ENOMEM);

        if (frames_ctx->initial_pool_size > 0)
            ctx->hwsurfaces_in_queue_max = frames_ctx->initial_pool_size - 1;

    } else if (avctx->hw_device_ctx) {
        ret = av_hwdevice_ctx_create_derived(&ctx->amf_device_ctx, AV_HWDEVICE_TYPE_AMF, avctx->hw_device_ctx, 0);
        if (ret < 0)
            return ret;

        ctx->hw_device_ctx = av_buffer_ref(avctx->hw_device_ctx);
        if (!ctx->hw_device_ctx)
            return AVERROR(ENOMEM);

    } else {
        ret = av_hwdevice_ctx_create(&ctx->amf_device_ctx, AV_HWDEVICE_TYPE_AMF, NULL, NULL, 0);
        if (ret < 0)
            return ret;
    }

    amf_ctx = ((AVHWDeviceContext*)ctx->amf_device_ctx->data)->hwctx;
    ctx->context = amf_ctx->context;
    ctx->factory = amf_ctx->factory;
    return 0;
}

static int amf_init_encoder(AVCodecContext *avctx)
{
    AvAmfEncoderContext        *ctx = avctx->priv_data;
    const wchar_t     *codec_id = NULL;
    AMF_RESULT         res;
    enum AVPixelFormat pix_fmt;

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            codec_id = AMFVideoEncoderVCE_AVC;
            break;
        case AV_CODEC_ID_HEVC:
            codec_id = AMFVideoEncoder_HEVC;
            break;
        default:
            break;
    }
    AMF_RETURN_IF_FALSE(ctx, codec_id != NULL, AVERROR(EINVAL), "Codec %d is not supported\n", avctx->codec->id);

    if (ctx->hw_frames_ctx)
        pix_fmt = ((AVHWFramesContext*)ctx->hw_frames_ctx->data)->sw_format;
    else
        pix_fmt = avctx->pix_fmt;

    ctx->format = amf_av_to_amf_format(pix_fmt);
    AMF_RETURN_IF_FALSE(ctx, ctx->format != AMF_SURFACE_UNKNOWN, AVERROR(EINVAL),
                        "Format %s is not supported\n", av_get_pix_fmt_name(pix_fmt));

    res = ctx->factory->pVtbl->CreateComponent(ctx->factory, ctx->context, codec_id, &ctx->encoder);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", codec_id, res);

    return 0;
}

int av_cold ff_amf_encode_close(AVCodecContext *avctx)
{
    AvAmfEncoderContext *ctx = avctx->priv_data;

    if (ctx->delayed_surface) {
        ctx->delayed_surface->pVtbl->Release(ctx->delayed_surface);
        ctx->delayed_surface = NULL;
    }

    if (ctx->encoder) {
        ctx->encoder->pVtbl->Terminate(ctx->encoder);
        ctx->encoder->pVtbl->Release(ctx->encoder);
        ctx->encoder = NULL;
    }

    av_buffer_unref(&ctx->hw_device_ctx);
    av_buffer_unref(&ctx->hw_frames_ctx);

    ctx->factory = NULL;
    ctx->context = NULL;
    ctx->delayed_drain = 0;
    av_frame_free(&ctx->delayed_frame);
    av_fifo_freep(&ctx->timestamp_list);

    av_buffer_unref(&ctx->amf_device_ctx);

    return 0;
}

static int amf_copy_surface(AVCodecContext *avctx, const AVFrame *frame,
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
        avctx->width, avctx->height);

    return 0;
}

static inline int timestamp_queue_enqueue(AVCodecContext *avctx, int64_t timestamp)
{
    AvAmfEncoderContext         *ctx = avctx->priv_data;
    if (av_fifo_space(ctx->timestamp_list) < sizeof(timestamp)) {
        if (av_fifo_grow(ctx->timestamp_list, sizeof(timestamp)) < 0) {
            return AVERROR(ENOMEM);
        }
    }
    av_fifo_generic_write(ctx->timestamp_list, &timestamp, sizeof(timestamp), NULL);
    return 0;
}

static int amf_copy_buffer(AVCodecContext *avctx, AVPacket *pkt, AMFBuffer *buffer)
{
    AvAmfEncoderContext      *ctx = avctx->priv_data;
    int              ret;
    AMFVariantStruct var = {0};
    int64_t          timestamp = AV_NOPTS_VALUE;
    int64_t          size = buffer->pVtbl->GetSize(buffer);

    if ((ret = ff_alloc_packet2(avctx, pkt, size, 0)) < 0) {
        return ret;
    }
    memcpy(pkt->data, buffer->pVtbl->GetNative(buffer), size);

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &var);
            if(var.int64Value == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR) {
                pkt->flags = AV_PKT_FLAG_KEY;
            }
            break;
        case AV_CODEC_ID_HEVC:
            buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &var);
            if (var.int64Value == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR) {
                pkt->flags = AV_PKT_FLAG_KEY;
            }
            break;
        default:
            break;
    }

    buffer->pVtbl->GetProperty(buffer, PTS_PROP, &var);

    pkt->pts = var.int64Value; // original pts


    AMF_RETURN_IF_FALSE(ctx, av_fifo_size(ctx->timestamp_list) > 0, AVERROR_UNKNOWN, "timestamp_list is empty\n");

    av_fifo_generic_read(ctx->timestamp_list, &timestamp, sizeof(timestamp), NULL);

    // calc dts shift if max_b_frames > 0
    if (avctx->max_b_frames > 0 && ctx->dts_delay == 0) {
        int64_t timestamp_last = AV_NOPTS_VALUE;
        AMF_RETURN_IF_FALSE(ctx, av_fifo_size(ctx->timestamp_list) > 0, AVERROR_UNKNOWN,
            "timestamp_list is empty while max_b_frames = %d\n", avctx->max_b_frames);
        av_fifo_generic_peek_at(
            ctx->timestamp_list,
            &timestamp_last,
            (av_fifo_size(ctx->timestamp_list) / sizeof(timestamp) - 1) * sizeof(timestamp_last),
            sizeof(timestamp_last),
            NULL);
        if (timestamp < 0 || timestamp_last < AV_NOPTS_VALUE) {
            return AVERROR(ERANGE);
        }
        ctx->dts_delay = timestamp_last - timestamp;
    }
    pkt->dts = timestamp - ctx->dts_delay;
    return 0;
}

// amfenc API implementation
int ff_amf_encode_init(AVCodecContext *avctx)
{
    int ret;

    if ((ret = amf_init_context(avctx)) == 0) {
        if ((ret = amf_init_encoder(avctx)) == 0) {
            return 0;
        }
    }
    ff_amf_encode_close(avctx);
    return ret;
}

static AMF_RESULT amf_set_property_buffer(AMFSurface *object, const wchar_t *name, AMFBuffer *val)
{
    AMF_RESULT res;
    AMFVariantStruct var;
    res = AMFVariantInit(&var);
    if (res == AMF_OK) {
        AMFGuid guid_AMFInterface = IID_AMFInterface();
        AMFInterface *amf_interface;
        res = val->pVtbl->QueryInterface(val, &guid_AMFInterface, (void**)&amf_interface);

        if (res == AMF_OK) {
            res = AMFVariantAssignInterface(&var, amf_interface);
            amf_interface->pVtbl->Release(amf_interface);
        }
        if (res == AMF_OK) {
            res = object->pVtbl->SetProperty(object, name, var);
        }
        AMFVariantClear(&var);
    }
    return res;
}

static AMF_RESULT amf_get_property_buffer(AMFData *object, const wchar_t *name, AMFBuffer **val)
{
    AMF_RESULT res;
    AMFVariantStruct var;
    res = AMFVariantInit(&var);
    if (res == AMF_OK) {
        res = object->pVtbl->GetProperty(object, name, &var);
        if (res == AMF_OK) {
            if (var.type == AMF_VARIANT_INTERFACE) {
                AMFGuid guid_AMFBuffer = IID_AMFBuffer();
                AMFInterface *amf_interface = AMFVariantInterface(&var);
                res = amf_interface->pVtbl->QueryInterface(amf_interface, &guid_AMFBuffer, (void**)val);
            } else {
                res = AMF_INVALID_DATA_TYPE;
            }
        }
        AMFVariantClear(&var);
    }
    return res;
}

static AMFBuffer *amf_create_buffer_with_frame_ref(const AVFrame *frame, AMFContext *context)
{
    AVFrame *frame_ref;
    AMFBuffer *frame_ref_storage_buffer = NULL;
    AMF_RESULT res;

    res = context->pVtbl->AllocBuffer(context, AMF_MEMORY_HOST, sizeof(frame_ref), &frame_ref_storage_buffer);
    if (res == AMF_OK) {
        frame_ref = av_frame_clone(frame);
        if (frame_ref) {
            memcpy(frame_ref_storage_buffer->pVtbl->GetNative(frame_ref_storage_buffer), &frame_ref, sizeof(frame_ref));
        } else {
            frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
            frame_ref_storage_buffer = NULL;
        }
    }
    return frame_ref_storage_buffer;
}

static void amf_release_buffer_with_frame_ref(AMFBuffer *frame_ref_storage_buffer)
{
    AVFrame *frame_ref;
    memcpy(&frame_ref, frame_ref_storage_buffer->pVtbl->GetNative(frame_ref_storage_buffer), sizeof(frame_ref));
    av_frame_free(&frame_ref);
    frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
}

int ff_amf_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    AvAmfEncoderContext *ctx = avctx->priv_data;
    AMFSurface *surface;
    AMF_RESULT  res;
    int         ret;

    if (!ctx->encoder)
        return AVERROR(EINVAL);

    if (!frame) { // submit drain
        if (!ctx->eof) { // submit drain one time only
            if (ctx->delayed_surface != NULL) {
                ctx->delayed_drain = 1; // input queue is full: resubmit Drain() in ff_amf_receive_packet
            } else if(!ctx->delayed_drain) {
                res = ctx->encoder->pVtbl->Drain(ctx->encoder);
                if (res == AMF_INPUT_FULL) {
                    ctx->delayed_drain = 1; // input queue is full: resubmit Drain() in ff_amf_receive_packet
                } else {
                    if (res == AMF_OK) {
                        ctx->eof = 1; // drain started
                    }
                    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "Drain() failed with error %d\n", res);
                }
            }
        } else{
            return AVERROR_EOF;
        }
    } else { // submit frame
        int hw_surface = 0;

        if (ctx->delayed_surface != NULL) {
            return AVERROR(EAGAIN); // should not happen when called from ffmpeg, other clients may resubmit
        }
        // prepare surface from frame
        switch (frame->format) {
#if CONFIG_D3D11VA
        case AV_PIX_FMT_D3D11:
            {
                static const GUID AMFTextureArrayIndexGUID = { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };
                ID3D11Texture2D *texture = (ID3D11Texture2D*)frame->data[0]; // actual texture
                int index = (intptr_t)frame->data[1]; // index is a slice in texture array is - set to tell AMF which slice to use

                av_assert0(frame->hw_frames_ctx       && ctx->hw_frames_ctx &&
                           frame->hw_frames_ctx->data == ctx->hw_frames_ctx->data);

                texture->lpVtbl->SetPrivateData(texture, &AMFTextureArrayIndexGUID, sizeof(index), &index);

                res = ctx->context->pVtbl->CreateSurfaceFromDX11Native(ctx->context, texture, &surface, NULL); // wrap to AMF surface
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX11Native() failed  with error %d\n", res);

                hw_surface = 1;
            }
            break;
#endif
#if CONFIG_DXVA2
        case AV_PIX_FMT_DXVA2_VLD:
            {
                IDirect3DSurface9 *texture = (IDirect3DSurface9 *)frame->data[3]; // actual texture

                res = ctx->context->pVtbl->CreateSurfaceFromDX9Native(ctx->context, texture, &surface, NULL); // wrap to AMF surface
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX9Native() failed  with error %d\n", res);

                hw_surface = 1;
            }
            break;
#endif
        default:
            {
                res = ctx->context->pVtbl->AllocSurface(ctx->context, AMF_MEMORY_HOST, ctx->format, avctx->width, avctx->height, &surface);
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "AllocSurface() failed  with error %d\n", res);
                amf_copy_surface(avctx, frame, surface);
            }
            break;
        }

        if (hw_surface) {
            AMFBuffer *frame_ref_storage_buffer;

            // input HW surfaces can be vertically aligned by 16; tell AMF the real size
            surface->pVtbl->SetCrop(surface, 0, 0, frame->width, frame->height);

            frame_ref_storage_buffer = amf_create_buffer_with_frame_ref(frame, ctx->context);
            AMF_RETURN_IF_FALSE(ctx, frame_ref_storage_buffer != NULL, AVERROR(ENOMEM), "create_buffer_with_frame_ref() returned NULL\n");

            res = amf_set_property_buffer(surface, L"av_frame_ref", frame_ref_storage_buffer);
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SetProperty failed for \"av_frame_ref\" with error %d\n", res);
            ctx->hwsurfaces_in_queue++;
            frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
        }

        surface->pVtbl->SetPts(surface, frame->pts);
        AMF_ASSIGN_PROPERTY_INT64(res, surface, PTS_PROP, frame->pts);

        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_AUD, !!ctx->aud);
            break;
        case AV_CODEC_ID_HEVC:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_INSERT_AUD, !!ctx->aud);
            break;
        default:
            break;
        }


        // submit surface
        res = ctx->encoder->pVtbl->SubmitInput(ctx->encoder, (AMFData*)surface);
        if (res == AMF_INPUT_FULL) { // handle full queue
            //store surface for later submission
            ctx->delayed_surface = surface;
            if (surface->pVtbl->GetMemoryType(surface) == AMF_MEMORY_DX11) {
                av_frame_ref(ctx->delayed_frame, frame);
            }
        } else {
            surface->pVtbl->Release(surface);
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SubmitInput() failed with error %d\n", res);

            if ((ret = timestamp_queue_enqueue(avctx, frame->pts)) < 0) {
                return ret;
            }

        }
    }
    return 0;
}
int ff_amf_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    int             ret;
    AMF_RESULT      res;
    AMF_RESULT      res_query;
    AvAmfEncoderContext     *ctx = avctx->priv_data;
    AMFData        *data = NULL;
    int             block_and_wait;

    if (!ctx->encoder)
        return AVERROR(EINVAL);

    do {
        block_and_wait = 0;
        // poll data
        res_query = ctx->encoder->pVtbl->QueryOutput(ctx->encoder, &data);
        if (data) {
            // copy data to packet
            AMFBuffer* buffer;
            AMFGuid guid = IID_AMFBuffer();
            data->pVtbl->QueryInterface(data, &guid, (void**)&buffer); // query for buffer interface
            ret = amf_copy_buffer(avctx, avpkt, buffer);

            buffer->pVtbl->Release(buffer);

            if (data->pVtbl->HasProperty(data, L"av_frame_ref")) {
                AMFBuffer *frame_ref_storage_buffer;
                res = amf_get_property_buffer(data, L"av_frame_ref", &frame_ref_storage_buffer);
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetProperty failed for \"av_frame_ref\" with error %d\n", res);
                amf_release_buffer_with_frame_ref(frame_ref_storage_buffer);
                ctx->hwsurfaces_in_queue--;
            }

            data->pVtbl->Release(data);

            AMF_RETURN_IF_FALSE(ctx, ret >= 0, ret, "amf_copy_buffer() failed with error %d\n", ret);

            if (ctx->delayed_surface != NULL) { // try to resubmit frame
                res = ctx->encoder->pVtbl->SubmitInput(ctx->encoder, (AMFData*)ctx->delayed_surface);
                if (res != AMF_INPUT_FULL) {
                    int64_t pts = ctx->delayed_surface->pVtbl->GetPts(ctx->delayed_surface);
                    ctx->delayed_surface->pVtbl->Release(ctx->delayed_surface);
                    ctx->delayed_surface = NULL;
                    av_frame_unref(ctx->delayed_frame);
                    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "Repeated SubmitInput() failed with error %d\n", res);

                    if ((ret = timestamp_queue_enqueue(avctx, pts)) < 0) {
                        return ret;
                    }
                } else {
                    av_log(avctx, AV_LOG_WARNING, "Data acquired but delayed frame submission got AMF_INPUT_FULL- should not happen\n");
                }
            } else if (ctx->delayed_drain) { // try to resubmit drain
                res = ctx->encoder->pVtbl->Drain(ctx->encoder);
                if (res != AMF_INPUT_FULL) {
                    ctx->delayed_drain = 0;
                    ctx->eof = 1; // drain started
                    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "Repeated Drain() failed with error %d\n", res);
                } else {
                    av_log(avctx, AV_LOG_WARNING, "Data acquired but delayed drain submission got AMF_INPUT_FULL- should not happen\n");
                }
            }
        } else if (ctx->delayed_surface != NULL || ctx->delayed_drain || (ctx->eof && res_query != AMF_EOF) || (ctx->hwsurfaces_in_queue >= ctx->hwsurfaces_in_queue_max)) {
            block_and_wait = 1;
            av_usleep(1000); // wait and poll again
        }
    } while (block_and_wait);

    if (res_query == AMF_EOF) {
        ret = AVERROR_EOF;
    } else if (data == NULL) {
        ret = AVERROR(EAGAIN);
    } else {
        ret = 0;
    }
    return ret;
}
