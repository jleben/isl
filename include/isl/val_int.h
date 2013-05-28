#ifndef ISL_VAL_INT_H
#define ISL_VAL_INT_H

#include <isl/int.h>
#include <isl/val.h>

__isl_give isl_val *isl_val_int_from_isl_int(isl_ctx *ctx, isl_int n);

#endif