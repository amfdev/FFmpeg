#include "amfdec.h"

static int amf_init_decoder(AVCodecContext *avctx)
{
    AVAMFDecoderContext        *ctx = avctx->priv_data;
    const wchar_t     *codec_id = NULL;
    AMF_RESULT         res;
    enum AVPixelFormat pix_fmt;

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

//    if (ctx->hw_frames_ctx)
//        pix_fmt = ((AVHWFramesContext*)ctx->hw_frames_ctx->data)->sw_format;
//    else
//        pix_fmt = avctx->pix_fmt;

//    ctx->format = amf_av_to_amf_format(pix_fmt);
//    AMF_RETURN_IF_FALSE(ctx, ctx->format != AMF_SURFACE_UNKNOWN, AVERROR(EINVAL),
//                        "Format %s is not supported\n", av_get_pix_fmt_name(pix_fmt));

    res = ctx->factory->pVtbl->CreateComponent(ctx->factory, ctx->context, codec_id, &ctx->decoder);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", codec_id, res);

    return 0;
}

static int amf_init_decoder_context(AVCodecContext *avctx)
{
//    AVAMFDecoderContext *ctx = avctx->priv_data;
//    AVAMFDeviceContext *amf_ctx;
//    int ret;

//    ret = av_hwdevice_ctx_create(&ctx->amf_device_ctx, AV_HWDEVICE_TYPE_AMF, NULL, NULL, 0);
//    if (ret < 0)
//        return ret;

//    amf_ctx = ((AVHWDeviceContext*)ctx->amf_device_ctx->data)->hwctx;
//    ctx->context = amf_ctx->context;
//    ctx->factory = amf_ctx->factory;
//    return ret;
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

    if (ctx->delayed_surface) {
        ctx->delayed_surface->pVtbl->Release(ctx->delayed_surface);
        ctx->delayed_surface = NULL;
    }

    if (ctx->decoder) {
        ctx->decoder->pVtbl->Terminate(ctx->decoder);
        ctx->decoder->pVtbl->Release(ctx->decoder);
        ctx->decoder = NULL;
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

////invert
//static int amf_copy_surface(AVCodecContext *avctx, const AVFrame *frame,
//    AMFSurface* surface)
//{
//    AMFPlane *plane;
//    uint8_t  *dst_data[4];
//    int       dst_linesize[4];
//    int       planes;
//    int       i;

//    planes = surface->pVtbl->GetPlanesCount(surface);
//    av_assert0(planes < FF_ARRAY_ELEMS(dst_data));

//    for (i = 0; i < planes; i++) {
//        plane = surface->pVtbl->GetPlaneAt(surface, i);
//        dst_data[i] = plane->pVtbl->GetNative(plane);
//        dst_linesize[i] = plane->pVtbl->GetHPitch(plane);
//    }
//    av_image_copy(dst_data, dst_linesize,
//        (const uint8_t**)frame->data, frame->linesize, frame->format,
//        avctx->width, avctx->height);

//    return 0;
//}

//static AVFrame *amf_amfsurface_to_avframe(AVFilterContext *avctx, AMFSurface* pSurface)
//{
//    AVFrame *frame = av_frame_alloc();

//    if (!frame)
//        return NULL;
//    pSurface . convert amf memory host
//            func copy from surf to av

//    switch (pSurface->pVtbl->GetMemoryType(pSurface))
//    {
//#if CONFIG_D3D11VA
//        case AMF_MEMORY_DX11:
//        {
//            AMFPlane *plane0 = pSurface->pVtbl->GetPlaneAt(pSurface, 0);
//            frame->data[0] = plane0->pVtbl->GetNative(plane0);
//            frame->data[1] = (uint8_t*)(intptr_t)0;

//            frame->buf[0] = av_buffer_create(NULL,
//                                     0,
//                                     amf_free_amfsurface,
//                                     pSurface,
//                                     AV_BUFFER_FLAG_READONLY);
//            pSurface->pVtbl->Acquire(pSurface);
//        }
//        break;
//#endif
//#if CONFIG_DXVA2
//        case AMF_MEMORY_DX9:
//        {
//            AMFPlane *plane0 = pSurface->pVtbl->GetPlaneAt(pSurface, 0);
//            frame->data[3] = plane0->pVtbl->GetNative(plane0);

//            frame->buf[0] = av_buffer_create(NULL,
//                                     0,
//                                     amf_free_amfsurface,
//                                     pSurface,
//                                     AV_BUFFER_FLAG_READONLY);
//            pSurface->pVtbl->Acquire(pSurface);
//        }
//        break;
//#endif
//    default:
//        {
//            av_assert0(0);//should not happen
//        }
//    }

//    return frame;
//}

//int ff_amf_receive_frame(AVCodecContext *avctx, AVFrame *frame)
//{
//    AVAMFDecoderContext *ctx = avctx->priv_data;
//    AMF_RESULT  res;
//    AMFSurface *surface;
//    AMFData *data_out;

//    res = ctx->decoder->pVtbl->QueryOutput(ctx->decoder, &data_out);
//    AMFAV_GOTO_FAIL_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "QueryOutput() failed with error %d\n", res);

//    if (data_out)
//    {
//        AMFGuid guid = IID_AMFSurface();
//        data_out->pVtbl->QueryInterface(data_out, &guid, (void**)&surface_out); // query for buffer interface
//        data_out->pVtbl->Release(data_out);
//    }
//    frame = amf_amfsurface_to_avframe(avctx, surface);
//    // copy props
//    frame->format = outlink->format;
//    frame->width  = outlink->w;
//    frame->height = outlink->h;

//    frame->hw_frames_ctx = av_buffer_ref(ctx->hwframes_out_ref);
//    if (!frame->hw_frames_ctx)
//    {
//        res = AVERROR(ENOMEM);
//        goto fail;
//    }
//fail:
//    av_frame_free(&frame);
//    return res;
//}


//AMF_RESULT BufferFromPacket(AMFContext * ctxt, const AVPacket* pPacket, AMFBuffer** ppBuffer)
//{
//    AMF_RETURN_IF_FALSE(pPacket != NULL, AMF_INVALID_ARG, L"BufferFromPacket() - packet not passed in");
//    AMF_RETURN_IF_FALSE(ppBuffer != NULL, AMF_INVALID_ARG, L"BufferFromPacket() - buffer pointer not passed in");


//    // Reproduce FFMPEG packet allocate logic (file libavcodec/avpacket.c function av_packet_duplicate)
//    // ...
//    //    data = av_malloc(pkt->size + FF_INPUT_BUFFER_PADDING_SIZE);
//    // ...
//    //MM this causes problems because there is no way to set real buffer size. Allocation has 32 byte alignment - should be enough.
//    AMF_RESULT err = ctxt->AllocBuffer(AMF_MEMORY_HOST, pPacket->size + FF_INPUT_BUFFER_PADDING_SIZE, ppBuffer);
//    AMF_RETURN_IF_FAILED(err, L"BufferFromPacket() - AllocBuffer failed");

//    AMFBuffer* pBuffer = *ppBuffer;
//    err = pBuffer->SetSize(pPacket->size);
//    AMF_RETURN_IF_FAILED(err, L"BufferFromPacket() - SetSize failed");

//    // get the memory location and check the buffer was indeed allocated
//    void* pMem = pBuffer->GetNative();
//    AMF_RETURN_IF_FALSE(pMem != NULL, AMF_INVALID_POINTER, L"BufferFromPacket() - GetMemory failed");

//    // copy the packet memory and don't forget to
//    // clear data padding like it is done by FFMPEG
//    memcpy(pMem, pPacket->data, pPacket->size);
//    memset(reinterpret_cast<amf_int8*>(pMem)+pPacket->size, 0, FF_INPUT_BUFFER_PADDING_SIZE);

//    // now that we created the buffer, it's time to update
//    // it's properties from the packet information...
//    return UpdateBufferProperties(pBuffer, pPacket);
//}

//AMF_RESULT UpdateBufferProperties(AMFBuffer* pBuffer, const AVPacket* pPacket)
//{
//    AMF_RETURN_IF_FALSE(pBuffer != NULL, AMF_INVALID_ARG, L"UpdateBufferProperties() - buffer not passed in");
//    AMF_RETURN_IF_FALSE(pPacket != NULL, AMF_INVALID_ARG, L"UpdateBufferProperties() - packet not passed in");

//    const AVStream*  ist = m_pInputContext->streams[pPacket->stream_index];
//    AMF_RETURN_IF_FALSE(ist != NULL, AMF_UNEXPECTED, L"UpdateBufferProperties() - stream not available");

//    const amf_int64  pts = av_rescale_q(pPacket->dts, ist->time_base, AMF_TIME_BASE_Q);
//    pBuffer->SetPts(pts - GetMinPosition());

//    if (pPacket != NULL)
//    {
//        pBuffer->SetProperty(L"FFMPEG:pts", AMFVariant(pPacket->pts));
//        pBuffer->SetProperty(L"FFMPEG:dts", AMFVariant(pPacket->dts));
//        pBuffer->SetProperty(L"FFMPEG:stream_index", AMFVariant(pPacket->stream_index));
//        pBuffer->SetProperty(L"FFMPEG:flags", AMFVariant(pPacket->flags));
//        pBuffer->SetProperty(L"FFMPEG:duration", AMFVariant(pPacket->duration));
//        pBuffer->SetProperty(L"FFMPEG:pos", AMFVariant(pPacket->pos));
//        pBuffer->SetProperty(L"FFMPEG:convergence_duration", AMFVariant(pPacket->convergence_duration));
//    }

//    pBuffer->SetProperty(L"FFMPEG:FirstPtsOffset", AMFVariant(m_ptsInitialMinPosition));

//    if (ist->start_time != AV_NOPTS_VALUE)
//    {
//        pBuffer->SetProperty(L"FFMPEG:start_time", AMFVariant(ist->start_time));
//    }
//    pBuffer->SetProperty(L"FFMPEG:time_base_den", AMFVariant(ist->time_base.den));
//    pBuffer->SetProperty(L"FFMPEG:time_base_num", AMFVariant(ist->time_base.num));


//    if ((m_iVideoStreamIndex == -1 || pPacket->stream_index == m_iVideoStreamIndex) && m_ptsSeekPos != -1)
//    {
//        if (pts < m_ptsSeekPos)
//        {
//            pBuffer->SetProperty(L"Seeking", AMFVariant(true));
//        }

//        if (m_ptsSeekPos <= m_ptsPosition)
//        {
//            pBuffer->SetProperty(L"EndSeeking", AMFVariant(true));

//            int default_stream_index = av_find_default_stream_index(m_pInputContext);
//            if (pPacket->stream_index == default_stream_index)
//            {
//                m_ptsSeekPos = -1;
//            }
//            // set end
//        }
//        else
//        {
//            if (pPacket->flags & AV_PKT_FLAG_KEY)
//            {
//                pBuffer->SetProperty(L"BeginSeeking", AMFVariant(true));
//            }
//        }
//    }

//    // update buffer duration, based on the type if info stored
//    if (ist->codec->codec_type == AVMEDIA_TYPE_VIDEO)
//    {
//        pBuffer->SetProperty(FFMPEG_DEMUXER_BUFFER_TYPE, AMFVariant(AMF_STREAM_VIDEO));
//        UpdateBufferVideoDuration(pBuffer, pPacket, ist);
////        AMFTraceWarning(AMF_FACILITY, L"Video PTS=%5.2f", (double)pBuffer->GetPts() / 10000);

//    }
////    else if (ist->codec->codec_type == AVMEDIA_TYPE_AUDIO)
////    {
////        pBuffer->SetProperty(FFMPEG_DEMUXER_BUFFER_TYPE, AMFVariant(AMF_STREAM_AUDIO));
////        UpdateBufferAudioDuration(pBuffer, pPacket, ist);
//////        AMFTraceWarning(AMF_FACILITY, L"Audio PTS=%5.2f", (double)pBuffer->GetPts() / 10000);
////    }
//    pBuffer->SetProperty(FFMPEG_DEMUXER_BUFFER_STREAM_INDEX, pPacket->stream_index);


//    return AMF_OK;
//}

int amf_decode_frame(AVCodecContext *avctx, void *data,
                       int *got_frame, AVPacket *avpkt)
{
//    AVAMFDecoderContext *ctx = avctx->priv_data;

//    AMFBufferPtr buf;
//    AMF_RESULT err = BufferFromPacket(ctx->context, avpkt, &buf);
//    AMF_RETURN_IF_FALSE(err == AMF_OK, err, L"Cannot convert AVPacket to AMFbuffer");
//    AVFrame *frame    = data;

//    while(!*got_frame)
//    {
//        submit_input
//        if (ok)
//        {
//            query_output
//            return;
//        }


//        //amf_pts start_time = amf_high_precision_clock();
//        //buf->SetProperty(START_TIME_PROPERTY, start_time);

//        res = ctx->decoder->SubmitInput(buf);
////        if(res == AMF_NEED_MORE_INPUT)
////        {
////            gotframe 0 return
////            continue;
////        }
//        else if(res == AMF_INPUT_FULL || res == AMF_DECODER_NO_FREE_SURFACES)
//        { // queue is full; sleep, try to get ready surfaces and repeat submission
//            amf_sleep(1);
//        }
//        else//res == AMF_OK
//        {
//            res = ff_amf_receive_frame(avctx, frame);
//            if (res == AMF_EOF || data == NULL)
//            {
//                break;// end of file
//            }
//            else if (res == AMF_NEED_MORE_INPUT)
//            {

//            }
//        }
//    }
//    //if empty packet
//    //res = ctx->decoder->Drain();
//    return res;
    return 0;
}

static void amf_decode_flush(AVCodecContext *avctx)
{

}

AVCodec ff_amf_decoder = {
    .name           = "amf",
//    .long_name      = NULL_IF_CONFIG_SMALL("AMD AMF decoder"),
    .long_name      = "AMD AMF decoder",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .init           = ff_amf_decode_init,
    .decode         = amf_decode_frame,
    .flush          = amf_decode_flush,
    .close          = ff_amf_decode_close,
    .bsfs           = "bsf_name", //TODO: real vcalue
    .capabilities   = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | //TODO: real vcalue
                      AV_CODEC_CAP_AVOID_PROBING,
    .wrapper_name   = "amf",
};
