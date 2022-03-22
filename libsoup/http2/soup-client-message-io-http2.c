/* soup-message-io-http2.c
 *
 * Copyright 2021 Igalia S.L.
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "libsoup-http2"

#include <glib.h>
#include <glib/gi18n-lib.h>

#include "soup-client-message-io-http2.h"

#include "soup-body-input-stream.h"
#include "soup-message-metrics-private.h"
#include "soup-message-headers-private.h"
#include "soup-message-private.h"
#include "soup-message-io-source.h"
#include "soup-message-queue-item.h"
#include "content-sniffer/soup-content-sniffer-stream.h"
#include "soup-client-input-stream.h"
#include "soup-logger-private.h"
#include "soup-uri-utils-private.h"

#include "content-decoder/soup-content-decoder.h"
#include "soup-body-input-stream-http2.h"

#include <nghttp2/nghttp2.h>

#define FRAME_HEADER_SIZE 9

typedef enum {
        STATE_NONE,
        STATE_WRITE_HEADERS,
        STATE_WRITE_DATA,
        STATE_WRITE_DONE,
        STATE_READ_HEADERS,
        STATE_READ_DATA_START,
        STATE_READ_DATA,
        STATE_READ_DONE,
} SoupHTTP2IOState;

typedef struct {
        SoupClientMessageIO iface;

        SoupConnection *conn;
        GIOStream *stream;
        GInputStream *istream;
        GOutputStream *ostream;
        guint64 connection_id;

        GError *error;
        GSource *read_source;
        GSource *write_source;

        GHashTable *messages;
        GHashTable *closed_messages;
        GList *pending_io_messages;

        nghttp2_session *session;

        /* Owned by nghttp2 */
        guint8 *write_buffer;
        gssize write_buffer_size;
        gssize written_bytes;

        gboolean is_shutdown;
        GTask *close_task;
        gboolean session_terminated;
        gboolean goaway_sent;

        guint in_callback;
} SoupClientMessageIOHTTP2;

typedef struct {
        SoupMessageQueueItem *item;
        SoupMessage *msg;
        SoupMessageMetrics *metrics;
        GInputStream *decoded_data_istream;
        GInputStream *body_istream;
        GTask *task;
        gboolean in_io_try_sniff_content;

        /* Request body logger */
        SoupLogger *logger;

        /* Pollable data sources */
        GSource *data_source_poll;

        /* Non-pollable data sources */
        GByteArray *data_source_buffer;
        GError *data_source_error;
        gboolean data_source_eof;

        SoupClientMessageIOHTTP2 *io; /* Unowned */
        SoupMessageIOCompletionFn completion_cb;
        gpointer completion_data;
        SoupHTTP2IOState state;
        GError *error;
        gboolean paused;
        guint32 stream_id;
        gboolean can_be_restarted;
        gboolean expect_continue;
} SoupHTTP2MessageData;

static void soup_client_message_io_http2_finished (SoupClientMessageIO *iface, SoupMessage *msg);
static ssize_t on_data_source_read_callback (nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);

static void
NGCHECK (int return_code)
{
        if (return_code == NGHTTP2_ERR_NOMEM)
                g_abort ();
        else if (return_code < 0)
                g_debug ("Unhandled NGHTTP2 Error: %s", nghttp2_strerror (return_code));
}

static const char *
frame_type_to_string (nghttp2_frame_type type)
{
        switch (type) {
        case NGHTTP2_DATA:
                return "DATA";
        case NGHTTP2_HEADERS:
                return "HEADERS";
        case NGHTTP2_PRIORITY:
                return "PRIORITY";
        case NGHTTP2_RST_STREAM:
                return "RST_STREAM";
        case NGHTTP2_SETTINGS:
                return "SETTINGS";
        case NGHTTP2_PING:
                return "PING";
        case NGHTTP2_GOAWAY:
                return "GOAWAY";
        case NGHTTP2_WINDOW_UPDATE:
                return "WINDOW_UPDATE";
        /* LCOV_EXCL_START */
        case NGHTTP2_PUSH_PROMISE:
                return "PUSH_PROMISE";
        case NGHTTP2_CONTINUATION:
                return "CONTINUATION";
        case NGHTTP2_ALTSVC:
                return "ALTSVC";
        case NGHTTP2_ORIGIN:
                return "ORIGIN";
        default:
                g_warn_if_reached ();
                return "UNKNOWN";
        /* LCOV_EXCL_STOP */
        }
}

static const char *
headers_category_to_string (nghttp2_headers_category catergory)
{
        switch (catergory) {
        case NGHTTP2_HCAT_REQUEST:
                return "REQUEST";
        case NGHTTP2_HCAT_RESPONSE:
                return "RESPONSE";
        case NGHTTP2_HCAT_PUSH_RESPONSE:
                return "PUSH_RESPONSE";
        case NGHTTP2_HCAT_HEADERS:
                return "HEADERS";
        }
        g_assert_not_reached ();
}

static const char *
state_to_string (SoupHTTP2IOState state)
{
        switch (state) {
        case STATE_NONE:
                return "NONE";
        case STATE_WRITE_HEADERS:
                return "WRITE_HEADERS";
        case STATE_WRITE_DATA:
                return "WRITE_DATA";
        case STATE_WRITE_DONE:
                return "WRITE_DONE";
        case STATE_READ_HEADERS:
                return "READ_HEADERS";
        case STATE_READ_DATA_START:
                return "READ_DATA_START";
        case STATE_READ_DATA:
                return "READ_DATA";
        case STATE_READ_DONE:
                return "READ_DONE";
        default:
                g_assert_not_reached ();
                return "";
        }
}

G_GNUC_PRINTF(3, 0)
static void
h2_debug (SoupClientMessageIOHTTP2   *io,
          SoupHTTP2MessageData       *data,
          const char                 *format,
          ...)
{
        va_list args;
        char *message;
        guint32 stream_id = 0;

        if (g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN))
                return;

	va_start (args, format);
	message = g_strdup_vprintf (format, args);
	va_end (args);

        if (data)
                stream_id = data->stream_id;

        g_assert (io);
        g_log (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "[C%" G_GUINT64_FORMAT "-S%u] [%s] %s", io->connection_id, stream_id, data ? state_to_string (data->state) : "-", message);

        g_free (message);
}

static SoupClientMessageIOHTTP2 *
get_io_data (SoupMessage *msg)
{
        return (SoupClientMessageIOHTTP2 *)soup_message_get_io_data (msg);
}

static int
get_data_io_priority (SoupHTTP2MessageData *data)
{
	if (!data->item->task)
		return G_PRIORITY_DEFAULT;

	return g_task_get_priority (data->item->task);
}

static void
set_error_for_data (SoupHTTP2MessageData *data,
                    GError               *error)
{
        h2_debug (data->io, data, "[SESSION] Error: %s", error->message);

        /* First error is probably the one we want. */
        if (!data->error)
                data->error = error;
        else
                g_error_free (error);
}

static void
set_io_error (SoupClientMessageIOHTTP2 *io,
              GError                   *error)
{
        h2_debug (io, NULL, "[SESSION] IO error: %s", error->message);

        if (!io->error)
                io->error = error;
        else
                g_error_free (error);
}

static void
advance_state_from (SoupHTTP2MessageData *data,
                    SoupHTTP2IOState      from,
                    SoupHTTP2IOState      to)
{
        if (data->state != from) {
                g_warning ("Unexpected state changed %s -> %s, expected to be from %s",
                           state_to_string (data->state), state_to_string (to),
                           state_to_string (from));
        }

        /* State never goes backwards */
        if (to < data->state) {
                g_warning ("Unexpected state changed %s -> %s, expected %s -> %s\n",
                           state_to_string (data->state), state_to_string (to),
                           state_to_string (from), state_to_string (to));
                return;
        }

        h2_debug (data->io, data, "[SESSION] State %s -> %s",
                  state_to_string (data->state), state_to_string (to));
        data->state = to;
}

static void
soup_http2_message_data_check_status (SoupHTTP2MessageData *data)
{
        SoupClientMessageIOHTTP2 *io = data->io;
        SoupMessage *msg = data->msg;
        GTask *task = data->task;
        GError *error = NULL;

        if (g_cancellable_set_error_if_cancelled (g_task_get_cancellable (task), &error)) {
                io->pending_io_messages = g_list_remove (io->pending_io_messages, data);
                data->task = NULL;
                g_task_return_error (task, error);
                g_object_unref (task);
                return;
        }

        if (data->paused)
                return;

        if (io->error && !data->error)
                data->error = g_error_copy (io->error);

        if (data->error) {
                GError *error = g_steal_pointer (&data->error);

                if (data->can_be_restarted)
                        data->item->state = SOUP_MESSAGE_RESTARTING;
                else
                        soup_message_set_metrics_timestamp (data->msg, SOUP_MESSAGE_METRICS_RESPONSE_END);
                io->pending_io_messages = g_list_remove (io->pending_io_messages, data);
                data->task = NULL;
                soup_client_message_io_http2_finished ((SoupClientMessageIO *)io, msg);

                g_task_return_error (task, error);
                g_object_unref (task);
                return;
        }

        if (data->state == STATE_READ_DATA_START && !soup_message_has_content_sniffer (msg))
                advance_state_from (data, STATE_READ_DATA_START, STATE_READ_DATA);

        if (data->state < STATE_READ_DATA)
                return;

        io->pending_io_messages = g_list_remove (io->pending_io_messages, data);
        data->task = NULL;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
}

static gboolean
io_write (SoupClientMessageIOHTTP2 *io,
          gboolean                  blocking,
          GCancellable             *cancellable,
          GError                  **error)
{
        /* We must write all of nghttp2's buffer before we ask for more */
        if (io->written_bytes == io->write_buffer_size)
                io->write_buffer = NULL;

        if (io->write_buffer == NULL) {
                io->written_bytes = 0;
                g_assert (io->in_callback == 0);
                io->write_buffer_size = nghttp2_session_mem_send (io->session, (const guint8**)&io->write_buffer);
                NGCHECK (io->write_buffer_size);
                if (io->write_buffer_size == 0) {
                        /* Done */
                        io->write_buffer = NULL;
                        return TRUE;
                }
        }

        gssize ret = g_pollable_stream_write (io->ostream,
                                              io->write_buffer + io->written_bytes,
                                              io->write_buffer_size - io->written_bytes,
                                              blocking, cancellable, error);
        if (ret < 0)
                return FALSE;

        io->written_bytes += ret;
        return TRUE;
}

static gboolean
io_write_ready (GObject                  *stream,
                SoupClientMessageIOHTTP2 *io)
{
        GError *error = NULL;

        if (io->error) {
                g_clear_pointer (&io->write_source, g_source_unref);
                return G_SOURCE_REMOVE;
        }

        while (nghttp2_session_want_write (io->session) && !error)
                io_write (io, FALSE, NULL, &error);

        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                g_error_free (error);
                return G_SOURCE_CONTINUE;
        }

        if (error)
                set_io_error (io, error);

        g_clear_pointer (&io->write_source, g_source_unref);
        return G_SOURCE_REMOVE;
}

static void
io_try_write (SoupClientMessageIOHTTP2 *io,
              gboolean                  blocking)
{
        GError *error = NULL;

        if (io->write_source)
                return;

        if (io->in_callback) {
                if (blocking || !nghttp2_session_want_write (io->session))
                        return;
        } else {
                while (nghttp2_session_want_write (io->session) && !error)
                        io_write (io, blocking, NULL, &error);
        }

        if (io->in_callback || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                g_clear_error (&error);
                io->write_source = g_pollable_output_stream_create_source (G_POLLABLE_OUTPUT_STREAM (io->ostream), NULL);
                g_source_set_name (io->write_source, "Soup HTTP/2 write source");
                g_source_set_callback (io->write_source, (GSourceFunc)io_write_ready, io, NULL);
                g_source_attach (io->write_source, g_main_context_get_thread_default ());
        }

        if (error)
                set_io_error (io, error);
}

static gboolean
io_read (SoupClientMessageIOHTTP2  *io,
         gboolean                   blocking,
         GCancellable              *cancellable,
         GError                   **error)
{
        guint8 buffer[8192];
        gssize read;
        int ret;

        if ((read = g_pollable_stream_read (io->istream, buffer, sizeof (buffer),
                                            blocking, cancellable, error)) < 0)
            return FALSE;

        g_assert (io->in_callback == 0);
        ret = nghttp2_session_mem_recv (io->session, buffer, read);
        NGCHECK (ret);
        return ret != 0;
}

static gboolean
io_read_ready (GObject                  *stream,
               SoupClientMessageIOHTTP2 *io)
{
        GError *error = NULL;
        gboolean progress = TRUE;

        if (io->error) {
                g_clear_pointer (&io->read_source, g_source_unref);
                return G_SOURCE_REMOVE;
        }

        /* Mark the connection as in use to make sure it's not disconnected while
         * processing pending messages, for example if a goaway is received.
         */
        if (io->conn)
                soup_connection_set_in_use (io->conn, TRUE);

        while (nghttp2_session_want_read (io->session) && progress) {
                progress = io_read (io, FALSE, NULL, &error);
                if (progress) {
                        g_list_foreach (io->pending_io_messages,
                                        (GFunc)soup_http2_message_data_check_status,
                                        NULL);
                }
        }

        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                g_error_free (error);
                if (io->conn)
                        soup_connection_set_in_use (io->conn, FALSE);
                return G_SOURCE_CONTINUE;
        }

        if (error) {
                set_io_error (io, error);
                g_list_foreach (io->pending_io_messages,
                                (GFunc)soup_http2_message_data_check_status,
                                NULL);
        }

        io->is_shutdown = TRUE;

        g_clear_pointer (&io->read_source, g_source_unref);
        if (io->conn)
                soup_connection_set_in_use (io->conn, FALSE);
        return G_SOURCE_REMOVE;
}

static void
io_try_sniff_content (SoupHTTP2MessageData *data,
                      gboolean              blocking,
                      GCancellable         *cancellable)
{
        GError *error = NULL;

        /* This can re-enter in sync mode */
        if (data->in_io_try_sniff_content)
                return;

        data->in_io_try_sniff_content = TRUE;

        if (soup_message_try_sniff_content (data->msg, data->decoded_data_istream, blocking, cancellable, &error)) {
                h2_debug (data->io, data, "[DATA] Sniffed content");
                advance_state_from (data, STATE_READ_DATA_START, STATE_READ_DATA);
        } else {
                h2_debug (data->io, data, "[DATA] Sniffer stream was not ready %s", error->message);

                g_clear_error (&error);
        }

        data->in_io_try_sniff_content = FALSE;
}

static void
soup_client_message_io_http2_terminate_session (SoupClientMessageIOHTTP2 *io)
{
        if (io->session_terminated)
                return;

        if (g_hash_table_size (io->messages) != 0)
                return;

        io->session_terminated = TRUE;
        NGCHECK (nghttp2_session_terminate_session (io->session, NGHTTP2_NO_ERROR));
        io_try_write (io, FALSE);
}

/* HTTP2 read callbacks */

static int
on_header_callback (nghttp2_session     *session,
                    const nghttp2_frame *frame,
                    const uint8_t       *name,
                    size_t               namelen,
                    const uint8_t       *value,
                    size_t               valuelen,
                    uint8_t              flags,
                    void                *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        if (!data)
                return 0;

        data->io->in_callback++;

        SoupMessage *msg = data->msg;
        if (name[0] == ':') {
                if (strcmp ((char *)name, ":status") == 0) {
                        guint status_code = (guint)g_ascii_strtoull ((char *)value, NULL, 10);
                        soup_message_set_status (msg, status_code, NULL);
                        data->io->in_callback--;
                        return 0;
                }
                g_debug ("Unknown header: %s = %s", name, value);
                data->io->in_callback--;
                return 0;
        }

        soup_message_headers_append_untrusted_data (soup_message_get_response_headers (data->msg),
                                                    (const char*)name, (const char*)value);
        data->io->in_callback--;
        return 0;
}

static GError *
memory_stream_need_more_data_callback (SoupBodyInputStreamHttp2 *stream,
                                       GCancellable             *cancellable,
                                       gpointer                  user_data)
{
        SoupHTTP2MessageData *data = (SoupHTTP2MessageData*)user_data;
        GError *error = NULL;

        if (nghttp2_session_want_read (data->io->session))
                io_read (data->io, TRUE, cancellable, &error);

        return error;
}

static int
on_begin_frame_callback (nghttp2_session        *session,
                         const nghttp2_frame_hd *hd,
                         void                   *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, hd->stream_id);

        h2_debug (user_data, data, "[RECV] [%s] Beginning", frame_type_to_string (hd->type));

        if (!data)
                return 0;

        data->io->in_callback++;

        switch (hd->type) {
        case NGHTTP2_HEADERS:
                if (data->state == STATE_WRITE_DONE) {
                        soup_message_set_metrics_timestamp (data->item->msg, SOUP_MESSAGE_METRICS_RESPONSE_START);
                        advance_state_from (data, STATE_WRITE_DONE, STATE_READ_HEADERS);
                }
                break;
        case NGHTTP2_DATA:
                if (data->state < STATE_READ_DATA_START) {
                        g_assert (!data->body_istream);
                        data->body_istream = soup_body_input_stream_http2_new ();
                        g_signal_connect (data->body_istream, "need-more-data",
                                          G_CALLBACK (memory_stream_need_more_data_callback), data);

                        g_assert (!data->decoded_data_istream);
                        data->decoded_data_istream = soup_session_setup_message_body_input_stream (data->item->session,
                                                                                                   data->msg,
                                                                                                   data->body_istream,
                                                                                                   SOUP_STAGE_MESSAGE_BODY);

                        advance_state_from (data, STATE_READ_HEADERS, STATE_READ_DATA_START);
                }
                break;
        }

        data->io->in_callback--;
        return 0;
}

static void
handle_goaway (SoupClientMessageIOHTTP2 *io,
               guint32                   error_code,
               int32_t                   last_stream_id)
{
        GHashTableIter iter;
        SoupHTTP2MessageData *data;

        g_hash_table_iter_init (&iter, io->messages);
        while (g_hash_table_iter_next (&iter, NULL, (gpointer*)&data)) {
                /* If there is no error it is a graceful shutdown and
                 * existing messages can be handled otherwise it is a fatal error */
                if ((error_code == 0 && (int32_t)data->stream_id > last_stream_id) ||
                     data->state < STATE_READ_DONE) {
                        /* TODO: We can restart unfinished messages */
                        set_error_for_data (data, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                            "HTTP/2 Error: %s", nghttp2_http2_strerror (error_code)));
                }
        }
}

static int
on_frame_recv_callback (nghttp2_session     *session,
                        const nghttp2_frame *frame,
                        gpointer             user_data)
{
        SoupClientMessageIOHTTP2 *io = user_data;
        SoupHTTP2MessageData *data;

        io->in_callback++;

        if (frame->hd.stream_id == 0) {
                h2_debug (io, NULL, "[RECV] [%s] Recieved (%u)", frame_type_to_string (frame->hd.type), frame->hd.flags);

                switch (frame->hd.type) {
                case NGHTTP2_GOAWAY:
                        h2_debug (io, NULL, "[RECV] GOAWAY: error=%s, last_stream_id=%d %s",
                                  nghttp2_http2_strerror (frame->goaway.error_code),
                                  frame->goaway.last_stream_id,
                                  frame->goaway.opaque_data ? (char *)frame->goaway.opaque_data : "");
                        handle_goaway (io, frame->goaway.error_code, frame->goaway.last_stream_id);
                        io->is_shutdown = TRUE;
                        soup_client_message_io_http2_terminate_session (io);
                        break;
                case NGHTTP2_WINDOW_UPDATE:
                        h2_debug (io, NULL, "[RECV] WINDOW_UPDATE: increment=%d, total=%d", frame->window_update.window_size_increment,
                                  nghttp2_session_get_remote_window_size (session));
                        break;
                }

                io->in_callback--;
                return 0;
        }

        data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);
        h2_debug (io, data, "[RECV] [%s] Recieved (%u)", frame_type_to_string (frame->hd.type), frame->hd.flags);

        if (!data) {
                if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM) && frame->hd.type != NGHTTP2_RST_STREAM)
                        g_warn_if_reached ();
                io->in_callback--;
                return 0;
        }

        switch (frame->hd.type) {
        case NGHTTP2_HEADERS: {
                guint status = soup_message_get_status (data->msg);

                if (data->metrics)
                        data->metrics->response_header_bytes_received += frame->hd.length + FRAME_HEADER_SIZE;

                h2_debug (io, data, "[HEADERS] category=%s status=%u",
                          headers_category_to_string (frame->headers.cat), status);
                switch (frame->headers.cat) {
                case NGHTTP2_HCAT_HEADERS:
                        if (!(frame->hd.flags & NGHTTP2_FLAG_END_HEADERS)) {
                                io->in_callback--;
                                return 0;
                        }
                        break;
                case NGHTTP2_HCAT_RESPONSE:
                        if (SOUP_STATUS_IS_INFORMATIONAL (status)) {
                                if (data->expect_continue && status == SOUP_STATUS_CONTINUE) {
                                        nghttp2_data_provider data_provider;

                                        data_provider.source.ptr = soup_message_get_request_body_stream (data->msg);
                                        data_provider.read_callback = on_data_source_read_callback;
                                        nghttp2_submit_data (io->session, NGHTTP2_FLAG_END_STREAM, frame->hd.stream_id, &data_provider);
                                        io_try_write (io, !data->item->async);
                                }

                                soup_message_got_informational (data->msg);
                                soup_message_cleanup_response (data->msg);
                                io->in_callback--;
                                return 0;
                        }
                        break;
                case NGHTTP2_HCAT_PUSH_RESPONSE:
                        g_warn_if_reached ();
                        break;
                default:
                        g_assert_not_reached ();
                }

                if (soup_message_get_status (data->msg) == SOUP_STATUS_NO_CONTENT || frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                        h2_debug (io, data, "Stream done");
                        advance_state_from (data, STATE_READ_HEADERS, STATE_READ_DATA);
                }
                soup_message_got_headers (data->msg);

                break;
        }
        case NGHTTP2_DATA:
                if (data->metrics)
                        data->metrics->response_body_bytes_received += frame->data.hd.length + FRAME_HEADER_SIZE;
                if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM && data->body_istream) {
                        soup_body_input_stream_http2_complete (SOUP_BODY_INPUT_STREAM_HTTP2 (data->body_istream));
                        if (data->state == STATE_READ_DATA_START) {
                                io_try_sniff_content (data, FALSE, data->item->cancellable);
                                if (data->state == STATE_READ_DATA && data->item->async)
                                        soup_http2_message_data_check_status (data);
                        }
                }
                /* Try to write after every data frame, since nghttp2 might need to send a window update. */
                io_try_write (io, !data->item->async);
                break;
        case NGHTTP2_RST_STREAM:
                if (frame->rst_stream.error_code != NGHTTP2_NO_ERROR) {
                        set_error_for_data (data, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                                       nghttp2_http2_strerror (frame->rst_stream.error_code)));
                }
                break;
        case NGHTTP2_WINDOW_UPDATE:
                h2_debug (io, data, "[RECV] WINDOW_UPDATE: increment=%d, total=%d", frame->window_update.window_size_increment,
                          nghttp2_session_get_stream_remote_window_size (session, frame->hd.stream_id));
                if (nghttp2_session_get_stream_remote_window_size (session, frame->hd.stream_id) > 0)
                        io_try_write (io, !data->item->async);
                break;
        };

        io->in_callback--;
        return 0;
}

static int
on_data_chunk_recv_callback (nghttp2_session *session,
                             uint8_t          flags,
                             int32_t          stream_id,
                             const uint8_t   *data,
                             size_t           len,
                             void            *user_data)
{
        SoupClientMessageIOHTTP2 *io = user_data;
        SoupHTTP2MessageData *msgdata = nghttp2_session_get_stream_user_data (session, stream_id);

        if (!msgdata)
                return NGHTTP2_ERR_CALLBACK_FAILURE;

        io->in_callback++;

        h2_debug (io, msgdata, "[DATA] Recieved chunk, len=%zu, flags=%u, paused=%d", len, flags, msgdata->paused);

        g_assert (msgdata->body_istream != NULL);
        soup_body_input_stream_http2_add_data (SOUP_BODY_INPUT_STREAM_HTTP2 (msgdata->body_istream), data, len);
        if (msgdata->state == STATE_READ_DATA_START)
                io_try_sniff_content (msgdata, FALSE, msgdata->item->cancellable);

        io->in_callback--;
        return 0;
}

/* HTTP2 write callbacks */

static int
on_before_frame_send_callback (nghttp2_session     *session,
                               const nghttp2_frame *frame,
                               void                *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        if (!data)
                return 0;

        data->io->in_callback++;

        switch (frame->hd.type) {
        case NGHTTP2_HEADERS:
                advance_state_from (data, STATE_NONE, STATE_WRITE_HEADERS);
                break;
        }

        data->io->in_callback--;
        return 0;
}

static gboolean
remove_closed_stream (SoupHTTP2MessageData *data,
                      gpointer              value,
                      nghttp2_frame        *frame)
{
        return data->stream_id == frame->hd.stream_id;
}

static gboolean
close_in_idle_cb (SoupClientMessageIOHTTP2 *io)
{
        g_task_return_boolean (io->close_task, TRUE);
        g_clear_object (&io->close_task);

        return G_SOURCE_REMOVE;
}

static int
on_frame_send_callback (nghttp2_session     *session,
                        const nghttp2_frame *frame,
                        void                *user_data)
{
        SoupClientMessageIOHTTP2 *io = user_data;
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        io->in_callback++;

        switch (frame->hd.type) {
        case NGHTTP2_HEADERS:
                g_assert (data);
                h2_debug (io, data, "[SEND] [HEADERS] category=%s finished=%d",
                          headers_category_to_string (frame->headers.cat),
                          (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) ? 1 : 0);

                if (data->metrics)
                        data->metrics->request_header_bytes_sent += frame->hd.length + FRAME_HEADER_SIZE;

                if (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {
                        soup_message_wrote_headers (data->msg);
                        if (soup_message_get_request_body_stream (data->msg) == NULL) {
                                advance_state_from (data, STATE_WRITE_HEADERS, STATE_WRITE_DONE);
                                soup_message_wrote_body (data->msg);
                        }
                }
                break;
        case NGHTTP2_DATA:
                g_assert (data);
                if (data->state < STATE_WRITE_DATA)
                        advance_state_from (data, STATE_WRITE_HEADERS, STATE_WRITE_DATA);

                h2_debug (io, data, "[SEND] [DATA] bytes=%zu, finished=%d",
                          frame->data.hd.length, frame->hd.flags & NGHTTP2_FLAG_END_STREAM);
                if (data->metrics) {
                        data->metrics->request_body_bytes_sent += frame->hd.length + FRAME_HEADER_SIZE;
                        data->metrics->request_body_size += frame->data.hd.length;
                }
                if (frame->data.hd.length)
                        soup_message_wrote_body_data (data->msg, frame->data.hd.length);
                if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                        advance_state_from (data, STATE_WRITE_DATA, STATE_WRITE_DONE);
                        soup_message_wrote_body (data->msg);
                }
                break;
        case NGHTTP2_RST_STREAM:
                h2_debug (io, data, "[SEND] [RST_STREAM] stream_id=%u", frame->hd.stream_id);
                g_hash_table_foreach_remove (io->closed_messages, (GHRFunc)remove_closed_stream, (gpointer)frame);
                break;
        case NGHTTP2_GOAWAY:
                h2_debug (io, data, "[SEND] [%s]", frame_type_to_string (frame->hd.type));
                io->goaway_sent = TRUE;
                if (io->close_task) {
                        GSource *source;

                        /* Close in idle to ensure all pending io is finished first */
                        source = g_idle_source_new ();
                        g_source_set_name (source, "Soup HTTP/2 close source");
                        g_source_set_callback (source, (GSourceFunc)close_in_idle_cb, io, NULL);
                        g_source_attach (source, g_main_context_get_thread_default ());
                        g_source_unref (source);
                }
                break;
        default:
                h2_debug (io, data, "[SEND] [%s]", frame_type_to_string (frame->hd.type));
                break;
        }

        io->in_callback--;
        return 0;
}

static int
on_frame_not_send_callback (nghttp2_session     *session,
                            const nghttp2_frame *frame,
                            int                  lib_error_code,
                            void                *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        h2_debug (user_data, data, "[SEND] [%s] Failed: %s", frame_type_to_string (frame->hd.type),
                  nghttp2_strerror (lib_error_code));

        return 0;
}

static int
on_stream_close_callback (nghttp2_session *session,
                          int32_t          stream_id,
                          uint32_t         error_code,
                          void            *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, stream_id);

        h2_debug (user_data, data, "[SESSION] Closed: %s", nghttp2_http2_strerror (error_code));
        if (!data)
                return 0;

        data->io->in_callback++;

        if (error_code == NGHTTP2_REFUSED_STREAM && data->state < STATE_READ_DATA)
                data->can_be_restarted = TRUE;

        data->io->in_callback--;
        return 0;
}

static gboolean
on_data_readable (GInputStream *stream,
                  gpointer      user_data)
{
        SoupHTTP2MessageData *data = (SoupHTTP2MessageData*)user_data;

        h2_debug (data->io, data, "on data readable");

        NGCHECK (nghttp2_session_resume_data (data->io->session, data->stream_id));
        io_try_write (data->io, !data->item->async);

        g_clear_pointer (&data->data_source_poll, g_source_unref);
        return G_SOURCE_REMOVE;
}

static void
on_data_read (GInputStream *source,
              GAsyncResult *res,
              gpointer      user_data)
{
        SoupHTTP2MessageData *data = user_data;
        GError *error = NULL;
        gssize read = g_input_stream_read_finish (source, res, &error);

        h2_debug (data->io, data, "[SEND_BODY] Read %zd", read);

        /* This operation may have outlived the message data in which
           case this will have been cancelled. */
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_error_free (error);
                return;
        }

        if (read < 0) {
                g_byte_array_set_size (data->data_source_buffer, 0);
                data->data_source_error = g_steal_pointer (&error);
        } else if (read == 0) {
                g_byte_array_set_size (data->data_source_buffer, 0);
                data->data_source_eof = TRUE;
        } else
                g_byte_array_set_size (data->data_source_buffer, read);

        h2_debug (data->io, data, "[SEND_BODY] Resuming send");
        NGCHECK (nghttp2_session_resume_data (data->io->session, data->stream_id));
        io_try_write (data->io, !data->item->async);
}

static void
log_request_data (SoupHTTP2MessageData *data,
                  const guint8         *buffer,
                  gsize                 len)
{
        if (!data->logger)
                return;

        /* NOTE: This doesn't exactly log data as it hits the network but
           rather as soon as we read it from our source which is as good
           as we can do since nghttp handles the actual io. */
        soup_logger_log_request_data (data->logger, data->msg, (const char *)buffer, len);
}

static ssize_t
on_data_source_read_callback (nghttp2_session     *session,
                              int32_t              stream_id,
                              uint8_t             *buf,
                              size_t               length,
                              uint32_t            *data_flags,
                              nghttp2_data_source *source,
                              void                *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, stream_id);
        data->io->in_callback++;

        /* We support pollable streams in the best case because they
         * should perform better with one fewer copy of each buffer and no threading. */
        if (G_IS_POLLABLE_INPUT_STREAM (source->ptr) && g_pollable_input_stream_can_poll (G_POLLABLE_INPUT_STREAM (source->ptr))) {
                GPollableInputStream *in_stream = G_POLLABLE_INPUT_STREAM (source->ptr);
                GError *error = NULL;

                gssize read = g_pollable_input_stream_read_nonblocking  (in_stream, buf, length, data->item->cancellable, &error);

                if (read) {
                        h2_debug (data->io, data, "[SEND_BODY] Read %zd", read);
                        log_request_data (data, buf, read);
                }

                if (read < 0) {
                        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                                g_assert (data->data_source_poll == NULL);

                                h2_debug (data->io, data, "[SEND_BODY] Polling");
                                data->data_source_poll = g_pollable_input_stream_create_source (in_stream, data->item->cancellable);
                                g_source_set_callback (data->data_source_poll, (GSourceFunc)on_data_readable, data, NULL);
                                g_source_set_priority (data->data_source_poll, get_data_io_priority (data));
                                g_source_attach (data->data_source_poll, g_main_context_get_thread_default ());

                                g_error_free (error);
                                data->io->in_callback--;
                                return NGHTTP2_ERR_DEFERRED;
                        }

                        set_error_for_data (data, g_steal_pointer (&error));
                        data->io->in_callback--;
                        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                }
                else if (read == 0) {
                        h2_debug (data->io, data, "[SEND_BODY] EOF");
                        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                }

                data->io->in_callback--;
                return read;
        } else {
                GInputStream *in_stream = G_INPUT_STREAM (source->ptr);

                /* To support non-pollable input streams we always deffer reads
                * and read async into a local buffer. The next time around we will
                * send that buffer or error.
                */
                if (!data->data_source_buffer)
                        data->data_source_buffer = g_byte_array_new ();

                guint buffer_len = data->data_source_buffer->len;
                if (buffer_len) {
                        h2_debug (data->io, data, "[SEND_BODY] Sending %zu", buffer_len);
                        g_assert (buffer_len <= length); /* QUESTION: Maybe not reliable */
                        memcpy (buf, data->data_source_buffer->data, buffer_len);
                        log_request_data (data, buf, buffer_len);
                        g_byte_array_set_size (data->data_source_buffer, 0);
                        data->io->in_callback--;
                        return buffer_len;
                } else if (data->data_source_eof) {
                        h2_debug (data->io, data, "[SEND_BODY] EOF");
                        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                        data->io->in_callback--;
                        return 0;
                } else if (data->data_source_error) {
                        set_error_for_data (data, g_steal_pointer (&data->data_source_error));
                        data->io->in_callback--;
                        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                } else {
                        h2_debug (data->io, data, "[SEND_BODY] Reading async");
                        g_byte_array_set_size (data->data_source_buffer, length);
                        g_input_stream_read_async (in_stream, data->data_source_buffer->data, length,
                                                   get_data_io_priority (data),
                                                   data->item->cancellable,
                                                   (GAsyncReadyCallback)on_data_read, data);
                        data->io->in_callback--;
                        return NGHTTP2_ERR_DEFERRED;
                }
        }
}

/* HTTP2 IO functions */

static int32_t
message_priority_to_weight (SoupMessage *msg)
{
        switch (soup_message_get_priority (msg)) {
        case SOUP_MESSAGE_PRIORITY_VERY_LOW:
                return NGHTTP2_MIN_WEIGHT;
        case SOUP_MESSAGE_PRIORITY_LOW:
                return (NGHTTP2_DEFAULT_WEIGHT - NGHTTP2_MIN_WEIGHT) / 2;
        case SOUP_MESSAGE_PRIORITY_NORMAL:
                return NGHTTP2_DEFAULT_WEIGHT;
        case SOUP_MESSAGE_PRIORITY_HIGH:
                return (NGHTTP2_MAX_WEIGHT - NGHTTP2_DEFAULT_WEIGHT) / 2;
        case SOUP_MESSAGE_PRIORITY_VERY_HIGH:
                return NGHTTP2_MAX_WEIGHT;
        }

        return NGHTTP2_DEFAULT_WEIGHT;
}

static void
message_priority_changed (SoupHTTP2MessageData *data)
{
        nghttp2_priority_spec priority_spec;
        int32_t weight;

        if (!data->stream_id)
                return;

        weight = message_priority_to_weight (data->msg);
        h2_debug (data->io, data, "[PRIORITY] weight=%d", weight);

        nghttp2_priority_spec_init (&priority_spec, 0, weight, 0);
        NGCHECK (nghttp2_submit_priority (data->io->session, NGHTTP2_FLAG_NONE, data->stream_id, &priority_spec));
        io_try_write (data->io, !data->item->async);
}

static SoupHTTP2MessageData *
add_message_to_io_data (SoupClientMessageIOHTTP2  *io,
                        SoupMessageQueueItem      *item,
                        SoupMessageIOCompletionFn  completion_cb,
                        gpointer                   completion_data)
{
        SoupHTTP2MessageData *data = g_new0 (SoupHTTP2MessageData, 1);

        data->item = soup_message_queue_item_ref (item);
        data->msg = item->msg;
        data->metrics = soup_message_get_metrics (data->msg);
        data->completion_cb = completion_cb;
        data->completion_data = completion_data;
        data->stream_id = 0;
        data->io = io;

        if (!g_hash_table_insert (io->messages, item->msg, data))
                g_warn_if_reached ();

        g_signal_connect_swapped (data->msg, "notify::priority",
                                  G_CALLBACK (message_priority_changed),
                                  data);

        return data;
}

static void
soup_http2_message_data_close (SoupHTTP2MessageData *data)
{
        /* Message data in close state is just waiting for reset stream to be sent
         * to be removed from the messages hash table. Everything is reset but
         * stream_id and io.
         */
        if (data->body_istream) {
                g_signal_handlers_disconnect_by_data (data->body_istream, data);
                g_clear_object (&data->body_istream);
        }

        if (data->msg)
                g_signal_handlers_disconnect_by_data (data->msg, data);

        data->msg = NULL;
        data->metrics = NULL;
        g_clear_pointer (&data->item, soup_message_queue_item_unref);
        g_clear_object (&data->decoded_data_istream);

        if (data->data_source_poll) {
                g_source_destroy (data->data_source_poll);
                g_clear_pointer (&data->data_source_poll, g_source_unref);
        }

        g_clear_error (&data->data_source_error);
        g_clear_pointer (&data->data_source_buffer, g_byte_array_unref);

        g_clear_error (&data->error);

        data->completion_cb = NULL;
        data->completion_data = NULL;
}

static void
soup_http2_message_data_free (SoupHTTP2MessageData *data)
{
        soup_http2_message_data_close (data);
        g_free (data);
}

static gboolean
request_header_is_valid (const char *name)
{
        static GHashTable *invalid_request_headers = NULL;

        if (g_once_init_enter (&invalid_request_headers)) {
                GHashTable *headers;

                headers= g_hash_table_new (soup_str_case_hash, soup_str_case_equal);
                g_hash_table_add (headers, "Connection");
                g_hash_table_add (headers, "Keep-Alive");
                g_hash_table_add (headers, "Proxy-Connection");
                g_hash_table_add (headers, "Transfer-Encoding");
                g_hash_table_add (headers, "Upgrade");

                g_once_init_leave (&invalid_request_headers, headers);
        }

        return !g_hash_table_contains (invalid_request_headers, name);
}

#define MAKE_NV(NAME, VALUE, VALUELEN)                                      \
        {                                                                   \
                (uint8_t *)NAME, (uint8_t *)VALUE, strlen (NAME), VALUELEN, \
                    NGHTTP2_NV_FLAG_NONE                                    \
        }

#define MAKE_NV2(NAME, VALUE)                                                     \
        {                                                                         \
                (uint8_t *)NAME, (uint8_t *)VALUE, strlen (NAME), strlen (VALUE), \
                    NGHTTP2_NV_FLAG_NONE                                          \
        }

#define MAKE_NV3(NAME, VALUE, FLAGS)                                              \
        {                                                                         \
                (uint8_t *)NAME, (uint8_t *)VALUE, strlen (NAME), strlen (VALUE), \
                    FLAGS                                                         \
        }

static void
send_message_request (SoupMessage          *msg,
                      SoupClientMessageIOHTTP2   *io,
                      SoupHTTP2MessageData *data)
{
        GArray *headers = g_array_new (FALSE, FALSE, sizeof (nghttp2_nv));

        GUri *uri = soup_message_get_uri (msg);
        char *host = soup_uri_get_host_for_headers (uri);
        char *authority = NULL;
        if (!soup_uri_uses_default_port (uri))
                authority = g_strdup_printf ("%s:%d", host, g_uri_get_port (uri));
        const char *authority_header = authority ? authority : host;

        char *path_and_query;
        if (soup_message_get_is_options_ping (msg))
                path_and_query = g_strdup ("*");
        else
                path_and_query = g_strdup_printf ("%s%c%s", g_uri_get_path (uri), g_uri_get_query (uri) ? '?' : '\0', g_uri_get_query (uri));

        const nghttp2_nv pseudo_headers[] = {
                MAKE_NV3 (":method", soup_message_get_method (msg), NGHTTP2_NV_FLAG_NO_COPY_VALUE),
                MAKE_NV2 (":scheme", g_uri_get_scheme (uri)),
                MAKE_NV2 (":authority", authority_header),
                MAKE_NV2 (":path", path_and_query),
        };

        for (guint i = 0; i < G_N_ELEMENTS (pseudo_headers); ++i) {
                g_array_append_val (headers, pseudo_headers[i]);
        }

        SoupMessageHeadersIter iter;
        const char *name, *value;
        soup_message_headers_iter_init (&iter, soup_message_get_request_headers (msg));
        while (soup_message_headers_iter_next (&iter, &name, &value)) {
                if (!request_header_is_valid (name))
                        continue;

                const nghttp2_nv nv = MAKE_NV2 (name, value);
                g_array_append_val (headers, nv);
        }

        GInputStream *body_stream = soup_message_get_request_body_stream (msg);
        SoupSessionFeature *logger = soup_session_get_feature_for_message (data->item->session, SOUP_TYPE_LOGGER, data->msg);
        if (logger && body_stream)
                data->logger = SOUP_LOGGER (logger);

        nghttp2_priority_spec priority_spec;
        nghttp2_priority_spec_init (&priority_spec, 0, message_priority_to_weight (msg), 0);

        int32_t stream_id;
        if (body_stream && soup_message_headers_get_expectations (soup_message_get_request_headers (msg)) & SOUP_EXPECTATION_CONTINUE) {
                data->expect_continue = TRUE;
                stream_id = nghttp2_submit_headers (io->session, 0, -1, &priority_spec, (const nghttp2_nv *)headers->data, headers->len, data);
        } else {
                nghttp2_data_provider data_provider;
                if (body_stream) {
                        data_provider.source.ptr = body_stream;
                        data_provider.read_callback = on_data_source_read_callback;
                }
                stream_id = nghttp2_submit_request (io->session, &priority_spec, (const nghttp2_nv *)headers->data, headers->len, body_stream ? &data_provider : NULL, data);
        }
        if (stream_id == NGHTTP2_ERR_STREAM_ID_NOT_AVAILABLE) {
                set_error_for_data (data,
                                    g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                         "HTTP/2 Error: stream ID not available"));
                data->can_be_restarted = TRUE;
        } else {
                NGCHECK (stream_id);
                data->stream_id = stream_id;
                h2_debug (io, data, "[SESSION] Request made for %s%s", authority_header, path_and_query);
                io_try_write (io, !data->item->async);
        }
        g_array_free (headers, TRUE);
        g_free (authority);
        g_free (host);
        g_free (path_and_query);
}

static void
soup_client_message_io_http2_send_item (SoupClientMessageIO       *iface,
                                        SoupMessageQueueItem      *item,
                                        SoupMessageIOCompletionFn  completion_cb,
                                        gpointer                   user_data)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = add_message_to_io_data (io, item, completion_cb, user_data);

        send_message_request (item->msg, io, data);
}

static SoupHTTP2MessageData *
get_data_for_message (SoupClientMessageIOHTTP2 *io,
                      SoupMessage              *msg)
{
        return g_hash_table_lookup (io->messages, msg);
}

static void
soup_client_message_io_http2_finished (SoupClientMessageIO *iface,
                                       SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data;
	SoupMessageIOCompletionFn completion_cb;
	gpointer completion_data;
        SoupMessageIOCompletion completion;

        data = get_data_for_message (io, msg);

        completion = data->state < STATE_READ_DONE ? SOUP_MESSAGE_IO_INTERRUPTED : SOUP_MESSAGE_IO_COMPLETE;

        h2_debug (io, data, "Finished: %s", completion == SOUP_MESSAGE_IO_COMPLETE ? "completed" : "interrupted");

	completion_cb = data->completion_cb;
	completion_data = data->completion_data;

	g_object_ref (msg);

        nghttp2_session_set_stream_user_data (io->session, data->stream_id, NULL);

        if (!io->is_shutdown) {
                NGCHECK (nghttp2_submit_rst_stream (io->session, NGHTTP2_FLAG_NONE, data->stream_id,
                                                    completion == SOUP_MESSAGE_IO_COMPLETE ? NGHTTP2_NO_ERROR : NGHTTP2_CANCEL));
                soup_http2_message_data_close (data);

                if (!g_hash_table_steal (io->messages, msg))
                        g_warn_if_reached ();
                if (!g_hash_table_add (io->closed_messages, data))
                        g_warn_if_reached ();
        } else {
                if (!g_hash_table_remove (io->messages, msg))
                        g_warn_if_reached ();
        }

	if (completion_cb)
		completion_cb (G_OBJECT (msg), SOUP_MESSAGE_IO_COMPLETE, completion_data);

	g_object_unref (msg);

        if (io->is_shutdown) {
                soup_client_message_io_http2_terminate_session (io);
                return;
        }

        io_try_write (io, FALSE);
}

static void
soup_client_message_io_http2_pause (SoupClientMessageIO *iface,
                                    SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        h2_debug (io, data, "[SESSION] Paused");

        if (data->paused)
                g_warn_if_reached ();

        data->paused = TRUE;
}

static void
soup_client_message_io_http2_unpause (SoupClientMessageIO *iface,
                                      SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        h2_debug (io, data, "[SESSION] Unpaused");

        if (!data->paused)
                g_warn_if_reached ();

        data->paused = FALSE;

        if (data->item->async)
                soup_http2_message_data_check_status (data);
}

static void
soup_client_message_io_http2_stolen (SoupClientMessageIO *iface)
{
        g_assert_not_reached ();
}

static gboolean
soup_client_message_io_http2_in_progress (SoupClientMessageIO *iface,
                                          SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;

        return io && get_data_for_message (io, msg) != NULL;
}

static gboolean
soup_client_message_io_http2_is_paused (SoupClientMessageIO *iface,
                                        SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        return data->paused;
}

static gboolean
soup_client_message_io_http2_is_open (SoupClientMessageIO *iface)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;

        if (!nghttp2_session_check_request_allowed (io->session))
                return FALSE;

        return !io->is_shutdown && !io->error;
}

static gboolean
soup_client_message_io_http2_is_reusable (SoupClientMessageIO *iface)
{
        return soup_client_message_io_http2_is_open (iface);
}

static GCancellable *
soup_client_message_io_http2_get_cancellable (SoupClientMessageIO *iface,
                                              SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        return data ? data->item->cancellable : NULL;
}

static void
client_stream_eof (SoupClientInputStream *stream,
                   gpointer               user_data)
{
	SoupMessage *msg = user_data;
	SoupClientMessageIOHTTP2 *io = get_io_data (msg);

        if (!io) {
                g_warn_if_reached ();
                return;
        }

        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
        h2_debug (io, data, "Client stream EOF");
        soup_message_set_metrics_timestamp (msg, SOUP_MESSAGE_METRICS_RESPONSE_END);
        advance_state_from (data, STATE_READ_DATA, STATE_READ_DONE);
        g_signal_handlers_disconnect_by_func (stream, client_stream_eof, msg);
        soup_message_got_body (data->msg);
}

static GInputStream *
soup_client_message_io_http2_get_response_istream (SoupClientMessageIO  *iface,
                                                   SoupMessage          *msg,
                                                   GError              **error)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
        GInputStream *client_stream, *base_stream;

        if (data->decoded_data_istream)
                base_stream = g_object_ref (data->decoded_data_istream);
        else /* For example with status_code == SOUP_STATUS_NO_CONTENT */
                base_stream = g_memory_input_stream_new ();

        client_stream = soup_client_input_stream_new (base_stream, msg);
        g_signal_connect (client_stream, "eof", G_CALLBACK (client_stream_eof), msg);

        g_object_unref (base_stream);

        return client_stream;
}

static gboolean
io_run (SoupHTTP2MessageData *data,
        GCancellable         *cancellable,
        GError              **error)
{
        gboolean progress = FALSE;

        if (data->state < STATE_WRITE_DONE && nghttp2_session_want_write (data->io->session))
                progress = io_write (data->io, TRUE, cancellable, error);
        else if (data->state < STATE_READ_DONE && nghttp2_session_want_read (data->io->session))
                progress = io_read (data->io, TRUE, cancellable, error);

        return progress;
}

static gboolean
io_run_until (SoupClientMessageIOHTTP2 *io,
              SoupMessage              *msg,
              SoupHTTP2IOState          state,
              GCancellable             *cancellable,
              GError                  **error)
{
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
	gboolean progress = TRUE, done;
	GError *my_error = NULL;

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return FALSE;
	else if (!io) {
		g_set_error_literal (error, G_IO_ERROR,
				     G_IO_ERROR_CANCELLED,
				     _("Operation was cancelled"));
		return FALSE;
	}

	g_object_ref (msg);

	while (progress && get_io_data (msg) == io && !data->paused && !data->error && data->state < state)
                progress = io_run (data, cancellable, &my_error);

        if (my_error) {
                g_propagate_error (error, my_error);
                g_object_unref (msg);
                return FALSE;
        }

	if (data->error) {
                g_propagate_error (error, g_steal_pointer (&data->error));
		g_object_unref (msg);
		return FALSE;
        }

        if (get_io_data (msg) != io) {
		g_set_error_literal (error, G_IO_ERROR,
				     G_IO_ERROR_CANCELLED,
				     _("Operation was cancelled"));
		g_object_unref (msg);
		return FALSE;
	}

	done = data->state >= state;

	g_object_unref (msg);
	return done;
}

static gboolean
soup_client_message_io_http2_run_until_read (SoupClientMessageIO  *iface,
                                             SoupMessage          *msg,
                                             GCancellable         *cancellable,
                                             GError              **error)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        if (io_run_until (io, msg, STATE_READ_DATA, cancellable, error))
                return TRUE;

        if (get_io_data (msg) == io) {
                if (data->can_be_restarted)
                        data->item->state = SOUP_MESSAGE_RESTARTING;
                else
                        soup_message_set_metrics_timestamp (msg, SOUP_MESSAGE_METRICS_RESPONSE_END);

                soup_client_message_io_http2_finished (iface, msg);
        }

        return FALSE;
}

static gboolean
soup_client_message_io_http2_skip (SoupClientMessageIO *iface,
                                   SoupMessage         *msg,
                                   gboolean             blocking,
                                   GCancellable        *cancellable,
                                   GError             **error)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data;

        if (g_cancellable_set_error_if_cancelled (cancellable, error))
                return FALSE;

        data = get_data_for_message (io, msg);
        if (!data || data->state == STATE_READ_DONE)
                return TRUE;

        h2_debug (io, data, "Skip");
        NGCHECK (nghttp2_submit_rst_stream (io->session, NGHTTP2_FLAG_NONE, data->stream_id, NGHTTP2_STREAM_CLOSED));
        io_try_write (io, blocking);
        return TRUE;
}

static void
soup_client_message_io_http2_run (SoupClientMessageIO *iface,
                                  SoupMessage         *msg,
		                  gboolean             blocking)
{
        g_assert_not_reached ();
}

static void
soup_client_message_io_http2_run_until_read_async (SoupClientMessageIO *iface,
                                                   SoupMessage         *msg,
                                                   int                  io_priority,
                                                   GCancellable        *cancellable,
                                                   GAsyncReadyCallback  callback,
                                                   gpointer             user_data)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        data->task = g_task_new (msg, cancellable, callback, user_data);
        g_task_set_priority (data->task, io_priority);
        io->pending_io_messages = g_list_prepend (io->pending_io_messages, data);
        if (data->error)
                soup_http2_message_data_check_status (data);
}

static gboolean
soup_client_message_io_http2_close_async (SoupClientMessageIO *iface,
                                          SoupConnection      *conn,
                                          GAsyncReadyCallback  callback)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;

        if (io->goaway_sent)
                return FALSE;

        g_assert (!io->close_task);
        io->close_task = g_task_new (conn, NULL, callback, NULL);
        soup_client_message_io_http2_terminate_session (io);
        return TRUE;
}

static void
soup_client_message_io_http2_destroy (SoupClientMessageIO *iface)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;

        if (io->read_source) {
                g_source_destroy (io->read_source);
                g_source_unref (io->read_source);
        }
        if (io->write_source) {
                g_source_destroy (io->write_source);
                g_source_unref (io->write_source);
        }

        if (io->conn)
                g_object_remove_weak_pointer (G_OBJECT (io->conn), (gpointer*)&io->conn);
        g_clear_object (&io->stream);
        g_clear_object (&io->close_task);
        g_clear_pointer (&io->session, nghttp2_session_del);
        g_clear_pointer (&io->messages, g_hash_table_unref);
        g_clear_pointer (&io->closed_messages, g_hash_table_unref);
        g_clear_pointer (&io->pending_io_messages, g_list_free);
        g_clear_error (&io->error);

        g_free (io);
}

static const SoupClientMessageIOFuncs io_funcs = {
        soup_client_message_io_http2_destroy,
        soup_client_message_io_http2_finished,
        soup_client_message_io_http2_stolen,
        soup_client_message_io_http2_send_item,
        soup_client_message_io_http2_get_response_istream,
        soup_client_message_io_http2_pause,
        soup_client_message_io_http2_unpause,
        soup_client_message_io_http2_is_paused,
        soup_client_message_io_http2_run,
        soup_client_message_io_http2_run_until_read,
        soup_client_message_io_http2_run_until_read_async,
        soup_client_message_io_http2_close_async,
        soup_client_message_io_http2_skip,
        soup_client_message_io_http2_is_open,
        soup_client_message_io_http2_in_progress,
        soup_client_message_io_http2_is_reusable,
        soup_client_message_io_http2_get_cancellable
};

G_GNUC_PRINTF(1, 0)
static void
debug_nghttp2 (const char *format,
               va_list     args)
{
        char *message;
        gsize len;

        if (g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, "nghttp2"))
                return;

        message = g_strdup_vprintf (format, args);
        len = strlen (message);
        if (len >= 1 && message[len - 1] == '\n')
                message[len - 1] = '\0';
        g_log ("nghttp2", G_LOG_LEVEL_DEBUG, "[NGHTTP2] %s", message);
        g_free (message);
}

static void
soup_client_message_io_http2_init (SoupClientMessageIOHTTP2 *io)
{
        static gsize nghttp2_debug_init = 0;
        if (g_once_init_enter (&nghttp2_debug_init)) {
                nghttp2_set_debug_vprintf_callback(debug_nghttp2);
                g_once_init_leave (&nghttp2_debug_init, 1);
        }

        nghttp2_session_callbacks *callbacks;
        NGCHECK (nghttp2_session_callbacks_new (&callbacks));
        nghttp2_session_callbacks_set_on_header_callback (callbacks, on_header_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback (callbacks, on_frame_recv_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback (callbacks, on_data_chunk_recv_callback);
        nghttp2_session_callbacks_set_on_begin_frame_callback (callbacks, on_begin_frame_callback);
        nghttp2_session_callbacks_set_before_frame_send_callback (callbacks, on_before_frame_send_callback);
        nghttp2_session_callbacks_set_on_frame_not_send_callback (callbacks, on_frame_not_send_callback);
        nghttp2_session_callbacks_set_on_frame_send_callback (callbacks, on_frame_send_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback (callbacks, on_stream_close_callback);

        NGCHECK (nghttp2_session_client_new (&io->session, callbacks, io));
        nghttp2_session_callbacks_del (callbacks);

        io->messages = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)soup_http2_message_data_free);
        io->closed_messages = g_hash_table_new_full (g_direct_hash, g_direct_equal, (GDestroyNotify)soup_http2_message_data_free, NULL);

        io->iface.funcs = &io_funcs;
}

#define INITIAL_WINDOW_SIZE (32 * 1024 * 1024) /* 32MB matches other implementations */
#define MAX_HEADER_TABLE_SIZE 65536 /* Match size used by Chromium/Firefox */

SoupClientMessageIO *
soup_client_message_io_http2_new (SoupConnection *conn)
{
        SoupClientMessageIOHTTP2 *io = g_new0 (SoupClientMessageIOHTTP2, 1);
        soup_client_message_io_http2_init (io);

        io->conn = conn;
        g_object_add_weak_pointer (G_OBJECT (io->conn), (gpointer*)&io->conn);

        io->stream = g_object_ref (soup_connection_get_iostream (conn));
        io->istream = g_io_stream_get_input_stream (io->stream);
        io->ostream = g_io_stream_get_output_stream (io->stream);
        io->connection_id = soup_connection_get_id (conn);

        io->read_source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (io->istream), NULL);
        g_source_set_name (io->read_source, "Soup HTTP/2 read source");
        g_source_set_callback (io->read_source, (GSourceFunc)io_read_ready, io, NULL);
        g_source_attach (io->read_source, g_main_context_get_thread_default ());

        NGCHECK (nghttp2_session_set_local_window_size (io->session, NGHTTP2_FLAG_NONE, 0, INITIAL_WINDOW_SIZE));

        const nghttp2_settings_entry settings[] = {
                { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, INITIAL_WINDOW_SIZE },
                { NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, MAX_HEADER_TABLE_SIZE },
                { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 },
        };
        NGCHECK (nghttp2_submit_settings (io->session, NGHTTP2_FLAG_NONE, settings, G_N_ELEMENTS (settings)));
        io_try_write (io, FALSE);

        return (SoupClientMessageIO *)io;
}
