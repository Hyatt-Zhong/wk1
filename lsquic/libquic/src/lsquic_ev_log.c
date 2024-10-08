/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
#ifndef WIN32
#include <arpa/inet.h>
#else
#include <vc_compat.h>
#endif
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "lsquic.h"
#include "lsquic_types.h"
#include "lsquic_int_types.h"
#include "lsquic_packet_common.h"
#include "lsquic_packet_gquic.h"
#include "lsquic_packet_in.h"
#include "lsquic_packet_out.h"
#include "lsquic_parse.h"
#include "lsquic_str.h"
#include "lsquic_enc_sess.h"
#include "lsquic_ev_log.h"
#include "lsquic_sizes.h"
#include "lsquic_trans_params.h"
#include "lsquic_util.h"
#include "lsquic_hash.h"
#include "lsquic_conn.h"

#define LSQUIC_LOGGER_MODULE LSQLM_EVENT
#include "lsquic_logger.h"


/*  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^  */
/*  ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||  */
/* Messages that do not include connection ID go above this point */

#define LSQUIC_LOG_CONN_ID cid
#define LCID(...) LSQ_LOG2(LSQ_LOG_DEBUG, __VA_ARGS__)   /* LCID: log with CID */

/* Messages that are to include connection ID go below this point */
/*  ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||  */
/*  VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV  */

void
lsquic_ev_log_packet_in (const lsquic_cid_t *cid,
                                        const lsquic_packet_in_t *packet_in)
{
    unsigned packet_sz;

    switch (packet_in->pi_flags & (PI_FROM_MINI|PI_GQUIC))
    {
    case PI_FROM_MINI|PI_GQUIC:
        LCID("RX packet #%"PRIu64" (mini)", packet_in->pi_packno);
        break;
    case PI_FROM_MINI:
        LCID("RX packet #%"PRIu64" %s (mini), ecn: %u",
            packet_in->pi_packno, lsquic_hety2str[packet_in->pi_header_type],
            lsquic_packet_in_ecn(packet_in));
        break;
    case PI_GQUIC:
        packet_sz = packet_in->pi_data_sz
            + (packet_in->pi_flags & PI_DECRYPTED ? GQUIC_PACKET_HASH_SZ : 0);
        LCID("RX packet #%"PRIu64", size: %u", packet_in->pi_packno, packet_sz);
        break;
    default:
        packet_sz = packet_in->pi_data_sz
            + (packet_in->pi_flags & PI_DECRYPTED ? IQUIC_TAG_LEN : 0);
        if (packet_in->pi_flags & PI_LOG_QL_BITS)
            LCID("RX packet #%"PRIu64" %s, size: %u; ecn: %u, spin: %d; "
                "path: %hhu; Q: %d; L: %d",
                packet_in->pi_packno, lsquic_hety2str[packet_in->pi_header_type],
                packet_sz,
                lsquic_packet_in_ecn(packet_in),
                /* spin bit value is only valid for short packet headers */
                lsquic_packet_in_spin_bit(packet_in), packet_in->pi_path_id,
                ((packet_in->pi_flags & PI_SQUARE_BIT) > 0),
                ((packet_in->pi_flags & PI_LOSS_BIT) > 0));
        else
            LCID("RX packet #%"PRIu64" %s, size: %u; ecn: %u, spin: %d; "
                "path: %hhu",
                packet_in->pi_packno, lsquic_hety2str[packet_in->pi_header_type],
                packet_sz,
                lsquic_packet_in_ecn(packet_in),
                /* spin bit value is only valid for short packet headers */
                lsquic_packet_in_spin_bit(packet_in), packet_in->pi_path_id);
        break;
    }
}


void
lsquic_ev_log_ack_frame_in (const lsquic_cid_t *cid,
                                        const struct ack_info *acki)
{
    char buf[MAX_ACKI_STR_SZ];

    lsquic_acki2str(acki, buf, sizeof(buf));
    LCID("RX ACK frame: %s", buf);
}


void
lsquic_ev_log_stream_frame_in (const lsquic_cid_t *cid,
                                        const struct stream_frame *frame)
{
    LCID("RX STREAM frame: stream %"PRIu64"; offset %"PRIu64"; size %"PRIu16
        "; fin: %d", frame->stream_id, frame->data_frame.df_offset,
        frame->data_frame.df_size, (int) frame->data_frame.df_fin);
}


void
lsquic_ev_log_crypto_frame_in (const lsquic_cid_t *cid,
                        const struct stream_frame *frame, unsigned enc_level)
{
    LCID("RX CRYPTO frame: level %u; offset %"PRIu64"; size %"PRIu16,
        enc_level, frame->data_frame.df_offset, frame->data_frame.df_size);
}


void
lsquic_ev_log_stop_waiting_frame_in (const lsquic_cid_t *cid,
                                                        lsquic_packno_t least)
{
    LCID("RX STOP_WAITING frame: least unacked packno %"PRIu64, least);
}


void
lsquic_ev_log_window_update_frame_in (const lsquic_cid_t *cid,
                                lsquic_stream_id_t stream_id, uint64_t offset)
{
    LCID("RX WINDOW_UPDATE frame: stream %"PRIu64"; offset %"PRIu64,
        stream_id, offset);
}


void
lsquic_ev_log_blocked_frame_in (const lsquic_cid_t *cid,
                                            lsquic_stream_id_t stream_id)
{
    LCID("RX BLOCKED frame: stream %"PRIu64, stream_id);
}


void
lsquic_ev_log_connection_close_frame_in (const lsquic_cid_t *cid,
                    uint64_t error_code, int reason_len, const char *reason)
{
    LCID("RX CONNECTION_CLOSE frame: error code %"PRIu64", reason: %.*s",
        error_code, reason_len, reason);
}


void
lsquic_ev_log_goaway_frame_in (const lsquic_cid_t *cid, uint32_t error_code,
            lsquic_stream_id_t stream_id, int reason_len, const char *reason)
{
    LCID("RX GOAWAY frame: error code %"PRIu32", stream %"PRIu64
        ", reason: %.*s", error_code, stream_id, reason_len, reason);
}


void
lsquic_ev_log_rst_stream_frame_in (const lsquic_cid_t *cid,
        lsquic_stream_id_t stream_id, uint64_t offset, uint64_t error_code)
{
    LCID("RX RST_STREAM frame: error code %"PRIu64", stream %"PRIu64
        ", offset: %"PRIu64, error_code, stream_id, offset);
}


void
lsquic_ev_log_stop_sending_frame_in (const lsquic_cid_t *cid,
                        lsquic_stream_id_t stream_id, uint64_t error_code)
{
    LCID("RX STOP_SENDING frame: error code %"PRIu64", stream %"PRIu64,
                                                     error_code, stream_id);
}


void
lsquic_ev_log_padding_frame_in (const lsquic_cid_t *cid, size_t len)
{
    LCID("RX PADDING frame of %zd bytes", len);
}


void
lsquic_ev_log_ping_frame_in (const lsquic_cid_t *cid)
{
    LCID("RX PING frame");
}


void
lsquic_ev_log_packet_created (const lsquic_cid_t *cid,
                                const struct lsquic_packet_out *packet_out)
{
    LCID("created packet #%"PRIu64" %s; flags: version=%d, nonce=%d, conn_id=%d",
        packet_out->po_packno,
        lsquic_hety2str[packet_out->po_header_type],
        !!(packet_out->po_flags & PO_VERSION),
        !!(packet_out->po_flags & PO_NONCE),
        !!(packet_out->po_flags & PO_CONN_ID));
}


void
lsquic_ev_log_packet_sent (const lsquic_cid_t *cid,
                                const struct lsquic_packet_out *packet_out)
{
    char frames[lsquic_frame_types_str_sz];
    if (lsquic_packet_out_verneg(packet_out))
        LCID("TX version negotiation packet, size %hu",
                                                    packet_out->po_data_sz);
    else if (lsquic_packet_out_retry(packet_out))
        LCID("TX stateless retry packet, size %hu", packet_out->po_data_sz);
    else if (lsquic_packet_out_pubres(packet_out))
        LCID("TX public reset packet, size %hu", packet_out->po_data_sz);
    else if (packet_out->po_lflags & POL_GQUIC)
        LCID("TX packet #%"PRIu64" (%s), size %hu",
            packet_out->po_packno,
                /* Frame types is a list of different frames types contained
                 * in the packet, no more.  Count and order of frames is not
                 * printed.
                 */
                lsquic_frame_types_to_str(frames, sizeof(frames),
                                          packet_out->po_frame_types),
             packet_out->po_enc_data_sz);
    else if (packet_out->po_lflags & POL_LOG_QL_BITS)
        LCID("TX packet #%"PRIu64" %s (%s), size %hu, "
            "ecn: %u, spin: %d; path: %hhu, flags: %u; "
            "Q: %u; L: %u",
            packet_out->po_packno, lsquic_hety2str[packet_out->po_header_type],
                /* Frame types is a list of different frames types contained
                 * in the packet, no more.  Count and order of frames is not
                 * printed.
                 */
                lsquic_frame_types_to_str(frames, sizeof(frames),
                                                packet_out->po_frame_types),
                packet_out->po_enc_data_sz,
                lsquic_packet_out_ecn(packet_out),
                /* spin bit value is only valid for short packet headers */
                lsquic_packet_out_spin_bit(packet_out),
                packet_out->po_path->np_path_id,
                (unsigned) packet_out->po_flags,
                lsquic_packet_out_square_bit(packet_out),
                lsquic_packet_out_loss_bit(packet_out));
    else
        LCID("TX packet #%"PRIu64" %s (%s), size %hu, "
            "ecn: %u, spin: %d; path: %hhu, flags: %u",
            packet_out->po_packno, lsquic_hety2str[packet_out->po_header_type],
                /* Frame types is a list of different frames types contained
                 * in the packet, no more.  Count and order of frames is not
                 * printed.
                 */
                lsquic_frame_types_to_str(frames, sizeof(frames),
                                                packet_out->po_frame_types),
                packet_out->po_enc_data_sz,
                lsquic_packet_out_ecn(packet_out),
                /* spin bit value is only valid for short packet headers */
                lsquic_packet_out_spin_bit(packet_out),
                packet_out->po_path->np_path_id,
                (unsigned) packet_out->po_flags);
}


void
lsquic_ev_log_packet_not_sent (const lsquic_cid_t *cid,
                                const struct lsquic_packet_out *packet_out)
{
    char frames[lsquic_frame_types_str_sz];
    LCID("unsent packet #%"PRIu64" %s, size %hu",
        packet_out->po_packno,
            /* Frame types is a list of different frames types contained in
             * the packet, no more.  Count and order of frames is not printed.
             */
            lsquic_frame_types_to_str(frames, sizeof(frames),
                                      packet_out->po_frame_types),
            packet_out->po_enc_data_sz);
}


void
lsquic_ev_log_action_stream_frame (const lsquic_cid_t *cid,
    const struct parse_funcs *pf, const unsigned char *buf, size_t bufsz,
    const char *what)
{
    struct stream_frame frame;
    int len;

    len = pf->pf_parse_stream_frame(buf, bufsz, &frame);
    if (len > 0)
        LCID("%s STREAM frame: stream %"PRIu64", offset: %"PRIu64
            ", size: %"PRIu16", fin: %d", what, frame.stream_id,
            frame.data_frame.df_offset, frame.data_frame.df_size,
            frame.data_frame.df_fin);
    else
        LSQ_LOG2(LSQ_LOG_WARN, "cannot parse STREAM frame");
}


void
lsquic_ev_log_generated_crypto_frame (const lsquic_cid_t *cid,
       const struct parse_funcs *pf, const unsigned char *buf, size_t bufsz)
{
    struct stream_frame frame;
    int len;

    len = pf->pf_parse_crypto_frame(buf, bufsz, &frame);
    if (len > 0)
        LCID("generated CRYPTO frame: offset: %"PRIu64", size: %"PRIu16,
            frame.data_frame.df_offset, frame.data_frame.df_size);
    else
        LSQ_LOG2(LSQ_LOG_WARN, "cannot parse CRYPTO frame");
}


void
lsquic_ev_log_generated_ack_frame (const lsquic_cid_t *cid,
                const struct parse_funcs *pf, const unsigned char *ack_buf,
                size_t ack_buf_sz)
{
    struct ack_info acki;
    int len;
    char buf[MAX_ACKI_STR_SZ];

    len = pf->pf_parse_ack_frame(ack_buf, ack_buf_sz, &acki,
                                                    TP_DEF_ACK_DELAY_EXP);
    if (len < 0)
    {
        LSQ_LOG2(LSQ_LOG_WARN, "cannot parse ACK frame");
        return;
    }

    lsquic_acki2str(&acki, buf, sizeof(buf));
    LCID("generated ACK frame: %s", buf);
}


void
lsquic_ev_log_generated_path_chal_frame (const lsquic_cid_t *cid,
                const struct parse_funcs *pf, const unsigned char *frame_buf,
                size_t frame_buf_sz)
{
    uint64_t chal;
    int len;
    char hexbuf[sizeof(chal) * 2 + 1];

    len = pf->pf_parse_path_chal_frame(frame_buf, frame_buf_sz, &chal);
    if (len > 0)
        LCID("generated PATH_CHALLENGE(%s) frame",
                        HEXSTR((unsigned char *) &chal, sizeof(chal), hexbuf));
    else
        LSQ_LOG2(LSQ_LOG_WARN, "cannot parse PATH_CHALLENGE frame");
}


void
lsquic_ev_log_generated_path_resp_frame (const lsquic_cid_t *cid,
                const struct parse_funcs *pf, const unsigned char *frame_buf,
                size_t frame_buf_sz)
{
    uint64_t resp;
    int len;
    char hexbuf[sizeof(resp) * 2 + 1];

    len = pf->pf_parse_path_resp_frame(frame_buf, frame_buf_sz, &resp);
    if (len > 0)
        LCID("generated PATH_RESPONSE(%s) frame",
                        HEXSTR((unsigned char *) &resp, sizeof(resp), hexbuf));
    else
        LSQ_LOG2(LSQ_LOG_WARN, "cannot parse PATH_RESPONSE frame");
}


void
lsquic_ev_log_generated_new_connection_id_frame (const lsquic_cid_t *cid,
                const struct parse_funcs *pf, const unsigned char *frame_buf,
                size_t frame_buf_sz)
{
    const unsigned char *token;
    lsquic_cid_t new_cid;
    uint64_t seqno, retire_prior_to;
    int len;
    char token_buf[IQUIC_SRESET_TOKEN_SZ * 2 + 1];
    char cid_buf[MAX_CID_LEN * 2 + 1];

    len = pf->pf_parse_new_conn_id(frame_buf, frame_buf_sz, &seqno,
                                        &retire_prior_to, &new_cid, &token);
    if (len < 0)
    {
        LSQ_LOG2(LSQ_LOG_WARN, "cannot parse NEW_CONNECTION_ID frame");
        return;
    }

    lsquic_hexstr(new_cid.idbuf, new_cid.len, cid_buf, sizeof(cid_buf));
    lsquic_hexstr(token, IQUIC_SRESET_TOKEN_SZ, token_buf, sizeof(token_buf));
    LCID("generated NEW_CONNECTION_ID frame: seqno: %"PRIu64"; retire prior "
        "to: %"PRIu64"; cid: %s; token: %s", seqno, retire_prior_to,
        cid_buf, token_buf);
}


void
lsquic_ev_log_generated_stop_waiting_frame (const lsquic_cid_t *cid,
                                            lsquic_packno_t lunack)
{
    LCID("generated STOP_WAITING frame; least unacked: %"PRIu64, lunack);
}


void
lsquic_ev_log_generated_stop_sending_frame (const lsquic_cid_t *cid,
                            lsquic_stream_id_t stream_id, uint16_t error_code)
{
    LCID("generated STOP_SENDING frame; stream ID: %"PRIu64"; error code: "
                                            "%"PRIu16, stream_id, error_code);
}


void
lsquic_ev_log_create_connection (const lsquic_cid_t *cid,
                                    const struct sockaddr *local_sa,
                                    const struct sockaddr *peer_sa)
{
    LCID("connection created");
}


void
lsquic_ev_log_hsk_completed (const lsquic_cid_t *cid)
{
    LCID("handshake completed");
}


void
lsquic_ev_log_sess_resume (const lsquic_cid_t *cid)
{
    LCID("sess_resume successful");
}


void
lsquic_ev_log_version_negotiation (const lsquic_cid_t *cid,
                                        const char *action, const char *ver)
{
    LCID("version negotiation: %s version %s", action, ver);
}
