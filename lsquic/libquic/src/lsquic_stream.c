/* Copyright (c) 2017 - 2022 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * lsquic_stream.c -- stream processing
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <stddef.h>

#ifdef WIN32
#include <malloc.h>
#endif

#include "fiu-local.h"

#include "lsquic.h"

#include "lsquic_int_types.h"
#include "lsquic_packet_common.h"
#include "lsquic_packet_in.h"
#include "lsquic_malo.h"
#include "lsquic_conn_flow.h"
#include "lsquic_rtt.h"
#include "lsquic_sfcw.h"
#include "lsquic_varint.h"
#include "lsquic_hq.h"
#include "lsquic_hash.h"
#include "lsquic_stream.h"
#include "lsquic_conn_public.h"
#include "lsquic_util.h"
#include "lsquic_mm.h"
#include "lsquic_conn.h"
#include "lsquic_data_in_if.h"
#include "lsquic_parse.h"
#include "lsquic_packet_in.h"
#include "lsquic_packet_out.h"
#include "lsquic_engine_public.h"
#include "lsquic_senhist.h"
#include "lsquic_pacer.h"
#include "lsquic_cubic.h"
#include "lsquic_bw_sampler.h"
#include "lsquic_minmax.h"
#include "lsquic_bbr.h"
#include "lsquic_adaptive_cc.h"
#include "lsquic_send_ctl.h"
#include "lsquic_ev_log.h"
#include "lsquic_enc_sess.h"
#include "lsquic_frab_list.h"
#include "lsquic_byteswap.h"
#include "lsquic_ietf.h"

#define LSQUIC_LOGGER_MODULE LSQLM_STREAM
#define LSQUIC_LOG_CONN_ID lsquic_conn_log_cid(stream->conn_pub->lconn)
#define LSQUIC_LOG_STREAM_ID stream->id
#include "lsquic_logger.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static void
drop_frames_in (lsquic_stream_t *stream);

static void
maybe_schedule_call_on_close (lsquic_stream_t *stream);

static int
stream_wantread (lsquic_stream_t *stream, int is_want);

static int
stream_wantwrite (lsquic_stream_t *stream, int is_want);

enum stream_write_options
{
    SWO_BUFFER  = 1 << 0,       /* Allow buffering in sm_buf */
};


static ssize_t
stream_write_to_packets (lsquic_stream_t *, struct lsquic_reader *, size_t,
                                                    enum stream_write_options);

static ssize_t
save_to_buffer (lsquic_stream_t *, struct lsquic_reader *, size_t len);

static int
stream_flush (lsquic_stream_t *stream);

static int
stream_flush_nocheck (lsquic_stream_t *stream);

static void
maybe_remove_from_write_q (lsquic_stream_t *stream, enum stream_q_flags flag);

enum swtp_status { SWTP_OK, SWTP_STOP, SWTP_ERROR };

static enum swtp_status
stream_write_to_packet_std (struct frame_gen_ctx *fg_ctx, const size_t size);

static enum swtp_status
stream_write_to_packet_hsk (struct frame_gen_ctx *fg_ctx, const size_t size);

static enum swtp_status
stream_write_to_packet_crypto (struct frame_gen_ctx *fg_ctx, const size_t size);

static size_t
stream_write_avail_no_frames (struct lsquic_stream *);

static int
stream_readable_non_http (struct lsquic_stream *stream);

static void
stream_reset (struct lsquic_stream *, uint64_t error_code, int do_close);

static size_t
active_hq_frame_sizes (const struct lsquic_stream *);


#if LSQUIC_KEEP_STREAM_HISTORY
/* These values are printable ASCII characters for ease of printing the
 * whole history in a single line of a log message.
 *
 * The list of events is not exhaustive: only most interesting events
 * are recorded.
 */
enum stream_history_event
{
    SHE_EMPTY              =  '\0',     /* Special entry.  No init besides memset required */
    SHE_PLUS               =  '+',      /* Special entry: previous event occured more than once */
    SHE_REACH_FIN          =  'a',
    SHE_EARLY_READ_STOP    =  'A',
    SHE_BLOCKED_OUT        =  'b',
    SHE_CREATED            =  'C',
    SHE_FRAME_IN           =  'd',
    SHE_FRAME_OUT          =  'D',
    SHE_RESET              =  'e',
    SHE_WINDOW_UPDATE      =  'E',
    SHE_FIN_IN             =  'f',
    SHE_FINISHED           =  'F',
    SHE_GOAWAY_IN          =  'g',
    SHE_USER_WRITE_HEADER  =  'h',
    SHE_HEADERS_IN         =  'H',
    SHE_IF_SWITCH          =  'i',
    SHE_ONCLOSE_SCHED      =  'l',
    SHE_ONCLOSE_CALL       =  'L',
    SHE_ONNEW              =  'N',
    SHE_SET_PRIO           =  'p',
    SHE_SHORT_WRITE        =  'q',
    SHE_USER_READ          =  'r',
    SHE_SHUTDOWN_READ      =  'R',
    SHE_RST_IN             =  's',
    SHE_STOP_SENDIG_IN     =  'S',
    SHE_RST_OUT            =  't',
    SHE_RST_ACKED          =  'T',
    SHE_FLUSH              =  'u',
    SHE_STOP_SENDIG_OUT    =  'U',
    SHE_USER_WRITE_DATA    =  'w',
    SHE_SHUTDOWN_WRITE     =  'W',
    SHE_CLOSE              =  'X',
    SHE_DELAY_SW           =  'y',
    SHE_FORCE_FINISH       =  'Z',
    SHE_WANTREAD_NO        =  '0',  /* "YES" must be one more than "NO" */
    SHE_WANTREAD_YES       =  '1',
    SHE_WANTWRITE_NO       =  '2',
    SHE_WANTWRITE_YES      =  '3',
};

static void
sm_history_append (lsquic_stream_t *stream, enum stream_history_event sh_event)
{
    enum stream_history_event prev_event;
    sm_hist_idx_t idx;
    int plus;

    idx = (stream->sm_hist_idx - 1) & SM_HIST_IDX_MASK;
    plus = SHE_PLUS == stream->sm_hist_buf[idx];
    idx = (idx - plus) & SM_HIST_IDX_MASK;
    prev_event = stream->sm_hist_buf[idx];

    if (prev_event == sh_event && plus)
        return;

    if (prev_event == sh_event)
        sh_event = SHE_PLUS;
    stream->sm_hist_buf[ stream->sm_hist_idx++ & SM_HIST_IDX_MASK ] = sh_event;

    if (0 == (stream->sm_hist_idx & SM_HIST_IDX_MASK))
        LSQ_DEBUG("history: [%.*s]", (int) sizeof(stream->sm_hist_buf),
                                                        stream->sm_hist_buf);
}


#   define SM_HISTORY_APPEND(stream, event) sm_history_append(stream, event)
#   define SM_HISTORY_DUMP_REMAINING(stream) do {                           \
        if (stream->sm_hist_idx & SM_HIST_IDX_MASK)                         \
            LSQ_DEBUG("history: [%.*s]",                                    \
                (int) ((stream)->sm_hist_idx & SM_HIST_IDX_MASK),           \
                (stream)->sm_hist_buf);                                     \
    } while (0)
#else
#   define SM_HISTORY_APPEND(stream, event)
#   define SM_HISTORY_DUMP_REMAINING(stream)
#endif


static int
stream_inside_callback (const lsquic_stream_t *stream)
{
    return stream->conn_pub->enpub->enp_flags & ENPUB_PROC;
}


/* This is an approximation.  If data is written or read outside of the
 * event loop, last_prog will be somewhat out of date, but it's close
 * enough for our purposes.
 */
static void
maybe_update_last_progress (struct lsquic_stream *stream)
{
    if (stream->conn_pub && !lsquic_stream_is_critical(stream))
    {
        if (stream->conn_pub->last_prog != stream->conn_pub->last_tick)
            LSQ_DEBUG("update last progress to %"PRIu64,
                                            stream->conn_pub->last_tick);
        stream->conn_pub->last_prog = stream->conn_pub->last_tick;
#ifndef NDEBUG
        stream->sm_last_prog = stream->conn_pub->last_tick;
#endif
    }
}


static void
maybe_conn_to_tickable (lsquic_stream_t *stream)
{
    if (!stream_inside_callback(stream))
        lsquic_engine_add_conn_to_tickable(stream->conn_pub->enpub,
                                           stream->conn_pub->lconn);
}


/* Here, "readable" means that the user is able to read from the stream. */
static void
maybe_conn_to_tickable_if_readable (lsquic_stream_t *stream)
{
    if (!stream_inside_callback(stream) && lsquic_stream_readable(stream))
    {
        lsquic_engine_add_conn_to_tickable(stream->conn_pub->enpub,
                                           stream->conn_pub->lconn);
    }
}


/* Here, "writeable" means that data can be put into packets to be
 * scheduled to be sent out.
 *
 * If `check_can_send' is false, it means that we do not need to check
 * whether packets can be sent.  This check was already performed when
 * we packetized stream data.
 */
static void
maybe_conn_to_tickable_if_writeable (lsquic_stream_t *stream,
                                                    int check_can_send)
{
    if (!stream_inside_callback(stream) &&
            (!check_can_send
             || lsquic_send_ctl_can_send(stream->conn_pub->send_ctl)) &&
          ! lsquic_send_ctl_have_delayed_packets(stream->conn_pub->send_ctl))
    {
        lsquic_engine_add_conn_to_tickable(stream->conn_pub->enpub,
                                           stream->conn_pub->lconn);
    }
}


static int
stream_stalled (const lsquic_stream_t *stream)
{
    return 0 == (stream->sm_qflags & (SMQF_WANT_WRITE|SMQF_WANT_READ)) &&
           ((STREAM_U_READ_DONE|STREAM_U_WRITE_DONE) & stream->stream_flags)
                                    != (STREAM_U_READ_DONE|STREAM_U_WRITE_DONE);
}


static size_t
stream_stream_frame_header_sz (const struct lsquic_stream *stream,
                                                            unsigned data_sz)
{
    return stream->conn_pub->lconn->cn_pf->pf_calc_stream_frame_header_sz(
                                    stream->id, stream->tosend_off, data_sz);
}


static size_t
stream_crypto_frame_header_sz (const struct lsquic_stream *stream,
                                                            unsigned data_sz)
{
    return stream->conn_pub->lconn->cn_pf
         ->pf_calc_crypto_frame_header_sz(stream->tosend_off, data_sz);
}


/* GQUIC-only function */
static int
stream_is_hsk (const struct lsquic_stream *stream)
{
    if (stream->sm_bflags & SMBF_IETF)
        return 0;
    else
        return lsquic_stream_is_crypto(stream);
}


/* This function's only job is to change the allocated packet's header
 * type to HETY_0RTT when stream frames are written before handshake
 * is complete.
 */
static struct lsquic_packet_out *
stream_get_packet_for_stream_0rtt (struct lsquic_send_ctl *ctl,
                unsigned need_at_least, const struct network_path *path,
                const struct lsquic_stream *stream)
{
    struct lsquic_packet_out *packet_out;

    if (stream->conn_pub->lconn->cn_flags & LSCONN_HANDSHAKE_DONE)
    {
        LSQ_DEBUG("switch to regular \"get packet for stream\" function");
        /* Here we drop the "const" because this is a static function.
         * Otherwise, we would not condone such sorcery.
         */
        ((struct lsquic_stream *) stream)->sm_get_packet_for_stream
                                = lsquic_send_ctl_get_packet_for_stream;
        return lsquic_send_ctl_get_packet_for_stream(ctl, need_at_least,
                                                            path, stream);
    }
    else
    {
        packet_out = lsquic_send_ctl_get_packet_for_stream(ctl, need_at_least,
                                                            path, stream);
        if (packet_out)
            packet_out->po_header_type = HETY_0RTT;
        return packet_out;
    }
}


static struct lsquic_stream *
stream_new_common (lsquic_stream_id_t id, struct lsquic_conn_public *conn_pub,
           const struct lsquic_stream_if *stream_if, void *stream_if_ctx,
           enum stream_ctor_flags ctor_flags)
{
    struct lsquic_stream *stream;

    stream = calloc(1, sizeof(*stream));
    if (!stream)
        return NULL;

    if (ctor_flags & SCF_USE_DI_HASH)
        stream->data_in = lsquic_data_in_hash_new(conn_pub, id, 0);
    else
        stream->data_in = lsquic_data_in_nocopy_new(conn_pub, id);
    if (!stream->data_in)
    {
        free(stream);
        return NULL;
    }

    stream->id        = id;
    stream->stream_if = stream_if;
    stream->conn_pub  = conn_pub;
    stream->sm_onnew_arg = stream_if_ctx;
    stream->sm_write_avail = stream_write_avail_no_frames;

    stream->sm_bflags |= ctor_flags & ((1 << N_SMBF_FLAGS) - 1);
    if (conn_pub->lconn->cn_flags & LSCONN_SERVER)
        stream->sm_bflags |= SMBF_SERVER;
    stream->sm_get_packet_for_stream = lsquic_send_ctl_get_packet_for_stream;

    return stream;
}


lsquic_stream_t *
lsquic_stream_new (lsquic_stream_id_t id,
        struct lsquic_conn_public *conn_pub,
        const struct lsquic_stream_if *stream_if, void *stream_if_ctx,
        unsigned initial_window, uint64_t initial_send_off,
        enum stream_ctor_flags ctor_flags)
{
    lsquic_cfcw_t *cfcw;
    lsquic_stream_t *stream;

    stream = stream_new_common(id, conn_pub, stream_if, stream_if_ctx,
                                                                ctor_flags);
    if (!stream)
        return NULL;

    if (!initial_window)
        initial_window = 16 * 1024;

    if (ctor_flags & SCF_IETF)
    {
        cfcw = &conn_pub->cfcw;
        stream->sm_bflags |= SMBF_CONN_LIMITED;
        stream->sm_readable = stream_readable_non_http;
        assert((ctor_flags & (SCF_HTTP|SCF_HTTP_PRIO))
                                                != (SCF_HTTP|SCF_HTTP_PRIO));
        lsquic_stream_set_priority_internal(stream,
                                        LSQUIC_STREAM_DEFAULT_PRIO);
        stream->sm_write_to_packet = stream_write_to_packet_std;
        stream->sm_frame_header_sz = stream_stream_frame_header_sz;
    }
    else
    {
        if (ctor_flags & SCF_CRITICAL)
            cfcw = NULL;
        else
        {
            cfcw = &conn_pub->cfcw;
            stream->sm_bflags |= SMBF_CONN_LIMITED;
            lsquic_stream_set_priority_internal(stream,
                                                LSQUIC_STREAM_DEFAULT_PRIO);
        }
        stream->sm_readable = stream_readable_non_http;
        if (ctor_flags & SCF_CRYPTO_FRAMES)
        {
            stream->sm_frame_header_sz = stream_crypto_frame_header_sz;
            stream->sm_write_to_packet = stream_write_to_packet_crypto;
        }
        else
        {
            if (stream_is_hsk(stream))
                stream->sm_write_to_packet = stream_write_to_packet_hsk;
            else
                stream->sm_write_to_packet = stream_write_to_packet_std;
            stream->sm_frame_header_sz = stream_stream_frame_header_sz;
        }
    }

    if ((stream->sm_bflags & (SMBF_SERVER|SMBF_IETF)) == SMBF_IETF
                    && !(conn_pub->lconn->cn_flags & LSCONN_HANDSHAKE_DONE))
    {
        LSQ_DEBUG("use wrapper \"get packet for stream\" function");
        stream->sm_get_packet_for_stream = stream_get_packet_for_stream_0rtt;
    }

    lsquic_sfcw_init(&stream->fc, initial_window, cfcw, conn_pub, id);
    stream->max_send_off = initial_send_off;
    LSQ_DEBUG("created stream");
    SM_HISTORY_APPEND(stream, SHE_CREATED);
    if (ctor_flags & SCF_CALL_ON_NEW)
        lsquic_stream_call_on_new(stream);
    return stream;
}


struct lsquic_stream *
lsquic_stream_new_crypto (enum enc_level enc_level,
        struct lsquic_conn_public *conn_pub,
        const struct lsquic_stream_if *stream_if, void *stream_if_ctx,
        enum stream_ctor_flags ctor_flags)
{
    struct lsquic_stream *stream;
    lsquic_stream_id_t stream_id;

    assert(ctor_flags & SCF_CRITICAL);

    fiu_return_on("stream/new_crypto", NULL);

    stream_id = ~0ULL - enc_level;
    stream = stream_new_common(stream_id, conn_pub, stream_if,
                                                stream_if_ctx, ctor_flags);
    if (!stream)
        return NULL;

    stream->sm_bflags |= SMBF_CRYPTO|SMBF_IETF;
    stream->sm_enc_level = enc_level;
    /* We allow buffering of up to 16 KB of CRYPTO data (I guess we could
     * make this configurable?).  The window is opened (without sending
     * MAX_STREAM_DATA) as CRYPTO data is consumed.  If too much comes in
     * at a time, we abort with TEC_CRYPTO_BUFFER_EXCEEDED.
     */
    lsquic_sfcw_init(&stream->fc, 16 * 1024, NULL, conn_pub, stream_id);
    /* Don't limit ourselves from sending CRYPTO data.  We assume that
     * the underlying crypto library behaves in a sane manner.
     */
    stream->max_send_off = UINT64_MAX;
    LSQ_DEBUG("created crypto stream");
    SM_HISTORY_APPEND(stream, SHE_CREATED);
    stream->sm_frame_header_sz = stream_crypto_frame_header_sz;
    stream->sm_write_to_packet = stream_write_to_packet_crypto;
    stream->sm_readable = stream_readable_non_http;
    if (ctor_flags & SCF_CALL_ON_NEW)
        lsquic_stream_call_on_new(stream);
    return stream;
}


void
lsquic_stream_call_on_new (lsquic_stream_t *stream)
{
    assert(!(stream->stream_flags & STREAM_ONNEW_DONE));
    if (!(stream->stream_flags & STREAM_ONNEW_DONE))
    {
        LSQ_DEBUG("calling on_new_stream");
        SM_HISTORY_APPEND(stream, SHE_ONNEW);
        stream->stream_flags |= STREAM_ONNEW_DONE;
        stream->st_ctx = stream->stream_if->on_new_stream(stream->sm_onnew_arg,
                                                          stream);
    }
}


static void
decr_conn_cap (struct lsquic_stream *stream, size_t incr)
{
    if (stream->sm_bflags & SMBF_CONN_LIMITED)
    {
        assert(stream->conn_pub->conn_cap.cc_sent >= incr);
        stream->conn_pub->conn_cap.cc_sent -= incr;
        LSQ_DEBUG("decrease cc_sent by %zd to %"PRIu64, incr,
                  stream->conn_pub->conn_cap.cc_sent);

    }
}


static void
maybe_resize_stream_buffer (struct lsquic_stream *stream)
{
    assert(0 == stream->sm_n_buffered);

    if (stream->sm_n_allocated < stream->conn_pub->path->np_pack_size)
    {
        free(stream->sm_buf);
        stream->sm_buf = NULL;
        stream->sm_n_allocated = 0;
    }
    else if (stream->sm_n_allocated > stream->conn_pub->path->np_pack_size)
        stream->sm_n_allocated = stream->conn_pub->path->np_pack_size;
}


static void
drop_buffered_data (struct lsquic_stream *stream)
{
    decr_conn_cap(stream, stream->sm_n_buffered);
    stream->sm_n_buffered = 0;
    maybe_resize_stream_buffer(stream);
    if (stream->sm_qflags & SMQF_WRITE_Q_FLAGS)
        maybe_remove_from_write_q(stream, SMQF_WRITE_Q_FLAGS);
}


void
lsquic_stream_destroy (lsquic_stream_t *stream)
{
    stream->stream_flags |= STREAM_U_WRITE_DONE|STREAM_U_READ_DONE;
    if ((stream->stream_flags & (STREAM_ONNEW_DONE|STREAM_ONCLOSE_DONE)) ==
                                                            STREAM_ONNEW_DONE)
    {
        stream->stream_flags |= STREAM_ONCLOSE_DONE;
        SM_HISTORY_APPEND(stream, SHE_ONCLOSE_CALL);
        stream->stream_if->on_close(stream, stream->st_ctx);
    }
    if (stream->sm_qflags & SMQF_SENDING_FLAGS)
        TAILQ_REMOVE(&stream->conn_pub->sending_streams, stream, next_send_stream);
    if (stream->sm_qflags & SMQF_WANT_READ)
        TAILQ_REMOVE(&stream->conn_pub->read_streams, stream, next_read_stream);
    if (stream->sm_qflags & SMQF_WRITE_Q_FLAGS)
        TAILQ_REMOVE(&stream->conn_pub->write_streams, stream, next_write_stream);
    if (stream->sm_qflags & SMQF_SERVICE_FLAGS)
        TAILQ_REMOVE(&stream->conn_pub->service_streams, stream, next_service_stream);
    drop_buffered_data(stream);
    lsquic_sfcw_consume_rem(&stream->fc);
    drop_frames_in(stream);
    free(stream->sm_buf);
    LSQ_DEBUG("destroyed stream");
    SM_HISTORY_DUMP_REMAINING(stream);
    free(stream);
}


static int
stream_is_finished (struct lsquic_stream *stream)
{
    return lsquic_stream_is_closed(stream)
        && (stream->sm_bflags & SMBF_DELAY_ONCLOSE ?
           /* Need a stricter check when on_close() is delayed: */
            !lsquic_stream_has_unacked_data(stream) :
           /* n_unacked checks that no outgoing packets that reference this
            * stream are outstanding:
            */
            0 == stream->n_unacked)
        && 0 == (stream->sm_qflags & (
           /* This checks that no packets that reference this stream will
            * become outstanding:
            */
                    SMQF_SEND_RST
           /* Can't finish stream until all "self" flags are unset: */
                    | SMQF_SELF_FLAGS))
        && ((stream->stream_flags & STREAM_FORCE_FINISH)
          || (stream->stream_flags & (STREAM_FIN_SENT |STREAM_RST_SENT)));
}


/* This is an internal function */
void
lsquic_stream_force_finish (struct lsquic_stream *stream)
{
    LSQ_DEBUG("stream is now finished");
    SM_HISTORY_APPEND(stream, SHE_FINISHED);
    if (0 == (stream->sm_qflags & SMQF_SERVICE_FLAGS))
        TAILQ_INSERT_TAIL(&stream->conn_pub->service_streams, stream,
                                                next_service_stream);
    stream->sm_qflags |= SMQF_FREE_STREAM;
    stream->stream_flags |= STREAM_FINISHED;
}


static void
maybe_finish_stream (lsquic_stream_t *stream)
{
    if (0 == (stream->stream_flags & STREAM_FINISHED) &&
                                                    stream_is_finished(stream))
        lsquic_stream_force_finish(stream);
}


static void
maybe_schedule_call_on_close (lsquic_stream_t *stream)
{
    if ((stream->stream_flags & (STREAM_U_READ_DONE|STREAM_U_WRITE_DONE|
                     STREAM_ONNEW_DONE|STREAM_ONCLOSE_DONE))
            == (STREAM_U_READ_DONE|STREAM_U_WRITE_DONE|STREAM_ONNEW_DONE)
            && (!(stream->sm_bflags & SMBF_DELAY_ONCLOSE)
                                || !lsquic_stream_has_unacked_data(stream))
            && !(stream->sm_qflags & SMQF_CALL_ONCLOSE))
    {
        if (0 == (stream->sm_qflags & SMQF_SERVICE_FLAGS))
            TAILQ_INSERT_TAIL(&stream->conn_pub->service_streams, stream,
                                                    next_service_stream);
        stream->sm_qflags |= SMQF_CALL_ONCLOSE;
        LSQ_DEBUG("scheduled calling on_close");
        SM_HISTORY_APPEND(stream, SHE_ONCLOSE_SCHED);
    }
}


void
lsquic_stream_call_on_close (lsquic_stream_t *stream)
{
    assert(stream->stream_flags & STREAM_ONNEW_DONE);
    stream->sm_qflags &= ~SMQF_CALL_ONCLOSE;
    if (!(stream->sm_qflags & SMQF_SERVICE_FLAGS))
        TAILQ_REMOVE(&stream->conn_pub->service_streams, stream,
                                                    next_service_stream);
    if (0 == (stream->stream_flags & STREAM_ONCLOSE_DONE))
    {
        LSQ_DEBUG("calling on_close");
        stream->stream_flags |= STREAM_ONCLOSE_DONE;
        SM_HISTORY_APPEND(stream, SHE_ONCLOSE_CALL);
        stream->stream_if->on_close(stream, stream->st_ctx);
    }
    else
        assert(0);
}


static int
stream_has_frame_at_read_offset (struct lsquic_stream *stream)
{
    if (!((stream->stream_flags & STREAM_CACHED_FRAME)
                    && stream->read_offset == stream->sm_last_frame_off))
    {
        stream->sm_has_frame = stream->data_in->di_if->di_get_frame(
                                stream->data_in, stream->read_offset) != NULL;
        stream->sm_last_frame_off = stream->read_offset;
        stream->stream_flags |= STREAM_CACHED_FRAME;
    }
    return stream->sm_has_frame;
}


static int
stream_readable_non_http (struct lsquic_stream *stream)
{
    return stream_has_frame_at_read_offset(stream);
}


static int
maybe_switch_data_in (struct lsquic_stream *stream)
{
    if ((stream->sm_bflags & SMBF_AUTOSWITCH) &&
            (stream->data_in->di_flags & DI_SWITCH_IMPL))
    {
        stream->data_in = stream->data_in->di_if->di_switch_impl(
                                    stream->data_in, stream->read_offset);
        if (!stream->data_in)
        {
            stream->data_in = lsquic_data_in_error_new();
            return -1;
        }
    }

    return 0;
}


/* Drain and discard any incoming data */
static int
stream_readable_discard (struct lsquic_stream *stream)
{
    struct data_frame *data_frame;
    uint64_t toread;
    int fin;

    while ((data_frame = stream->data_in->di_if->di_get_frame(
                                    stream->data_in, stream->read_offset)))
    {
        fin = data_frame->df_fin;
        toread = data_frame->df_size - data_frame->df_read_off;
        stream->read_offset += toread;
        data_frame->df_read_off = data_frame->df_size;
        stream->data_in->di_if->di_frame_done(stream->data_in, data_frame);
        if (fin)
            break;
    }

    (void) maybe_switch_data_in(stream);

    return 0;   /* Never readable */
}


static int
stream_is_read_reset (const struct lsquic_stream *stream)
{
    if (stream->sm_bflags & SMBF_IETF)
        return stream->stream_flags & STREAM_RST_RECVD;
    else
        return (stream->stream_flags & (STREAM_RST_RECVD|STREAM_RST_SENT))
            || (stream->sm_qflags & SMQF_SEND_RST);
}


int
lsquic_stream_readable (struct lsquic_stream *stream)
{
    /* A stream is readable if one of the following is true: */
    return
        /* - It is already finished: in that case, lsquic_stream_read() will
         *   return 0.
         */
            (stream->stream_flags & STREAM_FIN_REACHED)
        /* - The stream is reset, by either side.  In this case,
         *   lsquic_stream_read() will return -1 (we want the user to be
         *   able to collect the error).
         */
        ||  stream_is_read_reset(stream)
        /* Type-dependent readability check: */
        ||  stream->sm_readable(stream);
    ;
}


/* Return true if write end of the stream has been reset.
 * Note that the logic for gQUIC is the same for write and read resets.
 */
int
lsquic_stream_is_write_reset (const struct lsquic_stream *stream)
{
    /* The two protocols use different frames to effect write reset: */
    const enum stream_flags cause_flag = stream->sm_bflags & SMBF_IETF
        ? STREAM_SS_RECVD : STREAM_RST_RECVD;
    return (stream->stream_flags & (cause_flag|STREAM_RST_SENT))
        || (stream->sm_qflags & SMQF_SEND_RST);
}


static int
stream_writeable (struct lsquic_stream *stream)
{
    /* A stream is writeable if one of the following is true: */
    return
        /* - The stream is reset, by either side.  In this case,
         *   lsquic_stream_write() will return -1 (we want the user to be
         *   able to collect the error).
         */
           lsquic_stream_is_write_reset(stream)
        /* - Data can be written to stream: */
        || lsquic_stream_write_avail(stream)
    ;
}


static size_t
stream_write_avail_no_frames (struct lsquic_stream *stream)
{
    uint64_t stream_avail, conn_avail;

    stream_avail = stream->max_send_off - stream->tosend_off
                                                - stream->sm_n_buffered;

    if (stream->sm_bflags & SMBF_CONN_LIMITED)
    {
        conn_avail = lsquic_conn_cap_avail(&stream->conn_pub->conn_cap);
        if (conn_avail < stream_avail)
            stream_avail = conn_avail;
    }

    return stream_avail;
}


size_t
lsquic_stream_write_avail (struct lsquic_stream *stream)
{
    return stream->sm_write_avail(stream);
}


int
lsquic_stream_update_sfcw (lsquic_stream_t *stream, uint64_t max_off)
{
    struct lsquic_conn *lconn;

    if (max_off > lsquic_sfcw_get_max_recv_off(&stream->fc) &&
                    !lsquic_sfcw_set_max_recv_off(&stream->fc, max_off))
    {
        if (stream->sm_bflags & SMBF_IETF)
        {
            lconn = stream->conn_pub->lconn;
            if (lsquic_stream_is_crypto(stream))
                lconn->cn_if->ci_abort_error(lconn, 0,
                    TEC_CRYPTO_BUFFER_EXCEEDED,
                    "crypto buffer exceeded on in crypto level %"PRIu64,
                    crypto_level(stream));
            else
                lconn->cn_if->ci_abort_error(lconn, 0, TEC_FLOW_CONTROL_ERROR,
                    "flow control violation on stream %"PRIu64, stream->id);
        }
        return -1;
    }
    if (lsquic_sfcw_fc_offsets_changed(&stream->fc))
    {
        if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
            TAILQ_INSERT_TAIL(&stream->conn_pub->sending_streams, stream,
                                                    next_send_stream);
        stream->sm_qflags |= SMQF_SEND_WUF;
    }
    return 0;
}


int
lsquic_stream_frame_in (lsquic_stream_t *stream, stream_frame_t *frame)
{
    uint64_t max_off;
    int got_next_offset, rv, free_frame;
    enum ins_frame ins_frame;
    struct lsquic_conn *lconn;

    assert(frame->packet_in);

    SM_HISTORY_APPEND(stream, SHE_FRAME_IN);
    LSQ_DEBUG("received stream frame, offset %"PRIu64", len %u; "
        "fin: %d", frame->data_frame.df_offset, frame->data_frame.df_size, !!frame->data_frame.df_fin);

    rv = -1;
    if ((stream->sm_bflags & SMBF_USE_HEADERS)
                            && (stream->stream_flags & STREAM_HEAD_IN_FIN))
    {
        goto release_packet_frame;
    }

    if (frame->data_frame.df_fin && (stream->sm_bflags & SMBF_IETF)
            && (stream->stream_flags & STREAM_FIN_RECVD)
            && stream->sm_fin_off != DF_END(frame))
    {
        lconn = stream->conn_pub->lconn;
        lconn->cn_if->ci_abort_error(lconn, 0, TEC_FINAL_SIZE_ERROR,
            "new final size %"PRIu64" from STREAM frame (id: %"PRIu64") does "
            "not match previous final size %"PRIu64, DF_END(frame),
            stream->id, stream->sm_fin_off);
        goto release_packet_frame;
    }

    got_next_offset = frame->data_frame.df_offset == stream->read_offset;
  insert_frame:
    ins_frame = stream->data_in->di_if->di_insert_frame(stream->data_in, frame, stream->read_offset);
    if (INS_FRAME_OK == ins_frame)
    {
        /* Update maximum offset in the flow controller and check for flow
         * control violation:
         */
        free_frame = !stream->data_in->di_if->di_own_on_ok;
        max_off = frame->data_frame.df_offset + frame->data_frame.df_size;
        if (0 != lsquic_stream_update_sfcw(stream, max_off))
            goto end_ok;
        if (frame->data_frame.df_fin)
        {
            SM_HISTORY_APPEND(stream, SHE_FIN_IN);
            stream->stream_flags |= STREAM_FIN_RECVD;
            stream->sm_qflags &= ~SMQF_WAIT_FIN_OFF;
            stream->sm_fin_off = DF_END(frame);
            maybe_finish_stream(stream);
        }
        if (0 != maybe_switch_data_in(stream))
            goto end_ok;
        if (got_next_offset)
            /* Checking the offset saves di_get_frame() call */
            maybe_conn_to_tickable_if_readable(stream);
        rv = 0;
  end_ok:
        if (free_frame)
            lsquic_malo_put(frame);
        stream->stream_flags &= ~STREAM_CACHED_FRAME;
        return rv;
    }
    else if (INS_FRAME_DUP == ins_frame)
    {
        rv = 0;
    }
    else if (INS_FRAME_OVERLAP == ins_frame)
    {
        LSQ_DEBUG("overlap: switching DATA IN implementation");
        stream->data_in = stream->data_in->di_if->di_switch_impl(
                                    stream->data_in, stream->read_offset);
        if (stream->data_in)
            goto insert_frame;
        stream->data_in = lsquic_data_in_error_new();
    }
    else
    {
        assert(INS_FRAME_ERR == ins_frame);
    }
release_packet_frame:
    lsquic_packet_in_put(stream->conn_pub->mm, frame->packet_in);
    lsquic_malo_put(frame);
    return rv;
}


static void
drop_frames_in (lsquic_stream_t *stream)
{
    if (stream->data_in)
    {
        stream->data_in->di_if->di_destroy(stream->data_in);
        /* To avoid checking whether `data_in` is set, just set to the error
         * data-in stream.  It does the right thing after incoming data is
         * dropped.
         */
        stream->data_in = lsquic_data_in_error_new();
        stream->stream_flags &= ~STREAM_CACHED_FRAME;
    }
}


static void
maybe_elide_stream_frames (struct lsquic_stream *stream)
{
    if (!(stream->stream_flags & STREAM_FRAMES_ELIDED))
    {
        if (stream->n_unacked)
            lsquic_send_ctl_elide_stream_frames(stream->conn_pub->send_ctl,
                                                stream->id);
        stream->stream_flags |= STREAM_FRAMES_ELIDED;
    }
}


int
lsquic_stream_rst_in (lsquic_stream_t *stream, uint64_t offset,
                      uint64_t error_code)
{
    struct lsquic_conn *lconn;

    if ((stream->sm_bflags & SMBF_IETF)
            && (stream->stream_flags & STREAM_FIN_RECVD)
            && stream->sm_fin_off != offset)
    {
        lconn = stream->conn_pub->lconn;
        lconn->cn_if->ci_abort_error(lconn, 0, TEC_FINAL_SIZE_ERROR,
            "final size %"PRIu64" from RESET_STREAM frame (id: %"PRIu64") "
            "does not match previous final size %"PRIu64, offset,
            stream->id, stream->sm_fin_off);
        return -1;
    }

    if (stream->stream_flags & STREAM_RST_RECVD)
    {
        LSQ_DEBUG("ignore duplicate RST_STREAM frame");
        return 0;
    }

    SM_HISTORY_APPEND(stream, SHE_RST_IN);
    /* This flag must always be set, even if we are "ignoring" it: it is
     * used by elision code.
     */
    stream->stream_flags |= STREAM_RST_RECVD;

    if (lsquic_sfcw_get_max_recv_off(&stream->fc) > offset)
    {
        LSQ_INFO("RST_STREAM invalid: its offset %"PRIu64" is "
            "smaller than that of byte following the last byte we have seen: "
            "%"PRIu64, offset,
            lsquic_sfcw_get_max_recv_off(&stream->fc));
        return -1;
    }

    if (!lsquic_sfcw_set_max_recv_off(&stream->fc, offset))
    {
        LSQ_INFO("RST_STREAM invalid: its offset %"PRIu64
            " violates flow control", offset);
        return -1;
    }

    if (stream->stream_if->on_reset
                            && !(stream->stream_flags & STREAM_ONCLOSE_DONE))
    {
        if (stream->sm_bflags & SMBF_IETF)
        {
            if (!(stream->sm_dflags & SMDF_ONRESET0))
            {
                stream->stream_if->on_reset(stream, stream->st_ctx, 0);
                stream->sm_dflags |= SMDF_ONRESET0;
            }
        }
        else
        {
            if ((stream->sm_dflags & (SMDF_ONRESET0|SMDF_ONRESET1))
                                    != (SMDF_ONRESET0|SMDF_ONRESET1))
            {
                stream->stream_if->on_reset(stream, stream->st_ctx, 2);
                stream->sm_dflags |= SMDF_ONRESET0|SMDF_ONRESET1;
            }
        }
    }

    /* Let user collect error: */
    maybe_conn_to_tickable_if_readable(stream);

    lsquic_sfcw_consume_rem(&stream->fc);
    drop_frames_in(stream);

    if (!(stream->sm_bflags & SMBF_IETF))
    {
        drop_buffered_data(stream);
        maybe_elide_stream_frames(stream);
    }

    if (stream->sm_qflags & SMQF_WAIT_FIN_OFF)
    {
        stream->sm_qflags &= ~SMQF_WAIT_FIN_OFF;
        LSQ_DEBUG("final offset is now known: %"PRIu64, offset);
    }

    if (!(stream->stream_flags &
                        (STREAM_RST_SENT|STREAM_SS_SENT|STREAM_FIN_SENT))
                            && !(stream->sm_bflags & SMBF_IETF)
                                    && !(stream->sm_qflags & SMQF_SEND_RST))
        stream_reset(stream, 7 /* QUIC_RST_ACKNOWLEDGEMENT */, 0);

    stream->stream_flags |= STREAM_RST_RECVD;

    maybe_finish_stream(stream);
    maybe_schedule_call_on_close(stream);

    return 0;
}


void
lsquic_stream_stop_sending_in (struct lsquic_stream *stream,
                                                        uint64_t error_code)
{
    if (stream->stream_flags & STREAM_SS_RECVD)
    {
        LSQ_DEBUG("ignore duplicate STOP_SENDING frame");
        return;
    }

    SM_HISTORY_APPEND(stream, SHE_STOP_SENDIG_IN);
    stream->stream_flags |= STREAM_SS_RECVD;

    if (stream->stream_if->on_reset && !(stream->sm_dflags & SMDF_ONRESET1)
                            && !(stream->stream_flags & STREAM_ONCLOSE_DONE))
    {
        stream->stream_if->on_reset(stream, stream->st_ctx, 1);
        stream->sm_dflags |= SMDF_ONRESET1;
    }

    /* Let user collect error: */
    maybe_conn_to_tickable_if_writeable(stream, 0);

    lsquic_sfcw_consume_rem(&stream->fc);
    drop_buffered_data(stream);
    maybe_elide_stream_frames(stream);

    if (!(stream->stream_flags & (STREAM_RST_SENT|STREAM_FIN_SENT))
                                    && !(stream->sm_qflags & SMQF_SEND_RST))
        stream_reset(stream, 0, 0);

    if (stream->sm_qflags & (SMQF_SEND_WUF | SMQF_SEND_BLOCKED \
                             | SMQF_SEND_STOP_SENDING))
    {
        stream->sm_qflags &= ~(SMQF_SEND_WUF | SMQF_SEND_BLOCKED \
                               | SMQF_SEND_STOP_SENDING);
        if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
            TAILQ_REMOVE(&stream->conn_pub->sending_streams, stream, next_send_stream);
    }

    maybe_finish_stream(stream);
    maybe_schedule_call_on_close(stream);
}


uint64_t
lsquic_stream_fc_recv_off_const (const struct lsquic_stream *stream)
{
    return lsquic_sfcw_get_fc_recv_off(&stream->fc);
}


void
lsquic_stream_max_stream_data_sent (struct lsquic_stream *stream)
{
    assert(stream->sm_qflags & SMQF_SEND_MAX_STREAM_DATA);
    stream->sm_qflags &= ~SMQF_SEND_MAX_STREAM_DATA;
    if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
        TAILQ_REMOVE(&stream->conn_pub->sending_streams, stream, next_send_stream);
    stream->sm_last_recv_off = lsquic_sfcw_get_fc_recv_off(&stream->fc);
}


uint64_t
lsquic_stream_fc_recv_off (lsquic_stream_t *stream)
{
    assert(stream->sm_qflags & SMQF_SEND_WUF);
    stream->sm_qflags &= ~SMQF_SEND_WUF;
    if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
        TAILQ_REMOVE(&stream->conn_pub->sending_streams, stream, next_send_stream);
    return stream->sm_last_recv_off = lsquic_sfcw_get_fc_recv_off(&stream->fc);
}


void
lsquic_stream_peer_blocked (struct lsquic_stream *stream, uint64_t peer_off)
{
    uint64_t last_off;

    if (stream->sm_last_recv_off)
        last_off = stream->sm_last_recv_off;
    else
        /* This gets advertized in transport parameters */
        last_off = lsquic_sfcw_get_max_recv_off(&stream->fc);

    LSQ_DEBUG("Peer blocked at %"PRIu64", while the last MAX_STREAM_DATA "
        "frame we sent advertized the limit of %"PRIu64, peer_off, last_off);

    if (peer_off > last_off && !(stream->sm_qflags & SMQF_SEND_WUF))
    {
        if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
            TAILQ_INSERT_TAIL(&stream->conn_pub->sending_streams, stream,
                                                    next_send_stream);
        stream->sm_qflags |= SMQF_SEND_WUF;
        LSQ_DEBUG("marked to send MAX_STREAM_DATA frame");
    }
    else if (stream->sm_qflags & SMQF_SEND_WUF)
        LSQ_DEBUG("MAX_STREAM_DATA frame is already scheduled");
    else if (stream->sm_last_recv_off)
        LSQ_DEBUG("MAX_STREAM_DATA(%"PRIu64") has already been either "
            "packetized or sent", stream->sm_last_recv_off);
    else
        LSQ_INFO("Peer should have receive transport param limit "
            "of %"PRIu64"; odd.", last_off);
}


void
lsquic_stream_blocked_frame_sent (lsquic_stream_t *stream)
{
    assert(stream->sm_qflags & SMQF_SEND_BLOCKED);
    SM_HISTORY_APPEND(stream, SHE_BLOCKED_OUT);
    stream->sm_qflags &= ~SMQF_SEND_BLOCKED;
    stream->stream_flags |= STREAM_BLOCKED_SENT;
    if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
        TAILQ_REMOVE(&stream->conn_pub->sending_streams, stream, next_send_stream);
}


void
lsquic_stream_rst_frame_sent (lsquic_stream_t *stream)
{
    assert(stream->sm_qflags & SMQF_SEND_RST);
    SM_HISTORY_APPEND(stream, SHE_RST_OUT);
    stream->sm_qflags &= ~SMQF_SEND_RST;
    if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
        TAILQ_REMOVE(&stream->conn_pub->sending_streams, stream, next_send_stream);

    /* [RFC9000 QUIC] Section 19.4. RESET_Frames
     *  An endpoint uses a RESET_STREAM frame (type=0x04)
     *  to abruptly terminate the sending part of a stream.
     */
    stream->stream_flags |= STREAM_RST_SENT|STREAM_U_WRITE_DONE;
    maybe_finish_stream(stream);
}


static void
verify_cl_on_fin (struct lsquic_stream *stream)
{
    struct lsquic_conn *lconn;

    /* The rules in RFC7230, Section 3.3.2 are a bit too intricate.  We take
     * a simple approach and verify content-length only when there was any
     * payload at all.
     */
    if (stream->sm_data_in != 0 && stream->sm_cont_len != stream->sm_data_in)
    {
        lconn = stream->conn_pub->lconn;
        lconn->cn_if->ci_abort_error(lconn, 1, HEC_MESSAGE_ERROR,
            "number of bytes in DATA frames of stream %"PRIu64" is %llu, "
            "while content-length specified of %llu", stream->id,
            stream->sm_data_in, stream->sm_cont_len);
    }
}


static void
stream_consumed_bytes (struct lsquic_stream *stream)
{
    lsquic_sfcw_set_read_off(&stream->fc, stream->read_offset);
    if (lsquic_sfcw_fc_offsets_changed(&stream->fc)
            /* We advance crypto streams' offsets (to control amount of
             * buffering we allow), but do not send MAX_STREAM_DATA frames.
             */
            && !((stream->sm_bflags & (SMBF_IETF|SMBF_CRYPTO))
                                                == (SMBF_IETF|SMBF_CRYPTO)))
    {
        if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
            TAILQ_INSERT_TAIL(&stream->conn_pub->sending_streams, stream,
                                                            next_send_stream);
        stream->sm_qflags |= SMQF_SEND_WUF;
        maybe_conn_to_tickable_if_writeable(stream, 1);
    }
}


static ssize_t
read_data_frames (struct lsquic_stream *stream, int do_filtering,
        size_t (*readf)(void *, const unsigned char *, size_t, int), void *ctx)
{
    struct data_frame *data_frame;
    size_t nread, toread, total_nread;
    int short_read, processed_frames;

    processed_frames = 0;
    total_nread = 0;

    while ((data_frame = stream->data_in->di_if->di_get_frame(
                                        stream->data_in, stream->read_offset)))
    {

        ++processed_frames;

        do
        {
            if (do_filtering && stream->sm_sfi)
                toread = stream->sm_sfi->sfi_filter_df(stream, data_frame);
            else
                toread = data_frame->df_size - data_frame->df_read_off;

            if (toread || data_frame->df_fin)
            {
                nread = readf(ctx, data_frame->df_data + data_frame->df_read_off,
                                                     toread, data_frame->df_fin);
                if (do_filtering && stream->sm_sfi)
                    stream->sm_sfi->sfi_decr_left(stream, nread);
                data_frame->df_read_off += nread;
                stream->read_offset += nread;
                total_nread += nread;
                short_read = nread < toread;
            }
            else
                short_read = 0;

            if (data_frame->df_read_off == data_frame->df_size)
            {
                const int fin = data_frame->df_fin;
                stream->data_in->di_if->di_frame_done(stream->data_in, data_frame);
                data_frame = NULL;
                if (0 != maybe_switch_data_in(stream))
                    return -1;
                if (fin)
                {
                    stream->stream_flags |= STREAM_FIN_REACHED;
                    if (stream->sm_bflags & SMBF_VERIFY_CL)
                        verify_cl_on_fin(stream);
                    goto end_while;
                }
            }
            else if (short_read)
                goto end_while;
        }
        while (data_frame);
    }
  end_while:

    if (processed_frames)
        stream_consumed_bytes(stream);

    return total_nread;
}


static ssize_t
stream_readf (struct lsquic_stream *stream,
        size_t (*readf)(void *, const unsigned char *, size_t, int), void *ctx)
{
    size_t total_nread;
    ssize_t nread;

    total_nread = 0;

    nread = read_data_frames(stream, 1, readf, ctx);
    if (nread < 0)
        return nread;
    total_nread += (size_t) nread;

    LSQ_DEBUG("%s: read %zd bytes, read offset %"PRIu64", reached fin: %d",
        __func__, total_nread, stream->read_offset,
        !!(stream->stream_flags & STREAM_FIN_REACHED));

    if (total_nread)
        return total_nread;
    else if (stream->stream_flags & STREAM_FIN_REACHED)
        return 0;
    else
    {
        errno = EWOULDBLOCK;
        return -1;
    }
}


/* This function returns 0 when EOF is reached.
 */
ssize_t
lsquic_stream_readf (struct lsquic_stream *stream,
        size_t (*readf)(void *, const unsigned char *, size_t, int), void *ctx)
{
    ssize_t nread;

    SM_HISTORY_APPEND(stream, SHE_USER_READ);

    if (stream_is_read_reset(stream))
    {
        if (stream->stream_flags & STREAM_RST_RECVD)
            stream->stream_flags |= STREAM_RST_READ;
        errno = ECONNRESET;
        return -1;
    }
    if (stream->stream_flags & STREAM_U_READ_DONE)
    {
        errno = EBADF;
        return -1;
    }
    if (stream->stream_flags & STREAM_FIN_REACHED)
    {
       if (!(stream->sm_bflags & SMBF_USE_HEADERS))
           return 0;
    }

    nread = stream_readf(stream, readf, ctx);
    if (nread >= 0)
        maybe_update_last_progress(stream);

    return nread;
}


struct readv_ctx
{
    const struct iovec        *iov;
    const struct iovec *const  end;
    unsigned char             *p;
};


static size_t
readv_f (void *ctx_p, const unsigned char *buf, size_t len, int fin)
{
    struct readv_ctx *const ctx = ctx_p;
    const unsigned char *const end = buf + len;
    size_t ntocopy;

    while (ctx->iov < ctx->end && buf < end)
    {
        ntocopy = (unsigned char *) ctx->iov->iov_base + ctx->iov->iov_len
                                                                    - ctx->p;
        if (ntocopy > (size_t) (end - buf))
            ntocopy = end - buf;
        memcpy(ctx->p, buf, ntocopy);
        ctx->p += ntocopy;
        buf += ntocopy;
        if (ctx->p == (unsigned char *) ctx->iov->iov_base + ctx->iov->iov_len)
        {
            do
                ++ctx->iov;
            while (ctx->iov < ctx->end && ctx->iov->iov_len == 0);
            if (ctx->iov < ctx->end)
                ctx->p = ctx->iov->iov_base;
            else
                break;
        }
    }

    return len - (end - buf);
}


ssize_t
lsquic_stream_readv (struct lsquic_stream *stream, const struct iovec *iov,
                     int iovcnt)
{
    struct readv_ctx ctx = { iov, iov + iovcnt, iov->iov_base, };
    return lsquic_stream_readf(stream, readv_f, &ctx);
}


ssize_t
lsquic_stream_read (lsquic_stream_t *stream, void *buf, size_t len)
{
    struct iovec iov = { .iov_base = buf, .iov_len = len, };
    return lsquic_stream_readv(stream, &iov, 1);
}


void
lsquic_stream_ss_frame_sent (struct lsquic_stream *stream)
{
    assert(stream->sm_qflags & SMQF_SEND_STOP_SENDING);
    SM_HISTORY_APPEND(stream, SHE_STOP_SENDIG_OUT);
    stream->sm_qflags &= ~SMQF_SEND_STOP_SENDING;
    stream->stream_flags |= STREAM_SS_SENT;
    if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
        TAILQ_REMOVE(&stream->conn_pub->sending_streams, stream, next_send_stream);
}


static void
handle_early_read_shutdown_ietf (struct lsquic_stream *stream)
{
    if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
        TAILQ_INSERT_TAIL(&stream->conn_pub->sending_streams, stream,
                                                    next_send_stream);
    stream->sm_qflags |= SMQF_SEND_STOP_SENDING|SMQF_WAIT_FIN_OFF;
}


static void
handle_early_read_shutdown_gquic (struct lsquic_stream *stream)
{
    if (!(stream->stream_flags & STREAM_RST_SENT))
    {
        stream_reset(stream, 7 /* QUIC_STREAM_CANCELLED */, 0);
        stream->sm_qflags |= SMQF_WAIT_FIN_OFF;
    }
}


static void
handle_early_read_shutdown (struct lsquic_stream *stream)
{
    if (stream->sm_bflags & SMBF_IETF)
        handle_early_read_shutdown_ietf(stream);
    else
        handle_early_read_shutdown_gquic(stream);
}


static void
stream_shutdown_read (lsquic_stream_t *stream)
{
    if (!(stream->stream_flags & STREAM_U_READ_DONE))
    {
        if (!(stream->stream_flags & STREAM_FIN_REACHED))
        {
            LSQ_DEBUG("read shut down before reading FIN.  (FIN received: %d)",
                !!(stream->stream_flags & STREAM_FIN_RECVD));
            SM_HISTORY_APPEND(stream, SHE_EARLY_READ_STOP);
            if (!(stream->stream_flags & (STREAM_FIN_RECVD|STREAM_RST_RECVD)))
                handle_early_read_shutdown(stream);
        }
        SM_HISTORY_APPEND(stream, SHE_SHUTDOWN_READ);
        stream->stream_flags |= STREAM_U_READ_DONE;
        stream->sm_readable = stream_readable_discard;
        stream_wantread(stream, 0);
        maybe_finish_stream(stream);
    }
}


static int
stream_is_incoming_unidir (const struct lsquic_stream *stream)
{
    enum stream_id_type sit;

    if (stream->sm_bflags & SMBF_IETF)
    {
        sit = stream->id & SIT_MASK;
        if (stream->sm_bflags & SMBF_SERVER)
            return sit == SIT_UNI_CLIENT;
        else
            return sit == SIT_UNI_SERVER;
    }
    else
        return 0;
}


static void
stream_shutdown_write (lsquic_stream_t *stream)
{
    if (stream->stream_flags & STREAM_U_WRITE_DONE)
        return;

    SM_HISTORY_APPEND(stream, SHE_SHUTDOWN_WRITE);
    stream->stream_flags |= STREAM_U_WRITE_DONE;
    stream_wantwrite(stream, 0);

    /* Don't bother to check whether there is anything else to write if
     * the flags indicate that nothing else should be written.
     */
    if (!(stream->sm_bflags & SMBF_CRYPTO)
            && !((stream->stream_flags & (STREAM_FIN_SENT|STREAM_RST_SENT))
                    || (stream->sm_qflags & SMQF_SEND_RST))
                && !stream_is_incoming_unidir(stream)
                        /* In gQUIC, receiving a RESET means "stop sending" */
                    && !(!(stream->sm_bflags & SMBF_IETF)
                                && (stream->stream_flags & STREAM_RST_RECVD)))
    {
        if ((stream->sm_bflags & SMBF_USE_HEADERS)
                && !(stream->stream_flags & STREAM_HEADERS_SENT))
        {
            LSQ_DEBUG("headers not sent, send a reset");
            stream_reset(stream, 0, 1);
        }
        else if (stream->sm_n_buffered == 0)
        {
            if (0 == lsquic_send_ctl_turn_on_fin(stream->conn_pub->send_ctl,
                                                 stream))
            {
                LSQ_DEBUG("turned on FIN flag in the yet-unsent STREAM frame");
                stream->stream_flags |= STREAM_FIN_SENT;
                if (stream->sm_qflags & SMQF_WANT_FLUSH)
                {
                    LSQ_DEBUG("turned off SMQF_WANT_FLUSH flag as FIN flag is turned on.");
                    maybe_remove_from_write_q(stream, SMQF_WANT_FLUSH);
                }
            }
            else
            {
                LSQ_DEBUG("have to create a separate STREAM frame with FIN "
                          "flag in it");
                (void) stream_flush_nocheck(stream);
            }
        }
        else
            (void) stream_flush_nocheck(stream);
    }
}


static void
maybe_stream_shutdown_write (struct lsquic_stream *stream)
{
    if (stream->sm_send_headers_state == SSHS_BEGIN)
        stream_shutdown_write(stream);
    else if (0 == (stream->stream_flags & STREAM_DELAYED_SW))
    {
        LSQ_DEBUG("shutdown delayed");
        SM_HISTORY_APPEND(stream, SHE_DELAY_SW);
        stream->stream_flags |= STREAM_DELAYED_SW;
    }
}


int
lsquic_stream_shutdown (lsquic_stream_t *stream, int how)
{
    LSQ_DEBUG("shutdown; how: %d", how);
    if (lsquic_stream_is_closed(stream))
    {
        LSQ_INFO("Attempt to shut down a closed stream");
        errno = EBADF;
        return -1;
    }
    /* 0: read, 1: write: 2: read and write
     */
    if (how < 0 || how > 2)
    {
        errno = EINVAL;
        return -1;
    }

    if (how)
        maybe_stream_shutdown_write(stream);
    if (how != 1)
        stream_shutdown_read(stream);

    maybe_finish_stream(stream);
    maybe_schedule_call_on_close(stream);
    if (how && !(stream->stream_flags & STREAM_DELAYED_SW))
        maybe_conn_to_tickable_if_writeable(stream, 1);

    return 0;
}


static int
stream_wantread (lsquic_stream_t *stream, int is_want)
{
    const int old_val = !!(stream->sm_qflags & SMQF_WANT_READ);
    const int new_val = !!is_want;
    if (old_val != new_val)
    {
        if (new_val)
        {
            if (!old_val)
                TAILQ_INSERT_TAIL(&stream->conn_pub->read_streams, stream,
                                                            next_read_stream);
            stream->sm_qflags |= SMQF_WANT_READ;
        }
        else
        {
            stream->sm_qflags &= ~SMQF_WANT_READ;
            if (old_val)
                TAILQ_REMOVE(&stream->conn_pub->read_streams, stream,
                                                            next_read_stream);
        }
    }
    return old_val;
}


static void
maybe_put_onto_write_q (lsquic_stream_t *stream, enum stream_q_flags flag)
{
    assert(SMQF_WRITE_Q_FLAGS & flag);
    if (!(stream->sm_qflags & SMQF_WRITE_Q_FLAGS))
    {
        LSQ_DEBUG("put on write queue");
        TAILQ_INSERT_TAIL(&stream->conn_pub->write_streams, stream,
                                                        next_write_stream);
    }
    stream->sm_qflags |= flag;
}


static void
maybe_remove_from_write_q (lsquic_stream_t *stream, enum stream_q_flags flag)
{
    assert(SMQF_WRITE_Q_FLAGS & flag);
    if (stream->sm_qflags & flag)
    {
        stream->sm_qflags &= ~flag;
        if (!(stream->sm_qflags & SMQF_WRITE_Q_FLAGS))
            TAILQ_REMOVE(&stream->conn_pub->write_streams, stream,
                                                        next_write_stream);
    }
}


static int
stream_wantwrite (struct lsquic_stream *stream, int new_val)
{
    const int old_val = !!(stream->sm_qflags & SMQF_WANT_WRITE);

    assert(0 == (new_val & ~1));    /* new_val is either 0 or 1 */

    if (old_val != new_val)
    {
        if (new_val)
            maybe_put_onto_write_q(stream, SMQF_WANT_WRITE);
        else
            maybe_remove_from_write_q(stream, SMQF_WANT_WRITE);
    }
    return old_val;
}


int
lsquic_stream_wantread (lsquic_stream_t *stream, int is_want)
{
    SM_HISTORY_APPEND(stream, SHE_WANTREAD_NO + !!is_want);
    if (!(stream->stream_flags & STREAM_U_READ_DONE))
    {
        if (is_want)
            maybe_conn_to_tickable_if_readable(stream);
        return stream_wantread(stream, is_want);
    }
    else
    {
        errno = EBADF;
        return -1;
    }
}


int
lsquic_stream_wantwrite (lsquic_stream_t *stream, int is_want)
{
    int old_val;

    is_want = !!is_want;

    SM_HISTORY_APPEND(stream, SHE_WANTWRITE_NO + is_want);
    if (0 == (stream->stream_flags & STREAM_U_WRITE_DONE)
                            && SSHS_BEGIN == stream->sm_send_headers_state)
    {
        stream->sm_saved_want_write = is_want;
        if (is_want)
            maybe_conn_to_tickable_if_writeable(stream, 1);
        return stream_wantwrite(stream, is_want);
    }
    else if (SSHS_BEGIN != stream->sm_send_headers_state)
    {
        old_val = stream->sm_saved_want_write;
        stream->sm_saved_want_write = is_want;
        return old_val;
    }
    else
    {
        errno = EBADF;
        return -1;
    }
}


struct progress
{
    enum stream_flags   s_flags;
    enum stream_q_flags q_flags;
};


static struct progress
stream_progress (const struct lsquic_stream *stream)
{
    return (struct progress) {
        .s_flags = stream->stream_flags
          & (STREAM_U_WRITE_DONE|STREAM_U_READ_DONE),
        .q_flags = stream->sm_qflags
          & (SMQF_WANT_READ|SMQF_WANT_WRITE|SMQF_WANT_FLUSH|SMQF_SEND_RST),
    };
}


static int
progress_eq (struct progress a, struct progress b)
{
    return a.s_flags == b.s_flags && a.q_flags == b.q_flags;
}


static void
stream_dispatch_read_events_loop (lsquic_stream_t *stream)
{
    unsigned no_progress_count, no_progress_limit;
    struct progress progress;
    uint64_t size;

    no_progress_limit = stream->conn_pub->enpub->enp_settings.es_progress_check;

    no_progress_count = 0;
    while ((stream->sm_qflags & SMQF_WANT_READ)
                                            && lsquic_stream_readable(stream))
    {
        progress = stream_progress(stream);
        size  = stream->read_offset;

        stream->stream_if->on_read(stream, stream->st_ctx);

        if (no_progress_limit && size == stream->read_offset &&
                                progress_eq(progress, stream_progress(stream)))
        {
            ++no_progress_count;
            if (no_progress_count >= no_progress_limit)
            {
                LSQ_WARN("broke suspected infinite loop (%u callback%s without "
                    "progress) in user code reading from stream",
                    no_progress_count,
                    no_progress_count == 1 ? "" : "s");
                break;
            }
        }
        else
            no_progress_count = 0;
    }
}


static void
(*select_on_write (struct lsquic_stream *stream))(struct lsquic_stream *,
                                                        lsquic_stream_ctx_t *)
{
    assert(0 == (stream->stream_flags & STREAM_PUSHING)
                    && SSHS_HBLOCK_SENDING != stream->sm_send_headers_state);
    return stream->stream_if->on_write;
}


static void
stream_dispatch_write_events_loop (lsquic_stream_t *stream)
{
    unsigned no_progress_count, no_progress_limit;
    void (*on_write) (struct lsquic_stream *, lsquic_stream_ctx_t *);
    struct progress progress;

    no_progress_limit = stream->conn_pub->enpub->enp_settings.es_progress_check;

    no_progress_count = 0;
    stream->stream_flags |= STREAM_LAST_WRITE_OK;
    while ((stream->sm_qflags & SMQF_WANT_WRITE)
           && (stream->stream_flags & STREAM_LAST_WRITE_OK)
           && !(stream->stream_flags & STREAM_ONCLOSE_DONE)
           && stream_writeable(stream))
    {
        progress = stream_progress(stream);

        on_write = select_on_write(stream);
        on_write(stream, stream->st_ctx);

        if (no_progress_limit && progress_eq(progress, stream_progress(stream)))
        {
            ++no_progress_count;
            if (no_progress_count >= no_progress_limit)
            {
                LSQ_WARN("broke suspected infinite loop (%u callback%s without "
                    "progress) in user code writing to stream",
                    no_progress_count,
                    no_progress_count == 1 ? "" : "s");
                break;
            }
        }
        else
            no_progress_count = 0;
    }
}


static void
stream_dispatch_read_events_once (lsquic_stream_t *stream)
{
    if ((stream->sm_qflags & SMQF_WANT_READ) && lsquic_stream_readable(stream))
    {
        stream->stream_if->on_read(stream, stream->st_ctx);
    }
}


uint64_t
lsquic_stream_combined_send_off (const struct lsquic_stream *stream)
{
    size_t frames_sizes;

    frames_sizes = active_hq_frame_sizes(stream);
    return stream->tosend_off + stream->sm_n_buffered + frames_sizes;
}


static void
maybe_mark_as_blocked (lsquic_stream_t *stream)
{
    struct lsquic_conn_cap *cc;
    uint64_t used;

    used = lsquic_stream_combined_send_off(stream);
    if (stream->max_send_off == used)
    {
        if (stream->blocked_off < stream->max_send_off)
        {
            stream->blocked_off = used;
            if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
                TAILQ_INSERT_TAIL(&stream->conn_pub->sending_streams, stream,
                                                            next_send_stream);
            stream->sm_qflags |= SMQF_SEND_BLOCKED;
            LSQ_DEBUG("marked stream-blocked at stream offset "
                                            "%"PRIu64, stream->blocked_off);
        }
        else
            LSQ_DEBUG("stream is blocked, but BLOCKED frame for offset %"PRIu64
                " has been, or is about to be, sent", stream->blocked_off);
    }

    if ((stream->sm_bflags & SMBF_CONN_LIMITED)
        && (cc = &stream->conn_pub->conn_cap,
                stream->sm_n_buffered == lsquic_conn_cap_avail(cc)))
    {
        if (cc->cc_blocked < cc->cc_max)
        {
            cc->cc_blocked = cc->cc_max;
            stream->conn_pub->lconn->cn_flags |= LSCONN_SEND_BLOCKED;
            LSQ_DEBUG("marked connection-blocked at connection offset "
                                                    "%"PRIu64, cc->cc_max);
        }
        else
            LSQ_DEBUG("stream has already been marked connection-blocked "
                "at offset %"PRIu64, cc->cc_blocked);
    }
}


void
lsquic_stream_dispatch_read_events (lsquic_stream_t *stream)
{
    if (stream->sm_qflags & SMQF_WANT_READ)
    {
        if (stream->sm_bflags & SMBF_RW_ONCE)
            stream_dispatch_read_events_once(stream);
        else
            stream_dispatch_read_events_loop(stream);
    }
}


void
lsquic_stream_dispatch_write_events (lsquic_stream_t *stream)
{
    void (*on_write) (struct lsquic_stream *, lsquic_stream_ctx_t *);
    int progress;
    uint64_t tosend_off;
    unsigned short n_buffered;
    enum stream_q_flags q_flags;

    LSQ_DEBUG("dispatch_write_events, sm_qflags: %d. stream_flags: %d, sm_bflags: %d, "
                "max_send_off: %" PRIu64 ", tosend_off: %" PRIu64 ", sm_n_buffered: %u",
              stream->sm_qflags, stream->stream_flags, stream->sm_bflags,
              stream->max_send_off, stream->tosend_off, stream->sm_n_buffered);

    if (!(stream->sm_qflags & SMQF_WRITE_Q_FLAGS)
        || (stream->stream_flags & STREAM_FINISHED))
        return;

    q_flags = stream->sm_qflags & SMQF_WRITE_Q_FLAGS;
    tosend_off = stream->tosend_off;
    n_buffered = stream->sm_n_buffered;

    if (stream->sm_qflags & SMQF_WANT_FLUSH)
        (void) stream_flush(stream);

    if (stream->sm_bflags & SMBF_RW_ONCE)
    {
        if ((stream->sm_qflags & SMQF_WANT_WRITE)
            && !(stream->stream_flags & STREAM_ONCLOSE_DONE)
            && stream_writeable(stream))
        {
            on_write = select_on_write(stream);
            on_write(stream, stream->st_ctx);
        }
    }
    else
        stream_dispatch_write_events_loop(stream);

    if ((stream->sm_qflags & SMQF_SEND_BLOCKED) &&
        (stream->sm_bflags & SMBF_IETF))
    {
        lsquic_sendctl_gen_stream_blocked_frame(stream->conn_pub->send_ctl, stream);
    }

    /* Progress means either flags or offsets changed: */
    progress = !((stream->sm_qflags & SMQF_WRITE_Q_FLAGS) == q_flags &&
                        stream->tosend_off == tosend_off &&
                            stream->sm_n_buffered == n_buffered);

    if (stream->sm_qflags & SMQF_WRITE_Q_FLAGS)
    {
        if (progress)
        {   /* Move the stream to the end of the list to ensure fairness. */
            TAILQ_REMOVE(&stream->conn_pub->write_streams, stream,
                                                            next_write_stream);
            TAILQ_INSERT_TAIL(&stream->conn_pub->write_streams, stream,
                                                            next_write_stream);
        }
    }
}


static size_t
inner_reader_empty_size (void *ctx)
{
    return 0;
}


static size_t
inner_reader_empty_read (void *ctx, void *buf, size_t count)
{
    return 0;
}


static int
stream_flush (lsquic_stream_t *stream)
{
    struct lsquic_reader empty_reader;
    ssize_t nw;

    assert(stream->sm_qflags & SMQF_WANT_FLUSH);
    assert(stream->sm_n_buffered > 0 ||
        /* Flushing is also used to packetize standalone FIN: */
        ((stream->stream_flags & (STREAM_U_WRITE_DONE|STREAM_FIN_SENT))
                                                    == STREAM_U_WRITE_DONE));

    empty_reader.lsqr_size = inner_reader_empty_size;
    empty_reader.lsqr_read = inner_reader_empty_read;
    empty_reader.lsqr_ctx  = NULL;  /* pro forma */
    nw = stream_write_to_packets(stream, &empty_reader, 0, SWO_BUFFER);

    if (nw >= 0)
    {
        assert(nw == 0);    /* Empty reader: must have read zero bytes */
        return 0;
    }
    else
        return -1;
}


static int
stream_flush_nocheck (lsquic_stream_t *stream)
{
    size_t frames;

    frames = active_hq_frame_sizes(stream);
    stream->sm_flush_to = stream->tosend_off + stream->sm_n_buffered + frames;
    stream->sm_flush_to_payload = stream->sm_payload + stream->sm_n_buffered;
    maybe_put_onto_write_q(stream, SMQF_WANT_FLUSH);
    LSQ_DEBUG("will flush up to offset %"PRIu64, stream->sm_flush_to);

    return stream_flush(stream);
}


int
lsquic_stream_flush (lsquic_stream_t *stream)
{
    if (stream->stream_flags & STREAM_U_WRITE_DONE)
    {
        LSQ_DEBUG("cannot flush closed stream");
        errno = EBADF;
        return -1;
    }

    if (0 == stream->sm_n_buffered)
    {
        LSQ_DEBUG("flushing 0 bytes: noop");
        return 0;
    }

    return stream_flush_nocheck(stream);
}


static size_t
stream_get_n_allowed (const struct lsquic_stream *stream)
{
    if (stream->sm_n_allocated)
        return stream->sm_n_allocated;
    else
        return stream->conn_pub->path->np_pack_size;
}


/* The flush threshold is the maximum size of stream data that can be sent
 * in a full packet.
 */
#ifdef NDEBUG
static
#endif
       size_t
lsquic_stream_flush_threshold (const struct lsquic_stream *stream,
                                                            unsigned data_sz)
{
    enum packet_out_flags flags;
    enum packno_bits bits;
    size_t packet_header_sz, stream_header_sz;
    size_t threshold;

    bits = lsquic_send_ctl_packno_bits(stream->conn_pub->send_ctl, PNS_APP);
    flags = bits << POBIT_SHIFT;
    if (!(stream->conn_pub->lconn->cn_flags & LSCONN_TCID0))
        flags |= PO_CONN_ID;
    if (stream_is_hsk(stream))
        flags |= PO_LONGHEAD;

    packet_header_sz = lsquic_po_header_length(stream->conn_pub->lconn, flags,
                            stream->conn_pub->path->np_dcid.len, HETY_SHORT);
    stream_header_sz = stream->sm_frame_header_sz(stream, data_sz);

    threshold = stream_get_n_allowed(stream)
              - packet_header_sz - stream_header_sz;
    return threshold;
}


#define COMMON_WRITE_CHECKS() do {                                          \
    if ((stream->sm_bflags & SMBF_USE_HEADERS)                              \
            && !(stream->stream_flags & STREAM_HEADERS_SENT))               \
    {                                                                       \
        if (SSHS_BEGIN != stream->sm_send_headers_state)                    \
        {                                                                   \
            LSQ_DEBUG("still sending headers: no writing allowed");         \
            return 0;                                                       \
        }                                                                   \
        else                                                                \
        {                                                                   \
            LSQ_INFO("Attempt to write to stream before sending HTTP "      \
                                                                "headers"); \
            errno = EILSEQ;                                                 \
            return -1;                                                      \
        }                                                                   \
    }                                                                       \
    if (lsquic_stream_is_write_reset(stream))                               \
    {                                                                       \
        LSQ_INFO("Attempt to write to stream after it had been reset");     \
        errno = ECONNRESET;                                                 \
        return -1;                                                          \
    }                                                                       \
    if (stream->stream_flags & (STREAM_U_WRITE_DONE|STREAM_FIN_SENT))       \
    {                                                                       \
        LSQ_INFO("Attempt to write to stream after it was closed for "      \
                                                                "writing"); \
        errno = EBADF;                                                      \
        return -1;                                                          \
    }                                                                       \
} while (0)


struct frame_gen_ctx
{
    lsquic_stream_t      *fgc_stream;
    struct lsquic_reader *fgc_reader;
    /* We keep our own count of how many bytes were read from reader because
     * some readers are external.  The external caller does not have to rely
     * on our count, but it can.
     */
    size_t                fgc_nread_from_reader;
    size_t              (*fgc_size) (void *ctx);
    int                 (*fgc_fin) (void *ctx);
    gsf_read_f            fgc_read;
    size_t                fgc_thresh;
};


static size_t
frame_std_gen_size (void *ctx)
{
    struct frame_gen_ctx *fg_ctx = ctx;
    size_t available, remaining;

    /* Make sure we are not writing past available size: */
    remaining = fg_ctx->fgc_reader->lsqr_size(fg_ctx->fgc_reader->lsqr_ctx);
    available = lsquic_stream_write_avail(fg_ctx->fgc_stream);
    if (available < remaining)
        remaining = available;

    return remaining + fg_ctx->fgc_stream->sm_n_buffered;
}


static size_t
active_hq_frame_sizes (const struct lsquic_stream *stream)
{
    return 0;
}


static int
frame_std_gen_fin (void *ctx)
{
    struct frame_gen_ctx *fg_ctx = ctx;
    return !(fg_ctx->fgc_stream->sm_bflags & SMBF_CRYPTO)
        && (fg_ctx->fgc_stream->stream_flags & STREAM_U_WRITE_DONE)
        && 0 == fg_ctx->fgc_stream->sm_n_buffered
        /* Do not use frame_std_gen_size() as it may chop the real size: */
        && 0 == fg_ctx->fgc_reader->lsqr_size(fg_ctx->fgc_reader->lsqr_ctx);
}


static void
incr_conn_cap (struct lsquic_stream *stream, size_t incr)
{
    if (stream->sm_bflags & SMBF_CONN_LIMITED)
    {
        stream->conn_pub->conn_cap.cc_sent += incr;
        assert(stream->conn_pub->conn_cap.cc_sent
                                    <= stream->conn_pub->conn_cap.cc_max);
        LSQ_DEBUG("increase cc_sent by %zd to %"PRIu64, incr,
               stream->conn_pub->conn_cap.cc_sent);
    }
}


static void
incr_sm_payload (struct lsquic_stream *stream, size_t incr)
{
    stream->sm_payload += incr;
    stream->tosend_off += incr;
    assert(stream->tosend_off <= stream->max_send_off);
}


static void
maybe_resize_threshold (struct frame_gen_ctx *fg_ctx)
{
    struct lsquic_stream *stream = fg_ctx->fgc_stream;
    size_t old;

    if (fg_ctx->fgc_thresh)
    {
        old = fg_ctx->fgc_thresh;
        fg_ctx->fgc_thresh
            = lsquic_stream_flush_threshold(stream, fg_ctx->fgc_size(fg_ctx));
        LSQ_DEBUG("changed threshold from %zd to %zd", old, fg_ctx->fgc_thresh);
    }
}


static size_t
frame_std_gen_read (void *ctx, void *begin_buf, size_t len, int *fin)
{
    struct frame_gen_ctx *fg_ctx = ctx;
    unsigned char *p = begin_buf;
    unsigned char *const end = p + len;
    lsquic_stream_t *const stream = fg_ctx->fgc_stream;
    size_t n_written, available, n_to_write;

    if (stream->sm_n_buffered > 0)
    {
        if (len <= stream->sm_n_buffered)
        {
            memcpy(p, stream->sm_buf, len);
            memmove(stream->sm_buf, stream->sm_buf + len,
                                                stream->sm_n_buffered - len);
            stream->sm_n_buffered -= len;
            if (0 == stream->sm_n_buffered)
            {
                maybe_resize_stream_buffer(stream);
                maybe_resize_threshold(fg_ctx);
            }
            assert(stream->max_send_off >= stream->tosend_off + stream->sm_n_buffered);
            incr_sm_payload(stream, len);
            *fin = fg_ctx->fgc_fin(fg_ctx);
            return len;
        }
        memcpy(p, stream->sm_buf, stream->sm_n_buffered);
        p += stream->sm_n_buffered;
        stream->sm_n_buffered = 0;
        maybe_resize_stream_buffer(stream);
        maybe_resize_threshold(fg_ctx);
    }

    available = lsquic_stream_write_avail(fg_ctx->fgc_stream);
    n_to_write = end - p;
    if (n_to_write > available)
        n_to_write = available;
    n_written = fg_ctx->fgc_reader->lsqr_read(fg_ctx->fgc_reader->lsqr_ctx, p,
                                              n_to_write);
    p += n_written;
    fg_ctx->fgc_nread_from_reader += n_written;
    *fin = fg_ctx->fgc_fin(fg_ctx);
    incr_sm_payload(stream, p - (const unsigned char *) begin_buf);
    incr_conn_cap(stream, n_written);
    return p - (const unsigned char *) begin_buf;
}


struct hq_arr
{
    unsigned char     **p;
    unsigned            count;
    unsigned            max;
};


static void
check_flush_threshold (lsquic_stream_t *stream)
{
    if ((stream->sm_qflags & SMQF_WANT_FLUSH) &&
                            stream->tosend_off >= stream->sm_flush_to)
    {
        LSQ_DEBUG("flushed to or past required offset %"PRIu64,
                                                    stream->sm_flush_to);
        maybe_remove_from_write_q(stream, SMQF_WANT_FLUSH);
    }
}


#if LSQUIC_EXTRA_CHECKS
static void
verify_conn_cap (const struct lsquic_conn_public *conn_pub)
{
    const struct lsquic_stream *stream;
    struct lsquic_hash_elem *el;
    unsigned n_buffered;

    if (conn_pub->wtp_level > 1)
        return;

    if (!conn_pub->all_streams)
        /* TODO: enable this check for unit tests as well */
        return;

    n_buffered = 0;
    for (el = lsquic_hash_first(conn_pub->all_streams); el;
                                 el = lsquic_hash_next(conn_pub->all_streams))
    {
        stream = lsquic_hashelem_getdata(el);
        if (stream->sm_bflags & SMBF_CONN_LIMITED)
            n_buffered += stream->sm_n_buffered;
    }

    assert(n_buffered + conn_pub->stream_frame_bytes
                                            == conn_pub->conn_cap.cc_sent);
    LSQ_DEBUG("%s: cc_sent: %"PRIu64, __func__, conn_pub->conn_cap.cc_sent);
}


#endif


static int
write_stream_frame (struct frame_gen_ctx *fg_ctx, const size_t size,
                                        struct lsquic_packet_out *packet_out)
{
    lsquic_stream_t *const stream = fg_ctx->fgc_stream;
    const struct parse_funcs *const pf = stream->conn_pub->lconn->cn_pf;
    struct lsquic_send_ctl *const send_ctl = stream->conn_pub->send_ctl;
    unsigned off;
    int len, s;

#if LSQUIC_CONN_STATS || LSQUIC_EXTRA_CHECKS
    const uint64_t begin_off = stream->tosend_off;
#endif
    off = packet_out->po_data_sz;
    len = pf->pf_gen_stream_frame(
                packet_out->po_data + packet_out->po_data_sz,
                lsquic_packet_out_avail(packet_out), stream->id,
                stream->tosend_off,
                fg_ctx->fgc_fin(fg_ctx), size, fg_ctx->fgc_read, fg_ctx);
    if (len <= 0)
        return len;

#if LSQUIC_CONN_STATS
    stream->conn_pub->conn_stats->out.stream_frames += 1;
    stream->conn_pub->conn_stats->out.stream_data_sz
                                            += stream->tosend_off - begin_off;
#endif
    EV_LOG_GENERATED_STREAM_FRAME(LSQUIC_LOG_CONN_ID, pf,
                            packet_out->po_data + packet_out->po_data_sz, len);
    lsquic_send_ctl_incr_pack_sz(send_ctl, packet_out, len);
    packet_out->po_frame_types |= 1 << QUIC_FRAME_STREAM;
    if (0 == lsquic_packet_out_avail(packet_out))
        packet_out->po_flags |= PO_STREAM_END;
    s = lsquic_packet_out_add_stream(packet_out, stream->conn_pub->mm,
                                     stream, QUIC_FRAME_STREAM, off, len);
    if (s != 0)
    {
        LSQ_ERROR("adding stream to packet failed: %s", strerror(errno));
        return -1;
    }
#if LSQUIC_EXTRA_CHECKS
    if (stream->sm_bflags & SMBF_CONN_LIMITED)
    {
        stream->conn_pub->stream_frame_bytes += stream->tosend_off - begin_off;
        verify_conn_cap(stream->conn_pub);
    }
#endif

    check_flush_threshold(stream);
    return len;
}


static enum swtp_status
stream_write_to_packet_hsk (struct frame_gen_ctx *fg_ctx, const size_t size)
{
    struct lsquic_stream *const stream = fg_ctx->fgc_stream;
    struct lsquic_send_ctl *const send_ctl = stream->conn_pub->send_ctl;
    struct lsquic_packet_out *packet_out;
    int len;

    packet_out = lsquic_send_ctl_new_packet_out(send_ctl, 0, PNS_APP,
                                                    stream->conn_pub->path);
    if (!packet_out)
        return SWTP_STOP;
    packet_out->po_header_type = stream->tosend_off == 0
                                        ? HETY_INITIAL : HETY_HANDSHAKE;

    len = write_stream_frame(fg_ctx, size, packet_out);

    if (len > 0)
    {
        packet_out->po_flags |= PO_HELLO;
        lsquic_packet_out_zero_pad(packet_out);
        lsquic_send_ctl_scheduled_one(send_ctl, packet_out);
        return SWTP_OK;
    }
    else
        return SWTP_ERROR;
}


static enum swtp_status
stream_write_to_packet_std (struct frame_gen_ctx *fg_ctx, const size_t size)
{
    struct lsquic_stream *const stream = fg_ctx->fgc_stream;
    struct lsquic_send_ctl *const send_ctl = stream->conn_pub->send_ctl;
    unsigned stream_header_sz, need_at_least;
    struct lsquic_packet_out *packet_out;
    struct lsquic_stream *headers_stream;
    int len;

    if ((stream->stream_flags & (STREAM_HEADERS_SENT|STREAM_HDRS_FLUSHED))
                                                        == STREAM_HEADERS_SENT)
    {
        if (stream->sm_bflags & SMBF_IETF)
            headers_stream = NULL;
        if (headers_stream && lsquic_stream_has_data_to_flush(headers_stream))
        {
            LSQ_DEBUG("flushing headers stream before packetizing stream data");
            (void) lsquic_stream_flush(headers_stream);
        }
        /* If there is nothing to flush, some other stream must have flushed it:
         * this means our headers are flushed.  Either way, only do this once.
         */
        stream->stream_flags |= STREAM_HDRS_FLUSHED;
    }

    stream_header_sz = stream->sm_frame_header_sz(stream, size);
    need_at_least = stream_header_sz;
    if ((stream->sm_bflags & (SMBF_IETF|SMBF_USE_HEADERS))
                                       == (SMBF_IETF|SMBF_USE_HEADERS))
    {
        if (size > 0)
            need_at_least += 3;     /* Enough room for HTTP/3 frame */
    }
    else
        need_at_least += size > 0;
  get_packet:
    packet_out = stream->sm_get_packet_for_stream(send_ctl,
                                need_at_least, stream->conn_pub->path, stream);
    if (packet_out)
    {
        len = write_stream_frame(fg_ctx, size, packet_out);
        if (len > 0)
            return SWTP_OK;
        if (len == 0)
            return SWTP_STOP;
        if (-len > (int) need_at_least)
        {
            LSQ_DEBUG("need more room (%d bytes) than initially calculated "
                "%u bytes, will try again", -len, need_at_least);
            need_at_least = -len;
            goto get_packet;
        }
        return SWTP_ERROR;
    }
    else
        return SWTP_STOP;
}


/* Use for IETF crypto streams and gQUIC crypto stream for versions >= Q050. */
static enum swtp_status
stream_write_to_packet_crypto (struct frame_gen_ctx *fg_ctx, const size_t size)
{
    struct lsquic_stream *const stream = fg_ctx->fgc_stream;
    struct lsquic_send_ctl *const send_ctl = stream->conn_pub->send_ctl;
    const struct parse_funcs *const pf = stream->conn_pub->lconn->cn_pf;
    unsigned crypto_header_sz, need_at_least;
    struct lsquic_packet_out *packet_out;
    unsigned short off;
    enum packnum_space pns;
    int len, s;

    if (stream->sm_bflags & SMBF_IETF)
        pns = lsquic_enclev2pns[ crypto_level(stream) ];
    else
        pns = PNS_APP;

    assert(size > 0);
    crypto_header_sz = stream->sm_frame_header_sz(stream, size);
    need_at_least = crypto_header_sz + 1;

    packet_out = lsquic_send_ctl_get_packet_for_crypto(send_ctl,
                                    need_at_least, pns, stream->conn_pub->path);
    if (!packet_out)
        return SWTP_STOP;

    off = packet_out->po_data_sz;
    len = pf->pf_gen_crypto_frame(packet_out->po_data + packet_out->po_data_sz,
                lsquic_packet_out_avail(packet_out), 0, stream->tosend_off, 0,
                size, frame_std_gen_read, fg_ctx);
    if (len < 0)
        return len;

    EV_LOG_GENERATED_CRYPTO_FRAME(LSQUIC_LOG_CONN_ID, pf,
                            packet_out->po_data + packet_out->po_data_sz, len);
    lsquic_send_ctl_incr_pack_sz(send_ctl, packet_out, len);
    packet_out->po_frame_types |= 1 << QUIC_FRAME_CRYPTO;
    s = lsquic_packet_out_add_stream(packet_out, stream->conn_pub->mm,
                                     stream, QUIC_FRAME_CRYPTO, off, len);
    if (s != 0)
    {
        LSQ_WARN("adding crypto stream to packet failed: %s", strerror(errno));
        return -1;
    }

    packet_out->po_flags |= PO_HELLO;

    if (!(stream->sm_bflags & SMBF_IETF))
    {
        const unsigned short before = packet_out->po_data_sz;
        lsquic_packet_out_zero_pad(packet_out);
        /* XXX: too hacky */
        if (before < packet_out->po_data_sz)
            send_ctl->sc_bytes_scheduled += packet_out->po_data_sz - before;
    }

    check_flush_threshold(stream);
    return SWTP_OK;
}


static void
abort_connection (struct lsquic_stream *stream)
{
    if (0 == (stream->sm_qflags & SMQF_SERVICE_FLAGS))
        TAILQ_INSERT_TAIL(&stream->conn_pub->service_streams, stream,
                                                next_service_stream);
    stream->sm_qflags |= SMQF_ABORT_CONN;
    LSQ_INFO("connection will be aborted");
    maybe_conn_to_tickable(stream);
}


static ssize_t
stream_write_to_packets (lsquic_stream_t *stream, struct lsquic_reader *reader,
                         size_t thresh, enum stream_write_options swo)
{
    size_t size;
    ssize_t nw;
    unsigned seen_ok;
    int use_framing;
    struct frame_gen_ctx fg_ctx = {
        .fgc_stream = stream,
        .fgc_reader = reader,
        .fgc_nread_from_reader = 0,
        .fgc_thresh = thresh,
    };

#if LSQUIC_EXTRA_CHECKS
    if (stream->conn_pub)
        ++stream->conn_pub->wtp_level;
#endif
    use_framing = (stream->sm_bflags & (SMBF_IETF|SMBF_USE_HEADERS))
                                       == (SMBF_IETF|SMBF_USE_HEADERS);
    if (!use_framing)
    {
        fg_ctx.fgc_size = frame_std_gen_size;
        fg_ctx.fgc_read = frame_std_gen_read;
        fg_ctx.fgc_fin = frame_std_gen_fin;
    }

    seen_ok = 0;
    while ((size = fg_ctx.fgc_size(&fg_ctx),
                            fg_ctx.fgc_thresh
                          ? size >= fg_ctx.fgc_thresh : size > 0)
           || fg_ctx.fgc_fin(&fg_ctx))
    {
        switch (stream->sm_write_to_packet(&fg_ctx, size))
        {
        case SWTP_OK:
            if (!seen_ok++)
            {
                maybe_conn_to_tickable_if_writeable(stream, 0);
                maybe_update_last_progress(stream);
            }
            if (fg_ctx.fgc_fin(&fg_ctx))
            {
                stream->stream_flags |= STREAM_FIN_SENT;
                if (stream->sm_qflags & SMQF_WANT_FLUSH)
                {
                    LSQ_DEBUG("turned off SMQF_WANT_FLUSH flag as FIN has been sent.");
                    maybe_remove_from_write_q(stream, SMQF_WANT_FLUSH);
                }
                goto end;
            }
            else
                break;
        case SWTP_STOP:
            stream->stream_flags &= ~STREAM_LAST_WRITE_OK;
            goto end;
        default:
            abort_connection(stream);
            stream->stream_flags &= ~STREAM_LAST_WRITE_OK;
            goto err;
        }
    }

    if (fg_ctx.fgc_thresh && (swo & SWO_BUFFER))
    {
        assert(size < fg_ctx.fgc_thresh);
        assert(size >= stream->sm_n_buffered);
        size -= stream->sm_n_buffered;
        if (size > 0)
        {
            nw = save_to_buffer(stream, reader, size);
            if (nw < 0)
                goto err;
            fg_ctx.fgc_nread_from_reader += nw; /* Make this cleaner? */
        }
    }
#ifndef NDEBUG
    else if (swo & SWO_BUFFER)
    {
        /* We count flushed data towards both stream and connection limits,
         * so we should have been able to packetize all of it:
         */
        assert(0 == stream->sm_n_buffered);
        assert(size == 0);
    }
#endif

    maybe_mark_as_blocked(stream);

  end:
#if LSQUIC_EXTRA_CHECKS
    if (stream->conn_pub)
        --stream->conn_pub->wtp_level;
#endif
    return fg_ctx.fgc_nread_from_reader;

  err:
#if LSQUIC_EXTRA_CHECKS
    if (stream->conn_pub)
        --stream->conn_pub->wtp_level;
#endif
    return -1;
}


/* Perform an implicit flush when we hit connection or stream flow control
 * limit while buffering data.
 *
 * This is to prevent a (theoretical) stall.  Scenario 1:
 *
 * Imagine a number of streams, all of which buffered some data.  The buffered
 * data is up to connection cap, which means no further writes are possible.
 * None of them flushes, which means that data is not sent and connection
 * WINDOW_UPDATE frame never arrives from peer.  Stall.
 *
 * Scenario 2:
 *
 * Stream flow control window is smaller than the packetizing threshold.  In
 * this case, without a flush, the peer will never send a WINDOW_UPDATE.  Stall.
 */
static int
maybe_flush_stream (struct lsquic_stream *stream)
{
    if (stream->sm_n_buffered > 0 && stream->sm_write_avail(stream) == 0)
    {
        LSQ_DEBUG("out of flow control credits, flush %zu buffered bytes",
            stream->sm_n_buffered + active_hq_frame_sizes(stream));
        return stream_flush_nocheck(stream);
    }
    else
        return 0;
}


static ssize_t
save_to_buffer (lsquic_stream_t *stream, struct lsquic_reader *reader,
                                                                size_t len)
{
    size_t avail, n_written, n_allowed;

    avail = lsquic_stream_write_avail(stream);
    if (avail < len)
        len = avail;
    if (len == 0)
    {
        LSQ_DEBUG("zero-byte write (avail: %zu)", avail);
        return 0;
    }

    n_allowed = stream_get_n_allowed(stream);
    assert(stream->sm_n_buffered + len <= n_allowed);

    if (!stream->sm_buf)
    {
        stream->sm_buf = malloc(n_allowed);
        if (!stream->sm_buf)
            return -1;
        stream->sm_n_allocated = n_allowed;
    }

    n_written = reader->lsqr_read(reader->lsqr_ctx,
                        stream->sm_buf + stream->sm_n_buffered, len);
    stream->sm_n_buffered += n_written;
    assert(stream->max_send_off >= stream->tosend_off + stream->sm_n_buffered);
    incr_conn_cap(stream, n_written);
    LSQ_DEBUG("buffered %zd bytes; %hu bytes are now in buffer",
              n_written, stream->sm_n_buffered);
    if (0 != maybe_flush_stream(stream))
        return -1;
    return n_written;
}


static ssize_t
stream_write (lsquic_stream_t *stream, struct lsquic_reader *reader,
                                                enum stream_write_options swo)
{
    size_t thresh, len, total_len, n_allowed, nwritten;
    ssize_t nw;

    len = reader->lsqr_size(reader->lsqr_ctx);
    if (len == 0)
        return 0;

    total_len = len + stream->sm_n_buffered;
    thresh = lsquic_stream_flush_threshold(stream, total_len);
    n_allowed = stream_get_n_allowed(stream);
    if (total_len <= n_allowed && total_len < thresh)
    {
        if (!(swo & SWO_BUFFER))
            return 0;
        nwritten = 0;
        do
        {
            nw = save_to_buffer(stream, reader, len - nwritten);
            if (nw > 0)
                nwritten += (size_t) nw;
            else if (nw == 0)
                break;
            else
                return nw;
        }
        while (nwritten < len
                        && stream->sm_n_buffered < stream->sm_n_allocated);
    }
    else
        nwritten = stream_write_to_packets(stream, reader, thresh, swo);
    if ((stream->sm_qflags & SMQF_SEND_BLOCKED) &&
        (stream->sm_bflags & SMBF_IETF))
    {
        lsquic_sendctl_gen_stream_blocked_frame(stream->conn_pub->send_ctl, stream);
    }
    return nwritten;
}


ssize_t
lsquic_stream_write (lsquic_stream_t *stream, const void *buf, size_t len)
{
    struct iovec iov = { .iov_base = (void *) buf, .iov_len = len, };
    return lsquic_stream_writev(stream, &iov, 1);
}


struct inner_reader_iovec {
    const struct iovec       *iov;
    const struct iovec *end;
    unsigned                  cur_iovec_off;
};


static size_t
inner_reader_iovec_read (void *ctx, void *buf, size_t count)
{
    struct inner_reader_iovec *const iro = ctx;
    unsigned char *p = buf;
    unsigned char *const end = p + count;
    unsigned n_tocopy;

    while (iro->iov < iro->end && p < end)
    {
        n_tocopy = iro->iov->iov_len - iro->cur_iovec_off;
        if (n_tocopy > (unsigned) (end - p))
            n_tocopy = end - p;
        memcpy(p, (unsigned char *) iro->iov->iov_base + iro->cur_iovec_off,
                                                                    n_tocopy);
        p += n_tocopy;
        iro->cur_iovec_off += n_tocopy;
        if (iro->iov->iov_len == iro->cur_iovec_off)
        {
            ++iro->iov;
            iro->cur_iovec_off = 0;
        }
    }

    return p + count - end;
}


static size_t
inner_reader_iovec_size (void *ctx)
{
    struct inner_reader_iovec *const iro = ctx;
    const struct iovec *iov;
    size_t size;

    size = 0;
    for (iov = iro->iov; iov < iro->end; ++iov)
        size += iov->iov_len;

    return size - iro->cur_iovec_off;
}


ssize_t
lsquic_stream_writev (lsquic_stream_t *stream, const struct iovec *iov,
                                                                    int iovcnt)
{
    COMMON_WRITE_CHECKS();
    SM_HISTORY_APPEND(stream, SHE_USER_WRITE_DATA);

    struct inner_reader_iovec iro = {
        .iov = iov,
        .end = iov + iovcnt,
        .cur_iovec_off = 0,
    };
    struct lsquic_reader reader = {
        .lsqr_read = inner_reader_iovec_read,
        .lsqr_size = inner_reader_iovec_size,
        .lsqr_ctx  = &iro,
    };

    return stream_write(stream, &reader, SWO_BUFFER);
}


ssize_t
lsquic_stream_writef (lsquic_stream_t *stream, struct lsquic_reader *reader)
{
    COMMON_WRITE_CHECKS();
    SM_HISTORY_APPEND(stream, SHE_USER_WRITE_DATA);
    return stream_write(stream, reader, SWO_BUFFER);
}


/* Configuration for lsquic_stream_pwritev: */
#ifndef LSQUIC_PWRITEV_DEF_IOVECS
#define LSQUIC_PWRITEV_DEF_IOVECS  16
#endif
/* This is an overkill, this limit should only be reached during testing: */
#ifndef LSQUIC_PWRITEV_DEF_FRAMES
#define LSQUIC_PWRITEV_DEF_FRAMES (LSQUIC_PWRITEV_DEF_IOVECS * 2)
#endif

#ifdef NDEBUG
#define PWRITEV_IOVECS  LSQUIC_PWRITEV_DEF_IOVECS
#define PWRITEV_FRAMES  LSQUIC_PWRITEV_DEF_FRAMES
#else
#if _MSC_VER
#define MALLOC_PWRITEV 1
#else
#define MALLOC_PWRITEV 0
#endif
static unsigned
    PWRITEV_IOVECS  = LSQUIC_PWRITEV_DEF_IOVECS,
    PWRITEV_FRAMES  = LSQUIC_PWRITEV_DEF_FRAMES;


#endif

struct pwritev_ctx
{
    struct iovec *iov;
    const struct hq_arr *hq_arr;
    size_t        total_bytes;
    size_t        n_to_write;
    unsigned      n_iovecs, max_iovecs;
};


static size_t
pwritev_size (void *lsqr_ctx)
{
    struct pwritev_ctx *const ctx = lsqr_ctx;

    if (ctx->n_iovecs < ctx->max_iovecs
                                    && ctx->hq_arr->count < ctx->hq_arr->max)
        return ctx->n_to_write - ctx->total_bytes;
    else
        return 0;
}


static size_t
pwritev_read (void *lsqr_ctx, void *buf, size_t count)
{
    struct pwritev_ctx *const ctx = lsqr_ctx;

    assert(ctx->n_iovecs < ctx->max_iovecs);
    ctx->iov[ctx->n_iovecs].iov_base = buf;
    ctx->iov[ctx->n_iovecs].iov_len = count;
    ++ctx->n_iovecs;
    ctx->total_bytes += count;
    return count;
}


/* pwritev works as follows: allocate packets via lsquic_stream_writef() call
 * and record pointers and sizes into an iovec array.  Then issue a single call
 * to user-supplied preadv() to populate all packets in one shot.
 *
 * Unwinding state changes due to a short write is by far the most complicated
 * part of the machinery that follows.  We optimize the normal path: it should
 * be cheap to be prepared for the unwinding; unwinding itself can be more
 * expensive, as we do not expect it to happen often.
 */
ssize_t
lsquic_stream_pwritev (struct lsquic_stream *stream,
    ssize_t (*preadv)(void *user_data, const struct iovec *iov, int iovcnt),
    void *user_data, size_t n_to_write)
{
    struct lsquic_send_ctl *const ctl = stream->conn_pub->send_ctl;
#if MALLOC_PWRITEV
    struct iovec *iovecs;
    unsigned char **hq_frames;
#else
    struct iovec iovecs[PWRITEV_IOVECS];
    unsigned char *hq_frames[PWRITEV_FRAMES];
#endif
    struct iovec *last_iov;
    struct pwritev_ctx ctx;
    struct lsquic_reader reader;
    struct send_ctl_state ctl_state;
    struct hq_arr hq_arr;
    ssize_t nw;
    size_t n_allocated, sum;
#ifndef NDEBUG
    const unsigned short n_buffered = stream->sm_n_buffered;
#endif

    COMMON_WRITE_CHECKS();
    SM_HISTORY_APPEND(stream, SHE_USER_WRITE_DATA);

#if MALLOC_PWRITEV
    iovecs = malloc(sizeof(iovecs[0]) * PWRITEV_IOVECS);
    hq_frames = malloc(sizeof(hq_frames[0]) * PWRITEV_FRAMES);
    if (!(iovecs && hq_frames))
    {
        free(iovecs);
        free(hq_frames);
        return -1;
    }
#endif

    lsquic_send_ctl_snapshot(ctl, &ctl_state);

    ctx.total_bytes = 0;
    ctx.n_to_write = n_to_write;
    ctx.n_iovecs = 0;
    ctx.max_iovecs = PWRITEV_IOVECS;
    ctx.iov = iovecs;
    ctx.hq_arr = &hq_arr;

    hq_arr.p = hq_frames;
    hq_arr.count = 0;
    hq_arr.max = PWRITEV_FRAMES;

    reader.lsqr_ctx = &ctx;
    reader.lsqr_size = pwritev_size;
    reader.lsqr_read = pwritev_read;

    nw = stream_write(stream, &reader, 0);
    LSQ_DEBUG("pwritev: stream_write returned %zd, n_iovecs: %d", nw,
                                                                ctx.n_iovecs);
    if (nw > 0)
    {
        /* Amount of buffered data shouldn't have increased */
        assert(n_buffered >= stream->sm_n_buffered);
        n_allocated = (size_t) nw;
        nw = preadv(user_data, ctx.iov, ctx.n_iovecs);
        LSQ_DEBUG("pwritev: preadv returned %zd", nw);
        if (nw >= 0 && (size_t) nw < n_allocated)
            goto unwind_short_write;
    }

  cleanup:
#if MALLOC_PWRITEV
    free(iovecs);
    free(hq_frames);
#endif
    return nw;

  unwind_short_write:
    /* What follows is not the most efficient process.  The emphasis here is
     * on being simple instead.  We expect short writes to be rare, so being
     * slower than possible is a good tradeoff for being correct.
     */
    LSQ_DEBUG("short write occurred, unwind");
    SM_HISTORY_APPEND(stream, SHE_SHORT_WRITE);

    /* First, adjust connection cap and stream offsets, and HTTP/3 framing,
     * if necessary.
     */
    assert((stream->sm_bflags & (SMBF_USE_HEADERS|SMBF_IETF))
                                            != (SMBF_USE_HEADERS|SMBF_IETF));
    const size_t shortfall = n_allocated - (size_t) nw;
    decr_conn_cap(stream, shortfall);
    stream->sm_payload -= shortfall;
    stream->tosend_off -= shortfall;

    /* Find last iovec: */
    sum = 0;
    for (last_iov = iovecs; last_iov < iovecs + PWRITEV_IOVECS; ++last_iov)
    {
        sum += last_iov->iov_len;
        if ((last_iov == iovecs || (size_t) nw > sum - last_iov->iov_len)
                                                        && (size_t) nw <= sum)
            break;
    }
    assert(last_iov < iovecs + PWRITEV_IOVECS);
    lsquic_send_ctl_rollback(ctl, &ctl_state, last_iov, sum - nw);

    goto cleanup;
}


void
lsquic_stream_window_update (lsquic_stream_t *stream, uint64_t offset)
{
    if (offset > stream->max_send_off)
    {
        SM_HISTORY_APPEND(stream, SHE_WINDOW_UPDATE);
        LSQ_DEBUG("update max send offset from %"PRIu64" to "
            "%"PRIu64, stream->max_send_off, offset);
        stream->max_send_off = offset;
    }
    else
        LSQ_DEBUG("new offset %"PRIu64" is not larger than old "
            "max send offset %"PRIu64", ignoring", offset,
            stream->max_send_off);
}


/* This function is used to update offsets after handshake completes and we
 * learn of peer's limits from the handshake values.
 */
int
lsquic_stream_set_max_send_off (lsquic_stream_t *stream, uint64_t offset)
{
    LSQ_DEBUG("setting max_send_off to %"PRIu64, offset);
    if (offset > stream->max_send_off)
    {
        lsquic_stream_window_update(stream, offset);
        return 0;
    }
    else if (offset < stream->tosend_off)
    {
        LSQ_INFO("new offset (%"PRIu64" bytes) is smaller than the amount of "
            "data already sent on this stream (%"PRIu64" bytes)", offset,
            stream->tosend_off);
        return -1;
    }
    else
    {
        stream->max_send_off = offset;
        return 0;
    }
}


void
lsquic_stream_maybe_reset (struct lsquic_stream *stream, uint64_t error_code,
                           int do_close)
{
    if (!((stream->stream_flags
            & (STREAM_RST_SENT|STREAM_FIN_SENT|STREAM_U_WRITE_DONE))
        || (stream->sm_qflags & SMQF_SEND_RST)))
    {
        stream_reset(stream, error_code, do_close);
    }
    else if (do_close)
        stream_shutdown_read(stream);
}


static void
stream_reset (struct lsquic_stream *stream, uint64_t error_code, int do_close)
{
    if ((stream->stream_flags & STREAM_RST_SENT)
                                    || (stream->sm_qflags & SMQF_SEND_RST))
    {
        LSQ_INFO("reset already sent");
        return;
    }

    SM_HISTORY_APPEND(stream, SHE_RESET);

    LSQ_INFO("reset, error code %"PRIu64, error_code);
    stream->error_code = error_code;

    if (!(stream->sm_qflags & SMQF_SENDING_FLAGS))
        TAILQ_INSERT_TAIL(&stream->conn_pub->sending_streams, stream,
                                                        next_send_stream);
    stream->sm_qflags &= ~SMQF_SENDING_FLAGS;
    stream->sm_qflags |= SMQF_SEND_RST;

    drop_buffered_data(stream);
    maybe_elide_stream_frames(stream);
    maybe_schedule_call_on_close(stream);

    if (do_close)
        lsquic_stream_close(stream);
    else
        maybe_conn_to_tickable_if_writeable(stream, 1);
}


lsquic_stream_id_t
lsquic_stream_id (const lsquic_stream_t *stream)
{
    return stream->id;
}


#if !defined(NDEBUG) && __GNUC__
__attribute__((weak))
#endif
struct lsquic_conn *
lsquic_stream_conn (const lsquic_stream_t *stream)
{
    return stream->conn_pub->lconn;
}


int
lsquic_stream_close (lsquic_stream_t *stream)
{
    LSQ_DEBUG("lsquic_stream_close() called");
    SM_HISTORY_APPEND(stream, SHE_CLOSE);
    if (lsquic_stream_is_closed(stream))
    {
        LSQ_INFO("Attempt to close an already-closed stream");
        errno = EBADF;
        return -1;
    }
    maybe_stream_shutdown_write(stream);
    stream_shutdown_read(stream);
    maybe_schedule_call_on_close(stream);
    maybe_finish_stream(stream);
    if (!(stream->stream_flags & STREAM_DELAYED_SW))
        maybe_conn_to_tickable_if_writeable(stream, 1);
    return 0;
}


#ifndef NDEBUG
#if __GNUC__
__attribute__((weak))
#endif
#endif
void
lsquic_stream_acked (struct lsquic_stream *stream,
                                            enum quic_frame_type frame_type)
{
    if (stream->n_unacked > 0)
    {
        --stream->n_unacked;
        LSQ_DEBUG("ACKed; n_unacked: %u", stream->n_unacked);
        if (frame_type == QUIC_FRAME_RST_STREAM)
        {
            SM_HISTORY_APPEND(stream, SHE_RST_ACKED);
            LSQ_DEBUG("RESET that we sent has been acked by peer");
            stream->stream_flags |= STREAM_RST_ACKED;
        }
    }
    if (0 == stream->n_unacked)
    {
        maybe_schedule_call_on_close(stream);
        maybe_finish_stream(stream);
    }
}


int
lsquic_stream_set_priority_internal (lsquic_stream_t *stream, unsigned priority)
{
    /* The user should never get a reference to the special streams,
     * but let's check just in case:
     */
    if (lsquic_stream_is_critical(stream))
        return -1;

    assert(!(stream->sm_bflags & SMBF_HTTP_PRIO));
    if (priority < 1 || priority > 256)
        return -1;
    stream->sm_priority = 256 - priority;

    lsquic_send_ctl_invalidate_bpt_cache(stream->conn_pub->send_ctl);
    LSQ_DEBUG("set priority to %u", priority);
    SM_HISTORY_APPEND(stream, SHE_SET_PRIO);
    return 0;
}


lsquic_stream_ctx_t *
lsquic_stream_get_ctx (const lsquic_stream_t *stream)
{
    fiu_return_on("stream/get_ctx", NULL);
    return stream->st_ctx;
}


void
lsquic_stream_set_ctx (lsquic_stream_t *stream, lsquic_stream_ctx_t *ctx)
{
    stream->st_ctx = ctx;
}


/* These are IETF QUIC states */
enum stream_state_sending
lsquic_stream_sending_state (const struct lsquic_stream *stream)
{
    if (0 == (stream->stream_flags & STREAM_RST_SENT))
    {
        if (stream->stream_flags & STREAM_FIN_SENT)
        {
            if (stream->n_unacked)
                return SSS_DATA_SENT;
            else
                return SSS_DATA_RECVD;
        }
        else
        {
            if (stream->tosend_off
                            || (stream->stream_flags & STREAM_BLOCKED_SENT))
                return SSS_SEND;
            else
                return SSS_READY;
        }
    }
    else if (stream->stream_flags & STREAM_RST_ACKED)
        return SSS_RESET_RECVD;
    else
        return SSS_RESET_SENT;
}


const char *const lsquic_sss2str[] =
{
    [SSS_READY]        =  "Ready",
    [SSS_SEND]         =  "Send",
    [SSS_DATA_SENT]    =  "Data Sent",
    [SSS_RESET_SENT]   =  "Reset Sent",
    [SSS_DATA_RECVD]   =  "Data Recvd",
    [SSS_RESET_RECVD]  =  "Reset Recvd",
};


int
lsquic_stream_has_unacked_data (struct lsquic_stream *stream)
{
    return stream->n_unacked > 0 || stream->sm_n_buffered > 0;
}
