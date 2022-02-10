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

#ifndef SPRD_AV1_DECODER_H_
#define SPRD_AV1_DECODER_H_

#include "SprdSimpleOMXComponent.h"
#include <utils/KeyedVector.h>
#include "MemIon.h"
#include "av1_dec_api.h"

#include <VideoAPI.h>
#include <ColorUtils.h>
#define SPRD_ION_DEV "/dev/ion"

#define AV1_DECODER_INTERNAL_BUFFER_SIZE (0x100000)
#define AV1_DECODER_STREAM_BUFFER_SIZE (1024*1024*2)
#define AV1_HEADER_SIZE (1024)

struct tagAV1Handle;

namespace android {

struct SPRDAV1Decoder : public SprdSimpleOMXComponent {
    SPRDAV1Decoder(const char *name,
                   const OMX_CALLBACKTYPE *callbacks,
                   OMX_PTR appData,
                   OMX_COMPONENTTYPE **component);

    OMX_ERRORTYPE initCheck() const;
protected:
    virtual ~SPRDAV1Decoder();

    virtual OMX_ERRORTYPE internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params);

    virtual OMX_ERRORTYPE internalUseBuffer(
        OMX_BUFFERHEADERTYPE **buffer,
        OMX_U32 portIndex,
        OMX_PTR appPrivate,
        OMX_U32 size,
        OMX_U8 *ptr,
        BufferPrivateStruct* bufferPrivate=NULL);

    virtual OMX_ERRORTYPE allocateBuffer(
        OMX_BUFFERHEADERTYPE **header,
        OMX_U32 portIndex,
        OMX_PTR appPrivate,
        OMX_U32 size);

    virtual OMX_ERRORTYPE freeBuffer(
        OMX_U32 portIndex,
        OMX_BUFFERHEADERTYPE *header);

    virtual OMX_ERRORTYPE getConfig(OMX_INDEXTYPE index, OMX_PTR params);
    virtual OMX_ERRORTYPE setConfig(
        OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);
    virtual void onPortEnableCompleted(OMX_U32 portIndex, bool enabled);
    virtual void onPortFlushPrepare(OMX_U32 portIndex);
    virtual OMX_ERRORTYPE getExtensionIndex(const char *name, OMX_INDEXTYPE *index);
    virtual void onReset();

private:
    enum {
        kNumInputBuffers  = 8,
        kNumOutputBuffers = 8,
    };

    enum EOSStatus {
        INPUT_DATA_AVAILABLE,
        INPUT_EOS_SEEN,
        OUTPUT_FRAMES_FLUSHED,
    };

    enum OutputPortSettingChange {
        NONE,
        AWAITING_DISABLED,
        AWAITING_ENABLED
    };
    enum {
        kNotSupported,
        kPreferBitstream,
        kPreferContainer,
    };
    tagAV1Handle *mHandle;

    size_t mInputBufferCount;

    uint32_t mFrameWidth, mFrameHeight;
    uint32_t mStride, mSliceHeight;
    uint32_t mPictureSize;
    uint32_t mCropWidth, mCropHeight;
    Mutex mLock;
    EOSStatus mEOSStatus;
    OutputPortSettingChange mOutputPortSettingsChange;
    uint8 mBindcnt;
    int mDecSceneMode;
    bool mHeadersDecoded;
    bool mSignalledError;

    bool mAllocateBuffers;
    bool mNeedIVOP;

    bool mDumpYUVEnabled;
    bool mDumpStrmEnabled;
    bool mStopDecode;
    OMX_BOOL mThumbnailMode;

    uint8_t *mPbuf_stream_v;
    size_t mPbuf_stream_size;

    bool mAllocInput;

    AV1SwDecInfo mDecoderInfo;

    void *mLibHandle;
    FT_AV1DecInit mAV1DecInit;
    FT_AV1DecGetInfo mAV1DecGetInfo;
    FT_AV1DecDecode mAV1DecDecode;
    FT_AV1DecRelease mAV1DecRelease;
    FT_AV1Dec_SetCurRecPic  mAV1Dec_SetCurRecPic;
    FT_AV1Dec_GetLastDspFrm  mAV1Dec_GetLastDspFrm;
    FT_AV1Dec_ReleaseRefBuffers  mAV1Dec_ReleaseRefBuffers;
    FT_AV1GetCodecCapability mAV1GetCodecCapability;
    bool mFrameDecoded;
    bool mIsInterlacedSequence;
    OMX_BOOL iUseAndroidNativeBuffer[2];
    MMDecCapability mCapability;

    FILE* mFile_yuv;
    FILE* mFile_bs;

    OMX_ERRORTYPE mInitCheck;

    void initPorts();
    status_t initDecoder();
    void releaseDecoder();
    void updatePortDefinitions(bool updateCrop = true, bool updateInputSize = false);
    bool drainAllOutputBuffers();
    void drainOneOutputBuffer(int32_t picId, void* pBufferHeader, uint64 pts);
    bool handleCropRectEvent(const CropParams* crop);
    bool handlePortSettingChangeEvent(const AV1SwDecInfo *info);

    static int32_t BindFrameWrapper(void *aUserData, void *pHeader);
    static int32_t UnbindFrameWrapper(void *aUserData, void *pHeader);


    int VSP_bind_cb(void *pHeader);
    int VSP_unbind_cb(void *pHeader);
    bool openDecoder(const char* libName);
    void dump_yuv(uint8 *pBuffer, int32 aInBufSize);
    void dump_strm(uint8 *pBuffer, int32 aInBufSize);

    OMX_ERRORTYPE allocateStreamBuffer(OMX_U32 bufferSize);
    void releaseStreamBuffer();


    DISALLOW_EVIL_CONSTRUCTORS(SPRDAV1Decoder);
};

}  // namespace android

#endif  // SPRD_AV1_DECODER_H_

