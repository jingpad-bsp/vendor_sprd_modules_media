/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef SPRD_HEVC_ENCODER_H_
#define SPRD_HEVC_ENCODER_H_

#include "SprdSimpleOMXComponent.h"

#include "hevc_enc_api.h"

#define H265ENC_INTERNAL_BUFFER_SIZE  (0x200000)

namespace android {


struct SPRDHEVCEncoder :  public SprdSimpleOMXComponent {
    SPRDHEVCEncoder(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component);

    // Override SimpleSoftOMXComponent methods
    virtual OMX_ERRORTYPE internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params);

    virtual OMX_ERRORTYPE setConfig(
        OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);

    virtual OMX_ERRORTYPE getExtensionIndex(
        const char *name, OMX_INDEXTYPE *index);

protected:
    virtual ~SPRDHEVCEncoder();

private:
    enum {
        kNumBuffers = 4,
    };

    // OMX input buffer's timestamp and flags
    typedef struct {
        int64_t mTimeUs;
        int32_t mFlags;
    } InputBufferInfo;

    tagHEVCHandle          *mHandle;
    tagHEVCEncParam        *mEncParams;
    MMEncConfig *mEncConfig;
    uint32_t              *mSliceGroup;
    Vector<InputBufferInfo> mInputBufferInfoVec;

    int64_t  mNumInputFrames;
    int64_t  mPrevTimestampUs;
    int mSetFreqCount;
    int mBitrate;
    int mEncSceneMode;
    bool mSetEncMode;
    int32_t  mVideoWidth;
    int32_t  mVideoHeight;
    int32_t  mVideoFrameRate;
    int32_t  mVideoBitRate;
    int32_t  mVideoColorFormat;

    OMX_BOOL mStoreMetaData;
    OMX_BOOL mPrependSPSPPS;
    bool     mIOMMUEnabled;
    int mIOMMUID;
    bool     mDumpYUVEnabled;
    bool     mDumpStrmEnabled;
    bool     mStarted;
    bool     mSpsPpsHeaderReceived;
    bool     mReadyForNextFrame;
    bool     mSawInputEOS;
    bool     mSignalledError;
    bool     mKeyFrameRequested;
    bool     mIschangebitrate;

    uint8_t *mPbuf_inter;

    sp<MemIon> mYUVInPmemHeap;
    uint8_t *mPbuf_yuv_v;
    unsigned long mPbuf_yuv_p;
    size_t mPbuf_yuv_size;

    sp<MemIon> mPmem_stream;
    sp<MemIon> mPmem_streamPn;
    uint8_t *mPbuf_stream_v;
    unsigned long mPbuf_stream_p;
    size_t mPbuf_stream_size;
    uint8_t *mPbuf_stream_v_pn;
    unsigned long mPbuf_stream_p_pn;
    size_t mPbuf_stream_size_pn;

    sp<MemIon> mPmem_extra;
    uint8_t *mPbuf_extra_v;
    unsigned long  mPbuf_extra_p;
    size_t  mPbuf_extra_size;

    HEVCProfile mHEVCEncProfile;
    HEVCLevel   mHEVCEncLevel;
    OMX_U32 mPFrames;
    bool mNeedAlign;

    void* mLibHandle;
    FT_H265EncPreInit        mH265EncPreInit;
    FT_H265EncInit        mH265EncInit;
    FT_H265EncSetConf        mH265EncSetConf;
    FT_H265EncGetConf        mH265EncGetConf;
    FT_H265EncStrmEncode        mH265EncStrmEncode;
    FT_H265EncGenHeader        mH265EncGenHeader;
    FT_H265EncRelease        mH265EncRelease;
    FT_H265EncGetCodecCapability  mH265EncGetCodecCapability;
    FT_H265Enc_get_iova  mH265EncGetIOVA;
    FT_H265Enc_free_iova  mH265EncFreeIOVA;
    FT_H265Enc_get_IOMMU_status  mH265EncGetIOMMUStatus;

    FT_H265Enc_Need_Align  mH265Enc_NeedAlign;
    int32_t  mFrameWidth;
    int32_t  mFrameHeight;
    bool mSupportRGBEnc;

#ifdef CONFIG_SPRD_RECORD_EIS
    int32_t mEISMode;
#endif

    MMEncVideoInfo mEncInfo;
    MMEncCapability mCapability;

    FILE* mFile_yuv;
    FILE* mFile_bs;

    void initPorts();
    OMX_ERRORTYPE initEncParams();
    OMX_ERRORTYPE initEncoder();
    OMX_ERRORTYPE releaseEncoder();
    OMX_ERRORTYPE releaseResource();
    bool openEncoder(const char* libName);
    void flushCacheforBSBuf();
    static void FlushCacheWrapper(void* aUserData);

    DISALLOW_EVIL_CONSTRUCTORS(SPRDHEVCEncoder);
};

}  // namespace android

#endif  // SPRD_HEVC_ENCODER_H_
