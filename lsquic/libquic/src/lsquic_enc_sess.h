/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
#ifndef LSQUIC_ENC_SESS_H
#define LSQUIC_ENC_SESS_H 1

#include "lsquic_shared_support.h"

struct lsquic_alarmset;
struct lsquic_engine_public;
struct lsquic_packet_out;
struct lsquic_packet_in;
struct stream_wrapper;
struct ver_neg;
struct lsquic_conn;
struct transport_params;
struct lsquic_cid;
struct sockaddr;
struct conn_cid_elem;
struct lsquic_engine_settings;
enum lsquic_version;

#define DNONC_LENGTH 32
#define SRST_LENGTH 16

/* From [draft-ietf-quic-tls-14]:
 *
 * Data is protected using a number of encryption levels:
 *
 * o  Plaintext
 *
 * o  Early Data (0-RTT) Keys
 *
 * o  Handshake Keys
 *
 * o  Application Data (1-RTT) Keys
 */

/* This enum maps to the list above */
enum enc_level
{
    ENC_LEV_INIT,
    ENC_LEV_0RTT,
    ENC_LEV_HSK,
    ENC_LEV_APP,
    N_ENC_LEVS
};

enum handshake_error            /* TODO: rename this enum */
{
    DATA_NOT_ENOUGH = -2,
    DATA_FORMAT_ERROR = -1,
    HS_ERROR = -1,
    DATA_NO_ERROR = 0,
    HS_SHLO = 0,
    HS_1RTT = 1,
    HS_SREJ = 2,
};

#ifndef LSQUIC_KEEP_ENC_SESS_HISTORY
#   ifndef NDEBUG
#       define LSQUIC_KEEP_ENC_SESS_HISTORY 1
#   else
#       define LSQUIC_KEEP_ENC_SESS_HISTORY 0
#   endif
#endif

#if LSQUIC_KEEP_ENC_SESS_HISTORY
#define ESHIST_BITS 7
#define ESHIST_MASK ((1 << ESHIST_BITS) - 1)
#define ESHIST_STR_SIZE ((1 << ESHIST_BITS) + 1)
#endif

enum enc_packout { ENCPA_OK, ENCPA_NOMEM, ENCPA_BADCRYPT, };

enum dec_packin {
    DECPI_OK,
    DECPI_NOMEM,
    DECPI_TOO_SHORT,
    DECPI_NOT_YET,
    DECPI_BADCRYPT,
    DECPI_VIOLATION,
};

typedef void enc_session_t;

struct enc_session_funcs_common
{
    /* Need to pass lconn in encrypt and decrypt methods because enc_session
     * is allowed to be NULL for gQUIC.
     */
    enum enc_packout
    (*esf_encrypt_packet) (enc_session_t *, const struct lsquic_engine_public *,
        struct lsquic_conn *, struct lsquic_packet_out *);

    enum dec_packin
    (*esf_decrypt_packet)(enc_session_t *, struct lsquic_engine_public *,
        const struct lsquic_conn *, struct lsquic_packet_in *);

    int
    (*esf_verify_reset_token) (enc_session_t *, const unsigned char *, size_t);

    int
    (*esf_did_sess_resume_succeed) (enc_session_t *);

    int
    (*esf_is_sess_resume_enabled) (enc_session_t *);

    void
    (*esf_set_conn) (enc_session_t *, struct lsquic_conn *);
};

struct crypto_stream_if
{
    ssize_t     (*csi_write) (void *stream, const void *buf, size_t len);
    int         (*csi_flush) (void *stream);
    ssize_t     (*csi_readf) (void *stream,
        size_t (*readf)(void *, const unsigned char *, size_t, int), void *ctx);
    int         (*csi_wantwrite) (void *stream, int is_want);
    int         (*csi_wantread) (void *stream, int is_want);
    enum enc_level
                (*csi_enc_level) (void *stream);
};

struct enc_session_funcs_iquic
{
    enc_session_t *
    (*esfi_create_client) (const char *domain, struct lsquic_engine_public *,
                           struct lsquic_conn *, const struct lsquic_cid *,
                           const struct ver_neg *, void *(crypto_streams)[4],
                           const struct crypto_stream_if *,
                           const unsigned char *, size_t,
                           struct lsquic_alarmset *, unsigned, void*);

    void
    (*esfi_destroy) (enc_session_t *);

    struct transport_params *
    (*esfi_get_peer_transport_params) (enc_session_t *);

    int
    (*esfi_reset_dcid) (enc_session_t *, const struct lsquic_cid *,
                                                const struct lsquic_cid *);

    void
    (*esfi_set_iscid) (enc_session_t *, const struct lsquic_packet_in *);

    int
    (*esfi_init_server) (enc_session_t *);

    enc_session_t *
    (*esfi_create_server) (struct lsquic_engine_public *, struct lsquic_conn *,
                                                    const struct lsquic_cid *,
                           void *(crypto_streams)[4],
                           const struct crypto_stream_if *,
                           const struct lsquic_cid *odcid,
                           const struct lsquic_cid *iscid,
                           const struct lsquic_cid *rscid
                            );

    void
    (*esfi_shake_stream)(enc_session_t *, struct lsquic_stream *,
                         unsigned);

    void
    (*esfi_handshake_confirmed)(enc_session_t *);

    int
    (*esfi_in_init)(enc_session_t *);

    int
    (*esfi_data_in)(enc_session_t *, enum enc_level,
                                            const unsigned char *, size_t);
};

LSQUIC_EXTERN const struct enc_session_funcs_common lsquic_enc_session_common_ietf_v1;

LSQUIC_EXTERN const struct enc_session_funcs_iquic lsquic_enc_session_iquic_ietf_v1;

#define select_esf_common_by_ver(ver) ( \
    ver ? &lsquic_enc_session_common_ietf_v1 : &lsquic_enc_session_common_ietf_v1)

#define select_esf_iquic_by_ver(ver) ( \
    ver ? &lsquic_enc_session_iquic_ietf_v1 : &lsquic_enc_session_iquic_ietf_v1)

extern const char *const lsquic_enclev2str[];

LSQUIC_EXTERN const struct lsquic_stream_if lsquic_cry_sm_if;

LSQUIC_EXTERN const struct lsquic_stream_if lsquic_mini_cry_sm_if;

/* RFC 7301, Section 3.2 */
#define ALERT_NO_APPLICATION_PROTOCOL 120

enum lsquic_version
lsquic_sess_resume_version (const unsigned char *, size_t);

/* This is seems to be true for all of the ciphers used by IETF QUIC.
 * XXX: Perhaps add a check?
 */
#define IQUIC_TAG_LEN 16

/* Return number of bytes written to `buf' or -1 on error */
int
lsquic_enc_sess_ietf_gen_quic_ctx (
                const struct lsquic_engine_settings *settings,
                enum lsquic_version version, unsigned char *buf, size_t bufsz);

struct enc_sess_iquic;
int
iquic_esfi_init_server_tp (struct enc_sess_iquic *const enc_sess);

int
iquic_esfi_switch_version (enc_session_t *enc_session_p, lsquic_cid_t *dcid,
                           int backup_keys);

int
iquic_esf_is_enc_level_ready (enc_session_t *enc_session_p,
                              enum enc_level level);

#endif
