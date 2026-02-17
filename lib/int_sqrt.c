/*
 * Copyright (C) 2013 Davidlohr Bueso <davidlohr.bueso@hp.com>
 *
 *  Based on the shift-and-subtract algorithm for computing integer
 *  square root from Guy L. Steele.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/bitops.h>

/**
 * Optimized implementation using hybrid approach:
 * - Fast seed using CLZ (via fls)
 * - Single Newton iteration
 * - Final correction for exact floor(sqrt(x))
 * We will not use Newton-Raphson iteration only because is not guaranteed to output the exact floor(sqrt(x)) even though it's three times faster.
 */
unsigned long int_sqrt(unsigned long x)
{
    unsigned long y;

    if (x <= 1)
        return x;

    /*
     * ARM64-optimized integer sqrt
     * Fast seed using CLZ (via fls)
     */
    y = 1UL << (fls(x) >> 1);

    /*
     * Single Newton iteration
     * Deterministic, hardware division on ARM64
     */
    y = (y + x / y) >> 1;

    /*
     * Final correction — GUARANTEES floor(sqrt(x))
     * No loops, bounded, exact
     */
    if ((y + 1) * (y + 1) <= x)
        y++;
    else if (y * y > x)
        y--;

    return y;
}
EXPORT_SYMBOL(int_sqrt);
