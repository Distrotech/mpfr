/* Test file for mpfr_sinh_cosh.

Copyright 2007, 2008 Free Software Foundation, Inc.
Contributed by the Arenaire and Cacao projects, INRIA.

This file is part of the MPFR Library.

The MPFR Library is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or (at your
option) any later version.

The MPFR Library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
License for more details.

You should have received a copy of the GNU Lesser General Public License
along with the MPFR Library; see the file COPYING.LIB.  If not, write to
the Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
MA 02110-1301, USA. */

#include <stdio.h>
#include <stdlib.h>

#include "mpfr-test.h"

#if MPFR_VERSION >= MPFR_VERSION_NUM(2,4,0)

static void
failed (mpfr_t x, mpfr_t esh, mpfr_t gsh, mpfr_t ech, mpfr_t gch)
{
  printf ("error : mpfr_sinh_cosh (x) x = ");
  mpfr_out_str (stdout, 10, 0, x, GMP_RNDD);
  printf ("\nsinh(x) expected ");
  mpfr_out_str (stdout, 10, 0, esh, GMP_RNDD);
  printf ("\n        got ");
  mpfr_out_str (stdout, 10, 0, gsh, GMP_RNDD);
  printf ("\ncosh(x) expected ");
  mpfr_out_str (stdout, 10, 0, ech, GMP_RNDD);
  printf ("\n        got ");
  mpfr_out_str (stdout, 10, 0, gch, GMP_RNDD);
  putchar ('\n');

  mpfr_clears (x, esh, gsh, ech, gch, (void *) 0);
  exit (1);
}

/* check against sinh, cosh */
static void
check (mpfr_t x, mp_rnd_t rnd)
{
  mpfr_t s, c, sx, cx;
  int isc, is, ic;

  mpfr_inits2 (MPFR_PREC(x), s, c, sx, cx, (void *)0);

  isc = mpfr_sinh_cosh (sx, cx, x, rnd);
  is = mpfr_sinh (s, x, rnd);
  ic = mpfr_cosh (c, x, rnd);

  if (!mpfr_equal_p (s, sx) || !mpfr_equal_p (c, cx))
    failed (x, s, sx, c, cx);
  MPFR_ASSERTN (isc = is || ic);

  mpfr_clears (s, c, sx, cx, (void *) 0);
}

static void
check_nans (void)
{
  mpfr_t  x, sh, ch;

  mpfr_init2 (x, 123);
  mpfr_init2 (sh, 123);
  mpfr_init2 (ch, 123);

  /* nan */
  mpfr_set_nan (x);
  mpfr_sinh_cosh (sh, ch, x, GMP_RNDN);
  MPFR_ASSERTN (mpfr_nan_p (sh));
  MPFR_ASSERTN (mpfr_nan_p (ch));

  /* +inf */
  mpfr_set_inf (x, 1);
  mpfr_sinh_cosh (sh, ch, x, GMP_RNDN);
  MPFR_ASSERTN (mpfr_inf_p (sh));
  MPFR_ASSERTN (mpfr_sgn (sh) > 0);
  MPFR_ASSERTN (mpfr_inf_p (ch));
  MPFR_ASSERTN (mpfr_sgn (ch) > 0);

  /* -inf */
  mpfr_set_inf (x, -1);
  mpfr_sinh_cosh (sh, ch, x, GMP_RNDN);
  MPFR_ASSERTN (mpfr_inf_p (sh));
  MPFR_ASSERTN (mpfr_sgn (sh) < 0);
  MPFR_ASSERTN (mpfr_inf_p (ch));
  MPFR_ASSERTN (mpfr_sgn (ch) > 0);

  mpfr_clear (x);
  mpfr_clear (sh);
  mpfr_clear (ch);
}

int
main (int argc, char *argv[])
{
  int i;
  mpfr_t x;

  tests_start_mpfr ();

  check_nans ();

  /* check against values given by sinh(x), cosh(x) */
  mpfr_init2 (x, 53);
  mpfr_set_str (x, "FEDCBA987654321p-48", 16, GMP_RNDN);
  for (i = 0; i < 10; ++i)
    {
      /* x = i - x / 2 : boggle sign and bits */
      mpfr_ui_sub (x, i, x, GMP_RNDD);
      mpfr_div_2ui (x, x, 2, GMP_RNDD);

      check (x, GMP_RNDN);
      check (x, GMP_RNDU);
      check (x, GMP_RNDD);
    }
  mpfr_clear (x);

  tests_end_mpfr ();
  return 0;
}

#else

int
main (void)
{
  printf ("Warning! Test disabled for this MPFR version.\n");
  return 0;
}

#endif
