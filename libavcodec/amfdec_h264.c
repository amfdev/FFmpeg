#include "avcodec.h"
#include "internal.h"
#include "amfdec.h"
#include "../libavutil/internal.h"//change path
#include "../libavutil/opt.h"


static av_cold int amf_decode_init_h264(AVCodecContext *avctx)
{
//    int                 ret = 0;
//    AMF_RESULT          res = AMF_OK;
//    AmfContext         *ctx = avctx->priv_data;
//    AMFVariantStruct    var = {0};
//    amf_int64           profile = 0;
//    amf_int64           profile_level = 0;
//    AMFBuffer          *buffer;
//    AMFGuid             guid;
//    AMFRate             framerate;
//    AMFSize             framesize = AMFConstructSize(avctx->width, avctx->height);
//    int                 deblocking_filter = (avctx->flags & AV_CODEC_FLAG_LOOP_FILTER) ? 1 : 0;

//    if (avctx->framerate.num > 0 && avctx->framerate.den > 0) {
//        framerate = AMFConstructRate(avctx->framerate.num, avctx->framerate.den);
//    } else {
//        framerate = AMFConstructRate(avctx->time_base.den, avctx->time_base.num * avctx->ticks_per_frame);
//    }

//    if ((ret = ff_amf_encode_init(avctx)) < 0)
//        return ret;

//    // init static parameters
//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_USAGE, ctx->usage);

//    AMF_ASSIGN_PROPERTY_SIZE(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, framesize);

//    AMF_ASSIGN_PROPERTY_RATE(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_FRAMERATE, framerate);

//    switch (avctx->profile) {
//    case FF_PROFILE_HEVC_MAIN:
//        profile = AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN;
//        break;
//    default:
//        break;
//    }
//    if (profile == 0) {
//        profile = ctx->profile;
//    }
//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_PROFILE, profile);

//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_TIER, ctx->tier);

//    profile_level = avctx->level;
//    if (profile_level == 0) {
//        profile_level = ctx->level;
//    }
//    if (profile_level != 0) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_PROFILE_LEVEL, profile_level);
//    }
//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, ctx->quality);
//    // Maximum Reference Frames
//    if (avctx->refs != 0) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MAX_NUM_REFRAMES, avctx->refs);
//    }
//    // Aspect Ratio
//    if (avctx->sample_aspect_ratio.den && avctx->sample_aspect_ratio.num) {
//        AMFRatio ratio = AMFConstructRatio(avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
//        AMF_ASSIGN_PROPERTY_RATIO(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_ASPECT_RATIO, ratio);
//    }

//    // Picture control properties
//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, ctx->gops_per_idr);
//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, avctx->gop_size);
//    if (avctx->slices > 1) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME, avctx->slices);
//    }
//    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_DE_BLOCKING_FILTER_DISABLE, deblocking_filter);
//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, ctx->header_insertion_mode);

//    // Rate control
//    // autodetect rate control method
//    if (ctx->rate_control_mode == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_UNKNOWN) {
//        if (ctx->min_qp_i != -1 || ctx->max_qp_i != -1 ||
//            ctx->min_qp_p != -1 || ctx->max_qp_p != -1 ||
//            ctx->qp_i !=-1 || ctx->qp_p != -1) {
//            ctx->rate_control_mode = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP;
//            av_log(ctx, AV_LOG_DEBUG, "Rate control turned to CQP\n");
//        } else if (avctx->rc_max_rate > 0) {
//            ctx->rate_control_mode = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;
//            av_log(ctx, AV_LOG_DEBUG, "Rate control turned to Peak VBR\n");
//        } else {
//            ctx->rate_control_mode = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR;
//            av_log(ctx, AV_LOG_DEBUG, "Rate control turned to CBR\n");
//        }
//    }


//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, ctx->rate_control_mode);
//    if (avctx->rc_buffer_size) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, avctx->rc_buffer_size);

//        if (avctx->rc_initial_buffer_occupancy != 0) {
//            int amf_buffer_fullness = avctx->rc_initial_buffer_occupancy * 64 / avctx->rc_buffer_size;
//            if (amf_buffer_fullness > 64)
//                amf_buffer_fullness = 64;
//            AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_INITIAL_VBV_BUFFER_FULLNESS, amf_buffer_fullness);
//        }
//    }
//    // Pre-Pass, Pre-Analysis, Two-Pass
//    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_PREANALYSIS_ENABLE, ctx->preanalysis);

//    if (ctx->rate_control_mode == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP) {
//        AMF_ASSIGN_PROPERTY_BOOL(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, false);
//        if (ctx->enable_vbaq)
//            av_log(ctx, AV_LOG_WARNING, "VBAQ is not supported by cqp Rate Control Method, automatically disabled\n");
//    } else {
//        AMF_ASSIGN_PROPERTY_BOOL(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, !!ctx->enable_vbaq);
//    }
//    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MOTION_HALF_PIXEL, ctx->me_half_pel);
//    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MOTION_QUARTERPIXEL, ctx->me_quarter_pel);

//    // init dynamic rate control params
//    if (ctx->max_au_size)
//        ctx->enforce_hrd = 1;
//    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, ctx->enforce_hrd);
//    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, ctx->filler_data);

//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, avctx->bit_rate);

//    if (ctx->rate_control_mode == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, avctx->bit_rate);
//    }
//    if (avctx->rc_max_rate) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, avctx->rc_max_rate);
//    } else if (ctx->rate_control_mode == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR) {
//        av_log(ctx, AV_LOG_WARNING, "rate control mode is PEAK_CONSTRAINED_VBR but rc_max_rate is not set\n");
//    }

//    // init encoder
//    res = ctx->encoder->pVtbl->Init(ctx->encoder, ctx->format, avctx->width, avctx->height);
//    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_BUG, "encoder->Init() failed with error %d\n", res);

//    // init dynamic picture control params
//    AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MAX_AU_SIZE, ctx->max_au_size);

//    if (ctx->min_qp_i != -1) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MIN_QP_I, ctx->min_qp_i);
//    } else if (avctx->qmin != -1) {
//        int qval = avctx->qmin > 51 ? 51 : avctx->qmin;
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MIN_QP_I, qval);
//    }
//    if (ctx->max_qp_i != -1) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MAX_QP_I, ctx->max_qp_i);
//    } else if (avctx->qmax != -1) {
//        int qval = avctx->qmax > 51 ? 51 : avctx->qmax;
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MAX_QP_I, qval);
//    }
//    if (ctx->min_qp_p != -1) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MIN_QP_P, ctx->min_qp_p);
//    } else if (avctx->qmin != -1) {
//        int qval = avctx->qmin > 51 ? 51 : avctx->qmin;
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MIN_QP_P, qval);
//    }
//    if (ctx->max_qp_p != -1) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MAX_QP_P, ctx->max_qp_p);
//    } else if (avctx->qmax != -1) {
//        int qval = avctx->qmax > 51 ? 51 : avctx->qmax;
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_MAX_QP_P, qval);
//    }

//    if (ctx->qp_p != -1) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_QP_I, ctx->qp_p);
//    }
//    if (ctx->qp_i != -1) {
//        AMF_ASSIGN_PROPERTY_INT64(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_QP_P, ctx->qp_i);
//    }
//    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_SKIP_FRAME_ENABLE, ctx->skip_frame);


//    // fill extradata
//    res = AMFVariantInit(&var);
//    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_BUG, "AMFVariantInit() failed with error %d\n", res);

//    res = ctx->encoder->pVtbl->GetProperty(ctx->encoder, AMF_VIDEO_ENCODER_HEVC_EXTRADATA, &var);
//    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_BUG, "GetProperty(AMF_VIDEO_ENCODER_EXTRADATA) failed with error %d\n", res);
//    AMF_RETURN_IF_FALSE(ctx, var.pInterface != NULL, AVERROR_BUG, "GetProperty(AMF_VIDEO_ENCODER_EXTRADATA) returned NULL\n");

//    guid = IID_AMFBuffer();

//    res = var.pInterface->pVtbl->QueryInterface(var.pInterface, &guid, (void**)&buffer); // query for buffer interface
//    if (res != AMF_OK) {
//        var.pInterface->pVtbl->Release(var.pInterface);
//    }
//    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_BUG, "QueryInterface(IID_AMFBuffer) failed with error %d\n", res);

//    avctx->extradata_size = (int)buffer->pVtbl->GetSize(buffer);
//    avctx->extradata = av_mallocz(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
//    if (!avctx->extradata) {
//        buffer->pVtbl->Release(buffer);
//        var.pInterface->pVtbl->Release(var.pInterface);
//        return AVERROR(ENOMEM);
//    }
//    memcpy(avctx->extradata, buffer->pVtbl->GetNative(buffer), avctx->extradata_size);

//    buffer->pVtbl->Release(buffer);
//    var.pInterface->pVtbl->Release(var.pInterface);

    return 0;
}


AVCodec ff_h264_amf_decoder = {
    .name           = "h264_amf",
    .long_name      = NULL_IF_CONFIG_SMALL("AMD AMF H264 encoder"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .init           = amf_decode_init_h264,
    .receive_frame  = ff_amf_receive_frame,
    .close          = ff_amf_decode_close,
    .bsfs           = "bsf_name", //TODO: real vcalue
    .capabilities   = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | //TODO: real vcalue
                      AV_CODEC_CAP_AVOID_PROBING,
    .wrapper_name   = "amf",
};
