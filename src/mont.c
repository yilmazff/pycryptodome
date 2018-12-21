/* ===================================================================
 *
 * Copyright (c) 2018, Helder Eijs <helderijs@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * ===================================================================
 */

#include <assert.h>

#include "common.h"
#include "endianess.h"
#include "multiply.h"
#include "mont.h"

static inline unsigned is_odd(uint64_t x)
{
    return 1 == (x & 1);
}

static inline unsigned is_even(uint64_t x)
{
    return !is_odd(x);
}

/**
 * Compute the inverse modulo 2^64 of a 64-bit odd integer.
 *
 * See https://crypto.stackexchange.com/questions/47493/how-to-determine-the-multiplicative-inverse-modulo-64-or-other-power-of-two
 */
STATIC uint64_t inverse64(uint64_t a)
{
    uint64_t x;

    assert(1 & a);
    x = ((a << 1 ^ a) & 4) << 1 ^ a;
    x += x - a*x*x;
    x += x - a*x*x;
    x += x - a*x*x;
    x += x - a*x*x;
    assert((x*a & 0xFFFFFFFFFFFFFFFFULL) == 1);
    
    return x;
}

/**
 * Check if big integer x is greater or equal to y.
 *
 * @param x     The first term
 * @param y     The second term
 * @param nw    The number of words that make up both x and y
 * @return      1 if x>=y, 0 if x<y
 */
STATIC int ge(const uint64_t *x, const uint64_t *y, size_t nw)
{
    unsigned mask = -1;
    unsigned result = 0;
    size_t i, j;

    i = nw - 1;
    for (j=0; j<nw; j++, i--) {
        unsigned greater, lower;

        greater = x[i] > y[i];
        lower = x[i] < y[i];
        result |= mask & (greater | (lower << 1));
        mask &= (greater ^ lower) - 1;
    }

    return result<2;
}

/*
 * Subtract integer b from a, leaving the difference in a.
 *
 * @param out   Where to store the result
 * @param a     Number to subtract from
 * @param b     Number to subtract
 * @param nw    The number of words that make up both a and b
 * @result      0 if there is no borrow, 1 otherwise
 */
STATIC uint64_t sub(uint64_t *out, const uint64_t *a, const uint64_t *b, size_t nw)
{
    size_t i;
    uint64_t borrow1 , borrow2;

    borrow2 = 0;
    for (i=0; i<nw; i++) {
        borrow1 = b[i] > a[i];
        out[i] = a[i] - b[i];

        borrow1 |= borrow2 > out[i];
        out[i] -= borrow2;

        borrow2 = borrow1;
    }

    return borrow2;
}

/*
 * Compute R^2 mod N, where R is the smallest power of 2^64
 * which is larger than N.
 *
 * @param r2_mod_n  Where the result is stored to
 * @param n         The modulus N
 * @param nw        The number of 64-bit words that make up r2_mod_n and n
 */
STATIC void rsquare(uint64_t *r2_mod_n, uint64_t *n, size_t nw)
{
    size_t i;
    size_t R_bits;

    memset(r2_mod_n, 0, sizeof(uint64_t)*nw);

    /**
     * Start with R2=1, double 2*bitlen(R) times,
     * and reduce it as soon as it exceeds n
     */
    r2_mod_n[0] = 1;
    R_bits = nw * sizeof(uint64_t) * 8;
    for (i=0; i<R_bits*2; i++) {
        unsigned overflow;
        size_t j;
        
        /** Double, by shifting left by one bit **/
        overflow = (unsigned)(r2_mod_n[nw-1] >> 63);
        for (j=nw-1; j>0; j--) {
            r2_mod_n[j] = (r2_mod_n[j] << 1) + (r2_mod_n[j-1] >> 63);
        }
        /** Fill-in with zeroes **/
        r2_mod_n[0] <<= 1;
        
        /** Subtract n if the result exceeds it **/
        while (overflow || ge(r2_mod_n, n, nw)) {
            sub(r2_mod_n, r2_mod_n, n, nw);
            overflow = 0;
        }
    }
}

/*
 * Multiply a big integer a by a 64-bit scalar k and add the result to big
 * integer t.
 *
 * @param t     The big integer the result of the
 *              multiplication will be added to
 * @param tw    The number of words that make up t
 * @param a     The big integer to multiply with the scalar
 * @param aw    The number of words that make up a
 * @param k     The 64-bit scalar multiplier
 */
STATIC void addmul(uint64_t *t, size_t tw, const uint64_t *a, size_t aw, uint64_t k)
{
    size_t i;
    uint64_t carry;

    carry = 0;
    for (i=0; i<aw; i++) {
        uint64_t prod_lo, prod_hi;

        DP_MULT(a[i], k, prod_lo, prod_hi);
    
        prod_lo += carry;
        prod_hi += prod_lo < carry;

        t[i] += prod_lo;
        prod_hi += t[i] < prod_lo;

        carry = prod_hi;
    }

    for (; carry; i++) {
        t[i] += carry;
        carry = t[i] < carry;
    }

    assert(i <= tw);
}

/**
 * Multiply two big integers.
 *
 * @param t Where to store the result. Array of  2*nw words.
 * @param a The first term, array of nw words.
 * @param a The first term, array of nw words.
 *
 */
STATIC void product(uint64_t *t, const uint64_t *a, const uint64_t *b, size_t nw)
{
        size_t i;

        memset(t, 0, 2*sizeof(uint64_t)*nw);
        
        for (i=0; i<(nw ^ (nw & 1)); i+=2) {
            addmul128(&t[i], a, b[i], b[i+1], nw);
        }

        if (is_odd(nw)) {
            addmul(&t[nw-1], nw+2, a, nw, b[nw-1]);
        }
}

/*
 * Montgomery modular multiplication, that is a*b*R mod N.
 *
 * @param out   The location where the result is stored
 * @param a     The first term (already in Montgomery form, a*R mod N)
 * @param b     The second term (already in Montgomery form, b*R mod N)
 * @param b     The modulus (in normal form), such that R>N
 * @param m0    Least-significant word of the oppossite of the inverse of n modulo R, that is, inv(-n[0], R)
 * @param t     Temporary scratchpad with 3*nw+1 words
 * @param nw    Number of words making up the 3 integers: out, a, and b.
 *              It also defines R as 2^(64*nw).
 *
 * Useful read: https://alicebob.cryptoland.net/understanding-the-montgomery-reduction-algorithm/
 */
STATIC void mont_mult_internal(uint64_t *out, const uint64_t *a, const uint64_t *b, const uint64_t *n, uint64_t m0, uint64_t *t, size_t nw)
{
    size_t i;
    uint64_t *t2, mask;

    t2 = &t[2*nw+1];    /** Point to last nw words **/

    if (a == b) {
        square_w(t, a, nw);
    } else {
        product(t, a, b, nw);
    }

    t[2*nw] = 0; /** MSW **/

    /** Clear lower words (two at a time) **/
    for (i=0; i<(nw ^ (nw & 1)); i+=2) {
        uint64_t k0, k1, ti1, prod_lo, prod_hi;

        /** Multiplier for n that will make t[i+0] go 0 **/
        k0 = t[i] * m0;
        
        /** Simulate Muladd for digit 0 **/
        DP_MULT(k0, n[0], prod_lo, prod_hi);
        prod_lo += t[i];
        prod_hi += prod_lo < t[i];

        /** Expected digit 1 **/
        ti1 = t[i+1] + n[1]*k0 + prod_hi;
        
        /** Multiplier for n that will make t[i+1] go 0 **/
        k1 = ti1 * m0;
        
        addmul128(&t[i], n, k0, k1, nw);
    }

    /** One left for odd number of words **/
    if (is_odd(nw)) {
        addmul(&t[nw-1], nw+2, n, nw, t[nw-1]*m0);
    }
    
    assert(t[2*nw] <= 1); /** MSW **/

    /** t[0..nw-1] == 0 **/
    
    /** Divide by R and possibly subtract n **/
    sub(t2, &t[nw], n, nw);
    mask = (t[2*nw] == 1 || ge(&t[nw], n, nw)) - 1;
    for (i=0; i<nw; i++) {
        out[i] = (t[nw+i] & mask) ^ (t2[i] & ~mask);
    }
}

/* ---- PUBLIC FUNCTIONS ---- */

void mont_context_free(MontContext *ctx)
{
    if (NULL == ctx)
        return;
    free(ctx->one);
    free(ctx->r2_mod_n);
    free(ctx->r_mod_n);
    free(ctx->modulus);
    free(ctx->modulus_min_2);
    free(ctx);
}

size_t mont_bytes(MontContext *ctx)
{
    if (NULL == ctx)
        return 0;
    return ctx->bytes;
}

/*
 * Create a new context for Montgomery form in the given odd modulus.
 *
 * @param out       The memory area where the pointer to the new Montgomery context
 *                  structure is writter into.
 * @param modulus   The modulus encoded in big endian form.
 * @param mod_len   The length of the modulus in bytes.
 * @return          0 for success, the appropriate code otherwise.
 */
int mont_context_init(MontContext **out, const uint8_t *modulus, size_t mod_len)
{
    MontContext *ctx;
    uint64_t *tmp2 = NULL;
    int res;

    if (NULL == out || NULL == modulus)
        return ERR_NULL;

    if (0 == mod_len)
        return ERR_NOT_ENOUGH_DATA;

    /** Ensure modulus is odd and at least 3, otherwise we can't compute its inverse over B **/
    if (is_even(modulus[mod_len-1]))
        return ERR_VALUE;
    if (modulus[0] < 3) {
        size_t i;
        for (i=1; i<mod_len && modulus[i]; i++);
        if (i == mod_len)
            return ERR_VALUE; 
    }

    *out = ctx = (MontContext*)calloc(1, sizeof(MontContext));
    if (NULL == ctx)
        return ERR_MEMORY;

    ctx->words = (mod_len + 7) / 8;
    ctx->bytes = ctx->words * sizeof(uint64_t);

    /** Load modulus N **/
    ctx->modulus = (uint64_t*)calloc(ctx->words, sizeof(uint64_t));
    if (0 == ctx->modulus) {
        res = ERR_MEMORY;
        goto cleanup;
    }
    bytes_to_words(ctx->modulus, ctx->words, modulus, mod_len);
   
    /** Pre-compute R^2 mod N **/
    ctx->r2_mod_n = (uint64_t*)calloc(ctx->words, sizeof(uint64_t));
    if (0 == ctx->r2_mod_n) {
        res = ERR_MEMORY;
        goto cleanup;
    }
    rsquare(ctx->r2_mod_n, ctx->modulus, ctx->words);
    
    /** Pre-compute -n[0]^{-1} mod R **/
    ctx->m0 = inverse64(~ctx->modulus[0]+1);

    /** Prepare 1 **/
    ctx->one = (uint64_t*)calloc(ctx->words, sizeof(uint64_t));
    ctx->one[0] = 1;
    
    /** Pre-compute R mod N **/
    ctx->r_mod_n = (uint64_t*)calloc(ctx->words, sizeof(uint64_t));
    if (NULL == ctx->r_mod_n) {
        res = ERR_MEMORY;
        goto cleanup;
    }
    tmp2 = (uint64_t*)calloc(ctx->words*3+1, sizeof(uint64_t));
    if (NULL == tmp2) {
        res = ERR_MEMORY;
        goto cleanup;
    }
    mont_mult_internal(ctx->r_mod_n, ctx->one, ctx->r2_mod_n, ctx->modulus, ctx->m0, tmp2, ctx->words);

    /** Pre-compute modulus - 2 **/
    /** Modulus is guaranteed to be >= 3 **/
    ctx->modulus_min_2 = (uint64_t*)calloc(ctx->words, sizeof(uint64_t));
    if (NULL == ctx->modulus_min_2) {
        res = ERR_MEMORY;
        goto cleanup;
    }
    sub(ctx->modulus_min_2, ctx->modulus, ctx->one, ctx->words);
    sub(ctx->modulus_min_2, ctx->modulus_min_2, ctx->one, ctx->words);

    res = 0;

cleanup:
    free(tmp2);
    if (res != 0)
        mont_context_free(ctx);
    return res;
}

/*
 * Transform a big endian-encoded number into Montgomery form, by performing memory allocation.
 *
 * @param out       The memory area where the pointer to the newly allocated number in Montgomery form will be put.
 *                  The caller is responsible for deallocating this memory.
 * @param ctx       Montgomery context, as created by mont_context_init()
 * @param number    The big endian-encoded number to transform, strictly smaller than the modulus
 * @param len       The length of the big-endian number in bytes
 * @return          0 in case of success, the relevant error code otherwise
 */
int mont_from_bytes(uint64_t **out, const MontContext *ctx, const uint8_t *number, size_t len)
{
    uint64_t *encoded = NULL;
    uint64_t *tmp1 = NULL;
    uint64_t *tmp2 = NULL;
    int res = 0;

    if (NULL == out || NULL == ctx || NULL == number)
        return ERR_NULL;

    if (0 == len)
        return ERR_NOT_ENOUGH_DATA;

    if (ctx->bytes < len)
        return ERR_VALUE;

    /** The caller will deallocate this memory **/
    *out = encoded = (uint64_t*)calloc(ctx->words, sizeof(uint64_t));
    if (NULL == encoded)
        return ERR_MEMORY;

    /** Input number, loaded in words **/
    tmp1 = (uint64_t*)calloc(ctx->words, sizeof(uint64_t));
    if (NULL == tmp1) {
        res = ERR_MEMORY;
        goto cleanup;
    }
    bytes_to_words(tmp1, ctx->words, number, len);

    /** Make sure number<modulus **/
    if (ge(tmp1, ctx->modulus, ctx->words)) {
        res = ERR_VALUE;
        goto cleanup;
    }

    /** Scratchpad **/
    tmp2 = (uint64_t*)calloc(ctx->words*3+1, sizeof(uint64_t));
    if (NULL == tmp2) {
        res = ERR_MEMORY;
        goto cleanup;
    }

    mont_mult_internal(encoded, tmp1, ctx->r2_mod_n, ctx->modulus, ctx->m0, tmp2, ctx->words);
    res = 0;

cleanup:
    free(tmp2);
    free(tmp1);
    if (res != 0)
        free(encoded);
    return res;
}

/*
 * Transform a number from Montgomery representation to big endian-encoding.
 *
 * @param number        The memory area where the pointer to the big endian-encoded number is stored.
 *                      It must be contain mont_bytes(ctx) bytes.
 *                      The number will be padded with zeroes on the left.
 * @param ctx           The address of the Montgomery context.
 * @param mont_number   The array of words that make up the number in Montgomery form.
 *
 */
int mont_to_bytes(uint8_t *number, const MontContext *ctx, const uint64_t* mont_number)
{
    uint64_t *tmp1 = NULL;
    uint64_t *tmp2 = NULL;

    if (NULL == number || NULL == ctx || NULL == mont_number)
        return ERR_NULL;

   /** number in normal form, but still in words **/
    tmp1 = (uint64_t*)calloc(ctx->words, sizeof(uint64_t));
    if (NULL == tmp1)
        return ERR_MEMORY;

    /** scratchpad **/
    tmp2 = (uint64_t*)calloc(ctx->words*3+1, sizeof(uint64_t));
    if (NULL == tmp2) {
        free(tmp1);
        return ERR_MEMORY;
    }

    mont_mult_internal(tmp1, mont_number, ctx->one, ctx->modulus, ctx->m0, tmp2, ctx->words);
    words_to_bytes(number, ctx->bytes, tmp1, ctx->words);

    free(tmp2);
    free(tmp1);
    return 0;
}

/*
 * Add two numbers in Montgomery representation.
 *
 * @param out   Where to store the result; it must have space for mont_bytes(ctx) bytes
 * @param a     First term
 * @param b     Second term
 * @param ctx   Montgomery context
 * @return      0 for success, the relevant error code otherwise
 */
int mont_add(uint64_t* out, const uint64_t* a, const uint64_t* b, const MontContext *ctx)
{
    size_t i;
    unsigned carry;

    if (NULL == out || NULL == a || NULL == b || NULL == ctx)
        return ERR_NULL;

    for (i=0, carry=0; i<ctx->words; i++) {
        out[i] = a[i] + carry;
        carry = out[i] < carry;
        out[i] += b[i];
        carry += out[i] < b[i];
    }

    return 0;
}

/*
 * Multiply two numbers in Montgomery representation.
 *
 * @param out   Where to store the result; it must have space for mont_bytes(ctx) bytes
 * @param a     First term
 * @param k     Second term
 * @param ctx   Montgomery context
 * @return      0 for success, the relevant error code otherwise
 */
int mont_mult(uint64_t* out, const uint64_t* a, const uint64_t *b, const MontContext *ctx)
{
    uint64_t *t = NULL;

    if (NULL == out || NULL == a || NULL == b || NULL == ctx)
        return ERR_NULL;

    t = (uint64_t*)calloc(3*ctx->words+1, sizeof(uint64_t));
    if (NULL == t)
        return ERR_MEMORY;

    mont_mult_internal(out, a, b, ctx->modulus, ctx->m0, t, ctx->words);

    free(t);
    return 0;
}

/*
 * Multiply a number in Montgomery representation with a scalar.
 *
 * @param out   Where to store the result; it must have space for mont_bytes(ctx) bytes
 * @param a     First term
 * @param k     Second term (scalar)
 * @param ctx   Montgomery context
 * @return      0 for success, the relevant error code otherwise
 */
int mont_mult_scalar(uint64_t* out, const uint64_t* a, uint64_t k, const MontContext *ctx)
{
    size_t i;
    unsigned carry;

    if (NULL == out || NULL == a || NULL == ctx)
        return ERR_NULL;

    for (i=0, carry=0; i<ctx->words; i++) {
        uint64_t prod_lo, prod_hi;

        DP_MULT(a[i], k, prod_lo, prod_hi);
        prod_lo += carry;
        prod_hi += prod_lo < carry;
        out[i] = prod_lo;
        carry = prod_hi;
    }

    return 0;
}

/*
 * Subtract integer b from a.
 *
 * @param out   Where to store the result; it must have space for mont_bytes(ctx) bytes
 * @param a     Number to subtract from
 * @param b     Number to subtract
 * @param ctx   Montgomery context
 * @return      0 for success, the relevant error code otherwise
 */
int mont_sub(uint64_t *out, uint64_t *a, const uint64_t *b, MontContext *ctx)
{
    size_t i;
    uint64_t borrow1 , borrow2;

    if (NULL == out || NULL == a || NULL == b || NULL == ctx)
        return ERR_NULL;

    borrow2 = 0;
    for (i=0; i<ctx->words; i++) {
        borrow1 = b[i] > a[i];
        out[i] = a[i] - b[i];

        borrow1 |= borrow2 > out[i];
        out[i] -= borrow2;

        borrow2 = borrow1;
    }

    return 0;
}

/*
 * Compute the modular inverse of an integer in Montgomery form.
 *
 * Condition: the modulus defining the Montgomery context MUST BE a non-secret prime number.
 *
 * @param out   Where the inverse will be stored (in Montgomery form); it must have
 *              space for mont_bytes(ctx) bytes.
 * @param a     The number to compute the modular inverse of, already in Montgomery form
 * @param ctx   Montgomery context
 * @return      0 for success, the relevant error code otherwise
 */
int mont_inv_prime(uint64_t *out, uint64_t *a, MontContext *ctx)
{
    unsigned idx_word;
    uint64_t bit;
    uint64_t *tmp1 = NULL;
    uint64_t *tmp2 = NULL;
    uint64_t *exponent = NULL;
    int res;

    if (NULL == out || NULL == a || NULL == ctx)
        return ERR_NULL;

    tmp1 = (uint64_t*)calloc(ctx->words, sizeof(uint64_t));
    if (NULL == tmp1)
        return ERR_MEMORY;
    
    tmp2 = (uint64_t*)calloc(3*ctx->words+1, sizeof(uint64_t));
    if (NULL == tmp2) {
        res = ERR_MEMORY;
        goto cleanup;
    }
   
    /** Exponent is guaranteed to be >0 **/ 
    exponent = ctx->modulus_min_2;

    /* Find most significant bit */
    idx_word = ctx->words-1;
    for (;;) {
        if (exponent[idx_word] != 0)
            break;
        if (idx_word == 0)
            break;
        idx_word--;
    }
    for (bit = (uint64_t)1 << 63; 0 == (exponent[idx_word] & bit); bit--);

    /* Start from 1 (in Montgomery form, which is R mod N) */
    memcpy(out, ctx->r_mod_n, ctx->bytes);

    /** Left-to-right exponentiation **/
    for (;;) {
        for (;;) {
            mont_mult_internal(tmp1, out, out, ctx->modulus, ctx->m0, tmp2, ctx->words);
            if (exponent[idx_word] & bit) {
                mont_mult_internal(out, tmp1, a, ctx->modulus, ctx->m0, tmp2, ctx->words);
            } else {
                memcpy(out, tmp1, ctx->bytes);
            }
            if (bit == 1)
                break;
            bit >>= 1;
        }
        if (idx_word == 0)
            break;
        idx_word--;
        bit = 1UL << 63;
    }
    res = 0;

cleanup:
    free(tmp1);
    free(tmp2);
    return res;
}