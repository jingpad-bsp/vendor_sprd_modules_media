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

#ifndef SPRD_VP9_DECODER_H_
#define SPRD_VP9_DECODER_H_

#include "SprdSimpleOMXComponent.h"
#include "MemIon.h"

#include "vp9_dec_api.h"

#define SPRD_ION_DEV "/dev/ion"

#define VP9_DECODER_INTERNAL_BUFFER_SIZE  (0x200000)

namespace android {

struct SPRDVP9Decoder : public SprdSimpleOMXComponent {
    SPRDVP9Decoder(const char *name,
                   const OMX_CALLBACKTYPE *callbacks,
                   OMX_PTR appData,
                   OMX_COMPONENTTYPE **component);

    OMX_ERRORTYPE initCheck() const;
protected:
    virtual ~SPRDVP9Decoder();

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

    virtual OMX_ERRORTYPE getConfig(
        OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE setConfig(
        OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);
    virtual void onPortFlushPrepare(OMX_U32 portIndex);
    virtual void onPortEnableCompleted(OMX_U32 portIndex, bool enabled);
    virtual OMX_ERRORTYPE getExtensionIndex(
        const char *name, OMX_INDEXTYPE *index);
    virtual void onReset();

private:
    enum {
        kNumBuffers = 9//8
    };

    enum EOSStatus {
        INPUT_DATA_AVAILABLE,
        INPUT_EOS_SEEN,
        OUTPUT_FRAMES_FLUSHED,
    };

    enum OutputPortSettingChange{
        NONE,
        AWAITING_DISABLED,
        AWAITING_ENABLED
    };

    tagVP9Handle *mHandle;

    size_t mInputBufferCount;
    size_t mInputBufferSize;
    int mSetFreqCount;
    uint32_t mPictureSize;
    int32_t mFrameWidth, mFrameHeight;
    int32_t mStride, mSliceHeight;

    int32_t mMaxWidth, mMaxHeight;
    uint32_t mCropWidth, mCropHeight;
    uint8_t mFbcMode;
    uint32_t mUsage;

    Mutex mLock;

    EOSStatus mEOSStatus;
    OutputPortSettingChange mOutputPortSettingsChange;

    bool mHeadersDecoded;
    bool mSignalledError;
    bool mNeedIVOP;
    bool mIOMMUEnabled;
    int mIOMMUID;
    bool mDumpYUVEnabled;
    bool mDumpStrmEnabled;
    bool mAllocateBuffers;
    uint8_t *mPbuf_inter;

    sp<MemIon> mPmem_stream;
    uint8_t* mPbuf_stream_v;
    unsigned long mPbuf_stream_p;
    size_t mPbuf_stream_size;

    sp<MemIon> mPmem_extra;
    uint8_t*  mPbuf_extra_v;
    unsigned long  mPbuf_extra_p;
    size_t  mPbuf_extra_size;

    void* mLibHandle;
    FT_VP9DecSetCurRecPic mVP9DecSetCurRecPic;
    FT_VP9DecInit mVP9DecInit;
    FT_VP9DecDecode mVP9DecDecode;
    FT_VP9DecRelease mVP9DecRelease;
    FT_VP9GetBufferDimensions mVP9GetVideoDimensions;
    FT_VP9GetBufferDimensions mVP9GetBufferDimensions;
    FT_VP9DecReleaseRefBuffers  mVP9DecReleaseRefBuffers;
    FT_VP9DecGetLastDspFrm mVP9DecGetLastDspFrm;
    FT_VP9GetCodecCapability mVP9GetCodecCapability;
    FT_VP9Dec_get_iova mVP9DecGetIOVA;
    FT_VP9Dec_free_iova mVP9DecFreeIOVA;
    FT_VP9Dec_get_IOMMU_status mVP9DecGetIOMMUStatus;
    FT_VP9DecMemInit mVP9DecMemInit;
    bool mFrameDecoded;

    OMX_BOOL iUseAndroidNativeBuffer[2];
    OMX_ERRORTYPE mInitCheck;

    FILE* mFile_yuv;
    FILE* mFile_bs;

    static int32_t BindFrameWrapper(void *aUserData, void *pHeader, int flag);
    static int32_t UnbindFrameWrapper(void *aUserData, void *pHeader, int flag);
    static int32_t extMemoryAllocWrapper(void *userData, unsigned int extra_mem_size);

    int VSP_bind_cb(void *pHeader,int flag);
    int VSP_unbind_cb(void *pHeader,int flag);
    int extMemoryAlloc(unsigned int extra_mem_size);

    void initPorts();
    status_t initDecoder();
    void releaseDecoder();
    bool drainAllOutputBuffers();
    void updatePortDefinitions(bool updateCrop = true, bool updateInputSize = false);
    bool openDecoder(const char* libName);
    void freeOutputBufferIOVA();

    DISALLOW_EVIL_CONSTRUCTORS(SPRDVP9Decoder);
};

}  // namespace android

#endif  // SPRD_VP9_DECODER_H_
