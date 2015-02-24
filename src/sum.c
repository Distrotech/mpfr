/* Sum -- efficiently sum a list of floating-point numbers

Copyright 2014-2015 Free Software Foundation, Inc.
Contributed by the AriC and Caramel projects, INRIA.

This file is part of the GNU MPFR Library.

The GNU MPFR Library is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 3 of the License, or (at your
option) any later version.

The GNU MPFR Library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
License for more details.

You should have received a copy of the GNU Lesser General Public License
along with the GNU MPFR Library; see the file COPYING.LESSER.  If not, see
http://www.gnu.org/licenses/ or write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA. */

#define MPFR_NEED_LONGLONG_H
#include "mpfr-impl.h"

/* See the doc/sum.txt file for the algorithm and a part of its proof
(this will later go into algorithms.tex).

Note: see the following paper and its references:
http://www.eecs.berkeley.edu/~hdnguyen/public/papers/ARITH21_Fast_Sum.pdf
VL: This is very different:
          In MPFR             In the paper & references
    arbitrary precision            fixed precision
     correct rounding        just reproducible rounding
    integer operations        floating-point operations
        sequencial             parallel (& sequential)
*/

/* Update minexp after detecting a potential integer overflow in extreme
   cases (only 32-bit machines may be concerned in practice). */
#define UPDATE_MINEXP(E,SH)                     \
  do                                            \
    {                                           \
      mpfr_prec_t sh = (SH);                    \
      MPFR_ASSERTN ((E) >= MPFR_EXP_MIN + sh);  \
      minexp = (E) - sh;                        \
    }                                           \
  while (0)

/* Accumulate a new [minexp,maxexp[ block into (wp,ws). If e and err denote
 * the exponents of the computed result and of the error bound respectively,
 * while e - err is less than some given bound (due to cancellation), shift
 * the accumulator and reiterate.
 *   wp: pointer to the accumulator (least significant limb first).
 *   ws: size of the accumulator.
 *   wq: precision of the accumulator (ws * GMP_NUMB_BITS).
 *   x: array of the input numbers.
 *   n: size of this array (number of inputs).
 *   minexp: exponent of the least significant bit of the block.
 *   maxexp: exponent of the block (maximum exponent + 1).
 *   tp: pointer to a temporary area.
 *   ts: size of this temporary area.
 *   logn: ceil(log2(rn)), where rn is the number of regular inputs.
 *   cq: value of cq in the main code (logn + 1).
 *   prec: minimal value of e - err (see below).
 *   ep: pointer to mpfr_exp_t (see below), or a null pointer.
 *   errp: pointer to mpfr_exp_t (see below), or a null pointer.
 *   maxexpp: pointer to mpfr_exp_t (see below).
 * This function returns the number of cancelled bits (>= 1), or 0
 * if the accumulator is 0 (then the exact sum is necessarily 0).
 * In the former case, the function also returns:
 * - in ep: the exponent e of the computed result;
 * - in errp: the exponent err of the error bound;
 * - in maxexpp: the new value of maxexp.
 * Notes:
 * - minexp is also the exponent of the least significant bit of the
 *   accumulator;
 * - the temporary area must be large enough to hold a shifted input
 *   block, and the value of ts is used only when the full assertions
 *   are checked (i.e. with the --enable-assert configure option), to
 *   check that a buffer overflow doesn't occur;
 * - one has: *errp <= *ep - prec if the accumulator is not 0.
 */
static mpfr_prec_t
sum_raw (mp_limb_t *wp, mp_size_t ws, mpfr_prec_t wq, mpfr_ptr *const x,
         unsigned long n, mpfr_exp_t minexp, mpfr_exp_t maxexp,
         mp_limb_t *tp, mp_size_t ts, int logn, int cq, mpfr_prec_t prec,
         mpfr_exp_t *ep, mpfr_exp_t *errp, mpfr_exp_t *maxexpp)
{
  MPFR_LOG_FUNC
    (("ws=%Pd ts=%Pd prec=%Pd", (mpfr_prec_t) ws, (mpfr_prec_t) ts, prec),
     ("", 0));

  /* Consistency checks. */
  MPFR_ASSERTD (wq == (mpfr_prec_t) ws * GMP_NUMB_BITS);
  MPFR_ASSERTD (cq == logn + 1);

  while (1)
    {
      mpfr_exp_t maxexp2 = MPFR_EXP_MIN;
      unsigned long i;

      MPFR_LOG_MSG (("sum_raw loop: "
                     "maxexp=%" MPFR_EXP_FSPEC "d "
                     "minexp=%" MPFR_EXP_FSPEC "d\n",
                     (mpfr_eexp_t) maxexp, (mpfr_eexp_t) minexp));

      MPFR_ASSERTD (maxexp > minexp);

      for (i = 0; i < n; i++)
        if (! MPFR_IS_SINGULAR (x[i]))
          {
            mp_limb_t *dp, *vp;
            mp_size_t ds, vs, vds;
            mpfr_exp_t xe, vd;
            mpfr_prec_t xq;
            int tr;

            xe = MPFR_GET_EXP (x[i]);
            xq = MPFR_GET_PREC (x[i]);

            vp = MPFR_MANT (x[i]);
            vs = MPFR_PREC2LIMBS (xq);
            vd = xe - vs * GMP_NUMB_BITS - minexp;
            /* vd is the exponent of the least significant represented bit of
               x[i] (including the trailing bits, whose value is 0) minus the
               exponent of the least significant bit of the accumulator. To
               make the code simpler, we won't try to filter out the trailing
               bits of x[i]. */

            if (vd < 0)
              {
                /* This covers the following cases:
                 *     [-+- accumulator ---]
                 *   [---|----- x[i] ------|--]
                 *       |   [----- x[i] --|--]
                 *       |                 |[----- x[i] -----]
                 *       |                 |    [----- x[i] -----]
                 *     maxexp           minexp
                 */

                if (xe <= minexp)
                  {
                    /* x[i] is entirely after the LSB of the accumulator,
                       so that it will be ignored at this iteration. */
                    if (xe > maxexp2)
                      maxexp2 = xe;
                    continue;
                  }

                /* If some significant bits of x[i] are after the LSB of the
                   accumulator, then maxexp2 will necessarily be minexp. */
                if (MPFR_LIKELY (xe - xq < minexp))
                  maxexp2 = minexp;

                /* We need to ignore the least |vd| significant bits of x[i].
                   First, let's ignore the least vds = |vd| / GMP_NUMB_BITS
                   limbs. */
                vd = - vd;
                vds = vd / GMP_NUMB_BITS;
                vs -= vds;
                MPFR_ASSERTD (vs > 0);  /* see xe <= minexp test above */
                vp += vds;
                vd -= vds * GMP_NUMB_BITS;
                MPFR_ASSERTD (vd >= 0 && vd < GMP_NUMB_BITS);

                if (xe > maxexp)
                  {
                    vs -= (xe - maxexp) / GMP_NUMB_BITS;
                    MPFR_ASSERTD (vs > 0);
                    tr = (xe - maxexp) % GMP_NUMB_BITS;
                  }
                else
                  tr = 0;

                if (vd != 0)
                  {
                    MPFR_ASSERTD (vs <= ts);
                    mpn_rshift (tp, vp, vs, vd);
                    vp = tp;
                    tr += vd;
                    if (tr >= GMP_NUMB_BITS)
                      {
                        vs--;
                        tr -= GMP_NUMB_BITS;
                      }
                    MPFR_ASSERTD (tr >= 0 && tr < GMP_NUMB_BITS);
                    if (tr != 0)
                      {
                        tp[vs-1] &= MPFR_LIMB_MASK (GMP_NUMB_BITS - tr);
                        tr = 0;
                      }
                    /* Truncation has now been taken into account. */
                    MPFR_ASSERTD (tr == 0);
                  }

                dp = wp;
                ds = ws;
              }
            else  /* vd >= 0 */
              {
                /* This covers the following cases:
                 *               [-+- accumulator ---]
                 *   [- x[i] -]    |
                 *             [---|-- x[i] ------]  |
                 *          [------|-- x[i] ---------]
                 *                 |   [- x[i] -]    |
                 *               maxexp           minexp
                 */

                /* We need to ignore the least vd significant bits
                   of the accumulator. First, let's ignore the least
                   vds = vd / GMP_NUMB_BITS limbs. -> (dp,ds) */
                vds = vd / GMP_NUMB_BITS;
                ds = ws - vds;
                if (ds <= 0)
                  continue;
                dp = wp + vds;
                vd -= vds * GMP_NUMB_BITS;
                MPFR_ASSERTD (vd >= 0 && vd < GMP_NUMB_BITS);

                /* The low part of x[i] (to be determined) will have to be
                   shifted vd bits to the left if vd != 0. */

                if (xe > maxexp)
                  {
                    vs -= (xe - maxexp) / GMP_NUMB_BITS;
                    if (vs <= 0)
                      continue;
                    tr = (xe - maxexp) % GMP_NUMB_BITS;
                  }
                else
                  tr = 0;

                MPFR_ASSERTD (tr >= 0 && tr < GMP_NUMB_BITS && vs > 0);

                /* We need to consider the least significant vs limbs of x[i]
                   except the most significant tr bits. */

                if (vd != 0)
                  {
                    mp_limb_t carry;

                    MPFR_ASSERTD (vs <= ts);
                    carry = mpn_lshift (tp, vp, vs, vd);
                    tr -= vd;
                    if (tr < 0)
                      {
                        tr += GMP_NUMB_BITS;
                        MPFR_ASSERTD (vs + 1 <= ts);
                        tp[vs++] = carry;
                      }
                    MPFR_ASSERTD (tr >= 0 && tr < GMP_NUMB_BITS);
                    vp = tp;
                  }
              }

            MPFR_ASSERTD (vs > 0 && vs <= ds);

            /* We can't truncate the most significant limb of the input
               (in case it hasn't been shifted to the temporary area).
               So, let's ignore it now. It will be taken into account
               via carry propagation after the addition. */
            if (tr != 0)
              vs--;

            if (MPFR_IS_POS (x[i]))
              {
                mp_limb_t carry;

                carry = vs > 0 ? mpn_add_n (dp, dp, vp, vs) : 0;
                MPFR_ASSERTD (carry <= 1);
                if (tr != 0)
                  carry += vp[vs] & MPFR_LIMB_MASK (GMP_NUMB_BITS - tr);
                mpn_add_1 (dp + vs, dp + vs, ds - vs, carry);
              }
            else
              {
                mp_limb_t borrow;

                borrow = vs > 0 ? mpn_sub_n (dp, dp, vp, vs) : 0;
                MPFR_ASSERTD (borrow <= 1);
                if (tr != 0)
                  borrow += vp[vs] & MPFR_LIMB_MASK (GMP_NUMB_BITS - tr);
                mpn_sub_1 (dp + vs, dp + vs, ds - vs, borrow);
              }
          }

      {
        mpfr_prec_t cancel;  /* number of cancelled bits */
        mp_size_t wi;        /* index in the accumulator */
        mp_limb_t a, b;
        int cnt;

        cancel = 0;
        wi = ws - 1;
        MPFR_ASSERTD (wi >= 0);
        a = wp[wi] >> (GMP_NUMB_BITS - 1) ? MPFR_LIMB_MAX : MPFR_LIMB_ZERO;

        while (wi >= 0)
          if ((b = wp[wi]) == a)
            {
              cancel += GMP_NUMB_BITS;
              wi--;
            }
          else
            {
              b ^= a;
              MPFR_ASSERTD (b != 0 && b < MPFR_LIMB_HIGHBIT);
              count_leading_zeros (cnt, b);
              MPFR_ASSERTD (cnt >= 1);
              cancel += cnt;
              break;
            }

        if (wi >= 0 || a != MPFR_LIMB_ZERO)  /* accumulator != 0 */
          {
            mpfr_exp_t e;        /* exponent of the computed result */
            mpfr_exp_t err;      /* exponent of the error bound */

            MPFR_LOG_MSG (("accumulator %s 0, cancel = %Pd\n",
                           a != MPFR_LIMB_ZERO ? "<" : ">", cancel));

            MPFR_ASSERTD (cancel > 0);
            e = minexp + wq - cancel;
            MPFR_ASSERTD (e >= minexp);
            err = maxexp2 + logn;  /* OK even if maxexp2 == MPFR_EXP_MIN */

            /* The absolute value of the truncated sum is in the binade
               [2^(e-1),2^e] (closed on both ends due to two's complement).
               The error is strictly less than 2^err (and is 0 if
               maxexp2 == MPFR_EXP_MIN). */

            MPFR_LOG_MSG (("e = %" MPFR_EXP_FSPEC "d err= %" MPFR_EXP_FSPEC
                           "d\n", (mpfr_eexp_t) e, (mpfr_eexp_t) err));

            /* This basically tests whether err <= e - prec without
               potential integer overflow... */
            if (e >= 0 ? (err <= e - prec) :
                (err <= e && (mpfr_uexp_t) -e + prec >= -err))
              {
                if (ep != NULL)
                  *ep = e;
                if (errp != NULL)
                  *errp = err;
                *maxexpp = maxexp2;
                return cancel;
              }
            else
              {
                mpfr_exp_t diffexp;
                mpfr_prec_t shiftq;
                mpfr_size_t shifts;
                int shiftc;

                diffexp = err - e;
                if (diffexp < 0)
                  diffexp = 0;
                /* diffexp = max(0, err - e) */
                MPFR_ASSERTD (diffexp < cancel - 2);
                shiftq = cancel - 2 - (mpfr_prec_t) diffexp;
                MPFR_ASSERTD (shiftq > 0);
                shifts = shiftq / GMP_NUMB_BITS;
                shiftc = shiftq % GMP_NUMB_BITS;
                MPFR_LOG_MSG (("shiftq = %Pd = %Pd * GMP_NUMB_BITS + %d\n",
                               shiftq, (mpfr_prec_t) shifts, shiftc));
                if (MPFR_LIKELY (shiftc != 0))
                  mpn_lshift (wp + shifts, wp, ws - shifts, shiftc);
                else
                  MPN_COPY_DECR (wp + shifts, wp, ws - shifts);
                MPN_ZERO (wp, shifts);
                minexp -= shiftq;
              }
          }
        else if (maxexp2 == MPFR_EXP_MIN)
          {
            MPFR_LOG_MSG (("accumulator = 0, maxexp2 = MPFR_EXP_MIN\n", 0));
            return 0;
          }
        else
          {
            MPFR_LOG_MSG (("accumulator = 0, reiterate\n", 0));
            UPDATE_MINEXP (maxexp2, wq - cq);
          }
      }

      maxexp = maxexp2;
    }
}

/**********************************************************************/

/* Generic case: all the inputs are finite numbers,
   with at least 3 regular numbers. */
static int
sum_aux (mpfr_ptr sum, mpfr_ptr *const x, unsigned long n, mpfr_rnd_t rnd,
         mpfr_exp_t maxexp, unsigned long rn)
{
  mp_limb_t *sump;
  mp_limb_t *tp;  /* pointer to a temporary area */
  mp_limb_t *wp;  /* pointer to the accumulator */
  mp_size_t ts;   /* size of the temporary area, in limbs */
  mp_size_t ws;   /* size of the accumulator, in limbs */
  mpfr_prec_t wq; /* size of the accumulator, in bits */
  int logn;       /* ceil(log2(rn)) */
  int cq;
  mpfr_prec_t sq;
  int inex;
  MPFR_TMP_DECL (marker);

  MPFR_LOG_FUNC
    (("n=%lu rnd=%d maxexp=%" MPFR_EXP_FSPEC "d rn=%lu",
      n, rnd, (mpfr_eexp_t) maxexp, rn),
     ("sum[%Pu]=%.*Rg", mpfr_get_prec (sum), mpfr_log_prec, sum));

  MPFR_ASSERTD (rn >= 3 && rn <= n);

  /* Step 2: set up some variables and the accumulator. */

  sump = MPFR_MANT (sum);

  /* rn is the number of regular inputs (the singular ones will be
     ignored). Compute logn = ceil(log2(rn)). */
  logn = MPFR_INT_CEIL_LOG2 (rn);
  MPFR_ASSERTD (logn >= 2);

  MPFR_LOG_MSG (("Step 2 with logn=%d maxexp=%" MPFR_EXP_FSPEC "d\n",
                 logn, (mpfr_eexp_t) maxexp));

  sq = MPFR_GET_PREC (sum);
  cq = logn + 1;

  /* First determine the size of the accumulator. */
  ws = MPFR_PREC2LIMBS (cq + sq + logn + 2);
  wq = (mpfr_prec_t) ws * GMP_NUMB_BITS;
  MPFR_ASSERTD (wq - cq - sq >= 4);

  MPFR_LOG_MSG (("cq=%d sq=%Pd logn=%d wq=%Pd\n", cq, sq, logn, wq));

  /* An input block will have up to wq - cq bits, and its shifted value
     (to be correctly aligned) may take GMP_NUMB_BITS - 1 additional bits. */
  ts = MPFR_PREC2LIMBS (wq - cq + GMP_NUMB_BITS - 1);

  MPFR_TMP_MARK (marker);

  /* TODO: one may need a bit more memory later for Step 6.
     Should it be allocated here? */
  tp = MPFR_TMP_LIMBS_ALLOC (ts + ws);
  wp = tp + ts;

  MPN_ZERO (wp, ws);  /* zero the accumulator */

  {
    mpfr_exp_t minexp;   /* exponent of the LSB of the block */
    mpfr_prec_t cancel;  /* number of cancelled bits */
    mpfr_exp_t e;        /* temporary exponent of the result */
    mpfr_exp_t u;        /* temporary exponent of the ulp (quantum) */
    mpfr_exp_t err;      /* exponent of the error bound */
    mp_limb_t rbit;      /* rounding bit (corrected in halfway case) */
    mp_limb_t carry;     /* carry for the initial rounding (0 or 1) */
    int sd, sh;          /* shift counts */
    mp_size_t sn;        /* size of the output number */
    int tmd;             /* 0: the TMD does not occur
                            1: the TMD occurs on a machine number
                            2: the TMD occurs on a midpoint */
    int pos;             /* 0 if negative sum, 1 if positive */

    MPFR_LOG_MSG (("Steps 3 to 6\n", 0));

    UPDATE_MINEXP (maxexp, wq - cq);
    cancel = sum_raw (wp, ws, wq, x, n, minexp, maxexp, tp, ts,
                      logn, cq, sq + 3, &e, &err, &maxexp);

    if (MPFR_UNLIKELY (cancel == 0))
      {
        /* The exact sum is zero. Since not all inputs are 0, the sum
         * is +0 except in MPFR_RNDD, as specified according to the
         * IEEE 754 rules for the addition of two numbers.
         */
        MPFR_SET_SIGN (sum, (rnd != MPFR_RNDD ?
                             MPFR_SIGN_POS : MPFR_SIGN_NEG));
        MPFR_SET_ZERO (sum);
        MPFR_TMP_FREE (marker);
        MPFR_RET (0);
      }

    /* The absolute value of the truncated sum is in the binade
       [2^(e-1),2^e] (closed on both ends due to two's complement).
       The error is strictly less than 2^err (and is 0 if
       maxexp == MPFR_EXP_MIN). */

    u = e - sq;  /* e being the exponent, u is the ulp of the target */

    MPFR_LOG_MSG (("Step 7 with cancel=%Pd"
                   " e=%" MPFR_EXP_FSPEC "d"
                   " u=%" MPFR_EXP_FSPEC "d"
                   " err=%" MPFR_EXP_FSPEC "d"
                   " maxexp=%" MPFR_EXP_FSPEC "d%s\n",
                   cancel, (mpfr_eexp_t) e, (mpfr_eexp_t) u,
                   (mpfr_eexp_t) err, (mpfr_eexp_t) maxexp,
                   maxexp == MPFR_EXP_MIN ? " (MPFR_EXP_MIN)" : ""));

    /* Let's copy/shift the bits [max(u,minexp),e) to the
       most significant part of the destination, and zero
       the least significant part (there can be one only if
       u < minexp). The trailing bits of the destination may
       contain garbage at this point. Then, at the same time,
       take the absolute value and do an initial rounding,
       zeroing the trailing bits at this point.
       TODO: This may be improved by merging some operations
       is particular case. The average speed-up may not be
       significant, though. To be tested... */

    sn = MPFR_PREC2LIMBS (sq);
    sd = (mpfr_prec_t) sn * GMP_NUMB_BITS - sq;
    sh = cancel % GMP_NUMB_BITS;

    if (MPFR_LIKELY (u > minexp))
      {
        mpfr_prec_t tq;
        mp_size_t ei, fi, wi;
        int td;

        tq = u - minexp;
        MPFR_ASSERTD (tq > 0); /* number of trailing bits */

        wi = tq / GMP_NUMB_BITS;

        if (MPFR_LIKELY (sh != 0))
          {
            ei = (e - minexp) / GMP_NUMB_BITS;
            fi = ei - (sn - 1);
            MPFR_ASSERTD (fi == wi || fi == wi + 1);
            mpn_lshift (sump, wp + fi, sn, sh);
            if (fi != wi)
              sump[0] |= wp[wi] >> (GMP_NUMB_BITS - sh);
          }
        else
          {
            MPFR_ASSERTD ((mpfr_prec_t) (ws - (wi + sn)) * GMP_NUMB_BITS
                          == cancel);
            MPN_COPY (sump, wp + wi, sn);
          }

        /* Determine the rounding bit, which is represented. */
        td = tq % GMP_NUMB_BITS;
        rbit = td >= 1 ? ((wp[wi] >> (td - 1)) & MPFR_LIMB_ONE) :
          (MPFR_ASSERTD (wi >= 1), wp[wi-1] >> (GMP_NUMB_BITS - 1));

        if (maxexp == MPFR_EXP_MIN)
          {
            /* The sum in the accumulator is exact. Determine inex:
               inex = 0 if the final sum is exact, else 1, i.e.
               inex = rounding bit || sticky bit. In round to nearest,
               also determine the rounding direction: obtained from
               the rounding bit possibly except in halfway cases. */
            if (MPFR_LIKELY (rbit == 0 ||
                             (rnd == MPFR_RNDN && ((wp[wi] >> td) & 1) == 0)))
              {
                /* We need to determine the sticky bit, either to set inex
                   (if the rounding bit is 0) or to possibly "correct" rbit
                   (round to nearest, halfway case rounded downward) from
                   which the rounding direction will be determined. */
                inex = td >= 1 ? (wp[wi] & MPFR_LIMB_MASK (td)) != 0 : 0;

                if (inex == 0)
                  {
                    mp_size_t wj = wi;

                    while (inex == 0 && wj > 0)
                      inex = wp[--wj] != 0;
                    if (inex == 0 && rbit != 0)
                      {
                        /* sticky bit = 0, rounding bit = 1,
                           i.e. halfway case, which will be
                           rounded downward (see earlier if). */
                        MPFR_ASSERTD (rnd == MPFR_RNDN);
                        inex = 1;
                        rbit = 0;  /* even rounding downward */
                      }
                  }
              }
            else
              inex = 1;
            tmd = 0;  /* We can round correctly -> no TMD. */
          }
        else  /* maxexp > MPFR_EXP_MIN */
          {
            mpfr_exp_t d;
            mp_limb_t limb, mask;
            int nbits;

            inex = 1;  /* We do not know whether the sum is exact. */

            /* Let's see whether the TMD occurs. */
            MPFR_ASSERTD (u <= MPFR_EMAX_MAX);
            MPFR_ASSERTD (err >= MPFR_EMIN_MIN);
            d = u - err;  /* representable */
            MPFR_ASSERTD (d >= 3);

            /* First chunk after the rounding bit... It starts at:
               (wi,td-2) if td >= 2,
               (wi-1,td-2+GMP_NUMB_BITS) if td < 2. */
            if (td == 0)
              {
                MPFR_ASSERTD (wi >= 1);
                limb = wp[--wi];
                mask = MPFR_LIMB_MASK (GMP_NUMB_BITS - 1);
                nbits = GMP_NUMB_BITS - 1;
              }
            else if (td == 1)
              {
                limb = wi >= 1 ? wp[--wi] : MPFR_LIMB_ZERO;
                mask = MPFR_LIMB_MAX;
                nbits = GMP_NUMB_BITS;
              }
            else  /* td >= 2 */
              {
                MPFR_ASSERTD (td >= 2);
                limb = wp[wi];
                mask = MPFR_LIMB_MASK (td - 1);
                nbits = td - 1;
              }

            if (nbits > d - 1)
              {
                limb >>= nbits - (d - 1);
                mask >>= nbits - (d - 1);
                d = 0;
              }
            else
              {
                d -= 1 + nbits;
                MPFR_ASSERTD (d >= 0);
              }

            limb &= mask;
            tmd =
              limb == MPFR_LIMB_ZERO ?
                (rbit == 0 ? 1 : rnd == MPFR_RNDN ? 2 : 0) :
              limb == mask ?
                (limb = MPFR_LIMB_MAX,
                 rbit != 0 ? 1 : rnd == MPFR_RNDN ? 2 : 0) : 0;

            while (tmd != 0 && d != 0)
              {
                mp_limb_t limb2;

                MPFR_ASSERTD (d > 0);
                if (wi == 0)
                  {
                    /* The non-represented bits are 0's. */
                    if (limb != MPFR_LIMB_ZERO)
                      tmd = 0;
                    break;
                  }
                MPFR_ASSERTD (wi > 0);
                limb2 = wp[--wi];
                if (d < GMP_NUMB_BITS)
                  {
                    if ((limb2 >> d) != (limb >> d))
                      tmd = 0;
                    break;
                  }
                if (limb2 != limb)
                  tmd = 0;
                d -= GMP_NUMB_BITS;
              }
          }
      }
    else  /* u <= minexp */
      {
        mp_size_t en;

        en = (e - minexp + (GMP_NUMB_BITS - 1)) / GMP_NUMB_BITS;
        if (MPFR_LIKELY (sh != 0))
          mpn_lshift (sump + sn - en, wp, en, sh);
        else if (MPFR_UNLIKELY (en > 0))
          MPN_COPY (sump + sn - en, wp, en);
        if (sn > en)
          MPN_ZERO (sump, sn - en);

        /* The exact value of the accumulator has been copied.
         * The TMD occurs if and only if there are bits still
         * not taken into account, and if it occurs, this is
         * necessarily on a machine number (-> tmd = 1).
         */
        rbit = 0;
        inex = tmd = maxexp != MPFR_EXP_MIN;
      }

    /* Leading bit: 1 if positive, 0 if negative. */
    pos = sump[sn-1] >> (GMP_NUMB_BITS - 1);

    MPFR_LOG_MSG (("[Step 7] tmd=%d rbit=%d inex=%d pos=%d\n",
                   tmd, rbit != 0, inex, pos));

    /* Here, if the final sum is known to be exact, inex = 0,
       otherwise inex = 1. */

    /* Determine carry for the initial rounding. Note that in
       case of exact value (inex == 0), carry is set to 0. */
    switch (rnd)
      {
      case MPFR_RNDD:
        carry = 0;
        break;
      case MPFR_RNDU:
        carry = inex;
        break;
      case MPFR_RNDZ:
        carry = inex && !pos;
        break;
      case MPFR_RNDA:
        carry = inex && pos;
        break;
      default:
        MPFR_ASSERTN (rnd == MPFR_RNDN);
        /* Note: for known halfway cases (maxexp == MPFR_EXP_MIN)
           that are rounded downward, rbit has been changed to 0
           so that carry is set correctly. */
        carry = rbit;
      }

    /* Sign handling (-> absolute value and sign), together with
       initial rounding. */
    if (pos)
      {
        mp_limb_t carry_out;

        MPFR_SET_POS (sum);
        sump[0] &= ~ MPFR_LIMB_MASK (sd);
        carry_out = mpn_add_1 (sump, sump, sn, carry << sd);
        MPFR_ASSERTD (sump[sn-1] >> (GMP_NUMB_BITS - 1) == !carry_out);
        if (carry_out)
          {
            e++;
            sump[sn-1] = MPFR_LIMB_HIGHBIT;
          }
      }
    else
      {
        MPFR_SET_NEG (sum);
        if (carry)
          {
            mpn_com (sump, sump, sn);
            sump[0] &= ~ MPFR_LIMB_MASK (sd);
            MPFR_ASSERTD (sump[sn-1] >> (GMP_NUMB_BITS - 1) == 1);
          }
        else
          {
            mp_limb_t borrow_out;

            sump[0] &= ~ MPFR_LIMB_MASK (sd);
            borrow_out = mpn_neg (sump, sump, sn);
            MPFR_ASSERTD (sump[sn-1] >> (GMP_NUMB_BITS - 1) == borrow_out);
            if (!borrow_out)
              {
                e++;
                sump[sn-1] = MPFR_LIMB_HIGHBIT;
              }
          }
      }

    if (tmd == 0)  /* no TMD */
      {
        if (carry)  /* two's complement significand increased */
          inex = -1;
      }
    else  /* Step 8 */
      {
        mp_size_t zs;
        int sst;  /* sign of the secondary term */

        MPFR_ASSERTD (maxexp > MPFR_EXP_MIN);

        /* New accumulator size */
        ws = MPFR_PREC2LIMBS (wq - sq);
        wq = (mpfr_prec_t) ws * GMP_NUMB_BITS;

        MPFR_LOG_MSG (("Step 8 with"
                       " maxexp=%" MPFR_EXP_FSPEC "d"
                       " ws=%Pd"
                       " wq=%Pd\n",
                       (mpfr_eexp_t) maxexp,
                       (mpfr_prec_t) ws, wq));

        /* The d-1 bits from u-2 to u-d (= err) are identical. */

        if (err >= minexp)
          {
            mpfr_prec_t tq;
            mp_size_t wi;
            int td;

            /* Let's keep the last 2 over the d-1 identical bits and the
               following bits, i.e. the bits from err+1 to minexp. */
            tq = err - minexp + 2;  /* tq = number of such bits */
            MPFR_LOG_MSG (("[Step 8] tq=%Pd\n", tq));
            MPFR_ASSERTD (tq >= 2);

            wi = tq / GMP_NUMB_BITS;
            td = tq % GMP_NUMB_BITS;

            if (td != 0)
              {
                wi++;  /* number of words with represented bits */
                td = GMP_NUMB_BITS - td;
                zs = ws - wi;
                MPFR_ASSERTD (zs >= 0 && zs < ws);
                mpn_lshift (wp + zs, wp, wi, td);
              }
            else
              {
                MPFR_ASSERTD (wi > 0);
                zs = ws - wi;
                MPFR_ASSERTD (zs >= 0 && zs < ws);
                if (zs > 0)
                  MPN_COPY_INCR (wp + zs, wp, wi);
              }

            UPDATE_MINEXP (minexp, zs * GMP_NUMB_BITS + td);
          }
        else  /* err < minexp */
          {
            /* At least one of the identical bits is not represented,
               meaning that it is 0 and all these bits are 0's. Thus
               the accumulator will be 0. The new minexp is determined
               from maxexp, with cq bits reserved to avoid an overflow
               (as in the early steps). */
            MPFR_LOG_MSG (("[Step 8] err < minexp\n", 0));
            zs = ws;

            /* minexp = maxexp + cq - wq */
            UPDATE_MINEXP (maxexp, wq - cq);
          }

        MPN_ZERO (wp, zs);

        cancel = sum_raw (wp, ws, wq, x, n, minexp, maxexp, tp, ts,
                          logn, cq, 0, NULL, NULL, &maxexp);

        if ((wp[ws-1] & MPFR_LIMB_HIGHBIT) != 0)
          sst = -1;
        else if (maxexp != MPFR_EXP_MIN)
          sst = 1;
        else
          {
            do
              sst = wp[ws] != 0;
            while (sst == 0 && ws-- > 0);
            if (sst == 0 && tmd == 2)
              {
                /* For halfway cases, let's virtually eliminate them
                   by setting a sst equivalent to a non-halfway case,
                   which depends on the last bit of the pre-rounded
                   result and the sign. */
                MPFR_ASSERTD (rnd == MPFR_RNDN);
                sst = (sump[0] & MPFR_LIMB_ONE) ?
                  (pos ? 1 : -1) : (pos ? -1 : 1);
              }
          }

        MPFR_LOG_MSG (("[Step 8] tmd=%d rbit=%d sst=%d\n",
                       tmd, rbit != 0, sst));

        inex =
          MPFR_IS_LIKE_RNDD (rnd, pos ? 1 : -1) ? (sst ? -1 : 0) :
          MPFR_IS_LIKE_RNDU (rnd, pos ? 1 : -1) ? (sst ?  1 : 0) :
          (MPFR_ASSERTD (rnd == MPFR_RNDN),
           tmd == 1 ? - sst : sst);

        /* TODO: possible correction of the value (+/- 1 ulp)... */

      }  /* Step 8 block */

    MPFR_SET_EXP (sum, e);
  }  /* main block */

  MPFR_TMP_FREE (marker);
  return mpfr_check_range (sum, inex, rnd);
}

/**********************************************************************/

int
mpfr_sum (mpfr_ptr sum, mpfr_ptr *const x, unsigned long n, mpfr_rnd_t rnd)
{
  MPFR_LOG_FUNC
    (("n=%lu rnd=%d", n, rnd),
     ("sum[%Pu]=%.*Rg", mpfr_get_prec (sum), mpfr_log_prec, sum));

  if (MPFR_UNLIKELY (n <= 2))
    {
      if (n == 0)
        {
          MPFR_SET_ZERO (sum);
          MPFR_SET_POS (sum);
          MPFR_RET (0);
        }
      else if (n == 1)
        return mpfr_set (sum, x[0], rnd);
      else
        return mpfr_add (sum, x[0], x[1], rnd);
    }
  else
    {
      mpfr_exp_t maxexp = MPFR_EXP_MIN;  /* max(Empty) */
      unsigned long i;
      unsigned long rn = 0;  /* will be the number of regular inputs */
      /* sign of infinities and zeros (0: currently unknown) */
      int sign_inf = 0, sign_zero = 0;

      MPFR_LOG_MSG (("Step 1 with n = %lu >= 3\n", n));

      for (i = 0; i < n; i++)
        {
          if (MPFR_UNLIKELY (MPFR_IS_SINGULAR (x[i])))
            {
              if (MPFR_IS_NAN (x[i]))
                {
                  /* The current value x[i] is NaN. Then the sum is NaN. */
                nan:
                  MPFR_SET_NAN (sum);
                  MPFR_RET_NAN;
                }
              else if (MPFR_IS_INF (x[i]))
                {
                  /* The current value x[i] is an infinity.
                     There are two cases:
                     1. This is the first infinity value (sign_inf == 0).
                        Then set sign_inf to its sign, and go on.
                     2. All the infinities found until now have the same
                        sign sign_inf. If this new infinity has a different
                        sign, then return NaN immediately, else go on. */
                  if (sign_inf == 0)
                    sign_inf = MPFR_SIGN (x[i]);
                  else if (MPFR_SIGN (x[i]) != sign_inf)
                    goto nan;
                }
              else if (MPFR_UNLIKELY (rn == 0))
                {
                  /* The current value x[i] is a zero. The code below matters
                     only when all values found until now are zeros, otherwise
                     it is harmless (the test rn == 0 above is just a minor
                     optimization).
                     Here we track the sign of the zero result when all inputs
                     are zeros: if all zeros have the same sign, the result
                     will have this sign, otherwise (i.e. if there is at least
                     a zero of each sign), the sign of the zero result depends
                     only on the rounding mode (note that this choice is
                     sticky when new zeros are considered). */
                  MPFR_ASSERTD (MPFR_IS_ZERO (x[i]));
                  if (sign_zero == 0)
                    sign_zero = MPFR_SIGN (x[i]);
                  else if (MPFR_SIGN (x[i]) != sign_zero)
                    sign_zero = rnd == MPFR_RNDD ? -1 : 1;
                }
            }
          else
            {
              /* The current value x[i] is a regular number. */
              mpfr_exp_t e = MPFR_GET_EXP (x[i]);
              if (e > maxexp)
                maxexp = e;  /* maximum exponent found until now */
              rn++;  /* current number of regular inputs */
            }
        }

      MPFR_LOG_MSG (("[Step 1] rn=%lu sign_inf=%d sign_zero=%d\n",
                     rn, sign_inf, sign_zero));

      /* At this point the result cannot be NaN (this case has already
         been filtered out). */

      if (MPFR_UNLIKELY (sign_inf != 0))
        {
          /* At least one infinity, and all of them have the same sign
             sign_inf. The sum is the infinity of this sign. */
          MPFR_SET_INF (sum);
          MPFR_SET_SIGN (sum, sign_inf);
          MPFR_RET (0);
        }

      /* At this point, all the inputs are finite numbers. */

      if (MPFR_UNLIKELY (rn == 0))
        {
          /* All the numbers were zeros (and there is at least one).
             The sum is zero with sign sign_zero. */
          MPFR_ASSERTD (sign_zero != 0);
          MPFR_SET_ZERO (sum);
          MPFR_SET_SIGN (sum, sign_zero);
          MPFR_RET (0);
        }

      /* Optimize the case where there are only two regular numbers. */
      if (MPFR_UNLIKELY (rn <= 2))
        {
          unsigned long h = ULONG_MAX;

          for (i = 0; i < n; i++)
            if (! MPFR_IS_SINGULAR (x[i]))
              {
                if (rn == 1)
                  return mpfr_set (sum, x[i], rnd);
                if (h != ULONG_MAX)
                  return mpfr_add (sum, x[h], x[i], rnd);
                h = i;
              }
          MPFR_RET_NEVER_GO_HERE();
        }

      return sum_aux (sum, x, n, rnd, maxexp, rn);
    }
}
