/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * lsquic_parse_gquic_be.c -- Parsing functions specific to big-endian
 *                              (now only Q043) GQUIC.
 */

#include "lsquic_types.h"
#include "lsquic_parse.h"
#include "lsquic_parse_common.h"


const struct parse_funcs lsquic_parse_funcs_gquic_Q043 =
{
    .pf_generate_simple_prst          =  lsquic_generate_gquic_reset,
};
