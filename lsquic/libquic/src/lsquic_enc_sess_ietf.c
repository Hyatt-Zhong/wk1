/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * lsquic_enc_sess_ietf.c -- Crypto session for IETF QUIC
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <inttypes.h>
#if LSQUIC_PREFERRED_ADDR
#include <arpa/inet.h>
#endif

#include "fiu-local.h"

#include "lsquic_types.h"
#include "lsquic.h"
#include "lsquic_int_types.h"
#include "lsquic_sizes.h"
#include "lsquic_hash.h"
#include "lsquic_conn.h"
#include "lsquic_enc_sess.h"
#include "lsquic_parse.h"
#include "lsquic_mm.h"
#include "lsquic_engine_public.h"
#include "lsquic_packet_common.h"
#include "lsquic_packet_out.h"
#include "lsquic_packet_ietf.h"
#include "lsquic_packet_in.h"
#include "lsquic_util.h"
#include "lsquic_byteswap.h"
#include "lsquic_ev_log.h"
#include "lsquic_trans_params.h"
#include "lsquic_version.h"
#include "lsquic_ver_neg.h"
#include "lsquic_frab_list.h"
#include "lsquic_tokgen.h"
#include "lsquic_ietf.h"
#include "lsquic_alarmset.h"

#if __GNUC__
#   define UNLIKELY(cond) __builtin_expect(cond, 0)
#else
#   define UNLIKELY(cond) cond
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define LSQUIC_LOGGER_MODULE LSQLM_HANDSHAKE
#define LSQUIC_LOG_CONN_ID lsquic_conn_log_cid(enc_sess->esi_conn)
#include "lsquic_logger.h"

#define N_HSK_PAIRS (N_ENC_LEVS - 1)

struct enc_sess_iquic;
struct header_prot;

static int
setup_handshake_keys (struct enc_sess_iquic *, const lsquic_cid_t *);

static void
free_handshake_keys (struct enc_sess_iquic *);

static void
maybe_drop_SSL (struct enc_sess_iquic *);

static void
no_sess_ticket (enum alarm_id alarm_id, void *ctx,
                                  lsquic_time_t expiry, lsquic_time_t now);

static void
iquic_esfi_destroy (enc_session_t *);

static int
cry_sm_write_message (struct enc_sess_iquic *enc_sess, enum enc_level level, 
                      const uint8_t *data, size_t len);

static int
cry_sm_flush_flight (struct enc_sess_iquic *enc_sess);

static int
set_secret (struct enc_sess_iquic *enc_sess, enum enc_level enc_level, int rw);

struct header_prot
{
    enum enc_level      hp_enc_level;
    enum {
        HP_CAN_READ  = 1 << 0,
        HP_CAN_WRITE = 1 << 1,
    }                   hp_flags;
};

#define header_prot_inited(hp_, rw_) ((hp_)->hp_flags & (1 << (rw_)))


struct hsk_crypto
{
    struct header_prot     hp;
};


struct enc_sess_iquic
{
    struct lsquic_engine_public
                        *esi_enpub;
    struct lsquic_conn  *esi_conn;
    void               **esi_streams;
    const struct crypto_stream_if *esi_cryst_if;
    const struct ver_neg
                        *esi_ver_neg;
    unsigned char        esi_ssl_flag;

    /* These are used for forward encryption key phase 0 and 1 */
    struct header_prot   esi_hp;
    /* These are used during handshake.  There are three of them. */
    struct hsk_crypto   *esi_hsk_crypto;
    lsquic_packno_t      esi_max_packno[N_PNS];
    lsquic_cid_t         esi_odcid;
    lsquic_cid_t         esi_rscid; /* Retry SCID */
    lsquic_cid_t         esi_iscid; /* Initial SCID */
    enum {
        ESI_UNUSED0      = 1 << 0,
        ESI_LOG_SECRETS  = 1 << 1,
        ESI_HANDSHAKE_OK = 1 << 2,
        ESI_ODCID        = 1 << 3,
        ESI_ON_WRITE     = 1 << 4,
        ESI_SERVER       = 1 << 5,
        ESI_USE_SSL_TICKET = 1 << 6,
        ESI_HAVE_PEER_TP = 1 << 7,
        ESI_ALPN_CHECKED = 1 << 8,
        ESI_CACHED_INFO  = 1 << 9,
        ESI_HSK_CONFIRMED= 1 << 10,
        ESI_WANT_TICKET  = 1 << 11,
        ESI_RECV_QL_BITS = 1 << 12,
        ESI_SEND_QL_BITS = 1 << 13,
        ESI_RSCID        = 1 << 14,
        ESI_ISCID        = 1 << 15,
        ESI_RETRY        = 1 << 16, /* Connection was retried */
        ESI_MAX_PACKNO_INIT = 1 << 17,
        ESI_MAX_PACKNO_HSK  = ESI_MAX_PACKNO_INIT << PNS_HSK,
        ESI_MAX_PACKNO_APP  = ESI_MAX_PACKNO_INIT << PNS_APP,
        ESI_HAVE_0RTT_TP = 1 << 20,
        ESI_SWITCH_VER   = 1 << 21,
    }                    esi_flags;
    enum enc_level       esi_last_w;
    /* We never use the first two levels, so it seems we could reduce the
     * memory requirement here at the cost of adding some code.
     */
    struct frab_list     esi_frals[N_ENC_LEVS];
    struct transport_params
                         esi_peer_tp;
    struct lsquic_alarmset
                        *esi_alset;
    unsigned             esi_max_streams_uni;
    unsigned char        esi_grease;
    signed char          esi_have_forw;
    unsigned char        esi_tp_buf[sizeof(struct transport_params)];
    size_t               esi_tp_len;
    unsigned char        esi_peer_tp_buf[sizeof(struct transport_params)];
    size_t               esi_peer_tp_len;
};


static lsquic_packno_t
decode_packno (lsquic_packno_t max_packno, lsquic_packno_t packno,
                                                                unsigned shift)
{
    lsquic_packno_t candidates[3], epoch_delta;
    int64_t diffs[3];
    unsigned min;;

    epoch_delta = 1ULL << shift;
    candidates[1] = (max_packno & ~(epoch_delta - 1)) + packno;
    candidates[0] = candidates[1] - epoch_delta;
    candidates[2] = candidates[1] + epoch_delta;

    diffs[0] = llabs((int64_t) candidates[0] - (int64_t) max_packno);
    diffs[1] = llabs((int64_t) candidates[1] - (int64_t) max_packno);
    diffs[2] = llabs((int64_t) candidates[2] - (int64_t) max_packno);

    min = diffs[1] < diffs[0];
    if (diffs[2] < diffs[min])
        min = 2;

    return candidates[min];
}


static lsquic_packno_t
get_packno (struct enc_sess_iquic *enc_sess,
        struct header_prot *hp,
        unsigned char *dst, unsigned packno_off,
        unsigned *packno_len)
{
    enum packnum_space pns;
    lsquic_packno_t packno;
    unsigned shift;

    packno = 0;
    shift = 0;
    *packno_len = 1 + (dst[0] & 3);
    switch (*packno_len)
    {
    case 4:
        packno |= dst[packno_off + 3];
        shift += 8;
        /* fall-through */
    case 3:
        packno |= (unsigned) dst[packno_off + 2] << shift;
        shift += 8;
        /* fall-through */
    case 2:
        packno |= (unsigned) dst[packno_off + 1] << shift;
        shift += 8;
        /* fall-through */
    default:
        packno |= (unsigned) dst[packno_off + 0] << shift;
        shift += 8;
    }
    pns = lsquic_enclev2pns[hp->hp_enc_level];
    if (enc_sess->esi_flags & (ESI_MAX_PACKNO_INIT << pns))
    {
        LSQ_DEBUG("pre-decode packno: %"PRIu64, packno);
        return decode_packno(enc_sess->esi_max_packno[pns], packno, shift);
    }
    else
    {
        LSQ_DEBUG("first packet in %s, packno: %"PRIu64, lsquic_pns2str[pns],
                                                                        packno);
        return packno;
    }
}


static void
set_tp_version_info (struct transport_params *params,
                     unsigned versions, enum lsquic_version ver)
{
    assert(params->tp_version_cnt == 0);
    params->tp_version_info[params->tp_version_cnt++] = ver;
    if (versions & (1 << LSQVER_I002))
        params->tp_version_info[params->tp_version_cnt++] = LSQVER_I002;
    if (versions & (1 << LSQVER_I001))
        params->tp_version_info[params->tp_version_cnt++] = LSQVER_I001;
    params->tp_set |= 1 << TPI_VERSION_INFORMATION;
}


static int
gen_trans_params (struct enc_sess_iquic *enc_sess, unsigned char *buf,
                                                                size_t bufsz)
{
    const struct lsquic_engine_settings *const settings =
                                    &enc_sess->esi_enpub->enp_settings;
    struct transport_params params;
    const enum lsquic_version version = enc_sess->esi_conn->cn_version;
    int len;

    memset(&params, 0, sizeof(params));
    if (version > LSQVER_ID27)
    {
        params.tp_initial_source_cid = *CN_SCID(enc_sess->esi_conn);
        params.tp_set |= 1 << TPI_INITIAL_SOURCE_CID;
    }
    if (enc_sess->esi_flags & ESI_SERVER)
    {
        const struct lsquic_conn *const lconn = enc_sess->esi_conn;

        params.tp_set |= 1 << TPI_STATELESS_RESET_TOKEN;
        lsquic_tg_generate_sreset(enc_sess->esi_enpub->enp_tokgen,
            CN_SCID(lconn), params.tp_stateless_reset_token);

        if (enc_sess->esi_flags & ESI_ODCID)
        {
            params.tp_original_dest_cid = enc_sess->esi_odcid;
            params.tp_set |= 1 << TPI_ORIGINAL_DEST_CID;
        }
        if (enc_sess->esi_flags & ESI_RSCID)
        {
            params.tp_retry_source_cid = enc_sess->esi_rscid;
            params.tp_set |= 1 << TPI_RETRY_SOURCE_CID;
        }
#if LSQUIC_PREFERRED_ADDR
        char addr_buf[INET6_ADDRSTRLEN + 6 /* port */ + 1];
        const char *s, *colon;
        struct lsquic_conn *conn;
        struct conn_cid_elem *cce;
        unsigned seqno;
        s = getenv("LSQUIC_PREFERRED_ADDR4");
        if (s && strlen(s) < sizeof(addr_buf) && (colon = strchr(s, ':')))
        {
            strncpy(addr_buf, s, colon - s);
            addr_buf[colon - s] = '\0';
            inet_pton(AF_INET, addr_buf, params.tp_preferred_address.ipv4_addr);
            params.tp_preferred_address.ipv4_port = atoi(colon + 1);
            params.tp_set |= 1 << TPI_PREFERRED_ADDRESS;
        }
        s = getenv("LSQUIC_PREFERRED_ADDR6");
        if (s && strlen(s) < sizeof(addr_buf) && (colon = strrchr(s, ':')))
        {
            strncpy(addr_buf, s, colon - s);
            addr_buf[colon - s] = '\0';
            inet_pton(AF_INET6, addr_buf,
                                        params.tp_preferred_address.ipv6_addr);
            params.tp_preferred_address.ipv6_port = atoi(colon + 1);
            params.tp_set |= 1 << TPI_PREFERRED_ADDRESS;
        }
        conn = enc_sess->esi_conn;
        if ((params.tp_set & (1 << TPI_PREFERRED_ADDRESS))
                            && (1 << conn->cn_n_cces) - 1 != conn->cn_cces_mask)
        {
            seqno = 0;
            for (cce = lconn->cn_cces; cce < END_OF_CCES(lconn); ++cce)
            {
                if (lconn->cn_cces_mask & (1 << (cce - lconn->cn_cces)))
                {
                    if ((cce->cce_flags & CCE_SEQNO) && cce->cce_seqno > seqno)
                        seqno = cce->cce_seqno;
                }
                else
                    break;
            }
            if (cce == END_OF_CCES(lconn))
            {
                goto cant_use_prefaddr;
            }
            cce->cce_seqno = seqno + 1;
            cce->cce_flags = CCE_SEQNO;

            enc_sess->esi_enpub->enp_generate_scid(
                enc_sess->esi_enpub->enp_gen_scid_ctx, enc_sess->esi_conn,
                &cce->cce_cid, enc_sess->esi_enpub->enp_settings.es_scid_len);

            /* Don't add to hash: migration must not start until *after*
             * handshake is complete.
             */
            conn->cn_cces_mask |= 1 << (cce - conn->cn_cces);
            params.tp_preferred_address.cid = cce->cce_cid;
            lsquic_tg_generate_sreset(enc_sess->esi_enpub->enp_tokgen,
                &params.tp_preferred_address.cid,
                params.tp_preferred_address.srst);
        }
        else
        {
  cant_use_prefaddr:
            params.tp_set &= ~(1 << TPI_PREFERRED_ADDRESS);
        }
#endif
    }
    params.tp_init_max_data = settings->es_init_max_data;
    params.tp_init_max_stream_data_bidi_local
                            = settings->es_init_max_stream_data_bidi_local;
    params.tp_init_max_stream_data_bidi_remote
                            = settings->es_init_max_stream_data_bidi_remote;
    params.tp_init_max_stream_data_uni
                            = settings->es_init_max_stream_data_uni;
    params.tp_init_max_streams_uni
                            = enc_sess->esi_max_streams_uni;
    params.tp_init_max_streams_bidi
                            = settings->es_init_max_streams_bidi;
    params.tp_ack_delay_exponent
                            = TP_DEF_ACK_DELAY_EXP;
    params.tp_max_idle_timeout = settings->es_idle_timeout * 1000;
    params.tp_max_ack_delay = TP_DEF_MAX_ACK_DELAY;
    params.tp_active_connection_id_limit = MAX_IETF_CONN_DCIDS;
    params.tp_set |= (1 << TPI_INIT_MAX_DATA)
                  |  (1 << TPI_INIT_MAX_STREAM_DATA_BIDI_LOCAL)
                  |  (1 << TPI_INIT_MAX_STREAM_DATA_BIDI_REMOTE)
                  |  (1 << TPI_INIT_MAX_STREAM_DATA_UNI)
                  |  (1 << TPI_INIT_MAX_STREAMS_UNI)
                  |  (1 << TPI_INIT_MAX_STREAMS_BIDI)
                  |  (1 << TPI_ACK_DELAY_EXPONENT)
                  |  (1 << TPI_MAX_IDLE_TIMEOUT)
                  |  (1 << TPI_MAX_ACK_DELAY)
                  |  (1 << TPI_ACTIVE_CONNECTION_ID_LIMIT)
                  ;
    if (settings->es_max_udp_payload_size_rx)
    {
        params.tp_max_udp_payload_size = settings->es_max_udp_payload_size_rx;
        params.tp_set |= 1 << TPI_MAX_UDP_PAYLOAD_SIZE;
    }
    if (!settings->es_allow_migration)
        params.tp_set |= 1 << TPI_DISABLE_ACTIVE_MIGRATION;
    if (settings->es_ql_bits)
    {
        params.tp_loss_bits = settings->es_ql_bits - 1;
        params.tp_set |= 1 << TPI_LOSS_BITS;
    }
    if (settings->es_delayed_acks)
    {
        params.tp_numerics[TPI_MIN_ACK_DELAY] = TP_MIN_ACK_DELAY;
        params.tp_set |= 1 << TPI_MIN_ACK_DELAY;
        params.tp_numerics[TPI_MIN_ACK_DELAY_02] = TP_MIN_ACK_DELAY;
        params.tp_set |= 1 << TPI_MIN_ACK_DELAY_02;
    }
    if (settings->es_timestamps)
    {
        params.tp_numerics[TPI_TIMESTAMPS] = TS_GENERATE_THEM;
        params.tp_set |= 1 << TPI_TIMESTAMPS;
    }
    if (settings->es_datagrams)
    {
        if (params.tp_set & (1 << TPI_MAX_UDP_PAYLOAD_SIZE))
            params.tp_numerics[TPI_MAX_DATAGRAM_FRAME_SIZE]
                                            = params.tp_max_udp_payload_size;
        else
            params.tp_numerics[TPI_MAX_DATAGRAM_FRAME_SIZE]
                                            = TP_DEF_MAX_UDP_PAYLOAD_SIZE;
        params.tp_set |= 1 << TPI_MAX_DATAGRAM_FRAME_SIZE;
    }

    if (enc_sess->esi_ver_neg && enc_sess->esi_ver_neg->vn_supp
                                != (1u << enc_sess->esi_ver_neg->vn_ver))
        set_tp_version_info(&params, enc_sess->esi_ver_neg->vn_supp,
                            enc_sess->esi_ver_neg->vn_ver);
    else if (enc_sess->esi_conn->cn_flags & LSCONN_VER_UPDATED)
        set_tp_version_info(&params, settings->es_versions,
                            enc_sess->esi_conn->cn_version);

    len = (version == LSQVER_ID27 ? lsquic_tp_encode_27 : lsquic_tp_encode)(
                        &params, enc_sess->esi_flags & ESI_SERVER, buf, bufsz);
    if (len >= 0)
    {
        char str[MAX_TP_STR_SZ];
        LSQ_DEBUG("generated transport parameters buffer of %d bytes", len);
        LSQ_DEBUG("%s", ((version == LSQVER_ID27 ? lsquic_tp_to_str_27
                        : lsquic_tp_to_str)(&params, str, sizeof(str)), str));
    }
    else
        LSQ_WARN("cannot generate transport parameters: %d", errno);
    return len;
}


/*
 * Format:
 *      uint32_t    lsquic_ver_tag_t
 *      uint32_t    encoder version
 *      uint32_t    ticket_size
 *      uint8_t     ticket_buf[ ticket_size ]
 *      uint32_t    trapa_size
 *      uint8_t     trapa_buf[ trapa_size ]
 */


static void
init_frals (struct enc_sess_iquic *enc_sess)
{
    struct frab_list *fral;

    for (fral = enc_sess->esi_frals; fral < enc_sess->esi_frals
            + sizeof(enc_sess->esi_frals) / sizeof(enc_sess->esi_frals[0]);
                ++fral)
        lsquic_frab_list_init(fral, 0x100, NULL, NULL, NULL);
}


static enc_session_t *
iquic_esfi_create_client (const char *hostname,
            struct lsquic_engine_public *enpub, struct lsquic_conn *lconn,
            const lsquic_cid_t *dcid, const struct ver_neg *ver_neg,
            void *crypto_streams[4], const struct crypto_stream_if *cryst_if,
            const unsigned char *sess_resume, size_t sess_resume_sz,
            struct lsquic_alarmset *alset, unsigned max_streams_uni,
            void* peer_ctx)
{
    struct enc_sess_iquic *enc_sess;
    int transpa_len;
    unsigned char trans_params[0x80];

    fiu_return_on("enc_sess_ietf/create_client", NULL);

    enc_sess = calloc(1, sizeof(*enc_sess));
    if (!enc_sess)
        return NULL;

    enc_sess->esi_enpub = enpub;
    enc_sess->esi_streams = crypto_streams;
    enc_sess->esi_cryst_if = cryst_if;
    enc_sess->esi_conn = lconn;
    enc_sess->esi_ver_neg = ver_neg;

    enc_sess->esi_odcid = *dcid;
    enc_sess->esi_flags |= ESI_ODCID;
    enc_sess->esi_grease = 0xFF;

    init_frals(enc_sess);

    if (0 != setup_handshake_keys(enc_sess, dcid))
    {
        free(enc_sess);
        return NULL;
    }

    enc_sess->esi_max_streams_uni = max_streams_uni;

    enc_sess->esi_ssl_flag = 1;

    transpa_len = gen_trans_params(enc_sess, trans_params,
                                                    sizeof(trans_params));
    if (transpa_len < 0)
    {
        goto err;
    }

    memcpy(enc_sess->esi_tp_buf, trans_params, transpa_len);
    enc_sess->esi_tp_len = transpa_len;

    enc_sess->esi_alset = alset;
    lsquic_alarmset_init_alarm(enc_sess->esi_alset, AL_SESS_TICKET,
                                            no_sess_ticket, enc_sess);

    return enc_sess;

  err:
    if (enc_sess)
        iquic_esfi_destroy(enc_sess);
    return NULL;
}


static enc_session_t *
iquic_esfi_create_server (struct lsquic_engine_public *enpub,
                    struct lsquic_conn *lconn, const lsquic_cid_t *first_dcid,
                    void *(crypto_streams)[4],
                    const struct crypto_stream_if *cryst_if,
                    const struct lsquic_cid *odcid,
                    const struct lsquic_cid *iscid,
                    const struct lsquic_cid *rscid)
{
    struct enc_sess_iquic *enc_sess;

    enc_sess = calloc(1, sizeof(*enc_sess));
    if (!enc_sess)
        return NULL;

    enc_sess->esi_flags = ESI_SERVER;
    enc_sess->esi_streams = crypto_streams;
    enc_sess->esi_cryst_if = cryst_if;
    enc_sess->esi_enpub = enpub;
    enc_sess->esi_conn = lconn;
    enc_sess->esi_grease = 0xFF;

    if (odcid)
    {
        enc_sess->esi_odcid = *odcid;
        enc_sess->esi_flags |= ESI_ODCID;
    }
    if (rscid)
    {
        enc_sess->esi_rscid = *rscid;
        enc_sess->esi_flags |= ESI_RSCID;
    }
    enc_sess->esi_iscid = *iscid;
    enc_sess->esi_flags |= ESI_ISCID;

    init_frals(enc_sess);

    {
        const char *log;
        log = getenv("LSQUIC_LOG_SECRETS");
        if (log)
        {
            if (atoi(log))
                enc_sess->esi_flags |= ESI_LOG_SECRETS;
            LSQ_DEBUG("will %slog secrets", atoi(log) ? "" : "not ");
        }
    }

    if (0 != setup_handshake_keys(enc_sess, first_dcid))
    {
        free(enc_sess);
        return NULL;
    }

    enc_sess->esi_max_streams_uni
        = enpub->enp_settings.es_init_max_streams_uni;

    return enc_sess;
}


/* [draft-ietf-quic-tls-12] Section 5.3.2 */
static int
setup_handshake_keys (struct enc_sess_iquic *enc_sess, const lsquic_cid_t *cid)
{
    struct header_prot *hp;
    unsigned i;

    if (!enc_sess->esi_hsk_crypto)
    {
        enc_sess->esi_hsk_crypto = calloc(N_HSK_PAIRS,
                                          sizeof(enc_sess->esi_hsk_crypto[0]));
        if (!enc_sess->esi_hsk_crypto)
            return -1;
    }
    hp = &enc_sess->esi_hsk_crypto[ENC_LEV_INIT].hp;

    hp->hp_enc_level = ENC_LEV_INIT;
    for (i = 0; i < 2; ++i)
        hp->hp_flags |= 1 << i;

    return 0;
}


static void
free_handshake_keys (struct enc_sess_iquic *enc_sess)
{
    if (enc_sess->esi_hsk_crypto)
    {
        free(enc_sess->esi_hsk_crypto);
        enc_sess->esi_hsk_crypto = NULL;
    }
}


static void
iquic_esf_set_conn (enc_session_t *enc_session_p, struct lsquic_conn *lconn)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;
    enc_sess->esi_conn = lconn;
    LSQ_DEBUG("updated conn reference");
}


int
iquic_esfi_init_server_tp (struct enc_sess_iquic *const enc_sess)
{
    unsigned char trans_params[sizeof(struct transport_params)];
    int transpa_len;
    transpa_len = gen_trans_params(enc_sess, trans_params,
                                   sizeof(trans_params));
    if (transpa_len < 0)
        return -1;

    memcpy(enc_sess->esi_tp_buf, trans_params, transpa_len);
    enc_sess->esi_tp_len = transpa_len;

    return 0;
}


static int
iquic_esfi_init_server (enc_session_t *enc_session_p)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;
    enc_sess->esi_ssl_flag = 1;
    return 0;
}


/* [draft-ietf-quic-transport-31] Section 7.4.1:
 " If 0-RTT data is accepted by the server, the server MUST NOT reduce
 " any limits or alter any values that might be violated by the client
 " with its 0-RTT data.  In particular, a server that accepts 0-RTT data
 " MUST NOT set values for the following parameters (Section 18.2) that
 " are smaller than the remembered value of the parameters.
 "
 " *  active_connection_id_limit
 "
 " *  initial_max_data
 "
 " *  initial_max_stream_data_bidi_local
 "
 " *  initial_max_stream_data_bidi_remote
 "
 " *  initial_max_stream_data_uni
 "
 " *  initial_max_streams_bidi
 "
 " *  initial_max_streams_uni
 */
#define REDUCTION_PROHIBITED_TPS                                     (0 \
    | (1 << TPI_ACTIVE_CONNECTION_ID_LIMIT)                             \
    | (1 << TPI_INIT_MAX_DATA)                                          \
    | (1 << TPI_INIT_MAX_STREAMS_UNI)                                   \
    | (1 << TPI_INIT_MAX_STREAMS_BIDI)                                  \
    | (1 << TPI_INIT_MAX_STREAM_DATA_BIDI_LOCAL)                        \
    | (1 << TPI_INIT_MAX_STREAM_DATA_BIDI_REMOTE)                       \
    | (1 << TPI_INIT_MAX_STREAM_DATA_UNI)                               \
)


static int
check_server_tps_for_violations (const struct enc_sess_iquic *enc_sess,
                            const struct transport_params *params_0rtt,
                            const struct transport_params *new_params)
{
    enum transport_param_id tpi;

    for (tpi = 0; tpi <= MAX_NUMERIC_TPI; ++tpi)
        if ((1 << tpi) & REDUCTION_PROHIBITED_TPS)
            if (new_params->tp_numerics[tpi] < params_0rtt->tp_numerics[tpi])
            {
                LSQ_INFO("server's new TP %s decreased in value from %"PRIu64
                    " to %"PRIu64, lsquic_tpi2str[tpi],
                        params_0rtt->tp_numerics[tpi],
                        new_params->tp_numerics[tpi]);
                return -1;
            }

    LSQ_DEBUG("server's new transport parameters do not violate save 0-RTT "
        "parameters");
    return 0;
}


static int
get_peer_transport_params (struct enc_sess_iquic *enc_sess)
{
    struct transport_params *const trans_params = &enc_sess->esi_peer_tp;
    struct transport_params params_0rtt;
    const uint8_t *params_buf;
    size_t bufsz;
    char *params_str;
    const enum lsquic_version version = enc_sess->esi_conn->cn_version;
    int have_0rtt_tp;

    params_buf = enc_sess->esi_peer_tp_buf;
    bufsz = enc_sess->esi_peer_tp_len;

    have_0rtt_tp = !!(enc_sess->esi_flags & ESI_HAVE_0RTT_TP);
    if (have_0rtt_tp)
    {
        params_0rtt = enc_sess->esi_peer_tp;
        enc_sess->esi_flags &= ~ESI_HAVE_0RTT_TP;
    }

    LSQ_DEBUG("have peer transport parameters (%zu bytes)", bufsz);
    if (LSQ_LOG_ENABLED(LSQ_LOG_DEBUG))
    {
        params_str = lsquic_mm_get_4k(&enc_sess->esi_enpub->enp_mm);
        if (params_str)
        {
            lsquic_hexdump(params_buf, bufsz, params_str, 0x1000);
            LSQ_DEBUG("transport parameters (%zd bytes):\n%s", bufsz,
                                                            params_str);
            lsquic_mm_put_4k(&enc_sess->esi_enpub->enp_mm, params_str);
        }
    }
    if (0 > (version == LSQVER_ID27 ? lsquic_tp_decode_27
                : lsquic_tp_decode)(params_buf, bufsz,
                            !(enc_sess->esi_flags & ESI_SERVER),
                                                trans_params))
    {
        if (LSQ_LOG_ENABLED(LSQ_LOG_DEBUG))
        {
            params_str = lsquic_mm_get_4k(&enc_sess->esi_enpub->enp_mm);
            if (params_str)
            {
                lsquic_hexdump(params_buf, bufsz, params_str, 0x1000);
                LSQ_DEBUG("could not parse peer transport parameters "
                    "(%zd bytes):\n%s", bufsz, params_str);
                lsquic_mm_put_4k(&enc_sess->esi_enpub->enp_mm, params_str);
            }
            else
                LSQ_DEBUG("could not parse peer transport parameters "
                    "(%zd bytes)", bufsz);
        }
        return -1;
    }

    if (have_0rtt_tp && 0 != check_server_tps_for_violations(enc_sess,
                                                &params_0rtt, trans_params))
        return -1;

    const lsquic_cid_t *const cids[LAST_TPI + 1] = {
        [TP_CID_IDX(TPI_ORIGINAL_DEST_CID)]  = enc_sess->esi_flags & ESI_ODCID ? &enc_sess->esi_odcid : NULL,
        [TP_CID_IDX(TPI_RETRY_SOURCE_CID)]   = enc_sess->esi_flags & ESI_RSCID ? &enc_sess->esi_rscid : NULL,
        [TP_CID_IDX(TPI_INITIAL_SOURCE_CID)] = enc_sess->esi_flags & ESI_ISCID ? &enc_sess->esi_iscid : NULL,
    };

    unsigned must_have, must_not_have = 0;
    if (version > LSQVER_ID27)
    {
        must_have = 1 << TPI_INITIAL_SOURCE_CID;
        if (enc_sess->esi_flags & ESI_SERVER)
            must_not_have |= 1 << TPI_ORIGINAL_DEST_CID;
        else
            must_have |= 1 << TPI_ORIGINAL_DEST_CID;
        if ((enc_sess->esi_flags & (ESI_RETRY|ESI_SERVER)) == ESI_RETRY)
            must_have |= 1 << TPI_RETRY_SOURCE_CID;
        else
            must_not_have |= 1 << TPI_RETRY_SOURCE_CID;
    }
    else if ((enc_sess->esi_flags & (ESI_RETRY|ESI_SERVER)) == ESI_RETRY)
        must_have = 1 << TPI_ORIGINAL_DEST_CID;
    else
        must_have = 0;

    enum transport_param_id tpi;
    for (tpi = FIRST_TP_CID; tpi <= LAST_TP_CID; ++tpi)
    {
        if (!(must_have & (1 << tpi)))
            continue;
        if (!(trans_params->tp_set & (1 << tpi)))
        {
            LSQ_DEBUG("server did not produce %s", lsquic_tpi2str[tpi]);
            return -1;
        }
        if (!cids[TP_CID_IDX(tpi)])
        {
            LSQ_WARN("do not have CID %s for checking",
                                                    lsquic_tpi2str[tpi]);
            return -1;
        }
        if (LSQUIC_CIDS_EQ(cids[TP_CID_IDX(tpi)],
                                &trans_params->tp_cids[TP_CID_IDX(tpi)]))
            LSQ_DEBUG("%s values match", lsquic_tpi2str[tpi]);
        else
        {
            if (LSQ_LOG_ENABLED(LSQ_LOG_DEBUG))
            {
                char cidbuf[2][MAX_CID_LEN * 2 + 1];
                LSQ_DEBUG("server provided %s %"CID_FMT" that does not "
                    "match ours %"CID_FMT, lsquic_tpi2str[tpi],
                    CID_BITS_B(&trans_params->tp_cids[TP_CID_IDX(tpi)],
                                                            cidbuf[0]),
                    CID_BITS_B(cids[TP_CID_IDX(tpi)], cidbuf[1]));
            }
            enc_sess->esi_conn->cn_flags |= LSCONN_NO_BL;
            return -1;
        }
    }

    for (tpi = FIRST_TP_CID; tpi <= LAST_TP_CID; ++tpi)
        if (must_not_have & (1 << tpi) & trans_params->tp_set)
        {
            LSQ_DEBUG("server transport parameters unexpectedly contain %s",
                                        lsquic_tpi2str[tpi]);
            return -1;
        }

    if ((trans_params->tp_set & (1 << TPI_LOSS_BITS))
                            && enc_sess->esi_enpub->enp_settings.es_ql_bits)
    {
        const unsigned our_loss_bits
            = enc_sess->esi_enpub->enp_settings.es_ql_bits - 1;
        switch ((our_loss_bits << 1) | trans_params->tp_loss_bits)
        {
        case    (0             << 1) | 0:
            LSQ_DEBUG("both sides only tolerate QL bits: don't enable them");
            break;
        case    (0             << 1) | 1:
            LSQ_DEBUG("peer sends QL bits, we receive them");
            enc_sess->esi_flags |= ESI_RECV_QL_BITS;
            break;
        case    (1             << 1) | 0:
            LSQ_DEBUG("we send QL bits, peer receives them");
            enc_sess->esi_flags |= ESI_SEND_QL_BITS;
            break;
        default/*1             << 1) | 1*/:
            LSQ_DEBUG("enable sending and receiving QL bits");
            enc_sess->esi_flags |= ESI_RECV_QL_BITS;
            enc_sess->esi_flags |= ESI_SEND_QL_BITS;
            break;
        }
    }
    else
        LSQ_DEBUG("no QL bits");

    if (trans_params->tp_set & (1 << TPI_GREASE_QUIC_BIT))
    {
        if (enc_sess->esi_enpub->enp_settings.es_grease_quic_bit)
        {
            LSQ_DEBUG("will grease the QUIC bit");
            enc_sess->esi_grease = ~QUIC_BIT;
        }
        else
            LSQ_DEBUG("greasing turned off: won't grease the QUIC bit");
    }

    return 0;
}


static int
maybe_get_peer_transport_params (struct enc_sess_iquic *enc_sess)
{
    int s;

    if (enc_sess->esi_flags & ESI_HAVE_PEER_TP)
        return 0;

    s = get_peer_transport_params(enc_sess);
    if (s == 0)
        enc_sess->esi_flags |= ESI_HAVE_PEER_TP;

    return s;
}


enum iquic_handshake_status {
    IHS_WANT_READ,
    IHS_WANT_WRITE,
    IHS_WANT_RW,
    IHS_STOP,
};


static enum iquic_handshake_status
iquic_esfi_handshake (struct enc_sess_iquic *enc_sess, unsigned rw, enum enc_level now_level)
{
    enum lsquic_hsk_status hsk_status;
    unsigned char data[sizeof(struct transport_params)];
    int len;
    unsigned level = enc_sess->esi_last_w;

    if (rw == 1) {
        len = enc_sess->esi_tp_len;
        memcpy(data, enc_sess->esi_tp_buf, len);
        cry_sm_write_message(enc_sess, level, data, len);
        cry_sm_flush_flight(enc_sess);

        return IHS_WANT_READ;
    } else {
        char *custom_crypto = "PUWELL";
        int custom_crypto_len = strlen(custom_crypto);

        if (enc_sess->esi_flags & ESI_SERVER) {
            if (now_level == 0) {
                len = custom_crypto_len;
                memcpy(data, custom_crypto, len);
                cry_sm_write_message(enc_sess, level, data, len);
                set_secret(enc_sess, 2, 1);

                level = 2;
                len = enc_sess->esi_tp_len;
                memcpy(data, enc_sess->esi_tp_buf, len);
                cry_sm_write_message(enc_sess, level, data, len);
                set_secret(enc_sess, 3, 1);

                cry_sm_flush_flight(enc_sess);
                set_secret(enc_sess, 2, 0);
                
                return IHS_WANT_READ;
            } else {
                level = 3;
                len = custom_crypto_len;
                memcpy(data, custom_crypto, len);
                cry_sm_write_message(enc_sess, level, data, len);
                set_secret(enc_sess, 3, 0);
                cry_sm_flush_flight(enc_sess);
            }
        } else {
            if (now_level == 0) {
                set_secret(enc_sess, 2, 1);
                set_secret(enc_sess, 2, 0);
                return IHS_WANT_READ;
            } else {
                level = 2;
                len = custom_crypto_len;
                memcpy(data, custom_crypto, len);
                cry_sm_write_message(enc_sess, level, data, len);
                set_secret(enc_sess, 3, 1);
                set_secret(enc_sess, 3, 0);
                cry_sm_flush_flight(enc_sess);
            }
        }
    }

    hsk_status = LSQ_HSK_OK;
    LSQ_DEBUG("handshake reported complete");
    EV_LOG_HSK_COMPLETED(LSQUIC_LOG_CONN_ID);
    /* The ESI_USE_SSL_TICKET flag indicates if the client attempted session
     * resumption.  If the handshake is complete, and the client attempted
     * session resumption, it must have succeeded.
     */
    if (enc_sess->esi_flags & ESI_USE_SSL_TICKET)
    {
        hsk_status = LSQ_HSK_RESUMED_OK;
        EV_LOG_SESSION_RESUMPTION(LSQUIC_LOG_CONN_ID);
    }

    if (0 != maybe_get_peer_transport_params(enc_sess))
    {
        hsk_status = LSQ_HSK_FAIL;
        goto err;
    }

    enc_sess->esi_flags |= ESI_HANDSHAKE_OK;
    enc_sess->esi_conn->cn_if->ci_hsk_done(enc_sess->esi_conn, hsk_status);

    return IHS_STOP;    /* XXX: what else can come on the crypto stream? */

  err:
    LSQ_DEBUG("handshake failed");
    enc_sess->esi_conn->cn_if->ci_hsk_done(enc_sess->esi_conn, hsk_status);
    return IHS_STOP;
}


static struct transport_params *
iquic_esfi_get_peer_transport_params (enc_session_t *enc_session_p)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;

    if (enc_sess->esi_flags & ESI_HAVE_0RTT_TP)
        return &enc_sess->esi_peer_tp;
    else if (0 == maybe_get_peer_transport_params(enc_sess))
        return &enc_sess->esi_peer_tp;
    else
        return NULL;
}


static void
iquic_esfi_destroy (enc_session_t *enc_session_p)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;
    struct frab_list *fral;
    LSQ_DEBUG("iquic_esfi_destroy");

    for (fral = enc_sess->esi_frals; fral < enc_sess->esi_frals
            + sizeof(enc_sess->esi_frals) / sizeof(enc_sess->esi_frals[0]);
                ++fral)
        lsquic_frab_list_cleanup(fral);

    free_handshake_keys(enc_sess);

    free(enc_sess);
}


/* See [draft-ietf-quic-tls-14], Section 4 */
static const enum enc_level hety2el[] =
{
    [HETY_SHORT]     = ENC_LEV_APP,
    [HETY_VERNEG]    = 0,
    [HETY_INITIAL]   = ENC_LEV_INIT,
    [HETY_RETRY]     = 0,
    [HETY_HANDSHAKE] = ENC_LEV_HSK,
    [HETY_0RTT]      = ENC_LEV_0RTT,
};


static const enum enc_level pns2enc_level[2][N_PNS] =
{
    [0] = {
        [PNS_INIT]  = ENC_LEV_INIT,
        [PNS_HSK]   = ENC_LEV_HSK,
        [PNS_APP]   = ENC_LEV_0RTT,
    },
    [1] = {
        [PNS_INIT]  = ENC_LEV_INIT,
        [PNS_HSK]   = ENC_LEV_HSK,
        [PNS_APP]   = ENC_LEV_APP,
    },
};


int
iquic_esf_is_enc_level_ready (enc_session_t *enc_session_p,
                              enum enc_level level)
{
    const struct enc_sess_iquic *enc_sess = enc_session_p;
    const struct header_prot *hp;
    if (level == ENC_LEV_APP)
        hp = &enc_sess->esi_hp;
    else if (enc_sess->esi_hsk_crypto)
        hp = &enc_sess->esi_hsk_crypto[level].hp;
    else
        return 0;
    return header_prot_inited(hp, 0);
}


static enum enc_packout
iquic_esf_encrypt_packet (enc_session_t *enc_session_p,
    const struct lsquic_engine_public *enpub, struct lsquic_conn *lconn_UNUSED,
    struct lsquic_packet_out *packet_out)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;
    struct lsquic_conn *const lconn = enc_sess->esi_conn;
    unsigned char *dst;
    enum enc_level enc_level;
    size_t dst_sz;
    int header_sz;
    int ipv6;
    unsigned packno_off, packno_len;
    enum packnum_space pns;

    pns = lsquic_packet_out_pns(packet_out);
    enc_level = pns2enc_level[ enc_sess->esi_have_forw ][ pns ];

    if (packet_out->po_data_sz < 3)
    {
        /* [draft-ietf-quic-tls-20] Section 5.4.2 */
        enum packno_bits bits = lsquic_packet_out_packno_bits(packet_out);
        unsigned len = iquic_packno_bits2len(bits);
        if (packet_out->po_data_sz + len < 4)
        {
            len = 4 - packet_out->po_data_sz - len;
            memset(packet_out->po_data + packet_out->po_data_sz, 0, len);
            packet_out->po_data_sz += len;
            packet_out->po_frame_types |= QUIC_FTBIT_PADDING;
            LSQ_DEBUG("padded packet %"PRIu64" with %u bytes of PADDING",
                packet_out->po_packno, len);
        }
    }

    dst_sz = lconn->cn_pf->pf_packout_size(lconn, packet_out);
    ipv6 = NP_IS_IPv6(packet_out->po_path);
    dst = enpub->enp_pmi->pmi_allocate(enpub->enp_pmi_ctx,
            packet_out->po_path->np_peer_ctx, lconn->cn_conn_ctx, dst_sz, ipv6);
    if (!dst)
    {
        LSQ_DEBUG("could not allocate memory for outgoing packet of size %zd",
                                                                        dst_sz);
        return ENCPA_NOMEM;
    }

    header_sz = lconn->cn_pf->pf_gen_reg_pkt_header(lconn, packet_out, dst,
                                            dst_sz, &packno_off, &packno_len);
    if (header_sz < 0)
        goto err;
    dst[0] &= enc_sess->esi_grease | packet_out->po_path->np_dcid.idbuf[0];

    memcpy(dst + header_sz, packet_out->po_data, packet_out->po_data_sz);

#ifndef NDEBUG
    const unsigned sample_off = packno_off + 4;
    assert(sample_off <= dst_sz);
#endif

    packet_out->po_enc_data    = dst;
    packet_out->po_enc_data_sz = dst_sz;
    packet_out->po_sent_sz     = dst_sz;
    packet_out->po_flags &= ~PO_IPv6;
    packet_out->po_flags |= PO_ENCRYPTED|PO_SENT_SZ|(ipv6 << POIPv6_SHIFT);
    packet_out->po_dcid_len = packet_out->po_path->np_dcid.len;
    lsquic_packet_out_set_enc_level(packet_out, enc_level);

    return ENCPA_OK;

  err:
    enpub->enp_pmi->pmi_return(enpub->enp_pmi_ctx,
                                packet_out->po_path->np_peer_ctx, dst, ipv6);
    return ENCPA_BADCRYPT;
}


static enum dec_packin
iquic_esf_decrypt_packet (enc_session_t *enc_session_p,
        struct lsquic_engine_public *enpub, const struct lsquic_conn *lconn,
        struct lsquic_packet_in *packet_in)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;
    unsigned char *dst;
    struct header_prot *hp;
    unsigned sample_off, packno_len;
    enum enc_level enc_level;
    enum packnum_space pns;
    lsquic_packno_t packno;
    enum dec_packin dec_packin;
    const size_t dst_sz = packet_in->pi_data_sz;

    dst = lsquic_mm_get_packet_in_buf(&enpub->enp_mm, dst_sz);
    if (!dst)
    {
        LSQ_WARN("cannot allocate memory to copy incoming packet data");
        dec_packin = DECPI_NOMEM;
        goto err;
    }

    enc_level = hety2el[packet_in->pi_header_type];
    if (enc_level == ENC_LEV_APP)
        hp = &enc_sess->esi_hp;
    else if (enc_sess->esi_hsk_crypto)
        hp = &enc_sess->esi_hsk_crypto[ enc_level ].hp;
    else
        hp = NULL;

    /* Decrypt packet number.  After this operation, packet_in is adjusted:
     * the packet number becomes part of the header.
     */
    sample_off = packet_in->pi_header_sz + 4;
    if (sample_off > packet_in->pi_data_sz)
    {
        LSQ_INFO("packet data is too short: %hu bytes",
                                                packet_in->pi_data_sz);
        dec_packin = DECPI_TOO_SHORT;
        goto err;
    }
    memcpy(dst, packet_in->pi_data, sample_off);
    packet_in->pi_packno =
    packno = get_packno(enc_sess, hp,
        dst, packet_in->pi_header_sz, &packno_len);

    packet_in->pi_header_sz += packno_len;

    memcpy(dst + packet_in->pi_header_sz, packet_in->pi_data + packet_in->pi_header_sz, packet_in->pi_data_sz - packet_in->pi_header_sz);

    if (enc_sess->esi_flags & ESI_SEND_QL_BITS)
    {
        packet_in->pi_flags |= PI_LOG_QL_BITS;
        if (dst[0] & 0x10)
            packet_in->pi_flags |= PI_SQUARE_BIT;
        if (dst[0] & 0x08)
            packet_in->pi_flags |= PI_LOSS_BIT;
    }
    else if (dst[0] & (0x0C << (packet_in->pi_header_type == HETY_SHORT)))
    {
        LSQ_DEBUG("reserved bits are not set to zero");
        dec_packin = DECPI_VIOLATION;
        goto err;
    }

    if (packet_in->pi_flags & PI_OWN_DATA)
        lsquic_mm_put_packet_in_buf(&enpub->enp_mm, packet_in->pi_data,
                                                        packet_in->pi_data_sz);
    packet_in->pi_data = dst;
    packet_in->pi_flags |= PI_OWN_DATA | PI_DECRYPTED
                        | (enc_level << PIBIT_ENC_LEV_SHIFT);
    pns = lsquic_enclev2pns[enc_level];
    if (packet_in->pi_packno > enc_sess->esi_max_packno[pns]
            || !(enc_sess->esi_flags & (ESI_MAX_PACKNO_INIT << pns)))
        enc_sess->esi_max_packno[pns] = packet_in->pi_packno;
    enc_sess->esi_flags |= ESI_MAX_PACKNO_INIT << pns;
    return DECPI_OK;

  err:
    if (dst)
        lsquic_mm_put_packet_in_buf(&enpub->enp_mm, dst, dst_sz);
    EV_LOG_CONN_EVENT(LSQUIC_LOG_CONN_ID, "could not decrypt packet (type %s, "
        "number %"PRIu64")", lsquic_hety2str[packet_in->pi_header_type],
                                                    packet_in->pi_packno);
    return dec_packin;
}


static int
iquic_esf_sess_resume_enabled (enc_session_t *enc_session_p)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;
    return !!(enc_sess->esi_flags & ESI_USE_SSL_TICKET);
}


static void
iquic_esfi_set_iscid (enc_session_t *enc_session_p,
                                    const struct lsquic_packet_in *packet_in)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;

    if (!(enc_sess->esi_flags & ESI_ISCID))
    {
        lsquic_scid_from_packet_in(packet_in, &enc_sess->esi_iscid);
        enc_sess->esi_flags |= ESI_ISCID;
        LSQ_DEBUGC("set ISCID to %"CID_FMT, CID_BITS(&enc_sess->esi_iscid));
    }
}


int
iquic_esfi_switch_version (enc_session_t *enc_session_p, lsquic_cid_t *dcid,
                           int backup_keys)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;

    enc_sess->esi_flags |= ESI_SWITCH_VER;

    /* Free previous handshake keys */
    assert(enc_sess->esi_hsk_crypto);
    memset(&enc_sess->esi_hsk_crypto[ENC_LEV_INIT], 0,
            sizeof(enc_sess->esi_hsk_crypto[ENC_LEV_INIT]));

    if (0 == setup_handshake_keys(enc_sess, dcid ? dcid : &enc_sess->esi_odcid))
    {
        LSQ_INFO("update handshake keys to version %s",
                 lsquic_ver2str[enc_sess->esi_conn->cn_version]);
        return 0;
    }
    else
        return -1;
}


static int
iquic_esfi_reset_dcid (enc_session_t *enc_session_p,
        const lsquic_cid_t *old_dcid, const lsquic_cid_t *new_dcid)
{
    struct enc_sess_iquic *const enc_sess = enc_session_p;

    enc_sess->esi_odcid = *old_dcid;
    enc_sess->esi_rscid = *new_dcid;
    enc_sess->esi_flags |= ESI_ODCID|ESI_RSCID|ESI_RETRY;

    /* Free previous handshake keys */
    assert(enc_sess->esi_hsk_crypto);

    if (0 == setup_handshake_keys(enc_sess, new_dcid))
    {
        LSQ_INFOC("reset DCID to %"CID_FMT, CID_BITS(new_dcid));
        return 0;
    }
    else
        return -1;
}


static void
iquic_esfi_handshake_confirmed (enc_session_t *sess)
{
    struct enc_sess_iquic *enc_sess = (struct enc_sess_iquic *) sess;

    if (!(enc_sess->esi_flags & ESI_HSK_CONFIRMED))
    {
        LSQ_DEBUG("handshake has been confirmed");
        enc_sess->esi_flags |= ESI_HSK_CONFIRMED;
        maybe_drop_SSL(enc_sess);
    }
}


static int
iquic_esfi_in_init (enc_session_t *sess)
{
    struct enc_sess_iquic *enc_sess = (struct enc_sess_iquic *) sess;
    int in_init = !(enc_sess->esi_flags & ESI_HANDSHAKE_OK);
    return in_init;
}


static int
iquic_esfi_data_in (enc_session_t *sess, enum enc_level enc_level,
                                    const unsigned char *buf, size_t len)
{
    return 0;
}


static void iquic_esfi_shake_stream (enc_session_t *sess,
                            struct lsquic_stream *stream, unsigned rw);


const struct enc_session_funcs_iquic lsquic_enc_session_iquic_ietf_v1 =
{
    .esfi_create_client  = iquic_esfi_create_client,
    .esfi_destroy        = iquic_esfi_destroy,
    .esfi_get_peer_transport_params
                         = iquic_esfi_get_peer_transport_params,
    .esfi_reset_dcid     = iquic_esfi_reset_dcid,
    .esfi_init_server    = iquic_esfi_init_server,
    .esfi_set_iscid      = iquic_esfi_set_iscid,
    .esfi_create_server  = iquic_esfi_create_server,
    .esfi_shake_stream   = iquic_esfi_shake_stream,
    .esfi_handshake_confirmed
                         = iquic_esfi_handshake_confirmed,
    .esfi_in_init        = iquic_esfi_in_init,
    .esfi_data_in        = iquic_esfi_data_in,
};


const struct enc_session_funcs_common lsquic_enc_session_common_ietf_v1 =
{
    .esf_encrypt_packet  = iquic_esf_encrypt_packet,
    .esf_decrypt_packet  = iquic_esf_decrypt_packet,
    .esf_is_sess_resume_enabled = iquic_esf_sess_resume_enabled,
    .esf_set_conn        = iquic_esf_set_conn,
};


static void
drop_SSL (struct enc_sess_iquic *enc_sess)
{
    LSQ_DEBUG("drop SSL object");
    if (enc_sess->esi_conn->cn_if->ci_drop_crypto_streams)
        enc_sess->esi_conn->cn_if->ci_drop_crypto_streams(
                                                    enc_sess->esi_conn);
    enc_sess->esi_ssl_flag = 0;
    free_handshake_keys(enc_sess);
}


static void
maybe_drop_SSL (struct enc_sess_iquic *enc_sess)
{
    /* We rely on the following BoringSSL property: it writes new session
     * tickets before marking handshake as complete.  In this case, the new
     * session tickets have either been successfully written to crypto stream,
     * in which case we can close it, or (unlikely) they are buffered in the
     * frab list.
     */
    if ((enc_sess->esi_flags & (ESI_HSK_CONFIRMED|ESI_HANDSHAKE_OK))
                            == (ESI_HSK_CONFIRMED|ESI_HANDSHAKE_OK)
        && enc_sess->esi_ssl_flag
        && lsquic_frab_list_empty(&enc_sess->esi_frals[ENC_LEV_APP]))
    {
        if ((enc_sess->esi_flags & (ESI_SERVER|ESI_WANT_TICKET))
                                                            != ESI_WANT_TICKET)
            drop_SSL(enc_sess);
        else if (enc_sess->esi_alset
                && !lsquic_alarmset_is_set(enc_sess->esi_alset, AL_SESS_TICKET))
        {
            LSQ_DEBUG("no session ticket: delay dropping SSL object");
            lsquic_alarmset_set(enc_sess->esi_alset, AL_SESS_TICKET,
                /* Wait up to two seconds for session tickets */
                                                lsquic_time_now() + 2000000);
        }
    }
}


static void
no_sess_ticket (enum alarm_id alarm_id, void *ctx,
                                  lsquic_time_t expiry, lsquic_time_t now)
{
    struct enc_sess_iquic *enc_sess = ctx;

    LSQ_DEBUG("no session tickets forthcoming -- drop SSL");
    drop_SSL(enc_sess);
}


static int
set_secret (struct enc_sess_iquic *enc_sess, enum enc_level enc_level, int rw)
{
    struct header_prot *hp;

    if (enc_level < ENC_LEV_APP)
    {
        assert(enc_sess->esi_hsk_crypto);
        hp = &enc_sess->esi_hsk_crypto[enc_level].hp;
    }
    else
    {
        hp = &enc_sess->esi_hp;
    }

    if (rw)
    {
        hp->hp_enc_level = enc_level;
    }
    hp->hp_flags |= 1 << rw;

    if (rw && enc_level == ENC_LEV_APP)
        enc_sess->esi_have_forw = 1;

    return 1;
}


static int
cry_sm_write_message (struct enc_sess_iquic *enc_sess, enum enc_level level, 
                      const uint8_t *data, size_t len)
{
    void *stream;
    ssize_t nw;

    stream = enc_sess->esi_streams[level];
    if (!stream)
        return 0;

    /* The frab list logic is only applicable on the client.  XXX This is
     * likely to change when support for key updates is added.
     */
    if (enc_sess->esi_flags & (ESI_ON_WRITE|ESI_SERVER))
        nw = enc_sess->esi_cryst_if->csi_write(stream, data, len);
    else
    {
        LSQ_DEBUG("not in on_write event: buffer in a frab list");
        if (0 == lsquic_frab_list_write(&enc_sess->esi_frals[level], data, len))
        {
            if (!lsquic_frab_list_empty(&enc_sess->esi_frals[level]))
                enc_sess->esi_cryst_if->csi_wantwrite(stream, 1);
            nw = len;
        }
        else
            nw = -1;
    }

    if (nw >= 0 && (size_t) nw == len)
    {
        enc_sess->esi_last_w = level;
        LSQ_DEBUG("wrote %zu bytes to stream at encryption level %u",
            len, level);
        maybe_drop_SSL(enc_sess);
        return 1;
    }
    else
    {
        LSQ_INFO("could not write %zu bytes: returned %zd", len, nw);
        return 0;
    }
}


static int
cry_sm_flush_flight (struct enc_sess_iquic *enc_sess)
{
    void *stream;
    unsigned level;
    int s;

    level = enc_sess->esi_last_w;
    stream = enc_sess->esi_streams[level];
    if (!stream)
        return 0;

    if (lsquic_frab_list_empty(&enc_sess->esi_frals[level]))
    {
        s = enc_sess->esi_cryst_if->csi_flush(stream);
        return s == 0;
    }
    else
        /* Frab list will get flushed */    /* TODO: add support for
        recording flush points in frab list. */
        return 1;
}


static lsquic_stream_ctx_t *
chsk_ietf_on_new_stream (void *stream_if_ctx, struct lsquic_stream *stream)
{
    struct enc_sess_iquic *const enc_sess = stream_if_ctx;
    enum enc_level enc_level;

    enc_level = enc_sess->esi_cryst_if->csi_enc_level(stream);
    if (enc_level == ENC_LEV_INIT)
        enc_sess->esi_cryst_if->csi_wantwrite(stream, 1);

    LSQ_DEBUG("handshake stream created successfully");

    return stream_if_ctx;
}


static lsquic_stream_ctx_t *
shsk_ietf_on_new_stream (void *stream_if_ctx, struct lsquic_stream *stream)
{
    struct enc_sess_iquic *const enc_sess = stream_if_ctx;
    enum enc_level enc_level;

    enc_level = enc_sess->esi_cryst_if->csi_enc_level(stream);
    LSQ_DEBUG("on_new_stream called on level %u", enc_level);

    enc_sess->esi_cryst_if->csi_wantread(stream, 1);

    return stream_if_ctx;
}


static void
chsk_ietf_on_close (struct lsquic_stream *stream, lsquic_stream_ctx_t *ctx)
{
    struct enc_sess_iquic *const enc_sess = (struct enc_sess_iquic *) ctx;
    if (enc_sess && enc_sess->esi_cryst_if)
        LSQ_DEBUG("crypto stream level %u is closed",
                (unsigned) enc_sess->esi_cryst_if->csi_enc_level(stream));
}


static const char *const ihs2str[] = {
    [IHS_WANT_READ]  = "want read",
    [IHS_WANT_WRITE] = "want write",
    [IHS_WANT_RW]    = "want rw",
    [IHS_STOP]       = "stop",
};


static void
iquic_esfi_shake_stream (enc_session_t *sess,
                            struct lsquic_stream *stream, unsigned rw)
{
    struct enc_sess_iquic *enc_sess = (struct enc_sess_iquic *)sess;
    enum iquic_handshake_status st;
    enum enc_level enc_level;
    int write;
    enc_level = enc_sess->esi_cryst_if->csi_enc_level(stream);
    if (0 == (enc_sess->esi_flags & ESI_HANDSHAKE_OK))
        st = iquic_esfi_handshake(enc_sess, rw, enc_level);
    else
        st = IHS_WANT_READ;
    LSQ_DEBUG("enc level %s after %d: %s", lsquic_enclev2str[enc_level], rw,
                                                                ihs2str[st]);
    switch (st)
    {
    case IHS_WANT_READ:
        write = !lsquic_frab_list_empty(&enc_sess->esi_frals[enc_level]);
        enc_sess->esi_cryst_if->csi_wantwrite(stream, write);
        enc_sess->esi_cryst_if->csi_wantread(stream, 1);
        break;
    case IHS_WANT_WRITE:
        enc_sess->esi_cryst_if->csi_wantwrite(stream, 1);
        enc_sess->esi_cryst_if->csi_wantread(stream, 0);
        break;
    case IHS_WANT_RW:
        enc_sess->esi_cryst_if->csi_wantwrite(stream, 1);
        enc_sess->esi_cryst_if->csi_wantread(stream, 1);
        break;
    default:
        assert(st == IHS_STOP);
        write = !lsquic_frab_list_empty(&enc_sess->esi_frals[enc_level]);
        enc_sess->esi_cryst_if->csi_wantwrite(stream, write);
        enc_sess->esi_cryst_if->csi_wantread(stream, 0);
        break;
    }
    LSQ_DEBUG("Exit shake_stream");
    maybe_drop_SSL(enc_sess);
}


struct readf_ctx
{
    struct enc_sess_iquic  *enc_sess;
    enum enc_level          enc_level;
    int                     err;
};


static size_t
readf_cb (void *ctx, const unsigned char *buf, size_t len, int fin)
{
    struct readf_ctx *const readf_ctx = (void *) ctx;
    struct enc_sess_iquic *const enc_sess = readf_ctx->enc_sess;

    if (((enc_sess->esi_flags & ESI_SERVER) && readf_ctx->enc_level == 0) || (!(enc_sess->esi_flags & ESI_SERVER) && readf_ctx->enc_level == 2)) {
        memcpy(enc_sess->esi_peer_tp_buf, buf, len);
        enc_sess->esi_peer_tp_len = len;
    }
    return len;
}


static size_t
discard_cb (void *ctx, const unsigned char *buf, size_t len, int fin)
{
    return len;
}


static void
chsk_ietf_on_read (struct lsquic_stream *stream, lsquic_stream_ctx_t *ctx)
{
    struct enc_sess_iquic *const enc_sess = (void *) ctx;
    enum enc_level enc_level = enc_sess->esi_cryst_if->csi_enc_level(stream);
    struct readf_ctx readf_ctx = { enc_sess, enc_level, 0, };
    ssize_t nread;

    if (enc_sess->esi_ssl_flag) 
    {
        nread = enc_sess->esi_cryst_if->csi_readf(stream, readf_cb, &readf_ctx);
        if (!(nread < 0 || readf_ctx.err))
            iquic_esfi_shake_stream((enc_session_t *)enc_sess, stream, 0);
        else
            enc_sess->esi_conn->cn_if->ci_internal_error(enc_sess->esi_conn,
                "shaking stream failed: nread: %zd, err: %d",
                nread, readf_ctx.err);
    }
    else
    {
        /* This branch is reached when we don't want TLS ticket and drop
         * the SSL object before we process TLS tickets that have been
         * already received and waiting in the incoming stream buffer.
         */
        nread = enc_sess->esi_cryst_if->csi_readf(stream, discard_cb, NULL);
        lsquic_stream_wantread(stream, 0);
        LSQ_DEBUG("no SSL object: discard %zd bytes of SSL data", nread);
    }
}


static void
maybe_write_from_fral (struct enc_sess_iquic *enc_sess,
                                                struct lsquic_stream *stream)
{
    enum enc_level enc_level = enc_sess->esi_cryst_if->csi_enc_level(stream);
    struct frab_list *const fral = &enc_sess->esi_frals[enc_level];
    struct lsquic_reader reader = {
        .lsqr_read  = lsquic_frab_list_read,
        .lsqr_size  = lsquic_frab_list_size,
        .lsqr_ctx   = fral,
    };
    ssize_t nw;

    if (lsquic_frab_list_empty(fral))
        return;

    nw = lsquic_stream_writef(stream, &reader);
    if (nw >= 0)
    {
        LSQ_DEBUG("wrote %zd bytes to stream from frab list", nw);
        (void) lsquic_stream_flush(stream);
        if (lsquic_frab_list_empty(fral))
            lsquic_stream_wantwrite(stream, 0);
    }
    else
    {
        enc_sess->esi_conn->cn_if->ci_internal_error(enc_sess->esi_conn,
                            "cannot write to stream: %s", strerror(errno));
        lsquic_stream_wantwrite(stream, 0);
    }
}


static void
chsk_ietf_on_write (struct lsquic_stream *stream, lsquic_stream_ctx_t *ctx)
{
    struct enc_sess_iquic *const enc_sess = (void *) ctx;

    maybe_write_from_fral(enc_sess, stream);

    enc_sess->esi_flags |= ESI_ON_WRITE;
    iquic_esfi_shake_stream(enc_sess, stream, 1);
    enc_sess->esi_flags &= ~ESI_ON_WRITE;
}


const struct lsquic_stream_if lsquic_cry_sm_if =
{
    .on_new_stream = chsk_ietf_on_new_stream,
    .on_read       = chsk_ietf_on_read,
    .on_write      = chsk_ietf_on_write,
    .on_close      = chsk_ietf_on_close,
};


const struct lsquic_stream_if lsquic_mini_cry_sm_if =
{
    .on_new_stream = shsk_ietf_on_new_stream,
    .on_read       = chsk_ietf_on_read,
    .on_write      = chsk_ietf_on_write,
    .on_close      = chsk_ietf_on_close,
};


int
lsquic_enc_sess_ietf_gen_quic_ctx (
                const struct lsquic_engine_settings *settings,
                enum lsquic_version version, unsigned char *buf, size_t bufsz)
{
    struct transport_params params;
    int len;

    /* This code is pretty much copied from gen_trans_params(), with
     * small (but important) exceptions.
     */

    memset(&params, 0, sizeof(params));
    params.tp_init_max_data = settings->es_init_max_data;
    params.tp_init_max_stream_data_bidi_local
                            = settings->es_init_max_stream_data_bidi_local;
    params.tp_init_max_stream_data_bidi_remote
                            = settings->es_init_max_stream_data_bidi_remote;
    params.tp_init_max_stream_data_uni
                            = settings->es_init_max_stream_data_uni;
    params.tp_init_max_streams_uni
                            = settings->es_init_max_streams_uni;
    params.tp_init_max_streams_bidi
                            = settings->es_init_max_streams_bidi;
    params.tp_ack_delay_exponent
                            = TP_DEF_ACK_DELAY_EXP;
    params.tp_max_idle_timeout = settings->es_idle_timeout * 1000;
    params.tp_max_ack_delay = TP_DEF_MAX_ACK_DELAY;
    params.tp_active_connection_id_limit = MAX_IETF_CONN_DCIDS;
    params.tp_set |= (1 << TPI_INIT_MAX_DATA)
                  |  (1 << TPI_INIT_MAX_STREAM_DATA_BIDI_LOCAL)
                  |  (1 << TPI_INIT_MAX_STREAM_DATA_BIDI_REMOTE)
                  |  (1 << TPI_INIT_MAX_STREAM_DATA_UNI)
                  |  (1 << TPI_INIT_MAX_STREAMS_UNI)
                  |  (1 << TPI_INIT_MAX_STREAMS_BIDI)
                  |  (1 << TPI_ACK_DELAY_EXPONENT)
                  |  (1 << TPI_MAX_IDLE_TIMEOUT)
                  |  (1 << TPI_MAX_ACK_DELAY)
                  |  (1 << TPI_ACTIVE_CONNECTION_ID_LIMIT)
                  ;
    if (settings->es_max_udp_payload_size_rx)
    {
        params.tp_max_udp_payload_size = settings->es_max_udp_payload_size_rx;
        params.tp_set |= 1 << TPI_MAX_UDP_PAYLOAD_SIZE;
    }
    if (!settings->es_allow_migration)
        params.tp_set |= 1 << TPI_DISABLE_ACTIVE_MIGRATION;
    if (settings->es_ql_bits)
    {
        params.tp_loss_bits = settings->es_ql_bits - 1;
        params.tp_set |= 1 << TPI_LOSS_BITS;
    }
    if (settings->es_delayed_acks)
    {
        params.tp_numerics[TPI_MIN_ACK_DELAY] = TP_MIN_ACK_DELAY;
        params.tp_set |= 1 << TPI_MIN_ACK_DELAY;
        params.tp_numerics[TPI_MIN_ACK_DELAY_02] = TP_MIN_ACK_DELAY;
        params.tp_set |= 1 << TPI_MIN_ACK_DELAY_02;
    }
    if (settings->es_timestamps)
    {
        params.tp_numerics[TPI_TIMESTAMPS] = TS_GENERATE_THEM;
        params.tp_set |= 1 << TPI_TIMESTAMPS;
    }
    if (settings->es_datagrams)
    {
        if (params.tp_set & (1 << TPI_MAX_UDP_PAYLOAD_SIZE))
            params.tp_numerics[TPI_MAX_DATAGRAM_FRAME_SIZE]
                                            = params.tp_max_udp_payload_size;
        else
            params.tp_numerics[TPI_MAX_DATAGRAM_FRAME_SIZE]
                                            = TP_DEF_MAX_UDP_PAYLOAD_SIZE;
        params.tp_set |= 1 << TPI_MAX_DATAGRAM_FRAME_SIZE;
    }

    params.tp_set &= SERVER_0RTT_TPS;

    len = (version == LSQVER_ID27 ? lsquic_tp_encode_27 : lsquic_tp_encode)(
                        &params, 1, buf, bufsz);
    if (len >= 0)
    {
        char str[MAX_TP_STR_SZ];
        LSQ_LOG1(LSQ_LOG_DEBUG, "generated QUIC server context of %d bytes "
            "for version %s", len, lsquic_ver2str[version]);
        LSQ_LOG1(LSQ_LOG_DEBUG, "%s", ((version == LSQVER_ID27
                ? lsquic_tp_to_str_27 : lsquic_tp_to_str)(&params, str,
                                                            sizeof(str)), str));
    }
    else
        LSQ_LOG1(LSQ_LOG_WARN, "cannot generate QUIC server context: %d",
                                                                        errno);
    return len;
}