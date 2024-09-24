/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
#ifndef LSQUIC_QTAGS_H
#define LSQUIC_QTAGS_H 1

#define TAG(a, b, c, d) ((uint32_t)(((unsigned) d << 24) + \
                ((unsigned) c << 16) + ((unsigned) b << 8) + (unsigned) a))

#endif
