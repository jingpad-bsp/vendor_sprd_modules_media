#ifndef _AVC_UTILS_SPRD_H_
#define _AVC_UTILS_SPRD_H_

#include <utils/Log.h>
#include <utils/threads.h>
#include <utils/String8.h>

#define  SCI_TRACE_LOW ALOGI

#define MAX_REF_FRAME_NUMBER	16

typedef struct bitstream
{
    unsigned int bitcnt;
    unsigned int bitsLeft; // left bits in the word pointed by rdptr
    unsigned int *rdptr;
    unsigned int bitcnt_before_vld;
    unsigned int error_flag;
} DEC_BS_T;

int isInterlacedSequence(const unsigned char *bitstrm_ptr, size_t bitstrm_len);

#endif
