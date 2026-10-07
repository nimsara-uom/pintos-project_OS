#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#define FP_SHIFT 14
#define FP_F (1 << FP_SHIFT)
#define FP_FROM_INT(n) ((n) * FP_F)
#define FP_TO_INT_ZERO(x) ((x) / FP_F)
#define FP_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + FP_F / 2) / FP_F \
                                         : ((x) - FP_F / 2) / FP_F)
#define FP_ADD(x, y) ((x) + (y))
#define FP_SUB(x, y) ((x) - (y))
#define FP_ADD_INT(x, n) ((x) + (n) * FP_F)
#define FP_SUB_INT(x, n) ((x) - (n) * FP_F)
#define FP_MUL(x, y) ((int64_t) (x) * (y) / FP_F)
#define FP_MUL_INT(x, n) ((x) * (n))
#define FP_DIV(x, y) ((int64_t) (x) * FP_F / (y))
#define FP_DIV_INT(x, n) ((x) / (n))

#endif
