/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * lsquic_mini_conn.h -- Mini-connection
 *
 * Before a connection is established, the server keeps a "mini" connection
 * object where it keeps track of stream 1 offsets and so on.
 */

#ifndef LSQUIC_MINI_CONN_H
#define LSQUIC_MINI_CONN_H

#define MAX_MINI_CONN_LIFESPAN_IN_USEC \
    ((1 << (sizeof(((struct mini_conn *) 0)->mc_largest_recv) * 8)) - 1)

struct lsquic_packet_in;
struct lsquic_packet_out;
struct lsquic_engine_public;

struct mini_conn {
    /* mc_largest_recv is the timestamp of when packet with the largest
     * number was received; it is necessary to generate ACK frames.  24
     * bits holds about 16.5 seconds worth of microseconds, which is
     * larger than the maximum amount of time a mini connection object
     * is allowed to live.  To get the timestamp, add this value to
     * mc_created.
     */
    unsigned char          mc_largest_recv[3];
};

#endif
