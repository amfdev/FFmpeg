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

#include <string.h>

#include "config.h"

#include "avassert.h"
#include "avstring.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "hwcontext_amf.h"

#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif

#if CONFIG_DXVA2
#define COBJMACROS
#include "libavutil/hwcontext_dxva2.h"
#endif

#ifdef _WIN32
#include "compat/w32dlfcn.h"
#else
#include <dlfcn.h>
#endif

/**
* Error handling helper
*/
#define AMFAV_RETURN_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
    if (!(exp)) { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        return ret_value; \
    }

typedef struct AmfTraceWriter {
    AMFTraceWriterVtbl  *vtbl;
    void                *avcl;
} AmfTraceWriter;

static void AMF_CDECL_CALL AMFTraceWriter_Write(AMFTraceWriter *pThis,
    const wchar_t *scope, const wchar_t *message)
{
    AmfTraceWriter *tracer = (AmfTraceWriter*)pThis;
    av_log(tracer->avcl, AV_LOG_DEBUG, "%ls: %ls", scope, message);
}

static void AMF_CDECL_CALL AMFTraceWriter_Flush(AMFTraceWriter *pThis)
{
}

static AMFTraceWriterVtbl tracer_vtbl =
{
    .Write = AMFTraceWriter_Write,
    .Flush = AMFTraceWriter_Flush,
};

#define AMFAV_WRITER_ID L"avlog"

static const AVClass amflib_class = {
    .class_name = "amf",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
};

typedef struct AMFLibrary {
    const AVClass      *avclass;
    amf_handle          library;
    AMFFactory         *factory;
    AMFDebug           *debug;
    AMFTrace           *trace;

    amf_uint64          version;
    AmfTraceWriter      tracer;

} AMFLibrary;

static void amf_library_ctx_free(void *opaque, uint8_t *data)
{
    AMFLibrary *ctx = (AMFLibrary*)data;

    if (ctx->trace) {
        ctx->trace->pVtbl->UnregisterWriter(ctx->trace, AMFAV_WRITER_ID);
    }
    if (ctx->library) {
        dlclose(ctx->library);
    }

    av_freep(&ctx);
}

static AVBufferRef *amf_library_ctx_alloc(void)
{
    AVBufferRef *buf;
    AMFLibrary  *ctx;

    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    buf = av_buffer_create((uint8_t*)ctx, sizeof(*ctx), amf_library_ctx_free, NULL, AV_BUFFER_FLAG_READONLY);
    return buf;
}

static int amf_init_library(AMFLibrary *ctx)
{
    AMFInit_Fn              init_fun = NULL;
    AMFQueryVersion_Fn      version_fun = NULL;
    AMF_RESULT              res = AMF_OK;

    ctx->avclass = &amflib_class;
    ctx->library = dlopen(AMF_DLL_NAMEA, RTLD_NOW | RTLD_LOCAL);
    AMFAV_RETURN_IF_FALSE(ctx, ctx->library != NULL, AVERROR_UNKNOWN, "DLL %s failed to open\n", AMF_DLL_NAMEA);

    init_fun = (AMFInit_Fn)dlsym(ctx->library, AMF_INIT_FUNCTION_NAME);
    AMFAV_RETURN_IF_FALSE(ctx, init_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_INIT_FUNCTION_NAME);

    version_fun = (AMFQueryVersion_Fn)dlsym(ctx->library, AMF_QUERY_VERSION_FUNCTION_NAME);
    AMFAV_RETURN_IF_FALSE(ctx, version_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_QUERY_VERSION_FUNCTION_NAME);

    res = version_fun(&ctx->version);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_QUERY_VERSION_FUNCTION_NAME, res);
    res = init_fun(AMF_FULL_VERSION, &ctx->factory);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_INIT_FUNCTION_NAME, res);
    res = ctx->factory->pVtbl->GetTrace(ctx->factory, &ctx->trace);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetTrace() failed with error %d\n", res);
    res = ctx->factory->pVtbl->GetDebug(ctx->factory, &ctx->debug);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetDebug() failed with error %d\n", res);

    ctx->trace->pVtbl->EnableWriter(ctx->trace, AMF_TRACE_WRITER_CONSOLE, 0);
    ctx->trace->pVtbl->SetGlobalLevel(ctx->trace, AMF_TRACE_TRACE);

    // connect AMF logger to av_log
    ctx->tracer.vtbl = &tracer_vtbl;
    ctx->tracer.avcl = ctx;
    ctx->trace->pVtbl->RegisterWriter(ctx->trace, AMFAV_WRITER_ID, (AMFTraceWriter*)&ctx->tracer, 1);
    ctx->trace->pVtbl->SetWriterLevel(ctx->trace, AMFAV_WRITER_ID, AMF_TRACE_TRACE);
    return 0;
}

static AVBufferRef *amf_library_ctx = NULL;
static AVBufferRef *aquire_amf_library_ctx(void)
{
    AVBufferRef *ret = NULL;
    if (amf_library_ctx == NULL)
    {
        ret = amf_library_ctx_alloc();
        if(amf_init_library((AMFLibrary *)ret->data) < 0)
        {
            av_buffer_unref(&amf_library_ctx);
            ret = NULL;
        } else {
            amf_library_ctx = ret;
        }
    } else {
        ret = av_buffer_ref(amf_library_ctx);
    }
    return ret;
}

typedef struct AVAMFDeviceContextPrivate {
    AMFLibrary *lib;
    AVBufferRef *lib_ref;
} AVAMFDeviceContextPrivate;

static int amf_init_device_ctx_object(AVHWDeviceContext *ctx)
{
    AVAMFDeviceContext *hwctx = ctx->hwctx;
    AVAMFDeviceContextPrivate *priv = ctx->internal->priv;
    AMF_RESULT res;

    priv->lib_ref = aquire_amf_library_ctx();
    priv->lib = (AMFLibrary*)priv->lib_ref->data;

    res = priv->lib->factory->pVtbl->CreateContext(priv->lib->factory, &hwctx->context);
    hwctx->factory = priv->lib->factory;
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "CreateContext() failed with error %d\n", res);
    return 0;
}

static int amf_device_create(AVHWDeviceContext *ctx, const char *device,
                                AVDictionary *opts, int flags)
{
    AVAMFDeviceContext *amf_ctx = ctx->hwctx;
    AMF_RESULT res;
    int err;

    err = amf_init_device_ctx_object(ctx);
    if(err < 0)
        return err;

    res = amf_ctx->context->pVtbl->InitDX11(amf_ctx->context, NULL, AMF_DX11_1);
    if (res == AMF_OK) {
        av_log(ctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D11.\n");
    } else {
        res = amf_ctx->context->pVtbl->InitDX9(amf_ctx->context, NULL);
        if (res == AMF_OK) {
            av_log(ctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D9.\n");
        } else {
            av_log(ctx, AV_LOG_ERROR, "AMF initialisation failed via D3D9: error %d.\n", res);
            return AVERROR(ENOSYS);
        }
    }
    return 0;
}

static int amf_device_derive(AVHWDeviceContext *dst_ctx,
                                AVHWDeviceContext *src_ctx,
                                int flags)
{
    AVAMFDeviceContext *amf_ctx = dst_ctx->hwctx;
    AMF_RESULT res;
    int err;

    err = amf_init_device_ctx_object(dst_ctx);
    if(err < 0)
        return err;

    switch (src_ctx->type) {

#if CONFIG_D3D11VA
    case AV_HWDEVICE_TYPE_DXVA2:
        {
            AVDXVA2DeviceContext *dxva2_ctx = src_ctx->hwctx;
            HANDLE device_handle;
            IDirect3DDevice9 *device;
            HRESULT hr;
            AMF_RESULT res;
            int ret;

            hr = IDirect3DDeviceManager9_OpenDeviceHandle(dxva2_ctx->devmgr, &device_handle);
            if (FAILED(hr)) {
                av_log(dst_ctx, AV_LOG_ERROR, "Failed to open device handle for Direct3D9 device: %lx.\n", (unsigned long)hr);
                return AVERROR_EXTERNAL;
            }

            hr = IDirect3DDeviceManager9_LockDevice(dxva2_ctx->devmgr, device_handle, &device, FALSE);
            if (SUCCEEDED(hr)) {
                IDirect3DDeviceManager9_UnlockDevice(dxva2_ctx->devmgr, device_handle, FALSE);
                ret = 0;
            } else {
                av_log(dst_ctx, AV_LOG_ERROR, "Failed to lock device handle for Direct3D9 device: %lx.\n", (unsigned long)hr);
                ret = AVERROR_EXTERNAL;
            }

            IDirect3DDeviceManager9_CloseDeviceHandle(dxva2_ctx->devmgr, device_handle);

            if (ret < 0)
                return ret;

            res = amf_ctx->context->pVtbl->InitDX9(amf_ctx->context, device);

            IDirect3DDevice9_Release(device);

            if (res != AMF_OK) {
                if (res == AMF_NOT_SUPPORTED)
                    av_log(dst_ctx, AV_LOG_ERROR, "AMF via D3D9 is not supported on the given device.\n");
                else
                    av_log(dst_ctx, AV_LOG_ERROR, "AMF failed to initialise on given D3D9 device: %d.\n", res);
                return AVERROR(ENODEV);
            }
        }
        break;
#endif

#if CONFIG_D3D11VA
    case AV_HWDEVICE_TYPE_D3D11VA:
        {
            AVD3D11VADeviceContext *d3d11_ctx = src_ctx->hwctx;
            res = amf_ctx->context->pVtbl->InitDX11(amf_ctx->context, d3d11_ctx->device, AMF_DX11_1);
            if (res != AMF_OK) {
                if (res == AMF_NOT_SUPPORTED)
                    av_log(dst_ctx, AV_LOG_ERROR, "AMF via D3D11 is not supported on the given device.\n");
                else
                    av_log(dst_ctx, AV_LOG_ERROR, "AMF failed to initialise on the given D3D11 device: %d.\n", res);
                return AVERROR(ENODEV);
            }
        }
        break;
#endif
    default:
        av_log(dst_ctx, AV_LOG_ERROR, "AMF initialisation from a %s device is not supported.\n",
                av_hwdevice_get_type_name(src_ctx->type));
        return AVERROR(ENOSYS);
    }
    return 0;
}

static void amf_device_uninit(AVHWDeviceContext *ctx)
{
    AVAMFDeviceContext *amf_ctx = ctx->hwctx;
    AVAMFDeviceContextPrivate *priv = ctx->internal->priv;
    if (amf_ctx->context) {
        amf_ctx->context->pVtbl->Terminate(amf_ctx->context);
        amf_ctx->context->pVtbl->Release(amf_ctx->context);
        amf_ctx->context = NULL;
    }
    av_buffer_unref(&priv->lib_ref);
}

const HWContextType ff_hwcontext_type_amf = {
    .type                   = AV_HWDEVICE_TYPE_AMF,
    .name                   = "AMF",

    .device_hwctx_size      = sizeof(AVAMFDeviceContext),
    .device_priv_size       = sizeof(AVAMFDeviceContextPrivate),

    .device_create          = &amf_device_create,
    .device_derive          = &amf_device_derive,
    .device_uninit          = &amf_device_uninit,
};
