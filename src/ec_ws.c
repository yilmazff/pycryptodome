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
#include "ec.h"
#include "modexp_utils.h"

FAKE_INIT(ec_ws)

#ifdef MAIN
STATIC void print_x(const char *s, const uint64_t *number, const MontContext *ctx)
{
    unsigned i;
    size_t size;
    uint8_t *encoded;
    int res;

    size = mont_bytes(ctx);
    encoded = calloc(1, size);
    res = mont_to_bytes(encoded, number, ctx);
    assert(res == 0);

    printf("%s: ", s);
    for (i=0; i<size; i++)
        printf("%02X", encoded[i]);
    printf("\n");

    free(encoded);
}
#endif

STATIC Workplace *new_workplace(const MontContext *ctx)
{
    Workplace *wp;
    int res;

    wp = calloc(1, sizeof(Workplace));
    if (NULL == wp)
        return NULL;

    res = mont_number(&wp->a, 1, ctx);
    if (res) goto cleanup;
    res = mont_number(&wp->b, 1, ctx);
    if (res) goto cleanup;
    res = mont_number(&wp->c, 1, ctx);
    if (res) goto cleanup;
    res = mont_number(&wp->d, 1, ctx);
    if (res) goto cleanup;
    res = mont_number(&wp->e, 1, ctx);
    if (res) goto cleanup;
    res = mont_number(&wp->f, 1, ctx);
    if (res) goto cleanup;
    res = mont_number(&wp->g, 1, ctx);
    if (res) goto cleanup;
    res = mont_number(&wp->h, 1, ctx);
    if (res) goto cleanup;
    res = mont_number(&wp->scratch, SCRATCHPAD_NR, ctx);
    if (res) goto cleanup;
    return wp;

cleanup:
    free(wp->a);
    free(wp->b);
    free(wp->c);
    free(wp->d);
    free(wp->e);
    free(wp->f);
    free(wp->g);
    free(wp->h);
    free(wp->scratch);
    return NULL;
}

STATIC void free_workplace(Workplace *wp)
{
    if (NULL == wp)
        return;
    free(wp->a);
    free(wp->b);
    free(wp->c);
    free(wp->d);
    free(wp->e);
    free(wp->f);
    free(wp->g);
    free(wp->h);
    free(wp->scratch);
    free(wp);
}

/*
 * Convert jacobian coordinates to affine.
 */
STATIC void ec_ws_normalize(uint64_t *x3, uint64_t *y3,
                         const uint64_t *x1, uint64_t *y1, uint64_t *z1,
                         Workplace *tmp,
                         const MontContext *ctx)
{
    uint64_t *a = tmp->a;
    uint64_t *b = tmp->b;
    uint64_t *c = tmp->c;
    uint64_t *s = tmp->scratch;

    if (mont_is_zero(z1, ctx)) {
        mont_set(x3, 0, NULL, ctx);
        mont_set(y3, 0, NULL, ctx);
        return;
    }

    mont_inv_prime(a, z1, ctx);
    mont_mult(b, a, a, s, ctx);
    mont_mult(c, b, a, s, ctx);
    mont_mult(x3, x1, b, s, ctx);     /* X/Z² */
    mont_mult(y3, y1, c, s, ctx);     /* Y/Z³ */
}

/*
 * Double an EC point on a short Weierstrass curve of equation y²=x³-3x+b.
 * Jacobian coordinates.
 * Input and output points can match.
 */
STATIC void ec_full_double(uint64_t *x3, uint64_t *y3, uint64_t *z3,
                           const uint64_t *x1, const uint64_t *y1, const uint64_t *z1,
                           Workplace *tmp, const MontContext *ctx)
{
    uint64_t *a = tmp->a;
    uint64_t *b = tmp->b;
    uint64_t *c = tmp->c;
    uint64_t *d = tmp->d;
    uint64_t *e = tmp->e;
    uint64_t *s = tmp->scratch;

    if (mont_is_zero(z1, ctx)) {
        mont_set(x3, 1, NULL, ctx);
        mont_set(y3, 1, NULL, ctx);
        mont_set(z3, 0, NULL, ctx);
        return;
    }

    /* No need to explicitly handle the case y1=0 (for x1≠0).
     * The following code will already produce the point at infinity (t²,t³,0).
     */

    mont_mult(a, z1, z1, s, ctx);       /* a = delta = Z1² */
    mont_mult(b, y1, y1, s, ctx);       /* b = gamma = Y1² */
    mont_mult(c, x1, b, s, ctx);        /* c = beta = X1*gamma */
    mont_sub(d, x1, a, s, ctx);
    mont_add(e, x1, a, s, ctx);
    mont_mult(d, d, e, s, ctx);
    mont_add(e, d, d, s, ctx);
    mont_add(d, d, e, s, ctx);          /* d = alpha = 3*(X1-delta)*(X1+delta) */

    mont_add(z3, y1, z1, s, ctx);
    mont_mult(z3, z3, z3, s, ctx);
    mont_sub(z3, z3, b, s, ctx);
    mont_sub(z3, z3, a, s, ctx);        /* Z3 = (Y1+Z1)²-gamma-delta */

    mont_mult(x3, d, d, s, ctx);
    mont_add(e, c, c, s, ctx);
    mont_add(e, e, e, s, ctx);
    mont_add(e, e, e, s, ctx);
    mont_sub(x3, x3, e, s, ctx);        /* X3 = alpha²-8*beta */

    mont_add(e, c, c, s, ctx);
    mont_add(y3, e, e, s, ctx);
    mont_sub(y3, y3, x3, s, ctx);
    mont_mult(y3, d, y3, s, ctx);
    mont_mult(e, b, b, s, ctx);
    mont_add(e, e, e, s, ctx);
    mont_add(e, e, e, s, ctx);
    mont_add(e, e, e, s, ctx);
    mont_sub(y3, y3, e, s, ctx);        /* Y3 = alpha*(4*beta-X3)-8*gamma² */
}

/*
 * Add two EC points on a short Weierstrass curve of equation y²=x³-3x+b.
 * One input point has affine coordinates.
 * The other input and the the output points have Jacobian coordinates.
 * Input and output points can match.
 */
STATIC void ec_mix_add(uint64_t *x3, uint64_t *y3, uint64_t *z3,
                       const uint64_t *x1, const uint64_t *y1, const uint64_t *z1,
                       const uint64_t *x2, const uint64_t *y2,
                       Workplace *tmp,
                       const MontContext *ctx)
{
    uint64_t *a = tmp->a;
    uint64_t *b = tmp->b;
    uint64_t *c = tmp->c;
    uint64_t *d = tmp->d;
    uint64_t *e = tmp->e;
    uint64_t *f = tmp->f;
    uint64_t *s = tmp->scratch;

    /* First term may be point at infinity */
    if (mont_is_zero(z1, ctx)) {
        mont_copy(x3, x2, ctx);
        mont_copy(y3, y2, ctx);
        mont_set(z3, 1, tmp->scratch, ctx);
        return;
    }

    /* Second term may be point at infinity */
    if (mont_is_zero(x2, ctx) && mont_is_zero(y2, ctx)) {
        mont_copy(x3, x1, ctx);
        mont_copy(y3, y1, ctx);
        mont_copy(z3, z1, ctx);
        return;
    }

    mont_mult(a, z1, z1, s, ctx);       /* a = Z1Z1 = Z1² */
    mont_mult(b, x2, a, s, ctx);        /* b = U2 = X2*Z1Z1 */
    mont_mult(c, y2, z1, s, ctx);
    mont_mult(c, c, a, s, ctx);         /* c = S2 = Y2*Z1*Z1Z1 */

    /* Now that affine (x2, y2) is converted to Jacobian (U2, S2, Z1)
     * we can check if P1 is ±P2 and handle such special case */
    if (mont_is_equal(x1, b, ctx)) {
        if (mont_is_equal(y1, c, ctx)) {
            ec_full_double(x3, y3, z3, x1, y1, z1, tmp, ctx);
            return;
        } else {
            mont_set(x3, 1, NULL, ctx);
            mont_set(y3, 1, NULL, ctx);
            mont_set(z3, 0, NULL, ctx);
            return;
        }
    }

    mont_sub(b, b, x1, s, ctx);         /* b = H = U2-X1 */
    mont_mult(d, b, b, s, ctx);         /* d = HH = H² */
    mont_add(e, d, d, s, ctx);
    mont_add(e, e, e, s, ctx);          /* e = I = 4*HH */
    mont_mult(f, b, e, s, ctx);         /* f = J = H*I */

    mont_sub(c, c, y1, s, ctx);
    mont_add(c, c, c, s, ctx);          /* c = r = 2*(S2-Y1) */
    mont_mult(e, x1, e, s, ctx);        /* e = V = X1*I */

    mont_mult(x3, c, c, s, ctx);
    mont_sub(x3, x3, f, s, ctx);
    mont_sub(x3, x3, e, s, ctx);
    mont_sub(x3, x3, e, s, ctx);        /* X3 = r²-J-2*V */

    mont_mult(f, y1, f, s, ctx);
    mont_add(f, f, f, s, ctx);
    mont_sub(y3, e, x3, s, ctx);
    mont_mult(y3, c, y3, s, ctx);
    mont_sub(y3, y3, f, s, ctx);        /* Y3 = r*(V-X3)-2*Y1*J */

    mont_add(z3, z1, b, s, ctx);
    mont_mult(z3, z3, z3, s, ctx);
    mont_sub(z3, z3, a, s, ctx);
    mont_sub(z3, z3, d, s, ctx);        /* Z3 = (Z1+H)²-Z1Z1-HH **/
}

/*
 * Add two EC points on a short Weierstrass curve of equation y²=x³-3x+b.
 * All points have Jacobian coordinates.
 * Input and output points can match.
 */
STATIC void ec_full_add(uint64_t *x3, uint64_t *y3, uint64_t *z3,
                        const uint64_t *x1, const uint64_t *y1, const uint64_t *z1,
                        const uint64_t *x2, const uint64_t *y2, const uint64_t *z2,
                        Workplace *tmp,
                        const MontContext *ctx)
{
    uint64_t *a = tmp->a;
    uint64_t *b = tmp->b;
    uint64_t *c = tmp->c;
    uint64_t *d = tmp->d;
    uint64_t *e = tmp->e;
    uint64_t *f = tmp->f;
    uint64_t *g = tmp->g;
    uint64_t *h = tmp->h;
    uint64_t *s = tmp->scratch;
    unsigned p2_is_pai;

    /* First term may be point at infinity */
    if (mont_is_zero(z1, ctx)) {
        mont_copy(x3, x2, ctx);
        mont_copy(y3, y2, ctx);
        mont_copy(z3, z2, ctx);
        return;
    }

    /* Second term may be point at infinity,
     * if so we still go ahead with all computations
     * and only at the end copy over point 1 as result,
     * to limit timing leakages.
     */
    p2_is_pai = mont_is_zero(z2, ctx);

    mont_mult(a, z1, z1, s, ctx);       /* a = Z1Z1 = Z1² */
    mont_mult(b, z2, z2, s, ctx);       /* b = Z2Z2 = Z2² */
    mont_mult(c, x1, b, s, ctx);        /* c = U1 = X1*Z2Z2 */
    mont_mult(d, x2, a, s, ctx);        /* d = U2 = X2*Z1Z1 */
    mont_mult(e, y1, z2, s, ctx);
    mont_mult(e, e, b, s, ctx);         /* e = S1 = Y1*Z2*Z2Z2 */
    mont_mult(f, y2, z1, s, ctx);
    mont_mult(f, f, a, s, ctx);         /* f = S2 = Y2*Z1*Z1Z1 */

    /* We can check if P1 is ±P2 and handle such special case */
    if (mont_is_equal(c, d, ctx)) {
        if (mont_is_equal(e, f, ctx)) {
            ec_full_double(x3, y3, z3, x1, y1, z1, tmp, ctx);
        } else {
            mont_set(x3, 1, NULL, ctx);
            mont_set(y3, 1, NULL, ctx);
            mont_set(z3, 0, NULL, ctx);
        }
        return;
    }

    mont_sub(d, d, c, s, ctx);          /* d = H = U2-U1 */
    mont_add(g, d, d, s, ctx);
    mont_mult(g, g, g, s, ctx);         /* g = I = (2*H)² */
    mont_mult(h, d, g, s, ctx);         /* h = J = H*I */
    mont_sub(f, f, e, s, ctx);
    mont_add(f, f, f, s, ctx);          /* f = r = 2*(S2-S1) */
    mont_mult(c, c, g, s, ctx);         /* c = V = U1*I */

    mont_mult(g, f, f, s, ctx);
    mont_sub(g, g, h, s, ctx);
    mont_sub(g, g, c, s, ctx);
    mont_sub(g, g, c, s, ctx);          /* x3 = g = r²-J-2*V */

    mont_select(x3, x1, g, p2_is_pai, ctx);

    mont_sub(g, c, g, s, ctx);
    mont_mult(g, f, g, s, ctx);
    mont_mult(c, e, h, s, ctx);
    mont_add(c, c, c, s, ctx);
    mont_sub(g, g, c, s, ctx);        /* y3 = r*(V-X3)-2*S1*J */

    mont_select(y3, y1, g, p2_is_pai, ctx);

    mont_add(g, z1, z2, s, ctx);
    mont_mult(g, g, g, s, ctx);
    mont_sub(g, g, a, s, ctx);
    mont_sub(g, g, b, s, ctx);
    mont_mult(g, g, d, s, ctx);       /* z3 = ((Z1+Z2)²-Z1Z1-Z2Z2)*H */

    mont_select(z3, z1, g, p2_is_pai, ctx);
}

#define WINDOW_SIZE_BITS 4
#define WINDOW_SIZE_ITEMS (1<<WINDOW_SIZE_BITS)

/*
 * Compute the scalar multiplication of an EC point.
 * Jacobian coordinates as output, affine an input.
 */
STATIC int ec_exp(uint64_t *x3, uint64_t *y3, uint64_t *z3,
                   const uint64_t *x1, const uint64_t *y1, const uint64_t *z1,
                   const uint8_t *exp, size_t exp_size, uint64_t seed,
                   Workplace *wp1,
                   Workplace *wp2,
                   const MontContext *ctx)
{
    unsigned z1_is_one;
    int i;
    int res;
    uint64_t *window_x[WINDOW_SIZE_ITEMS],
             *window_y[WINDOW_SIZE_ITEMS],
             *window_z[WINDOW_SIZE_ITEMS];
    uint64_t *xw=NULL, *yw=NULL, *zw=NULL;
    ProtMemory *prot_x=NULL, *prot_y=NULL, *prot_z=NULL;

    struct BitWindow bw;

    z1_is_one = mont_is_one(z1, ctx);
    res = ERR_MEMORY;

    #define alloc(n) n=calloc(ctx->words, 8); if (NULL == n) goto cleanup;

    alloc(xw);
    alloc(yw);
    alloc(zw);

    /** Create window O, P, P² .. P¹⁵ **/
    memset(window_x, 0, sizeof window_x);
    memset(window_y, 0, sizeof window_y);
    memset(window_z, 0, sizeof window_z);

    for (i=0; i<WINDOW_SIZE_ITEMS; i++) {
        alloc(window_x[i]);
        alloc(window_y[i]);
        alloc(window_z[i]);
    }

    #undef alloc

    mont_set(window_x[0], 1, NULL, ctx);
    mont_set(window_y[0], 1, NULL, ctx);
    mont_set(window_z[0], 0, NULL, ctx);

    mont_copy(window_x[1], x1, ctx);
    mont_copy(window_y[1], y1, ctx);
    mont_copy(window_z[1], z1, ctx);

    for (i=2; i<WINDOW_SIZE_ITEMS; i++) {
        if (z1_is_one)
            ec_mix_add(window_x[i],   window_y[i],   window_z[i],
                       window_x[i-1], window_y[i-1], window_z[i-1],
                       x1, y1,
                       wp1, ctx);
        else
            ec_full_add(window_x[i],   window_y[i],   window_z[i],
                        window_x[i-1], window_y[i-1], window_z[i-1],
                        x1, y1, z1,
                        wp1, ctx);
    }

    res = scatter(&prot_x, (void**)window_x, WINDOW_SIZE_ITEMS, mont_bytes(ctx), seed);
    if (res) goto cleanup;
    res = scatter(&prot_y, (void**)window_y, WINDOW_SIZE_ITEMS, mont_bytes(ctx), seed);
    if (res) goto cleanup;
    res = scatter(&prot_z, (void**)window_z, WINDOW_SIZE_ITEMS, mont_bytes(ctx), seed);
    if (res) goto cleanup;

    /** Start from PAI **/
    mont_set(x3, 1, NULL, ctx);
    mont_set(y3, 1, NULL, ctx);
    mont_set(z3, 0, NULL, ctx);

    /** Find first non-zero byte in exponent **/
    for (; exp_size && *exp==0; exp++, exp_size--);
    bw = init_bit_window(WINDOW_SIZE_BITS, exp, exp_size);

    /** For every nibble, double 16 times and add window value **/
    for (i=0; i < bw.nr_windows; i++) {
        unsigned index;
        int j;

        index = get_next_digit(&bw);
        gather(xw, prot_x, index);
        gather(yw, prot_y, index);
        gather(zw, prot_z, index);
        for (j=0; j<WINDOW_SIZE_BITS; j++)
            ec_full_double(x3, y3, z3, x3, y3, z3, wp1, ctx);
        ec_full_add(x3, y3, z3, x3, y3, z3, xw, yw, zw, wp1, ctx);
    }

    res = 0;

cleanup:
    free(xw);
    free(yw);
    free(zw);
    for (i=0; i<WINDOW_SIZE_ITEMS; i++) {
        free(window_x[i]);
        free(window_y[i]);
        free(window_z[i]);
    }
    free_scattered(prot_x);
    free_scattered(prot_y);
    free_scattered(prot_z);

    return res;
}

/*
 * Create an Elliptic Curve context for Weierstress curves y²=x³+ax+b with a=-3
 *
 * @param pec_ctx   The memory area where the pointer to the newly allocated
 *                  EC context will be stored.
 * @param modulus   The prime modulus for the curve, big-endian encoded
 * @param b         The constant b, big-endian encoded
 * @param order     The order of the EC curve
 * @param len       The length in bytes of modulus, b, and order
 * @return          0 for success, the appopriate error code otherwise
 */
EXPORT_SYM int ec_ws_new_context(EcContext **pec_ctx,
                                 const uint8_t *modulus,
                                 const uint8_t *b,
                                 const uint8_t *order,
                                 size_t len)
{
    EcContext *ec_ctx = NULL;
    unsigned order_words;
    int res;

    if (NULL == pec_ctx || NULL == modulus || NULL == b)
        return ERR_NULL;

    *pec_ctx = NULL;

    if (len == 0)
        return ERR_NOT_ENOUGH_DATA;

    *pec_ctx = ec_ctx = (EcContext*)calloc(1, sizeof(EcContext));
    if (NULL == ec_ctx)
        return ERR_MEMORY;

    res = mont_context_init(&ec_ctx->mont_ctx, modulus, len);
    if (res) goto cleanup;

    res = mont_from_bytes(&ec_ctx->b, b, len, ec_ctx->mont_ctx);
    if (res) goto cleanup;

    order_words = (len+7)/8;
    ec_ctx->order = (uint64_t*)calloc(order_words, sizeof(uint64_t));
    if (NULL == ec_ctx->order) goto cleanup;
    bytes_to_words(ec_ctx->order, order_words, order, len);

    return 0;

cleanup:
    free(ec_ctx->b);
    free(ec_ctx->order);
    mont_context_free(ec_ctx->mont_ctx);
    free(ec_ctx);
    return res;
}

EXPORT_SYM void ec_free_context(EcContext *ec_ctx)
{
    if (NULL == ec_ctx)
        return;

    free(ec_ctx->b);
    free(ec_ctx->order);
    mont_context_free(ec_ctx->mont_ctx);
    free(ec_ctx);
}

/*
 * Create a new EC point on the given EC curve.
 *
 *  @param pecp     The memory area where the pointer to the newly allocated EC
 *                  point will be stored. Use ec_free_point() for deallocating it.
 *  @param x        The X-coordinate (affine, big-endian)
 *  @param y        The Y-coordinate (affine, big-endian)
 *  @param len      The length of x and y in bytes
 *  @param ec_ctx   The EC context
 *  @return         0 for success, the appopriate error code otherwise
 */
EXPORT_SYM int ec_ws_new_point(EcPoint **pecp, const uint8_t *x, const uint8_t *y, size_t len, const EcContext *ec_ctx)
{
    int res;
    Workplace *wp = NULL;
    EcPoint *ecp;
    MontContext *ctx;
    
    if (NULL == pecp || NULL == x || NULL == y || NULL == ec_ctx)
        return ERR_NULL;
    ctx = ec_ctx->mont_ctx;

    if (len != ctx->bytes)
        return ERR_VALUE;

    *pecp = ecp = (EcPoint*)calloc(1, sizeof(EcPoint));
    if (NULL == ecp)
        return ERR_MEMORY;

    ecp->ec_ctx = ec_ctx;
    res = mont_from_bytes(&ecp->x, x, len, ctx);
    if (res) goto cleanup;
    res = mont_from_bytes(&ecp->y, y, len, ctx);
    if (res) goto cleanup;
    res = mont_number(&ecp->z, 1, ctx);
    if (res) goto cleanup;
    mont_set(ecp->z, 1, NULL, ctx);

    /** Convert (0, 0) to (1, 1, 0) */
    /** Verify the point is on the curve, if not point-at-infinity */
    if (mont_is_zero(ecp->x, ctx) && mont_is_zero(ecp->y, ctx)) {
        mont_set(ecp->x, 1, NULL, ctx);
        mont_set(ecp->y, 1, NULL, ctx);
        mont_set(ecp->z, 0, NULL, ctx);
    } else {
        wp = new_workplace(ctx);
        mont_mult(wp->a, ecp->y, ecp->y, wp->scratch, ctx);
        mont_mult(wp->c, ecp->x, ecp->x, wp->scratch, ctx);
        mont_mult(wp->c, wp->c, ecp->x, wp->scratch, ctx);
        mont_sub(wp->c, wp->c, ecp->x, wp->scratch, ctx);
        mont_sub(wp->c, wp->c, ecp->x, wp->scratch, ctx);
        mont_sub(wp->c, wp->c, ecp->x, wp->scratch, ctx);
        mont_add(wp->c, wp->c, ec_ctx->b, wp->scratch, ctx);
        res = !mont_is_equal(wp->a, wp->c, ctx);
        free_workplace(wp);

        if (res) {
            res = ERR_EC_POINT;
            goto cleanup;
        }
    }
    return 0;

cleanup:
    free(ecp->x);
    free(ecp->y);
    free(ecp->z);
    free(ecp);
    *pecp = NULL;
    return res;
}

EXPORT_SYM void ec_free_point(EcPoint *ecp)
{
    if (NULL == ecp)
        return;

    /* It is not up to us to deallocate the EC context */
    free(ecp->x);
    free(ecp->y);
    free(ecp->z);
    free(ecp);
}

/*
 * Encode the affine coordinates of an EC point.
 *
 * @param x     The location where the affine X-coordinate will be store in big-endian mode
 * @param y     The location where the affine Y-coordinate will be store in big-endian mode
 * @param len   The memory available for x and y in bytes.
 *              It must be as long as the prime modulus of the curve field.
 * @param ecp   The EC point to encode.
 */
EXPORT_SYM int ec_ws_get_xy(uint8_t *x, uint8_t *y, size_t len, const EcPoint *ecp)
{
    uint64_t *xw=NULL, *yw=NULL;
    Workplace *wp;
    MontContext *ctx;
    int res;

    if (NULL == x || NULL == y || NULL == ecp)
        return ERR_NULL;
    ctx = ecp->ec_ctx->mont_ctx;

    if (len != mont_bytes(ctx))
        return ERR_VALUE;

    wp = new_workplace(ctx);
    if (NULL == wp)
        return ERR_MEMORY;

    res = mont_number(&xw, 1, ctx);
    if (res) goto cleanup;
    res = mont_number(&yw, 1, ctx);
    if (res) goto cleanup;

    ec_ws_normalize(xw, yw, ecp->x, ecp->y, ecp->z, wp, ctx);
    res = mont_to_bytes(x, xw, ctx);
    if (res) goto cleanup;
    res = mont_to_bytes(y, yw, ctx);
    if (res) goto cleanup;

    res = 0;

cleanup:
    free_workplace(wp);
    free(xw);
    free(yw);
    return res;
}

/*
 * Double an EC point
 */
EXPORT_SYM int ec_ws_double(EcPoint *p)
{
    Workplace *wp;
    MontContext *ctx;

    if (NULL == p)
        return ERR_NULL;
    ctx = p->ec_ctx->mont_ctx;

    wp = new_workplace(ctx);
    if (NULL == wp)
        return ERR_MEMORY;

    ec_full_double(p->x, p->y, p->z, p->x, p->y, p->z, wp, ctx);

    free_workplace(wp);
    return 0;
}

/*
 * Add an EC point to another
 */
EXPORT_SYM int ec_ws_add(EcPoint *ecpa, EcPoint *ecpb)
{
    Workplace *wp;
    MontContext *ctx;

    if (NULL == ecpa || NULL == ecpb)
        return ERR_NULL;
    if (ecpa->ec_ctx != ecpb->ec_ctx)
        return ERR_EC_CURVE;
    ctx = ecpa->ec_ctx->mont_ctx;

    wp = new_workplace(ctx);
    if (NULL == wp)
        return ERR_MEMORY;

    ec_full_add(ecpa->x, ecpa->y, ecpa->z,
                ecpa->x, ecpa->y, ecpa->z,
                ecpb->x, ecpb->y, ecpb->z,
                wp, ctx);

    free_workplace(wp);
    return 0;
}

/*
 * Blind the scalar factor to be used in an EC multiplication
 *
 * @param blind_scalar      The area of memory where the pointer to a newly
 *                          allocated blind scalar is stored, in big endian mode.
 *                          The caller must deallocate this memory.
 * @param blind_scalar_len  The area where the length of the blind scalar in bytes will be written to.
 * @param scalar            The (secret) scalar to blind.
 * @param scalar_len        The length of the secret scalar in bytes.
 * @param R_seed            The 32-bit factor to use to blind the scalar.
 * @param order             The order of the EC curve, big-endian mode, 64 bit words
 * @param order_words       The number of words making up the order
 */
static int blind_scalar_factor(uint8_t **blind_scalar,
                        size_t *blind_scalar_len,
                        const uint8_t *scalar,
                        size_t scalar_len,
                        uint32_t R_seed,
                        uint64_t *order,
                        size_t order_words)
{
    size_t scalar_words;
    size_t blind_scalar_words;
    uint64_t *output_u64 = NULL;
    int res = ERR_MEMORY;

    scalar_words = (scalar_len+7)/8;
    blind_scalar_words = MAX(order_words+2, scalar_words+2);
    *blind_scalar_len = blind_scalar_words*sizeof(uint64_t);

    *blind_scalar = (uint8_t*)calloc(*blind_scalar_len, 1);
    if (NULL == *blind_scalar)
        goto cleanup;

    output_u64 = (uint64_t*)calloc(blind_scalar_words, sizeof(uint64_t));
    if (NULL == output_u64)
        goto cleanup;

    bytes_to_words(output_u64, blind_scalar_words, scalar, scalar_len);
    addmul128(output_u64, order, R_seed, 0, order_words);
    words_to_bytes(*blind_scalar, *blind_scalar_len, output_u64, blind_scalar_words);

    res = 0;

cleanup:
    free(output_u64);
    return res;
}

/*
 * Multiply an EC point by a scalar
 *
 * @param ecp   The EC point to multiply
 * @param k     The scalar, encoded in big endian mode
 * @param len   The length of the scalar, in bytes
 * @param seed  The 64-bit to drive the randomizations against SCAs
 * @return      0 in case of success, the appropriate error code otherwise
 */
EXPORT_SYM int ec_ws_scalar_multiply(EcPoint *ecp, const uint8_t *k, size_t len, uint64_t seed)
{
    Workplace *wp1=NULL, *wp2=NULL;
    MontContext *ctx;
    int res;

    if (NULL == ecp || NULL == k)
        return ERR_NULL;
    ctx = ecp->ec_ctx->mont_ctx;

    if (len == 0) {
        return ERR_NOT_ENOUGH_DATA;
    }

    wp1 = new_workplace(ctx);
    if (NULL == wp1) {
        res = ERR_MEMORY;
        goto cleanup;
    }

    wp2 = new_workplace(ctx);
    if (NULL == wp2) {
        res = ERR_MEMORY;
        goto cleanup;
    }

    if (seed != 0) {
        uint8_t *blind_scalar=NULL;
        size_t blind_scalar_len;
        uint64_t *factor=NULL;
        uint64_t *factor_pow=NULL;

        /* Create the blinding factor for the base point */
        res = mont_number(&factor, 2, ctx);
        if (res)
            goto cleanup;
        expand_seed(seed, (uint8_t*)factor, mont_bytes(ctx));
        factor_pow = &factor[ctx->words];

        /* Blind the base point */
        mont_mult(ecp->z, ecp->z, factor, wp1->scratch, ctx);
        mont_mult(factor_pow, factor, factor, wp1->scratch, ctx);
        mont_mult(ecp->x, ecp->x, factor_pow, wp1->scratch, ctx);
        mont_mult(factor_pow, factor_pow, factor, wp1->scratch, ctx);
        mont_mult(ecp->y, ecp->y, factor_pow, wp1->scratch, ctx);

        free(factor);

        /* Blind the scalar, by adding R*order where R is at least 32 bits */
        res = blind_scalar_factor(&blind_scalar,
                                  &blind_scalar_len,
                                  k, len,
                                  (uint32_t)seed,
                                  ecp->ec_ctx->order,
                                  ctx->words);
        if (res) goto cleanup;
        res = ec_exp(ecp->x, ecp->y, ecp->z,
                     ecp->x, ecp->y, ecp->z,
                     blind_scalar, blind_scalar_len,
                     seed + 1,
                     wp1, wp2, ctx);

        free(blind_scalar);
        if (res) goto cleanup;
    } else {
        res = ec_exp(ecp->x, ecp->y, ecp->z,
                     ecp->x, ecp->y, ecp->z,
                     k, len,
                     seed + 1,
                     wp1, wp2, ctx);
        if (res) goto cleanup;
    }

    res = 0;

cleanup:
    free_workplace(wp1);
    free_workplace(wp2);
    return res;
}

EXPORT_SYM int ec_ws_clone(EcPoint **pecp2, const EcPoint *ecp)
{
    int res;
    EcPoint *ecp2;
    MontContext *ctx;

    if (NULL == pecp2 || NULL == ecp)
        return ERR_NULL;
    ctx = ecp->ec_ctx->mont_ctx;

    *pecp2 = ecp2 = (EcPoint*)calloc(1, sizeof(EcPoint));
    if (NULL == ecp2)
        return ERR_MEMORY;

    ecp2->ec_ctx = ecp->ec_ctx;

    res = mont_number(&ecp2->x, 1, ctx);
    if (res) goto cleanup;
    mont_copy(ecp2->x, ecp->x, ctx);

    res = mont_number(&ecp2->y, 1, ctx);
    if (res) goto cleanup;
    mont_copy(ecp2->y, ecp->y, ctx);

    res = mont_number(&ecp2->z, 1, ctx);
    if (res) goto cleanup;
    mont_copy(ecp2->z, ecp->z, ctx);

    return 0;

cleanup:
    free(ecp2->x);
    free(ecp2->y);
    free(ecp2->z);
    free(ecp2);
    *pecp2 = NULL;
    return res;
}

EXPORT_SYM int ec_ws_cmp(const EcPoint *ecp1, const EcPoint *ecp2)
{
    Workplace *wp;
    MontContext *ctx;

    if (NULL == ecp1 || NULL == ecp2)
        return ERR_NULL;

    if (ecp1->ec_ctx != ecp2->ec_ctx)
        return ERR_EC_CURVE;
    ctx = ecp1->ec_ctx->mont_ctx;

    /* Check for point-at-infinity */
    if (mont_is_zero(ecp1->z, ctx) && mont_is_zero(ecp2->z, ctx))
        return 0;

    /* Check when Z1=Z2 */
    if (mont_is_equal(ecp1->z, ecp2->z, ctx)) {
        return !mont_is_equal(ecp1->x, ecp2->x, ctx) || !mont_is_equal(ecp1->y, ecp2->y, ctx);
    }

    /** Normalize to have Z1=Z2 */
    wp = new_workplace(ctx);
    if (NULL == wp)
        return ERR_MEMORY;

    mont_mult(wp->a, ecp2->z, ecp2->z, wp->scratch, ctx);
    mont_mult(wp->b, ecp1->x, wp->a, wp->scratch, ctx);      /* B = X1*Z2² */

    mont_mult(wp->c, ecp1->z, ecp1->z, wp->scratch, ctx);
    mont_mult(wp->d, ecp2->x, wp->c, wp->scratch, ctx);      /* C = X2*Z1² */

    if (!mont_is_equal(wp->b, wp->d, ctx))
        return -1;

    mont_mult(wp->a, ecp2->z, wp->a, wp->scratch, ctx);
    mont_mult(wp->e, ecp1->y, wp->a, wp->scratch, ctx);      /* E = Y1*Z2³ */

    mont_mult(wp->c, ecp1->z, wp->c, wp->scratch, ctx);
    mont_mult(wp->f, ecp2->y, wp->c, wp->scratch, ctx);      /* F = Y2*Z1³ */

    if (!mont_is_equal(wp->e, wp->f, ctx))
        return -2;

    return 0;
}

EXPORT_SYM int ec_ws_neg(EcPoint *p)
{
    MontContext *ctx;
    uint64_t *tmp;
    int res;

    if (NULL == p)
        return ERR_NULL;
    ctx = p->ec_ctx->mont_ctx;

    res = mont_number(&tmp, SCRATCHPAD_NR, ctx);
    if (res)
        return res;

    mont_sub(p->y, ctx->modulus, p->y, tmp, ctx);
    free(tmp);
    return 0;
}

#ifdef MAIN2
int main(void)
{
    MontContext *ctx;
    Workplace *wp1, *wp2;
    const uint8_t p256_mod[32] = "\xff\xff\xff\xff\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff";
    const uint8_t p256_Gx[32] = "\x6b\x17\xd1\xf2\xe1\x2c\x42\x47\xf8\xbc\xe6\xe5\x63\xa4\x40\xf2\x77\x03\x7d\x81\x2d\xeb\x33\xa0\xf4\xa1\x39\x45\xd8\x98\xc2\x96";
    const uint8_t p256_Gy[32] = "\x4f\xe3\x42\xe2\xfe\x1a\x7f\x9b\x8e\xe7\xeb\x4a\x7c\x0f\x9e\x16\x2b\xce\x33\x57\x6b\x31\x5e\xce\xcb\xb6\x40\x68\x37\xbf\x51\xf5";

    uint64_t *Gx, *Gy, *Gz;
    uint64_t *Qx, *Qy, *Qz;
    unsigned i;

    mont_context_init(&ctx, p256_mod, sizeof(p256_mod));
    wp1 = new_workplace(ctx);
    wp2 = new_workplace(ctx);

    mont_from_bytes(&Gx, p256_Gx, sizeof(p256_Gx), ctx);
    mont_from_bytes(&Gy, p256_Gy, sizeof(p256_Gy), ctx);
    mont_number(&Gz, 1, ctx);
    mont_set(Gz, 1, NULL, ctx);

    /* Create point in Jacobian coordinates */
    mont_number(&Qx, 1, ctx);
    mont_number(&Qy, 1, ctx);
    mont_number(&Qz, 1, ctx);

    printf("----------------------------\n");

    for (i=0; i<=5000; i++)
        ec_exp(Qx, Qy, Qz, Gx, Gy, Gz, (uint8_t*)"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8, wp1, wp2, ctx);

    print_x("Qx", Qx, ctx);
    print_x("Qy", Qy, ctx);
    print_x("Qz", Qz, ctx);

    printf("----------------------------\n");

    ec_ws_normalize(Qx, Qy, Qx, Qy, Qz, wp1, ctx);

    print_x("Qx", Qx, ctx);
    print_x("Qy", Qy, ctx);

    free(Gx);
    free(Gy);
    free(Qx);
    free(Qy);
    free(Qz);
    free_workplace(wp1);
    free_workplace(wp2);
    mont_context_free(ctx);

    return 0;
}
#endif

#ifdef MAIN
int main(void)
{
    const uint8_t p256_mod[32] = "\xff\xff\xff\xff\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff";
    const uint8_t  b[32] = "\x5a\xc6\x35\xd8\xaa\x3a\x93\xe7\xb3\xeb\xbd\x55\x76\x98\x86\xbc\x65\x1d\x06\xb0\xcc\x53\xb0\xf6\x3b\xce\x3c\x3e\x27\xd2\x60\x4b";
    const uint8_t order[32] = "\xff\xff\xff\xff\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\xbc\xe6\xfa\xad\xa7\x17\x9e\x84\xf3\xb9\xca\xc2\xfc\x63\x25\x51";
    const uint8_t p256_Gx[32] = "\x6b\x17\xd1\xf2\xe1\x2c\x42\x47\xf8\xbc\xe6\xe5\x63\xa4\x40\xf2\x77\x03\x7d\x81\x2d\xeb\x33\xa0\xf4\xa1\x39\x45\xd8\x98\xc2\x96";
    const uint8_t p256_Gy[32] = "\x4f\xe3\x42\xe2\xfe\x1a\x7f\x9b\x8e\xe7\xeb\x4a\x7c\x0f\x9e\x16\x2b\xce\x33\x57\x6b\x31\x5e\xce\xcb\xb6\x40\x68\x37\xbf\x51\xf5";
    uint8_t x[32], y[32];
    uint8_t exp[32];
    EcContext *ec_ctx;
    EcPoint *ecp = NULL;
    int i;

    memset(exp, 0xFF, 32);

    ec_ws_new_context(&ec_ctx, p256_mod, b, order, 32);
    ec_ws_new_point(&ecp, p256_Gx, p256_Gy, 32, ec_ctx);

    for (i=0; i<=5000; i++)
        ec_ws_scalar_multiply(ecp, exp, 32, 0xFFF);

    ec_ws_get_xy(x, y, 32, ecp);
    printf("X: ");
    for (i=0; i<32; i++)
        printf("%02X", x[i]);
    printf("\n");
    printf("Y: ");
    for (i=0; i<32; i++)
        printf("%02X", y[i]);
    printf("\n");


    ec_free_point(ecp);
    ec_free_context(ec_ctx);

    return 0;
}
#endif
