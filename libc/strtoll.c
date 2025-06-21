/*

@deftypefn Supplemental {long long int} strtoll (const char *@var{string}, @
  char **@var{endptr}, int @var{base})
@deftypefnx Supplemental {unsigned long long int} strtoul (@
  const char *@var{string}, char **@var{endptr}, int @var{base})

The @code{strtoll} function converts the string in @var{string} to a
long long integer value according to the given @var{base}, which must be
between 2 and 36 inclusive, or be the special value 0.  If @var{base}
is 0, @code{strtoll} will look for the prefixes @code{0} and @code{0x}
to indicate bases 8 and 16, respectively, else default to base 10.
When the base is 16 (either explicitly or implicitly), a prefix of
@code{0x} is allowed.  The handling of @var{endptr} is as that of
@code{strtod} above.  The @code{strtoull} function is the same, except
that the converted value is unsigned.

@end deftypefn

*/

__extension__
typedef unsigned long long ullong_type;

__extension__
typedef long long llong_type;

/* FIXME: It'd be nice to configure around these, but the include files are too
   painful.  These macros should at least be more portable than hardwired hex
   constants. */

#ifndef ULLONG_MAX
#define ULLONG_MAX (~(ullong_type)0) /* 0xFFFFFFFFFFFFFFFF */
#endif

#ifndef LLONG_MAX
#define LLONG_MAX ((llong_type)(ULLONG_MAX >> 1)) /* 0x7FFFFFFFFFFFFFFF */
#endif

#ifndef LLONG_MIN
#define LLONG_MIN (~LLONG_MAX) /* 0x8000000000000000 */
#endif

/*
 * Convert a string to a long long integer.
 *
 * Ignores `locale' stuff.  Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 */
llong_type
strtoll(const char *nptr, char **endptr, register int base)
{
	register const char *s = nptr;
	register ullong_type acc;
	register int c;
	register ullong_type cutoff;
	register int neg = 0, any, cutlim;

	/*
	 * Skip white space and pick up leading +/- sign if any.
	 * If base is 0, allow 0x for hex and 0 for octal, else
	 * assume decimal; if base is already 16, allow 0x.
	 */
	do {
		c = *s++;
	} while (isspace(c));
	if (c == '-') {
		neg = 1;
		c = *s++;
	} else if (c == '+')
		c = *s++;
	if ((base == 0 || base == 16) &&
	    c == '0' && (*s == 'x' || *s == 'X')) {
		c = s[1];
		s += 2;
		base = 16;
	}
	if (base == 0)
		base = c == '0' ? 8 : 10;

	/*
	 * Compute the cutoff value between legal numbers and illegal
	 * numbers.  That is the largest legal value, divided by the
	 * base.  An input number that is greater than this value, if
	 * followed by a legal input character, is too big.  One that
	 * is equal to this value may be valid or not; the limit
	 * between valid and invalid numbers is then based on the last
	 * digit.  For instance, if the range for longs is
	 * [-2147483648..2147483647] and the input base is 10,
	 * cutoff will be set to 214748364 and cutlim to either
	 * 7 (neg==0) or 8 (neg==1), meaning that if we have accumulated
	 * a value > 214748364, or equal but the next digit is > 7 (or 8),
	 * the number is too big, and we will return a range error.
	 *
	 * Set any if any `digits' consumed; make it negative to indicate
	 * overflow.
	 */
	cutoff = neg ? -(ullong_type)LLONG_MIN : LLONG_MAX;
	cutlim = cutoff % (ullong_type)base;
	cutoff /= (ullong_type)base;
	for (acc = 0, any = 0;; c = *s++) {
		if (isdigit(c))
			c -= '0';
		else if (isxdigit(c))
			c -= isupper(c) ? 'A' - 10 : 'a' - 10;
		else
			break;

		if (c >= base)
			break;
		if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
			any = -1;
		else {
			any = 1;
			acc *= base;
			acc += c;
		}
	}
	if (any < 0) {
		acc = neg ? LLONG_MIN : LLONG_MAX;
	} else if (neg)
		acc = -acc;
	if (endptr != 0)
		*endptr = (char *) (any ? s - 1 : nptr);
	return (acc);
}

