/* rv_fp: IEEE 754-2019 binary32/binary64 arithmetic for the golden model,
 * written with integer operations only (no float, no double, no <math.h>),
 * so every result is the same on every host.  The behaviour is the RISC-V
 * F/D one: tininess is detected after rounding, every NaN result is the
 * canonical quiet NaN, and invalid float-to-integer conversions saturate.
 *
 * Values are raw bit patterns in a uint64_t.  `d` selects the format:
 * 0 = binary32 (in the low 32 bits, already un-boxed), 1 = binary64.
 * Functions OR the exception flags they raise into *fl.
 *
 * Checked against Berkeley TestFloat for every operation, format and
 * rounding mode (make fp-model-test; see docs/report.md section 12). */
#ifndef RV_FP_H
#define RV_FP_H

#include <stdint.h>

enum { RV_RNE = 0, RV_RTZ = 1, RV_RDN = 2, RV_RUP = 3, RV_RMM = 4 };         /* frm */
enum { RV_NX = 1, RV_UF = 2, RV_OF = 4, RV_DZ = 8, RV_NV = 16 };             /* fflags */

uint64_t rvfp_add(int d, uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t rvfp_sub(int d, uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t rvfp_mul(int d, uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t rvfp_div(int d, uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t rvfp_sqrt(int d, uint64_t a, int rm, uint32_t *fl);
/* (-1)^neg_prod * (a * b) + (-1)^neg_c * c, rounded once */
uint64_t rvfp_fma(int d, uint64_t a, uint64_t b, uint64_t c, int neg_prod, int neg_c, int rm, uint32_t *fl);
/* FMIN/FMAX of the 2019 standard (minimumNumber/maximumNumber): -0 < +0, a
 * quiet NaN loses against a number */
uint64_t rvfp_minmax(int d, uint64_t a, uint64_t b, int is_max, uint32_t *fl);
int      rvfp_eq(int d, uint64_t a, uint64_t b, uint32_t *fl);   /* quiet */
int      rvfp_lt(int d, uint64_t a, uint64_t b, uint32_t *fl);   /* signaling */
int      rvfp_le(int d, uint64_t a, uint64_t b, uint32_t *fl);   /* signaling */
uint32_t rvfp_classify(int d, uint64_t a);                       /* FCLASS mask */
/* conversions; to_d / from_d give the format of the result / the operand */
uint64_t rvfp_f2f(int to_d, uint64_t a, int rm, uint32_t *fl);   /* from the other format */
uint64_t rvfp_i2f(int to_d, uint32_t v, int is_unsigned, int rm, uint32_t *fl);
uint32_t rvfp_f2i(int from_d, uint64_t a, int is_unsigned, int rm, uint32_t *fl);

#endif
