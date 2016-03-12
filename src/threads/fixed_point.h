/* fixed_point.h - Macros for some fixed point operations (see appendix in pintos manual)
 * Symbol x, y denote fixed point numbers, on the other hand,
 * n denote integer number */

/* 16.15 fixed point representation */
#define F (1 << 15)
#define FP_MAX (1 << 16)

/* Convert integer to fixed point number */
#define INT_FP(n) n * F

/* Convert fixed point to neares integer */
#define FP_INT_NEAR(x) x >= 0 ? (x + F / 2) / F: (x - F / 2) /F

/* Convert fixed point to integer with round to zero */
#define FP_INT_ZERO(x) x / F

/* Add two fixed point numbers */
#define FP_ADD(x, y) x + y

/* Add integer with fixed point number */
#define FP_INT_ADD(x, n) x + INT_FP(n)

/* Subtract two fixed point numbers */
#define FP_SUB(x, y) x - y

/* Subtract intger with fixed point number */
#define FP_INT_SUB(x, n) x - INT_FP(n)

/* Multiply two fixed point numbers */
#define FP_MUL(x, y) ((int64_t) x) * y / F

/* Multiply integer to fixed point number */
#define FP_INT_MUL(x, n) x * n

/* Division operations for two fixed point numbers */
#define FP_DIV(x, y) ((int64_t) x) * F / y

/* Divide integer n into fixed point number y */
#define FP_INT_DIV(x, n) x / n

