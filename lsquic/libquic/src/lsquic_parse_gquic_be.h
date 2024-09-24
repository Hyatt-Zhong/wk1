/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
#ifndef LSQUIC_PARSE_GQUIC_BE_H
#define LSQUIC_PARSE_GQUIC_BE_H

/* Header file to make it easy to reference gQUIC parsing functions.  This
 * is only meant to be used internally.  The alternative would be to place
 * all gQUIC-big-endian functions -- from all versions -- in a single file,
 * and that would be a mess.
 */

#define CHECK_SPACE(need, pstart, pend)  \
    do { if ((intptr_t) (need) > ((pend) - (pstart))) { return -1; } } while (0)

#endif
