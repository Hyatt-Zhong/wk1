/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <time.h>

#ifndef WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#else
#include "vc_compat.h"
#include <Ws2tcpip.h>
#endif

#include "lsquic.h"
#include "lsquic_int_types.h"
#include "lsquic_sizes.h"
#include "lsquic_types.h"
#include "lsquic_packet_common.h"
#include "lsquic_packet_in.h"
#include "lsquic_tokgen.h"
#include "lsquic_trans_params.h"
#include "lsquic_util.h"
#include "lsquic_mm.h"
#include "lsquic_engine_public.h"

#define LSQUIC_LOGGER_MODULE LSQLM_TOKGEN
#include "lsquic_logger.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define TOKGEN_VERSION 2

#define CRYPTER_KEY_SIZE        16
#define SRST_MAX_PRK_SIZE       64

#define TOKGEN_SHM_KEY "TOKGEN" TOSTRING(TOKGEN_VERSION)
#define TOKGEN_SHM_KEY_SIZE (sizeof(TOKGEN_SHM_KEY) - 1)

#define TOKGEN_SHM_MAGIC_TOP "Feliz"
#define TOKGEN_SHM_MAGIC_BOTTOM "Navidad"

struct tokgen_shm_state
{
    uint8_t     tgss_version;
    uint8_t     tgss_magic_top[sizeof(TOKGEN_SHM_MAGIC_TOP) - 1];
    uint8_t     tgss_crypter_key[N_TOKEN_TYPES][CRYPTER_KEY_SIZE];
    uint8_t     tgss_srst_prk_size;
    uint8_t     tgss_srst_prk[SRST_MAX_PRK_SIZE];
    uint8_t     tgss_magic_bottom[sizeof(TOKGEN_SHM_MAGIC_BOTTOM) - 1];
};


struct crypter
{
    unsigned long   nonce_counter;
    size_t          nonce_prk_sz;
    uint8_t         nonce_prk_buf[64];
};


/* Bloom filter of Resume tokens.  See below. */
struct resumed_token_page
{
    TAILQ_ENTRY(resumed_token_page)     next;
    time_t                              begin,  /* Oldest entry */
                                        end;    /* Newest entry */
    unsigned                            count;  /* Number of entries */
    uintptr_t                           masks[];
};


struct token_generator
{
    /* We encrypt different token types using different keys. */
    struct crypter  tg_crypters[N_TOKEN_TYPES];

    /* Stateless reset token is generated using HKDF with CID as the
     * `info' parameter to HKDF-Expand.
     */
    size_t          tg_srst_prk_sz;
    uint8_t         tg_srst_prk_buf[SRST_MAX_PRK_SIZE];
    unsigned        tg_retry_token_duration;
    TAILQ_HEAD(resumed_token_pages_head, resumed_token_page)
                    tg_resume_token_pages;
};


static int
setup_nonce_prk (unsigned char *nonce_prk_buf, size_t *nonce_prk_sz,
                                                    unsigned i, time_t now)
{
    *nonce_prk_sz = 32;
    rand_bytes(nonce_prk_buf, *nonce_prk_sz);
    return 0;
}


static int
get_or_generate_state (struct lsquic_engine_public *enpub, time_t now,
                                        struct tokgen_shm_state *shm_state)
{
    const struct lsquic_shared_hash_if *const shi = enpub->enp_shi;
    void *const ctx = enpub->enp_shi_ctx;
    void *data, *copy;
    char key_copy[TOKGEN_SHM_KEY_SIZE];
    int s;
    unsigned sz;
    size_t bufsz;
    struct {
        time_t        now;
        unsigned char buf[24];
    }
#if __GNUC__
    /* This is more of a documentation note: this struct should already
     * have a multiple-of-eight size.
     */
    __attribute__((packed))
#endif
    srst_ikm;

    data = shm_state;
    sz = sizeof(*shm_state);
    s = shi->shi_lookup(ctx, TOKGEN_SHM_KEY, TOKGEN_SHM_KEY_SIZE, &data, &sz);

    if (s == 1)
    {
        if (sz != sizeof(*shm_state))
        {
            LSQ_WARN("found SHM data has non-matching size %u", sz);
            return -1;
        }
        if (data != (void *) shm_state)
            memcpy(shm_state, data, sizeof(*shm_state));
        if (shm_state->tgss_version != TOKGEN_VERSION)
        {
            LSQ_DEBUG("found SHM data has non-matching version %u",
                                                        shm_state->tgss_version);
            return -1;
        }
        LSQ_DEBUG("found SHM data: size %u; version %u", sz,
                                                        shm_state->tgss_version);
        return 0;
    }

    if (s != 0)
    {
        if (s != -1)
            LSQ_WARN("SHM lookup returned unexpected value %d", s);
        LSQ_DEBUG("SHM lookup returned an error: generate");
        goto generate;
    }

    assert(s == 0);
    LSQ_DEBUG("%s does not exist: generate", TOKGEN_SHM_KEY);
  generate:
    now = time(NULL);
    memset(shm_state, 0, sizeof(*shm_state));
    shm_state->tgss_version = TOKGEN_VERSION;
    memcpy(shm_state->tgss_magic_top, TOKGEN_SHM_MAGIC_TOP,
                                        sizeof(TOKGEN_SHM_MAGIC_TOP) - 1);
    if (getenv("LSQUIC_NULL_TOKGEN"))
    {
        LSQ_NOTICE("using NULL tokgen");
        memset(shm_state->tgss_crypter_key, 0,
                                        sizeof(shm_state->tgss_crypter_key));
        memset(&srst_ikm, 0, sizeof(srst_ikm));
    }
    else
    {
        rand_bytes((void *) shm_state->tgss_crypter_key,
                                        sizeof(shm_state->tgss_crypter_key));
        srst_ikm.now = now;
        rand_bytes(srst_ikm.buf, sizeof(srst_ikm.buf));
    }
    bufsz = 32;
    rand_bytes(shm_state->tgss_srst_prk, bufsz);
    shm_state->tgss_srst_prk_size = (uint8_t) bufsz;
    memcpy(shm_state->tgss_magic_bottom, TOKGEN_SHM_MAGIC_BOTTOM,
                                        sizeof(TOKGEN_SHM_MAGIC_BOTTOM) - 1);

    data = shm_state;
    memcpy(key_copy, TOKGEN_SHM_KEY, TOKGEN_SHM_KEY_SIZE);
    s = shi->shi_insert(ctx, key_copy, TOKGEN_SHM_KEY_SIZE, data,
                                                    sizeof(*shm_state), 0);
    if (s != 0)
    {
        LSQ_ERROR("cannot insert into SHM");
        return -1;
    }
    sz = sizeof(*shm_state);
    s = shi->shi_lookup(ctx, TOKGEN_SHM_KEY, TOKGEN_SHM_KEY_SIZE, &copy, &sz);
    if (s != 1 || sz != sizeof(*shm_state))
    {
        LSQ_ERROR("cannot lookup after insert: s=%d; sz=%u", s, sz);
        return -1;
    }
    if (copy != data)
        memcpy(shm_state, copy, sizeof(*shm_state));
    LSQ_INFO("inserted %s of size %u", TOKGEN_SHM_KEY, sz);
    return 0;
}


struct token_generator *
lsquic_tg_new (struct lsquic_engine_public *enpub)
{
    struct token_generator *tokgen;
    time_t now;
    struct tokgen_shm_state shm_state;

    tokgen = calloc(1, sizeof(*tokgen));
    if (!tokgen)
        goto err;

    now = time(NULL);
    if (0 != get_or_generate_state(enpub, now, &shm_state))
        goto err;

    TAILQ_INIT(&tokgen->tg_resume_token_pages);
    unsigned i;
    for (i = 0; i < sizeof(tokgen->tg_crypters)
                                    / sizeof(tokgen->tg_crypters[0]); ++i)
    {
        struct crypter *crypter;
        crypter = tokgen->tg_crypters + i;
        if (0 != setup_nonce_prk(crypter->nonce_prk_buf,
                                        &crypter->nonce_prk_sz, i, now))
            goto err;
    }

    tokgen->tg_retry_token_duration
                            = enpub->enp_settings.es_retry_token_duration;
    if (tokgen->tg_retry_token_duration == 0)
        tokgen->tg_retry_token_duration = LSQUIC_DF_RETRY_TOKEN_DURATION;

    tokgen->tg_srst_prk_sz = shm_state.tgss_srst_prk_size;
    if (tokgen->tg_srst_prk_sz > sizeof(tokgen->tg_srst_prk_buf))
    {
        LSQ_WARN("bad stateless reset key size");
        goto err;
    }
    memcpy(tokgen->tg_srst_prk_buf, shm_state.tgss_srst_prk,
                                                    tokgen->tg_srst_prk_sz);

    LSQ_DEBUG("initialized");
    return tokgen;

  err:
    LSQ_ERROR("error initializing");
    free(tokgen);
    return NULL;
}


void
lsquic_tg_destroy (struct token_generator *tokgen)
{
    struct resumed_token_page *page;
    struct crypter *crypter;
    unsigned i;

    while ((page = TAILQ_FIRST(&tokgen->tg_resume_token_pages)))
    {
        TAILQ_REMOVE(&tokgen->tg_resume_token_pages, page, next);
        free(page);
    }
    for (i = 0; i < sizeof(tokgen->tg_crypters)
                                    / sizeof(tokgen->tg_crypters[0]); ++i)
    {
        crypter = tokgen->tg_crypters + i;
    }
    free(tokgen);
    LSQ_DEBUG("destroyed");
}


void
lsquic_tg_generate_sreset (struct token_generator *tokgen,
        const struct lsquic_cid *cid, unsigned char *reset_token)
{
    char str[IQUIC_SRESET_TOKEN_SZ * 2 + 1];

    rand_bytes(reset_token, IQUIC_SRESET_TOKEN_SZ);
    LSQ_DEBUGC("generated stateless reset token %s for CID %"CID_FMT,
        HEXSTR(reset_token, IQUIC_SRESET_TOKEN_SZ, str), CID_BITS(cid));
}
