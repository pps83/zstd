/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/* zstd_decompress_block :
 * this module takes care of decompressing _compressed_ block */

/*-*******************************************************
*  Dependencies
*********************************************************/
#include "../common/zstd_deps.h"   /* ZSTD_memcpy, ZSTD_memmove, ZSTD_memset */
#include "../common/compiler.h"    /* prefetch */
#include "../common/cpu.h"         /* bmi2 */
#include "../common/mem.h"         /* low level memory routines */
#define FSE_STATIC_LINKING_ONLY
#include "../common/fse.h"
#include "../common/huf.h"
#include "../common/zstd_internal.h"
#include "zstd_decompress_internal.h"   /* ZSTD_DCtx */
#include "zstd_ddict.h"  /* ZSTD_DDictDictContent */
#include "zstd_decompress_block.h"
#include "../common/bits.h"  /* ZSTD_highbit32 */

/*_*******************************************************
*  Macros
**********************************************************/

/* These two optional macros force the use one way or another of the two
 * ZSTD_decompressSequences implementations. You can't force in both directions
 * at the same time.
 */
#if defined(ZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT) && \
    defined(ZSTD_FORCE_DECOMPRESS_SEQUENCES_LONG)
#error "Cannot force the use of the short and the long ZSTD_decompressSequences variants!"
#endif


/*_*******************************************************
*  Memory operations
**********************************************************/
static void ZSTD_copy4(void* dst, const void* src) { ZSTD_memcpy(dst, src, 4); }

typedef struct {
    size_t litLength;
    size_t matchLength;
    size_t offset;
} seq_t;

typedef struct {
    size_t state;
    const ZSTD_seqSymbol* table;
} ZSTD_fseState;

typedef struct {
    BIT_DStream_t DStream;
    ZSTD_fseState stateLL;
    ZSTD_fseState stateOffb;
    ZSTD_fseState stateML;
    size_t prevOffset[ZSTD_REP_NUM];
} seqState_t;

/*! ZSTD_overlapCopy8() :
 *  Copies 8 bytes from ip to op and updates op and ip where ip <= op.
 *  If the offset is < 8 then the offset is spread to at least 8 bytes.
 *
 *  Precondition: *ip <= *op
 *  Postcondition: *op - *op >= 8
 */
HINT_INLINE void ZSTD_overlapCopy8(BYTE** op, BYTE const** ip, size_t offset) {
    assert(*ip <= *op);
    if (offset < 8) {
        /* close range match, overlap */
        static const U32 dec32table[] = { 0, 1, 2, 1, 4, 4, 4, 4 };   /* added */
        static const int dec64table[] = { 8, 8, 8, 7, 8, 9,10,11 };   /* subtracted */
        int const sub2 = dec64table[offset];
        (*op)[0] = (*ip)[0];
        (*op)[1] = (*ip)[1];
        (*op)[2] = (*ip)[2];
        (*op)[3] = (*ip)[3];
        *ip += dec32table[offset];
        ZSTD_copy4(*op+4, *ip);
        *ip -= sub2;
    } else {
        ZSTD_copy8(*op, *ip);
    }
    *ip += 8;
    *op += 8;
    assert(*op - *ip >= 8);
}

/*! ZSTD_safecopy() :
 *  Specialized version of memcpy() that is allowed to READ up to WILDCOPY_OVERLENGTH past the input buffer
 *  and write up to 16 bytes past oend_w (op >= oend_w is allowed).
 *  This function is only called in the uncommon case where the sequence is near the end of the block. It
 *  should be fast for a single long sequence, but can be slow for several short sequences.
 *
 *  @param ovtype controls the overlap detection
 *         - ZSTD_no_overlap: The source and destination are guaranteed to be at least WILDCOPY_VECLEN bytes apart.
 *         - ZSTD_overlap_src_before_dst: The src and dst may overlap and may be any distance apart.
 *           The src buffer must be before the dst buffer.
 */
static void ZSTD_safecopy(BYTE* op, const BYTE* const oend_w, BYTE const* ip, ptrdiff_t length, ZSTD_overlap_e ovtype) {
    ptrdiff_t const diff = op - ip;
    BYTE* const oend = op + length;

    assert((ovtype == ZSTD_no_overlap && (diff <= -8 || diff >= 8 || op >= oend_w)) ||
           (ovtype == ZSTD_overlap_src_before_dst && diff >= 0));

    if (length < 8) {
        /* Handle short lengths. */
        while (op < oend) *op++ = *ip++;
        return;
    }
    if (ovtype == ZSTD_overlap_src_before_dst) {
        /* Copy 8 bytes and ensure the offset >= 8 when there can be overlap. */
        assert(length >= 8);
        ZSTD_overlapCopy8(&op, &ip, diff);
        length -= 8;
        assert(op - ip >= 8);
        assert(op <= oend);
    }

    if (oend <= oend_w) {
        /* No risk of overwrite. */
        ZSTD_wildcopy(op, ip, length, ovtype);
        return;
    }
    if (op <= oend_w) {
        /* Wildcopy until we get close to the end. */
        assert(oend > oend_w);
        ZSTD_wildcopy(op, ip, oend_w - op, ovtype);
        ip += oend_w - op;
        op += oend_w - op;
    }
    /* Handle the leftovers. */
    while (op < oend) *op++ = *ip++;
}

/* ZSTD_execSequenceEnd():
 * This version handles cases that are near the end of the output buffer. It requires
 * more careful checks to make sure there is no overflow. By separating out these hard
 * and unlikely cases, we can speed up the common cases.
 *
 * NOTE: This function needs to be fast for a single long sequence, but doesn't need
 * to be optimized for many small sequences, since those fall into ZSTD_execSequence().
 */
FORCE_NOINLINE
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
size_t ZSTD_execSequenceEnd(BYTE* op,
    BYTE* const oend, seq_t sequence,
    const BYTE** litPtr, const BYTE* const litLimit,
    const BYTE* const prefixStart, const BYTE* const virtualStart, const BYTE* const dictEnd)
{
    BYTE* const oLitEnd = op + sequence.litLength;
    size_t const sequenceLength = sequence.litLength + sequence.matchLength;
    const BYTE* const iLitEnd = *litPtr + sequence.litLength;
    const BYTE* match = oLitEnd - sequence.offset;
    BYTE* const oend_w = oend - WILDCOPY_OVERLENGTH;

    /* bounds checks : careful of address space overflow in 32-bit mode */
    RETURN_ERROR_IF(sequenceLength > (size_t)(oend - op), dstSize_tooSmall, "last match must fit within dstBuffer");
    RETURN_ERROR_IF(sequence.litLength > (size_t)(litLimit - *litPtr), corruption_detected, "try to read beyond literal buffer");
    assert(op < op + sequenceLength);
    assert(oLitEnd < op + sequenceLength);

    /* copy literals */
    ZSTD_safecopy(op, oend_w, *litPtr, sequence.litLength, ZSTD_no_overlap);
    op = oLitEnd;
    *litPtr = iLitEnd;

    /* copy Match */
    if (sequence.offset > (size_t)(oLitEnd - prefixStart)) {
        /* offset beyond prefix */
        RETURN_ERROR_IF(sequence.offset > (size_t)(oLitEnd - virtualStart), corruption_detected, "");
        match = dictEnd - (prefixStart - match);
        if (match + sequence.matchLength <= dictEnd) {
            ZSTD_memmove(oLitEnd, match, sequence.matchLength);
            return sequenceLength;
        }
        /* span extDict & currentPrefixSegment */
        {   size_t const length1 = dictEnd - match;
        ZSTD_memmove(oLitEnd, match, length1);
        op = oLitEnd + length1;
        sequence.matchLength -= length1;
        match = prefixStart;
        }
    }
    ZSTD_safecopy(op, oend_w, match, sequence.matchLength, ZSTD_overlap_src_before_dst);
    return sequenceLength;
}

HINT_INLINE
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
size_t ZSTD_execSequence(BYTE* op,
    BYTE* const oend, seq_t sequence,
    const BYTE** litPtr, const BYTE* const litLimit,
    const BYTE* const prefixStart, const BYTE* const virtualStart, const BYTE* const dictEnd)
{
    BYTE* const oLitEnd = op + sequence.litLength;
    size_t const sequenceLength = sequence.litLength + sequence.matchLength;
    BYTE* const oMatchEnd = op + sequenceLength;   /* risk : address space overflow (32-bits) */
    BYTE* const oend_w = oend - WILDCOPY_OVERLENGTH;   /* risk : address space underflow on oend=NULL */
    const BYTE* const iLitEnd = *litPtr + sequence.litLength;
    const BYTE* match = oLitEnd - sequence.offset;

    assert(op != NULL /* Precondition */);
    assert(oend_w < oend /* No underflow */);

#if defined(__aarch64__)
    /* prefetch sequence starting from match that will be used for copy later */
    PREFETCH_L1(match);
#endif
    /* Handle edge cases in a slow path:
     *   - Read beyond end of literals
     *   - Match end is within WILDCOPY_OVERLIMIT of oend
     *   - 32-bit mode and the match length overflows
     */
    if (UNLIKELY(
        iLitEnd > litLimit ||
        oMatchEnd > oend_w ||
        (MEM_32bits() && (size_t)(oend - op) < sequenceLength + WILDCOPY_OVERLENGTH)))
        return ZSTD_execSequenceEnd(op, oend, sequence, litPtr, litLimit, prefixStart, virtualStart, dictEnd);

    /* Assumptions (everything else goes into ZSTD_execSequenceEnd()) */
    assert(op <= oLitEnd /* No overflow */);
    assert(oLitEnd < oMatchEnd /* Non-zero match & no overflow */);
    assert(oMatchEnd <= oend /* No underflow */);
    assert(iLitEnd <= litLimit /* Literal length is in bounds */);
    assert(oLitEnd <= oend_w /* Can wildcopy literals */);
    assert(oMatchEnd <= oend_w /* Can wildcopy matches */);

    /* Copy Literals:
     * Split out litLength <= 16 since it is nearly always true. +1.6% on gcc-9.
     * We likely don't need the full 32-byte wildcopy.
     */
    assert(WILDCOPY_OVERLENGTH >= 16);
    ZSTD_copy16(op, (*litPtr));
    if (UNLIKELY(sequence.litLength > 16)) {
        ZSTD_wildcopy(op + 16, (*litPtr) + 16, sequence.litLength - 16, ZSTD_no_overlap);
    }
    op = oLitEnd;
    *litPtr = iLitEnd;   /* update for next sequence */

    /* Copy Match */
    if (sequence.offset > (size_t)(oLitEnd - prefixStart)) {
        /* offset beyond prefix -> go into extDict */
        RETURN_ERROR_IF(UNLIKELY(sequence.offset > (size_t)(oLitEnd - virtualStart)), corruption_detected, "");
        match = dictEnd + (match - prefixStart);
        if (match + sequence.matchLength <= dictEnd) {
            ZSTD_memmove(oLitEnd, match, sequence.matchLength);
            return sequenceLength;
        }
        /* span extDict & currentPrefixSegment */
        {   size_t const length1 = dictEnd - match;
        ZSTD_memmove(oLitEnd, match, length1);
        op = oLitEnd + length1;
        sequence.matchLength -= length1;
        match = prefixStart;
        }
    }
    /* Match within prefix of 1 or more bytes */
    assert(op <= oMatchEnd);
    assert(oMatchEnd <= oend_w);
    assert(match >= prefixStart);
    assert(sequence.matchLength >= 1);

    /* Nearly all offsets are >= WILDCOPY_VECLEN bytes, which means we can use wildcopy
     * without overlap checking.
     */
    if (LIKELY(sequence.offset >= WILDCOPY_VECLEN)) {
        /* We bet on a full wildcopy for matches, since we expect matches to be
         * longer than literals (in general). In silesia, ~10% of matches are longer
         * than 16 bytes.
         */
        ZSTD_wildcopy(op, match, (ptrdiff_t)sequence.matchLength, ZSTD_no_overlap);
        return sequenceLength;
    }
    assert(sequence.offset < WILDCOPY_VECLEN);

    /* Copy 8 bytes and spread the offset to be >= 8. */
    ZSTD_overlapCopy8(&op, &match, sequence.offset);

    /* If the match length is > 8 bytes, then continue with the wildcopy. */
    if (sequence.matchLength > 8) {
        assert(op < oMatchEnd);
        ZSTD_wildcopy(op, match, (ptrdiff_t)sequence.matchLength - 8, ZSTD_overlap_src_before_dst);
    }
    return sequenceLength;
}

static void
ZSTD_initFseState(ZSTD_fseState* DStatePtr, BIT_DStream_t* bitD, const ZSTD_seqSymbol* dt)
{
    const void* ptr = dt;
    const ZSTD_seqSymbol_header* const DTableH = (const ZSTD_seqSymbol_header*)ptr;
    DStatePtr->state = BIT_readBits(bitD, DTableH->tableLog);
    DEBUGLOG(6, "ZSTD_initFseState : val=%u using %u bits",
                (U32)DStatePtr->state, DTableH->tableLog);
    BIT_reloadDStream(bitD);
    DStatePtr->table = dt + 1;
}

FORCE_INLINE_TEMPLATE void
ZSTD_updateFseStateWithDInfo(ZSTD_fseState* DStatePtr, BIT_DStream_t* bitD, U16 nextState, U32 nbBits)
{
    size_t const lowBits = BIT_readBits(bitD, nbBits);
    DStatePtr->state = nextState + lowBits;
}

/* We need to add at most (ZSTD_WINDOWLOG_MAX_32 - 1) bits to read the maximum
 * offset bits. But we can only read at most STREAM_ACCUMULATOR_MIN_32
 * bits before reloading. This value is the maximum number of bytes we read
 * after reloading when we are decoding long offsets.
 */
#define LONG_OFFSETS_MAX_EXTRA_BITS_32                       \
    (ZSTD_WINDOWLOG_MAX_32 > STREAM_ACCUMULATOR_MIN_32       \
        ? ZSTD_WINDOWLOG_MAX_32 - STREAM_ACCUMULATOR_MIN_32  \
        : 0)

typedef enum { ZSTD_lo_isRegularOffset, ZSTD_lo_isLongOffset=1 } ZSTD_longOffset_e;

/**
 * ZSTD_decodeSequence():
 * @p longOffsets : tells the decoder to reload more bit while decoding large offsets
 *                  only used in 32-bit mode
 * @return : Sequence (litL + matchL + offset)
 */
FORCE_INLINE_TEMPLATE seq_t
ZSTD_decodeSequence(seqState_t* seqState, const ZSTD_longOffset_e longOffsets, const int isLastSeq)
{
    seq_t seq;
    /*
     * ZSTD_seqSymbol is a 64 bits wide structure.
     * It can be loaded in one operation
     * and its fields extracted by simply shifting or bit-extracting on aarch64.
     * GCC doesn't recognize this and generates more unnecessary ldr/ldrb/ldrh
     * operations that cause performance drop. This can be avoided by using this
     * ZSTD_memcpy hack.
     */
#if defined(__aarch64__) && (defined(__GNUC__) && !defined(__clang__))
    ZSTD_seqSymbol llDInfoS, mlDInfoS, ofDInfoS;
    ZSTD_seqSymbol* const llDInfo = &llDInfoS;
    ZSTD_seqSymbol* const mlDInfo = &mlDInfoS;
    ZSTD_seqSymbol* const ofDInfo = &ofDInfoS;
    ZSTD_memcpy(llDInfo, seqState->stateLL.table + seqState->stateLL.state, sizeof(ZSTD_seqSymbol));
    ZSTD_memcpy(mlDInfo, seqState->stateML.table + seqState->stateML.state, sizeof(ZSTD_seqSymbol));
    ZSTD_memcpy(ofDInfo, seqState->stateOffb.table + seqState->stateOffb.state, sizeof(ZSTD_seqSymbol));
#else
    const ZSTD_seqSymbol* const llDInfo = seqState->stateLL.table + seqState->stateLL.state;
    const ZSTD_seqSymbol* const mlDInfo = seqState->stateML.table + seqState->stateML.state;
    const ZSTD_seqSymbol* const ofDInfo = seqState->stateOffb.table + seqState->stateOffb.state;
#endif
    seq.matchLength = mlDInfo->baseValue;
    seq.litLength = llDInfo->baseValue;
    {   U32 const ofBase = ofDInfo->baseValue;
        BYTE const llBits = llDInfo->nbAdditionalBits;
        BYTE const mlBits = mlDInfo->nbAdditionalBits;
        BYTE const ofBits = ofDInfo->nbAdditionalBits;
        BYTE const totalBits = llBits+mlBits+ofBits;

        U16 const llNext = llDInfo->nextState;
        U16 const mlNext = mlDInfo->nextState;
        U16 const ofNext = ofDInfo->nextState;
        U32 const llnbBits = llDInfo->nbBits;
        U32 const mlnbBits = mlDInfo->nbBits;
        U32 const ofnbBits = ofDInfo->nbBits;

        assert(llBits <= MaxLLBits);
        assert(mlBits <= MaxMLBits);
        assert(ofBits <= MaxOff);
        /*
         * As gcc has better branch and block analyzers, sometimes it is only
         * valuable to mark likeliness for clang, it gives around 3-4% of
         * performance.
         */

        /* sequence */
        {   size_t offset;
            if (ofBits > 1) {
                ZSTD_STATIC_ASSERT(ZSTD_lo_isLongOffset == 1);
                ZSTD_STATIC_ASSERT(LONG_OFFSETS_MAX_EXTRA_BITS_32 == 5);
                ZSTD_STATIC_ASSERT(STREAM_ACCUMULATOR_MIN_32 > LONG_OFFSETS_MAX_EXTRA_BITS_32);
                ZSTD_STATIC_ASSERT(STREAM_ACCUMULATOR_MIN_32 - LONG_OFFSETS_MAX_EXTRA_BITS_32 >= MaxMLBits);
                if (MEM_32bits() && longOffsets && (ofBits >= STREAM_ACCUMULATOR_MIN_32)) {
                    /* Always read extra bits, this keeps the logic simple,
                     * avoids branches, and avoids accidentally reading 0 bits.
                     */
                    U32 const extraBits = LONG_OFFSETS_MAX_EXTRA_BITS_32;
                    offset = ofBase + (BIT_readBitsFast(&seqState->DStream, ofBits - extraBits) << extraBits);
                    BIT_reloadDStream(&seqState->DStream);
                    offset += BIT_readBitsFast(&seqState->DStream, extraBits);
                } else {
                    offset = ofBase + BIT_readBitsFast(&seqState->DStream, ofBits/*>0*/);   /* <=  (ZSTD_WINDOWLOG_MAX-1) bits */
                    if (MEM_32bits()) BIT_reloadDStream(&seqState->DStream);
                }
                seqState->prevOffset[2] = seqState->prevOffset[1];
                seqState->prevOffset[1] = seqState->prevOffset[0];
                seqState->prevOffset[0] = offset;
            } else {
                U32 const ll0 = (llDInfo->baseValue == 0);
                if (LIKELY((ofBits == 0))) {
                    offset = seqState->prevOffset[ll0];
                    seqState->prevOffset[1] = seqState->prevOffset[!ll0];
                    seqState->prevOffset[0] = offset;
                } else {
                    offset = ofBase + ll0 + BIT_readBitsFast(&seqState->DStream, 1);
                    {   size_t temp = (offset==3) ? seqState->prevOffset[0] - 1 : seqState->prevOffset[offset];
                        temp -= !temp; /* 0 is not valid: input corrupted => force offset to -1 => corruption detected at execSequence */
                        if (offset != 1) seqState->prevOffset[2] = seqState->prevOffset[1];
                        seqState->prevOffset[1] = seqState->prevOffset[0];
                        seqState->prevOffset[0] = offset = temp;
            }   }   }
            seq.offset = offset;
        }

        if (mlBits > 0)
            seq.matchLength += BIT_readBitsFast(&seqState->DStream, mlBits/*>0*/);

        if (MEM_32bits() && (mlBits+llBits >= STREAM_ACCUMULATOR_MIN_32-LONG_OFFSETS_MAX_EXTRA_BITS_32))
            BIT_reloadDStream(&seqState->DStream);
        if (MEM_64bits() && UNLIKELY(totalBits >= STREAM_ACCUMULATOR_MIN_64-(LLFSELog+MLFSELog+OffFSELog)))
            BIT_reloadDStream(&seqState->DStream);
        /* Ensure there are enough bits to read the rest of data in 64-bit mode. */
        ZSTD_STATIC_ASSERT(16+LLFSELog+MLFSELog+OffFSELog < STREAM_ACCUMULATOR_MIN_64);

        if (llBits > 0)
            seq.litLength += BIT_readBitsFast(&seqState->DStream, llBits/*>0*/);

        if (MEM_32bits())
            BIT_reloadDStream(&seqState->DStream);

        DEBUGLOG(6, "seq: litL=%u, matchL=%u, offset=%u",
                    (U32)seq.litLength, (U32)seq.matchLength, (U32)seq.offset);

        if (!isLastSeq) {
            /* don't update FSE state for last Sequence */
            ZSTD_updateFseStateWithDInfo(&seqState->stateLL, &seqState->DStream, llNext, llnbBits);    /* <=  9 bits */
            ZSTD_updateFseStateWithDInfo(&seqState->stateML, &seqState->DStream, mlNext, mlnbBits);    /* <=  9 bits */
            if (MEM_32bits()) BIT_reloadDStream(&seqState->DStream);    /* <= 18 bits */
            ZSTD_updateFseStateWithDInfo(&seqState->stateOffb, &seqState->DStream, ofNext, ofnbBits);  /* <=  8 bits */
            BIT_reloadDStream(&seqState->DStream);
        }
    }

    return seq;
}

extern size_t(*ZSTD_decompressSequences_body)(ZSTD_DCtx* dctx,
    void* dst, size_t maxDstSize,
    const void* seqStart, size_t seqSize, int nbSeq,
    const ZSTD_longOffset_e isLongOffset);
static size_t(**decompressSequences_body)(ZSTD_DCtx* dctx,
    void* dst, size_t maxDstSize,
    const void* seqStart, size_t seqSize, int nbSeq,
    const ZSTD_longOffset_e isLongOffset) = &ZSTD_decompressSequences_body;

#if defined(__INTEL_LLVM_COMPILER) && (__INTEL_LLVM_COMPILER / 10000 % 1000 == 24)
#define ZSTD_decompressSequences_body ZSTD_decompressSequences_body_icx24
#define ZSTD_enable ZSTD_enable_icx24
#elif defined(__INTEL_LLVM_COMPILER) && (__INTEL_LLVM_COMPILER / 10000 % 1000 == 25)
#define ZSTD_decompressSequences_body ZSTD_decompressSequences_body_icx25
#define ZSTD_enable ZSTD_enable_icx25
#elif defined(__clang__)
#define ZSTD_decompressSequences_body ZSTD_decompressSequences_body_clang
#define ZSTD_enable ZSTD_enable_clang
#elif defined(_MSC_VER)
#define ZSTD_decompressSequences_body ZSTD_decompressSequences_body_cl
#define ZSTD_enable ZSTD_enable_cl
#endif

size_t DONT_VECTORIZE
ZSTD_decompressSequences_body(ZSTD_DCtx* dctx,
    void* dst, size_t maxDstSize,
    const void* seqStart, size_t seqSize, int nbSeq,
    const ZSTD_longOffset_e isLongOffset)
{
    const BYTE* ip = (const BYTE*)seqStart;
    const BYTE* const iend = ip + seqSize;
    BYTE* const ostart = (BYTE*)dst;
    BYTE* const oend = dctx->litBufferLocation == ZSTD_not_in_dst ? ZSTD_maybeNullPtrAdd(ostart, maxDstSize) : dctx->litBuffer;
    BYTE* op = ostart;
    const BYTE* litPtr = dctx->litPtr;
    const BYTE* const litEnd = litPtr + dctx->litSize;
    const BYTE* const prefixStart = (const BYTE*)(dctx->prefixStart);
    const BYTE* const vBase = (const BYTE*)(dctx->virtualStart);
    const BYTE* const dictEnd = (const BYTE*)(dctx->dictEnd);
    DEBUGLOG(5, "ZSTD_decompressSequences_body: nbSeq = %d", nbSeq);

    /* Regen sequences */
    if (nbSeq) {
        seqState_t seqState;
        dctx->fseEntropy = 1;
        { U32 i; for (i = 0; i < ZSTD_REP_NUM; i++) seqState.prevOffset[i] = dctx->entropy.rep[i]; }
        RETURN_ERROR_IF(
            ERR_isError(BIT_initDStream(&seqState.DStream, ip, iend - ip)),
            corruption_detected, "");
        ZSTD_initFseState(&seqState.stateLL, &seqState.DStream, dctx->LLTptr);
        ZSTD_initFseState(&seqState.stateOffb, &seqState.DStream, dctx->OFTptr);
        ZSTD_initFseState(&seqState.stateML, &seqState.DStream, dctx->MLTptr);
        assert(dst != NULL);

#if defined(__GNUC__) && defined(__x86_64__)
            __asm__(".p2align 6");
            __asm__("nop");
#  if __GNUC__ >= 7
            __asm__(".p2align 5");
            __asm__("nop");
            __asm__(".p2align 3");
#  else
            __asm__(".p2align 4");
            __asm__("nop");
            __asm__(".p2align 3");
#  endif
#endif

        for ( ; nbSeq ; nbSeq--) {
            seq_t const sequence = ZSTD_decodeSequence(&seqState, isLongOffset, nbSeq==1);
            size_t const oneSeqSize = ZSTD_execSequence(op, oend, sequence, &litPtr, litEnd, prefixStart, vBase, dictEnd);
#if defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION) && defined(FUZZING_ASSERT_VALID_SEQUENCE)
            assert(!ZSTD_isError(oneSeqSize));
            ZSTD_assertValidSequence(dctx, op, oend, sequence, prefixStart, vBase);
#endif
            if (UNLIKELY(ZSTD_isError(oneSeqSize)))
                return oneSeqSize;
            DEBUGLOG(6, "regenerated sequence size : %u", (U32)oneSeqSize);
            op += oneSeqSize;
        }

        /* check if reached exact end */
        assert(nbSeq == 0);
        RETURN_ERROR_IF(!BIT_endOfDStream(&seqState.DStream), corruption_detected, "");
        /* save reps for next block */
        { U32 i; for (i=0; i<ZSTD_REP_NUM; i++) dctx->entropy.rep[i] = (U32)(seqState.prevOffset[i]); }
    }

    /* last literal segment */
    {   size_t const lastLLSize = (size_t)(litEnd - litPtr);
        DEBUGLOG(6, "copy last literals : %u", (U32)lastLLSize);
        RETURN_ERROR_IF(lastLLSize > (size_t)(oend-op), dstSize_tooSmall, "");
        if (op != NULL) {
            ZSTD_memcpy(op, litPtr, lastLLSize);
            op += lastLLSize;
    }   }

    DEBUGLOG(6, "decoded block of size %u bytes", (U32)(op - ostart));
    return (size_t)(op - ostart);
}

void ZSTD_enable()
{
    *decompressSequences_body = &ZSTD_decompressSequences_body;
}
