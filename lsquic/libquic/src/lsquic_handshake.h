/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
#ifndef LSQUIC_HANDSHAKE_H
#define LSQUIC_HANDSHAKE_H 1

struct sockaddr;
struct lsquic_str;
struct lsquic_packet_in;
struct lsquic_cid;
struct lsquic_enc_session;
struct lsquic_engine_public;

enum lsquic_version
lsquic_sess_resume_version (const unsigned char *, size_t);

#endif
