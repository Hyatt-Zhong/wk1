/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * lsquic_parse_gquic_common.c -- Parsing functions common to GQUIC
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <sys/queue.h>
#ifndef WIN32
#include <sys/types.h>
#else
#include <vc_compat.h>
#endif

#include "lsquic_types.h"
#include "lsquic_int_types.h"
#include "lsquic_packet_common.h"
#include "lsquic_packet_out.h"
#include "lsquic_packet_gquic.h"
#include "lsquic_packet_in.h"
#include "lsquic_parse_common.h"
#include "lsquic_parse.h"
#include "lsquic_version.h"
#include "lsquic.h"

#define LSQUIC_LOGGER_MODULE LSQLM_PARSE
#include "lsquic_logger.h"

#define CHECK_SPACE(need, pstart, pend)  \
    do { if ((intptr_t) (need) > ((pend) - (pstart))) { return -1; } } while (0)

/* This partially parses `packet_in' and returns 0 if in case it succeeded and
 * -1 on failure.
 *
 * After this function returns 0, connection ID, nonce, and version fields can
 * be examined.  To finsh parsing the packet, call version-specific
 * pf_parse_packet_in_finish() routine.
 */
int
lsquic_gquic_parse_packet_in_begin (lsquic_packet_in_t *packet_in,
                size_t length, int is_server, unsigned cid_len,
                struct packin_parse_state *state)
{
    int nbytes;
    enum PACKET_PUBLIC_FLAGS public_flags;
    const unsigned char *p = packet_in->pi_data;
    const unsigned char *const pend = packet_in->pi_data + length;

    if (length > GQUIC_MAX_PACKET_SZ)
    {
        LSQ_DEBUG("Cannot handle packet_in_size(%zd) > %d packet incoming "
            "packet's header", length, GQUIC_MAX_PACKET_SZ);
        return -1;
    }

    CHECK_SPACE(1, p, pend);

    public_flags = *p++;

    if (public_flags & PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID)
    {
        CHECK_SPACE(8, p, pend);
        memset(&packet_in->pi_conn_id, 0, sizeof(packet_in->pi_conn_id));
        packet_in->pi_conn_id.len = 8;
        memcpy(&packet_in->pi_conn_id.idbuf, p, 8);
        packet_in->pi_flags |= PI_CONN_ID;
        p += 8;
    }

    if (public_flags & PACKET_PUBLIC_FLAGS_VERSION)
    {
        /* It seems that version negotiation packets sent by Google may have
         * NONCE bit set.  Ignore it:
         */
        public_flags &= ~PACKET_PUBLIC_FLAGS_NONCE;

        if (is_server)
        {
            CHECK_SPACE(4, p, pend);
            packet_in->pi_quic_ver = p - packet_in->pi_data;
            p += 4;
        }
        else
        {   /* OK, we have a version negotiation packet.  We need to verify
             * that it has correct structure.  See Section 4.3 of
             * [draft-ietf-quic-transport-00].
             */
            if ((public_flags & ~(PACKET_PUBLIC_FLAGS_VERSION|
                                  PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID))
                || ((pend - p) & 3))
                return -1;
            CHECK_SPACE(4, p, pend);
            packet_in->pi_quic_ver = p - packet_in->pi_data;
            p = pend;
        }
    }
    else
    {
        /* From [draft-hamilton-quic-transport-protocol-01]:
         *    0x40 = MULTIPATH. This bit is reserved for multipath use.
         *    0x80 is currently unused, and must be set to 0.
         *
         * The reference implementation checks that two high bits are not set
         * if version flag is not set or if the version is the same.  For our
         * purposes, all GQUIC version we support so far have these bits set
         * to zero.
         */
        if (public_flags & (0x80|0x40))
            return -1;
        packet_in->pi_quic_ver = 0;
    }

    if (!is_server && (public_flags & PACKET_PUBLIC_FLAGS_NONCE) ==
                                            PACKET_PUBLIC_FLAGS_NONCE)
    {
        CHECK_SPACE(32, p, pend);
        packet_in->pi_nonce = p - packet_in->pi_data;
        p += 32;
    }
    else
        packet_in->pi_nonce = 0;

    state->pps_p = p;

    packet_in->pi_packno = 0;
    if (0 == (public_flags & (PACKET_PUBLIC_FLAGS_VERSION|PACKET_PUBLIC_FLAGS_RST))
        || ((public_flags & PACKET_PUBLIC_FLAGS_VERSION) && is_server))
    {
        nbytes = twobit_to_1246((public_flags >> 4) & 3);
        CHECK_SPACE(nbytes, p, pend);
        p += nbytes;
        state->pps_nbytes = nbytes;
    }
    else
        state->pps_nbytes = 0;

    packet_in->pi_header_sz    = p - packet_in->pi_data;
    packet_in->pi_frame_types  = 0;
    memset(&packet_in->pi_next, 0, sizeof(packet_in->pi_next));
    packet_in->pi_data_sz      = length;
    packet_in->pi_refcnt       = 0;
    packet_in->pi_received     = 0;
    packet_in->pi_flags       |= PI_GQUIC;
    packet_in->pi_flags       |= ((public_flags >> 4) & 3) << PIBIT_BITS_SHIFT;

    return 0;
}


static const unsigned char simple_prst_payload[] = {
    'P', 'R', 'S', 'T',
    0x01, 0x00, 0x00, 0x00,
    'R', 'N', 'O', 'N',
    0x08, 0x00, 0x00, 0x00,
    1, 2, 3, 4, 5, 6, 7, 8,
};


typedef char correct_simple_prst_size[(GQUIC_RESET_SZ ==
                1 + GQUIC_CID_LEN + sizeof(simple_prst_payload)) ? 1 : -1 ];


ssize_t
lsquic_generate_gquic_reset (const lsquic_cid_t *cidp,
                                        unsigned char *buf, size_t buf_sz)
{
    lsquic_cid_t cid;

    if (buf_sz < 1 + GQUIC_CID_LEN + sizeof(simple_prst_payload))
    {
        errno = ENOBUFS;
        return -1;
    }

    if (cidp)
    {
        assert(GQUIC_CID_LEN == cidp->len);
        cid = *cidp;
    }
    else
    {
        memset(&cid, 0, sizeof(cid));
        cid.len = GQUIC_CID_LEN;
    }

    *buf++ = PACKET_PUBLIC_FLAGS_RST | PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID;

    memcpy(buf, cid.idbuf, GQUIC_CID_LEN);
    buf += GQUIC_CID_LEN;

    memcpy(buf, simple_prst_payload, sizeof(simple_prst_payload));
    return 1 + GQUIC_CID_LEN + sizeof(simple_prst_payload);
}


static const char *const ecn2str[4] =
{
    [ECN_NOT_ECT]   = "",
    [ECN_ECT0]      = "ECT(0)",
    [ECN_ECT1]      = "ECT(1)",
    [ECN_CE]        = "CE",
};


void
lsquic_acki2str (const struct ack_info *acki, char *buf, size_t bufsz)
{
    size_t off, nw;
    enum ecn ecn;
    unsigned n;

    off = 0;
    for (n = 0; n < acki->n_ranges; ++n)
    {
        nw = snprintf(buf + off, bufsz - off, "[%"PRIu64"-%"PRIu64"]",
                acki->ranges[n].high, acki->ranges[n].low);
        if (nw > bufsz - off)
            return;
        off += nw;
    }

    if (acki->flags & AI_TRUNCATED)
    {
        nw = snprintf(buf + off, bufsz - off, RANGES_TRUNCATED_STR);
        if (nw > bufsz - off)
            return;
        off += nw;
    }

    if (acki->flags & AI_ECN)
    {
        for (ecn = 1; ecn <= 3; ++ecn)
        {
            nw = snprintf(buf + off, bufsz - off, " %s: %"PRIu64"%.*s",
                        ecn2str[ecn], acki->ecn_counts[ecn], ecn < 3, ";");
            if (nw > bufsz - off)
                return;
            off += nw;
        }
    }
}


int
lsquic_gquic_gen_ver_nego_pkt (unsigned char *buf, size_t bufsz,
                        const lsquic_cid_t *cid, unsigned version_bitmask)
{
    int sz;
    unsigned char *p = buf;
    unsigned char *const pend = p + bufsz;

    CHECK_SPACE(1, p, pend);
    *p = PACKET_PUBLIC_FLAGS_VERSION | PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID;
    ++p;

    if (GQUIC_CID_LEN != cid->len)
        return -1;

    CHECK_SPACE(GQUIC_CID_LEN, p, pend);
    memcpy(p, cid->idbuf, GQUIC_CID_LEN);
    p += GQUIC_CID_LEN;

    sz = lsquic_gen_ver_tags(p, pend - p, version_bitmask);
    if (sz < 0)
        return -1;

    return p + sz - buf;
}


/* `dst' serves both as source and destination.  `src' is the new frame */
int
lsquic_merge_acks (struct ack_info *dst, const struct ack_info *src)
{
    const struct lsquic_packno_range *a, *a_end, *b, *b_end, **p;
    struct lsquic_packno_range *out, *out_end;
    unsigned i;
    int ok;
    struct lsquic_packno_range out_ranges[256];

    if (!(dst->n_ranges && src->n_ranges))
        return -1;

    a = dst->ranges;
    a_end = a + dst->n_ranges;
    b = src->ranges;
    b_end = b + src->n_ranges;
    out = out_ranges;
    out_end = out + sizeof(out_ranges) / sizeof(out_ranges[0]);

    if (a->high >= b->high)
        *out = *a;
    else
        *out = *b;

    while (1)
    {
        if (a < a_end && b < b_end)
        {
            if (a->high >= b->high)
                p = &a;
            else
                p = &b;
        }
        else if (a < a_end)
            p = &a;
        else if (b < b_end)
            p = &b;
        else
        {
            ++out;
            break;
        }

        if ((*p)->high + 1 >= out->low)
            out->low = (*p)->low;
        else if (out + 1 < out_end)
            *++out = **p;
        else
            return -1;
        ++*p;
    }

    if (src->flags & AI_ECN)
    {
        /* New ACK frame (src) should not contain ECN counts that are smaller
         * than previous ACK frame, otherwise we cannot merge.
         */
        ok = 1;
        for (i = 0; i < sizeof(src->ecn_counts)
                                        / sizeof(src->ecn_counts[0]); ++i)
            ok &= dst->ecn_counts[i] <= src->ecn_counts[i];
        if (ok)
            for (i = 0; i < sizeof(src->ecn_counts)
                                            / sizeof(src->ecn_counts[0]); ++i)
                dst->ecn_counts[i] = src->ecn_counts[i];
        else
            return -1;
    }
    dst->flags |= src->flags;
    dst->lack_delta = src->lack_delta;
    dst->n_ranges = out - out_ranges;
    memcpy(dst->ranges, out_ranges, sizeof(out_ranges[0]) * dst->n_ranges);

    return 0;
}
