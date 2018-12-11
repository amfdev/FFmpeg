#include "amfdec.h"
#include <AMF/core/Variant.h>
#include <AMF/core/PropertyStorage.h>
#include <AMF/components/FFMPEGFileDemuxer.h>
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
//#include "../libavutil/Frame.h"


#define propNotFound 0

typedef struct FormatMap {
    enum AVPixelFormat       av_format;
    enum AMF_SURFACE_FORMAT  amf_format;
} FormatMap;

static const FormatMap format_map[] =
{
    { AV_PIX_FMT_NV12,       AMF_SURFACE_NV12 },

    { AV_PIX_FMT_BGR0,       AMF_SURFACE_BGRA },
    { AV_PIX_FMT_BGRA,       AMF_SURFACE_BGRA },

    { AV_PIX_FMT_RGB0,       AMF_SURFACE_RGBA },
    { AV_PIX_FMT_RGBA,       AMF_SURFACE_RGBA },

    { AV_PIX_FMT_0RGB,       AMF_SURFACE_ARGB },
    { AV_PIX_FMT_ARGB,       AMF_SURFACE_ARGB },

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

static enum AVPixelFormat amf_amf_to_av_format(enum AMF_SURFACE_FORMAT fmt)
{
    int i;
    for (i = 0; i < amf_countof(format_map); i++) {
        if (format_map[i].amf_format == fmt) {
            return format_map[i].av_format;
        }
    }
    return AMF_SURFACE_UNKNOWN;
}


static void amf_free_amfsurface(void *opaque, uint8_t *data)
{
    AMFSurface *surface = (AMFSurface*)(opaque);
    surface->pVtbl->Release(surface);
}

static int amf_init_decoder(AVCodecContext *avctx)
{
    AVAMFDecoderContext        *ctx = avctx->priv_data;
    const wchar_t     *codec_id = NULL;
    AMF_RESULT         res;
    enum AMF_SURFACE_FORMAT formatOut = AMF_SURFACE_NV12;
    AMFBuffer * buffer;
    //enum AVPixelFormat pix_fmt = avctx->sw_pix_fmt;

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            codec_id = AMFVideoDecoderUVD_H264_AVC;
            break;
        case AV_CODEC_ID_HEVC:
            codec_id = AMFVideoDecoderHW_H265_HEVC;
            break;
        default:
            break;
    }
    AMF_RETURN_IF_FALSE(ctx, codec_id != NULL, AVERROR(EINVAL), "Codec %d is not supported\n", avctx->codec->id);

    res = ctx->factory->pVtbl->CreateComponent(ctx->factory, ctx->context, codec_id, &ctx->decoder);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", codec_id, res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_TIMESTAMP_MODE, AMF_TS_DECODE);// our sample H264 parser provides decode order timestamps - change this depend on demuxer

    if (avctx->extradata_size)
    { // set SPS/PPS extracted from stream or container; Alternatively can use parser->SetUseStartCodes(true)
        ctx->context->pVtbl->AllocBuffer(ctx->context, AMF_MEMORY_HOST, avctx->extradata_size, &buffer);

        memcpy(buffer->pVtbl->GetNative(buffer), avctx->extradata, avctx->extradata_size);
        AMF_ASSIGN_PROPERTY_INTERFACE(res,ctx->decoder, AMF_VIDEO_DECODER_EXTRADATA, buffer);
    }

    res = ctx->decoder->pVtbl->Init(ctx->decoder, formatOut, avctx->width, avctx->height);// parser->GetPictureWidth(), parser->GetPictureHeight()
    return 0;
}

static int amf_init_decoder_context(AVCodecContext *avctx)
{
    AVAMFDecoderContext *ctx = avctx->priv_data;
    AVAMFDeviceContext *amf_ctx;
    int ret;

    ret = av_hwdevice_ctx_create(&ctx->amf_device_ctx, AV_HWDEVICE_TYPE_AMF, NULL, NULL, 0);
    if (ret < 0)
        return ret;

    amf_ctx = ((AVHWDeviceContext*)ctx->amf_device_ctx->data)->hwctx;
    ctx->context = amf_ctx->context;
    ctx->factory = amf_ctx->factory;
    return ret;
    return 0;
}

int ff_amf_decode_init(AVCodecContext *avctx)
{
    int ret;

    if ((ret = amf_init_decoder_context(avctx)) == 0) {
        if ((ret = amf_init_decoder(avctx)) == 0) {
            return 0;
        }
    }
    ff_amf_decode_close(avctx);
    return ret;
}

int ff_amf_decode_close(AVCodecContext *avctx)
{
    AVAMFDecoderContext *ctx = avctx->priv_data;

    if (ctx->decoder) {
        ctx->decoder->pVtbl->Terminate(ctx->decoder);
        ctx->decoder->pVtbl->Release(ctx->decoder);
        ctx->decoder = NULL;
    }

    av_buffer_unref(&ctx->hw_device_ctx);
    av_buffer_unref(&ctx->hw_frames_ctx);

    ctx->factory = NULL;
    ctx->context = NULL;
    av_fifo_freep(&ctx->timestamp_list);

    av_buffer_unref(&ctx->amf_device_ctx);

    return 0;
}

static int amf_amfsurface_to_avframe(AVCodecContext *avctx, const AMFSurface* surface, AVFrame *frame)
{
    AMFPlane *plane;
    AMF_RESULT  ret = AMF_OK;
    int       i;

    if (!frame)
        return AMF_INVALID_POINTER;

    for (i = 0; i < surface->pVtbl->GetPlanesCount(surface); i++)
    {
        plane = surface->pVtbl->GetPlaneAt(surface, i);
        frame->data[i] = plane->pVtbl->GetNative(plane);
        frame->linesize[i] = plane->pVtbl->GetHPitch(plane);
    }
    surface->pVtbl->Acquire(surface);
    frame->buf[0] = av_buffer_create(NULL,
                                         0,
                                         amf_free_amfsurface,
                                         surface,
                                         AV_BUFFER_FLAG_READONLY);

    frame->format = amf_amf_to_av_format(surface->pVtbl->GetFormat(surface));
    frame->width  = avctx->width;
    frame->height = avctx->height;
    //TODO: pts, duration, etc

    return AMF_OK;
}

static int ff_amf_receive_frame(const AVCodecContext *avctx, AVFrame *frame)
{
    AVAMFDecoderContext *ctx = avctx->priv_data;
    AMF_RESULT  ret;
    AMFSurface *surface = NULL;
    AVFrame *data = NULL;
    AMFData *data_out = NULL;

    if (!ctx->decoder)
        return AVERROR(EINVAL);

    ret = ctx->decoder->pVtbl->QueryOutput(ctx->decoder, &data_out);
    AMFAV_GOTO_FAIL_IF_FALSE(avctx, ret == AMF_OK, AVERROR_UNKNOWN, "QueryOutput() failed with error %d\n", ret);

    AMFAV_GOTO_FAIL_IF_FALSE(avctx, data_out, AVERROR_UNKNOWN, "QueryOutput() return empty data %d\n", ret);

    ret = data_out->pVtbl->Convert(data_out, AMF_MEMORY_HOST);
    AMF_RETURN_IF_FALSE(avctx, ret == AMF_OK, AMF_UNEXPECTED, L"Convert(amf::AMF_MEMORY_HOST) failed with error %d\n");
    //AMFAV_GOTO_FAIL_IF_FALSE(avctx, ret == AMF_OK, AVERROR_UNKNOWN, "Convert(amf::AMF_MEMORY_HOST) failed with error %d\n", ret);

    if (data_out)
    {
        AMFGuid guid = IID_AMFSurface();
        data_out->pVtbl->QueryInterface(data_out, &guid, (void**)&surface); // query for buffer interface
        data_out->pVtbl->Release(data_out);
    }

    data = av_frame_alloc();
    ret = amf_amfsurface_to_avframe(avctx, surface, data);
    AMFAV_GOTO_FAIL_IF_FALSE(avctx, ret == AMF_OK, AVERROR_UNKNOWN, "Failed to convert AMFSurface to AVFrame", ret);

    av_frame_move_ref(frame, data);

    return ret;
fail:
    if (data) {
        av_frame_free(&data);
    }
    if (surface){
        surface->pVtbl->Release(surface);
    }
    return ret;
}

static void AMF_STD_CALL  UpdateBufferVideoDuration(AVCodecContext *avctx, AMFBuffer* pBuffer, const AVPacket* pPacket)
{
    if (pPacket->duration != 0)
    {
        amf_int64 durationByFFMPEG    = av_rescale_q(pPacket->duration, avctx->time_base, AMF_TIME_BASE_Q);
        amf_int64 durationByFrameRate = (amf_int64)((amf_double)AMF_SECOND / ((amf_double)avctx->framerate.num / (amf_double)avctx->framerate.den));
        if (abs(durationByFrameRate - durationByFFMPEG) > AMF_MIN(durationByFrameRate, durationByFFMPEG) / 2)
        {
            durationByFFMPEG = durationByFrameRate;
        }

        pBuffer->pVtbl->SetDuration(pBuffer, durationByFFMPEG);
    }
}

static AMF_RESULT UpdateBufferProperties(AVCodecContext *avctx, AMFBuffer* pBuffer, const AVPacket* pPacket)
{
    AVAMFDecoderContext *ctx = avctx->priv_data;
    AMFContext *ctxt = ctx->context;
    AMF_RESULT res;

    AMF_RETURN_IF_FALSE(ctxt, pBuffer != NULL, AMF_INVALID_ARG, "UpdateBufferProperties() - buffer not passed in");
    AMF_RETURN_IF_FALSE(ctxt, pPacket != NULL, AMF_INVALID_ARG, "UpdateBufferProperties() - packet not passed in");

    const amf_int64  pts = av_rescale_q(pPacket->dts, avctx->time_base, AMF_TIME_BASE_Q);
    //pBuffer->pVtbl->SetPts(pBuffer, pts - GetMinPosition());
    if (pPacket != NULL)
    {
        //AMFVariantStruct var;
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:pts", pPacket->pts);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:dts", pPacket->dts);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:stream_index", pPacket->stream_index);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:flags", pPacket->flags);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:duration", pPacket->duration);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:pos", pPacket->pos);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:convergence_duration", pPacket->convergence_duration);
    }
    int m_ptsInitialMinPosition = propNotFound;
    AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:FirstPtsOffset", m_ptsInitialMinPosition);

    int vst = propNotFound;//avctx->vst
    if (vst != AV_NOPTS_VALUE)
    {
        int start_time = propNotFound;
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:start_time", start_time);
    }

    AMF_ASSIGN_PROPERTY_DATA(res, Int64, pBuffer, L"FFMPEG:time_base_den", avctx->time_base.den);
    AMF_ASSIGN_PROPERTY_DATA(res, Int64, pBuffer, L"FFMPEG:time_base_num", avctx->time_base.num);

    int condition1 = propNotFound;//(m_iVideoStreamIndex == -1 || pPacket->stream_index == m_iVideoStreamIndex) && m_ptsSeekPos != -1
    if (condition1)
    {
        int m_ptsSeekPos = propNotFound;
        if (pts < m_ptsSeekPos)
        {
            AMF_ASSIGN_PROPERTY_BOOL(res, pBuffer, L"Seeking", true);
        }
        int m_ptsPosition = propNotFound;
        if (m_ptsSeekPos <= m_ptsPosition)
        {
            AMF_ASSIGN_PROPERTY_BOOL(res, pBuffer, L"EndSeeking", true);

            AVFormatContext * m_pInputContext = propNotFound;
            int default_stream_index = av_find_default_stream_index(m_pInputContext);
            if (pPacket->stream_index == default_stream_index)
            {
                m_ptsSeekPos = -1;
            }
        }
        else
        {
            if (pPacket->flags & AV_PKT_FLAG_KEY)
            {
                AMF_ASSIGN_PROPERTY_BOOL(res, pBuffer, L"BeginSeeking", true);
            }
        }
    }
    AMF_ASSIGN_PROPERTY_DATA(res, Int64, pBuffer, FFMPEG_DEMUXER_BUFFER_TYPE, AMF_STREAM_VIDEO);
    UpdateBufferVideoDuration(avctx, pBuffer, pPacket);

    AMF_ASSIGN_PROPERTY_DATA(res, Int64, pBuffer, FFMPEG_DEMUXER_BUFFER_STREAM_INDEX, pPacket->stream_index);
    AMF_RETURN_IF_FALSE(res == AMF_OK, ctxt, res, L"Failed to set property");
    return AMF_OK;
}

static AMF_RESULT BufferFromPacket(AVCodecContext *avctx, const AVPacket* pPacket, AMFBuffer** ppBuffer)
{
    AVAMFDecoderContext *ctx = avctx->priv_data;
    AMFContext *ctxt = ctx->context;
    void *pMem;
    AMF_RESULT err;

    AMF_RETURN_IF_FALSE(ctxt, pPacket != NULL, AMF_INVALID_ARG, L"BufferFromPacket() - packet not passed in");
    AMF_RETURN_IF_FALSE(ctxt, ppBuffer != NULL, AMF_INVALID_ARG, L"BufferFromPacket() - buffer pointer not passed in");


    // Reproduce FFMPEG packet allocate logic (file libavcodec/avpacket.c function av_packet_duplicate)
    // ...
    //    data = av_malloc(pkt->size + FF_INPUT_BUFFER_PADDING_SIZE);
    // ...
    //MM this causes problems because there is no way to set real buffer size. Allocation has 32 byte alignment - should be enough.
    err = ctxt->pVtbl->AllocBuffer(ctxt, AMF_MEMORY_HOST, pPacket->size + AV_INPUT_BUFFER_PADDING_SIZE, ppBuffer);
    AMF_RETURN_IF_FALSE(ctxt, err == AMF_OK, err, L"BufferFromPacket() - AllocBuffer failed");

    AMFBuffer* pBuffer = *ppBuffer;
    err = pBuffer->pVtbl->SetSize(pBuffer, pPacket->size);
    AMF_RETURN_IF_FALSE(ctxt, err == AMF_OK, err, L"BufferFromPacket() - SetSize failed");

    // get the memory location and check the buffer was indeed allocated
    pMem = pBuffer->pVtbl->GetNative(pBuffer);
    AMF_RETURN_IF_FALSE(ctxt, pMem != NULL, AMF_INVALID_POINTER, "BufferFromPacket() - GetMemory failed");

    // copy the packet memory and don't forget to
    // clear data padding like it is done by FFMPEG
    memcpy(pMem, pPacket->data, pPacket->size);
    memset((amf_int8*)(pMem)+pPacket->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    // now that we created the buffer, it's time to update
    // it's properties from the packet information...
    return UpdateBufferProperties(avctx, pBuffer, pPacket);
}



int amf_decode_frame(AVCodecContext *avctx, void *data,
                       int *got_frame, AVPacket *avpkt)
{
    AVAMFDecoderContext *ctx = avctx->priv_data;

    AMFBuffer * buf;
    AMF_RESULT res;
    AVFrame *frame = data;

    if (!avpkt->size)
    {
        res = ff_amf_receive_frame(avctx, frame);
        if (res == AMF_OK)
        {
            *got_frame = 1;
        }
        return 0;
    }

    res = BufferFromPacket(avctx, avpkt, &buf);
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, 0, "Cannot convert AVPacket to AMFbuffer");

    while(1)
    {
        res = ctx->decoder->pVtbl->SubmitInput(ctx->decoder, buf);
        if (res == AMF_OK)
        {
            av_log(avctx, AV_LOG_ERROR, "\nsubmit successful\n");
            break;
        }
        else if (res == AMF_INPUT_FULL || res == AMF_DECODER_NO_FREE_SURFACES)
        {
            av_log(avctx, AV_LOG_ERROR, "\nsubmit Failed: input full\n");
            res = ff_amf_receive_frame(avctx, frame);
            if (res == AMF_OK)
            {
                AMF_RETURN_IF_FALSE(avctx, !*got_frame, avpkt->size, "frame already got");
                *got_frame = 1;
                av_usleep(1000);
            }
        }
        else
        {
            AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, 0, "SubmitInput to decoder failed");
        }
    }

    return avpkt->size;
}

static void amf_decode_flush(AVCodecContext *avctx)
{
    int i = 5;
    i+=2;
}

#define OFFSET(x) offsetof(AVAMFDecoderContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    //Decoder mode
    { "decoder_mode",          "Decoder mode",        OFFSET(decoder_mode),  AV_OPT_TYPE_INT,   { .i64 = AMF_VIDEO_DECODER_MODE_REGULAR      }, AMF_VIDEO_DECODER_MODE_REGULAR, AMF_VIDEO_DECODER_MODE_LOW_LATENCY, VD, "decoder_mode" },
    { "timestamp_mode",          "Timestamp mode",        OFFSET(timestamp_mode),  AV_OPT_TYPE_INT,   { .i64 = AMF_TS_PRESENTATION      }, AMF_TS_PRESENTATION, AMF_TS_DECODE, VD, "timestamp_mode" },
    { NULL }
};

static const AVClass amf_decode_class = {
    .class_name = "amf",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const enum AVPixelFormat ff_amf_pix_fmts1[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
#if CONFIG_D3D11VA
    AV_PIX_FMT_D3D11,
#endif
#if CONFIG_DXVA2
    AV_PIX_FMT_DXVA2_VLD,
#endif
    AV_PIX_FMT_NONE
};

AVCodec ff_amf_decoder = {
    .name           = "amf",
//    .long_name      = NULL_IF_CONFIG_SMALL("AMD AMF decoder"),
    .long_name      = "AMD AMF decoder",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(AVAMFDecoderContext),
    .priv_class     = &amf_decode_class,
    .init           = ff_amf_decode_init,
    .decode         = amf_decode_frame,
    .flush          = amf_decode_flush,
    .close          = ff_amf_decode_close,
    .pix_fmts       = ff_amf_pix_fmts1,
    //.bsfs           = "h264", //TODO: real vcalue
    .capabilities   = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | //TODO: real vcalue
                      AV_CODEC_CAP_AVOID_PROBING,
    .wrapper_name   = "amf",
};
