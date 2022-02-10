/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "SPRDAV1Decoder"
#include <utils/Log.h>

#include "SPRDAV1Decoder.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include "Rect.h"

#include <dlfcn.h>
#include <media/hardware/HardwareAPI.h>
#include <ui/GraphicBufferMapper.h>
#include <sys/prctl.h>
#include <cutils/properties.h>

#include "gralloc_public.h"
#include "sprd_ion.h"
#include "av1_dec_api.h"

namespace android {

#define MAX_INSTANCES 10

static int instances = 0;

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

SPRDAV1Decoder::SPRDAV1Decoder(
    const char *name,
    const OMX_CALLBACKTYPE *callbacks,
    OMX_PTR appData,
    OMX_COMPONENTTYPE **component)
    : SprdSimpleOMXComponent(name, callbacks, appData, component),
      mHandle(new tagAV1Handle),
      mInputBufferCount(0),

      mFrameWidth(320),
      mFrameHeight(240),

      mStride(mFrameWidth),
      mSliceHeight(mFrameHeight),
      mPictureSize(mStride * mSliceHeight * 3 / 2),
      mCropWidth(mFrameWidth),
      mCropHeight(mFrameHeight),
      mEOSStatus(INPUT_DATA_AVAILABLE),
      mOutputPortSettingsChange(NONE),

      mSignalledError(false),

      mAllocateBuffers(false),
      mNeedIVOP(true),

      mDumpYUVEnabled(false),
      mDumpStrmEnabled(false),
      mStopDecode(false),
      mThumbnailMode(OMX_FALSE),

      mPbuf_stream_v(NULL),

      mPbuf_stream_size(0),
      mBindcnt(0),
      mLibHandle(NULL),
      mAV1DecInit(NULL),
      mAV1DecGetInfo(NULL),
      mAV1DecDecode(NULL),
      mAV1DecRelease(NULL),
      mAV1Dec_SetCurRecPic(NULL),
      mAV1Dec_GetLastDspFrm(NULL),
      mAV1Dec_ReleaseRefBuffers(NULL),
      mAV1GetCodecCapability(NULL),

      mFrameDecoded(false),
      mIsInterlacedSequence(false) {
    ALOGI("Construct SPRDAV1Decoder, this: %p, instances: %d", (void *)this, instances);

    mInitCheck = OMX_ErrorNone;


    bool ret = false;

    ret = openDecoder("libomx_av1dec_sw_sprd.so");

    CHECK_EQ(ret, true);

    char value_dump[PROPERTY_VALUE_MAX];

    property_get("vendor.av1dec.yuv.dump", value_dump, "false");
    mDumpYUVEnabled = !strcmp(value_dump, "true");

    property_get("vendor.av1dec.strm.dump", value_dump, "false");
    mDumpStrmEnabled = !strcmp(value_dump, "true");
    ALOGI("%s, mDumpYUVEnabled: %d, mDumpStrmEnabled: %d", __FUNCTION__, mDumpYUVEnabled, mDumpStrmEnabled);


    CHECK_EQ(initDecoder(), (status_t)OK);

    initPorts();

    iUseAndroidNativeBuffer[OMX_DirInput] = OMX_FALSE;
    iUseAndroidNativeBuffer[OMX_DirOutput] = OMX_FALSE;

    instances++;
    if (instances > MAX_INSTANCES) {
        ALOGE("instances(%d) are too much, return OMX_ErrorInsufficientResources", instances);
        mInitCheck = OMX_ErrorInsufficientResources;
    }

    int64_t start_decode = systemTime();

    if(mDumpYUVEnabled) {
        char s1[100];
        sprintf(s1,"/data/misc/media/video_out_%p_%lld.yuv",(void *)this,start_decode);
        mFile_yuv = fopen(s1, "wb");
        ALOGI("yuv file name %s",s1);
    }

    if(mDumpStrmEnabled) {
        char s2[100];
        uint8 ivf_header[32] = {0x44,0x4b,0x49,0x46,0x00,0x00,0x20,0x00,0x41,0x56,0x30,0x31,0x80,0x02,0x68,0x01,
                                0x00,0x3c,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x10,0x0e,0x00,0x00,0x00,0x00,0x00};
        sprintf(s2,"/data/misc/media/video_es_%p_%lld.ivf",(void *)this,start_decode);
        ALOGI("bs file name %s",s2);
        mFile_bs = fopen(s2, "wb");
        fwrite(ivf_header,1,32,mFile_bs);
    }
}

SPRDAV1Decoder::~SPRDAV1Decoder() {
    ALOGI("Destruct SPRDAV1Decoder, this: %p, instances: %d", (void *)this, instances);

    releaseDecoder();

    delete mHandle;
    mHandle = NULL;

    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    CHECK(outQueue.empty());
    CHECK(inQueue.empty());

    if(mDumpYUVEnabled) {
        if (mFile_yuv) {
            fclose(mFile_yuv);
            mFile_yuv = NULL;
        }
    }

    if(mDumpStrmEnabled) {
        if (mFile_bs) {
            fclose(mFile_bs);
            mFile_bs = NULL;
        }
    }
    instances--;
}

OMX_ERRORTYPE SPRDAV1Decoder::initCheck() const {
    ALOGI("%s, mInitCheck: 0x%x", __FUNCTION__, mInitCheck);
    return mInitCheck;
}

OMX_ERRORTYPE SPRDAV1Decoder::allocateStreamBuffer(OMX_U32 bufferSize)
{
    unsigned long phy_addr = 0;
    size_t size = 0;


    mPbuf_stream_v = (uint8_t*)malloc(bufferSize * sizeof(unsigned char));

    mPbuf_stream_size = bufferSize;


    ALOGI("%s,  %p - %zd", __FUNCTION__, mPbuf_stream_v, mPbuf_stream_size);

    return OMX_ErrorNone;
}

void SPRDAV1Decoder::releaseStreamBuffer()
{
    ALOGI("%s, %p - %zd", __FUNCTION__, mPbuf_stream_v, mPbuf_stream_size);

    if (mPbuf_stream_v != NULL) {
        free(mPbuf_stream_v);
        mPbuf_stream_v = NULL;
    }
}

void SPRDAV1Decoder::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = kInputPortIndex;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumInputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 1920*1088*3/2/2;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    def.format.video.cMIMEType = const_cast<char *>("video/av01");
    def.format.video.pNativeRender = NULL;
    def.format.video.nFrameWidth = mFrameWidth;
    def.format.video.nFrameHeight = mFrameHeight;
    def.format.video.nStride = def.format.video.nFrameWidth;
    def.format.video.nSliceHeight = def.format.video.nFrameHeight;
    def.format.video.nBitrate = 0;
    def.format.video.xFramerate = 0;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.format.video.eCompressionFormat = OMX_VIDEO_CodingAV1;
    def.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    def.format.video.pNativeWindow = NULL;

    addPort(def);

    def.nPortIndex = kOutputPortIndex;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = kNumOutputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.video.cMIMEType = const_cast<char *>("video/raw");
    def.format.video.pNativeRender = NULL;
    def.format.video.nFrameWidth = mFrameWidth;
    def.format.video.nFrameHeight = mFrameHeight;
    def.format.video.nStride = def.format.video.nFrameWidth;
    def.format.video.nSliceHeight = def.format.video.nFrameHeight;
    def.format.video.nBitrate = 0;
    def.format.video.xFramerate = 0;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    def.format.video.eColorFormat = OMX_COLOR_FormatYUV420Planar;
    def.format.video.pNativeWindow = NULL;

    def.nBufferSize =
        (def.format.video.nFrameWidth * def.format.video.nFrameHeight * 3) / 2;

    addPort(def);
}

status_t SPRDAV1Decoder::initDecoder() {

    memset(mHandle, 0, sizeof(tagAV1Handle));

    mHandle->userdata = (void *)this;
    mHandle->VSP_bindCb = BindFrameWrapper;
    mHandle->VSP_unbindCb = UnbindFrameWrapper;
    mHandle->framethreads = 4;
    mHandle->tilethreads = 1;

    if ((*mAV1DecInit)(mHandle) != MMDEC_OK) {
        ALOGE("Failed to init AV1DEC");
        return OMX_ErrorUndefined;
    }

    //int32 codec_capabilty;
    if ((*mAV1GetCodecCapability)(mHandle, &mCapability) != MMDEC_OK) {
        ALOGE("Failed to mAV1GetCodecCapability");
    }

    ALOGI("initDecoder, Capability: max wh=%d %d", mCapability.max_width, mCapability.max_height);


    return allocateStreamBuffer(AV1_DECODER_STREAM_BUFFER_SIZE);
}

void SPRDAV1Decoder::releaseDecoder() {
    releaseStreamBuffer();

    if( mAV1DecRelease!=NULL )
        (*mAV1DecRelease)(mHandle);

    if(mLibHandle) {
        dlclose(mLibHandle);
        mLibHandle = NULL;
        mAV1Dec_ReleaseRefBuffers = NULL;
        mAV1DecRelease = NULL;
    }
}

OMX_ERRORTYPE SPRDAV1Decoder::internalGetParameter(
    OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
    case OMX_IndexParamVideoPortFormat:
    {
        OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
            (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

        if (formatParams->nPortIndex > kOutputPortIndex) {
            return OMX_ErrorUndefined;
        }

        if (formatParams->nIndex != 0) {
            return OMX_ErrorNoMore;
        }

        if (formatParams->nPortIndex == kInputPortIndex) {
            formatParams->eCompressionFormat = OMX_VIDEO_CodingAV1;
            formatParams->eColorFormat = OMX_COLOR_FormatUnused;
            formatParams->xFramerate = 0;
            ALOGI("internalGetParameter, OMX_IndexParamVideoPortFormat, eCompressionFormat: 0x%x",formatParams->eCompressionFormat);
        } else {
            CHECK(formatParams->nPortIndex == kOutputPortIndex);

            PortInfo *pOutPort = editPortInfo(kOutputPortIndex);
            ALOGI("internalGetParameter, OMX_IndexParamVideoPortFormat, eColorFormat: 0x%x",pOutPort->mDef.format.video.eColorFormat);
            formatParams->eCompressionFormat = OMX_VIDEO_CodingUnused;
            formatParams->eColorFormat = pOutPort->mDef.format.video.eColorFormat;
            formatParams->xFramerate = 0;
        }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamEnableAndroidBuffers:
    {
        EnableAndroidNativeBuffersParams *peanbp = (EnableAndroidNativeBuffersParams *)params;
        peanbp->enable = iUseAndroidNativeBuffer[OMX_DirOutput];
        ALOGI("internalGetParameter, OMX_IndexParamEnableAndroidBuffers %d",peanbp->enable);
        return OMX_ErrorNone;
    }

    case OMX_IndexParamGetAndroidNativeBuffer:
    {
        GetAndroidNativeBufferUsageParams *pganbp;

        pganbp = (GetAndroidNativeBufferUsageParams *)params;
        pganbp->nUsage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
        ALOGI("internalGetParameter, OMX_IndexParamGetAndroidNativeBuffer 0x%x",pganbp->nUsage);
        return OMX_ErrorNone;
    }

    case OMX_IndexParamPortDefinition:
    {
        OMX_PARAM_PORTDEFINITIONTYPE *defParams =
            (OMX_PARAM_PORTDEFINITIONTYPE *)params;

        if (defParams->nPortIndex > 1
                || defParams->nSize
                != sizeof(OMX_PARAM_PORTDEFINITIONTYPE)) {
            return OMX_ErrorUndefined;
        }

        PortInfo *port = editPortInfo(defParams->nPortIndex);

        {
            Mutex::Autolock autoLock(mLock);
            memcpy(defParams, &port->mDef, sizeof(port->mDef));
        }
        return OMX_ErrorNone;
    }

    default:
        return OMX_ErrorUnsupportedIndex;
    }
}

OMX_ERRORTYPE SPRDAV1Decoder::internalSetParameter(
    OMX_INDEXTYPE index, const OMX_PTR params) {
    switch (index) {
    case OMX_IndexParamStandardComponentRole:
    {
        const OMX_PARAM_COMPONENTROLETYPE *roleParams =
            (const OMX_PARAM_COMPONENTROLETYPE *)params;

        if (strncmp((const char *)roleParams->cRole,
                    "video_decoder.av1",
                    OMX_MAX_STRINGNAME_SIZE - 1)) {
            return OMX_ErrorUndefined;
        }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamVideoPortFormat:
    {
        OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
            (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

        if (formatParams->nPortIndex > kOutputPortIndex) {
            return OMX_ErrorUndefined;
        }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamEnableAndroidBuffers:
    {
        EnableAndroidNativeBuffersParams *peanbp = (EnableAndroidNativeBuffersParams *)params;
        PortInfo *pOutPort = editPortInfo(kOutputPortIndex);
        if (peanbp->enable == OMX_FALSE) {
            ALOGI("internalSetParameter, disable AndroidNativeBuffer");
            iUseAndroidNativeBuffer[OMX_DirOutput] = OMX_FALSE;

            pOutPort->mDef.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
            /*FIXME: when NativeWindow is null, we can't use nBufferCountMin to calculate to
            * nBufferCountActual in Acodec.cpp&OMXCodec.cpp. So we need set nBufferCountActual
            * manually.
            * 4: reserved buffers by SurfaceFlinger(according to Acodec.cpp&OMXCodec.cpp)*/
            //pOutPort->mDef.nBufferCountActual = pOutPort->mDef.nBufferCountMin + 4;
        } else {
            ALOGI("internalSetParameter, enable AndroidNativeBuffer");
            iUseAndroidNativeBuffer[OMX_DirOutput] = OMX_TRUE;

            pOutPort->mDef.format.video.eColorFormat = (OMX_COLOR_FORMATTYPE)HAL_PIXEL_FORMAT_YCbCr_420_P;
        }
        return OMX_ErrorNone;
    }

    case OMX_IndexParamAllocNativeHandle:
    {
        EnableAndroidNativeBuffersParams *peanbp = (EnableAndroidNativeBuffersParams *)params;
        PortInfo *pOutPort = editPortInfo(kInputPortIndex);
        if (peanbp->enable == OMX_FALSE) {
            ALOGI("internalSetParameter, disable AllocNativeHandle");
            iUseAndroidNativeBuffer[OMX_DirInput] = OMX_FALSE;
        } else {
            ALOGI("internalSetParameter, enable AllocNativeHandle");
            iUseAndroidNativeBuffer[OMX_DirInput] = OMX_TRUE;
        }
        return OMX_ErrorNone;
    }

    case OMX_IndexParamPortDefinition:
    {
        OMX_PARAM_PORTDEFINITIONTYPE *defParams =
            (OMX_PARAM_PORTDEFINITIONTYPE *)params;

        if (defParams->nPortIndex > 1) {
            return OMX_ErrorBadPortIndex;
        }
        if (defParams->nSize != sizeof(OMX_PARAM_PORTDEFINITIONTYPE)) {
            return OMX_ErrorUnsupportedSetting;
        }

        PortInfo *port = editPortInfo(defParams->nPortIndex);

        // default behavior is that we only allow buffer size to increase
        if (defParams->nBufferSize > port->mDef.nBufferSize) {
            port->mDef.nBufferSize = defParams->nBufferSize;
        }

        if (defParams->nBufferCountActual == 1) {
            mThumbnailMode = OMX_TRUE;
        } else {
            if (defParams->nBufferCountActual < port->mDef.nBufferCountMin) {
                ALOGW("component requires at least %u buffers (%u requested)",
                      port->mDef.nBufferCountMin, defParams->nBufferCountActual);
                return OMX_ErrorUnsupportedSetting;
            }
            port->mDef.nBufferCountActual = defParams->nBufferCountActual;
        }

        uint32_t oldWidth = port->mDef.format.video.nFrameWidth;
        uint32_t oldHeight = port->mDef.format.video.nFrameHeight;
        uint32_t newWidth = defParams->format.video.nFrameWidth;
        uint32_t newHeight = defParams->format.video.nFrameHeight;

        ALOGI("%s, port:%d, old wh:%d %d, new wh:%d %d", __FUNCTION__, defParams->nPortIndex,
              oldWidth, oldHeight, newWidth, newHeight);

        memcpy(&port->mDef.format.video, &defParams->format.video, sizeof(OMX_VIDEO_PORTDEFINITIONTYPE));

        if((oldWidth != newWidth || oldHeight != newHeight)) {
            if (defParams->nPortIndex == kOutputPortIndex) {
                mFrameWidth = newWidth;
                mFrameHeight = newHeight;
                mStride = ((newWidth + 15) & ~15);
                mSliceHeight = newHeight;
                mPictureSize = mStride* mSliceHeight * 3 / 2;

                ALOGI("%s, mFrameWidth %d, mFrameHeight %d, mStride %d, mSliceHeight %d", __FUNCTION__,
                      mFrameWidth, mFrameHeight, mStride, mSliceHeight);

                updatePortDefinitions(true, true);
            } else {
                port->mDef.format.video.nFrameWidth = newWidth;
                port->mDef.format.video.nFrameHeight = newHeight;
            }
        }

        PortInfo *inPort = editPortInfo(kInputPortIndex);
        if (inPort->mDef.nBufferSize > mPbuf_stream_size) {
            releaseStreamBuffer();
            allocateStreamBuffer(inPort->mDef.nBufferSize + 4);//additional 4 for startcode
        }

        return OMX_ErrorNone;
    }

    default:
        return SprdSimpleOMXComponent::internalSetParameter(index, params);
    }
}

OMX_ERRORTYPE SPRDAV1Decoder::internalUseBuffer(
    OMX_BUFFERHEADERTYPE **header,
    OMX_U32 portIndex,
    OMX_PTR appPrivate,
    OMX_U32 size,
    OMX_U8 *ptr,
    BufferPrivateStruct* bufferPrivate) {

    *header = new OMX_BUFFERHEADERTYPE;
    (*header)->nSize = sizeof(OMX_BUFFERHEADERTYPE);
    (*header)->nVersion.s.nVersionMajor = 1;
    (*header)->nVersion.s.nVersionMinor = 0;
    (*header)->nVersion.s.nRevision = 0;
    (*header)->nVersion.s.nStep = 0;
    (*header)->pBuffer = ptr;
    (*header)->nAllocLen = size;
    (*header)->nFilledLen = 0;
    (*header)->nOffset = 0;
    (*header)->pAppPrivate = appPrivate;
    (*header)->pPlatformPrivate = NULL;
    (*header)->pInputPortPrivate = NULL;
    (*header)->pOutputPortPrivate = NULL;
    (*header)->hMarkTargetComponent = NULL;
    (*header)->pMarkData = NULL;
    (*header)->nTickCount = 0;
    (*header)->nTimeStamp = 0;
    (*header)->nFlags = 0;
    (*header)->nOutputPortIndex = portIndex;
    (*header)->nInputPortIndex = portIndex;

    if(portIndex == OMX_DirOutput) {
        (*header)->pOutputPortPrivate = new BufferCtrlStruct;
        CHECK((*header)->pOutputPortPrivate != NULL);
        BufferCtrlStruct* pBufCtrl= (BufferCtrlStruct*)((*header)->pOutputPortPrivate);
        pBufCtrl->iRefCount = 1; //init by1
        pBufCtrl->id = 0;
        if(mAllocateBuffers) {
            if(bufferPrivate != NULL) {
                pBufCtrl->pMem = ((BufferPrivateStruct*)bufferPrivate)->pMem;
                pBufCtrl->phyAddr = ((BufferPrivateStruct*)bufferPrivate)->phyAddr;
                pBufCtrl->bufferSize = ((BufferPrivateStruct*)bufferPrivate)->bufferSize;
                pBufCtrl->bufferFd = ((BufferPrivateStruct*)bufferPrivate)->bufferFd;
            } else {
                pBufCtrl->pMem = NULL;
                pBufCtrl->phyAddr = 0;
                pBufCtrl->bufferSize = 0;
                pBufCtrl->bufferFd = 0;
            }
        } else {
            pBufCtrl->pMem = NULL;
            pBufCtrl->bufferFd = 0;
            pBufCtrl->phyAddr = 0;
            pBufCtrl->bufferSize = 0;
        }
    } else if (portIndex == OMX_DirInput) {
        if (mAllocInput) {
            (*header)->pInputPortPrivate = new BufferCtrlStruct;
            CHECK((*header)->pInputPortPrivate != NULL);
            BufferCtrlStruct* pBufCtrl= (BufferCtrlStruct*)((*header)->pInputPortPrivate);
            if(bufferPrivate != NULL) {
                pBufCtrl->pMem = ((BufferPrivateStruct*)bufferPrivate)->pMem;
                pBufCtrl->phyAddr = ((BufferPrivateStruct*)bufferPrivate)->phyAddr;
                pBufCtrl->bufferSize = ((BufferPrivateStruct*)bufferPrivate)->bufferSize;
                pBufCtrl->bufferFd = 0;
            } else {
                pBufCtrl->pMem = NULL;
                pBufCtrl->phyAddr = 0;
                pBufCtrl->bufferSize = 0;
                pBufCtrl->bufferFd = 0;
            }
        }
    }

    PortInfo *port = editPortInfo(portIndex);

    port->mBuffers.push();

    BufferInfo *buffer =
        &port->mBuffers.editItemAt(port->mBuffers.size() - 1);
    ALOGI("internalUseBuffer, portIndex= %d, header=%p, pBuffer=%p, size=%d", portIndex, *header, ptr, size);
    buffer->mHeader = *header;
    buffer->mOwnedByUs = false;

    if (port->mBuffers.size() == port->mDef.nBufferCountActual) {
        port->mDef.bPopulated = OMX_TRUE;
        checkTransitions();
    }

    return OMX_ErrorNone;
}

OMX_ERRORTYPE SPRDAV1Decoder::allocateBuffer(
    OMX_BUFFERHEADERTYPE **header,
    OMX_U32 portIndex,
    OMX_PTR appPrivate,
    OMX_U32 size) {
    switch (portIndex) {
    case OMX_DirInput:
    {
        ALOGI("%s,%d,%d", __FUNCTION__,__LINE__,portIndex);
        return SprdSimpleOMXComponent::allocateBuffer(header, portIndex, appPrivate, size);
    }

    case OMX_DirOutput:
    {
        mAllocateBuffers = true;
        ALOGI("%s,%d,%d", __FUNCTION__,__LINE__,portIndex);
        return SprdSimpleOMXComponent::allocateBuffer(header, portIndex, appPrivate, size);

    }

    default:
        return OMX_ErrorUnsupportedIndex;

    }
}

OMX_ERRORTYPE SPRDAV1Decoder::freeBuffer(
    OMX_U32 portIndex,
    OMX_BUFFERHEADERTYPE *header) {
    switch (portIndex) {
    case OMX_DirInput:
    {
        return SprdSimpleOMXComponent::freeBuffer(portIndex, header);
    }

    case OMX_DirOutput:
    {
        BufferCtrlStruct* pBufCtrl= (BufferCtrlStruct*)(header->pOutputPortPrivate);
        if(pBufCtrl != NULL) {
            if(pBufCtrl->pMem != NULL) {
                pBufCtrl->pMem.clear();
            }
            return SprdSimpleOMXComponent::freeBuffer(portIndex, header);
        } else {
            ALOGE("freeBuffer, pBufCtrl==NULL");
            return OMX_ErrorUndefined;
        }
    }

    default:
        return OMX_ErrorUnsupportedIndex;
    }
}

OMX_ERRORTYPE SPRDAV1Decoder::getConfig(
    OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
    case OMX_IndexConfigCommonOutputCrop:
    {
        OMX_CONFIG_RECTTYPE *rectParams = (OMX_CONFIG_RECTTYPE *)params;

        if (rectParams->nPortIndex != 1) {
            return OMX_ErrorUndefined;
        }

        rectParams->nLeft = 0;
        rectParams->nTop = 0;
        {
            ALOGI("%s, mCropWidth:%d, mCropHeight:%d",
                  __FUNCTION__, mCropWidth, mCropHeight);
            Mutex::Autolock autoLock(mLock);
            rectParams->nWidth = mCropWidth;
            rectParams->nHeight = mCropHeight;
        }
        return OMX_ErrorNone;
    }

    default:
        return OMX_ErrorUnsupportedIndex;
    }
}

OMX_ERRORTYPE SPRDAV1Decoder::setConfig(
    OMX_INDEXTYPE index, const OMX_PTR params) {

    switch (index) {
    case OMX_IndexConfigThumbnailMode:
    {
        OMX_BOOL *pEnable = (OMX_BOOL *)params;

        if (*pEnable == OMX_TRUE) {
            mThumbnailMode = OMX_TRUE;
        }

        ALOGI("setConfig, mThumbnailMode = %d", mThumbnailMode);

        if (mThumbnailMode) {
            PortInfo *pInPort = editPortInfo(OMX_DirInput);
            PortInfo *pOutPort = editPortInfo(OMX_DirOutput);
            pInPort->mDef.nBufferCountActual = 2;
            pOutPort->mDef.nBufferCountActual = 2;

        }
        return OMX_ErrorNone;
    }
    case OMX_IndexConfigDecSceneMode:
    {
        int *pDecSceneMode = (int *)params;

        ALOGI("%s,%d,setConfig, pDecSceneMode = %d",__FUNCTION__,__LINE__, *pDecSceneMode);
        return OMX_ErrorNone;
    }

    default:
        return SprdSimpleOMXComponent::setConfig(index, params);
    }
}

void SPRDAV1Decoder::dump_strm(uint8 *pBuffer, int32 aInBufSize) {
    if(mDumpStrmEnabled) {
        uint32 tmp = 0;
        fwrite(&aInBufSize, 1,4, mFile_bs);
        fwrite(&tmp, 1,4, mFile_bs);
        fwrite(&tmp, 1,4, mFile_bs);
        fwrite(pBuffer,1,aInBufSize,mFile_bs);
    }
}

void SPRDAV1Decoder::dump_yuv(uint8 *pBuffer, int32 aInBufSize) {
    if(mDumpYUVEnabled) {
        fwrite(pBuffer,1,aInBufSize,mFile_yuv);
    }
}

void SPRDAV1Decoder::onQueueFilled(OMX_U32 portIndex) {
    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    if (mEOSStatus == OUTPUT_FRAMES_FLUSHED) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    while (!mStopDecode && (mEOSStatus != INPUT_DATA_AVAILABLE || !inQueue.empty())
            && (outQueue.size() != 0 )) {
        if (mEOSStatus == INPUT_EOS_SEEN) {
            drainAllOutputBuffers();
            return;
        }

        BufferInfo *inInfo = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

        OMX_BUFFERHEADERTYPE *outHeader = NULL;
        List<BufferInfo *>::iterator itBuffer;
        BufferCtrlStruct *pBufCtrl = NULL;
        size_t count = 0;
        uint32_t queueSize = 0;


        itBuffer = outQueue.begin();

        do {

            queueSize = outQueue.size();

            if(mBindcnt > mHandle->framethreads) {
                ALOGE("error bitstream detected, decoder will quit.,%d,%d\n",mBindcnt,mHandle->framethreads);
                notify(OMX_EventError, OMX_ErrorFormatNotDetected, 0, NULL);
                mSignalledError = true;
                return;
            }
            if(count >= queueSize) {
                ALOGI("onQueueFilled, get outQueue buffer, return, count=%zd, queue_size=%d",count, queueSize);
                return;
            }

            outHeader = (*itBuffer)->mHeader;
            pBufCtrl= (BufferCtrlStruct*)(outHeader->pOutputPortPrivate);
            if(pBufCtrl == NULL) {
                ALOGE("onQueueFilled, pBufCtrl == NULL, fail");
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }

            itBuffer++;
            count++;
        } while (pBufCtrl->iRefCount > 0);

        ALOGI("%s, %d, outHeader:%p, inHeader: %p, len: %d, nOffset: %d, time: %lld, EOS: %d",
              __FUNCTION__, __LINE__,outHeader,inHeader, inHeader->nFilledLen,
              inHeader->nOffset, inHeader->nTimeStamp,inHeader->nFlags & OMX_BUFFERFLAG_EOS);


        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            mEOSStatus = INPUT_EOS_SEEN;
        }

        if(inHeader->nFilledLen == 0) {
            inInfo->mOwnedByUs = false;
            inQueue.erase(inQueue.begin());
            inInfo = NULL;
            notifyEmptyBufferDone(inHeader);
            inHeader = NULL;
            continue;
        }

        MMDecInput dec_in = {0};
        MMDecOutput dec_out = {0};

        uint8_t *bitstream = inHeader->pBuffer + inHeader->nOffset;
        uint32_t bufferSize = inHeader->nFilledLen;
        uint32_t copyLen = 0;
        uint32_t add_startcode_len = 0;

        ALOGI("%x,%x,%x,%x\n",bitstream[0],bitstream[1],bitstream[2],bitstream[3]);
        dec_in.pStream = mPbuf_stream_v;


        dec_in.beLastFrm = 0;
        dec_in.expected_IVOP = mNeedIVOP;
        dec_in.beDisplayed = 1;
        dec_in.err_pkt_num = 0;
        dec_in.nTimeStamp = (uint64)(inHeader->nTimeStamp);

        dec_out.frameEffective = 0;



        if (mPbuf_stream_v != NULL) {
            memcpy(mPbuf_stream_v , bitstream, bufferSize);
        }

        dec_in.dataLen = bufferSize;

        ALOGV("%s, %d, dec_in.dataLen: %d\n", __FUNCTION__, __LINE__, dec_in.dataLen);

        outHeader->nTimeStamp = inHeader->nTimeStamp;
        outHeader->nFlags = inHeader->nFlags;

        unsigned long picPhyAddr = 0;

        ALOGV("%s, %d, outHeader: %p, pBuffer: %p, phyAddr: 0x%lx",
              __FUNCTION__, __LINE__, outHeader, outHeader->pBuffer, picPhyAddr);
        GraphicBufferMapper &mapper = GraphicBufferMapper::get();

        if(iUseAndroidNativeBuffer[OMX_DirOutput]) {
            OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(kOutputPortIndex)->mDef;
            int width = def->format.video.nStride;
            int height = def->format.video.nSliceHeight;
            Rect bounds(width, height);
            void *vaddr;
            int usage;
            usage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;

            if(mapper.lock((const native_handle_t*)outHeader->pBuffer, usage, bounds, &vaddr)) {
                ALOGE("onQueueFilled, mapper.lock fail %p",outHeader->pBuffer);
                return ;
            }
            ALOGV("%s, %d, pBuffer: 0x%p, vaddr: %p", __FUNCTION__, __LINE__, outHeader->pBuffer,vaddr);
            uint8_t *yuv = (uint8_t *)((uint8_t *)vaddr + outHeader->nOffset);
            ALOGV("%s, %d, yuv: %p, outHeader: %p, outHeader->pBuffer: %p, outHeader->nTimeStamp: %lld",
                  __FUNCTION__, __LINE__, yuv, outHeader, outHeader->pBuffer, outHeader->nTimeStamp);
            (*mAV1Dec_SetCurRecPic)(mHandle, yuv, (uint8 *)picPhyAddr, (void *)outHeader);
        } else {
            uint8 *yuv = (uint8 *)(outHeader->pBuffer + outHeader->nOffset);
            (*mAV1Dec_SetCurRecPic)(mHandle, yuv, (uint8 *)picPhyAddr, (void *)outHeader);
        }


        dump_strm(mPbuf_stream_v, dec_in.dataLen);

        int64_t start_decode = systemTime();
        MMDecRet decRet = (*mAV1DecDecode)(mHandle, &dec_in,&dec_out);
        int64_t end_decode = systemTime();
        ALOGI("%s, %d, decRet: %d, %dms, dec_out.frameEffective: %d, needIVOP: %d, in {%p, 0x%lx}, consume byte: %u, flag:0x%x, pts:%lld",
              __FUNCTION__, __LINE__, decRet, (unsigned int)((end_decode-start_decode) / 1000000L),
              dec_out.frameEffective, mNeedIVOP, dec_in.pStream, dec_in.pStream_phy, dec_in.dataLen, inHeader->nFlags, dec_out.pts);


        if(iUseAndroidNativeBuffer[OMX_DirOutput]) {
            if(mapper.unlock((const native_handle_t*)outHeader->pBuffer)) {
                ALOGE("onQueueFilled, mapper.unlock fail %p",outHeader->pBuffer);
            }
        }

        if (decRet != MMDEC_OK) {
            ALOGE("failed to support this format.");
            notify(OMX_EventError, OMX_ErrorFormatNotDetected, 0, NULL);
            mSignalledError = true;
            return;
        }


        MMDecRet ret;
        ret = (*mAV1DecGetInfo)(mHandle, &mDecoderInfo);

        if(ret == MMDEC_OK) {
            if (!((mDecoderInfo.picWidth<= mCapability.max_width&& mDecoderInfo.picHeight<= mCapability.max_height)
                    || (mDecoderInfo.picWidth <= mCapability.max_height && mDecoderInfo.picHeight <= mCapability.max_width))) {
                ALOGE("[%d,%d] is out of range [%d, %d], failed to support this format.",
                      mDecoderInfo.picWidth, mDecoderInfo.picHeight, mCapability.max_width, mCapability.max_height);
                notify(OMX_EventError, OMX_ErrorFormatNotDetected, 0, NULL);
                mSignalledError = true;
                return;
            }

            if (handlePortSettingChangeEvent(&mDecoderInfo)) {
                return;
            }
        } else {
            ALOGE("failed to get decoder information.");
        }


        if (dec_in.dataLen == 0) {
            inHeader->nOffset = 0;
            inInfo->mOwnedByUs = false;
            inQueue.erase(inQueue.begin());
            inInfo = NULL;
            notifyEmptyBufferDone(inHeader);
            inHeader = NULL;
        }


        while (!outQueue.empty() &&
                dec_out.frameEffective) {
            ALOGV("%s, %d, dec_out.pBufferHeader: %p, dec_out.mPicId: %d, dec_out.pts: %lld",
                  __FUNCTION__, __LINE__, dec_out.pBufferHeader, dec_out.mPicId, dec_out.pts);
            drainOneOutputBuffer(dec_out.mPicId, dec_out.pBufferHeader, dec_out.pts);
            dump_yuv(dec_out.pOutFrameY, mPictureSize);
            dec_out.frameEffective = false;
            if(mThumbnailMode) {
                mStopDecode = true;
            }
        }

    }
}

bool SPRDAV1Decoder::handlePortSettingChangeEvent(const AV1SwDecInfo *info) {
    OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(kOutputPortIndex)->mDef;
    OMX_BOOL useNativeBuffer = iUseAndroidNativeBuffer[OMX_DirOutput];

    ALOGI("%s, %d, mStride: %d, mSliceHeight: %d, info->picWidth: %d, info->picHeight: %d",
          __FUNCTION__, __LINE__,mStride, mSliceHeight, info->picWidth, info->picHeight);

    if ((info->picWidth && info->picHeight) && (mStride != ((info->picWidth + 15) & ~15))) {
        Mutex::Autolock autoLock(mLock);
        int32_t picId;
        uint8* pbuffer;
        void* pBufferHeader;
        uint64 pts;

        while (MMDEC_OK == (*mAV1Dec_GetLastDspFrm)(mHandle, (void**)&pbuffer, &pBufferHeader, &pts)) {
            drainOneOutputBuffer(picId, pBufferHeader, pts);
        }


        mFrameWidth = info->picWidth;
        mFrameHeight = info->picHeight;

        mStride = ((info->picWidth + 15) & ~15);
        //mSliceHeight = ((info->picHeight + 63) & ~63);
        mPictureSize = mStride * mSliceHeight * 3 / 2;

        updatePortDefinitions(true, true);
        (*mAV1Dec_ReleaseRefBuffers)(mHandle);
        notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
        mOutputPortSettingsChange = AWAITING_DISABLED;
        return true;
    }

    return false;
}


void SPRDAV1Decoder::drainOneOutputBuffer(int32_t picId, void* pBufferHeader, uint64 pts) {

    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    List<BufferInfo *>::iterator it = outQueue.begin();
    while ((*it)->mHeader != (OMX_BUFFERHEADERTYPE*)pBufferHeader && it != outQueue.end()) {
        ++it;
    }
    CHECK((*it)->mHeader == (OMX_BUFFERHEADERTYPE*)pBufferHeader);

    BufferInfo *outInfo = *it;
    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    outHeader->nFilledLen = mPictureSize;
    outHeader->nTimeStamp = (OMX_TICKS)pts;

    ALOGI("%s, %d, outHeader: %p, outHeader->pBuffer: %p, outHeader->nOffset: %d, outHeader->nFlags: %d, outHeader->nTimeStamp: %lld",
          __FUNCTION__, __LINE__, outHeader , outHeader->pBuffer, outHeader->nOffset, outHeader->nFlags, outHeader->nTimeStamp);

    outInfo->mOwnedByUs = false;
    outQueue.erase(it);
    outInfo = NULL;

    BufferCtrlStruct* pOutBufCtrl= (BufferCtrlStruct*)(outHeader->pOutputPortPrivate);
    pOutBufCtrl->iRefCount++;
    notifyFillBufferDone(outHeader);
}

bool SPRDAV1Decoder::drainAllOutputBuffers() {
    ALOGI("%s, %d", __FUNCTION__, __LINE__);

    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *outInfo;
    List<BufferInfo *>::iterator it;
    List<BufferInfo *>::iterator it_end;
    OMX_BUFFERHEADERTYPE *outHeader;

    uint8* pbuffer;
    void* pBufferHeader;
    uint64 pts;
    while (mEOSStatus != OUTPUT_FRAMES_FLUSHED) {

        if(outQueue.empty()) {
            ALOGW("%s, %d, There is no more  display buffer\n", __FUNCTION__, __LINE__);
            return false;
        }


        it = outQueue.begin();
        it_end = outQueue.end();


        if (MMDEC_OK == (*mAV1Dec_GetLastDspFrm)(mHandle,(void**)&pbuffer,&pBufferHeader, &pts) ) {
            while ((*it)->mHeader != (OMX_BUFFERHEADERTYPE*)pBufferHeader && it != it_end) {
                ++it;
            }
            CHECK((*it)->mHeader == (OMX_BUFFERHEADERTYPE*)pBufferHeader);
            outInfo = *it;
            outHeader = outInfo->mHeader;
            outHeader->nFilledLen = mPictureSize;

        } else {
            outInfo = *it;
            outHeader = outInfo->mHeader;
            outHeader->nTimeStamp = 0;
            outHeader->nFilledLen = 0;
            outHeader->nFlags = OMX_BUFFERFLAG_EOS;
            mEOSStatus = OUTPUT_FRAMES_FLUSHED;
        }


        outQueue.erase(it);
        outInfo->mOwnedByUs = false;
        BufferCtrlStruct* pOutBufCtrl= (BufferCtrlStruct*)(outHeader->pOutputPortPrivate);
        pOutBufCtrl->iRefCount++;
        notifyFillBufferDone(outHeader);

    }

    return true;
}

void SPRDAV1Decoder::onPortFlushCompleted(OMX_U32 portIndex) {
    if (portIndex == kInputPortIndex) {
        mEOSStatus = INPUT_DATA_AVAILABLE;
        mNeedIVOP = true;
    }

}

void SPRDAV1Decoder::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    switch (mOutputPortSettingsChange) {
    case NONE:
        break;

    case AWAITING_DISABLED:
    {
        CHECK(!enabled);
        mOutputPortSettingsChange = AWAITING_ENABLED;
        break;
    }

    default:
    {
        CHECK_EQ((int)mOutputPortSettingsChange, (int)AWAITING_ENABLED);
        CHECK(enabled);
        mOutputPortSettingsChange = NONE;
        break;
    }
    }
}

void SPRDAV1Decoder::onPortFlushPrepare(OMX_U32 portIndex) {
    if(portIndex == OMX_DirInput)
        ALOGI("%s", __FUNCTION__);

    if(portIndex == OMX_DirOutput) {
        if( NULL!=mAV1Dec_ReleaseRefBuffers )
            (*mAV1Dec_ReleaseRefBuffers)(mHandle);
        mNeedIVOP = true;
    }
}

void SPRDAV1Decoder::onReset() {
    mSignalledError = false;

    //avoid process error after stop codec and restart codec when port settings changing.
    mOutputPortSettingsChange = NONE;

}

void SPRDAV1Decoder::updatePortDefinitions(bool updateCrop, bool updateInputSize) {
    OMX_PARAM_PORTDEFINITIONTYPE *outDef = &editPortInfo(kOutputPortIndex)->mDef;

    if (updateCrop) {
        mCropWidth = mFrameWidth;
        mCropHeight = mFrameHeight;
    }
    outDef->format.video.nFrameWidth = mStride;
    outDef->format.video.nFrameHeight = mSliceHeight;
    outDef->format.video.nStride = mStride;
    outDef->format.video.nSliceHeight = mSliceHeight;
    outDef->nBufferSize = mPictureSize;

    ALOGI("%s, %d %d %d %d", __FUNCTION__, outDef->format.video.nFrameWidth,
          outDef->format.video.nFrameHeight,
          outDef->format.video.nStride,
          outDef->format.video.nSliceHeight);

    OMX_PARAM_PORTDEFINITIONTYPE *inDef = &editPortInfo(kInputPortIndex)->mDef;
    inDef->format.video.nFrameWidth = mFrameWidth;
    inDef->format.video.nFrameHeight = mFrameHeight;
    // input port is compressed, hence it has no stride
    inDef->format.video.nStride = 0;
    inDef->format.video.nSliceHeight = 0;

    // when output format changes, input buffer size does not actually change
    if (updateInputSize) {
        inDef->nBufferSize = max(
                                 outDef->nBufferSize / 2,
                                 inDef->nBufferSize);
    }
}


// static

int32_t SPRDAV1Decoder::BindFrameWrapper(void *aUserData, void *pHeader) {
    return static_cast<SPRDAV1Decoder *>(aUserData)->VSP_bind_cb(pHeader);
}

// static
int32_t SPRDAV1Decoder::UnbindFrameWrapper(void *aUserData, void *pHeader) {
    return static_cast<SPRDAV1Decoder *>(aUserData)->VSP_unbind_cb(pHeader);
}

int SPRDAV1Decoder::VSP_bind_cb(void *pHeader) {
    BufferCtrlStruct *pBufCtrl = (BufferCtrlStruct *)(((OMX_BUFFERHEADERTYPE *)pHeader)->pOutputPortPrivate);

    ALOGV("VSP_bind_cb, pBuffer: %p, pHeader: %p; iRefCount=%d",
          ((OMX_BUFFERHEADERTYPE *)pHeader)->pBuffer, pHeader,pBufCtrl->iRefCount);

    pBufCtrl->iRefCount++;
    mBindcnt++;
    return 0;
}

int SPRDAV1Decoder::VSP_unbind_cb(void *pHeader) {
    BufferCtrlStruct *pBufCtrl = (BufferCtrlStruct *)(((OMX_BUFFERHEADERTYPE *)pHeader)->pOutputPortPrivate);

    ALOGV("VSP_unbind_cb, pBuffer: %p, pHeader: %p; iRefCount=%d",
          ((OMX_BUFFERHEADERTYPE *)pHeader)->pBuffer, pHeader,pBufCtrl->iRefCount);

    if (pBufCtrl->iRefCount  > 0) {
        pBufCtrl->iRefCount--;
    }
    if (mBindcnt >0)
        mBindcnt--;
    return 0;
}

OMX_ERRORTYPE SPRDAV1Decoder::getExtensionIndex(
    const char *name, OMX_INDEXTYPE *index) {

    ALOGI("getExtensionIndex, name: %s",name);
    if(strcmp(name, SPRD_INDEX_PARAM_ENABLE_ANB) == 0) {
        ALOGI("getExtensionIndex:%s",SPRD_INDEX_PARAM_ENABLE_ANB);
        *index = (OMX_INDEXTYPE) OMX_IndexParamEnableAndroidBuffers;
        return OMX_ErrorNone;
    } else if (strcmp(name, SPRD_INDEX_PARAM_GET_ANB) == 0) {
        ALOGI("getExtensionIndex:%s",SPRD_INDEX_PARAM_GET_ANB);
        *index = (OMX_INDEXTYPE) OMX_IndexParamGetAndroidNativeBuffer;
        return OMX_ErrorNone;
    } else if (strcmp(name, SPRD_INDEX_PARAM_USE_ANB) == 0) {
        ALOGI("getExtensionIndex:%s",SPRD_INDEX_PARAM_USE_ANB);
        *index = OMX_IndexParamUseAndroidNativeBuffer2;
        return OMX_ErrorNone;
    } else if (strcmp(name, SPRD_INDEX_CONFIG_THUMBNAIL_MODE) == 0) {
        ALOGI("getExtensionIndex:%s",SPRD_INDEX_CONFIG_THUMBNAIL_MODE);
        *index = OMX_IndexConfigThumbnailMode;
        return OMX_ErrorNone;
    } else if (strcmp(name, SPRD_INDEX_PARAM_ALLOC_NATIVE_HANDLE) == 0) {
        ALOGI("getExtensionIndex:%s",SPRD_INDEX_PARAM_ALLOC_NATIVE_HANDLE);
        *index = OMX_IndexParamAllocNativeHandle;
        return OMX_ErrorNone;
    } else if (!strcmp(name,SPRD_INDEX_CONFIG_DEC_SCENE_MODE)) {
        ALOGI("getExtensionIndex:%s",SPRD_INDEX_CONFIG_DEC_SCENE_MODE);
        *index = (OMX_INDEXTYPE) OMX_IndexConfigDecSceneMode;
        return OMX_ErrorNone;
    }
    return OMX_ErrorNotImplemented;
}

bool SPRDAV1Decoder::openDecoder(const char* libName) {
    if(mLibHandle) {
        dlclose(mLibHandle);
    }

    ALOGI("openDecoder, lib: %s", libName);

    mLibHandle = dlopen(libName, RTLD_NOW);
    if(mLibHandle == NULL) {
        ALOGE("openDecoder, can't open lib: %s",libName);
        return false;
    }

    mAV1DecInit = (FT_AV1DecInit)dlsym(mLibHandle, "AV1DecInit");
    if(mAV1DecInit == NULL) {
        ALOGE("Can't find AV1DecInit in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mAV1DecGetInfo = (FT_AV1DecGetInfo)dlsym(mLibHandle, "AV1DecGetInfo");
    if(mAV1DecGetInfo == NULL) {
        ALOGE("Can't find AV1DecGetInfo in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mAV1DecDecode = (FT_AV1DecDecode)dlsym(mLibHandle, "AV1DecDecode");
    if(mAV1DecDecode == NULL) {
        ALOGE("Can't find AV1DecDecode in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mAV1DecRelease = (FT_AV1DecRelease)dlsym(mLibHandle, "AV1DecRelease");
    if(mAV1DecRelease == NULL) {
        ALOGE("Can't find AV1DecRelease in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mAV1Dec_SetCurRecPic = (FT_AV1Dec_SetCurRecPic)dlsym(mLibHandle, "AV1Dec_SetCurRecPic");
    if(mAV1Dec_SetCurRecPic == NULL) {
        ALOGE("Can't find AV1Dec_SetCurRecPic in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mAV1Dec_GetLastDspFrm = (FT_AV1Dec_GetLastDspFrm)dlsym(mLibHandle, "AV1Dec_GetLastDspFrm");
    if(mAV1Dec_GetLastDspFrm == NULL) {
        ALOGE("Can't find AV1Dec_GetLastDspFrm in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mAV1Dec_ReleaseRefBuffers = (FT_AV1Dec_ReleaseRefBuffers)dlsym(mLibHandle, "AV1Dec_ReleaseRefBuffers");
    if(mAV1Dec_ReleaseRefBuffers == NULL) {
        ALOGE("Can't find AV1Dec_ReleaseRefBuffers in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mAV1GetCodecCapability = (FT_AV1GetCodecCapability)dlsym(mLibHandle, "AV1GetCodecCapability");
    if(mAV1GetCodecCapability == NULL) {
        ALOGE("Can't find AV1GetCodecCapability in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    return true;
}

}  // namespace android

android::SprdOMXComponent *createSprdOMXComponent(
    const char *name, const OMX_CALLBACKTYPE *callbacks,
    OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    return new android::SPRDAV1Decoder(name, callbacks, appData, component);
}
