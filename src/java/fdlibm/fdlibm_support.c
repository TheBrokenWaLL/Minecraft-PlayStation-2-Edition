/* Minimal support functions required by the bundled fdlibm 5.3 routines.
 * fabs/floor/scalbn are exact elementary operations for finite IEEE-754
 * doubles; keeping them here avoids routing transcendental functions through
 * the platform libm.
 *
 * Include <math.h> first: fdlibm.h defines the legacy HUGE/MAXFLOAT macros,
 * which would corrupt the system header's declarations parsed afterwards -
 * MSVC's UCRT <math.h> declares a HUGE variable that the macro would rewrite
 * into a parenthesized constant. */
#include <math.h>
#include "fdlibm.h"

double ieee_fabs(double x) { return fabs(x); }
double ieee_floor(double x) { return floor(x); }
double ieee_scalbn(double x, int n) { return scalbn(x, n); }
extern double __ieee754_sqrt(double);
double ieee_sqrt(double x) { return __ieee754_sqrt(x); }
