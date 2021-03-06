/******************************************************************************
 ** File Name:    vp9_dec_api.h *
 ** Author: *
 ** DATE:         3/15/2007                                                   *
 ** Copyright:    2007 Spreadtrum, Incorporated. All Rights Reserved.         *
 ** Description:  define data structures for Video Codec                      *
 *****************************************************************************/
/******************************************************************************
 **                   Edit    History                                         *
 **---------------------------------------------------------------------------*
 ** DATE          NAME            DESCRIPTION                                 *
 ** 3/15/2007        Create. *
 *****************************************************************************/
#ifndef _VP9_DEC_API_H_
#define _VP9_DEC_API_H_

/*----------------------------------------------------------------------------*
**                        Dependencies                                        *
**---------------------------------------------------------------------------*/
//#include "mmcodec.h"
/**---------------------------------------------------------------------------*
 **                             Compiler Flag                                 *
 **---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char BOOLEAN;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef signed long long int64;

typedef signed char int8;
typedef signed short int16;
typedef signed int int32;

/*standard*/
typedef enum {
  ITU_H263 = 0,
  MPEG4,
  JPEG,
  FLV_V1,
  H264,
  RV8,
  RV9
} VIDEO_STANDARD_E;

typedef enum {
  MMDEC_OK = 0,
  MMDEC_ERROR = -1,
  MMDEC_PARAM_ERROR = -2,
  MMDEC_MEMORY_ERROR = -3,
  MMDEC_INVALID_STATUS = -4,
  MMDEC_STREAM_ERROR = -5,
  MMDEC_OUTPUT_BUFFER_OVERFLOW = -6,
  MMDEC_HW_ERROR = -7,
  MMDEC_NOT_SUPPORTED = -8,
  MMDEC_FRAME_SEEK_IVOP = -9,
  MMDEC_MEMORY_ALLOCED = -10
} MMDecRet;

typedef enum {
  YUV420P_YU12 = 0,
  YUV420P_YV12 = 1,
  YUV420SP_NV12 = 2, /*u/v interleaved*/
  YUV420SP_NV21 = 3, /*v/u interleaved*/
} MM_YUV_FORMAT_E;

typedef enum {
    INTER_MEM = 0,  /*internal memory, only for software writing and reading and initialized when initialize decoder*/
    HW_NO_CACHABLE, /*physical continuous and no-cachable, only for VSP writing and reading */
    HW_CACHABLE,    /*physical continuous and cachable, for software writing and VSP reading */
    SW_CACHABLE,    /*only for software writing and reading*/
    MAX_MEM_TYPE
} CODEC_BUF_TYPE;

// decoder video format structure
typedef struct {
  int32 video_std;  // video standard, 0: VSP_ITU_H263, 1: VSP_MPEG4, 2:
                    // VSP_JPEG, 3: VSP_FLV_V1
  int32 frame_width;
  int32 frame_height;
  int32 i_extra;
  void *p_extra;
  unsigned long p_extra_phy;
  int32 yuv_format;
} MMDecVideoFormat;

// Decoder buffer for decoding structure
typedef struct {
  uint8 *common_buffer_ptr;  // Pointer to buffer used when decoding
  unsigned long common_buffer_ptr_phy;
  uint32 size;  // Number of bytes decoding buffer

  int32 frameBfr_num;  // YUV frame buffer number

  uint8 *int_buffer_ptr;  // internal memory address
  int32 int_size;  // internal memory size
} MMCodecBuffer;

typedef MMCodecBuffer MMDecBuffer;

typedef struct {
  uint16 start_pos;
  uint16 end_pos;
} ERR_POS_T;

#define MAX_ERR_PKT_NUM 30

// Decoder input structure
typedef struct {
  uint8 *pStream;  // Pointer to stream to be decoded. Virtual address.
  unsigned long pStream_phy;  // Pointer to stream to be decoded. Physical address.
  uint32 dataLen;   // Number of bytes to be decoded
  int32 beLastFrm;  // whether the frame is the last frame.  1: yes,   0: no

  int32 expected_IVOP;  // control flag, seek for IVOP,
  uint64 pts;            // presentation time stamp

  int32 beDisplayed;  // whether the frame to be displayed    1: display   0:
                      // not //display

  int32 err_pkt_num;                       // error packet number
  ERR_POS_T err_pkt_pos[MAX_ERR_PKT_NUM];  // position of each error packet in
  uint8 fbc_mode;                                         // bitstream
} MMDecInput;

// Decoder output structure
typedef struct {
  uint8 *pOutFrameY;  // Pointer to the recent decoded picture
  uint8 *pOutFrameU;
  uint8 *pOutFrameV;

  uint32 frame_width;
  uint32 frame_height;

  int32 is_transposed;  // the picture is transposed or not, in 8800H5, it
                        // should always 0.

  uint64 pts;  // presentation time stamp
  int32 frameEffective;

  int32 err_MB_num;  // error MB number
  void *pBufferHeader;
  int VopPredType;
} MMDecOutput;

enum FBC_MODE{
    FBC_NONE,
    IFBC = 1,
    AFBC = 2
};
typedef int (*FunctionType_BufCB)(void *userdata, void *pHeader, int flag);

typedef int (*FunctionType_MallocCB)(void* aUserData, unsigned int size_extra);

/* Application controls, this structed shall be allocated */
/*    and initialized in the application.                 */
typedef struct tagVP9Handle {
  /* The following fucntion pointer is copied to BitstreamDecVideo structure  */
  /*    upon initialization and never used again. */
  //    int (*readBitstreamData)(uint8 *buf, int nbytes_required, void
  //    *appData);
  //    applicationData appData;

  //    uint8 *outputFrame;
  void *videoDecoderData; /* this is an internal pointer that is only used */
                          /* in the decoder library.   */
#ifdef PV_MEMORY_POOL
  int32 size;
#endif

  void *userdata;

  FunctionType_BufCB VSP_bindCb;
  FunctionType_BufCB VSP_unbindCb;
  FunctionType_MallocCB VSP_extMemCb;
} VP9Handle;

/**----------------------------------------------------------------------------*
**                           Function Prototype                               **
**----------------------------------------------------------------------------*/
void VP9GetVideoDimensions(VP9Handle *vp9Handle, int32 *display_width,
                           int32 *display_height);
void VP9GetBufferDimensions(VP9Handle *vp9Handle, int32 *width, int32 *height);
MMDecRet VP8GetCodecCapability(VP9Handle *vp9Handle, int32 *max_width,
                               int32 *max_height);
void VP9DecSetCurRecPic(VP9Handle *vp9Handle, uint8 *pFrameY,
                        uint8 *pFrameY_phy, void *pBufferHeader);
int VP9DecGetLastDspFrm(VP9Handle *vp9Handle, void **pOutput);

/*****************************************************************************/
//Description: Init vp9 decoder
//Global resource dependence:
//Author:
//Note:
/*****************************************************************************/
MMDecRet VP9DecInit(VP9Handle *vp9Handle, MMCodecBuffer *pInterMemBfr, MMDecVideoFormat *pVideoFormat);

/*****************************************************************************/
//Description: Decode one vop
//Global resource dependence:
//Author:
//Note:
/*****************************************************************************/
MMDecRet VP9DecDecode(VP9Handle *vp9Handle, MMDecInput *pInput,
                      MMDecOutput *pOutput);

/*****************************************************************************/
//Description: Close vp9 decoder
//Global resource dependence:
//Author:
//Note:
/*****************************************************************************/
MMDecRet VP8DecRelease(VP9Handle *vp9Handle);

int VP9Dec_get_iova(VP9Handle *vp9Handle, int fd, unsigned long *iova, size_t *size);

int VP9Dec_free_iova(VP9Handle *vp9Handle, unsigned long iova, size_t size);

int VP9Dec_get_IOMMU_status(VP9Handle *vp9Handle);

MMDecRet VP9DecMemInit(VP9Handle *vp9Handle, MMCodecBuffer *pBuffer);

typedef void (*FT_VP9GetVideoDimensions)(VP9Handle *vp9Handle, int32 *width,
                                         int32 *height);
typedef void (*FT_VP9GetBufferDimensions)(VP9Handle *vp9Handle, int32 *width,
                                          int32 *height);
typedef MMDecRet (*FT_VP9GetCodecCapability)(VP9Handle *vp9Handle,
                                             int32 *max_width,
                                             int32 *max_height);
typedef void (*FT_VP9DecSetCurRecPic)(VP9Handle *vp9Handle, uint8 *pFrameY,
                                      uint8 *pFrameY_phy, void *pBufferHeader);
typedef MMDecRet (*FT_VP9DecInit)(VP9Handle *vp9Handle,
                                  MMCodecBuffer *pInterMemBfr,
                                  MMDecVideoFormat *pVideoFormat);
typedef MMDecRet (*FT_VP9DecDecode)(VP9Handle *vp9Handle, MMDecInput *pInput,
                                    MMDecOutput *pOutput);
typedef MMDecRet (*FT_VP9DecRelease)(VP9Handle *vp9Handle);
typedef void (*FT_VP9DecReleaseRefBuffers)(VP9Handle *vp9Handle);
typedef int (*FT_VP9DecGetLastDspFrm)(VP9Handle *vp9Handle, void **pOutput);

typedef int (*FT_VP9Dec_get_iova)(VP9Handle *vp9Handle, int fd, unsigned long *iova, size_t *size);

typedef int (*FT_VP9Dec_free_iova)(VP9Handle *vp9Handle, unsigned long iova, size_t size);

typedef int (*FT_VP9Dec_get_IOMMU_status)(VP9Handle *vp9Handle);

typedef MMDecRet (*FT_VP9DecMemInit)(VP9Handle *vp9Handle, MMCodecBuffer *pBuffer);

/**----------------------------------------------------------------------------*
**                         Compiler Flag                                      **
**----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/**---------------------------------------------------------------------------*/
#endif  //_VP8_DEC_H_
// End
