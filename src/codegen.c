/*

 This file is part of FFTS -- The Fastest Fourier Transform in the South

 Copyright (c) 2012, Anthony M. Blake <amb@anthonix.com>
 Copyright (c) 2012, The University of Waikato

 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 	* Redistributions of source code must retain the above copyright
 		notice, this list of conditions and the following disclaimer.
 	* Redistributions in binary form must reproduce the above copyright
 		notice, this list of conditions and the following disclaimer in the
 		documentation and/or other materials provided with the distribution.
 	* Neither the name of the organization nor the
	  names of its contributors may be used to endorse or promote products
 		derived from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL ANTHONY M. BLAKE BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "codegen.h"
#include "macros.h"
#include "ffts.h"

#ifdef __arm__
typedef uint32_t insns_t;
#else
typedef uint8_t insns_t;
#endif

#ifdef HAVE_NEON
#include "codegen_arm.h"
#include "neon.h"
#elif HAVE_VFP
#include "codegen_arm.h"
#include "vfp.h"
#else
#include "codegen_sse.h"
#include "macros-sse.h"
#endif

#include <assert.h>
#include <errno.h>
#include <stddef.h>
/* #include <stdio.h> */
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <unistd.h>
#endif

static int ffts_tree_count(int N, int leaf_N, int offset)
{
    int count;

    if (N <= leaf_N) {
        return 0;
    }

    count  = ffts_tree_count(N/4, leaf_N, offset);
    count += ffts_tree_count(N/8, leaf_N, offset + N/4);
    count += ffts_tree_count(N/8, leaf_N, offset + N/4 + N/8);
    count += ffts_tree_count(N/4, leaf_N, offset + N/2);
    count += ffts_tree_count(N/4, leaf_N, offset + 3*N/4);

    return 1 + count;
}

static void ffts_elaborate_tree(size_t **p, int N, int leaf_N, int offset)
{
    if (N <= leaf_N) {
        return;
    }

    ffts_elaborate_tree(p, N/4, leaf_N, offset);
    ffts_elaborate_tree(p, N/8, leaf_N, offset + N/4);
    ffts_elaborate_tree(p, N/8, leaf_N, offset + N/4 + N/8);
    ffts_elaborate_tree(p, N/4, leaf_N, offset + N/2);
    ffts_elaborate_tree(p, N/4, leaf_N, offset + 3*N/4);

    (*p)[0] = N;
    (*p)[1] = 2 * offset;

    (*p) += 2;
}

transform_func_t ffts_generate_func_code(ffts_plan_t *p, size_t N, size_t leaf_N, int sign)
{
    uint32_t offsets[8] = {0, N, N/2, 3*N/2, N/4, 5*N/4, 7*N/4, 3*N/4};
    uint32_t offsets_o[8] = {0, N, N/2, 3*N/2, 7*N/4, 3*N/4, N/4, 5*N/4};

    int32_t pAddr = 0;
    int32_t pN = 0;
    int32_t pLUT = 0;

    insns_t  *fp;
    insns_t  *start;
    insns_t  *x_4_addr;
    insns_t  *x_8_addr;
    uint32_t  loop_count;

    int       count;
    int       i;
    ptrdiff_t len;

    size_t   *ps;
    size_t   *pps;

    count = ffts_tree_count(N, leaf_N, 0) + 1;

    ps = pps = malloc(2 * count * sizeof(*ps));
    if (!ps) {
        return NULL;
    }

    ffts_elaborate_tree(&pps, N, leaf_N, 0);

    pps[0] = 0;
    pps[1] = 0;

    pps = ps;

#ifdef __x86_64__
    if (sign < 0) {
        p->constants = sse_constants;
    } else {
        p->constants = sse_constants_inv;
    }
#endif

    fp = (insns_t*) p->transform_base;

	/* generate base cases */
	x_4_addr = generate_size4_base_case(&fp, sign);
	x_8_addr = generate_size8_base_case(&fp, sign);

#ifdef __arm__
	start = generate_prologue(&fp, p);
#else
	start = generate_prologue(&fp, p);

    /* assign loop counter register */
    loop_count = 4 * p->i0;
#ifdef _M_X64
    MOVI(&fp, EBX, loop_count);
#else
    MOVI(&fp, ECX, loop_count);
#endif
#endif

#ifdef __arm__
#ifdef HAVE_NEON
    memcpy(fp, neon_ee, neon_oo - neon_ee);
    if (sign < 0) {
        fp[33] ^= 0x00200000;
        fp[37] ^= 0x00200000;
        fp[38] ^= 0x00200000;
        fp[39] ^= 0x00200000;
        fp[40] ^= 0x00200000;
        fp[41] ^= 0x00200000;
        fp[44] ^= 0x00200000;
        fp[45] ^= 0x00200000;
        fp[46] ^= 0x00200000;
        fp[47] ^= 0x00200000;
        fp[48] ^= 0x00200000;
        fp[57] ^= 0x00200000;
    }

    fp += (neon_oo - neon_ee) / 4;
#else
    memcpy(fp, vfp_e, vfp_o - vfp_e);

    if (sign > 0) {
        fp[64] ^= 0x00000040;
        fp[65] ^= 0x00000040;
        fp[68] ^= 0x00000040;
        fp[75] ^= 0x00000040;
        fp[76] ^= 0x00000040;
        fp[79] ^= 0x00000040;
        fp[80] ^= 0x00000040;
        fp[83] ^= 0x00000040;
        fp[84] ^= 0x00000040;
        fp[87] ^= 0x00000040;
        fp[91] ^= 0x00000040;
        fp[93] ^= 0x00000040;
    }
    fp += (vfp_o - vfp_e) / 4;
#endif
#else
    //fprintf(stderr, "Body start address = %016p\n", start);

    /* copy function */
    assert((char*) leaf_ee > (char*) leaf_ee_init);
    len = (char*) leaf_ee - (char*) leaf_ee_init;
    memcpy(fp, leaf_ee_init, (size_t) len);
    fp += len;

    /* align loop/jump destination */
#ifdef _M_X64
    ffts_align_mem16(&fp, 8);
#else
    ffts_align_mem16(&fp, 9);
#endif

    /* copy function */
    assert((char*) leaf_oo > (char*) leaf_ee);
    len = (char*) leaf_oo - (char*) leaf_ee;
    memcpy(fp, leaf_ee, (size_t) len);

    /* patch offsets */
    for (i = 0; i < 8; i++) {
        IMM32_NI(fp + sse_leaf_ee_offsets[i], 4 * offsets[i]);
    }

    fp += len;

    if (ffts_ctzl(N) & 1) {
        if (p->i1) {
            loop_count += 4 * p->i1;

            /* align loop/jump destination */
#ifdef _M_X64
            MOVI(&fp, EBX, loop_count);
            ffts_align_mem16(&fp, 3);
#else
            MOVI(&fp, ECX, loop_count);
            ffts_align_mem16(&fp, 4);
#endif

            /* copy function */
            assert((char*) leaf_eo > (char*) leaf_oo);
            len = (char*) leaf_eo - (char*) leaf_oo;
            memcpy(fp, leaf_oo, len);

            /* patch offsets */
            for (i = 0; i < 8; i++) {
                IMM32_NI(fp + sse_leaf_oo_offsets[i], 4 * offsets_o[i]);
            }

            fp += len;
        }

        loop_count += 4;

        /* copy function */
        assert((char*) leaf_end > (char*) leaf_oe);
        len = (char*) leaf_end - (char*) leaf_oe;
        memcpy(fp, leaf_oe, len);

        /* patch offsets */
        for (i = 0; i < 8; i++) {
            IMM32_NI(fp + sse_leaf_oe_offsets[i], 4 * offsets_o[i]);
        }

        fp += len;
    } else {
        loop_count += 4;

        /* copy function */
        assert((char*) leaf_oe > (char*) leaf_eo);
        len = (char*) leaf_oe - (char*) leaf_eo;
        memcpy(fp, leaf_eo, len);

        /* patch offsets */
        for (i = 0; i < 8; i++) {
            IMM32_NI(fp + sse_leaf_eo_offsets[i], 4 * offsets[i]);
        }

        fp += len;

        if (p->i1) {
            loop_count += 4 * p->i1;

            /* align loop/jump destination */
#ifdef _M_X64
            MOVI(&fp, EBX, loop_count);
            ffts_align_mem16(&fp, 3);
#else
            MOVI(&fp, ECX, loop_count);
            ffts_align_mem16(&fp, 4);
#endif

            /* copy function */
            assert((char*) leaf_eo > (char*) leaf_oo);
            len = (char*) leaf_eo - (char*) leaf_oo;
            memcpy(fp, leaf_oo, len);

            for (i = 0; i < 8; i++) {
                IMM32_NI(fp + sse_leaf_oo_offsets[i], 4 * offsets_o[i]);
            }

            fp += len;
        }
    }

    if (p->i1) {
        uint32_t offsets_oe[8] = {7*N/4, 3*N/4, N/4, 5*N/4, 0, N, 3*N/2, N/2};

        loop_count += 4 * p->i1;

        /* align loop/jump destination */
#ifdef _M_X64
        MOVI(&fp, EBX, loop_count);
        ffts_align_mem16(&fp, 8);
#else
        MOVI(&fp, ECX, loop_count);
        ffts_align_mem16(&fp, 9);
#endif

        assert((char*) leaf_oo > (char*) leaf_ee);
        len = (char*) leaf_oo - (char*) leaf_ee;
        memcpy(fp, leaf_ee, len);

        for (i = 0; i < 8; i++) {
            IMM32_NI(fp + sse_leaf_ee_offsets[i], 4 * offsets_oe[i]);
        }

        fp += len;
    }

	assert((char*) x4 > (char*) x_init);
    len = (char*) x4 - (char*) x_init;
    memcpy(fp, x_init, len);
    fp += len;

	/* generate subtransform calls */
    count = 2;
    while (pps[0]) {
        size_t ws_is;

        if (!pN) {
#ifdef _M_X64
            MOVI(&fp, EBX, pps[0]);
#else
            MOVI(&fp, ECX, pps[0] / 4);
#endif
        } else {
            int offset = (4 * pps[1]) - pAddr;
            if (offset) {
#ifdef _M_X64
                ADDI(&fp, R8, offset);
#else
                ADDI(&fp, RDX, offset);
#endif
            }

            if (pps[0] > leaf_N && pps[0] - pN) {
                int factor = ffts_ctzl(pps[0]) - ffts_ctzl(pN);

#ifdef _M_X64
                SHIFT(&fp, EBX, factor);
#else
                SHIFT(&fp, ECX, factor);
#endif
            }
        }

        ws_is = 8 * p->ws_is[ffts_ctzl(pps[0] / leaf_N) - 1];
        if (ws_is != pLUT) {
            int offset = (int) (ws_is - pLUT);

#ifdef _M_X64
            ADDI(&fp, RDI, offset);
#else
            ADDI(&fp, R8, offset);
#endif
        }

        if (pps[0] == 2 * leaf_N) {
            CALL(&fp, x_4_addr);
        } else {
            CALL(&fp, x_8_addr);
        }

        pAddr = 4 * pps[1];
        if (pps[0] > leaf_N) {
            pN = pps[0];
        }

        pLUT = ws_is;//LUT_offset(pps[0], leafN);
        //fprintf(stderr, "LUT offset for %d is %d\n", pN, pLUT);
        count += 4;
        pps += 2;
    }
#endif

#ifdef __arm__
#ifdef HAVE_NEON
    if(__builtin_ctzl(N) & 1) {
        ADDI(&fp, 2, 7, 0);
        ADDI(&fp, 7, 9, 0);
        ADDI(&fp, 9, 2, 0);

        ADDI(&fp, 2, 8, 0);
        ADDI(&fp, 8, 10, 0);
        ADDI(&fp, 10, 2, 0);

        if(p->i1) {
            MOVI(&fp, 11, p->i1);
            memcpy(fp, neon_oo, neon_eo - neon_oo);
            if(sign < 0) {
                fp[12] ^= 0x00200000;
                fp[13] ^= 0x00200000;
                fp[14] ^= 0x00200000;
                fp[15] ^= 0x00200000;
                fp[27] ^= 0x00200000;
                fp[29] ^= 0x00200000;
                fp[30] ^= 0x00200000;
                fp[31] ^= 0x00200000;
                fp[46] ^= 0x00200000;
                fp[47] ^= 0x00200000;
                fp[48] ^= 0x00200000;
                fp[57] ^= 0x00200000;
            }
            fp += (neon_eo - neon_oo) / 4;
        }

        *fp = LDRI(11, 1, ((uint32_t)&p->oe_ws) - ((uint32_t)p));
        fp++;

        memcpy(fp, neon_oe, neon_end - neon_oe);
        if(sign < 0) {
            fp[19] ^= 0x00200000;
            fp[20] ^= 0x00200000;
            fp[22] ^= 0x00200000;
            fp[23] ^= 0x00200000;
            fp[37] ^= 0x00200000;
            fp[38] ^= 0x00200000;
            fp[40] ^= 0x00200000;
            fp[41] ^= 0x00200000;
            fp[64] ^= 0x00200000;
            fp[65] ^= 0x00200000;
            fp[66] ^= 0x00200000;
            fp[67] ^= 0x00200000;
        }
        fp += (neon_end - neon_oe) / 4;

    } else {

        *fp = LDRI(11, 1, ((uint32_t)&p->eo_ws) - ((uint32_t)p));
        fp++;

        memcpy(fp, neon_eo, neon_oe - neon_eo);
        if(sign < 0) {
            fp[10] ^= 0x00200000;
            fp[11] ^= 0x00200000;
            fp[13] ^= 0x00200000;
            fp[14] ^= 0x00200000;
            fp[31] ^= 0x00200000;
            fp[33] ^= 0x00200000;
            fp[34] ^= 0x00200000;
            fp[35] ^= 0x00200000;
            fp[59] ^= 0x00200000;
            fp[60] ^= 0x00200000;
            fp[61] ^= 0x00200000;
            fp[62] ^= 0x00200000;
        }
        fp += (neon_oe - neon_eo) / 4;

        ADDI(&fp, 2, 7, 0);
        ADDI(&fp, 7, 9, 0);
        ADDI(&fp, 9, 2, 0);

        ADDI(&fp, 2, 8, 0);
        ADDI(&fp, 8, 10, 0);
        ADDI(&fp, 10, 2, 0);

        if(p->i1) {
            MOVI(&fp, 11, p->i1);
            memcpy(fp, neon_oo, neon_eo - neon_oo);
            if(sign < 0) {
                fp[12] ^= 0x00200000;
                fp[13] ^= 0x00200000;
                fp[14] ^= 0x00200000;
                fp[15] ^= 0x00200000;
                fp[27] ^= 0x00200000;
                fp[29] ^= 0x00200000;
                fp[30] ^= 0x00200000;
                fp[31] ^= 0x00200000;
                fp[46] ^= 0x00200000;
                fp[47] ^= 0x00200000;
                fp[48] ^= 0x00200000;
                fp[57] ^= 0x00200000;
            }
            fp += (neon_eo - neon_oo) / 4;
        }

    }


    if(p->i1) {
        ADDI(&fp, 2, 3, 0);
        ADDI(&fp, 3, 7, 0);
        ADDI(&fp, 7, 2, 0);

        ADDI(&fp, 2, 4, 0);
        ADDI(&fp, 4, 8, 0);
        ADDI(&fp, 8, 2, 0);

        ADDI(&fp, 2, 5, 0);
        ADDI(&fp, 5, 9, 0);
        ADDI(&fp, 9, 2, 0);

        ADDI(&fp, 2, 6, 0);
        ADDI(&fp, 6, 10, 0);
        ADDI(&fp, 10, 2, 0);

        ADDI(&fp, 2, 9, 0);
        ADDI(&fp, 9, 10, 0);
        ADDI(&fp, 10, 2, 0);

        *fp = LDRI(2, 1, ((uint32_t)&p->ee_ws) - ((uint32_t)p));
        fp++;
        MOVI(&fp, 11, p->i1);
        memcpy(fp, neon_ee, neon_oo - neon_ee);
        if(sign < 0) {
            fp[33] ^= 0x00200000;
            fp[37] ^= 0x00200000;
            fp[38] ^= 0x00200000;
            fp[39] ^= 0x00200000;
            fp[40] ^= 0x00200000;
            fp[41] ^= 0x00200000;
            fp[44] ^= 0x00200000;
            fp[45] ^= 0x00200000;
            fp[46] ^= 0x00200000;
            fp[47] ^= 0x00200000;
            fp[48] ^= 0x00200000;
            fp[57] ^= 0x00200000;
        }
        fp += (neon_oo - neon_ee) / 4;

    }
#else
    ADDI(&fp, 2, 7, 0);
    ADDI(&fp, 7, 9, 0);
    ADDI(&fp, 9, 2, 0);

    ADDI(&fp, 2, 8, 0);
    ADDI(&fp, 8, 10, 0);
    ADDI(&fp, 10, 2, 0);

    MOVI(&fp, 11, (p->i1>0) ? p->i1 : 1);
    memcpy(fp, vfp_o, vfp_x4 - vfp_o);
    if(sign > 0) {
        fp[22] ^= 0x00000040;
        fp[24] ^= 0x00000040;
        fp[25] ^= 0x00000040;
        fp[26] ^= 0x00000040;
        fp[62] ^= 0x00000040;
        fp[64] ^= 0x00000040;
        fp[65] ^= 0x00000040;
        fp[66] ^= 0x00000040;
    }
    fp += (vfp_x4 - vfp_o) / 4;

    ADDI(&fp, 2, 3, 0);
    ADDI(&fp, 3, 7, 0);
    ADDI(&fp, 7, 2, 0);

    ADDI(&fp, 2, 4, 0);
    ADDI(&fp, 4, 8, 0);
    ADDI(&fp, 8, 2, 0);

    ADDI(&fp, 2, 5, 0);
    ADDI(&fp, 5, 9, 0);
    ADDI(&fp, 9, 2, 0);

    ADDI(&fp, 2, 6, 0);
    ADDI(&fp, 6, 10, 0);
    ADDI(&fp, 10, 2, 0);

    ADDI(&fp, 2, 9, 0);
    ADDI(&fp, 9, 10, 0);
    ADDI(&fp, 10, 2, 0);

    *fp = LDRI(2, 1, ((uint32_t)&p->ee_ws) - ((uint32_t)p));
    fp++;
    MOVI(&fp, 11, (p->i2>0) ? p->i2 : 1);
    memcpy(fp, vfp_e, vfp_o - vfp_e);
    if(sign > 0) {
        fp[64] ^= 0x00000040;
        fp[65] ^= 0x00000040;
        fp[68] ^= 0x00000040;
        fp[75] ^= 0x00000040;
        fp[76] ^= 0x00000040;
        fp[79] ^= 0x00000040;
        fp[80] ^= 0x00000040;
        fp[83] ^= 0x00000040;
        fp[84] ^= 0x00000040;
        fp[87] ^= 0x00000040;
        fp[91] ^= 0x00000040;
        fp[93] ^= 0x00000040;
    }
    fp += (vfp_o - vfp_e) / 4;

#endif
    *fp = LDRI(2, 1, ((uint32_t)&p->ws) - ((uint32_t)p));
    fp++; // load offsets into r12
    //ADDI(&fp, 2, 1, 0);
    MOVI(&fp, 1, 0);

    // args: r0 - out
    //       r1 - N
    //       r2 - ws
    //	ADDI(&fp, 3, 1, 0); // put N into r3 for counter

    int32_t pAddr = 0;
    int32_t pN = 0;
    int32_t pLUT = 0;
    count = 2;
    while(pps[0]) {

        //	fprintf(stderr, "size %zu at %zu - diff %zu\n", pps[0], pps[1]*4, (pps[1]*4) - pAddr);
        if(!pN) {
            MOVI(&fp, 1, pps[0]);
        } else {
            if((pps[1]*4)-pAddr) ADDI(&fp, 0, 0, (pps[1] * 4)- pAddr);
            if(pps[0] - pN) ADDI(&fp, 1, 1, pps[0] - pN);
        }

        if(p->ws_is[__builtin_ctzl(pps[0]/leafN)-1]*8 - pLUT)
            ADDI(&fp, 2, 2, p->ws_is[__builtin_ctzl(pps[0]/leafN)-1]*8 - pLUT);


        if(pps[0] == 2*leafN) {
            *fp = BL(fp+2, x_4_addr);
            fp++;
        } else if(!pps[2]) {
            //uint32_t *x_8_t_addr = fp;
#ifdef HAVE_NEON
            memcpy(fp, neon_x8_t, neon_ee - neon_x8_t);
            if(sign < 0) {
                fp[31] ^= 0x00200000;
                fp[32] ^= 0x00200000;
                fp[33] ^= 0x00200000;
                fp[34] ^= 0x00200000;
                fp[65] ^= 0x00200000;
                fp[66] ^= 0x00200000;
                fp[70] ^= 0x00200000;
                fp[74] ^= 0x00200000;
                fp[97] ^= 0x00200000;
                fp[98] ^= 0x00200000;
                fp[102] ^= 0x00200000;
                fp[104] ^= 0x00200000;
            }
            fp += (neon_ee - neon_x8_t) / 4;
            //*fp++ = BL(fp+2, x_8_t_addr);

#else
            *fp = BL(fp+2, x_8_addr);
            fp++;
#endif
        } else {
            *fp = BL(fp+2, x_8_addr);
            fp++;
        }

        pAddr = pps[1] * 4;
        pN = pps[0];
        pLUT = p->ws_is[__builtin_ctzl(pps[0]/leafN)-1]*8;//LUT_offset(pps[0], leafN);
        //	fprintf(stderr, "LUT offset for %d is %d\n", pN, pLUT);
        count += 4;
        pps += 2;
    }

    *fp++ = 0xecbd8b10;
    *fp++ = POP_LR();
    count++;
#else
	generate_epilogue(&fp);
#endif

    //	*fp++ = B(14); count++;

    //for(int i=0;i<(neon_x8 - neon_x4)/4;i++)
    //	fprintf(stderr, "%08x\n", x_4_addr[i]);
    //fprintf(stderr, "\n");
    //for(int i=0;i<count;i++)

    fprintf(stderr, "size of transform %u = %d\n", N, (fp - x_8_addr) * sizeof(*fp));

    free(ps);

    return (transform_func_t) start;
}