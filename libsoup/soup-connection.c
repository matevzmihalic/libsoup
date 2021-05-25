/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/*
 * soup-connection.c: A single HTTP/HTTPS connection
 *
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "soup-connection.h"
#include "soup.h"
#include "soup-io-stream.h"
#include "soup-message-queue-item.h"
#include "soup-client-message-io-http1.h"
#include "soup-client-message-io-http2.h"
#include "soup-socket-properties.h"
#include "soup-private-enum-types.h"
#include <gio/gnetworking.h>

struct _SoupConnection {
        GObject parent_instance;
};

typedef struct {
	GIOStream *connection;
	GSocketConnectable *remote_connectable;
	GIOStream *iostream;
	SoupSocketProperties *socket_props;
        guint64 id;
        GSocketAddress *remote_address;
        gboolean force_http1;

	GUri *proxy_uri;
	gboolean ssl;

	SoupMessage *proxy_msg;
        SoupClientMessageIO *io_data;
	SoupConnectionState state;
	time_t       unused_timeout;
	GSource     *idle_timeout_src;
        guint        in_use;
        SoupHTTPVersion http_version;

	GCancellable *cancellable;
} SoupConnectionPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (SoupConnection, soup_connection, G_TYPE_OBJECT)

enum {
	EVENT,
	ACCEPT_CERTIFICATE,
	DISCONNECTED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum {
	PROP_0,

        PROP_ID,
	PROP_REMOTE_CONNECTABLE,
        PROP_REMOTE_ADDRESS,
	PROP_SOCKET_PROPERTIES,
	PROP_STATE,
	PROP_SSL,
	PROP_TLS_CERTIFICATE,
	PROP_TLS_CERTIFICATE_ERRORS,
        PROP_FORCE_HTTP1,

	LAST_PROPERTY
};

static GParamSpec *properties[LAST_PROPERTY] = { NULL, };

static void stop_idle_timer (SoupConnectionPrivate *priv);

/* Number of seconds after which we close a connection that hasn't yet
 * been used.
 */
#define SOUP_CONNECTION_UNUSED_TIMEOUT 3

static void
soup_connection_init (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        priv->http_version = SOUP_HTTP_1_1;
}

static void
soup_connection_finalize (GObject *object)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (SOUP_CONNECTION (object));

	g_clear_pointer (&priv->proxy_uri, g_uri_unref);
	g_clear_pointer (&priv->socket_props, soup_socket_properties_unref);
        g_clear_pointer (&priv->io_data, soup_client_message_io_destroy);
	g_clear_object (&priv->remote_connectable);
        g_clear_object (&priv->remote_address);
	g_clear_object (&priv->proxy_msg);

	if (priv->cancellable) {
		g_warning ("Disposing connection %p during connect", object);
		g_object_unref (priv->cancellable);
	}

	if (priv->connection) {
		g_warning ("Disposing connection %p while still connected", object);
		g_io_stream_close (priv->connection, NULL, NULL);
		g_object_unref (priv->connection);
	}

	g_clear_object (&priv->iostream);

	G_OBJECT_CLASS (soup_connection_parent_class)->finalize (object);
}

static void
soup_connection_dispose (GObject *object)
{
	SoupConnection *conn = SOUP_CONNECTION (object);
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

	stop_idle_timer (priv);

	G_OBJECT_CLASS (soup_connection_parent_class)->dispose (object);
}

static void
soup_connection_set_property (GObject *object, guint prop_id,
			      const GValue *value, GParamSpec *pspec)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (SOUP_CONNECTION (object));

	switch (prop_id) {
	case PROP_REMOTE_CONNECTABLE:
		priv->remote_connectable = g_value_dup_object (value);
		break;
	case PROP_SOCKET_PROPERTIES:
		priv->socket_props = g_value_dup_boxed (value);
		break;
	case PROP_SSL:
		priv->ssl = g_value_get_boolean (value);
		break;
	case PROP_ID:
		priv->id = g_value_get_uint64 (value);
		break;
	case PROP_FORCE_HTTP1:
		priv->force_http1 = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
soup_connection_get_property (GObject *object, guint prop_id,
			      GValue *value, GParamSpec *pspec)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (SOUP_CONNECTION (object));

	switch (prop_id) {
	case PROP_REMOTE_CONNECTABLE:
		g_value_set_object (value, priv->remote_connectable);
		break;
        case PROP_REMOTE_ADDRESS:
                g_value_set_object (value, priv->remote_address);
	        break;
	case PROP_SOCKET_PROPERTIES:
		g_value_set_boxed (value, priv->socket_props);
		break;
	case PROP_STATE:
		g_value_set_enum (value, priv->state);
		break;
	case PROP_SSL:
		g_value_set_boolean (value, priv->ssl);
		break;
	case PROP_ID:
		g_value_set_uint64 (value, priv->id);
		break;
	case PROP_TLS_CERTIFICATE:
		g_value_set_object (value, soup_connection_get_tls_certificate (SOUP_CONNECTION (object)));
		break;
	case PROP_TLS_CERTIFICATE_ERRORS:
		g_value_set_flags (value, soup_connection_get_tls_certificate_errors (SOUP_CONNECTION (object)));
		break;
	case PROP_FORCE_HTTP1:
		g_value_set_boolean (value, priv->force_http1);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
soup_connection_class_init (SoupConnectionClass *connection_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (connection_class);

	/* virtual method override */
	object_class->dispose = soup_connection_dispose;
	object_class->finalize = soup_connection_finalize;
	object_class->set_property = soup_connection_set_property;
	object_class->get_property = soup_connection_get_property;

	/* signals */
	signals[EVENT] =
		g_signal_new ("event",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      0,
			      NULL, NULL,
			      NULL,
			      G_TYPE_NONE, 2,
			      G_TYPE_SOCKET_CLIENT_EVENT,
			      G_TYPE_IO_STREAM);
	signals[ACCEPT_CERTIFICATE] =
		g_signal_new ("accept-certificate",
                              G_OBJECT_CLASS_TYPE (object_class),
                              G_SIGNAL_RUN_LAST,
                              0,
                              g_signal_accumulator_true_handled, NULL,
                              NULL,
                              G_TYPE_BOOLEAN, 2,
                              G_TYPE_TLS_CERTIFICATE,
                              G_TYPE_TLS_CERTIFICATE_FLAGS);
	signals[DISCONNECTED] =
		g_signal_new ("disconnected",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      0,
			      NULL, NULL,
			      NULL,
			      G_TYPE_NONE, 0);

	/* properties */
        properties[PROP_REMOTE_CONNECTABLE] =
                g_param_spec_object ("remote-connectable",
                                     "Remote Connectable",
                                     "Socket to connect to make outgoing connections on",
                                     G_TYPE_SOCKET_CONNECTABLE,
                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                     G_PARAM_STATIC_STRINGS);
        properties[PROP_REMOTE_ADDRESS] =
                g_param_spec_object ("remote-address",
                                     "Remote Address",
                                     "Remote address of connection",
                                     G_TYPE_SOCKET_ADDRESS,
                                     G_PARAM_READABLE |
                                     G_PARAM_STATIC_STRINGS);
        properties[PROP_SOCKET_PROPERTIES] =
		g_param_spec_boxed ("socket-properties",
				    "Socket properties",
				    "Socket properties",
				    SOUP_TYPE_SOCKET_PROPERTIES,
				    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);
        properties[PROP_STATE] =
		g_param_spec_enum ("state",
				   "Connection state",
				   "Current state of connection",
				   SOUP_TYPE_CONNECTION_STATE,
                                   SOUP_CONNECTION_NEW,
				   G_PARAM_READABLE |
				   G_PARAM_STATIC_STRINGS);
        properties[PROP_SSL] =
		g_param_spec_boolean ("ssl",
				      "Connection uses TLS",
				      "Whether the connection should use TLS",
				      FALSE,G_PARAM_READWRITE |
				      G_PARAM_STATIC_STRINGS);
        properties[PROP_ID] =
		g_param_spec_uint64 ("id",
                                     "Connection Identifier",
                                     "Unique identifier for the connection",
                                     0, G_MAXUINT64,
                                     0, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                     G_PARAM_STATIC_STRINGS);
        properties[PROP_TLS_CERTIFICATE] =
		g_param_spec_object ("tls-certificate",
                                     "TLS Certificate",
                                     "The TLS certificate associated with the connection",
                                     G_TYPE_TLS_CERTIFICATE,
                                     G_PARAM_READABLE |
	                             G_PARAM_STATIC_STRINGS);
        properties[PROP_TLS_CERTIFICATE_ERRORS] =
                g_param_spec_flags ("tls-certificate-errors",
                                    "TLS Certificate Errors",
                                    "The verification errors on the connections's TLS certificate",
                                    G_TYPE_TLS_CERTIFICATE_FLAGS, 0,
                                    G_PARAM_READABLE |
                                    G_PARAM_STATIC_STRINGS);
        properties[PROP_FORCE_HTTP1] =
                g_param_spec_boolean ("force-http1",
                                      "Force HTTP 1.x",
                                      "Force connection to use HTTP 1.x",
                                      FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                      G_PARAM_STATIC_STRINGS);

        g_object_class_install_properties (object_class, LAST_PROPERTY, properties);
}

static void
soup_connection_event (SoupConnection      *conn,
		       GSocketClientEvent   event,
		       GIOStream           *connection)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

	g_signal_emit (conn, signals[EVENT], 0,
		       event, connection ? connection : priv->connection);
}

static gboolean
idle_timeout (gpointer conn)
{
	soup_connection_disconnect (conn);
	return FALSE;
}

static void
start_idle_timer (SoupConnection *conn)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

	if (priv->socket_props->idle_timeout > 0 && !priv->idle_timeout_src) {
		priv->idle_timeout_src =
			soup_add_timeout (g_main_context_get_thread_default (),
					  priv->socket_props->idle_timeout * 1000,
					  idle_timeout, conn);
	}
}

static void
stop_idle_timer (SoupConnectionPrivate *priv)
{
	if (priv->idle_timeout_src) {
		g_source_destroy (priv->idle_timeout_src);
                g_clear_pointer (&priv->idle_timeout_src, g_source_unref);
	}
}

static void
soup_connection_set_state (SoupConnection     *conn,
                           SoupConnectionState state)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        if (priv->state == state)
                return;

        priv->state = state;
        if (priv->state == SOUP_CONNECTION_IDLE)
                start_idle_timer (conn);

        g_object_notify_by_pspec (G_OBJECT (conn), properties[PROP_STATE]);
}

static void
proxy_msg_got_body (SoupMessage    *msg,
                    SoupConnection *conn)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

	if (SOUP_STATUS_IS_SUCCESSFUL (soup_message_get_status (msg))) {
		soup_connection_event (conn, G_SOCKET_CLIENT_PROXY_NEGOTIATED, NULL);

		/* We're now effectively no longer proxying */
		g_clear_pointer (&priv->proxy_uri, g_uri_unref);
                g_signal_handlers_disconnect_by_func (priv->proxy_msg, proxy_msg_got_body, conn);
                g_clear_object (&priv->proxy_msg);
	}
}

static void
clear_proxy_msg (SoupConnection *conn)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        if (!priv->proxy_msg)
                return;

        g_signal_handlers_disconnect_by_func (priv->proxy_msg, proxy_msg_got_body, conn);
        g_clear_object (&priv->proxy_msg);
}

static void
set_proxy_msg (SoupConnection *conn,
               SoupMessage    *msg)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        g_assert (priv->http_version != SOUP_HTTP_2_0);

        clear_proxy_msg (conn);
	priv->proxy_msg = g_object_ref (msg);
	g_signal_connect_object (msg, "got-body",
                                 G_CALLBACK (proxy_msg_got_body),
                                 conn, 0);

        soup_connection_event (conn, G_SOCKET_CLIENT_PROXY_NEGOTIATING, NULL);
}

static void
soup_connection_set_connection (SoupConnection *conn,
				GIOStream      *connection)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        g_clear_pointer (&priv->io_data, soup_client_message_io_destroy);

	g_clear_object (&priv->connection);
	priv->connection = connection;
	g_clear_object (&priv->iostream);
	priv->iostream = soup_io_stream_new (G_IO_STREAM (priv->connection), FALSE);
}

static void
soup_connection_create_io_data (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        g_assert (!priv->io_data);
        switch (priv->http_version) {
        case SOUP_HTTP_1_0:
        case SOUP_HTTP_1_1:
                priv->io_data = soup_client_message_io_http1_new (priv->iostream);
                break;
        case SOUP_HTTP_2_0:
                priv->io_data = soup_client_message_io_http2_new (priv->iostream, priv->id);
                break;
        }
}

static void
re_emit_socket_event (GSocketClient       *client,
		      GSocketClientEvent   event,
		      GSocketConnectable  *connectable,
		      GIOStream           *connection,
		      SoupConnection      *conn)
{
	/* We handle COMPLETE ourselves */
	if (event == G_SOCKET_CLIENT_COMPLETE)
		return;

	soup_connection_event (conn, event, connection);
}

static GSocketClient *
new_socket_client (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);
        GSocketClient *client;
        SoupSocketProperties *props = priv->socket_props;

        client = g_socket_client_new ();
        g_signal_connect_object (client, "event",
                                 G_CALLBACK (re_emit_socket_event),
                                 conn, 0);

	if (!props->proxy_use_default) {
		if (props->proxy_resolver) {
			g_socket_client_set_proxy_resolver (client, props->proxy_resolver);
			g_socket_client_add_application_proxy (client, "http");
		} else
			g_socket_client_set_enable_proxy (client, FALSE);
	}
        if (props->io_timeout)
                g_socket_client_set_timeout (client, props->io_timeout);
        if (props->local_addr)
                g_socket_client_set_local_address (client, G_SOCKET_ADDRESS (props->local_addr));

        return client;
}

static gboolean
tls_connection_accept_certificate (SoupConnection      *conn,
                                   GTlsCertificate     *tls_certificate,
                                   GTlsCertificateFlags tls_errors)
{
	gboolean accept = FALSE;

        g_signal_emit (conn, signals[ACCEPT_CERTIFICATE], 0,
		       tls_certificate, tls_errors, &accept);
        return accept;
}

static void
tls_connection_peer_certificate_changed (SoupConnection *conn)
{
	g_object_notify_by_pspec (G_OBJECT (conn), properties[PROP_TLS_CERTIFICATE]);
}

static GTlsClientConnection *
new_tls_connection (SoupConnection    *conn,
                    GSocketConnection *connection,
                    GError           **error)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);
        GTlsClientConnection *tls_connection;
        GPtrArray *advertised_protocols = g_ptr_array_sized_new (4);

        // https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xhtml
        if (!priv->force_http1)
                g_ptr_array_add (advertised_protocols, "h2");

        g_ptr_array_add (advertised_protocols, "http/1.1");
        g_ptr_array_add (advertised_protocols, "http/1.0");
        g_ptr_array_add (advertised_protocols, NULL);

        tls_connection = g_initable_new (g_tls_backend_get_client_connection_type (g_tls_backend_get_default ()),
                                         priv->cancellable, error,
                                         "base-io-stream", connection,
                                         "server-identity", priv->remote_connectable,
                                         "require-close-notify", FALSE,
                                         "interaction", priv->socket_props->tls_interaction,
                                         "advertised-protocols", advertised_protocols->pdata,
                                         NULL);

        g_ptr_array_unref (advertised_protocols);

        if (!tls_connection)
                return NULL;

	if (!priv->socket_props->tlsdb_use_default)
		g_tls_connection_set_database (G_TLS_CONNECTION (tls_connection), priv->socket_props->tlsdb);

	g_signal_connect_object (tls_connection, "accept-certificate",
				 G_CALLBACK (tls_connection_accept_certificate),
				 conn, G_CONNECT_SWAPPED);
	g_signal_connect_object (tls_connection, "notify::peer-certificate",
				 G_CALLBACK (tls_connection_peer_certificate_changed),
				 conn, G_CONNECT_SWAPPED);

        return tls_connection;
}

static gboolean
soup_connection_connected (SoupConnection    *conn,
                           GSocketConnection *connection,
                           GError           **error)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);
        GSocket *socket;

        socket = g_socket_connection_get_socket (connection);
        g_socket_set_timeout (socket, priv->socket_props->io_timeout);
        g_socket_set_option (socket, IPPROTO_TCP, TCP_NODELAY, TRUE, NULL);

        g_clear_object (&priv->remote_address);
        priv->remote_address = g_socket_get_remote_address (socket, NULL);
        g_object_notify_by_pspec (G_OBJECT (conn), properties[PROP_REMOTE_ADDRESS]);

        if (priv->remote_address && G_IS_PROXY_ADDRESS (priv->remote_address)) {
                GProxyAddress *paddr = G_PROXY_ADDRESS (priv->remote_address);

                if (strcmp (g_proxy_address_get_protocol (paddr), "http") == 0) {
                        GError *error = NULL;
                        priv->proxy_uri = g_uri_parse (g_proxy_address_get_uri (paddr), SOUP_HTTP_URI_FLAGS, &error);
                        if (error) {
                                g_warning ("Failed to parse proxy URI %s: %s", g_proxy_address_get_uri (paddr), error->message);
                                g_error_free (error);
                        }
                }
        }

        if (priv->ssl && !priv->proxy_uri) {
                GTlsClientConnection *tls_connection;

                tls_connection = new_tls_connection (conn, connection, error);
                if (!tls_connection)
                        return FALSE;

                g_object_unref (connection);
                soup_connection_set_connection (conn, G_IO_STREAM (tls_connection));
        } else {
                soup_connection_set_connection (conn, G_IO_STREAM (connection));
        }

        return TRUE;
}

static void
soup_connection_complete (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        g_clear_object (&priv->cancellable);

        if (G_IS_TLS_CONNECTION (priv->connection)) {
                const char *protocol = g_tls_connection_get_negotiated_protocol (G_TLS_CONNECTION (priv->connection));
                if (g_strcmp0 (protocol, "h2") == 0)
                        priv->http_version = SOUP_HTTP_2_0;
                else if (g_strcmp0 (protocol, "http/1.0") == 0)
                        priv->http_version = SOUP_HTTP_1_0;
                else if (g_strcmp0 (protocol, "http/1.1") == 0)
                        priv->http_version = SOUP_HTTP_1_1;
        }

        if (!priv->ssl || !priv->proxy_uri) {
                soup_connection_event (conn,
                                       G_SOCKET_CLIENT_COMPLETE,
                                       NULL);
        }

        soup_connection_create_io_data (conn);

        soup_connection_set_state (conn, SOUP_CONNECTION_IN_USE);
        priv->unused_timeout = time (NULL) + SOUP_CONNECTION_UNUSED_TIMEOUT;
        start_idle_timer (conn);
}

static void
handshake_ready_cb (GTlsConnection *tls_connection,
                    GAsyncResult   *result,
                    GTask          *task)
{
        SoupConnection *conn = g_task_get_source_object (task);
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);
        GError *error = NULL;

        if (g_tls_connection_handshake_finish (tls_connection, result, &error)) {
                soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKED, NULL);
                soup_connection_complete (conn);
                g_task_return_boolean (task, TRUE);
        } else {
                g_clear_object (&priv->cancellable);
                g_task_return_error (task, error);
        }
        g_object_unref (task);
}

static void
connect_async_ready_cb (GSocketClient *client,
                        GAsyncResult  *result,
                        GTask         *task)
{
        SoupConnection *conn = g_task_get_source_object (task);
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);
        GSocketConnection *connection;
        GError *error = NULL;

        connection = g_socket_client_connect_finish (client, result, &error);
        if (!connection) {
		g_clear_object (&priv->cancellable);
                g_task_return_error (task, error);
                g_object_unref (task);
                return;
        }

        if (!soup_connection_connected (conn, connection, &error)) {
		g_clear_object (&priv->cancellable);
                g_task_return_error (task, error);
                g_object_unref (task);
                g_object_unref (connection);
                return;
        }

        if (G_IS_TLS_CONNECTION (priv->connection)) {
                soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKING, NULL);

                g_tls_connection_handshake_async (G_TLS_CONNECTION (priv->connection),
                                                  g_task_get_priority (task),
                                                  priv->cancellable,
                                                  (GAsyncReadyCallback)handshake_ready_cb,
                                                  task);
                return;
        }

        soup_connection_complete (conn);
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
}

void
soup_connection_connect_async (SoupConnection      *conn,
                               int                  io_priority,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
        SoupConnectionPrivate *priv;
        GTask *task;
        GSocketClient *client;

        g_return_if_fail (SOUP_IS_CONNECTION (conn));

        priv = soup_connection_get_instance_private (conn);

        soup_connection_set_state (conn, SOUP_CONNECTION_CONNECTING);

        priv->cancellable = cancellable ? g_object_ref (cancellable) : g_cancellable_new ();
        task = g_task_new (conn, priv->cancellable, callback, user_data);
        g_task_set_priority (task, io_priority);

        client = new_socket_client (conn);
        g_socket_client_connect_async (client,
                                       priv->remote_connectable,
                                       priv->cancellable,
                                       (GAsyncReadyCallback)connect_async_ready_cb,
                                       task);
        g_object_unref (client);
}

gboolean
soup_connection_connect_finish (SoupConnection  *conn,
                                GAsyncResult    *result,
                                GError         **error)
{
        return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
soup_connection_connect (SoupConnection  *conn,
			 GCancellable    *cancellable,
			 GError         **error)
{
        SoupConnectionPrivate *priv;
        GSocketClient *client;
        GSocketConnection *connection;

        g_return_val_if_fail (SOUP_IS_CONNECTION (conn), FALSE);

        priv = soup_connection_get_instance_private (conn);

        soup_connection_set_state (conn, SOUP_CONNECTION_CONNECTING);

        priv->cancellable = cancellable ? g_object_ref (cancellable) : g_cancellable_new ();

        client = new_socket_client (conn);
        connection = g_socket_client_connect (client,
                                              priv->remote_connectable,
                                              priv->cancellable,
                                              error);
        g_object_unref (client);

        if (!connection) {
                g_clear_object (&priv->cancellable);
                return FALSE;
        }

        if (!soup_connection_connected (conn, connection, error)) {
                g_object_unref (connection);
                g_clear_object (&priv->cancellable);
                return FALSE;
        }

        if (G_IS_TLS_CONNECTION (priv->connection)) {
                soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKING, NULL);
                if (!g_tls_connection_handshake (G_TLS_CONNECTION (priv->connection),
                                                 priv->cancellable, error)) {
                        g_clear_object (&priv->cancellable);
                        return FALSE;
                }
                soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKED, NULL);
        }

        soup_connection_complete (conn);

        return TRUE;
}

gboolean
soup_connection_is_tunnelled (SoupConnection *conn)
{
	SoupConnectionPrivate *priv;

	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), FALSE);
	priv = soup_connection_get_instance_private (conn);

	return priv->ssl && priv->proxy_uri != NULL;
}

static void
tunnel_handshake_ready_cb (GTlsConnection *tls_connection,
                           GAsyncResult   *result,
                           GTask          *task)
{
        SoupConnection *conn = g_task_get_source_object (task);
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);
        GError *error = NULL;

        g_clear_object (&priv->cancellable);

        if (g_tls_connection_handshake_finish (tls_connection, result, &error)) {
                soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKED, NULL);
                soup_connection_event (conn, G_SOCKET_CLIENT_COMPLETE, NULL);

                g_assert (!priv->io_data);
                priv->io_data = soup_client_message_io_http1_new (priv->iostream);

                g_task_return_boolean (task, TRUE);
        } else {
                g_task_return_error (task, error);
        }
        g_object_unref (task);
}

void
soup_connection_tunnel_handshake_async (SoupConnection     *conn,
                                        int                 io_priority,
                                        GCancellable       *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer            user_data)
{
        SoupConnectionPrivate *priv;
        GTask *task;
        GTlsClientConnection *tls_connection;
        GError *error = NULL;

        g_return_if_fail (SOUP_IS_CONNECTION (conn));

        priv = soup_connection_get_instance_private (conn);
        g_return_if_fail (G_IS_SOCKET_CONNECTION (priv->connection));
        g_return_if_fail (priv->cancellable == NULL);

        priv->cancellable = cancellable ? g_object_ref (cancellable) : g_cancellable_new ();
        task = g_task_new (conn, priv->cancellable, callback, user_data);
        g_task_set_priority (task, io_priority);

        tls_connection = new_tls_connection (conn, G_SOCKET_CONNECTION (priv->connection), &error);
        if (!tls_connection) {
		g_clear_object (&priv->cancellable);
                g_task_return_error (task, error);
                g_object_unref (task);
                return;
        }

        soup_connection_set_connection (conn, G_IO_STREAM (tls_connection));
        soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKING, NULL);
        g_tls_connection_handshake_async (G_TLS_CONNECTION (priv->connection),
                                          g_task_get_priority (task),
                                          priv->cancellable,
                                          (GAsyncReadyCallback)tunnel_handshake_ready_cb,
                                          task);
}

gboolean
soup_connection_tunnel_handshake_finish (SoupConnection *conn,
                                         GAsyncResult   *result,
                                         GError        **error)
{
        g_return_val_if_fail (SOUP_IS_CONNECTION (conn), FALSE);

        return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
soup_connection_tunnel_handshake (SoupConnection *conn,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
        SoupConnectionPrivate *priv;
        GTlsClientConnection *tls_connection;

        g_return_val_if_fail (SOUP_IS_CONNECTION (conn), FALSE);

        priv = soup_connection_get_instance_private (conn);
        g_return_val_if_fail (G_IS_SOCKET_CONNECTION (priv->connection), FALSE);
        g_return_val_if_fail (priv->cancellable == NULL, FALSE);

        tls_connection = new_tls_connection (conn, G_SOCKET_CONNECTION (priv->connection), error);
        if (!tls_connection)
                return FALSE;

        soup_connection_set_connection (conn, G_IO_STREAM (tls_connection));
        soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKING, NULL);

        priv->cancellable = cancellable ? g_object_ref (cancellable) : g_cancellable_new ();
        if (!g_tls_connection_handshake (G_TLS_CONNECTION (priv->connection),
                                         priv->cancellable, error)) {
                g_clear_object (&priv->cancellable);
                return FALSE;
        }
        g_clear_object (&priv->cancellable);

        soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKED, NULL);
        soup_connection_event (conn, G_SOCKET_CLIENT_COMPLETE, NULL);

        g_assert (!priv->io_data);
        priv->io_data = soup_client_message_io_http1_new (priv->iostream);

        return TRUE;
}

static void
soup_connection_disconnected (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        if (priv->connection) {
                GIOStream *connection;

                connection = priv->connection;
                priv->connection = NULL;

                g_io_stream_close (connection, NULL, NULL);
                g_signal_handlers_disconnect_by_data (connection, conn);
                g_object_unref (connection);
        }

        g_signal_emit (conn, signals[DISCONNECTED], 0);
}

static void
client_message_io_closed_cb (SoupConnection *conn,
                             GAsyncResult   *result)
{
        g_task_propagate_boolean (G_TASK (result), NULL);
        soup_connection_disconnected (conn);
}

/**
 * soup_connection_disconnect:
 * @conn: a connection
 *
 * Disconnects @conn's socket and emits a %disconnected signal.
 * After calling this, @conn will be essentially useless.
 **/
void
soup_connection_disconnect (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        if (priv->state == SOUP_CONNECTION_DISCONNECTED)
                return;

        soup_connection_set_state (conn, SOUP_CONNECTION_DISCONNECTED);

        if (priv->cancellable) {
                g_cancellable_cancel (priv->cancellable);
                priv->cancellable = NULL;
        }

        if (priv->io_data &&
            soup_client_message_io_close_async (priv->io_data, conn, (GAsyncReadyCallback)client_message_io_closed_cb))
                return;

        soup_connection_disconnected (conn);
}

GSocket *
soup_connection_get_socket (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);
        GSocketConnection *connection = NULL;

        g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);

        if (G_IS_TLS_CONNECTION (priv->connection)) {
                g_object_get (priv->connection, "base-io-stream", &connection, NULL);
                g_object_unref (connection);
        } else if (G_IS_SOCKET_CONNECTION (priv->connection))
                connection = G_SOCKET_CONNECTION (priv->connection);

        return connection ? g_socket_connection_get_socket (connection) : NULL;
}

GIOStream *
soup_connection_get_iostream (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);

        return priv->iostream;
}

GIOStream *
soup_connection_steal_iostream (SoupConnection *conn)
{
        SoupConnectionPrivate *priv;
        GSocket *socket;
        GIOStream *iostream;

        g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);

        socket = soup_connection_get_socket (conn);
        g_socket_set_timeout (socket, 0);

        priv = soup_connection_get_instance_private (conn);
        iostream = priv->iostream;
        priv->iostream = NULL;

        g_object_set_data_full (G_OBJECT (iostream), "GSocket",
                                g_object_ref (socket), g_object_unref);
        g_clear_object (&priv->connection);

        if (priv->io_data)
                soup_client_message_io_stolen (priv->io_data);

        return iostream;
}

GUri *
soup_connection_get_proxy_uri (SoupConnection *conn)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);

	return priv->proxy_uri;
}

gboolean
soup_connection_is_via_proxy (SoupConnection *conn)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), FALSE);

	return priv->proxy_uri != NULL;
}

gboolean
soup_connection_is_idle_open (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        g_assert (priv->state == SOUP_CONNECTION_IDLE);

	if (!g_socket_is_connected (soup_connection_get_socket (conn)))
		return FALSE;

	if (priv->unused_timeout && priv->unused_timeout < time (NULL))
		return FALSE;

        return soup_client_message_io_is_open (priv->io_data);
}

SoupConnectionState
soup_connection_get_state (SoupConnection *conn)
{
	SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

	return priv->state;
}

void
soup_connection_set_in_use (SoupConnection *conn,
                            gboolean        in_use)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        g_assert (in_use || priv->in_use > 0);

        if (in_use)
                priv->in_use++;
        else
                priv->in_use--;

        if (priv->in_use > 0) {
                if (priv->state == SOUP_CONNECTION_IDLE)
                        soup_connection_set_state (conn, SOUP_CONNECTION_IN_USE);
                return;
        }

        clear_proxy_msg (conn);

        if (soup_connection_is_reusable (conn))
                soup_connection_set_state (conn, SOUP_CONNECTION_IDLE);
        else
                soup_connection_disconnect (conn);
}

SoupClientMessageIO *
soup_connection_setup_message_io (SoupConnection *conn,
                                  SoupMessage    *msg)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        g_assert (priv->state == SOUP_CONNECTION_IN_USE);

        priv->unused_timeout = 0;
        stop_idle_timer (priv);

        if (priv->proxy_uri && soup_message_get_method (msg) == SOUP_METHOD_CONNECT)
                set_proxy_msg (conn, msg);

        if (!soup_client_message_io_is_reusable (priv->io_data)) {
                g_clear_pointer (&priv->io_data, soup_client_message_io_destroy);
                soup_connection_create_io_data (conn);
        }

        return priv->io_data;
}

GTlsCertificate *
soup_connection_get_tls_certificate (SoupConnection *conn)
{
	SoupConnectionPrivate *priv;

	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);

	priv = soup_connection_get_instance_private (conn);

	if (!G_IS_TLS_CONNECTION (priv->connection))
		return NULL;

	return g_tls_connection_get_peer_certificate (G_TLS_CONNECTION (priv->connection));
}

GTlsCertificateFlags
soup_connection_get_tls_certificate_errors (SoupConnection *conn)
{
	SoupConnectionPrivate *priv;

	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), 0);

	priv = soup_connection_get_instance_private (conn);

	if (!G_IS_TLS_CONNECTION (priv->connection))
		return 0;

	return g_tls_connection_get_peer_certificate_errors (G_TLS_CONNECTION (priv->connection));
}

guint64
soup_connection_get_id (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        return priv->id;
}

GSocketAddress *
soup_connection_get_remote_address (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        return priv->remote_address;
}

SoupHTTPVersion
soup_connection_get_negotiated_protocol (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        return priv->http_version;
}

gboolean
soup_connection_is_reusable (SoupConnection *conn)
{
        SoupConnectionPrivate *priv = soup_connection_get_instance_private (conn);

        return priv->io_data && soup_client_message_io_is_reusable (priv->io_data);
}
