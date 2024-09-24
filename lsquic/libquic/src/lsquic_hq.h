/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * lsquic_hq.h -- HTTP/3 (originally "HTTP over QUIC" or HQ) types
 */

#ifndef LSQUIC_HQ_H
#define LSQUIC_HQ_H 1

enum http_error_code
{
    HEC_NO_ERROR                =  0x100,
    HEC_INTERNAL_ERROR          =  0x102,
    HEC_MESSAGE_ERROR           =  0x10E,
};

#endif
