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

/* FIXME[VL]: mpfr_sum can take too much memory and too much time.
   Rewrite using mpn, with additions by blocks (most significant first)?
   The algorithm could be as follows:
   1. In a first pass, just look at the exponent field of each input
      (this is fast). This allows us to detect the singular cases
      (at least a NaN or an Inf in the inputs, or all zeros) and to
      determine the maximum exponent maxexp of the regular inputs.
   2. Compute the truncated sum in two's complement in a window around
      maxexp + log2(N) to maxexp - output_prec - log2(N). The log2(N)
      term for the MSB avoids overflows. The log2(N) term for the LSB
      allows one to take into account the accumulation of errors (i.e.
      from everything less significant than the window LSB); one may
      need an exponent a bit smaller to handle partial cancellation,
      but important cancellation will lead to another iteration.
      At the same time, use the same loop to determine the exponent
      maxexp2 of the most significant bit that has been ignored.
   3. In applicable (see below), add both windows.
   4. Determine the number of cancelled bits.
   5. If the truncated sum is 0, reiterate at (2) with maxexp = maxexp2.
   6. If the error is too large, shift the truncated sum to the left of
      the window, and reiterate at (2) with a second window. Using a
      second window is necessary to avoid carry propagations to the
      full window, as it is expected that in general, the second window
      will be small (small cancellation). The cumulated size of both
      windows should be no more than: output_prec + k * log2(N), where
      k is a small constant.
   7. If only the sign of the error term is unknown, reiterate at (2)
      to compute it using a second window where output_prec = 0, i.e.
      around maxexp + log2(N) to maxexp - log2(N).
      Note: the sign of the error term is needed to round the result in
      the right direction and/or to determine the ternary value.
   8. Copy the rounded result to the destination, and exit with the
      correct ternary value.
   As a bonus, this will also solve overflow, underflow and normalization
   issues, since everything is done in fixed point and the output exponent
   will be considered only at the end (early overflow detection could also
   be done).

Note: see the following paper and its references:
http://www.eecs.berkeley.edu/~hdnguyen/public/papers/ARITH21_Fast_Sum.pdf
VL: This is very different:
          In MPFR             In the paper & references
    arbitrary precision            fixed precision
     correct rounding        just reproducible rounding
    integer operations        floating-point operations
        sequencial             parallel (& sequential)
*/

/* I would really like to use "mpfr_srcptr const []" but the norm is buggy:
   it doesn't automatically cast a "mpfr_ptr []" to "mpfr_srcptr const []"
   if necessary. So the choice are:
     mpfr_s **                : OK
     mpfr_s *const*           : OK
     mpfr_s **const           : OK
     mpfr_s *const*const      : OK
     const mpfr_s *const*     : no
     const mpfr_s **const     : no
     const mpfr_s *const*const: no
   VL: this is not a bug, but a feature. See the reason here:
     http://c-faq.com/ansi/constmismatch.html
*/
static void heap_sort (mpfr_srcptr *const, unsigned long, mpfr_srcptr *);
static void count_sort (mpfr_srcptr *const, unsigned long, mpfr_srcptr *,
                        mpfr_exp_t, mpfr_uexp_t);

/* Either sort the tab in perm and returns 0
   Or returns 1 for +INF, -1 for -INF and 2 for NAN.
   Also set *maxprec to the maximal precision of tab[0..n-1] and of the
   initial value of *maxprec.
*/
int
mpfr_sum_sort (mpfr_srcptr *const tab, unsigned long n, mpfr_srcptr *perm,
               mpfr_prec_t *maxprec)
{
  mpfr_exp_t min, max;
  mpfr_uexp_t exp_num;
  unsigned long i;
  int sign_inf;

  sign_inf = 0;
  min = MPFR_EMIN_MAX;
  max = MPFR_EMAX_MIN;
  for (i = 0; i < n; i++)
    {
      if (MPFR_UNLIKELY (MPFR_IS_SINGULAR (tab[i])))
        {
          if (MPFR_IS_NAN (tab[i]))
            return 2; /* Return NAN code */
          else if (MPFR_IS_INF (tab[i]))
            {
              if (sign_inf == 0) /* No previous INF */
                sign_inf = MPFR_SIGN (tab[i]);
              else if (sign_inf != MPFR_SIGN (tab[i]))
                return 2; /* Return NAN */
            }
        }
      else
        {
          MPFR_ASSERTD (MPFR_IS_PURE_FP (tab[i]));
          if (MPFR_GET_EXP (tab[i]) < min)
            min = MPFR_GET_EXP(tab[i]);
          if (MPFR_GET_EXP (tab[i]) > max)
            max = MPFR_GET_EXP(tab[i]);
        }
      if (MPFR_PREC (tab[i]) > *maxprec)
        *maxprec = MPFR_PREC (tab[i]);
    }
  if (MPFR_UNLIKELY (sign_inf != 0))
    return sign_inf;

  exp_num = max - min + 1;
  /* FIXME : better test */
  if (exp_num > n * MPFR_INT_CEIL_LOG2 (n))
    heap_sort (tab, n, perm);
  else
    count_sort (tab, n, perm, min, exp_num);
  return 0;
}

#define GET_EXP1(x) (MPFR_IS_ZERO (x) ? min : MPFR_GET_EXP (x))
/* Performs a count sort of the entries */
static void
count_sort (mpfr_srcptr *const tab, unsigned long n,
            mpfr_srcptr *perm, mpfr_exp_t min, mpfr_uexp_t exp_num)
{
  unsigned long *account;
  unsigned long target_rank, i;
  MPFR_TMP_DECL(marker);

  /* Reserve a place for potential 0 (with EXP min-1)
     If there is no zero, we only lose one unused entry */
  min--;
  exp_num++;

  /* Performs a counting sort of the entries */
  MPFR_TMP_MARK (marker);
  account = (unsigned long *) MPFR_TMP_ALLOC (exp_num * sizeof *account);
  for (i = 0; i < exp_num; i++)
    account[i] = 0;
  for (i = 0; i < n; i++)
    account[GET_EXP1 (tab[i]) - min]++;
  for (i = exp_num - 1; i >= 1; i--)
    account[i - 1] += account[i];
  for (i = 0; i < n; i++)
    {
      target_rank = --account[GET_EXP1 (tab[i]) - min];
      perm[target_rank] = tab[i];
    }
  MPFR_TMP_FREE (marker);
}


#define GET_EXP2(x) (MPFR_IS_ZERO (x) ? MPFR_EMIN_MIN : MPFR_GET_EXP (x))

/* Performs a heap sort of the entries */
static void
heap_sort (mpfr_srcptr *const tab, unsigned long n, mpfr_srcptr *perm)
{
  unsigned long dernier_traite;
  unsigned long i, pere;
  mpfr_srcptr tmp;
  unsigned long fils_gauche, fils_droit, fils_indigne;
  /* Reminder of a heap structure :
     node(i) has for left son node(2i +1) and right son node(2i)
     and father(node(i)) = node((i - 1) / 2)
  */

  /* initialize the permutation to identity */
  for (i = 0; i < n; i++)
    perm[i] = tab[i];

  /* insertion phase */
  for (dernier_traite = 1; dernier_traite < n; dernier_traite++)
    {
      i = dernier_traite;
      while (i > 0)
        {
          pere = (i - 1) / 2;
          if (GET_EXP2 (perm[pere]) > GET_EXP2 (perm[i]))
            {
              tmp = perm[pere];
              perm[pere] = perm[i];
              perm[i] = tmp;
              i = pere;
            }
          else
            break;
        }
    }

  /* extraction phase */
  for (dernier_traite = n - 1; dernier_traite > 0; dernier_traite--)
    {
      tmp = perm[0];
      perm[0] = perm[dernier_traite];
      perm[dernier_traite] = tmp;

      i = 0;
      while (1)
        {
          fils_gauche = 2 * i + 1;
          fils_droit = fils_gauche + 1;
          if (fils_gauche < dernier_traite)
            {
              if (fils_droit < dernier_traite)
                {
                  if (GET_EXP2(perm[fils_droit]) < GET_EXP2(perm[fils_gauche]))
                    fils_indigne = fils_droit;
                  else
                    fils_indigne = fils_gauche;

                  if (GET_EXP2 (perm[i]) > GET_EXP2 (perm[fils_indigne]))
                    {
                      tmp = perm[i];
                      perm[i] = perm[fils_indigne];
                      perm[fils_indigne] = tmp;
                      i = fils_indigne;
                    }
                  else
                    break;
                }
              else /* on a un fils gauche, pas de fils droit */
                {
                  if (GET_EXP2 (perm[i]) > GET_EXP2 (perm[fils_gauche]))
                    {
                      tmp = perm[i];
                      perm[i] = perm[fils_gauche];
                      perm[fils_gauche] = tmp;
                    }
                  break;
                }
            }
          else /* on n'a pas de fils */
            break;
        }
    }
}


/* Sum a list of float with order given by permutation perm,
 * intermediate size set to F. Return non-zero if at least one of
 * the operations is inexact (thus 0 implies that the sum is exact).
 * Internal use function.
 */
static int
sum_once (mpfr_ptr ret, mpfr_srcptr *const tab, unsigned long n, mpfr_prec_t F)
{
  mpfr_t sum;
  unsigned long i;
  int error_trap;

  MPFR_ASSERTD (n >= 2);

  mpfr_init2 (sum, F);
  error_trap = mpfr_set (sum, tab[0], MPFR_RNDN);
  for (i = 1; i < n - 1; i++)
    {
      MPFR_ASSERTD (!MPFR_IS_NAN (sum) && !MPFR_IS_INF (sum));
      if (mpfr_add (sum, sum, tab[i], MPFR_RNDN))
        error_trap = 1;
    }
  if (mpfr_add (ret, sum, tab[n - 1], MPFR_RNDN))
    error_trap = 1;
  mpfr_clear (sum);
  return error_trap;
}

/* The following function will disappear in the final code. */
static int
mpfr_sum_old (mpfr_ptr ret, mpfr_ptr *const tab_p, unsigned long n, mpfr_rnd_t rnd)
{
  mpfr_t cur_sum;
  mpfr_prec_t prec;
  mpfr_srcptr *perm, *const tab = (mpfr_srcptr *) tab_p;
  int k, error_trap;
  MPFR_ZIV_DECL (loop);
  MPFR_SAVE_EXPO_DECL (expo);
  MPFR_TMP_DECL (marker);

  if (MPFR_UNLIKELY (n <= 1))
    {
      if (n < 1)
        {
          MPFR_SET_ZERO (ret);
          MPFR_SET_POS (ret);
          return 0;
        }
      else
        return mpfr_set (ret, tab[0], rnd);
    }

  /* Sort and treat special cases */
  MPFR_TMP_MARK (marker);
  perm = (mpfr_srcptr *) MPFR_TMP_ALLOC (n * sizeof *perm);
  prec = MPFR_PREC (ret);
  error_trap = mpfr_sum_sort (tab, n, perm, &prec);
  /* Check if there was a NAN or a INF */
  if (MPFR_UNLIKELY (error_trap != 0))
    {
      MPFR_TMP_FREE (marker);
      if (error_trap == 2)
        {
          MPFR_SET_NAN (ret);
          MPFR_RET_NAN;
        }
      MPFR_SET_INF (ret);
      MPFR_SET_SIGN (ret, error_trap);
      MPFR_RET (0);
    }

  /* Initial precision is max(prec(ret),prec(tab[0]),...,prec(tab[n-1])) */
  k = MPFR_INT_CEIL_LOG2 (n) + 1;
  prec += k + 2;
  mpfr_init2 (cur_sum, prec);

  /* Ziv Loop */
  MPFR_SAVE_EXPO_MARK (expo);
  MPFR_ZIV_INIT (loop, prec);
  for (;;)
    {
      error_trap = sum_once (cur_sum, perm, n, prec + k);
      if (MPFR_LIKELY (error_trap == 0 ||
                       (!MPFR_IS_ZERO (cur_sum) &&
                        mpfr_can_round (cur_sum, prec - 2,
                                        MPFR_RNDN, rnd, MPFR_PREC (ret)))))
        break;
      MPFR_ZIV_NEXT (loop, prec);
      mpfr_set_prec (cur_sum, prec);
    }
  MPFR_ZIV_FREE (loop);
  MPFR_TMP_FREE (marker);

  if (mpfr_set (ret, cur_sum, rnd))
    error_trap = 1;
  mpfr_clear (cur_sum);

  MPFR_SAVE_EXPO_FREE (expo);
  if (mpfr_check_range (ret, 0, rnd))
    error_trap = 1;
  return error_trap; /* It doesn't return the ternary value */
}

int
mpfr_sum (mpfr_ptr sum, mpfr_ptr *const p, unsigned long n, mpfr_rnd_t rnd)
{
  MPFR_LOG_FUNC
    (("n=%lu rnd=%d", n, rnd),
     ("sum[%Pu]=%.*Rg", mpfr_get_prec (sum), mpfr_log_prec, sum));

  if (MPFR_UNLIKELY (n <= 1))
    {
      if (n < 1)
        {
          MPFR_SET_ZERO (sum);
          MPFR_SET_POS (sum);
          MPFR_RET (0);
        }
      else
        return mpfr_set (sum, p[0], rnd);
    }
  else
    {
      mpfr_exp_t maxexp = MPFR_EXP_MIN;  /* not a valid exponent */
      int sign_inf = 0, sign_zero = 0;
      unsigned long i, rn = 0;
      int logn;       /* ceil(log2(rn)) */
      mp_limb_t *wp;  /* pointer to the window */
      mp_limb_t *tp;  /* pointer to a temporary area (same size + 1) */
      mp_size_t wn;   /* size of the window */
      mpfr_prec_t wq; /* precision of the window */
      MPFR_TMP_DECL (marker);

      for (i = 0; i < n; i++)
        {
          if (MPFR_UNLIKELY (MPFR_IS_SINGULAR (p[i])))
            {
              if (MPFR_IS_NAN (p[i]))
                {
                nan:
                  MPFR_SET_NAN (sum);
                  MPFR_RET_NAN;
                }
              else if (MPFR_IS_INF (p[i]))
                {
                  if (!sign_inf)
                    sign_inf = MPFR_SIGN (p[i]);
                  else if (MPFR_SIGN (p[i]) != sign_inf)
                    goto nan;
                }
              else
                {
                  MPFR_ASSERTD (MPFR_IS_ZERO (p[i]));
                  if (!sign_zero)
                    sign_zero = MPFR_SIGN (p[i]);
                  else if (MPFR_SIGN (p[i]) != sign_zero)
                    sign_zero = rnd == MPFR_RNDD ? -1 : 1;
                }
            }
          else
            {
              mpfr_exp_t e = MPFR_GET_EXP (p[i]);
              if (e > maxexp)
                maxexp = e;
              rn++;
            }
        }

      MPFR_LOG_MSG (("rn=%lu sign_inf=%d sign_zero=%d\n",
                     rn, sign_inf, sign_zero));

      if (MPFR_UNLIKELY (sign_inf))
        {
          /* At least one infinity. And all of them have the same sign.
             The result is the infinity of this sign. */
          MPFR_SET_INF (sum);
          MPFR_SET_SIGN (sum, sign_inf);
          MPFR_RET (0);
        }

      if (MPFR_UNLIKELY (rn == 0))
        {
          /* All the numbers were zeros (and there is at least one). */
          MPFR_ASSERTD (sign_zero != 0);
          MPFR_SET_ZERO (sum);
          MPFR_SET_SIGN (sum, sign_zero);
          MPFR_RET (0);
        }

      /* rn is the number of regular numbers (the singular numbers
         will be ignored). Compute logn = ceil(log2(rn)). */
      logn = MPFR_INT_CEIL_LOG2 (rn);

      MPFR_LOG_MSG (("logn=%d maxexp=%" MPFR_EXP_FSPEC "d\n",
                     logn, (mpfr_eexp_t) maxexp));

      MPFR_TMP_MARK (marker);

      /* Determine the window size wn and allocate the corresponding memory.
         One logn is for the potential carries, the other one is due to the
         approximations (ignored limbs of each number).
         TODO: We may want to add some margin (a small constant). Check
         whether this would be useful in practical cases. */
      wn = (MPFR_GET_PREC (sum) + 2 * logn) / GMP_NUMB_BITS + 1;
      wp = MPFR_TMP_LIMBS_ALLOC (2 * wn + 1);
      wq = wn * GMP_NUMB_BITS;
      tp = wp + wn;

      /* Initial truncated sum: 0 */
      MPN_ZERO (wp, wn);

      do
        {
          mpfr_exp_t rexp;
          mpfr_prec_t signif;
          mp_size_t wi;
          int neg;

          /* We will consider only the bits of exponent < maxexp of each
             input number. Thus the sum S will satisfy:
               |S| < rn * 2^maxexp <= 2^(maxexp+logn)
             So, computing the partial sum modulo 2^(maxexp+logn+1), with
             a two's complement representation, is sufficient to retrieve
             the corresponding truncated sum. Thus the most significant bit
             of the window will have the exponent maxexp+logn, and it will
             be the sign of the truncated result. And we will ignore the
             bits of exponent <= maxexp + logn - wq. */
          rexp = maxexp + logn - wq;

          for (i = 0; i < n; i++)
            if (! MPFR_IS_SINGULAR (p[i]))
              {
                mp_limb_t *vp;
                mp_size_t vn;
                mpfr_exp_t vsh;

                vp = MPFR_MANT (p[i]);
                vn = MPFR_PREC2LIMBS (MPFR_GET_PREC (p[i]));
                vsh = MPFR_GET_EXP (p[i]) - vn * GMP_NUMB_BITS - rexp;
                /* vsh is the exponent of the least significant represented
                   bit of p[i] (including the trailing bits at 0) minus the
                   exponent of the least significant bit of the window. */

                if (vsh <= 0)
                  {
                    mp_size_t shn;

                    /* We need to ignore the least |vsh| significant bits
                       of p[i]. First, let's ignore the least
                       shn = |vsh| / GMP_NUMB_BITS limbs. */
                    vsh = - vsh;
                    shn = vsh / GMP_NUMB_BITS;
                    vn -= shn;
                    if (vn <= 0)  /* No overlapping between p[i] */
                      continue;   /* and the window.             */
                    vp += shn;
                    vsh -= shn * GMP_NUMB_BITS;
                    MPFR_ASSERTD (vsh >= 0 && vsh < GMP_NUMB_BITS);

                    if (vsh != 0)
                      {
                        mpn_rshift (tp, vp, vn > wn ? (vn = wn, wn + 1) : vn,
                                    vsh);
                        vp = tp;
                      }
                    else if (vn > wn)
                      vn = wn;
                    MPFR_ASSERTD (vn <= wn);

                    if (MPFR_IS_POS (p[i]))
                      {
                        mp_limb_t carry;

                        carry = mpn_add_n (wp, wp, vp, vn);
                        if (carry != 0 && vn < wn)
                          mpn_add_1 (wp + vn, wp + vn, wn - vn, carry);
                      }
                    else
                      {
                        mp_limb_t borrow;

                        borrow = mpn_sub_n (wp, wp, vp, vn);
                        if (borrow != 0 && vn < wn)
                          mpn_sub_1 (wp + vn, wp + vn, wn - vn, borrow);
                      }
                  }
                else
                  {
                    mp_limb_t *dp;
                    mp_size_t dn, shn;

                    /* We need to ignore the least vsh significant bits
                       of the window. First, let's ignore the least
                       shn = vsh / GMP_NUMB_BITS limbs. -> (dp,dn) */
                    shn = vsh / GMP_NUMB_BITS;
                    dn = wn - shn;
                    if (dn <= 0)
                      continue;
                    dp = wp + shn;
                    vsh -= shn * GMP_NUMB_BITS;
                    MPFR_ASSERTD (vsh >= 0 && vsh < GMP_NUMB_BITS);

                    if (vn > dn)
                      vn = dn;
                    if (vsh != 0)
                      {
                        mpn_lshift (tp, vp, vn, vsh);
                        vp = tp;
                      }

                    if (MPFR_IS_POS (p[i]))
                      {
                        mp_limb_t carry;

                        carry = mpn_add_n (dp, dp, vp, vn);
                        if (carry != 0 && vn < dn)
                          mpn_add_1 (dp + vn, dp + vn, dn - vn, carry);
                      }
                    else
                      {
                        mp_limb_t borrow;

                        borrow = mpn_sub_n (dp, dp, vp, vn);
                        if (borrow != 0 && vn < dn)
                          mpn_sub_1 (dp + vn, dp + vn, dn - vn, borrow);
                      }
                  }
              }

          /* The truncated sum has been computed. Let's determine its
             sign, absolute value, and the number of significant bits
             (starting from the most significant 1).
             Note: the mpn_neg may be useless, but it doesn't waste
             much time, it's simpler and it makes the code shorter
             than analyzing different cases. */
          wi = wn - 1;
          neg = MPFR_LIMB_MSB (wp[wi]) != 0;
          /* FIXME: Do not do the mpn_neg inside the loop since in
             case of another iteration, we want to keep the number
             in the same representation, but shifted to the left of
             the window. */
          if (neg)
            mpn_neg (wp, wp, wn);
          signif = wq;
          for (; wi >= 0; wi--)
            {
              if (wp[wi] != 0)
                {
                  int cnt;
                  count_leading_zeros (cnt, wp[wi]);
                  signif -= cnt;
                  break;
                }
              signif -= GMP_NUMB_BITS;
            }
          MPFR_LOG_MSG (("neg=%d signif=%Pu\n", neg, signif));

          if (MPFR_UNLIKELY (signif == 0))
            {
              /* There may be a big hole between numbers! We need to
                 determine a new maximum exponent among the numbers
                 with at least a represented bit of exponent <= rexp.
                 Note that there may be no such numbers, in which case
                 the exact sum is 0. */

            }

        }
      while (0); /* the condition will be determined later */

      MPFR_TMP_FREE (marker);

      /* ... */

      return mpfr_sum_old (sum, p, n, rnd);
    }
}
