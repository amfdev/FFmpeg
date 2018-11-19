#ifndef AVCODEC_AMFDEC_H
#define AVCODEC_AMFDEC_H
#include "avcodec.h"
#include "internal.h"

#include "../libavutil/frame.h"//change path
#include <AMF/core/Buffer.h>
#include <AMF/components/Component.h>
#include <AMF/core/Surface.h>
#include "avcodec.h"


/**
* AMF encoder context
*/

typedef struct AVAMFDecoderContext {
    AVClass            *avclass;

    AMFContext         *context;
    AMFFactory         *factory;
    AVBufferRef        *amf_device_ctx;

    //encoder
    AMFComponent       *decoder; ///< AMF decoder object
    AMF_SURFACE_FORMAT  format;  ///< AMF surface format

    AVBufferRef        *hw_device_ctx; ///< pointer to HW accelerator (decoder)
    AVBufferRef        *hw_frames_ctx; ///< pointer to HW accelerator (frame allocator)

    // helpers to handle async calls
    int                 delayed_drain;
    AMFSurface         *delayed_surface;
    AVFrame            *delayed_frame;

    // shift dts back by max_b_frames in timing
    AVFifoBuffer       *timestamp_list;
    int64_t             dts_delay;

    // common encoder option options

    int                 log_to_dbg;

} AVAMFDecoderContext;

/**
* Common encoder initization function
*/
int ff_amf_decode_init(AVCodecContext *avctx);
/**
* Common encoder termination function
*/
int ff_amf_decode_close(AVCodecContext *avctx);

/**
* Ecoding one frame - common function for all AMF encoders
*/

#ifndef AMF_RETURN_IF_FALSE
/**
* Error handling helper
*/
#define AMF_RETURN_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
    if (!(exp)) { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        return ret_value; \
    }
#endif

#define AMFAV_GOTO_FAIL_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
    if (!(exp)) { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        ret = ret_value; \
        goto fail; \
    }

static int ff_amf_send_packet(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt);


#endif // AVCODEC_AMFDEC_H
