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

#ifndef SPRD_AVC_DECODER_H_
#define SPRD_AVC_DECODER_H_

#include "SprdSimpleOMXComponent.h"
#include <utils/KeyedVector.h>
#include "MemIon.h"
#include "avc_dec_api.h"
#include "SPRDDeinterlace.h"
#include <VideoAPI.h>
#include <ColorUtils.h>
#define SPRD_ION_DEV "/dev/ion"

#define H264_DECODER_INTERNAL_BUFFER_SIZE (0x100000)
#define H264_DECODER_STREAM_BUFFER_SIZE (1024*1024*2)
#define H264_HEADER_SIZE (1024)

struct tagAVCHandle;

namespace android {

struct SPRDAVCDecoder : public SprdSimpleOMXComponent {
    SPRDAVCDecoder(const char *name,
                   const OMX_CALLBACKTYPE *callbacks,
                   OMX_PTR appData,
                   OMX_COMPONENTTYPE **component);

    OMX_ERRORTYPE initCheck() const;
protected:
    virtual ~SPRDAVCDecoder();

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
    virtual void onDecodePrepare(OMX_BUFFERHEADERTYPE *header);

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
    tagAVCHandle *mHandle;

    size_t mInputBufferCount;
    int32_t mPicId;
    int mSetFreqCount;
    uint32_t mMinCompressionRatio;

    uint32_t mFrameWidth, mFrameHeight;
    uint32_t mStride, mSliceHeight;
    uint32_t mPictureSize;
    uint32_t mCropWidth, mCropHeight;
    uint8_t mFbcMode;
    uint32_t mUsage;

    Mutex mLock;

    EOSStatus mEOSStatus;
    OutputPortSettingChange mOutputPortSettingsChange;

    int mDecSceneMode;
    bool mHeadersDecoded;
    bool mSignalledError;
    bool mDecoderSwFlag;
    bool mChangeToSwDec;
    bool mAllocateBuffers;
    bool mNeedIVOP;
    bool mIOMMUEnabled;
    int mIOMMUID;
    bool mDumpYUVEnabled;
    bool mDumpStrmEnabled;
    bool mStopDecode;
    OMX_BOOL mThumbnailMode;

    uint8_t *mCodecInterBuffer;
    uint8_t *mCodecExtraBuffer;

    sp<MemIon> mPmem_stream;
    uint8_t *mPbuf_stream_v;
    unsigned long mPbuf_stream_p;
    size_t mPbuf_stream_size;

    sp<MemIon> mPmem_extra;
    uint8_t *mPbuf_extra_v;
    unsigned long  mPbuf_extra_p;
    size_t  mPbuf_extra_size;

    sp<MemIon> mPmem_mbinfo[17];
    uint8_t *mPbuf_mbinfo_v[17];
    unsigned long  mPbuf_mbinfo_p[17];
    size_t  mPbuf_mbinfo_size[17];
    int mPbuf_mbinfo_idx;

    bool mDecoderSawSPS;
    bool mDecoderSawPPS;
    uint8_t *mSPSData;
    uint32_t mSPSDataSize;
    uint8_t *mPPSData;
    uint32_t mPPSDataSize;
    bool mIsResume;
    bool mSecureFlag;
    bool mAllocInput;

    SPRDDeinterlace* mDeintl;
    bool mDeintInitialized;
    bool mNeedDeinterlace;
    H264SwDecInfo mDecoderInfo;

    void *mLibHandle;
    FT_H264DecInit mH264DecInit;
    FT_H264DecGetInfo mH264DecGetInfo;
    FT_H264DecDecode mH264DecDecode;
    FT_H264DecRelease mH264DecRelease;
    FT_H264Dec_SetCurRecPic  mH264Dec_SetCurRecPic;
    FT_H264Dec_GetLastDspFrm  mH264Dec_GetLastDspFrm;
    FT_H264Dec_ReleaseRefBuffers  mH264Dec_ReleaseRefBuffers;
    FT_H264DecMemInit mH264DecMemInit;
    FT_H264GetCodecCapability mH264GetCodecCapability;
    FT_H264DecGetNALType mH264DecGetNALType;
    FT_H264DecSetparam mH264DecSetparam;
    FT_H264Dec_get_iova mH264DecGetIOVA;
    FT_H264Dec_free_iova mH264DecFreeIOVA;
    FT_H264Dec_get_IOMMU_status mH264DecGetIOMMUStatus;
    FT_H264Dec_InitStructForNewSeq mH264DecInitStructForNewSeq;
    FT_H264DecNeedToSwitchToSwDecoder mH264DecNeedToSwitchToSwDecoder;
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
    bool handlePortSettingChangeEvent(const H264SwDecInfo *info);

    static int32_t MbinfoMemAllocWrapper(void* aUserData, unsigned int size_mbinfo, unsigned long *pPhyAddr);
    static int32_t ExtMemAllocWrapper(void* aUserData, unsigned int size_extra);
    static int32_t BindFrameWrapper(void *aUserData, void *pHeader);
    static int32_t UnbindFrameWrapper(void *aUserData, void *pHeader);
    static void  DrainOneOutputBuffercallback(void* decoder, int32_t picId, void* pBufferHeader, unsigned long long pts);

    int VSP_malloc_mbinfo_cb(unsigned int size_mbinfo, unsigned long *pPhyAddr);
    int VSP_malloc_cb(unsigned int size_extra);
    int VSP_bind_cb(void *pHeader);
    int VSP_unbind_cb(void *pHeader);
    bool openDecoder(const char* libName);
    void findCodecConfigData(OMX_BUFFERHEADERTYPE *header);
    void dump_yuv(uint8 *pBuffer, int32 aInBufSize);
    void dump_strm(uint8 *pBuffer, int32 aInBufSize);
    void freeOutputBufferIOVA();
    bool outputBuffersNotEnough(const H264SwDecInfo *, OMX_U32 , OMX_U32 , OMX_BOOL);
    OMX_ERRORTYPE allocateDecOutputBuffer(H264SwDecInfo* pDecoderInfo);
    OMX_ERRORTYPE freeDecOutputBuffer();
    bool drainAllDecOutputBufferforDeintl();
    void drainOneDeintlBuffer(void* pBufferHeader, uint64 pts);
    void signalDeintlThread();
    void flushDeintlBuffer();
    OMX_ERRORTYPE allocateStreamBuffer(OMX_U32 bufferSize);
    void releaseStreamBuffer();
    bool supportsDescribeColorAspects();
    int getColorAspectPreference();
    bool mUpdateColorAspects;

    Mutex mColorAspectsLock;
    // color aspects passed from the framework.
    ColorAspects mDefaultColorAspects;
    // color aspects parsed from the bitstream.
    ColorAspects mBitstreamColorAspects;
    // final color aspects after combining the above two aspects.
    ColorAspects mFinalColorAspects;

    bool colorAspectsDiffer(const ColorAspects &a, const ColorAspects &b);

    // This functions takes two color aspects and updates the mFinalColorAspects
    // based on |preferredAspects|.
    void updateFinalColorAspects(
    const ColorAspects &otherAspects, const ColorAspects &preferredAspects);

    // This function will update the mFinalColorAspects based on codec preference.
    status_t handleColorAspectsChange();

    bool getVUIParams(H264SwDecInfo *mDecoderInfo);

    bool mDeintlNeedRestart;
    DISALLOW_EVIL_CONSTRUCTORS(SPRDAVCDecoder);
};

}  // namespace android

#endif  // SPRD_AVC_DECODER_H_

