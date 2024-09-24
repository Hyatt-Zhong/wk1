/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
#ifndef LSQUIC_TOKEN_H
#define LSQUIC_TOKEN_H 1

struct lsquic_engine_public;
struct sockaddr;
struct lsquic_packet_in;
struct lsquic_cid;

enum token_type { TOKEN_RETRY, TOKEN_RESUME, N_TOKEN_TYPES, };

struct token_generator;

struct token_generator *
lsquic_tg_new (struct lsquic_engine_public *);

void
lsquic_tg_destroy (struct token_generator *);

/* `reset_token' must be IQUIC_SRESET_TOKEN_SZ bytes in length */
void
lsquic_tg_generate_sreset (struct token_generator *,
        const struct lsquic_cid *cid, unsigned char *reset_token);


/* Retry and Resume tokens have identical sizes.  Use *RETRY* macros
 * for both.
 */
#define RETRY_TAG_LEN 16

/* Type is encoded in the nonce */
#define RETRY_NONCE_LEN 12

#define MAX_RETRY_TOKEN_LEN (RETRY_NONCE_LEN + 1 /* version */ + \
    sizeof(time_t) /* time */ + 1 /* IPv4 or IPv6 */ + \
    16 /* IPv6 or IPv4 address */ + 2 /* Port number */ + MAX_CID_LEN + \
    RETRY_TAG_LEN)

#endif
