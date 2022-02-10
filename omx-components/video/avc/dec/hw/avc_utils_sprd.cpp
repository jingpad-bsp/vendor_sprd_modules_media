#include "avc_utils_sprd.h"

const unsigned int g_msk[33] =
{
    0x00000000, 0x00000001, 0x00000003, 0x00000007,
    0x0000000f, 0x0000001f, 0x0000003f, 0x0000007f,
    0x000000ff, 0x000001ff, 0x000003ff, 0x000007ff,
    0x00000fff, 0x00001fff, 0x00003fff, 0x00007fff,
    0x0000ffff, 0x0001ffff, 0x0003ffff, 0x0007ffff,
    0x000fffff, 0x001fffff, 0x003fffff, 0x007fffff,
    0x00ffffff, 0x01ffffff, 0x03ffffff, 0x07ffffff,
    0x0fffffff, 0x1fffffff, 0x3fffffff, 0x7fffffff,
    0xffffffff
};

#define READ_FLC(stream, nbits)	READ_BITS(stream, nbits)

//big endian
#define BITSTREAMSHOWBITS(bitstream, nBits) \
((nBits) <= bitstream->bitsLeft) ? (((bitstream->rdptr[0]) >> (bitstream->bitsLeft - (nBits))) & g_msk [(nBits)]) : \
        ((((bitstream->rdptr[0])  << ((nBits) - bitstream->bitsLeft)) | ((bitstream->rdptr[1]) >> (32 - (nBits) + bitstream->bitsLeft))) & g_msk[(nBits)])

#define BITSTREAMFLUSHBITS(stream, nbits) \
{ \
          stream->bitcnt += (nbits); \
          if (nbits < stream->bitsLeft) \
          {              \
              stream->bitsLeft -= (nbits);	\
          } \
          else \
          {\
              stream->bitsLeft += 32 - (nbits);\
              stream->rdptr++;\
          }    \
}



static unsigned int READ_BITS1(DEC_BS_T *stream)
{
    unsigned int val;

    val = ((*stream->rdptr >> (stream->bitsLeft - 1)) & 0x1);
    stream->bitcnt++;
    stream->bitsLeft--;

    if (!stream->bitsLeft)
    {
        stream->bitsLeft = 32;
        stream->rdptr++;
    }

    return val;
}

static unsigned int READ_BITS(DEC_BS_T *stream, unsigned int nbits)
{
    unsigned int temp;

    temp = BITSTREAMSHOWBITS(stream, nbits);
    BITSTREAMFLUSHBITS(stream, nbits);

    return temp;
}

static unsigned int READ_UE_V (DEC_BS_T * stream)
{
    int info;
    unsigned int ret;
    unsigned int tmp;
    unsigned int leading_zero = 0;

    //tmp = BitstreamShowBits (stream, 32);
    tmp = (unsigned int)(BITSTREAMSHOWBITS (stream, 32));

    /*find the leading zero number*/
#ifndef _ARM_CLZ_OPT_
    if (!tmp)
    {
        stream->error_flag |= 1;
        return 0;
    }
    while ( (tmp & (1 << (31 - leading_zero))) == 0 )
        leading_zero++;
#else
#if defined(__GNUC__)
    __asm__("clz %0, %1":"=&r"(leading_zero):"r"(tmp):"cc");
#else
    __asm {
        clz leading_zero, tmp
    }
#endif
#endif

    //must! because BITSTRM may be error and can't find the leading zero, xw@20100527
    if (leading_zero > 16)
    {
        stream->error_flag |= 1;
        return 0;
    }

    BITSTREAMFLUSHBITS(stream, leading_zero * 2 + 1);

    info = (tmp >> (32 - 2 * leading_zero -1)) & g_msk [leading_zero];
    ret = (1 << leading_zero) + info - 1;

    return ret;
}

static int READ_SE_V (DEC_BS_T * stream)
{
    int ret;
    int info;
    int tmp;
    unsigned int leading_zero = 0;

    //tmp = BitstreamShowBits (stream, 32);
    tmp = BITSTREAMSHOWBITS(stream, 32);

    /*find the leading zero number*/
#ifndef _ARM_CLZ_OPT_
    if (!tmp)
    {
        stream->error_flag |= 1;
        return 0;
    }
    while ( (tmp & (1 << (31 - leading_zero))) == 0 )
        leading_zero++;
#else
#if defined(__GNUC__)
    __asm__("clz %0, %1":"=&r"(leading_zero):"r"(tmp):"cc");
#else
    __asm {
        clz leading_zero, tmp
    }
#endif
#endif

    //must! because BITSTRM may be error and can't find the leading zero, xw@20100527
    if (leading_zero > 16)
    {
        stream->error_flag |= 1;
        return 0;
    }

    BITSTREAMFLUSHBITS (stream, leading_zero * 2 + 1);

    info = (tmp >> (32 - 2 * leading_zero -1)) & g_msk [leading_zero];

    tmp = (1 << leading_zero) + info - 1;
    ret = (tmp + 1) / 2;

    if ( (tmp & 1) == 0 )
        ret = -ret;

    return ret;
}

/*
static unsigned int H264Dec_Long_UEV (DEC_BS_T * stream)
{
    unsigned int tmp;
    int leading_zero = 0;

    tmp = BITSTREAMSHOWBITS (stream, 16);

    if (tmp == 0)
    {
        READ_FLC (stream, 16);
        leading_zero = 16;

        do {
            tmp = READ_BITS1 (stream);
            leading_zero++;
        } while(!tmp);

        leading_zero--;
        tmp = READ_FLC (stream, leading_zero);

        return tmp;
    } else
    {
        return READ_UE_V (stream);
    }
}
*/

static int H264Dec_Long_SEV (DEC_BS_T * stream)
{
    unsigned int tmp;
    int leading_zero = 0;

    tmp = BITSTREAMSHOWBITS (stream, 16);

    if (tmp == 0)
    {
        READ_FLC (stream, 16);
        leading_zero = 16;

        do {
            tmp = READ_BITS1 (stream);
            leading_zero++;
        } while(!tmp);

        leading_zero--;
        tmp = READ_FLC (stream, leading_zero);

        return tmp;
    } else
    {
        return READ_SE_V (stream);
    }
}

void decode_scaling_list(DEC_BS_T *stream, int size)
{
    int i, last = 8, next = 8;

    /* matrix not written, we use the predicted one */
    if(!READ_FLC(stream, 1)) ;
    else
    {
        for(i=0; i<size; i++) {
            if(next)
                next = (last +  READ_SE_V (stream)) & 0xff;
            if(!i && !next) { /* matrix not written, we use the preset one */
                break;
            }
            last = next ? next : last;
        }
    }
}

void decode_scaling_matrices(DEC_BS_T *stream, int is_sps)
{
    decode_scaling_list(stream, 16); // Intra, Y
    decode_scaling_list(stream, 16); // Intra, Cr
    decode_scaling_list(stream, 16); // Intra, Cb
    decode_scaling_list(stream, 16); // Inter, Y
    decode_scaling_list(stream, 16); // Inter, Cr
    decode_scaling_list(stream, 16); // Inter, Cb
    if(is_sps) {
        decode_scaling_list(stream,64);  // Intra, Y
        decode_scaling_list(stream,64);  // Inter, Y
    }
}

void H264Dec_InitBitstream(DEC_BS_T * stream, void *pOneFrameBitstream, int length)
{
    stream->bitcnt = 0;
    stream->rdptr = (unsigned int *)pOneFrameBitstream;
    stream->bitsLeft   = 32;
    stream->bitcnt_before_vld = stream->bitcnt;
    length = 0;
}

int  InterpretH264Header (DEC_BS_T *stream)
{
    char        profile_idc; //u(8)
    char        chroma_format_idc;
    char        pic_order_cnt_type;     //
    char        num_ref_frames;    //ue(v)
    char        frame_mbs_only_flag;
    char        seq_scaling_matrix_present_flag;
    char        frame_is_mbaff;

    //int reserved_zero;
    unsigned int	num_ref_frames_in_pic_order_cnt_cycle;	//ue(v)

    profile_idc = READ_FLC(stream, 8); //sps_ptr->profile_idc
    SCI_TRACE_LOW("%s, %d, profile_idc = %d\n", __FUNCTION__, __LINE__, profile_idc);
    if ((profile_idc != 0x42) && (profile_idc != 0x4d) && (profile_idc != 0x64))//0x42: bp, 0x4d: mp, 0x64: hp
    {
        return -1;
    }

    READ_BITS1(stream); //sps_ptr->constrained_set0_flag
    READ_BITS1(stream); //sps_ptr->constrained_set1_flag
    READ_BITS1(stream); //sps_ptr->constrained_set2_flag
    READ_BITS1(stream); //sps_ptr->constrained_set3_flag

    READ_FLC(stream, 4); //reserved_zero

    READ_FLC(stream, 8); // sps_ptr->level_idc
    READ_UE_V(stream); //sps_ptr->seq_parameter_set_id

    if (profile_idc == 0x64) //hp
    {
        chroma_format_idc = READ_UE_V(stream);
        if ((chroma_format_idc != 1))
        {
            return -1;
        }

        READ_UE_V(stream); //sps_ptr->bit_depth_luma_minus8
        READ_UE_V(stream); //sps_ptr->bit_depth_chroma_minus8
        READ_FLC(stream, 1); //sps_ptr->qpprime_y_zero_transform_bypass_flag
        seq_scaling_matrix_present_flag =   READ_BITS1(stream);
        if(seq_scaling_matrix_present_flag)
        {
            decode_scaling_matrices(stream, 1);
        }
    }

    READ_UE_V(stream); //log2_max_frame_num_minus4
    pic_order_cnt_type = READ_UE_V(stream);

    if (pic_order_cnt_type == 0)
    {
        READ_UE_V(stream); //log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1)
    {
        int i;

        READ_BITS1(stream); //sps_ptr->delta_pic_order_always_zero_flag
        H264Dec_Long_SEV(stream); //sps_ptr->offset_for_non_ref_pic
        H264Dec_Long_SEV(stream); //sps_ptr->offset_for_top_to_bottom_field
        num_ref_frames_in_pic_order_cnt_cycle = READ_UE_V(stream);

        if (num_ref_frames_in_pic_order_cnt_cycle > 255)
        {
            return -1;
        }

        for (i = 0; i < (int)(num_ref_frames_in_pic_order_cnt_cycle); i++)
        {
            H264Dec_Long_SEV(stream); //sps_ptr->offset_for_ref_frame[i]
        }
    }

    num_ref_frames = READ_UE_V(stream);
    SCI_TRACE_LOW("%s, %d, num_ref_frames = %d\n", __FUNCTION__, __LINE__, num_ref_frames);
    if (num_ref_frames > MAX_REF_FRAME_NUMBER)
    {
        return -1;
    }

    READ_BITS1(stream); //sps_ptr->gaps_in_frame_num_value_allowed_flag
    READ_UE_V(stream); //sps_ptr->pic_width_in_mbs_minus1
    READ_UE_V(stream); //sps_ptr->pic_height_in_map_units_minus1

    frame_mbs_only_flag = READ_BITS1(stream);

    SCI_TRACE_LOW("%s, %d, frame_mbs_only_flag = %d\n", __FUNCTION__, __LINE__, frame_mbs_only_flag);

    if (frame_mbs_only_flag)
    {
        return 0;
    }
    else
    {
        frame_is_mbaff = READ_BITS1(stream);
        SCI_TRACE_LOW("%s, %d, frame_is_mbaff = %d\n", __FUNCTION__, __LINE__, frame_is_mbaff);
        return 1;
    }
}

static int get_unit (unsigned char** nalu_buf, unsigned int* p_nalu_len,  unsigned char *pInStream, unsigned int frm_bs_len, unsigned int *slice_unit_len)
{
    int len = 0;
    unsigned char *ptr;
    unsigned char data = 0;
    int declen = 0;
    int zero_num = 0;
    int startCode_len = 0;
    int stuffing_num = 0;
    int *stream_ptr;// = (int32 *)bfr;
    int code = 0;
    unsigned long byte_rest;

    ptr = pInStream;

//    SCI_TRACE_LOW("get_unit 0, frm_bs_len %d, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x, %0x\n",frm_bs_len,
//    ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7], ptr[8], ptr[9], ptr[10], ptr[11], ptr[12], ptr[13], ptr[14], ptr[15]);

    //start code
    while ((data = *ptr++) == 0x00)
    {
        len++;
    }
    len++;
    declen += len;

//    g_nalu_ptr->buf += (*start_code_len);

    byte_rest = (unsigned long)pInStream;
    byte_rest = ((byte_rest)>>2)<<2;	//word aligned

    //destuffing
    stream_ptr = (int *)(byte_rest);
    *nalu_buf = (unsigned char *)(byte_rest);
//	len = 0;

    //read til next start code, and remove the third start code code emulation byte
    byte_rest = 4;
    declen = frm_bs_len - len;
    //while (declen < frm_bs_len)
    do
    {
        data = *ptr++;
//        len++;
//        declen++;

        if (zero_num < 2)
        {
            //*bfr++ = data;
            zero_num++;
            byte_rest--;
            //if (byte_rest >= 0)
            //{
                code = (code <<8) | data;	//big endian
            //}
            if (0 == byte_rest)
            {
                byte_rest = 4;
                *stream_ptr++ = code;
            }

            if(data != 0)
            {
                zero_num = 0;
            }
        } else
        {
            if ((zero_num == 2) && (data == 0x03))
            {
                zero_num = 0;
                stuffing_num++;
                goto next_data;
            } else
            {
                //*bfr++ = data;
                byte_rest--;
                //if (byte_rest >= 0)
                //{
                    code = (code<<8) | data;	//big endian
                //}
                if (0 == byte_rest)
                {
                    byte_rest = 4;
                    *stream_ptr++ = code;
                }

                if (data == 0x1)
                {
                    //	if (zero_num >= 2)
                    {
                        startCode_len = zero_num + 1;
                        break;
                    }
                } else if (data == 0x00)
                {
                    zero_num++;
                } else
                {
                    zero_num = 0;
                }
            }
        }
next_data:
        declen--;
    } while(declen);

#if 0
    if (((unsigned long)stream_ptr) == (((((unsigned long)ptr) - startCode_len/*len*/) >> 2) << 2))
    {
        img_ptr->g_need_back_last_word = 1;
        img_ptr->g_back_last_word = *stream_ptr;
    } else
    {
        img_ptr->g_need_back_last_word = 0;
    }
#endif

    *stream_ptr++ = code << (byte_rest*8);

    if (declen == 0)
    {
        declen = 1;
    }
    declen = frm_bs_len - declen;
    declen++;
    len = declen - len;

    *slice_unit_len = (declen - startCode_len /*+ stuffing_num*/);

    *p_nalu_len = len - startCode_len - stuffing_num;

    while (code && !(code&0xff))
    {
        (*p_nalu_len)--;
        code >>= 8;

//        SCI_TRACE_LOW("code: %0x, nal->len: %d", code, g_nalu_ptr->len);
    }
    declen -= startCode_len;

    if ((unsigned long)declen >= frm_bs_len)
    {
//        declen = 0;
//        s_bFisrtUnit = TRUE;
        return 1;
    }

    return 0;
}


/*********************************************************
*bitstrm_ptr: input bitstream buffer
*bitstrm_len: input bitstream length
*return value:
*  1      : interlaced
*  0      : not interlaced
* -1     :error
*********************************************************/
int isInterlacedSequence(const unsigned char *bitstrm_ptr, size_t bitstrm_len)
{
    DEC_BS_T stream;
    unsigned int slice_unit_len = 0;
    unsigned int nalu_len = 0;
    unsigned char* nalu_buf = 0;
    unsigned char* pstream = 0;
    char nal_unit_type;
    int ret;

    pstream = (unsigned char*)malloc(bitstrm_len+4);

    if (*bitstrm_ptr == 0
            && *(bitstrm_ptr + 1) == 0
            && *(bitstrm_ptr + 2) == 0
            && *(bitstrm_ptr + 3) == 1)
    {
        memcpy(pstream, bitstrm_ptr, bitstrm_len);
    }
    else
    {
        memcpy(pstream+4, bitstrm_ptr, bitstrm_len);
        *pstream = 0;
        *(pstream + 1) = 0;
        *(pstream + 2) = 0;
        *(pstream + 3) = 1;
    }

    get_unit (&nalu_buf, &nalu_len, pstream, bitstrm_len, &slice_unit_len);

    H264Dec_InitBitstream(&stream, nalu_buf, nalu_len);

    nal_unit_type = READ_FLC(&stream, 8) & 0x1f;

    if (nal_unit_type == 7)
    {
        ret =  InterpretH264Header(&stream);
    } else
    {
        ret = -1;
    }

    SCI_TRACE_LOW("%s, Interlaced: %d\n", __FUNCTION__, ret);

    free(pstream);
    return ret;
}
