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

//#define LOG_NDEBUG 0
#define LOG_TAG "SPRDAVCEncoder"
#include <utils/Log.h>
#ifndef CONFIG_BIA_SUPPORT
#include <arm_neon.h>
#endif
#include "avc_enc_api.h"
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <MetadataBufferType.h>
#include <HardwareAPI.h>

#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include <dlfcn.h>

#include "MemIon.h"

#include "SPRDAVCEncoder.h"
#include "sprd_ion.h"
#include "gralloc_public.h"
#include <cutils/properties.h>

namespace android {

static const CodecProfileLevel kProfileLevels[] = {
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel51 },

    { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel51 },

    { OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel51 },
};


template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

typedef struct LevelConversion {
    OMX_U32 omxLevel;
    AVCLevel avcLevel;
} LevelConcersion;

typedef struct ProfileConversion {
    OMX_U32 omxProfile;
    AVCProfile avcProfile;
} ProfileConcersion;

static ProfileConcersion ConversionTableProfile[] = {
    { OMX_VIDEO_AVCProfileBaseline,  AVC_BASELINE },
    { OMX_VIDEO_AVCProfileMain, AVC_MAIN   },
    { OMX_VIDEO_AVCProfileExtended, AVC_EXTENDED },
    { OMX_VIDEO_AVCProfileHigh, AVC_HIGH },
    { OMX_VIDEO_AVCProfileHigh10, AVC_HIGH10 },
    { OMX_VIDEO_AVCProfileHigh422,  AVC_HIGH422 },
    { OMX_VIDEO_AVCProfileHigh444,  AVC_HIGH444 },
};

static LevelConversion ConversionTable[] = {
    { OMX_VIDEO_AVCLevel1,  AVC_LEVEL1_B },
    { OMX_VIDEO_AVCLevel1b, AVC_LEVEL1   },
    { OMX_VIDEO_AVCLevel11, AVC_LEVEL1_1 },
    { OMX_VIDEO_AVCLevel12, AVC_LEVEL1_2 },
    { OMX_VIDEO_AVCLevel13, AVC_LEVEL1_3 },
    { OMX_VIDEO_AVCLevel2,  AVC_LEVEL2 },
    { OMX_VIDEO_AVCLevel21, AVC_LEVEL2_1 },
    { OMX_VIDEO_AVCLevel22, AVC_LEVEL2_2 },
    { OMX_VIDEO_AVCLevel3,  AVC_LEVEL3   },
    { OMX_VIDEO_AVCLevel31, AVC_LEVEL3_1 },
    { OMX_VIDEO_AVCLevel32, AVC_LEVEL3_2 },
    { OMX_VIDEO_AVCLevel4,  AVC_LEVEL4   },
    { OMX_VIDEO_AVCLevel41, AVC_LEVEL4_1 },
    { OMX_VIDEO_AVCLevel42, AVC_LEVEL4_2 },
    { OMX_VIDEO_AVCLevel5,  AVC_LEVEL5   },
    { OMX_VIDEO_AVCLevel51, AVC_LEVEL5_1 },
};

static status_t ConvertOmxAvcProfileToAvcSpecProfile(
    OMX_U32 omxProfile, AVCProfile *avcProfile) {
    for (size_t i = 0, n = sizeof(ConversionTableProfile)/sizeof(ConversionTableProfile[0]);
            i < n; ++i) {
        if (omxProfile == ConversionTableProfile[i].omxProfile) {
            *avcProfile = ConversionTableProfile[i].avcProfile;
            return OK;
        }
    }
    ALOGE("ConvertOmxAvcProfileToAvcSpecProfile: %d Profile not supported", (int32_t)omxProfile);
    return BAD_VALUE;
}

static status_t ConvertOmxAvcLevelToAvcSpecLevel(
    OMX_U32 omxLevel, AVCLevel *avcLevel) {
    for (size_t i = 0, n = sizeof(ConversionTable)/sizeof(ConversionTable[0]);
            i < n; ++i) {
        if (omxLevel == ConversionTable[i].omxLevel) {
            *avcLevel = ConversionTable[i].avcLevel;
            return OK;
        }
    }

    ALOGE("ConvertOmxAvcLevelToAvcSpecLevel: %d level not supported",
          (int32_t)omxLevel);

    return BAD_VALUE;
}

static status_t ConvertAvcSpecLevelToOmxAvcLevel(
    AVCLevel avcLevel, OMX_U32 *omxLevel) {
    for (size_t i = 0, n = sizeof(ConversionTable)/sizeof(ConversionTable[0]);
            i < n; ++i) {
        if (avcLevel == ConversionTable[i].avcLevel) {
            *omxLevel = ConversionTable[i].omxLevel;
            return OK;
        }
    }

    ALOGE("ConvertAvcSpecLevelToOmxAvcLevel: %d level not supported",
          (int32_t) avcLevel);

    return BAD_VALUE;
}

/*
 * In case of orginal input height_org is not 16 aligned, we shoud copy original data to a larger space
 * Example: width_org = 640, height_org = 426,
 * We have to copy this data to a width_dst = 640 height_dst = 432 buffer which is 16 aligned.
 * Be careful, when doing this convert we MUST keep UV in their right position.
 *
 * FIXME: If width_org is not 16 aligned also, this would be much complicate
 *
 */
inline static void ConvertYUV420PlanarToYVU420SemiPlanar(uint8_t *inyuv, uint8_t* outyuv,
        int32_t width_org, int32_t height_org, int32_t width_dst, int32_t height_dst) {

    int32_t inYsize = width_org * height_org;
    uint32_t *outy =  (uint32_t *) outyuv;
    uint16_t *incb = (uint16_t *) (inyuv + inYsize);
    uint16_t *incr = (uint16_t *) (inyuv + inYsize + (inYsize >> 2));

    /* Y copying */
    memcpy(outy, inyuv, inYsize);

    /* U & V copying, Make sure uv data is in their right position*/
    uint32_t *outUV = (uint32_t *) (outyuv + width_dst * height_dst);
    for (int32_t i = height_org >> 1; i > 0; --i) {
        for (int32_t j = width_org >> 2; j > 0; --j) {
            uint32_t tempU = *incb++;
            uint32_t tempV = *incr++;

            tempU = (tempU & 0xFF) | ((tempU & 0xFF00) << 8);
            tempV = (tempV & 0xFF) | ((tempV & 0xFF00) << 8);
            uint32_t temp = tempV | (tempU << 8);

            // Flip U and V
            *outUV++ = temp;
        }
    }
}
#ifdef CONFIG_BIA_SUPPORT
static int RGB_r_y[256];
static int RGB_r_cb[256];
static int RGB_r_cr_b_cb[256];
static int RGB_g_y[256];
static int RGB_g_cb[256];
static int RGB_g_cr[256];
static int RGB_b_y[256];
static int RGB_b_cr[256];
static  bool mConventFlag = false;

//init the convert table, the Transformation matrix is as:
// Y  =  ((66 * (_r)  + 129 * (_g)  + 25    * (_b)) >> 8) + 16
// Cb = ((-38 * (_r) - 74   * (_g)  + 112  * (_b)) >> 8) + 128
// Cr =  ((112 * (_r) - 94   * (_g)  - 18    * (_b)) >> 8) + 128
inline static void inittable() {
    ALOGI("init table");
    int i = 0;
    for(i = 0; i < 256; i++) {
        RGB_r_y[i] =  (66 * i);
        RGB_r_cb[i] = (38 * i);
        RGB_r_cr_b_cb[i] = (112 * i);
        RGB_g_y[i] = (129 * i);
        RGB_g_cb[i] = (74 * i) ;
        RGB_g_cr[i] = (94 * i);
        RGB_b_y[i] =  (25 * i);
        RGB_b_cr[i] = (18 * i);
    }
}
inline static void ConvertARGB888ToYVU420SemiPlanar_c(uint8_t *inrgb, uint8_t* outy,uint8_t* outuv,
        int32_t width_org, int32_t height_org, int32_t width_dst, int32_t height_dst) {
#define RGB2Y(_r, _g, _b)    ((  *(RGB_r_y +_r)      +   *(RGB_g_y+_g)   +    *(RGB_b_y+_b)) >> 8) + 16;
#define RGB2CB(_r, _g, _b)   (( -*(RGB_r_cb +_r)     -   *(RGB_g_cb+_g)  +    *(RGB_r_cr_b_cb+_b)) >> 8) + 128;
#define RGB2CR(_r, _g, _b)   ((  *(RGB_r_cr_b_cb +_r)-   *(RGB_g_cr+_g)  -    *(RGB_b_cr+_b))>>8) + 128;
    uint8_t *argb_ptr = inrgb;
    uint8_t *y_p = outy;
    uint8_t *vu_p = outuv;

    if (NULL == inrgb || NULL ==  outy || NULL == outuv)
        return;

    if ((height_org & 0x1) != 0)
        height_org &= ~0x1;

    if ((width_org & 0x1) != 0) {
        ALOGE("width_org:%d is not supported", width_org);
        return;
    }

    if(!mConventFlag) {
        mConventFlag = true;
        inittable();
    }
    ALOGI("rgb2yuv start");
    uint8_t *y_ptr;
    uint8_t *vu_ptr;
    int64_t start_encode = systemTime();
    uint32 i ;
    uint32 j = height_org + 1;
    while(--j) {
        //the width_dst may be bigger than width_org,
        //make start byte in every line of Y and CbCr align
        y_ptr = y_p;
        y_p += width_dst;
        if (!(j & 1))  {
            vu_ptr = vu_p;
            vu_p += width_dst;
            i  = width_org / 2 + 1;
            while(--i) {
                //format abgr, little endian
                *y_ptr++    = RGB2Y(*argb_ptr, *(argb_ptr+1), *(argb_ptr+2));
                *vu_ptr++ =  RGB2CR(*argb_ptr, *(argb_ptr+1), *(argb_ptr+2));
                *vu_ptr++  = RGB2CB(*argb_ptr, *(argb_ptr+1), *(argb_ptr+2));
                *y_ptr++    = RGB2Y(*(argb_ptr + 4), *(argb_ptr+5), *(argb_ptr+6));
                argb_ptr += 8;
            }
        } else {
            i  = width_org + 1;
            while(--i) {
                //format abgr, litter endian
                *y_ptr++ = RGB2Y(*argb_ptr, *(argb_ptr+1), *(argb_ptr+2));
                argb_ptr += 4;
            }
        }
    }
    int64_t end_encode = systemTime();
    ALOGI("rgb2yuv time: %d",(unsigned int)((end_encode-start_encode) / 1000000L));
}

static void swap_uv_component(uint32 width, uint32 height, uint8* p_uv)
{
    uint32 uv_size = width * height >> 1;
    uint32 i;
    uint8 tmp;

    for (i = 0; i < uv_size/2; i ++){
        tmp         = p_uv[2*i]  ;
        p_uv[2*i]   = p_uv[2*i+1];
        p_uv[2*i+1] = tmp        ;
    }
}

#else
/*this is neon assemble function.It is need width_org align in 16Bytes.height_org align in 2Bytes*/
/*in cpu not busy status,it deal with 1280*720 rgb data in 5-6ms */
extern "C" void neon_intrinsics_ARGB888ToYVU420Semi(uint8_t *inrgb, uint8_t* outy,uint8_t* outuv,
                    int32_t width_org, int32_t height_org, int32_t width_dst, int32_t height_dst);

/*this is neon c function.It is need width_org align in 2Bytes.height_org align in 2Bytes*/
/*like ConvertARGB888ToYVU420SemiPlanar function parameters requirement*/
/*in cpu not busy status,it deal with 1280*720 rgb data in 5-6ms */
void neon_intrinsics_ARGB888ToYVU420Semi_c(uint8_t *inrgb, uint8_t* outy,uint8_t* outuv,
                    int32_t width_org, int32_t height_org, int32_t width_dst, int32_t height_dst){
   int32_t i, j;
   uint8_t *argb_ptr = inrgb;
   uint8_t *y_ptr = outy;
   uint8_t *temp_y_ptr = y_ptr;
   uint8_t *uv_ptr = outuv;
   uint8_t *argb_tmpptr ;
   uint8x8_t r1fac = vdup_n_u8(66);
   uint8x8_t g1fac = vdup_n_u8(129);
   uint8x8_t b1fac = vdup_n_u8(25);
   uint8x8_t r2fac = vdup_n_u8(38);
   uint8x8_t g2fac = vdup_n_u8(74);
   uint8x8_t b2fac = vdup_n_u8(112);
   // int8x8_t r3fac = vdup_n_s16(112);
   uint8x8_t g3fac = vdup_n_u8(94);
   uint8x8_t b3fac = vdup_n_u8(18);

   uint8x8_t y_base = vdup_n_u8(16);
   uint8x8_t uv_base = vdup_n_u8(128);

   bool needadjustPos = true;
   //due to width_dst is align in 16Bytes.so if width_org is align in 16bytes.no need adjust pos.
   if(width_org%16 == 0){
       needadjustPos = false;
   }
   int32_t linecount=0;
   for (i = height_org; i > 0; i--) {   /////  line
      for (j = (width_org >> 3); j > 0; j--) {   ///// col
          uint8 y, cb, cr;
          int8 r, g, b;
          uint8 p_r[16],p_g[16],p_b[16];
          uint16x8_t temp;
          uint8x8_t result;
          uint8x8x2_t result_uv;
          uint8x8_t result_u;
          uint8x8_t result_v;

          uint8x8x4_t argb = vld4_u8(argb_ptr);
          temp = vmull_u8(argb.val[0],r1fac);    ///////////////////////y  0,1,2
          temp = vmlal_u8(temp,argb.val[1],g1fac);
          temp = vmlal_u8(temp,argb.val[2],b1fac);
          result = vshrn_n_u16(temp,8);
          result = vadd_u8(result,y_base);
          vst1_u8(y_ptr,result);     ////*y_ptr = y;

          if(linecount%2==0){
              temp = vmull_u8(argb.val[2],b2fac);    ///////////////////////cb
              temp = vmlsl_u8(temp,argb.val[1],g2fac);
              temp = vmlsl_u8(temp,argb.val[0],r2fac);
              result_u = vshrn_n_u16(temp,8);
              result_u = vadd_u8(result_u,uv_base);

              temp = vmull_u8(argb.val[0],b2fac);    ///////////////////////cr
              temp = vmlsl_u8(temp,argb.val[1],g3fac);
              temp = vmlsl_u8(temp,argb.val[2],b3fac);
              result_v = vshrn_n_u16(temp,8);
              result_v = vadd_u8(result_v,uv_base);

              /////uuuuuuuuvvvvvvvv -->> uvuvuvuvuvuvuvuvuv
              result_uv = vtrn_u8( result_v,result_u );
              vst1_u8(uv_ptr,result_uv.val[0]);
              uv_ptr += 8;
          }
          y_ptr += 8;
          argb_ptr += 32;
      }
      linecount++;
      if(needadjustPos){
          //note:let y,uv,argb get correct position before operating next line.
          y_ptr = outy + width_dst*linecount;
          uv_ptr = outuv + width_dst*(linecount/2);
          argb_ptr = inrgb + 4*width_org*linecount;
      }
   }
}

inline static void ConvertARGB888ToYVU420SemiPlanar_neon(uint8_t *inrgb, uint8_t* outy,uint8_t* outuv,
                    int32_t width_org, int32_t height_org, int32_t width_dst, int32_t height_dst) {
#define RGB2Y(_r, _g, _b) (((66 * (_r) + 129 * (_g) + 25 * (_b)) >> 8) + 16)
#define RGB2CB(_r, _g, _b) (((-38 * (_r) - 74 * (_g) + 112 * (_b)) >> 8) + 128)
#define RGB2CR(_r, _g, _b) (((112 * (_r) - 94 * (_g) - 18 * (_b)) >> 8) + 128)
    uint32_t i, j;
    uint32_t *argb_ptr = (uint32_t *)inrgb;
    uint8_t *y_ptr = outy;
    uint8_t *vu_ptr = outuv;

    if (NULL == inrgb || NULL == outuv || NULL==outy)
        return;

    if ((height_org & 0x1) != 0)
        height_org &= ~0x1;

    if ((width_org & 0x1) != 0) {
        ALOGE("width_org:%d is not supported", width_org);
        return;
    }

    int64_t start_encode = systemTime();
    neon_intrinsics_ARGB888ToYVU420Semi_c(inrgb,  y_ptr, vu_ptr, //  1280*720  =>  22ms in padv2
                                        width_org,  height_org,  width_dst,  height_dst);
    int64_t end_encode = systemTime();
    ALOGI("wfd: ConvertARGB888ToYVU420SemiPlanar_neon:  rgb2yuv cost time: %d",
            (unsigned int)((end_encode-start_encode) / 1000000L));
}

static void swap_uv_component(uint32 width, uint32 height, uint8* p_uv)
{
    uint32 uv_size = width * height >> 1;
    uint8x16_t data0, data1;
    uint32 i;

    for (i = 0; i < uv_size; i += 16){
        data0 = vld1q_u8(p_uv+i);
        data1 = vrev16q_u8(data0);
        vst1q_u8(p_uv+i, data1);
    }
}
#endif
inline static void ConvertARGB888ToYVU420SemiPlanar(uint8_t *inrgb, uint8_t* outy,uint8_t* outuv,
                    int32_t width_org, int32_t height_org, int32_t width_dst, int32_t height_dst) {
#ifdef CONFIG_BIA_SUPPORT
ConvertARGB888ToYVU420SemiPlanar_c(inrgb, outy, outuv, width_org, height_org, width_dst, height_dst);
#else
ConvertARGB888ToYVU420SemiPlanar_neon(inrgb, outy, outuv, width_org, height_org, width_dst, height_dst);
#endif
}

SPRDAVCEncoder::SPRDAVCEncoder(
    const char *name,
    const OMX_CALLBACKTYPE *callbacks,
    OMX_PTR appData,
    OMX_COMPONENTTYPE **component)
    : SprdSimpleOMXComponent(name, callbacks, appData, component),
      mHandle(new tagAVCHandle),
      mEncParams(new tagAVCEncParam),
      mEncConfig(new MMEncConfig),
      mSliceGroup(NULL),
      mNumInputFrames(-1),
      mPrevTimestampUs(-1),
      mSetFreqCount(0),
      mBitrate(0),
      mEncSceneMode(0),
      mEncDbFrmMode(0),
      mSetEncMode(false),
      mVideoWidth(176),
      mVideoHeight(144),
      mVideoFrameRate(30),
      mRateCtrlEnable(1),
      mVideoBitRate(192000),
      mVideoColorFormat(OMX_SPRD_COLOR_FormatYVU420SemiPlanar),
      mStoreMetaData(OMX_FALSE),
      mPrependSPSPPS(OMX_FALSE),
      mIOMMUEnabled(false),
      mIOMMUID(-1),
      mDumpYUVEnabled(false),
      mDumpStrmEnabled(false),
      mStarted(false),
      mSpsPpsHeaderReceived(false),
      mReadyForNextFrame(true),
      mSawInputEOS(false),
      mSignalledError(false),
      mKeyFrameRequested(false),
      mIschangebitrate(false),
      mUVExchange(false),
      mPbuf_inter(NULL),
      mPbuf_yuv_v(NULL),
      mPbuf_yuv_p(0),
      mPbuf_yuv_size(0),
      mPbuf_stream_v(NULL),
      mPbuf_stream_p(0),
      mPbuf_stream_size(0),
      mPbuf_stream_v_pn(NULL),
      mPbuf_stream_p_pn(0),
      mPbuf_stream_size_pn(0),
      mPbuf_extra_v(NULL),
      mPbuf_extra_p(0),
      mPbuf_extra_size(0),
      mAVCEncProfile(AVC_BASELINE),
      mAVCEncLevel(AVC_LEVEL2),
      mPFrames(29),
      mNeedAlign(true),
      mLibHandle(NULL),
      mH264EncPreInit(NULL),
      mH264EncInit(NULL),
      mH264EncSetConf(NULL),
      mH264EncGetConf(NULL),
      mH264EncStrmEncode(NULL),
      mH264EncGenHeader(NULL),
      mH264EncRelease(NULL),
      mH264EncGetCodecCapability(NULL),
      mH264EncGetIOVA(NULL),
      mH264EncFreeIOVA(NULL),
      mH264EncGetIOMMUStatus(NULL),
      mH264Enc_NeedAlign(NULL),
      mFrameWidth(176),
      mFrameHeight(144),
      mSupportRGBEnc(false) {

    ALOGI("Construct SPRDAVCEncoder, this: 0x%p", (void *)this);

    CHECK(mHandle != NULL);
    memset(mHandle, 0, sizeof(tagAVCHandle));

    mHandle->videoEncoderData = NULL;
    mHandle->userData = this;
    mHandle->VSP_FlushBSCache = FlushCacheWrapper;

    memset(&mEncInfo, 0, sizeof(mEncInfo));

#ifdef CONFIG_SPRD_RECORD_EIS
    mEISMode = false;
#endif
    CHECK_EQ(openEncoder("libomx_avcenc_hw_sprd.so"), true);

    ALOGI("%s, line:%d, name: %s", __FUNCTION__, __LINE__, name);

    char value_dump[PROPERTY_VALUE_MAX];

    property_get("vendor.h264enc.yuv.dump", value_dump, "false");
    mDumpYUVEnabled = !strcmp(value_dump, "true");

    property_get("vendor.h264enc.strm.dump", value_dump, "false");
    mDumpStrmEnabled = !strcmp(value_dump, "true");
    ALOGI("%s, mDumpYUVEnabled: %d, mDumpStrmEnabled: %d", __FUNCTION__, mDumpYUVEnabled, mDumpStrmEnabled);

    MMCodecBuffer InterMemBfr;
    uint32_t size_inter = H264ENC_INTERNAL_BUFFER_SIZE;

    mPbuf_inter = (uint8_t *)malloc(size_inter);
    CHECK(mPbuf_inter != NULL);
    InterMemBfr.common_buffer_ptr = mPbuf_inter;
    InterMemBfr.common_buffer_ptr_phy= 0;
    InterMemBfr.size = size_inter;

    CHECK_EQ((*mH264EncPreInit)(mHandle, &InterMemBfr), MMENC_OK);

    CHECK_EQ ((*mH264EncGetCodecCapability)(mHandle, &mCapability), MMENC_OK);

    initPorts();
    ALOGI("Construct SPRDAVCEncoder, Capability: profile %d, level %d, max wh=%d %d",
          mCapability.profile, mCapability.level, mCapability.max_width, mCapability.max_height);
#ifdef CONFIG_RGB_ENC_SUPPORT
    mSupportRGBEnc = true;
#endif
    int ret = (*mH264Enc_NeedAlign)(mHandle);
    ALOGI("Enc needAign:%d",ret);
    if (ret == 0) {
        mNeedAlign = false;
    }
    ret = (*mH264EncGetIOMMUStatus)(mHandle);
    ALOGI("Get IOMMU Status: %d(%s)", ret, strerror(errno));
    if (ret < 0) {
        mIOMMUEnabled = false;
    } else {
        mIOMMUEnabled = true;
    }

    int64_t start_decode = systemTime();

    if(mDumpYUVEnabled){
        char s1[100];
        sprintf(s1,"/data/misc/media/video_in_%p_%lld.yuv",(void *)this,start_decode);
        mFile_yuv = fopen(s1, "wb");
        ALOGI("yuv file name %s",s1);
    }

    if(mDumpStrmEnabled){
        char s2[100];
        sprintf(s2,"/data/misc/media/video_es_%p_%lld.h264",(void *)this,start_decode);
        ALOGI("bs file name %s",s2);
        mFile_bs = fopen(s2, "wb");
    }
}

SPRDAVCEncoder::~SPRDAVCEncoder() {
    ALOGI("Destruct SPRDAVCEncoder, this: 0x%p", (void *)this);

    releaseResource();

    releaseEncoder();

    List<BufferInfo *> &outQueue = getPortQueue(1);
    List<BufferInfo *> &inQueue = getPortQueue(0);
    CHECK(outQueue.empty());
    CHECK(inQueue.empty());

    if (mLibHandle) {
        dlclose(mLibHandle);
        mLibHandle = NULL;
    }

    if(mDumpYUVEnabled){
        if (mFile_yuv) {
            fclose(mFile_yuv);
            mFile_yuv = NULL;
        }
    }

     if(mDumpStrmEnabled){
        if (mFile_bs) {
            fclose(mFile_bs);
            mFile_bs = NULL;
        }
    }
}

OMX_ERRORTYPE SPRDAVCEncoder::initEncParams() {
    CHECK(mEncConfig != NULL);
    memset(mEncConfig, 0, sizeof(MMEncConfig));

    CHECK(mEncParams != NULL);
    memset(mEncParams, 0, sizeof(tagAVCEncParam));
    mEncParams->rate_control = AVC_ON;
    mEncParams->initQP = 0;
    mEncParams->init_CBP_removal_delay = 1600;

    mEncParams->intramb_refresh = 0;
    mEncParams->auto_scd = AVC_ON;
    mEncParams->out_of_band_param_set = AVC_ON;
    mEncParams->poc_type = 2;
    mEncParams->log2_max_poc_lsb_minus_4 = 12;
    mEncParams->delta_poc_zero_flag = 0;
    mEncParams->offset_poc_non_ref = 0;
    mEncParams->offset_top_bottom = 0;
    mEncParams->num_ref_in_cycle = 0;
    mEncParams->offset_poc_ref = NULL;

    mEncParams->num_ref_frame = 1;
    mEncParams->num_slice_group = 1;
    mEncParams->fmo_type = 0;

    mEncParams->db_filter = AVC_ON;
    mEncParams->disable_db_idc = 0;

    mEncParams->alpha_offset = 0;
    mEncParams->beta_offset = 0;
    mEncParams->constrained_intra_pred = AVC_OFF;

    mEncParams->data_par = AVC_OFF;
    mEncParams->fullsearch = AVC_OFF;
    mEncParams->search_range = 16;
    mEncParams->sub_pel = AVC_OFF;
    mEncParams->submb_pred = AVC_OFF;
    mEncParams->rdopt_mode = AVC_OFF;
    mEncParams->bidir_pred = AVC_OFF;
    mEncParams->use_overrun_buffer = AVC_OFF;

    MMCodecBuffer ExtraMemBfr;
    MMCodecBuffer StreamMemBfr, StreamMemBfrPn;
    unsigned long phy_addr = 0;
    size_t size = 0;
    size_t size_of_yuv = ((mVideoWidth+15)&(~15)) * ((mVideoHeight+15)&(~15)) * 3/2;

    size_t size_extra = size_of_yuv << 1;
    size_extra += (406*2*sizeof(uint32));
    size_extra += 1024;
#ifdef CONFIG_SPRD_RECORD_EIS
    if (mEISMode) {
        size_extra += 1024*5;
    }
#endif
    if (mIOMMUEnabled) {
        mPmem_extra = new MemIon("/dev/ion", size_extra, MemIon::NO_CACHING, ION_HEAP_ID_MASK_SYSTEM);
    } else {
        mPmem_extra = new MemIon("/dev/ion", size_extra, MemIon::NO_CACHING, ION_HEAP_ID_MASK_MM);
    }

    int fd = mPmem_extra->getHeapID();
    if (fd < 0) {
        ALOGE("Failed to alloc extra buffer (%zd), getHeapID failed", size_extra);
        return OMX_ErrorInsufficientResources;
    }

    int ret;
    if (mIOMMUEnabled) {
        ret = (*mH264EncGetIOVA)(mHandle, fd, &phy_addr, &size);
    } else {
        ret = mPmem_extra->get_phy_addr_from_ion(&phy_addr, &size);
    }
    if (ret < 0) {
        ALOGE("Failed to alloc extra buffer, get phy addr failed");
        return OMX_ErrorInsufficientResources;
    }

    mPbuf_extra_v = (uint8_t*)mPmem_extra->getBase();
    mPbuf_extra_p = phy_addr;
    mPbuf_extra_size = size;

    size_t size_stream = size_of_yuv >> 1;
    if (mIOMMUEnabled) {
        mPmem_stream = new MemIon("/dev/ion", size_stream, 0 , (1<<31) |ION_HEAP_ID_MASK_SYSTEM);
    } else {
        mPmem_stream = new MemIon("/dev/ion", size_stream, 0 , (1<<31) |ION_HEAP_ID_MASK_MM);
    }

    fd = mPmem_stream->getHeapID();
    if (fd < 0) {
        ALOGE("Failed to alloc stream buffer (%zd), getHeapID failed", size_stream);
        return OMX_ErrorInsufficientResources;
    }

    if (mIOMMUEnabled) {
        ret = (*mH264EncGetIOVA)(mHandle, fd, &phy_addr, &size);
    } else {
        ret = mPmem_stream->get_phy_addr_from_ion(&phy_addr, &size);
    }
    if (ret < 0) {
        ALOGE("Failed to alloc stream buffer, get phy addr failed");
        return OMX_ErrorInsufficientResources;
    }

    mPbuf_stream_v = (uint8_t*)mPmem_stream->getBase();
    mPbuf_stream_p = phy_addr;
    mPbuf_stream_size = size;

    size_t size_stream_pn = size_of_yuv >> 1;
    if(mEncDbFrmMode){
        if (mIOMMUEnabled) {
            mPmem_stream_pn = new MemIon("/dev/ion", size_stream_pn, 0 , (1<<31) |ION_HEAP_ID_MASK_SYSTEM);
        } else {
            mPmem_stream_pn = new MemIon("/dev/ion", size_stream_pn, 0 , (1<<31) |ION_HEAP_ID_MASK_MM);
        }

        fd = mPmem_stream_pn->getHeapID();
        if (fd < 0) {
            ALOGE("Failed to alloc stream buffer (%zd), getHeapID failed", size_stream_pn);
            return OMX_ErrorInsufficientResources;
        }

        if (mIOMMUEnabled) {
            ret = (*mH264EncGetIOVA)(mHandle, fd, &phy_addr, &size);
        } else {
            ret = mPmem_stream_pn->get_phy_addr_from_ion(&phy_addr, &size);
        }
        if (ret < 0) {
            ALOGE("Failed to alloc stream buffer, get phy addr failed");
            return OMX_ErrorInsufficientResources;
        }

        mPbuf_stream_v_pn = (uint8_t*)mPmem_stream_pn->getBase();
        mPbuf_stream_p_pn = phy_addr;
        mPbuf_stream_size_pn = size;
    }

    ExtraMemBfr.common_buffer_ptr = mPbuf_extra_v;
    ExtraMemBfr.common_buffer_ptr_phy = mPbuf_extra_p;
    ExtraMemBfr.size = size_extra;

    StreamMemBfr.common_buffer_ptr = mPbuf_stream_v;
    StreamMemBfr.common_buffer_ptr_phy = mPbuf_stream_p;
    StreamMemBfr.size = size_stream;

    StreamMemBfrPn.common_buffer_ptr = mPbuf_stream_v_pn;
    StreamMemBfrPn.common_buffer_ptr_phy = mPbuf_stream_p_pn;
    StreamMemBfrPn.size = size_stream_pn;

    mEncInfo.frame_width = mFrameWidth;
    mEncInfo.frame_height = mFrameHeight;
    mEncInfo.EISMode = false;
#ifdef CONFIG_SPRD_RECORD_EIS
    mEncInfo.EISMode = mEISMode;
#endif
    mEncInfo.is_h263 = 0;
    mEncInfo.org_width = mVideoWidth;
    mEncInfo.org_height = mVideoHeight;
    if (mVideoColorFormat == OMX_COLOR_FormatYUV420SemiPlanar) {
        mEncInfo.yuv_format = MMENC_YUV420SP_NV12;
    } else {
        mEncInfo.yuv_format = MMENC_YUV420SP_NV21;
    }

    mEncInfo.time_scale = 1000;

    if ((*mH264EncInit)(mHandle, &ExtraMemBfr,&StreamMemBfr, &StreamMemBfrPn, &mEncInfo)) {
        ALOGE("Failed to init mp4enc");
        return OMX_ErrorUndefined;
    }

    if ((*mH264EncGetConf)(mHandle, mEncConfig)) {
        ALOGE("Failed to get default encoding parameters");
        return OMX_ErrorUndefined;
    }

    mEncConfig->h263En = 0;
    mEncConfig->RateCtrlEnable = mRateCtrlEnable;
    mEncConfig->targetBitRate = mVideoBitRate;
    mEncConfig->FrameRate = mVideoFrameRate;
    mEncConfig->PFrames = mPFrames;
    if (mEncSceneMode == 0) {
        mEncConfig->QP_IVOP = 28;
    }else {
        mEncConfig->QP_IVOP = 36;
    }
    mEncConfig->QP_PVOP = 28;
    mEncConfig->vbv_buf_size = mVideoBitRate/2;
    mEncConfig->profileAndLevel = ( (mAVCEncProfile &0xff) <<8) | mAVCEncLevel;
    ALOGI("mEncConfig->profileAndLevel %x", mEncConfig->profileAndLevel);
    mEncConfig->PrependSPSPPSEnalbe = ((mPrependSPSPPS == OMX_FALSE) ? 0 : 1);
    mEncConfig->EncSceneMode = mEncSceneMode;
    mEncConfig->EncDbFrmMode  = mEncDbFrmMode;
    if ((*mH264EncSetConf)(mHandle, mEncConfig)) {
        ALOGE("Failed to set default encoding parameters");
        return OMX_ErrorUndefined;
    }

    mEncParams->width = mVideoWidth;
    mEncParams->height = mVideoHeight;
    mEncParams->bitrate = mVideoBitRate;
    mEncParams->frame_rate = 1000 * mVideoFrameRate;  // In frames/ms!
    mEncParams->CPB_size = (uint32_t) (mVideoBitRate >> 1);

    // Set profile and level
    mEncParams->profile = mAVCEncProfile;
    mEncParams->level = mAVCEncLevel;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE SPRDAVCEncoder::initEncoder() {
    CHECK(!mStarted);

    OMX_ERRORTYPE errType = OMX_ErrorNone;
    if (OMX_ErrorNone != (errType = initEncParams())) {
        ALOGE("Failed to initialized encoder params");
        mSignalledError = true;
        notify(OMX_EventError, OMX_ErrorUndefined, 0, 0);
        return errType;
    }

    mNumInputFrames = -2;  // 1st two buffers contain SPS and PPS
    mSpsPpsHeaderReceived = false;
    mReadyForNextFrame = true;
    mStarted = true;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE SPRDAVCEncoder::releaseEncoder() {

    (*mH264EncRelease)(mHandle);

    if (mPbuf_inter != NULL) {
        free(mPbuf_inter);
        mPbuf_inter = NULL;
    }

    delete mEncParams;
    mEncParams = NULL;

    delete mEncConfig;
    mEncConfig = NULL;

    delete mHandle;
    mHandle = NULL;

    mStarted = false;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE SPRDAVCEncoder::releaseResource() {

    if (mPbuf_extra_v != NULL) {
        if (mIOMMUEnabled) {
            (*mH264EncFreeIOVA)(mHandle, mPbuf_extra_p, mPbuf_extra_size);
        }
        mPmem_extra.clear();
        mPbuf_extra_v = NULL;
        mPbuf_extra_p = 0;
        mPbuf_extra_size = 0;
    }

    if (mPbuf_stream_v != NULL) {
        if (mIOMMUEnabled) {
            (*mH264EncFreeIOVA)(mHandle, mPbuf_stream_p, mPbuf_stream_size);
        }
        mPmem_stream.clear();
        mPbuf_stream_v = NULL;
        mPbuf_stream_p = 0;
        mPbuf_stream_size = 0;
    }

    if (mPbuf_stream_v_pn != NULL) {
        if (mIOMMUEnabled) {
            (*mH264EncFreeIOVA)(mHandle, mPbuf_stream_p_pn, mPbuf_stream_size_pn);
        }
        mPmem_stream_pn.clear();
        mPbuf_stream_v_pn= NULL;
        mPbuf_stream_p_pn= 0;
        mPbuf_stream_size_pn= 0;
    }

    if (mPbuf_yuv_v != NULL) {
        if (mIOMMUEnabled) {
            (*mH264EncFreeIOVA)(mHandle, mPbuf_yuv_p, mPbuf_yuv_size);
        }
        mYUVInPmemHeap.clear();
        mPbuf_yuv_v = NULL;
        mPbuf_yuv_p = 0;
        mPbuf_yuv_size = 0;
    }

    return OMX_ErrorNone;
}

void SPRDAVCEncoder::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    const size_t kInputBufferSize = (mVideoWidth * mVideoHeight * 3) >> 1;
    const size_t kOutputBufferSize = kInputBufferSize >> 1;

    def.nPortIndex = 0;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = kInputBufferSize;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    def.format.video.cMIMEType = const_cast<char *>("video/raw");
    def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    def.format.video.eColorFormat = OMX_SPRD_COLOR_FormatYVU420SemiPlanar;
    def.format.video.xFramerate = (mVideoFrameRate << 16);  // Q16 format
    def.format.video.nBitrate = mVideoBitRate;
    def.format.video.nFrameWidth = mVideoWidth;
    def.format.video.nFrameHeight = mVideoHeight;
    def.format.video.nStride = mVideoWidth;
    def.format.video.nSliceHeight = mVideoHeight;

    addPort(def);

    def.nPortIndex = 1;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = kNumBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = kOutputBufferSize;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.video.cMIMEType = const_cast<char *>("video/avc");
    def.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
    def.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    def.format.video.xFramerate = (0 << 16);  // Q16 format
    def.format.video.nBitrate = mVideoBitRate;
    def.format.video.nFrameWidth = mVideoWidth;
    def.format.video.nFrameHeight = mVideoHeight;
    def.format.video.nStride = mVideoWidth;
    def.format.video.nSliceHeight = mVideoHeight;

    addPort(def);
}

OMX_ERRORTYPE SPRDAVCEncoder::internalGetParameter(
    OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
    case OMX_IndexParamVideoErrorCorrection:
    {
        return OMX_ErrorNotImplemented;
    }

    case OMX_IndexParamVideoBitrate:
    {
        OMX_VIDEO_PARAM_BITRATETYPE *bitRate =
            (OMX_VIDEO_PARAM_BITRATETYPE *) params;

        if (bitRate->nPortIndex != 1) {
            return OMX_ErrorUndefined;
        }

        bitRate->eControlRate = OMX_Video_ControlRateVariable;
        bitRate->nTargetBitrate = mVideoBitRate;
        return OMX_ErrorNone;
    }

    case OMX_IndexParamVideoPortFormat:
    {
        OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
            (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

        if (formatParams->nPortIndex > 1) {
            return OMX_ErrorUndefined;
        }

        if (formatParams->nIndex > 3) {
            return OMX_ErrorNoMore;
        }

        if (formatParams->nPortIndex == 0) {
            formatParams->eCompressionFormat = OMX_VIDEO_CodingUnused;
            if (formatParams->nIndex == 0) {
                formatParams->eColorFormat = OMX_SPRD_COLOR_FormatYVU420SemiPlanar;
            } else if (formatParams->nIndex == 1) {
                formatParams->eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
            } else if (formatParams->nIndex == 2) {
                formatParams->eColorFormat = OMX_COLOR_FormatYUV420Planar;
            } else {
                formatParams->eColorFormat = OMX_COLOR_FormatAndroidOpaque;
            }
        } else {
            formatParams->eCompressionFormat = OMX_VIDEO_CodingAVC;
            formatParams->eColorFormat = OMX_COLOR_FormatUnused;
        }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamVideoAvc:
    {
        OMX_VIDEO_PARAM_AVCTYPE *avcParams =
            (OMX_VIDEO_PARAM_AVCTYPE *)params;

        if (avcParams->nPortIndex != 1) {
            return OMX_ErrorUndefined;
        }

        avcParams->eProfile = OMX_VIDEO_AVCProfileBaseline;
        OMX_U32 omxLevel = AVC_LEVEL2;
        if (OMX_ErrorNone !=
                ConvertAvcSpecLevelToOmxAvcLevel(mAVCEncLevel, &omxLevel)) {
            return OMX_ErrorUndefined;
        }

        avcParams->eLevel = (OMX_VIDEO_AVCLEVELTYPE) omxLevel;
        avcParams->nRefFrames = 1;
        avcParams->nBFrames = 0;
        avcParams->bUseHadamard = OMX_TRUE;
        avcParams->nAllowedPictureTypes =
            (OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP);
        avcParams->nRefIdx10ActiveMinus1 = 0;
        avcParams->nRefIdx11ActiveMinus1 = 0;
        avcParams->bWeightedPPrediction = OMX_FALSE;
        avcParams->bEntropyCodingCABAC = OMX_FALSE;
        avcParams->bconstIpred = OMX_FALSE;
        avcParams->bDirect8x8Inference = OMX_FALSE;
        avcParams->bDirectSpatialTemporal = OMX_FALSE;
        avcParams->nCabacInitIdc = 0;
        return OMX_ErrorNone;
    }

    case OMX_IndexParamVideoProfileLevelQuerySupported:
    {
        OMX_VIDEO_PARAM_PROFILELEVELTYPE *profileLevel =
            (OMX_VIDEO_PARAM_PROFILELEVELTYPE *)params;

        if (profileLevel->nPortIndex != 1) {
            return OMX_ErrorUndefined;
        }

        size_t index = profileLevel->nProfileIndex;
        size_t nProfileLevels =
            sizeof(kProfileLevels) / sizeof(kProfileLevels[0]);
        if (index >= nProfileLevels) {
            return OMX_ErrorNoMore;
        }

        profileLevel->eProfile = kProfileLevels[index].mProfile;
        profileLevel->eLevel = kProfileLevels[index].mLevel;

        if (profileLevel->eProfile == OMX_VIDEO_AVCProfileHigh) {
            if (mCapability.profile < AVC_HIGH) {
                profileLevel->eProfile = OMX_VIDEO_AVCProfileMain;
            }
        }

        if (profileLevel->eProfile == OMX_VIDEO_AVCProfileMain) {
            if (mCapability.profile < AVC_MAIN) {
                profileLevel->eProfile = OMX_VIDEO_AVCProfileBaseline;
            }
        }

        const size_t size =
            sizeof(ConversionTable) / sizeof(ConversionTable[0]);

        for (index = 0; index < size; index++) {
            if (ConversionTable[index].avcLevel > mCapability.level) {
                index--;
                break;
            }
        }

        if (index < size && profileLevel->eLevel > ConversionTable[index].omxLevel) {
            profileLevel->eLevel = ConversionTable[index].omxLevel;
        }
        return OMX_ErrorNone;
    }

    case OMX_IndexParamStoreMetaDataBuffer:
    {
        StoreMetaDataInBuffersParams *pStoreMetaData = (StoreMetaDataInBuffersParams *)params;
        if (pStoreMetaData->nPortIndex != 0) {
            ALOGE("%s: StoreMetadataInBuffersParams.nPortIndex not zero!",
                    __FUNCTION__);
            return OMX_ErrorUndefined;
        }

        pStoreMetaData->bStoreMetaData = mStoreMetaData;
        return OMX_ErrorNone;
    }

    case OMX_IndexParamDescribeColorFormat:
    {
        DescribeColorFormatParams *pDescribeColorFormat = (DescribeColorFormatParams *)params;

        MediaImage &image = pDescribeColorFormat->sMediaImage;
        memset(&image, 0, sizeof(image));

        image.mType = MediaImage::MEDIA_IMAGE_TYPE_UNKNOWN;
        image.mNumPlanes = 0;

        const OMX_COLOR_FORMATTYPE fmt = pDescribeColorFormat->eColorFormat;
        image.mWidth = pDescribeColorFormat->nFrameWidth;
        image.mHeight = pDescribeColorFormat->nFrameHeight;

        ALOGI("%s, DescribeColorFormat: 0x%x, w h = %d %d", __FUNCTION__,
                pDescribeColorFormat->eColorFormat,
                pDescribeColorFormat->nFrameWidth, pDescribeColorFormat->nFrameHeight);

        if (fmt != OMX_SPRD_COLOR_FormatYVU420SemiPlanar &&
            fmt != OMX_COLOR_FormatYUV420SemiPlanar &&
            fmt != OMX_COLOR_FormatYUV420Planar) {
            ALOGW("do not know color format 0x%x = %d", fmt, fmt);
            return OMX_ErrorUnsupportedSetting;
        }

        // TEMPORARY FIX for some vendors that advertise sliceHeight as 0
        if (pDescribeColorFormat->nStride != 0 && pDescribeColorFormat->nSliceHeight == 0) {
            ALOGW("using sliceHeight=%u instead of what codec advertised (=0)",
                    pDescribeColorFormat->nFrameHeight);
            pDescribeColorFormat->nSliceHeight = pDescribeColorFormat->nFrameHeight;
        }

        // we need stride and slice-height to be non-zero
        if (pDescribeColorFormat->nStride == 0 || pDescribeColorFormat->nSliceHeight == 0) {
            ALOGW("cannot describe color format 0x%x = %d with stride=%u and sliceHeight=%u",
                    fmt, fmt, pDescribeColorFormat->nStride, pDescribeColorFormat->nSliceHeight);
            return OMX_ErrorBadParameter;
        }

        // set-up YUV format
        image.mType = MediaImage::MEDIA_IMAGE_TYPE_YUV;
        image.mNumPlanes = 3;
        image.mBitDepth = 8;
        image.mPlane[image.Y].mOffset = 0;
        image.mPlane[image.Y].mColInc = 1;
        image.mPlane[image.Y].mRowInc = pDescribeColorFormat->nStride;
        image.mPlane[image.Y].mHorizSubsampling = 1;
        image.mPlane[image.Y].mVertSubsampling = 1;

        switch (fmt) {
            case OMX_SPRD_COLOR_FormatYVU420SemiPlanar:
            // NV21
            image.mPlane[image.V].mOffset = pDescribeColorFormat->nStride*pDescribeColorFormat->nSliceHeight;
            image.mPlane[image.V].mColInc = 2;
            image.mPlane[image.V].mRowInc = pDescribeColorFormat->nStride;
            image.mPlane[image.V].mHorizSubsampling = 2;
            image.mPlane[image.V].mVertSubsampling = 2;

            image.mPlane[image.U].mOffset = image.mPlane[image.V].mOffset + 1;
            image.mPlane[image.U].mColInc = 2;
            image.mPlane[image.U].mRowInc = pDescribeColorFormat->nStride;
            image.mPlane[image.U].mHorizSubsampling = 2;
            image.mPlane[image.U].mVertSubsampling = 2;
            break;

            case OMX_COLOR_FormatYUV420SemiPlanar:
                // FIXME: NV21 for sw-encoder, NV12 for decoder and hw-encoder
                // NV12
                image.mPlane[image.U].mOffset = pDescribeColorFormat->nStride*pDescribeColorFormat->nSliceHeight;
                image.mPlane[image.U].mColInc = 2;
                image.mPlane[image.U].mRowInc = pDescribeColorFormat->nStride;
                image.mPlane[image.U].mHorizSubsampling = 2;
                image.mPlane[image.U].mVertSubsampling = 2;

                image.mPlane[image.V].mOffset = image.mPlane[image.U].mOffset + 1;
                image.mPlane[image.V].mColInc = 2;
                image.mPlane[image.V].mRowInc = pDescribeColorFormat->nStride;
                image.mPlane[image.V].mHorizSubsampling = 2;
                image.mPlane[image.V].mVertSubsampling = 2;
                break;

            case OMX_COLOR_FormatYUV420Planar: // used for YV12
                image.mPlane[image.U].mOffset = pDescribeColorFormat->nStride*pDescribeColorFormat->nSliceHeight;
                image.mPlane[image.U].mColInc = 1;
                image.mPlane[image.U].mRowInc = pDescribeColorFormat->nStride / 2;
                image.mPlane[image.U].mHorizSubsampling = 2;
                image.mPlane[image.U].mVertSubsampling = 2;

                image.mPlane[image.V].mOffset = image.mPlane[image.U].mOffset
                        + (pDescribeColorFormat->nStride * pDescribeColorFormat->nSliceHeight / 4);
                image.mPlane[image.V].mColInc = 1;
                image.mPlane[image.V].mRowInc = pDescribeColorFormat->nStride / 2;
                image.mPlane[image.V].mHorizSubsampling = 2;
                image.mPlane[image.V].mVertSubsampling = 2;
                break;

            default:
                TRESPASS();
        }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamVideoInit:
    {
        OMX_PORT_PARAM_TYPE* pParam = (OMX_PORT_PARAM_TYPE*)params;
        pParam->nPorts = 2;
        pParam->nStartPortNumber = 0;
        return OMX_ErrorNone;
    }

    default:
        return SprdSimpleOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SPRDAVCEncoder::internalSetParameter(
    OMX_INDEXTYPE index, const OMX_PTR params) {

    ALOGI("%s, %d, cmd index: 0x%x",__FUNCTION__,__LINE__,index);
    switch (index) {
    case OMX_IndexParamVideoErrorCorrection:
    {
        return OMX_ErrorNotImplemented;
    }

    case OMX_IndexConfigVideoRecordMode:
    {
        OMX_BOOL *pEnable = (OMX_BOOL *)params;
#ifdef CONFIG_SPRD_RECORD_EIS
        if (*pEnable == 1) {
            mEISMode = OMX_TRUE;
        }
        ALOGI("record eismode=%d",mEISMode);
#endif
        return OMX_ErrorNone;
    }

    case OMX_IndexParamVideoBitrate:
    {
        OMX_VIDEO_PARAM_BITRATETYPE *bitRate =
            (OMX_VIDEO_PARAM_BITRATETYPE *) params;
        if (bitRate->nPortIndex != 1 ||
                (bitRate->eControlRate != OMX_Video_ControlRateDisable && \
                bitRate->eControlRate != OMX_Video_ControlRateVariable && \
                bitRate->eControlRate != OMX_Video_ControlRateConstant)) {
            ALOGI("OMX_ErrorUndefined bitRate->eControlRate %d",bitRate->eControlRate);
            return OMX_ErrorUndefined;
        }

        mVideoBitRate = bitRate->nTargetBitrate;

        if(bitRate->eControlRate == OMX_Video_ControlRateDisable)
        {
            mRateCtrlEnable = 0;
        }else{
            mRateCtrlEnable = 1;
       }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamPortDefinition:
    {
        OMX_PARAM_PORTDEFINITIONTYPE *def =
            (OMX_PARAM_PORTDEFINITIONTYPE *)params;
        if (def->nPortIndex > 1) {
            return OMX_ErrorUndefined;
        }

        if (def->nPortIndex == 0) {
            if (def->format.video.eCompressionFormat != OMX_VIDEO_CodingUnused ||
                    (def->format.video.eColorFormat != OMX_COLOR_FormatYUV420Flexible &&
                     def->format.video.eColorFormat != OMX_SPRD_COLOR_FormatYVU420SemiPlanar &&
                     def->format.video.eColorFormat != OMX_COLOR_FormatYUV420SemiPlanar &&
                     def->format.video.eColorFormat != OMX_COLOR_FormatYUV420Planar &&
                     def->format.video.eColorFormat != OMX_COLOR_FormatAndroidOpaque)) {
                return OMX_ErrorUndefined;
            }
        } else {
            if (def->format.video.eCompressionFormat != OMX_VIDEO_CodingAVC ||
                    (def->format.video.eColorFormat != OMX_COLOR_FormatUnused)) {
                return OMX_ErrorUndefined;
            }
        }
        int32_t stride, sliceHeight;
        if (mNeedAlign) {
            sliceHeight = (def->format.video.nFrameHeight+15)&(~15);
        } else {
            sliceHeight = def->format.video.nFrameHeight;
        }
        stride = (def->format.video.nFrameWidth+15)&(~15);
        // As encoder we should modify bufferSize on Input port
        // Make sure we have enough input date for input buffer
        if(def->nPortIndex <= 1) {
            uint32_t bufferSize = stride * sliceHeight * 3/2;
            if(bufferSize > def->nBufferSize) {
                def->nBufferSize = bufferSize;
            }
        }
        //translate Flexible 8-bit YUV format to our default YUV format
        if (def->format.video.eColorFormat == OMX_COLOR_FormatYUV420Flexible) {
            ALOGI("internalSetParameter, translate Flexible 8-bit YUV format to SPRD YVU420SemiPlanar");
            def->format.video.eColorFormat = OMX_SPRD_COLOR_FormatYVU420SemiPlanar;
        }

        OMX_ERRORTYPE err = SprdSimpleOMXComponent::internalSetParameter(index, params);
        if (OMX_ErrorNone != err) {
            return err;
        }

        if (def->nPortIndex == 0) {
            mFrameWidth = def->format.video.nFrameWidth;
            mFrameHeight = def->format.video.nFrameHeight;
            mVideoWidth = stride;
            mVideoHeight = sliceHeight;
            mVideoFrameRate = def->format.video.xFramerate >> 16;
            mVideoColorFormat = def->format.video.eColorFormat;
        } else {
            mVideoBitRate = def->format.video.nBitrate;
        }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamStandardComponentRole:
    {
        const OMX_PARAM_COMPONENTROLETYPE *roleParams =
            (const OMX_PARAM_COMPONENTROLETYPE *)params;

        if (strncmp((const char *)roleParams->cRole,
                    "video_encoder.avc",
                    OMX_MAX_STRINGNAME_SIZE - 1)) {
            return OMX_ErrorUndefined;
        }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamVideoPortFormat:
    {
        const OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
            (const OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

        if (formatParams->nPortIndex > 1) {
            return OMX_ErrorUndefined;
        }

        if (formatParams->nPortIndex == 0) {
            if (formatParams->eCompressionFormat != OMX_VIDEO_CodingUnused ||
                  (formatParams->eColorFormat != OMX_SPRD_COLOR_FormatYVU420SemiPlanar &&
                   formatParams->eColorFormat != OMX_COLOR_FormatYUV420SemiPlanar &&
                   formatParams->eColorFormat != OMX_COLOR_FormatYUV420Planar &&
                   formatParams->eColorFormat != OMX_COLOR_FormatAndroidOpaque)){
                return OMX_ErrorUndefined;
            }
            mVideoColorFormat = formatParams->eColorFormat;
        } else {
            if (formatParams->eCompressionFormat != OMX_VIDEO_CodingAVC ||
                    formatParams->eColorFormat != OMX_COLOR_FormatUnused) {
                return OMX_ErrorUndefined;
            }
        }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamVideoAvc:
    {
        OMX_VIDEO_PARAM_AVCTYPE *avcType =
            (OMX_VIDEO_PARAM_AVCTYPE *)params;

        if (avcType->nPortIndex != 1) {
            return OMX_ErrorUndefined;
        }

        mPFrames = avcType->nPFrames;
        ALOGI("%s, mPFrames: %d",__FUNCTION__,mPFrames);

        if (OK != ConvertOmxAvcProfileToAvcSpecProfile(avcType->eProfile, &mAVCEncProfile)) {
            ALOGE("ConvertOmxAvcProfileToAvcSpecProfile error");
            return OMX_ErrorUndefined;
        }

        if (OK != ConvertOmxAvcLevelToAvcSpecLevel(avcType->eLevel, &mAVCEncLevel)) {
            ALOGE("ConvertOmxAvcLevelToAvcSpecLevel error");
            return OMX_ErrorUndefined;
        }

        return OMX_ErrorNone;
    }

    case OMX_IndexParamStoreMetaDataBuffer:
    {
        StoreMetaDataInBuffersParams *pStoreMetaData = (StoreMetaDataInBuffersParams *)params;
        if (pStoreMetaData->nPortIndex != 0) {
            ALOGE("%s: StoreMetadataInBuffersParams.nPortIndex not zero!",
                    __FUNCTION__);
            return OMX_ErrorUndefined;
        }

        mStoreMetaData = pStoreMetaData->bStoreMetaData;
        ALOGV("StoreMetaDataInBuffers set to: %s",
                mStoreMetaData ? " true" : "false");
        return OMX_ErrorNone;
    }

    case OMX_IndexParamPrependSPSPPSToIDR:
    {
        PrependSPSPPSToIDRFramesParams *pPrependSPSPPS = (PrependSPSPPSToIDRFramesParams *)params;
        mPrependSPSPPS = pPrependSPSPPS->bEnable; // currently avcenc driver will PrependSPSPPS defaultly
        ALOGI("OMX_IndexParamPrependSPSPPSToIDR return ok");
        return OMX_ErrorNone;
    }

    case OMX_IndexParamVideoIntraRefresh:
    {
        return OMX_ErrorNone;   ///hw encoder may not support this mode
    }

    default:
        return SprdSimpleOMXComponent::internalSetParameter(index, params);
    }
}

OMX_ERRORTYPE SPRDAVCEncoder::setConfig(
    OMX_INDEXTYPE index, const OMX_PTR params) {
    switch (index) {
        case OMX_IndexConfigVideoIntraVOPRefresh:
        {
            OMX_CONFIG_INTRAREFRESHVOPTYPE *pConfigIntraRefreshVOP =
                (OMX_CONFIG_INTRAREFRESHVOPTYPE *)params;

            if (pConfigIntraRefreshVOP->nPortIndex != kOutputPortIndex) {
                return OMX_ErrorBadPortIndex;
            }

            mKeyFrameRequested = pConfigIntraRefreshVOP->IntraRefreshVOP;
            return OMX_ErrorNone;
        }
        case OMX_IndexConfigVideoBitrate:
        {
            OMX_VIDEO_CONFIG_BITRATETYPE *pConfigParams=
             (OMX_VIDEO_CONFIG_BITRATETYPE *)params;
            if (pConfigParams->nPortIndex != kOutputPortIndex) {
                return OMX_ErrorBadPortIndex;
            }
            mBitrate = pConfigParams->nEncodeBitrate;
            mIschangebitrate=1;
            return OMX_ErrorNone;
        }
        case OMX_IndexConfigEncSceneMode:
        {
            int *pEncSceneMode = (int *)params;
            mEncSceneMode = *pEncSceneMode;
            mSetEncMode = true;
            return OMX_ErrorNone;
        }
        default:
            return SprdSimpleOMXComponent::setConfig(index, params);
    }
}

OMX_ERRORTYPE SPRDAVCEncoder::getExtensionIndex(
    const char *name, OMX_INDEXTYPE *index) {
    if(strcmp(name, "OMX.google.android.index.storeMetaDataInBuffers") == 0) {
        *index = (OMX_INDEXTYPE) OMX_IndexParamStoreMetaDataBuffer;
        return OMX_ErrorNone;
    } else if (strcmp(name, "OMX.google.android.index.describeColorFormat") == 0) {
        *index = (OMX_INDEXTYPE) OMX_IndexParamDescribeColorFormat;
        return OMX_ErrorNone;
    } else if(strcmp(name, "OMX.google.android.index.prependSPSPPSToIDRFrames") == 0) {
        *index = (OMX_INDEXTYPE) OMX_IndexParamPrependSPSPPSToIDR;
        return OMX_ErrorNone;
    } else if (strcmp(name, "OMX.sprd.index.RecordMode") == 0) {
        *index = (OMX_INDEXTYPE) OMX_IndexConfigVideoRecordMode;
        return OMX_ErrorNone;
    } else if (strcmp(name, "OMX.sprd.index.EncSceneMode") == 0) {
        *index = (OMX_INDEXTYPE) OMX_IndexConfigEncSceneMode;
        return OMX_ErrorNone;
    }

    return SprdSimpleOMXComponent::getExtensionIndex(name, index);
}

// static
void SPRDAVCEncoder::FlushCacheWrapper(void* aUserData) {
    static_cast<SPRDAVCEncoder *>(aUserData)->flushCacheforBSBuf();

    return;
}

void SPRDAVCEncoder::flushCacheforBSBuf(){
    //mPmem_stream->flush_ion_buffer(
    //    (void*)mPbuf_stream_v, (void*)mPbuf_stream_p, mPbuf_stream_size);
    mPmem_stream->invalid_ion_buffer();

    if(mEncDbFrmMode){
        //mPmem_stream_pn->flush_ion_buffer(
        //    (void*)mPbuf_stream_v_pn, (void*)mPbuf_stream_p_pn, mPbuf_stream_size_pn);
        mPmem_stream_pn->invalid_ion_buffer();
    }
    return;
}

void SPRDAVCEncoder::onQueueFilled(OMX_U32 portIndex) {
    if (mSignalledError || mSawInputEOS) {
        return;
    }

    if (!mStarted) {
        if (OMX_ErrorNone != initEncoder()) {
            return;
        }
    }

    List<BufferInfo *> &inQueue = getPortQueue(0);
    List<BufferInfo *> &outQueue = getPortQueue(1);

    while (!mSawInputEOS && !inQueue.empty() && !outQueue.empty()) {
        BufferInfo *inInfo = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;
        BufferInfo *outInfo = *outQueue.begin();
        OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

        outHeader->nTimeStamp = 0;
        outHeader->nFlags = 0;
        outHeader->nOffset = 0;
        outHeader->nFilledLen = 0;
        outHeader->nOffset = 0;

        uint8_t *outPtr = (uint8_t *) outHeader->pBuffer;
        uint32_t dataLength = outHeader->nAllocLen;

        // Combine SPS and PPS and place them in the very first output buffer
        // SPS and PPS are separated by start code 0x00000001
        // Assume that we have exactly one SPS and exactly one PPS.
        if (!mSpsPpsHeaderReceived && mNumInputFrames <= 0) {
            MMEncOut sps_header, pps_header;
            int ret;

            memset(&sps_header, 0, sizeof(MMEncOut));
            memset(&pps_header, 0, sizeof(MMEncOut));

            ++mNumInputFrames;

            if((ret = (*mH264EncGenHeader)(mHandle, &sps_header, 1)) < 0){
                ALOGE("%s, line:%d, mH264EncGenHeader failed, ret: %d\n", __FUNCTION__, __LINE__, ret);
                return;
            }

            outHeader->nFilledLen = sps_header.strmSize;
            ALOGI("%s, %d, sps_header.strmSize: %d", __FUNCTION__, __LINE__, sps_header.strmSize);

            {
                uint8_t *p = (uint8_t *)(sps_header.pOutBuf);

                ALOGI("sps: %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x,%0x, %0x, %0x, %0x, %0x",
                      p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],p[10], p[11], p[12], p[13], p[14], p[15]);
            }

            if(mDumpStrmEnabled){
                if (mFile_bs != NULL) {
                    fwrite(sps_header.pOutBuf, 1, sps_header.strmSize, mFile_bs);
                }
            }

            memcpy(outPtr, sps_header.pOutBuf, sps_header.strmSize);
            outPtr+= sps_header.strmSize;
            ++mNumInputFrames;

            if((ret = (*mH264EncGenHeader)(mHandle, &pps_header, 0)) < 0){
                ALOGE("%s, line:%d, mH264EncGenHeader failed, ret: %d\n", __FUNCTION__, __LINE__, ret);
                return;
            }

            ALOGI("%s, %d, pps_header.strmSize: %d", __FUNCTION__, __LINE__, pps_header.strmSize);

            {
                uint8_t *p = (uint8_t *)(pps_header.pOutBuf);

                ALOGI("pps: %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x,",
                      p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            }

            if(mDumpStrmEnabled){
                if (mFile_bs != NULL) {
                    fwrite(pps_header.pOutBuf, 1, pps_header.strmSize, mFile_bs);
                }
            }

            outHeader->nFilledLen += pps_header.strmSize;
            memcpy(outPtr, pps_header.pOutBuf, pps_header.strmSize);

            mSpsPpsHeaderReceived = true;
            CHECK_EQ(0, mNumInputFrames);  // 1st video frame is 0
            outHeader->nFlags = OMX_BUFFERFLAG_CODECCONFIG;
            outQueue.erase(outQueue.begin());
            outInfo->mOwnedByUs = false;
            notifyFillBufferDone(outHeader);
            return;
        }

        ALOGV("%s, line:%d, inHeader->nFilledLen: %d, mStoreMetaData: %d, mVideoColorFormat: 0x%x",
              __FUNCTION__, __LINE__, inHeader->nFilledLen, mStoreMetaData, mVideoColorFormat);

        // Save the input buffer info so that it can be
        // passed to an output buffer
        InputBufferInfo info;
        info.mTimeUs = inHeader->nTimeStamp;
        info.mFlags = inHeader->nFlags;
        mInputBufferInfoVec.push(info);

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            mSawInputEOS = true;
        }

        if (inHeader->nFilledLen > 0) {
            const void *inData = inHeader->pBuffer + inHeader->nOffset;
            uint8_t *inputData = (uint8_t *) inData;
            CHECK(inputData != NULL);

            MMEncIn vid_in;
            MMEncOut vid_out;
            memset(&vid_in, 0, sizeof(MMEncIn));
            memset(&vid_out, 0, sizeof(MMEncOut));
            uint8_t* py = NULL;
            uint8_t* py_phy = NULL;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t x = 0;
            uint32_t y = 0;
            bool needUnmap = false;
            int bufFd = -1;
            unsigned long iova = 0;
            size_t iovaLen = 0;

            if (mVideoColorFormat == OMX_COLOR_FormatYUV420SemiPlanar) {
                vid_in.yuv_format = MMENC_YUV420SP_NV12;
            } else {
                vid_in.yuv_format = MMENC_YUV420SP_NV21;
            }

            if (mStoreMetaData) {
                unsigned int *mataData = (unsigned int *)inputData;
                unsigned int type = *mataData++;

                if (type == kMetadataBufferTypeCameraSource) {
                    py_phy = (uint8_t*)(*(unsigned long *)mataData);
                    mataData += sizeof(unsigned long)/sizeof(unsigned int);
                    py = (uint8_t*)(*(unsigned long *)mataData);
                    mataData += sizeof(unsigned long)/sizeof(unsigned int);
                    width = (uint32_t)(*((uint32_t *) mataData++));
                    height = (uint32_t)(*((uint32_t *) mataData++));
                    x = (uint32_t)(*((uint32_t *) mataData++));
                    y = (uint32_t)(*((uint32_t *) mataData++));
                    int fd = (int32)(*((int32*) mataData));
                    unsigned long py_addr=0;
                    size_t buf_size=0;
                    int ret = 0;
                    if (mIOMMUEnabled) {
                        ret = (*mH264EncGetIOVA)(mHandle, fd, &py_addr, &buf_size);
                    } else {
                        ret = MemIon::Get_phy_addr_from_ion(fd, &py_addr, &buf_size);
                    }
                    if(ret) {
                        ALOGE("Failed to Get_iova or Get_phy_addr_from_ion %d", ret);
                        return;
                    }
                    if (mIOMMUEnabled) {
                        needUnmap = true;
                        bufFd = fd;
                        iova = py_addr;
                        iovaLen = buf_size;
                    }
                    py_phy = (uint8_t*)py_addr;
                } else if (type == kMetadataBufferTypeGrallocSource) {
                    buffer_handle_t buf = *((buffer_handle_t *)(inputData + sizeof(void *)));

                    ALOGV("format:0x%x, usage:0x%x", ADP_FORMAT(buf), ADP_USAGE(buf));

                    if (mPbuf_yuv_v == NULL) {
                        size_t yuv_size;
                        if ((ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_RGBA_8888) && mSupportRGBEnc) {
                            yuv_size = ((mVideoWidth+15)&(~15)) * ((mVideoHeight+15)&(~15)) * 4;
                            vid_in.yuv_format = MMENC_RGBA32;
                        } else {
                            yuv_size = mVideoWidth * mVideoHeight * 3/2;
                        }
                        if (mIOMMUEnabled) {
                            mYUVInPmemHeap = new MemIon("/dev/ion", yuv_size, MemIon::NO_CACHING, ION_HEAP_ID_MASK_SYSTEM);
                        } else {
                            mYUVInPmemHeap = new MemIon("/dev/ion", yuv_size, MemIon::NO_CACHING, ION_HEAP_ID_MASK_MM);
                        }

                        int fd = mYUVInPmemHeap->getHeapID();
                        if (fd < 0) {
                            ALOGE("Failed to alloc yuv buffer");
                            return;
                        }

                        int ret;
                        unsigned long phy_addr;
                        size_t buffer_size;
                        if(mIOMMUEnabled) {
                            ret = (*mH264EncGetIOVA)(mHandle, fd, &phy_addr, &buffer_size);
                        } else {
                            ret = mYUVInPmemHeap->get_phy_addr_from_ion(&phy_addr, &buffer_size);
                        }
                        if(ret) {
                            ALOGE("Failed to get_iova or get_phy_addr_from_ion %d", ret);
                            return;
                        }

                        mPbuf_yuv_v =(uint8_t *) mYUVInPmemHeap->getBase();
                        mPbuf_yuv_p = phy_addr;
                        mPbuf_yuv_size = buffer_size;
                    }

                    py = mPbuf_yuv_v;
                    py_phy = (uint8_t*)mPbuf_yuv_p;

                    GraphicBufferMapper &mapper = GraphicBufferMapper::get();
                    Rect bounds(mVideoWidth, mVideoHeight);

                    void* vaddr = NULL;
                    struct android_ycbcr ycbcr;
                    ycbcr.chroma_step = 0;
                    if (ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_YCBCR_420_888){
                        if (mapper.lockYCbCr(buf, GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_NEVER, bounds, &ycbcr)) {
                            ALOGE("%s, line:%d, mapper.lockYCbCr failed", __FUNCTION__, __LINE__);
                            return;
                        }
                        vaddr = malloc(mVideoWidth * mVideoHeight * 3 / 2);
                        SprdSimpleOMXComponent::ConvertFlexYUVToPlanar((uint8_t*)vaddr, mVideoWidth, mVideoHeight, &ycbcr, mVideoWidth, mVideoHeight);
                    } else {
                        // sharkl3 screencapture
                        #if(defined PLATFORM_SHARKL3 ||defined PLATFORM_ROC1)
                        ALOGI("PLATFORM_SHARKL3 line:%d, mEncSceneMode:%d, ADP_FORMAT(buf):%d", __LINE__, mEncSceneMode, ADP_FORMAT(buf));
                        if (!(ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_RGBA_8888 && mEncSceneMode != 2)) {
                              if (mapper.lock(buf, GRALLOC_USAGE_SW_READ_OFTEN|GRALLOC_USAGE_SW_WRITE_NEVER, bounds, &vaddr)) {
                                  ALOGE("%s, line:%d, mapper.lock failed", __FUNCTION__, __LINE__);
                                  return;
                               }
                        }
                        #else
                        if (mapper.lock(buf, GRALLOC_USAGE_SW_READ_OFTEN|GRALLOC_USAGE_SW_WRITE_NEVER, bounds, &vaddr)) {
                            ALOGE("%s, line:%d, mapper.lock failed", __FUNCTION__, __LINE__);
                            return;
                        }
                        #endif
                    }

                    if (mVideoColorFormat == OMX_COLOR_FormatYUV420Planar) {
                        ConvertYUV420PlanarToYVU420SemiPlanar((uint8_t*)vaddr, py, mVideoWidth, mVideoHeight,
                                                              mVideoWidth, mVideoHeight);
                    } else if(mVideoColorFormat == OMX_COLOR_FormatAndroidOpaque) {
                        if (ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_YCbCr_420_SP || ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_YCBCR_420_888)
                            vid_in.yuv_format = MMENC_YUV420SP_NV12;
                        else if ((ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_RGBA_8888) && mSupportRGBEnc)
                            vid_in.yuv_format = MMENC_RGBA32;
                        else
                            vid_in.yuv_format = MMENC_YUV420SP_NV21;

                        if(ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_YCrCb_420_SP ||
                             ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_YCbCr_420_SP ||
                             ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
                            if (ADP_USAGE(buf) & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
                                unsigned long py_addr=0;
                                size_t buf_size=0;
                                int fd = ADP_BUFFD(buf);
                                int ret = 0;

                                if (mIOMMUEnabled) {
                                    ret = (*mH264EncGetIOVA)(mHandle, fd, &py_addr, &buf_size);
                                } else {
                                    ret = MemIon::Get_phy_addr_from_ion(fd, &py_addr, &buf_size);
                                }
                                if(ret) {
                                    ALOGE("Failed to Get_iova or Get_phy_addr_from_ion %d", ret);
                                    return;
                                }
                                if (mIOMMUEnabled) {
                                    needUnmap = true;
                                    bufFd = fd;
                                    iova = py_addr;
                                    iovaLen = buf_size;
                                }

                                py = (uint8_t*)vaddr;
                                py_phy = (uint8_t*)py_addr;

                                if(ADP_USAGE(buf) & GRALLOC_USAGE_CURSOR) {
                                    vid_in.fbc_mode = AFBC;
                                    ALOGV("%s,%d,buf size %d, fbc mode %d\n",__FUNCTION__,__LINE__,buf_size,vid_in.fbc_mode);
                                }
                            } else {
                                memcpy(py, vaddr, mVideoWidth * mVideoHeight * 3/2);
                            }
                        } else if (ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_YCbCr_420_P || ycbcr.chroma_step == 1) {
                            ConvertYUV420PlanarToYVU420SemiPlanar((uint8_t*)vaddr, py, mVideoWidth, mVideoHeight, mVideoWidth, mVideoHeight);
                            free (vaddr);
                        } else if (ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_YCBCR_420_888 && ycbcr.chroma_step == 2) {
                            memcpy(py, vaddr, mVideoWidth * mVideoHeight * 3/2);
                            free (vaddr);
                        } else if ((ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_RGBA_8888) && mSupportRGBEnc) {
//#if (defined PLATFORM_SHARKL5 || defined PLATFORM_ROC1)
#if 0
                            ALOGI("copy rgb data, HW encoder: %d", ADP_USAGE(buf) & GRALLOC_USAGE_HW_VIDEO_ENCODER);
                            memcpy(py, vaddr, mVideoWidth * mVideoHeight * 4);
#else
                            if (ADP_USAGE(buf) & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
                                unsigned long py_addr=0;
                                size_t buf_size=0;
                                int fd = ADP_BUFFD(buf);
                                int ret = 0;

                                if (mIOMMUEnabled) {
                                    ret = (*mH264EncGetIOVA)(mHandle, fd, &py_addr, &buf_size);
                                } else {
                                    ret = MemIon::Get_phy_addr_from_ion(fd, &py_addr, &buf_size);
                                }
                                if(ret) {
                                    ALOGE("Failed to Get_iova or Get_phy_addr_from_ion %d", ret);
                                    return;
                                }
                                if (mIOMMUEnabled) {
                                    needUnmap = true;
                                    bufFd = fd;
                                    iova = py_addr;
                                    iovaLen = buf_size;
                                }

                                py = (uint8_t*)vaddr;
                                py_phy = (uint8_t*)py_addr;
                            } else {
                                memcpy(py, vaddr, mVideoWidth * mVideoHeight * 4);
                            }
#endif
                        } else {
                            ConvertARGB888ToYVU420SemiPlanar((uint8_t*)vaddr, py, py+(mVideoWidth * mVideoHeight), mVideoWidth, mVideoHeight, mVideoWidth, mVideoHeight);
                        }
                    } else if(ADP_USAGE(buf) & GRALLOC_USAGE_HW_VIDEO_ENCODER ||
                                mVideoColorFormat == OMX_COLOR_FormatYUV420SemiPlanar) {
                        unsigned long py_addr=0;
                        size_t buf_size=0;
                        int fd = ADP_BUFFD(buf);
                        int ret = 0;
                        ALOGV("private_h->format:0x%x", ADP_FORMAT(buf));

                        if (mIOMMUEnabled) {
                            ret = (*mH264EncGetIOVA)(mHandle, fd, &py_addr, &buf_size);
                        } else {
                            ret = MemIon::Get_phy_addr_from_ion(fd,&py_addr,&buf_size);
                        }
                        if(ret) {
                            ALOGE("Failed to Get_iova or Get_phy_addr_from_ion %d", ret);
                            return;
                        }
                        if (mIOMMUEnabled) {
                            needUnmap = true;
                            bufFd = fd;
                            iova = py_addr;
                            iovaLen = buf_size;
                        }

                        py = (uint8_t*)vaddr;
                        py_phy = (uint8_t*)py_addr;
                        ALOGV("%s, mIOMMUEnabled = %d, fd = 0x%lx, py = 0x%lx, py_phy = 0x%lx",
                                __FUNCTION__, mIOMMUEnabled, fd, py, py_phy);
                    } else {
                        memcpy(py, vaddr, mVideoWidth * mVideoHeight * 3/2);
                    }

                    if (mUVExchange){
                        uint8* pu = py + mVideoWidth * mVideoHeight;
                        swap_uv_component(mVideoWidth, mVideoHeight, pu);
                        //MemIon::Flush_ion_buffer(bufFd, py, py_phy, iovaLen);
                        MemIon::Invalid_ion_buffer(bufFd);
                    }

                    #if (defined PLATFORM_SHARKL3 || defined PLATFORM_ROC1)
                        if (!(ADP_FORMAT(buf) == HAL_PIXEL_FORMAT_RGBA_8888 && mEncSceneMode != 2)) {
                            if (mapper.unlock(buf)) {
                                ALOGE("%s, line:%d, mapper.unlock failed", __FUNCTION__, __LINE__);
                                return;
                            }
                         }
                    #else
                        if (mapper.unlock(buf)) {
                           ALOGE("%s, line:%d, mapper.unlock failed", __FUNCTION__, __LINE__);
                           return;
                        }
                    #endif
                } else {
                    ALOGE("Error MetadataBufferType %d", type);
                    return;
                }
            } else {
                if (mPbuf_yuv_v == NULL) {
                    int32 yuv_size;
                    if (mVideoColorFormat == OMX_COLOR_FormatAndroidOpaque && mSupportRGBEnc)
                        yuv_size = ((mVideoWidth+15)&(~15)) * ((mVideoHeight+15)&(~15)) * 4;
                    else
                        yuv_size = mVideoWidth * mVideoHeight * 3/2;
                    if(mIOMMUEnabled) {
                        mYUVInPmemHeap = new MemIon("/dev/ion", yuv_size, MemIon::NO_CACHING, ION_HEAP_ID_MASK_SYSTEM);
                    } else {
                        mYUVInPmemHeap = new MemIon("/dev/ion", yuv_size, MemIon::NO_CACHING, ION_HEAP_ID_MASK_MM);
                    }

                    int fd = mYUVInPmemHeap->getHeapID();
                    if (fd < 0) {
                        ALOGE("Failed to alloc yuv buffer");
                        return;
                    }

                    int ret;
                    unsigned long phy_addr;
                    size_t buffer_size;
                    if(mIOMMUEnabled) {
                        ret = (*mH264EncGetIOVA)(mHandle, fd, &phy_addr, &buffer_size);
                    } else {
                        ret = mYUVInPmemHeap->get_phy_addr_from_ion(&phy_addr, &buffer_size);
                    }
                    if(ret) {
                        ALOGE("Failed to get_iova or get_phy_addr_from_ion %d", ret);
                        return;
                    }

                    mPbuf_yuv_v =(uint8_t *) mYUVInPmemHeap->getBase();
                    mPbuf_yuv_p = phy_addr;
                    mPbuf_yuv_size = buffer_size;
                }

                py = mPbuf_yuv_v;
                py_phy = (uint8_t*)mPbuf_yuv_p;

                if (mVideoColorFormat == OMX_COLOR_FormatYUV420Planar) {
                    ConvertYUV420PlanarToYVU420SemiPlanar(inputData, py, mVideoWidth, mVideoHeight,
                                                          mVideoWidth, mVideoHeight);
                } else if(mVideoColorFormat == OMX_COLOR_FormatAndroidOpaque) {
                    if(mSupportRGBEnc) {
                        memcpy(py, inputData, ((mVideoWidth+15)&(~15)) * ((mVideoHeight+15)&(~15)) * 4);
                        vid_in.yuv_format = MMENC_RGBA32;
                    } else {
                        ConvertARGB888ToYVU420SemiPlanar(inputData, py, py+(mVideoWidth * mVideoHeight), mVideoWidth, mVideoHeight, mVideoWidth, mVideoHeight);
                    }
                } else {
                    if (mVideoWidth != mFrameWidth && mVideoHeight == mFrameHeight &&
                           *(inputData+mFrameWidth*mFrameHeight*3/2) == 0) {
                        uint8_t* align_data = NULL;
                        uint8_t* org_data = NULL;
                        align_data  = py;
                        org_data = inputData;
                        for (int i = 0; i < mVideoHeight; i++) {
                            org_data = inputData + mFrameWidth*i;
                            align_data = py + mVideoWidth*i;
                            memcpy(align_data,org_data,mFrameWidth);
                            for (int m = 0; m<mVideoWidth-mFrameWidth; m++) {
                                *(align_data+mFrameWidth+m) = *(align_data+mFrameWidth-1);
                            }
                        }
                        for (int j = 0; j < mVideoHeight/2; j++) {
                            org_data = inputData + mFrameWidth * mVideoHeight + mFrameWidth*j;
                            align_data = py + mVideoWidth * mVideoHeight + mVideoWidth*j;
                            memcpy(align_data,org_data,mFrameWidth);
                            for (int m = 0; m < mVideoWidth-mFrameWidth; m++) {
                                *(align_data+mFrameWidth+m) = *(align_data+mFrameWidth-1);
                            }
                        }
                    } else {
                        memcpy(py, inputData, mVideoWidth * mVideoHeight * 3/2);
                    }
                }
            }

#ifdef CONFIG_SPRD_RECORD_EIS
            if (mEISMode) {
                char *parameter = (char *)py+mVideoWidth * mVideoHeight * 3/2;
                double *warp = (double *)parameter;
                if (*warp++ == 16171225) {
                    for (int i=0;i<3;i++) {
                        for (int j=0;j<3;j++) {
                            vid_in.matrx[i][j]=*warp++;
                        }
                    }
                } else {
                    vid_in.matrx[0][0] = vid_in.matrx[1][1] = vid_in.matrx[2][2] = 1.0;
                    vid_in.matrx[0][1] = vid_in.matrx[0][2] = vid_in.matrx[1][0] = 0.0;
                    vid_in.matrx[1][2] = vid_in.matrx[2][0] = vid_in.matrx[2][1] = 0.0;
                }
                ALOGV("EISLib2record:%f %f %f %f %f %f %f %f %f",
                    vid_in.matrx[0][0], vid_in.matrx[0][1], vid_in.matrx[0][2],
                    vid_in.matrx[1][0], vid_in.matrx[1][1], vid_in.matrx[1][2],
                    vid_in.matrx[2][0], vid_in.matrx[2][1], vid_in.matrx[2][2]);
            }
#endif
            // vid_in.time_stamp is not use for now.
            vid_in.time_stamp = (inHeader->nTimeStamp + 500) / 1000;  // in ms;
            vid_in.channel_quality = 1;
            if (mIschangebitrate) {
                vid_in.bitrate=mBitrate;
                vid_in.ischangebitrate = true;
                mIschangebitrate =false;
            } else {
                vid_in.ischangebitrate = false;
            }
            vid_in.needIVOP = false;    // default P frame
            if (mKeyFrameRequested || (mNumInputFrames == 0)) {
                vid_in.needIVOP = true;    // I frame
                ALOGI("Request an IDR frame");
            }

            vid_in.p_src_y = py;
            vid_in.p_src_v = 0;
            vid_in.p_src_y_phy = py_phy;
            vid_in.p_src_v_phy = 0;

            if(width != 0 && height != 0) {
                vid_in.p_src_u = py + width*height;
                vid_in.p_src_u_phy = py_phy + width*height;
            } else {
                vid_in.p_src_u = py + mVideoWidth * mVideoHeight;
                vid_in.p_src_u_phy = py_phy + mVideoWidth * mVideoHeight;
            }

            vid_in.org_img_width = (int32_t)width;
            vid_in.org_img_height = (int32_t)height;
            vid_in.crop_x = (int32_t)x;
            vid_in.crop_y = (int32_t)y;

            if(mDumpYUVEnabled){
                if (mFile_yuv != NULL) {
                    fwrite(py, 1, mVideoWidth * mVideoHeight * 3/2, mFile_yuv);
               }
            }
            //mPmem_stream->flush_ion_buffer((void*)mPbuf_stream_v, (void*)mPbuf_stream_p, mPbuf_stream_size);
            mPmem_stream->invalid_ion_buffer();
            if(mEncDbFrmMode){
                //mPmem_stream_pn->flush_ion_buffer((void*)mPbuf_stream_v_pn, (void*)mPbuf_stream_p_pn, mPbuf_stream_size_pn);
                mPmem_stream_pn->invalid_ion_buffer();
            }

            int64_t start_encode = systemTime();
            int ret = (*mH264EncStrmEncode)(mHandle, &vid_in, &vid_out);
            int64_t end_encode = systemTime();
            ALOGI("H264EncStrmEncode[%lld] %dms, in {%p-%p, %dx%d, %d}, out {%p-%d, %d}, wh{%d, %d}, xy{%d, %d}",
                  mNumInputFrames, (unsigned int)((end_encode-start_encode) / 1000000L), py, py_phy,
                  mVideoWidth, mVideoHeight, vid_in.yuv_format, vid_out.pOutBuf, vid_out.strmSize, vid_out.vopType, width, height, x, y);

            if (needUnmap) {
                ALOGV("Free_iova, fd: %d, iova: 0x%lx, size: %zd", bufFd, iova, iovaLen);
                (*mH264EncFreeIOVA)(mHandle, iova, iovaLen);
            }

            if ((vid_out.strmSize < 0) || (ret != MMENC_OK)) {
                ALOGE("Failed to encode frame %lld, ret=%d", mNumInputFrames, ret);
            } else {
                ALOGV("%s, %d, out_stream_ptr: %p", __FUNCTION__, __LINE__, outPtr);

                {
                    uint8_t *p = (uint8_t *)(vid_out.pOutBuf);
                    if (vid_out.strmSize > 7) {
                        ALOGV("frame: %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x,",
                              p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

                        p[0] = p[1] = p[2] = 0x0;
                        p[3] = 0x1;
                    }
                }
            }

            if(mDumpStrmEnabled){
                if (mFile_bs != NULL) {
                    fwrite(vid_out.pOutBuf, 1, vid_out.strmSize, mFile_bs);
                }
            }

            if(vid_out.strmSize > 0) {
                dataLength = vid_out.strmSize;
                memcpy(outPtr, vid_out.pOutBuf, dataLength);

                if (vid_out.vopType == 0) { //I VOP
                    mKeyFrameRequested = false;
                    outHeader->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;
                }
                ++mNumInputFrames;
            } else {
                dataLength = 0;
            }
        } else {
            dataLength = 0;
        }

        if ((inHeader->nFlags & OMX_BUFFERFLAG_EOS) && (inHeader->nFilledLen == 0)) {
            // We also tag this output buffer with EOS if it corresponds
            // to the final input buffer.
            ALOGI("saw EOS");
            outHeader->nFlags = OMX_BUFFERFLAG_EOS;
        }

        inQueue.erase(inQueue.begin());
        inInfo->mOwnedByUs = false;
        notifyEmptyBufferDone(inHeader);

        if (!((mVideoWidth<= mCapability.max_width&& mVideoHeight<= mCapability.max_height)
                || (mVideoWidth <= mCapability.max_height && mVideoHeight <= mCapability.max_width))) {
            ALOGE("[%d,%d] is out of range [%d, %d], failed to support this format.",
                  mVideoWidth, mVideoHeight, mCapability.max_width, mCapability.max_height);
            notify(OMX_EventError, OMX_ErrorBadParameter, 0, NULL);
            mSignalledError = true;
            return;
        }

        CHECK(!mInputBufferInfoVec.empty());
        InputBufferInfo *inputBufInfo = mInputBufferInfoVec.begin();
        if (dataLength > 0 || (inHeader->nFlags & OMX_BUFFERFLAG_EOS)) {
            outQueue.erase(outQueue.begin());
            outHeader->nTimeStamp = inputBufInfo->mTimeUs;
            outHeader->nFlags |= (inputBufInfo->mFlags | OMX_BUFFERFLAG_ENDOFFRAME);
            outHeader->nFilledLen = dataLength;
            outInfo->mOwnedByUs = false;
            notifyFillBufferDone(outHeader);
        }
        mInputBufferInfoVec.erase(mInputBufferInfoVec.begin());
    }
}

bool SPRDAVCEncoder::openEncoder(const char* libName) {
    if(mLibHandle) {
        dlclose(mLibHandle);
    }

    ALOGI("openEncoder, lib: %s", libName);

    mLibHandle = dlopen(libName, RTLD_NOW);
    if(mLibHandle == NULL) {
        ALOGE("openEncoder, can't open lib: %s",libName);
        return false;
    }

    mH264EncPreInit = (FT_H264EncPreInit)dlsym(mLibHandle, "H264EncPreInit");
    if(mH264EncPreInit == NULL) {
        ALOGE("Can't find mH264EncPreInit in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncInit = (FT_H264EncInit)dlsym(mLibHandle, "H264EncInit");
    if(mH264EncInit == NULL) {
        ALOGE("Can't find H264EncInit in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncSetConf = (FT_H264EncSetConf)dlsym(mLibHandle, "H264EncSetConf");
    if(mH264EncSetConf == NULL) {
        ALOGE("Can't find H264EncSetConf in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncGetConf = (FT_H264EncGetConf)dlsym(mLibHandle, "H264EncGetConf");
    if(mH264EncGetConf == NULL) {
        ALOGE("Can't find H264EncGetConf in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncStrmEncode = (FT_H264EncStrmEncode)dlsym(mLibHandle, "H264EncStrmEncode");
    if(mH264EncStrmEncode == NULL) {
        ALOGE("Can't find H264EncStrmEncode in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncGenHeader = (FT_H264EncGenHeader)dlsym(mLibHandle, "H264EncGenHeader");
    if(mH264EncGenHeader == NULL) {
        ALOGE("Can't find H264EncGenHeader in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncRelease = (FT_H264EncRelease)dlsym(mLibHandle, "H264EncRelease");
    if(mH264EncRelease == NULL) {
        ALOGE("Can't find H264EncRelease in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncGetCodecCapability = (FT_H264EncGetCodecCapability)dlsym(mLibHandle, "H264EncGetCodecCapability");
    if(mH264EncGetCodecCapability == NULL) {
        ALOGE("Can't find H264EncGetCodecCapability in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncGetIOVA = (FT_H264Enc_get_iova)dlsym(mLibHandle, "H264Enc_get_iova");
    if(mH264EncGetIOVA == NULL) {
        ALOGE("Can't find H264Enc_get_iova in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncFreeIOVA = (FT_H264Enc_free_iova)dlsym(mLibHandle, "H264Enc_free_iova");
    if(mH264EncFreeIOVA == NULL) {
        ALOGE("Can't find H264Enc_free_iova in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264EncGetIOMMUStatus = (FT_H264Enc_get_IOMMU_status)dlsym(mLibHandle, "H264Enc_get_IOMMU_status");
    if(mH264EncGetIOMMUStatus == NULL) {
        ALOGE("Can't find H264Enc_get_IOMMU_status in %s",libName);
        dlclose(mLibHandle);
        mLibHandle = NULL;
        return false;
    }

    mH264Enc_NeedAlign = (FT_H264Enc_Need_Align)dlsym(mLibHandle, "H264Enc_NeedAlign");
    if(mH264Enc_NeedAlign == NULL) {
        ALOGE("Can't find H264Enc_NeedAlign in %s",libName);
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
    return new android::SPRDAVCEncoder(name, callbacks, appData, component);
}
