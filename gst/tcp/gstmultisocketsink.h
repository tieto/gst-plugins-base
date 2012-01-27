/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) <2004> Thomas Vander Stichele <thomas at apestaart dot org>
 * Copyright (C) <2011> Collabora Ltd.
 *     Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#ifndef __GST_MULTI_SOCKET_SINK_H__
#define __GST_MULTI_SOCKET_SINK_H__

#include <gio/gio.h>

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include "gstmultihandlesink.h"

G_BEGIN_DECLS

#define GST_TYPE_MULTI_SOCKET_SINK \
  (gst_multi_socket_sink_get_type())
#define GST_MULTI_SOCKET_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_MULTI_SOCKET_SINK,GstMultiSocketSink))
#define GST_MULTI_SOCKET_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_MULTI_SOCKET_SINK,GstMultiSocketSinkClass))
#define GST_IS_MULTI_SOCKET_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_MULTI_SOCKET_SINK))
#define GST_IS_MULTI_SOCKET_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_MULTI_SOCKET_SINK))
#define GST_MULTI_SOCKET_SINK_GET_CLASS(klass) \
  (G_TYPE_INSTANCE_GET_CLASS ((klass), GST_TYPE_MULTI_SOCKET_SINK, GstMultiSocketSinkClass))


typedef struct _GstMultiSocketSink GstMultiSocketSink;
typedef struct _GstMultiSocketSinkClass GstMultiSocketSinkClass;

/* structure for a client
 */
typedef struct {
  GstMultiHandleClient client;

  GSocket *socket;
  GSource *source;

  /* method to sync client when connecting */
  GstSyncMethod sync_method;
  GstFormat     burst_min_format;
  guint64       burst_min_value;
  GstFormat     burst_max_format;
  guint64       burst_max_value;
} GstSocketClient;

/**
 * GstMultiSocketSink:
 *
 * The multisocketsink object structure.
 */
struct _GstMultiSocketSink {
  GstMultiHandleSink element;

  /*< private >*/
  GHashTable *socket_hash;  /* index on socket to client */

  GMainContext *main_context;
  GCancellable *cancellable;

  gboolean previous_buffer_in_caps;

  guint mtu;
  gint qos_dscp;

  /* these values are used to check if a client is reading fast
   * enough and to control receovery */
  GstFormat unit_type;/* the format of the units */
  gint64 units_max;       /* max units to queue for a client */
  gint64 units_soft_max;  /* max units a client can lag before recovery starts */

  GstFormat     def_burst_format;
  guint64       def_burst_value;

  guint8 header_flags;
};

struct _GstMultiSocketSinkClass {
  GstMultiHandleSinkClass parent_class;

  /* element methods */
  void          (*add)          (GstMultiSocketSink *sink, GSocket *socket);
  void          (*add_full)     (GstMultiSocketSink *sink, GSocket *socket, GstSyncMethod sync,
		                 GstFormat format, guint64 value, 
				 GstFormat max_format, guint64 max_value);
  void          (*remove)       (GstMultiSocketSink *sink, GSocket *socket);
  void          (*remove_flush) (GstMultiSocketSink *sink, GSocket *socket);
  void          (*clear)        (GstMultiSocketSink *sink);
  GstStructure* (*get_stats)    (GstMultiSocketSink *sink, GSocket *socket);

  /* vtable */
  void (*removed) (GstMultiSocketSink *sink, GSocket *socket);

  /* signals */
  void (*client_added) (GstElement *element, GSocket *socket);
  void (*client_removed) (GstElement *element, GSocket *socket, GstClientStatus status);
  void (*client_socket_removed) (GstElement *element, GSocket *socket);
};

GType gst_multi_socket_sink_get_type (void);

void          gst_multi_socket_sink_add          (GstMultiSocketSink *sink, GSocket *socket);
void          gst_multi_socket_sink_add_full     (GstMultiSocketSink *sink, GSocket *socket, GstSyncMethod sync,
                                              GstFormat min_format, guint64 min_value,
                                              GstFormat max_format, guint64 max_value);
void          gst_multi_socket_sink_remove       (GstMultiSocketSink *sink, GSocket *socket);
void          gst_multi_socket_sink_remove_flush (GstMultiSocketSink *sink, GSocket *socket);
void          gst_multi_socket_sink_clear        (GstMultiHandleSink *sink);
GstStructure*  gst_multi_socket_sink_get_stats    (GstMultiSocketSink *sink, GSocket *socket);

G_END_DECLS

#endif /* __GST_MULTI_SOCKET_SINK_H__ */