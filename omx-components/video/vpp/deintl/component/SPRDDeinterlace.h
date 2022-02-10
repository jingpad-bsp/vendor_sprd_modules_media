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

#ifndef SPRD_DEINTERLACE_H_
#define SPRD_DEINTERLACE_H_
#include "SprdSimpleOMXComponent.h"

#include "vpp_drv_interface.h"
#include "MemIon.h"
#include <media/stagefright/foundation/ABase.h>

namespace android {

//#define  T_BUFFER_INFO SprdSimpleOMXComponent::BufferInfo
struct BufferInfoBase {   //must be same to SprdSimpleOMXComponent::BufferInfo
        OMX_BUFFERHEADERTYPE *mHeader;
        bool mOwnedByUs;

        uint32_t DeintlWidth;
        uint32_t DeintlHeight;

        int32_t mPicId;
        int32_t mNodeId;
};
struct BufferInfo: public BufferInfoBase {   //for the intel deintelace function only
        OMX_BUFFERHEADERTYPE *mDecHeader;
};

typedef void (*DrainOneOutputBuffercallback)(void* decoder, int32_t picId, void* pBufferHeader, unsigned long long pts);

typedef struct DeinterlaceInfo
{
    int width;
    int height;
    DrainOneOutputBuffercallback callback;
    void* decoder;
    Mutex* threadLock;
}DeinterlaceInfo;


struct SPRDDeinterlace {

public:
    typedef struct BufferCtrlStruct
    {
        uint32_t iRefCount;
        sp<MemIon> pMem;
        int bufferFd;
        unsigned long phyAddr;
        size_t bufferSize;
        int id;
    } BufferCtrlStruct;

    explicit SPRDDeinterlace(void* decOutputBufQueue, void* deinterInputBufQueue, void* outQueue, DeinterlaceInfo& info);
    virtual ~SPRDDeinterlace();

    bool mDone;
    bool mUseNativeBuffer;
    uint32_t mDeintlFrameNum;
    Condition mDeinterReadyCondition;   // Signal that de-interlace queue are available

    void startDeinterlaceThread();
    void stopDeinterlaceThread();
    int32 VspFreeIova(unsigned long iova, size_t size);
    int32 VspGetIova(int fd, unsigned long *iova, size_t *size);

private:

    bool mIOMMU_VPP_Enabled;
    int32_t  mIOMMU_VPP_ID;

    pthread_t   mThread;                // Thread id for deinterlace

    VPPObject* mVPPHeader;
    //void* mComponent;
    List<BufferInfoBase *>* mDecOutputBufQueue;
    List<BufferInfoBase *>* mDeinterInputBufQueue;
    List<BufferInfoBase *>* mOutQueue;
    DeinterlaceInfo mDeinterlaceInfo;

    //BufferCtrlStruct * mDispBufferCtrl;

    //KeyedVector<T_BUFFER_INFO *, BufferCtrlStruct *> mBufferCtrlbyBufferInfo;

    void deintlThreadFunc();
    static void * ThreadWrapper(void *me);
    int remapDispBuffer(OMX_BUFFERHEADERTYPE * header, unsigned long* phyAddr);
    int remapDeintlSrcBuffer(BufferInfoBase * itBuffer,  unsigned long* phyAddr);
    void unmapDeintlSrcBuffer(BufferInfoBase * itBuffer);
    void fillDeintlSrcBuffer(BufferInfoBase * Pre, BufferInfoBase * Cur);

    DISALLOW_EVIL_CONSTRUCTORS(SPRDDeinterlace);
};

}  // namespace android

#endif  // SPRD_AVC_DECODER_H_

