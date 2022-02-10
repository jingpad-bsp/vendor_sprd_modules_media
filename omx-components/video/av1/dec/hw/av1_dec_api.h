/******************************************************************************
 ** File Name:    h264dec.h                                                   *
 ** Author: *
 ** DATE:         8/9/2019                                                   *
 ** Copyright:    2019 Unisoc, Incorporated. All Rights Reserved.         *
 ** Description:  define data structures for Video Codec                      *
 *****************************************************************************/
/******************************************************************************
 **                   Edit    History                                         *
 **---------------------------------------------------------------------------*
 ** DATE          NAME            DESCRIPTION                                 *
 ** 8/9/2019     			      Create. *
 *****************************************************************************/
#ifndef _AV1_DEC_H_
#define _AV1_DEC_H_

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
// typedef unsigned char		Bool;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef signed long long int64;

typedef signed char int8;
typedef signed short int16;
typedef signed int int32;

/**
This enumeration is for profiles. The value follows the profile_idc  in sequence
parameter set rbsp. See Annex A.
@publishedAll
*/

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

typedef struct
{
    uint16 start_pos;
    uint16 end_pos;
} ERR_POS_T;


#define MAX_ERR_PKT_NUM 30

// Decoder input structure
typedef struct {
    uint8 *pStream;             // Pointer to stream to be decoded
    unsigned long pStream_phy;  // Pointer to stream to be decoded, phy
    uint32 dataLen;             // Number of bytes to be decoded
    int32 beLastFrm;  // whether the frame is the last frame.  1: yes,   0: no

    int32 expected_IVOP;  // control flag, seek for IVOP,
    uint64 nTimeStamp;    // time stamp, it is PTS or DTS

    int32 beDisplayed;  // whether the frame to be displayed    1: display   0:
    // not //display

    int32 err_pkt_num;                       // error packet number
    ERR_POS_T err_pkt_pos[MAX_ERR_PKT_NUM];  // position of each error packet in
    uint8 fbc_mode;
    // bitstream
} MMDecInput;

// Decoder output structure
typedef struct {
    uint8 *pOutFrameY;  // Pointer to the recent decoded picture
    uint8 *pOutFrameU;
    uint8 *pOutFrameV;

    uint32 frame_width;
    uint32 frame_height;

    int32 is_transposed;  // the picture is transposed or not, in 8800S4, it
    // should always 0.

    uint64 pts;  // presentation time stamp
    int32 frameEffective;

    int32 err_MB_num;  // error MB number
    void *pBufferHeader;
    int reqNewBuf;
    int32 mPicId;

    BOOLEAN sawSPS;
    BOOLEAN sawPPS;
} MMDecOutput;
struct bufferHeader {
    int iRefCount;
    uint8 *pyuv;
    unsigned long pyuv_phy;
};

// Decoder video capability structure
typedef struct {
    uint32 max_width;
    uint32 max_height;

} MMDecCapability;

typedef struct {
    uint32 cropLeftOffset;
    uint32 cropOutWidth;
    uint32 cropTopOffset;
    uint32 cropOutHeight;
} CropParams;

typedef struct {
    uint32 profile;
    uint32 picWidth;
    uint32 picHeight;
    uint32 videoRange;

} AV1SwDecInfo;

typedef int (*FunctionType_BufCB)(void *userdata, void *pHeader);

/* Application controls, this structed shall be allocated */
/*    and initialized in the application.                 */
typedef struct tagAV1Handle {

    void *videoDecoderData; /* this is an internal pointer that is only used */


    void *userdata;

    FunctionType_BufCB VSP_bindCb;
    FunctionType_BufCB VSP_unbindCb;
    uint8 framethreads;
    uint8 tilethreads;
} AV1Handle;

/**----------------------------------------------------------------------------*
**                           Function Prototype                               **
**----------------------------------------------------------------------------*/

void AV1Dec_ReleaseRefBuffers(AV1Handle *av1Handle);
MMDecRet AV1Dec_GetLastDspFrm(AV1Handle *av1Handle,void **pOutFrameY, void **pBufferHeader, uint64 *pts);
void AV1Dec_SetCurRecPic(AV1Handle *av1Handle, uint8	*pFrameY, uint8 *pFrameY_phy,void *pBufferHeader);
MMDecRet AV1DecGetInfo(AV1Handle *av1Handle, AV1SwDecInfo *pDecInfo);
MMDecRet AV1GetCodecCapability(AV1Handle *av1Handle, MMDecCapability *Capability);

/*****************************************************************************/
//  Description: Init h264 decoder
//	Global resource dependence:
//  Author:
//	Note:
/*****************************************************************************/
MMDecRet AV1DecInit(AV1Handle *av1Handle);

/*****************************************************************************/
//  Description: Decode one vop
//	Global resource dependence:
//  Author:
//	Note:
/*****************************************************************************/
MMDecRet AV1DecDecode(AV1Handle *av1Handle, MMDecInput *pInput,MMDecOutput *pOutput);

/*****************************************************************************/
//  Description: Close mpeg4 decoder
//	Global resource dependence:
//  Author:
//	Note:
/*****************************************************************************/
MMDecRet AV1DecRelease(AV1Handle *av1Handle);

typedef MMDecRet (*FT_AV1DecInit)(AV1Handle *av1Handle);
typedef MMDecRet (*FT_AV1DecGetInfo)(AV1Handle *av1Handle,
                                     AV1SwDecInfo *pDecInfo);
typedef MMDecRet (*FT_AV1GetCodecCapability)(AV1Handle *av1Handle,
        MMDecCapability *Capability);

typedef MMDecRet (*FT_AV1DecDecode)(AV1Handle *av1Handle, MMDecInput *pInput,
                                    MMDecOutput *pOutput);
typedef MMDecRet (*FT_AV1DecRelease)(AV1Handle *av1Handle);
typedef void (*FT_AV1Dec_SetCurRecPic)(AV1Handle *av1Handle, uint8 *pFrameY,
                                       uint8 *pFrameY_phy, void *pBufferHeader);
typedef MMDecRet (*FT_AV1Dec_GetLastDspFrm)(AV1Handle *av1Handle, void **pOutFrameY, void **pBufferHeader, uint64 *pts);
typedef void (*FT_AV1Dec_ReleaseRefBuffers)(AV1Handle *av1Handle);


/**----------------------------------------------------------------------------*
**                         Compiler Flag                                      **
**----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/**---------------------------------------------------------------------------*/
#endif  //_AV1_DEC_H_
// End

