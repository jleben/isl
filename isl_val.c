/*
 * Copyright 2013      Ecole Normale Superieure
 *
 * Use of this software is governed by the MIT license
 *
 * Written by Sven Verdoolaege,
 * Ecole Normale Superieure, 45 rue d'Ulm, 75230 Paris, France
 */

#include <isl_int.h>
#include <isl_ctx_private.h>
#include <isl_val_private.h>

#undef BASE
#define BASE val

#include <isl_list_templ.c>

/* Allocate an isl_val object with indeterminate value.
 */
__isl_give isl_val *isl_val_alloc(isl_ctx *ctx)
{
	isl_val *v;

	v = isl_alloc_type(ctx, struct isl_val);
	if (!v)
		return NULL;

	v->ctx = ctx;
	isl_ctx_ref(ctx);
	v->ref = 1;
	isl_int_init(v->n);
	isl_int_init(v->d);

	return v;
}

/* Return a reference to an isl_val representing zero.
 */
__isl_give isl_val *isl_val_zero(isl_ctx *ctx)
{
	return isl_val_int_from_si(ctx, 0);
}

/* Return a reference to an isl_val representing one.
 */
__isl_give isl_val *isl_val_one(isl_ctx *ctx)
{
	return isl_val_int_from_si(ctx, 1);
}

/* Return a reference to an isl_val representing NaN.
 */
__isl_give isl_val *isl_val_nan(isl_ctx *ctx)
{
	isl_val *v;

	v = isl_val_alloc(ctx);
	if (!v)
		return NULL;

	isl_int_set_si(v->n, 0);
	isl_int_set_si(v->d, 0);

	return v;
}

/* Change "v" into a NaN.
 */
__isl_give isl_val *isl_val_set_nan(__isl_take isl_val *v)
{
	if (!v)
		return NULL;
	if (isl_val_is_nan(v))
		return v;
	v = isl_val_cow(v);
	if (!v)
		return NULL;

	isl_int_set_si(v->n, 0);
	isl_int_set_si(v->d, 0);

	return v;
}

/* Return a reference to an isl_val representing +infinity.
 */
__isl_give isl_val *isl_val_infty(isl_ctx *ctx)
{
	isl_val *v;

	v = isl_val_alloc(ctx);
	if (!v)
		return NULL;

	isl_int_set_si(v->n, 1);
	isl_int_set_si(v->d, 0);

	return v;
}

/* Return a reference to an isl_val representing -infinity.
 */
__isl_give isl_val *isl_val_neginfty(isl_ctx *ctx)
{
	isl_val *v;

	v = isl_val_alloc(ctx);
	if (!v)
		return NULL;

	isl_int_set_si(v->n, -1);
	isl_int_set_si(v->d, 0);

	return v;
}

/* Return a reference to an isl_val representing the integer "i".
 */
__isl_give isl_val *isl_val_int_from_si(isl_ctx *ctx, long i)
{
	isl_val *v;

	v = isl_val_alloc(ctx);
	if (!v)
		return NULL;

	isl_int_set_si(v->n, i);
	isl_int_set_si(v->d, 1);

	return v;
}

/* Change the value of "v" to be equal to the integer "i".
 */
__isl_give isl_val *isl_val_set_si(__isl_take isl_val *v, long i)
{
	if (!v)
		return NULL;
	if (isl_val_is_int(v) && isl_int_cmp_si(v->n, i) == 0)
		return v;
	v = isl_val_cow(v);
	if (!v)
		return NULL;

	isl_int_set_si(v->n, i);
	isl_int_set_si(v->d, 1);

	return v;
}

/* Change the value of "v" to be equal to zero.
 */
__isl_give isl_val *isl_val_set_zero(__isl_take isl_val *v)
{
	return isl_val_set_si(v, 0);
}

/* Return a reference to an isl_val representing the unsigned integer "u".
 */
__isl_give isl_val *isl_val_int_from_ui(isl_ctx *ctx, unsigned long u)
{
	isl_val *v;

	v = isl_val_alloc(ctx);
	if (!v)
		return NULL;

	isl_int_set_ui(v->n, u);
	isl_int_set_si(v->d, 1);

	return v;
}

/* Return a reference to an isl_val representing the integer "n".
 */
__isl_give isl_val *isl_val_int_from_isl_int(isl_ctx *ctx, isl_int n)
{
	isl_val *v;

	v = isl_val_alloc(ctx);
	if (!v)
		return NULL;

	isl_int_set(v->n, n);
	isl_int_set_si(v->d, 1);

	return v;
}

/* Return a reference to an isl_val representing the rational value "n"/"d".
 * Normalizing the isl_val (if needed) is left to the caller.
 */
__isl_give isl_val *isl_val_rat_from_isl_int(isl_ctx *ctx,
	isl_int n, isl_int d)
{
	isl_val *v;

	v = isl_val_alloc(ctx);
	if (!v)
		return NULL;

	isl_int_set(v->n, n);
	isl_int_set(v->d, d);

	return v;
}

/* Return a new reference to "v".
 */
__isl_give isl_val *isl_val_copy(__isl_keep isl_val *v)
{
	if (!v)
		return NULL;

	v->ref++;
	return v;
}

/* Return a fresh copy of "val".
 */
__isl_give isl_val *isl_val_dup(__isl_keep isl_val *val)
{
	isl_val *dup;

	if (!val)
		return NULL;

	dup = isl_val_alloc(isl_val_get_ctx(val));
	if (!dup)
		return NULL;

	isl_int_set(dup->n, val->n);
	isl_int_set(dup->d, val->d);

	return dup;
}

/* Return an isl_val that is equal to "val" and that has only
 * a single reference.
 */
__isl_give isl_val *isl_val_cow(__isl_take isl_val *val)
{
	if (!val)
		return NULL;

	if (val->ref == 1)
		return val;
	val->ref--;
	return isl_val_dup(val);
}

/* Free "v" and return NULL.
 */
void *isl_val_free(__isl_take isl_val *v)
{
	if (!v)
		return NULL;

	if (--v->ref > 0)
		return NULL;

	isl_ctx_deref(v->ctx);
	isl_int_clear(v->n);
	isl_int_clear(v->d);
	free(v);
	return NULL;
}

/* Extract the numerator of a rational value "v" as an integer.
 *
 * If "v" is not a rational value, then the result is undefined.
 */
long isl_val_get_num_si(__isl_keep isl_val *v)
{
	if (!v)
		return 0;
	if (!isl_val_is_rat(v))
		isl_die(isl_val_get_ctx(v), isl_error_invalid,
			"expecting rational value", return 0);
	if (!isl_int_fits_slong(v->n))
		isl_die(isl_val_get_ctx(v), isl_error_invalid,
			"numerator too large", return 0);
	return isl_int_get_si(v->n);
}

/* Extract the denominator of a rational value "v" as an integer.
 *
 * If "v" is not a rational value, then the result is undefined.
 */
long isl_val_get_den_si(__isl_keep isl_val *v)
{
	if (!v)
		return 0;
	if (!isl_val_is_rat(v))
		isl_die(isl_val_get_ctx(v), isl_error_invalid,
			"expecting rational value", return 0);
	if (!isl_int_fits_slong(v->d))
		isl_die(isl_val_get_ctx(v), isl_error_invalid,
			"denominator too large", return 0);
	return isl_int_get_si(v->d);
}

/* Return an approximation of "v" as a double.
 */
double isl_val_get_d(__isl_keep isl_val *v)
{
	if (!v)
		return 0;
	if (!isl_val_is_rat(v))
		isl_die(isl_val_get_ctx(v), isl_error_invalid,
			"expecting rational value", return 0);
	return isl_int_get_d(v->n) / isl_int_get_d(v->d);
}

/* Return the isl_ctx to which "val" belongs.
 */
isl_ctx *isl_val_get_ctx(__isl_keep isl_val *val)
{
	return val ? val->ctx : NULL;
}

/* Normalize "v".
 *
 * In particular, make sure that the denominator of a rational value
 * is positive and the numerator and denominator do not have any
 * common divisors.
 *
 * This function should not be called by an external user
 * since it will only be given normalized values.
 */
__isl_give isl_val *isl_val_normalize(__isl_take isl_val *v)
{
	isl_ctx *ctx;

	if (!v)
		return NULL;
	if (isl_val_is_int(v))
		return v;
	if (!isl_val_is_rat(v))
		return v;
	if (isl_int_is_neg(v->d)) {
		isl_int_neg(v->d, v->d);
		isl_int_neg(v->n, v->n);
	}
	ctx = isl_val_get_ctx(v);
	isl_int_gcd(ctx->normalize_gcd, v->n, v->d);
	if (isl_int_is_one(ctx->normalize_gcd))
		return v;
	isl_int_divexact(v->n, v->n, ctx->normalize_gcd);
	isl_int_divexact(v->d, v->d, ctx->normalize_gcd);
	return v;
}

/* Return the opposite of "v".
 */
__isl_give isl_val *isl_val_neg(__isl_take isl_val *v)
{
	if (!v)
		return NULL;
	if (isl_val_is_nan(v))
		return v;
	if (isl_val_is_zero(v))
		return v;

	v = isl_val_cow(v);
	if (!v)
		return NULL;
	isl_int_neg(v->n, v->n);

	return v;
}

/* Return the absolute value of "v".
 */
__isl_give isl_val *isl_val_abs(__isl_take isl_val *v)
{
	if (!v)
		return NULL;
	if (isl_val_is_nan(v))
		return v;
	if (isl_val_is_nonneg(v))
		return v;
	return isl_val_neg(v);
}

/* Return the "floor" (greatest integer part) of "v".
 * That is, return the result of rounding towards -infinity.
 */
__isl_give isl_val *isl_val_floor(__isl_take isl_val *v)
{
	if (!v)
		return NULL;
	if (isl_val_is_int(v))
		return v;
	if (!isl_val_is_rat(v))
		return v;

	v = isl_val_cow(v);
	if (!v)
		return NULL;
	isl_int_fdiv_q(v->n, v->n, v->d);
	isl_int_set_si(v->d, 1);

	return v;
}

/* Return the "ceiling" of "v".
 * That is, return the result of rounding towards +infinity.
 */
__isl_give isl_val *isl_val_ceil(__isl_take isl_val *v)
{
	if (!v)
		return NULL;
	if (isl_val_is_int(v))
		return v;
	if (!isl_val_is_rat(v))
		return v;

	v = isl_val_cow(v);
	if (!v)
		return NULL;
	isl_int_cdiv_q(v->n, v->n, v->d);
	isl_int_set_si(v->d, 1);

	return v;
}

/* Truncate "v".
 * That is, return the result of rounding towards zero.
 */
__isl_give isl_val *isl_val_trunc(__isl_take isl_val *v)
{
	if (!v)
		return NULL;
	if (isl_val_is_int(v))
		return v;
	if (!isl_val_is_rat(v))
		return v;

	v = isl_val_cow(v);
	if (!v)
		return NULL;
	isl_int_tdiv_q(v->n, v->n, v->d);
	isl_int_set_si(v->d, 1);

	return v;
}

/* Return 2^v, where v is an integer (that is not too large).
 */
__isl_give isl_val *isl_val_2exp(__isl_take isl_val *v)
{
	unsigned long exp;
	int neg;

	v = isl_val_cow(v);
	if (!v)
		return NULL;
	if (!isl_val_is_int(v))
		isl_die(isl_val_get_ctx(v), isl_error_invalid,
			"can only compute integer powers",
			return isl_val_free(v));
	neg = isl_val_is_neg(v);
	if (neg)
		isl_int_neg(v->n, v->n);
	if (!isl_int_fits_ulong(v->n))
		isl_die(isl_val_get_ctx(v), isl_error_invalid,
			"exponent too large", return isl_val_free(v));
	exp = isl_int_get_ui(v->n);
	if (neg) {
		isl_int_mul_2exp(v->d, v->d, exp);
		isl_int_set_si(v->n, 1);
	} else {
		isl_int_mul_2exp(v->n, v->d, exp);
	}

	return v;
}

/* Return the minimum of "v1" and "v2".
 */
__isl_give isl_val *isl_val_min(__isl_take isl_val *v1, __isl_take isl_val *v2)
{
	if (!v1 || !v2)
		goto error;

	if (isl_val_is_nan(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_nan(v2)) {
		isl_val_free(v1);
		return v2;
	}
	if (isl_val_le(v1, v2)) {
		isl_val_free(v2);
		return v1;
	} else {
		isl_val_free(v1);
		return v2;
	}
error:
	isl_val_free(v1);
	isl_val_free(v2);
	return NULL;
}

/* Return the maximum of "v1" and "v2".
 */
__isl_give isl_val *isl_val_max(__isl_take isl_val *v1, __isl_take isl_val *v2)
{
	if (!v1 || !v2)
		goto error;

	if (isl_val_is_nan(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_nan(v2)) {
		isl_val_free(v1);
		return v2;
	}
	if (isl_val_ge(v1, v2)) {
		isl_val_free(v2);
		return v1;
	} else {
		isl_val_free(v1);
		return v2;
	}
error:
	isl_val_free(v1);
	isl_val_free(v2);
	return NULL;
}

/* Return the sum of "v1" and "v2".
 */
__isl_give isl_val *isl_val_add(__isl_take isl_val *v1, __isl_take isl_val *v2)
{
	if (!v1 || !v2)
		goto error;
	if (isl_val_is_nan(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_nan(v2)) {
		isl_val_free(v1);
		return v2;
	}
	if ((isl_val_is_infty(v1) && isl_val_is_neginfty(v2)) ||
	    (isl_val_is_neginfty(v1) && isl_val_is_infty(v2))) {
		isl_val_free(v2);
		return isl_val_set_nan(v1);
	}
	if (isl_val_is_infty(v1) || isl_val_is_neginfty(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_infty(v2) || isl_val_is_neginfty(v2)) {
		isl_val_free(v1);
		return v2;
	}
	if (isl_val_is_zero(v1)) {
		isl_val_free(v1);
		return v2;
	}
	if (isl_val_is_zero(v2)) {
		isl_val_free(v2);
		return v1;
	}

	v1 = isl_val_cow(v1);
	if (!v1)
		goto error;
	if (isl_val_is_int(v1) && isl_val_is_int(v2))
		isl_int_add(v1->n, v1->n, v2->n);
	else {
		if (isl_int_eq(v1->d, v2->d))
			isl_int_add(v1->n, v1->n, v2->n);
		else {
			isl_int_mul(v1->n, v1->n, v2->d);
			isl_int_addmul(v1->n, v2->n, v1->d);
			isl_int_mul(v1->d, v1->d, v2->d);
		}
		v1 = isl_val_normalize(v1);
	}
	isl_val_free(v2);
	return v1;
error:
	isl_val_free(v1);
	isl_val_free(v2);
	return NULL;
}

/* Return the sum of "v1" and "v2".
 */
__isl_give isl_val *isl_val_add_ui(__isl_take isl_val *v1, unsigned long v2)
{
	if (!v1)
		return NULL;
	if (!isl_val_is_rat(v1))
		return v1;
	if (v2 == 0)
		return v1;
	v1 = isl_val_cow(v1);
	if (!v1)
		return NULL;

	isl_int_addmul_ui(v1->n, v1->d, v2);

	return v1;
}

/* Subtract "v2" from "v1".
 */
__isl_give isl_val *isl_val_sub(__isl_take isl_val *v1, __isl_take isl_val *v2)
{
	if (!v1 || !v2)
		goto error;
	if (isl_val_is_nan(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_nan(v2)) {
		isl_val_free(v1);
		return v2;
	}
	if ((isl_val_is_infty(v1) && isl_val_is_infty(v2)) ||
	    (isl_val_is_neginfty(v1) && isl_val_is_neginfty(v2))) {
		isl_val_free(v2);
		return isl_val_set_nan(v1);
	}
	if (isl_val_is_infty(v1) || isl_val_is_neginfty(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_infty(v2) || isl_val_is_neginfty(v2)) {
		isl_val_free(v1);
		return isl_val_neg(v2);
	}
	if (isl_val_is_zero(v2)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_zero(v1)) {
		isl_val_free(v1);
		return isl_val_neg(v2);
	}

	v1 = isl_val_cow(v1);
	if (!v1)
		goto error;
	if (isl_val_is_int(v1) && isl_val_is_int(v2))
		isl_int_sub(v1->n, v1->n, v2->n);
	else {
		if (isl_int_eq(v1->d, v2->d))
			isl_int_sub(v1->n, v1->n, v2->n);
		else {
			isl_int_mul(v1->n, v1->n, v2->d);
			isl_int_submul(v1->n, v2->n, v1->d);
			isl_int_mul(v1->d, v1->d, v2->d);
		}
		v1 = isl_val_normalize(v1);
	}
	isl_val_free(v2);
	return v1;
error:
	isl_val_free(v1);
	isl_val_free(v2);
	return NULL;
}

/* Subtract "v2" from "v1".
 */
__isl_give isl_val *isl_val_sub_ui(__isl_take isl_val *v1, unsigned long v2)
{
	if (!v1)
		return NULL;
	if (!isl_val_is_rat(v1))
		return v1;
	if (v2 == 0)
		return v1;
	v1 = isl_val_cow(v1);
	if (!v1)
		return NULL;

	isl_int_submul_ui(v1->n, v1->d, v2);

	return v1;
}

/* Return the product of "v1" and "v2".
 */
__isl_give isl_val *isl_val_mul(__isl_take isl_val *v1, __isl_take isl_val *v2)
{
	if (!v1 || !v2)
		goto error;
	if (isl_val_is_nan(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_nan(v2)) {
		isl_val_free(v1);
		return v2;
	}
	if ((!isl_val_is_rat(v1) && isl_val_is_zero(v2)) ||
	    (isl_val_is_zero(v1) && !isl_val_is_rat(v2))) {
		isl_val_free(v2);
		return isl_val_set_nan(v1);
	}
	if (isl_val_is_zero(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_zero(v2)) {
		isl_val_free(v1);
		return v2;
	}
	if (isl_val_is_infty(v1) || isl_val_is_neginfty(v1)) {
		if (isl_val_is_neg(v2))
			v1 = isl_val_neg(v1);
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_infty(v2) || isl_val_is_neginfty(v2)) {
		if (isl_val_is_neg(v1))
			v2 = isl_val_neg(v2);
		isl_val_free(v1);
		return v2;
	}

	v1 = isl_val_cow(v1);
	if (!v1)
		goto error;
	if (isl_val_is_int(v1) && isl_val_is_int(v2))
		isl_int_mul(v1->n, v1->n, v2->n);
	else {
		isl_int_mul(v1->n, v1->n, v2->n);
		isl_int_mul(v1->d, v1->d, v2->d);
		v1 = isl_val_normalize(v1);
	}
	isl_val_free(v2);
	return v1;
error:
	isl_val_free(v1);
	isl_val_free(v2);
	return NULL;
}

/* Return the product of "v1" and "v2".
 */
__isl_give isl_val *isl_val_mul_ui(__isl_take isl_val *v1, unsigned long v2)
{
	if (!v1)
		return NULL;
	if (isl_val_is_nan(v1))
		return v1;
	if (!isl_val_is_rat(v1)) {
		if (v2 == 0)
			v1 = isl_val_set_nan(v1);
		return v1;
	}
	if (v2 == 1)
		return v1;
	v1 = isl_val_cow(v1);
	if (!v1)
		return NULL;

	isl_int_mul_ui(v1->n, v1->n, v2);

	return isl_val_normalize(v1);
}

/* Divide "v1" by "v2".
 */
__isl_give isl_val *isl_val_div(__isl_take isl_val *v1, __isl_take isl_val *v2)
{
	if (!v1 || !v2)
		goto error;
	if (isl_val_is_nan(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_nan(v2)) {
		isl_val_free(v1);
		return v2;
	}
	if (isl_val_is_zero(v2) ||
	    (!isl_val_is_rat(v1) && !isl_val_is_rat(v2))) {
		isl_val_free(v2);
		return isl_val_set_nan(v1);
	}
	if (isl_val_is_zero(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_infty(v1) || isl_val_is_neginfty(v1)) {
		if (isl_val_is_neg(v2))
			v1 = isl_val_neg(v1);
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_infty(v2) || isl_val_is_neginfty(v2)) {
		isl_val_free(v2);
		return isl_val_set_zero(v1);
	}

	v1 = isl_val_cow(v1);
	if (!v1)
		goto error;
	if (isl_val_is_int(v2)) {
		isl_int_mul(v1->d, v1->d, v2->n);
		v1 = isl_val_normalize(v1);
	} else {
		isl_int_mul(v1->d, v1->d, v2->n);
		isl_int_mul(v1->n, v1->n, v2->d);
		v1 = isl_val_normalize(v1);
	}
	isl_val_free(v2);
	return v1;
error:
	isl_val_free(v1);
	isl_val_free(v2);
	return NULL;
}

/* Given two integer values "v1" and "v2", check if "v1" is divisible by "v2".
 */
int isl_val_is_divisible_by(__isl_keep isl_val *v1, __isl_keep isl_val *v2)
{
	if (!v1 || !v2)
		return -1;

	if (!isl_val_is_int(v1) || !isl_val_is_int(v2))
		isl_die(isl_val_get_ctx(v1), isl_error_invalid,
			"expecting two integers", return -1);

	return isl_int_is_divisible_by(v1->n, v2->n);
}

/* Given two integer values "v1" and "v2", return the residue of "v1"
 * modulo "v2".
 */
__isl_give isl_val *isl_val_mod(__isl_take isl_val *v1, __isl_take isl_val *v2)
{
	if (!v1 || !v2)
		goto error;
	if (!isl_val_is_int(v1) || !isl_val_is_int(v2))
		isl_die(isl_val_get_ctx(v1), isl_error_invalid,
			"expecting two integers", goto error);
	if (isl_val_is_nonneg(v1) && isl_val_lt(v1, v2)) {
		isl_val_free(v2);
		return v1;
	}
	v1 = isl_val_cow(v1);
	if (!v1)
		goto error;
	isl_int_fdiv_r(v1->n, v1->n, v2->n);
	isl_val_free(v2);
	return v1;
error:
	isl_val_free(v1);
	isl_val_free(v2);
	return NULL;
}

/* Given two integer values, return their greatest common divisor.
 */
__isl_give isl_val *isl_val_gcd(__isl_take isl_val *v1, __isl_take isl_val *v2)
{
	if (!v1 || !v2)
		goto error;
	if (!isl_val_is_int(v1) || !isl_val_is_int(v2))
		isl_die(isl_val_get_ctx(v1), isl_error_invalid,
			"expecting two integers", goto error);
	if (isl_val_eq(v1, v2)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_one(v1)) {
		isl_val_free(v2);
		return v1;
	}
	if (isl_val_is_one(v2)) {
		isl_val_free(v1);
		return v2;
	}
	v1 = isl_val_cow(v1);
	if (!v1)
		goto error;
	isl_int_gcd(v1->n, v1->n, v2->n);
	isl_val_free(v2);
	return v1;
error:
	isl_val_free(v1);
	isl_val_free(v2);
	return NULL;
}

/* Given two integer values v1 and v2, return their greatest common divisor g,
 * as well as two integers x and y such that x * v1 + y * v2 = g.
 */
__isl_give isl_val *isl_val_gcdext(__isl_take isl_val *v1,
	__isl_take isl_val *v2, __isl_give isl_val **x, __isl_give isl_val **y)
{
	isl_ctx *ctx;
	isl_val *a = NULL, *b = NULL;

	if (!x && !y)
		return isl_val_gcd(v1, v2);

	if (!v1 || !v2)
		goto error;

	ctx = isl_val_get_ctx(v1);
	if (!isl_val_is_int(v1) || !isl_val_is_int(v2))
		isl_die(ctx, isl_error_invalid,
			"expecting two integers", goto error);

	v1 = isl_val_cow(v1);
	a = isl_val_alloc(ctx);
	b = isl_val_alloc(ctx);
	if (!v1 || !a || !b)
		goto error;
	isl_int_gcdext(v1->n, a->n, b->n, v1->n, v2->n);
	if (x) {
		isl_int_set_si(a->d, 1);
		*x = a;
	} else
		isl_val_free(a);
	if (y) {
		isl_int_set_si(b->d, 1);
		*y = b;
	} else
		isl_val_free(b);
	isl_val_free(v2);
	return v1;
error:
	isl_val_free(v1);
	isl_val_free(v2);
	isl_val_free(a);
	isl_val_free(b);
	if (x)
		*x = NULL;
	if (y)
		*y = NULL;
	return NULL;
}

/* Does "v" represent an integer value?
 */
int isl_val_is_int(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return isl_int_is_one(v->d);
}

/* Does "v" represent a rational value?
 */
int isl_val_is_rat(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return !isl_int_is_zero(v->d);
}

/* Does "v" represent NaN?
 */
int isl_val_is_nan(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return isl_int_is_zero(v->n) && isl_int_is_zero(v->d);
}

/* Does "v" represent +infinity?
 */
int isl_val_is_infty(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return isl_int_is_pos(v->n) && isl_int_is_zero(v->d);
}

/* Does "v" represent -infinity?
 */
int isl_val_is_neginfty(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return isl_int_is_neg(v->n) && isl_int_is_zero(v->d);
}

/* Does "v" represent the integer zero?
 */
int isl_val_is_zero(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return isl_int_is_zero(v->n) && !isl_int_is_zero(v->d);
}

/* Does "v" represent the integer one?
 */
int isl_val_is_one(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return isl_int_eq(v->n, v->d);
}

/* Does "v" represent the integer negative one?
 */
int isl_val_is_negone(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return isl_int_is_neg(v->n) && isl_int_abs_eq(v->n, v->d);
}

/* Is "v" (strictly) positive?
 */
int isl_val_is_pos(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return isl_int_is_pos(v->n);
}

/* Is "v" (strictly) negative?
 */
int isl_val_is_neg(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	return isl_int_is_neg(v->n);
}

/* Is "v" non-negative?
 */
int isl_val_is_nonneg(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	if (isl_val_is_nan(v))
		return 0;

	return isl_int_is_nonneg(v->n);
}

/* Is "v" non-positive?
 */
int isl_val_is_nonpos(__isl_keep isl_val *v)
{
	if (!v)
		return -1;

	if (isl_val_is_nan(v))
		return 0;

	return isl_int_is_nonpos(v->n);
}

/* Return the sign of "v".
 *
 * The sign of NaN is undefined.
 */
int isl_val_sgn(__isl_keep isl_val *v)
{
	if (!v)
		return 0;
	if (isl_val_is_zero(v))
		return 0;
	if (isl_val_is_pos(v))
		return 1;
	return -1;
}

/* Is "v1" (strictly) less than "v2"?
 */
int isl_val_lt(__isl_keep isl_val *v1, __isl_keep isl_val *v2)
{
	isl_int t;
	int lt;

	if (!v1 || !v2)
		return -1;
	if (isl_val_is_int(v1) && isl_val_is_int(v2))
		return isl_int_lt(v1->n, v2->n);
	if (isl_val_is_nan(v1) || isl_val_is_nan(v2))
		return 0;
	if (isl_val_eq(v1, v2))
		return 0;
	if (isl_val_is_infty(v2))
		return 1;
	if (isl_val_is_infty(v1))
		return 0;
	if (isl_val_is_neginfty(v1))
		return 1;
	if (isl_val_is_neginfty(v2))
		return 0;

	isl_int_init(t);
	isl_int_mul(t, v1->n, v2->d);
	isl_int_submul(t, v2->n, v1->d);
	lt = isl_int_is_neg(t);
	isl_int_clear(t);

	return lt;
}

/* Is "v1" (strictly) greater than "v2"?
 */
int isl_val_gt(__isl_keep isl_val *v1, __isl_keep isl_val *v2)
{
	return isl_val_lt(v2, v1);
}

/* Is "v1" less than or equal to "v2"?
 */
int isl_val_le(__isl_keep isl_val *v1, __isl_keep isl_val *v2)
{
	isl_int t;
	int le;

	if (!v1 || !v2)
		return -1;
	if (isl_val_is_int(v1) && isl_val_is_int(v2))
		return isl_int_le(v1->n, v2->n);
	if (isl_val_is_nan(v1) || isl_val_is_nan(v2))
		return 0;
	if (isl_val_eq(v1, v2))
		return 1;
	if (isl_val_is_infty(v2))
		return 1;
	if (isl_val_is_infty(v1))
		return 0;
	if (isl_val_is_neginfty(v1))
		return 1;
	if (isl_val_is_neginfty(v2))
		return 0;

	isl_int_init(t);
	isl_int_mul(t, v1->n, v2->d);
	isl_int_submul(t, v2->n, v1->d);
	le = isl_int_is_nonpos(t);
	isl_int_clear(t);

	return le;
}

/* Is "v1" greater than or equal to "v2"?
 */
int isl_val_ge(__isl_keep isl_val *v1, __isl_keep isl_val *v2)
{
	return isl_val_le(v2, v1);
}

/* How does "v" compare to "i"?
 *
 * Return 1 if v is greater, -1 if v is smaller and 0 if v is equal to i.
 *
 * If v is NaN (or NULL), then the result is undefined.
 */
int isl_val_cmp_si(__isl_keep isl_val *v, long i)
{
	isl_int t;
	int cmp;

	if (!v)
		return 0;
	if (isl_val_is_int(v))
		return isl_int_cmp_si(v->n, i);
	if (isl_val_is_nan(v))
		return 0;
	if (isl_val_is_infty(v))
		return 1;
	if (isl_val_is_neginfty(v))
		return -1;

	isl_int_init(t);
	isl_int_mul_si(t, v->d, i);
	isl_int_sub(t, v->n, t);
	cmp = isl_int_sgn(t);
	isl_int_clear(t);

	return cmp;
}

/* Is "v1" equal to "v2"?
 */
int isl_val_eq(__isl_keep isl_val *v1, __isl_keep isl_val *v2)
{
	if (!v1 || !v2)
		return -1;
	if (isl_val_is_nan(v1) || isl_val_is_nan(v2))
		return 0;

	return isl_int_eq(v1->n, v2->n) && isl_int_eq(v1->d, v2->d);
}

/* Is "v1" different from "v2"?
 */
int isl_val_ne(__isl_keep isl_val *v1, __isl_keep isl_val *v2)
{
	if (!v1 || !v2)
		return -1;
	if (isl_val_is_nan(v1) || isl_val_is_nan(v2))
		return 0;

	return isl_int_ne(v1->n, v2->n) || isl_int_ne(v1->d, v2->d);
}

/* Print a textual representation of "v" onto "p".
 */
__isl_give isl_printer *isl_printer_print_val(__isl_take isl_printer *p,
	__isl_keep isl_val *v)
{
	int neg;

	if (!p || !v)
		return isl_printer_free(p);

	neg = isl_int_is_neg(v->n);
	if (neg) {
		p = isl_printer_print_str(p, "-");
		isl_int_neg(v->n, v->n);
	}
	if (isl_int_is_zero(v->d)) {
		int sgn = isl_int_sgn(v->n);
		p = isl_printer_print_str(p, sgn < 0 ? "-infty" :
					    sgn == 0 ? "NaN" : "infty");
	} else
		p = isl_printer_print_isl_int(p, v->n);
	if (neg)
		isl_int_neg(v->n, v->n);
	if (!isl_int_is_zero(v->d) && !isl_int_is_one(v->d)) {
		p = isl_printer_print_str(p, "/");
		p = isl_printer_print_isl_int(p, v->d);
	}

	return p;
}