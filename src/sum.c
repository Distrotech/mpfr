/* Sum -- efficiently sum a list of floating-point numbers

Copyright 2004-2014 Free Software Foundation, Inc.
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

/* See the sum.txt file for the algorithm and a part of its proof
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

int
mpfr_sum (mpfr_ptr sum, mpfr_ptr *const p, unsigned long n, mpfr_rnd_t rnd)
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
        return mpfr_set (sum, p[0], rnd);
      else
        return mpfr_add (sum, p[0], p[1], rnd);
    }
  else
    {
      mp_limb_t *tp;  /* pointer to a temporary area */
      mp_limb_t *wp;  /* pointer to the accumulator */
      mp_size_t ts;   /* size of the temporary area, in limbs */
      mp_size_t ws;   /* size of the accumulator, in limbs */
      mpfr_prec_t wq; /* size of the accumulator, in bits */
      mpfr_exp_t maxexp;
      unsigned long i, rn;
      int logn;       /* ceil(log2(rn)) */
      int cq, cq0;
      mpfr_prec_t sq;
      int inex;
      MPFR_TMP_DECL (marker);

      /* Pre-iteration (Step 1) */
      {
        /* sign of infinities and zeros (0: currently unknown) */
        int sign_inf = 0, sign_zero = 0;

        rn = 0;  /* will be the number of regular inputs */
        maxexp = MPFR_EXP_MIN;  /* max(Empty), <= any valid exponent */
        for (i = 0; i < n; i++)
          {
            if (MPFR_UNLIKELY (MPFR_IS_SINGULAR (p[i])))
              {
                if (MPFR_IS_NAN (p[i]))
                  {
                    /* The current value p[i] is NaN. Then the sum is NaN. */
                  nan:
                    MPFR_SET_NAN (sum);
                    MPFR_RET_NAN;
                  }
                else if (MPFR_IS_INF (p[i]))
                  {
                    /* The current value p[i] is an infinity.
                       There are two cases:
                       1. This is the first infinity value (sign_inf == 0).
                          Then set sign_inf to its sign, and go on.
                       2. All the infinities found until now have the same
                          sign sign_inf. If this new infinity has a different
                          sign, then return NaN immediately, else go on. */
                    if (sign_inf == 0)
                      sign_inf = MPFR_SIGN (p[i]);
                    else if (MPFR_SIGN (p[i]) != sign_inf)
                      goto nan;
                  }
                else if (MPFR_UNLIKELY (rn == 0))
                  {
                    /* The current value p[i] is a zero. The code below
                       matters only when all values found until now are
                       zeros, otherwise it is harmless (the test rn == 0
                       above is just a minor optimization).
                       Here we track the sign of the zero result when all
                       inputs are zeros: if all zeros have the same sign,
                       the result will have this sign, otherwise (i.e. if
                       there is at least a zero of each sign), the sign of
                       the zero result depends only on the rounding mode
                       (note that this choice is sticky when new zeros are
                       considered). */
                    MPFR_ASSERTD (MPFR_IS_ZERO (p[i]));
                    if (sign_zero == 0)
                      sign_zero = MPFR_SIGN (p[i]);
                    else if (MPFR_SIGN (p[i]) != sign_zero)
                      sign_zero = rnd == MPFR_RNDD ? -1 : 1;
                  }
              }
            else
              {
                /* The current value p[i] is a regular number. */
                mpfr_exp_t e = MPFR_GET_EXP (p[i]);
                if (e > maxexp)
                  maxexp = e;  /* maximum exponent found until now */
                rn++;  /* current number of regular inputs */
              }
          }

        MPFR_LOG_MSG (("rn=%lu sign_inf=%d sign_zero=%d\n",
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

      } /* End of the pre-iteration (Step 1) */

      if (MPFR_UNLIKELY (rn <= 2))
        {
          unsigned long h = ULONG_MAX;

          for (i = 0; i < n; i++)
            if (! MPFR_IS_SINGULAR (p[i]))
              {
                if (rn == 1)
                  return mpfr_set (sum, p[i], rnd);
                if (h != ULONG_MAX)
                  return mpfr_add (sum, p[h], p[i], rnd);
                h = i;
              }
        }

      /* Generic case: all the inputs are finite numbers, with at least
         3 regular numbers. */

      /* Step 2: set up some variables and the accumulator. */

      /* rn is the number of regular inputs (the singular ones will be
         ignored). Compute logn = ceil(log2(rn)). */
      logn = MPFR_INT_CEIL_LOG2 (rn);
      MPFR_ASSERTD (logn >= 2);

      MPFR_LOG_MSG (("logn=%d maxexp=%" MPFR_EXP_FSPEC "d\n",
                     logn, (mpfr_eexp_t) maxexp));

      sq = MPFR_GET_PREC (sum);
      cq = logn + 1;
      cq0 = cq;

      /* First determine the size of the accumulator. */
      ws = MPFR_PREC2LIMBS (cq + sq + logn + 2);
      wq = (mpfr_prec_t) ws * GMP_NUMB_BITS;
      MPFR_ASSERTD (wq - cq - sq >= 4);

      /* An input block will have up to wq - cq bits, and its shifted
         value (to be correctly aligned) may take GMP_NUMB_BITS - 1
         additional bits. */
      ts = MPFR_PREC2LIMBS (wq - cq + GMP_NUMB_BITS - 1);

      MPFR_TMP_MARK (marker);

      /* TODO: one may need a bit more memory later for Step 6.
         Should it be allocated here? */
      tp = MPFR_TMP_LIMBS_ALLOC (ts + ws);
      wp = tp + ts;

      MPN_ZERO (wp, ws);  /* zero the accumulator */

      while (1)
        {
          mpfr_exp_t minexp = maxexp + cq - wq;
          mpfr_exp_t maxexp2 = MPFR_EXP_MIN;  /* < any valid exponent */

          /* Step 3: compute the truncated sum. */

          MPFR_LOG_MSG (("Step 3 with"
                         " maxexp=%" MPFR_EXP_FSPEC "d"
                         " minexp=%" MPFR_EXP_FSPEC "d\n",
                         (mpfr_eexp_t) maxexp,
                         (mpfr_eexp_t) minexp));

          for (i = 0; i < n; i++)
            if (! MPFR_IS_SINGULAR (p[i]))
              {
                mp_limb_t *vp;
                mp_size_t vs;
                mpfr_exp_t pe, vd;
                mpfr_prec_t pq;

                pe = MPFR_GET_EXP (p[i]);
                pq = MPFR_GET_PREC (p[i]);

                vp = MPFR_MANT (p[i]);
                vs = MPFR_PREC2LIMBS (pq);
                vd = pe - vs * GMP_NUMB_BITS - minexp;
                /* vd is the exponent of the least significant represented
                   bit of p[i] (including the trailing bits, whose value
                   is 0) minus the exponent of the least significant bit
                   of the accumulator. */

                if (vd < 0)
                  {
                    mp_size_t vds;
                    int tr;

                    /* This covers the following cases:
                     *     [-+- accumulator ---]
                     *   [---|----- p[i] ------|--]
                     *       |   [----- p[i] --|--]
                     *       |                 |[----- p[i] -----]
                     *       |                 |    [----- p[i] -----]
                     *     maxexp           minexp
                     */

                    if (pe <= minexp)
                      {
                        /* p[i] is entirely after the LSB of the accumulator,
                           so that it will be ignored at this iteration. */
                        if (pe > maxexp2)
                          maxexp2 = pe;
                        continue;
                      }

                    /* If some significant bits of p[i] are after the LSB
                       of the accumulator, then maxexp2 will necessarily
                       be minexp. */
                    if (MPFR_LIKELY (pe - pq < minexp))
                      maxexp2 = minexp;

                    /* We need to ignore the least |vd| significant bits
                       of p[i]. First, let's ignore the least
                       vds = |vd| / GMP_NUMB_BITS limbs. */
                    vd = - vd;
                    vds = vd / GMP_NUMB_BITS;
                    vs -= vds;
                    MPFR_ASSERTD (vs > 0);  /* see pe <= minexp test above */
                    vp += vds;
                    vd -= vds * GMP_NUMB_BITS;
                    MPFR_ASSERTD (vd >= 0 && vd < GMP_NUMB_BITS);

                    if (pe > maxexp)
                      {
                        vs -= (pe - maxexp) / GMP_NUMB_BITS;
                        tr = (pe - maxexp) % GMP_NUMB_BITS;
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
                            tp[vs-1] &=
                              MPFR_LIMB_MASK (GMP_NUMB_BITS - tr);
                            tr = 0;
                          }
                        /* Truncation has now been taken into account. */
                        MPFR_ASSERTD (tr == 0);
                      }

                    MPFR_ASSERTD (vs <= ws);

                    if (tr != 0)
                      {
                        /* We can't truncate the most significant limb of
                           the input. So, let's ignore it now. It will be
                           taken into account after the addition. */
                        vs--;
                      }

                    if (MPFR_IS_POS (p[i]))
                      {
                        mp_limb_t carry;

                        carry = mpn_add_n (wp, wp, vp, vs);
                        if (tr != 0)
                          carry += vp[vs] &
                            MPFR_LIMB_MASK (GMP_NUMB_BITS - tr);
                        if (carry != 0 && vs < ws)
                          mpn_add_1 (wp + vs, wp + vs, ws - vs, carry);
                      }
                    else
                      {
                        mp_limb_t borrow;

                        borrow = mpn_sub_n (wp, wp, vp, vs);
                        if (tr != 0)
                          borrow += vp[vs] &
                            MPFR_LIMB_MASK (GMP_NUMB_BITS - tr);
                        if (borrow != 0 && vs < ws)
                          mpn_sub_1 (wp + vs, wp + vs, ws - vs, borrow);
                      }
                  }
                else  /* vd >= 0 */
                  {
                    mp_limb_t *dp;
                    mp_size_t ds, vds;
                    int tr;

                    /* This covers the following cases:
                     *               [-+- accumulator ---]
                     *   [- p[i] -]    |
                     *             [---|-- p[i] ------]  |
                     *          [------|-- p[i] ---------]
                     *                 |   [- p[i] -]    |
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

                    if (pe > maxexp)
                      {
                        vs -= (pe - maxexp) / GMP_NUMB_BITS;
                        tr = (pe - maxexp) % GMP_NUMB_BITS;
                        if (tr > vd || (vd != 0 && tr == vd))
                          {
                            vs--;
                            tr -= GMP_NUMB_BITS;
                          }
                      }
                    else
                      tr = 0;
                    MPFR_ASSERTD (tr >= 1 - GMP_NUMB_BITS && tr <= vd);

                    if (vd != 0)
                      {
                        MPFR_ASSERTD (vs + 1 <= ts);
                        tp[vs] = mpn_lshift (tp, vp, vs, vd);
                        MPFR_ASSERTD (vd - tr > 0);
                        if (vd - tr < GMP_NUMB_BITS)
                          tp[vs] &= MPFR_LIMB_MASK (vd - tr);
                        vp = tp;
                        tr = 0;
                      }

                    if (MPFR_IS_POS (p[i]))
                      {
                        mp_limb_t carry;

                        carry = mpn_add_n (dp, dp, vp, vs);
                        if (tr != 0)
                          carry += vp[vs] & MPFR_LIMB_MASK (- tr);
                        if (carry != 0 && vs < ds)
                          mpn_add_1 (dp + vs, dp + vs, ds - vs, carry);
                      }
                    else
                      {
                        mp_limb_t borrow;

                        borrow = mpn_sub_n (dp, dp, vp, vs);
                        if (tr != 0)
                          borrow += vp[vs] & MPFR_LIMB_MASK (- tr);
                        if (borrow != 0 && vs < ds)
                          mpn_sub_1 (dp + vs, dp + vs, ds - vs, borrow);
                      }
                  }
              }  /* end of the iteration (Step 3) */

          {
            mpfr_prec_t cancel;  /* number of cancelled bits */
            mp_size_t wi;        /* index in the accumulator */
            mp_limb_t msl;       /* most significant limb */
            mpfr_exp_t e;        /* temporary exponent of the result */
            mpfr_exp_t q;        /* temporary quantum (ulp) exponent */
            mpfr_exp_t err;      /* exponent of the error bound */

            /* Step 4: determine the number of cancelled bits. */

            cancel = 0;
            wi = ws - 1;
            MPFR_ASSERTD (wi >= 0);
            msl = wp[wi];

            MPFR_LOG_MSG (("Step 4 with msl=%Mx\n", msl));

            /* Limbs whose bits are identical (000...00 or 111...11). */
            if (MPFR_UNLIKELY (msl == MPFR_LIMB_ZERO || msl == MPFR_LIMB_MAX))
              {
                while (wi >= 0 && wp[wi] == msl)
                  {
                    cancel += GMP_NUMB_BITS;
                    wi--;
                  }

                if (wi < 0 && msl == MPFR_LIMB_ZERO)
                  {
                    /* Step 5: the truncated sum is zero. Reiterate with
                     * maxexp = maxexp2. Note: we do not need to zero the
                     * accumulator since it is already 0 in this case.
                     */
                    MPFR_LOG_MSG (("Step 5 (truncated sum = 0) with"
                                   " maxexp=%" MPFR_EXP_FSPEC "d\n",
                                   (mpfr_eexp_t) maxexp));
                    maxexp = maxexp2;
                    cq = cq0;
                    continue;
                  }
              }

            /* Let's count the number of identical leading bits of
               the next limb, if there is one. */
            if (MPFR_LIKELY (wi >= 0))
              {
                int cnt;

                msl = wp[wi];
                if (msl & MPFR_LIMB_HIGHBIT)
                  msl ^= MPFR_LIMB_MAX;
                count_leading_zeros (cnt, msl);
                cancel += cnt;
              }

            /* Step 6 */

            e = maxexp + cq - cancel;
            q = e - sq;
            err = maxexp2 + logn;

            MPFR_LOG_MSG (("Step 6 with cancel=%Pd"
                           " e=%" MPFR_EXP_FSPEC "d"
                           " q=%" MPFR_EXP_FSPEC "d"
                           " err=%" MPFR_EXP_FSPEC "d\n",
                           cancel, (mpfr_eexp_t) e, (mpfr_eexp_t) q,
                           (mpfr_eexp_t) err));

            /* The absolute value of the truncated sum is in the binade
               [2^(e-1),2^e] (closed on both ends due to two's complement).
               The error is strictly less than 2^err. */

            if (err > q - 3)
              {
                mpfr_exp_t diffexp;
                mpfr_prec_t shiftq;
                mpfr_size_t shifts;
                int shiftc;

                diffexp = maxexp2 + logn - e;
                if (diffexp < 0)
                  diffexp = 0;
                MPFR_ASSERTD (diffexp <= cancel - 2);
                shiftq = cancel - 2 - (mpfr_prec_t) diffexp;
                MPFR_ASSERTD (shiftq >= 0);
                shifts = shiftq / GMP_NUMB_BITS;
                shiftc = shiftq % GMP_NUMB_BITS;
                MPFR_LOG_MSG (("shiftq = %Pd = %Pd * GMP_NUMB_BITS + %d\n",
                               shiftq, (mpfr_prec_t) shifts, shiftc));
                if (MPFR_LIKELY (shiftc != 0))
                  mpn_lshift (wp + shifts, wp, ws - shifts, shiftc);
                else
                  MPN_COPY_DECR (wp + shifts, wp, ws - shifts);
                MPN_ZERO (wp, shifts);
                maxexp = maxexp2;
                MPFR_ASSERTD (minexp - maxexp2 < shiftq);
                /* therefore minexp - maxexp2 fits in mpfr_prec_t */
                cq = wq - shiftq + (mpfr_prec_t) (minexp - maxexp2);
                MPFR_ASSERTD (cq < wq);
                continue;
              }

          }
        }

      MPFR_TMP_FREE (marker);
      return mpfr_check_range (sum, inex, rnd);
    }
}
