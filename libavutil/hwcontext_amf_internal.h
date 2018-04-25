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

#ifndef AVUTIL_HWCONTEXT_AMF_INTERNAL_H
#define AVUTIL_HWCONTEXT_AMF_INTERNAL_H

#include "frame.h"
#include "AMF/core/Context.h"
#include "AMF/core/Factory.h"
#include "AMF/core/Surface.h"


static AMFBuffer* amf_create_buffer_with_frame_ref(const AVFrame* frame, AMFContext *context)
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
    AVFrame *av_frame_ref;
    memcpy(&av_frame_ref, frame_ref_storage_buffer->pVtbl->GetNative(frame_ref_storage_buffer), sizeof(av_frame_ref));
    av_frame_free(&av_frame_ref);
    frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
} 


#define AMFAV_RETURN_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
    if (!(exp)) { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        return ret_value; \
    }

#define AMF_AV_QUERY_INTERFACE(res, from, InterfaceTypeTo, to) \
    { \
        AMFGuid guid_##InterfaceTypeTo = IID_##InterfaceTypeTo(); \
        res = from->pVtbl->QueryInterface(from, &guid_##InterfaceTypeTo, (void**)&to); \
    }

#define AMFAV_ASSIGN_PROPERTY_INTERFACE(res, pThis, name, val) \
    { \
        AMFInterface *amf_interface; \
        AMFVariantStruct var; \
        res = AMFVariantInit(&var); \
        if (res != AMF_OK) \
            return res; \
        if (res == AMF_OK) { \
            AMF_AV_QUERY_INTERFACE(res, val, AMFInterface, amf_interface)\
        } \
        if (res == AMF_OK) { \
            res = AMFVariantAssignInterface(&var, amf_interface); \
            amf_interface->pVtbl->Release(amf_interface); \
        } \
        if (res == AMF_OK) { \
            res = pThis->pVtbl->SetProperty(pThis, name, var); \
        } \
        AMFVariantClear(&var); \
    }

#define AMFAV_GET_PROPERTY_INTERFACE(res, pThis, name, TargetType, val) \
    { \
        AMFVariantStruct var; \
        res = AMFVariantInit(&var); \
        if (res != AMF_OK) \
            return res; \
        res = pThis->pVtbl->GetProperty(pThis, name, &var); \
        if (res == AMF_OK) { \
            if (var.type == AMF_VARIANT_INTERFACE && AMFVariantInterface(&var)) { \
                AMF_AV_QUERY_INTERFACE(res, AMFVariantInterface(&var), TargetType, val); \
            } else { \
                res = AMF_INVALID_DATA_TYPE; \
            } \
        } \
        AMFVariantClear(&var); \
    }


#endif /* AVUTIL_HWCONTEXT_AMF_INTERNAL_H */
