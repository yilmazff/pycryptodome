#ifndef _EC_H
#define _EC_H

#include "common.h"
#include "mont.h"

typedef struct {
    uint64_t *a, *b, *c, *d, *e, *f, *g, *h;
    uint64_t *scratch;
} Workplace;

/*
 * The description of a short Weierstrass curve, with y²=x³-3x+b
 */
typedef struct _EcContext {
    MontContext *mont_ctx;
    uint64_t *b;
} EcContext;

/*
 * An EC point in Jacobian coordinates
 */
typedef struct _EcPoint {
    const EcContext *ec_ctx;
    uint64_t *x;
    uint64_t *y;
    uint64_t *z;
} EcPoint;

EXPORT_SYM int ec_ws_new_context(EcContext **pec_ctx, const uint8_t *modulus, const uint8_t *b, size_t len);
EXPORT_SYM void ec_free_context(EcContext *ec_ctx);
EXPORT_SYM int ec_ws_new_point(EcPoint **pecp, uint8_t *x, uint8_t *y, size_t len, const EcContext *ec_ctx);
EXPORT_SYM void ec_free_point(EcPoint *ecp);
EXPORT_SYM int ec_ws_get_xy(uint8_t *x, uint8_t *y, size_t len, const EcPoint *ecp);
EXPORT_SYM int ec_ws_double(EcPoint *p);
EXPORT_SYM int ec_ws_add(EcPoint *ecpa, EcPoint *ecpb);
EXPORT_SYM int ec_ws_scalar_multiply(EcPoint *ecp, const uint8_t *k, size_t len, uint64_t seed);
EXPORT_SYM int ec_ws_clone(EcPoint **pecp2, const EcPoint *ecp);
EXPORT_SYM int ec_ws_cmp(const EcPoint *ecp1, const EcPoint *ecp2);
EXPORT_SYM int ec_ws_neg(EcPoint *p);

#endif
