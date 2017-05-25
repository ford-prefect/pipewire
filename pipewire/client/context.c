/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include "pipewire/client/pipewire.h"

#include "pipewire/client/context.h"
#include "pipewire/client/introspect.h"
#include "pipewire/client/protocol-native.h"
#include "pipewire/client/connection.h"
#include "pipewire/client/subscribe.h"

struct context {
  struct pw_context this;

  bool no_proxy;

  int fd;
  struct pw_connection *connection;
  struct spa_source *source;

  bool disconnecting;
  struct pw_listener  need_flush;
  struct spa_source     *flush_event;
};

/**
 * pw_context_state_as_string:
 * @state: a #enum pw_context_state
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const char *
pw_context_state_as_string (enum pw_context_state state)
{
  switch (state) {
    case PW_CONTEXT_STATE_ERROR:
      return "error";
    case PW_CONTEXT_STATE_UNCONNECTED:
      return "unconnected";
    case PW_CONTEXT_STATE_CONNECTING:
      return "connecting";
    case PW_CONTEXT_STATE_CONNECTED:
      return "connected";
  }
  return "invalid-state";
}

static void
context_set_state (struct pw_context     *context,
                   enum pw_context_state  state,
                   const char            *fmt,
                   ...)
{
  if (context->state != state) {

    if (context->error)
      free (context->error);

    if (fmt) {
      va_list varargs;

      va_start (varargs, fmt);
      vasprintf (&context->error, fmt, varargs);
      va_end (varargs);
    } else {
      context->error = NULL;
    }
    pw_log_debug ("context %p: update state from %s -> %s (%s)", context,
                pw_context_state_as_string (context->state),
                pw_context_state_as_string (state),
                context->error);

    context->state = state;
    pw_signal_emit (&context->state_changed, context);
  }
}

static void
core_event_info (void                *object,
                 struct pw_core_info *info)
{
  struct pw_proxy *proxy = object;
  struct pw_context *this = proxy->context;
  enum pw_subscription_event event;

  pw_log_debug ("got core info");

  if (proxy->user_data == NULL)
    event = PW_SUBSCRIPTION_EVENT_NEW;
  else
    event = PW_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pw_core_info_update (proxy->user_data, info);

  pw_signal_emit (&this->subscription,
                  this,
                  event,
                  proxy->type,
                  proxy->id);
}

static void
core_event_done (void     *object,
                 uint32_t  seq)
{
  struct pw_proxy *proxy = object;
  struct pw_context *this = proxy->context;

  if (seq == 0) {
    pw_core_do_sync (this->core_proxy, 1);
  } else if (seq == 1) {
    context_set_state (this, PW_CONTEXT_STATE_CONNECTED, NULL);
  }
}

static void
core_event_error (void       *object,
                  uint32_t    id,
                  int   res,
                  const char *error, ...)
{
  struct pw_proxy *proxy = object;
  struct pw_context *this = proxy->context;
  context_set_state (this, PW_CONTEXT_STATE_ERROR, error);
}

static void
core_event_remove_id (void     *object,
                      uint32_t  id)
{
  struct pw_proxy *core_proxy = object;
  struct pw_context *this = core_proxy->context;
  struct pw_proxy *proxy;

  proxy = pw_map_lookup (&this->objects, id);
  if (proxy) {
    pw_log_debug ("context %p: object remove %u", this, id);
    pw_proxy_destroy (proxy);
  }
}

static void
core_event_update_types (void          *object,
                         uint32_t       first_id,
                         uint32_t       n_types,
                         const char   **types)
{
  struct pw_proxy *proxy = object;
  struct pw_context *this = proxy->context;
  int i;

  for (i = 0; i < n_types; i++, first_id++) {
    uint32_t this_id = spa_type_map_get_id (this->type.map, types[i]);
    if (!pw_map_insert_at (&this->types, first_id, PW_MAP_ID_TO_PTR (this_id)))
      pw_log_error ("can't add type for client");
  }
}

static const struct pw_core_events core_events = {
  &core_event_info,
  &core_event_done,
  &core_event_error,
  &core_event_remove_id,
  &core_event_update_types
};

static void
module_event_info (void                  *object,
                   struct pw_module_info *info)
{
  struct pw_proxy *proxy = object;
  struct pw_context *this = proxy->context;
  enum pw_subscription_event event;

  pw_log_debug ("got module info");

  if (proxy->user_data == NULL)
    event = PW_SUBSCRIPTION_EVENT_NEW;
  else
    event = PW_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pw_module_info_update (proxy->user_data, info);

  pw_signal_emit (&this->subscription,
                  this,
                  event,
                  proxy->type,
                  proxy->id);
}

static const struct pw_module_events module_events = {
  &module_event_info,
};

static void
node_event_info (void                *object,
                 struct pw_node_info *info)
{
  struct pw_proxy *proxy = object;
  struct pw_context *this = proxy->context;
  enum pw_subscription_event event;

  pw_log_debug ("got node info");

  if (proxy->user_data == NULL)
    event = PW_SUBSCRIPTION_EVENT_NEW;
  else
    event = PW_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pw_node_info_update (proxy->user_data, info);

  pw_signal_emit (&this->subscription,
                  this,
                  event,
                  proxy->type,
                  proxy->id);
}

static const struct pw_node_events node_events = {
  &node_event_info
};

static void
client_event_info (void                  *object,
                   struct pw_client_info *info)
{
  struct pw_proxy *proxy = object;
  struct pw_context *this = proxy->context;
  enum pw_subscription_event event;

  pw_log_debug ("got client info");

  if (proxy->user_data == NULL)
    event = PW_SUBSCRIPTION_EVENT_NEW;
  else
    event = PW_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pw_client_info_update (proxy->user_data, info);

  pw_signal_emit (&this->subscription,
                  this,
                  event,
                  proxy->type,
                  proxy->id);
}

static const struct pw_client_events client_events = {
  &client_event_info
};

static void
link_event_info (void                *object,
                 struct pw_link_info *info)
{
  struct pw_proxy *proxy = object;
  struct pw_context *this = proxy->context;
  enum pw_subscription_event event;

  pw_log_debug ("got link info");

  if (proxy->user_data == NULL)
    event = PW_SUBSCRIPTION_EVENT_NEW;
  else
    event = PW_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pw_link_info_update (proxy->user_data, info);

  pw_signal_emit (&this->subscription,
                     this,
                     event,
                     proxy->type,
                     proxy->id);
}

static const struct pw_link_events link_events = {
  &link_event_info
};

static void
registry_event_global (void          *object,
                       uint32_t       id,
                       const char    *type)
{
  struct pw_proxy *registry_proxy = object;
  struct pw_context *this = registry_proxy->context;
  struct context *impl = SPA_CONTAINER_OF (this, struct context, this);
  struct pw_proxy *proxy = NULL;

  if (impl->no_proxy)
    return;

  pw_log_debug ("got global %u %s", id, type);

  if (!strcmp (type, PIPEWIRE_TYPE__Node)) {
    proxy = pw_proxy_new (this,
                             SPA_ID_INVALID,
                             this->type.node);
    if (proxy == NULL)
      goto no_mem;

    proxy->implementation = &node_events;
  } else if (!strcmp (type, PIPEWIRE_TYPE__Module)) {
    proxy = pw_proxy_new (this,
                             SPA_ID_INVALID,
                             this->type.module);
    if (proxy == NULL)
      goto no_mem;

    proxy->implementation = &module_events;
  } else if (!strcmp (type, PIPEWIRE_TYPE__Client)) {
    proxy = pw_proxy_new (this,
                             SPA_ID_INVALID,
                             this->type.client);
    if (proxy == NULL)
      goto no_mem;

    proxy->implementation = &client_events;
  } else if (!strcmp (type, PIPEWIRE_TYPE__Link)) {
    proxy = pw_proxy_new (this,
                             SPA_ID_INVALID,
                             this->type.link);
    if (proxy == NULL)
      goto no_mem;

    proxy->implementation = &link_events;
  }
  if (proxy) {
    pw_registry_do_bind (registry_proxy, id, proxy->id);
  }

  return;

no_mem:
  pw_log_error ("context %p: failed to create proxy", this);
  return;
}

static void
registry_event_global_remove (void     *object,
                              uint32_t  id)
{
  struct pw_proxy *proxy = object;
  struct pw_context *this = proxy->context;

  pw_log_debug ("got global remove %u", id);

  pw_signal_emit (&this->subscription,
                     this,
                     PW_SUBSCRIPTION_EVENT_REMOVE,
                     SPA_ID_INVALID,
                     id);
}

static const struct pw_registry_events registry_events = {
  &registry_event_global,
  &registry_event_global_remove
};

typedef bool (*demarshal_func_t) (void *object, void *data, size_t size);

static void
do_flush_event (struct spa_loop_utils *utils,
                struct spa_source    *source,
                void         *data)
{
  struct context *impl = data;
  if (impl->connection)
    if (!pw_connection_flush (impl->connection))
      pw_context_disconnect (&impl->this);
}

static void
on_need_flush (struct pw_listener   *listener,
               struct pw_connection *connection)
{
  struct context *impl = SPA_CONTAINER_OF (listener, struct context, need_flush);
  struct pw_context *this = &impl->this;
  pw_loop_signal_event (this->loop, impl->flush_event);
}

static void
on_context_data (struct spa_loop_utils *utils,
                 struct spa_source    *source,
                 int           fd,
                 enum spa_io         mask,
                 void         *data)
{
  struct context *impl = data;
  struct pw_context *this = &impl->this;
  struct pw_connection *conn = impl->connection;

  if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
    context_set_state (this,
                       PW_CONTEXT_STATE_ERROR,
                       "connection closed");
    return;
  }

  if (mask & SPA_IO_IN) {
    uint8_t opcode;
    uint32_t id;
    uint32_t size;
    void *message;

    while (!impl->disconnecting
           && pw_connection_get_next (conn, &opcode, &id, &message, &size)) {
      struct pw_proxy *proxy;
      const demarshal_func_t *demarshal;

      pw_log_trace ("context %p: got message %d from %u", this, opcode, id);

      proxy = pw_map_lookup (&this->objects, id);
      if (proxy == NULL) {
        pw_log_error ("context %p: could not find proxy %u", this, id);
        continue;
      }
      if (opcode >= proxy->iface->n_events) {
        pw_log_error ("context %p: invalid method %u for %u", this, opcode, id);
        continue;
      }

      demarshal = proxy->iface->events;
      if (demarshal[opcode]) {
        if (!demarshal[opcode] (proxy, message, size))
          pw_log_error ("context %p: invalid message received %u for %u", this, opcode, id);
      } else
        pw_log_error ("context %p: function %d not implemented on %u", this, opcode, id);

    }
  }
}

/**
 * pw_context_new:
 * @context: a #GMainContext to run in
 * @name: an application name
 * @properties: (transfer full): optional properties
 *
 * Make a new unconnected #struct pw_context
 *
 * Returns: a new unconnected #struct pw_context
 */
struct pw_context *
pw_context_new (struct pw_loop       *loop,
                const char           *name,
                struct pw_properties *properties)
{
  struct context *impl;
  struct pw_context *this;

  impl = calloc (1, sizeof (struct context));
  if (impl == NULL)
    return NULL;

  impl->fd = -1;

  this = &impl->this;
  pw_log_debug ("context %p: new", impl);

  this->name = strdup (name);

  if (properties == NULL)
    properties = pw_properties_new ("application.name", name, NULL);
  if (properties == NULL)
    goto no_mem;

  pw_fill_context_properties (properties);
  this->properties = properties;

  pw_type_init (&this->type);

  this->loop = loop;

  impl->flush_event = pw_loop_add_event (loop, do_flush_event, impl);

  this->state = PW_CONTEXT_STATE_UNCONNECTED;

  pw_map_init (&this->objects, 64, 32);
  pw_map_init (&this->types, 64, 32);

  spa_list_init (&this->stream_list);
  spa_list_init (&this->global_list);
  spa_list_init (&this->proxy_list);

  pw_signal_init (&this->state_changed);
  pw_signal_init (&this->subscription);
  pw_signal_init (&this->destroy_signal);

  return this;

no_mem:
  free (this->name);
  free (impl);
  return NULL;
}

void
pw_context_destroy (struct pw_context *context)
{
  struct context *impl = SPA_CONTAINER_OF (context, struct context, this);
  struct pw_stream *stream, *t1;
  struct pw_proxy *proxy, *t2;

  pw_log_debug ("context %p: destroy", context);
  pw_signal_emit (&context->destroy_signal, context);

  pw_loop_destroy_source (impl->this.loop, impl->flush_event);

  if (context->state != PW_CONTEXT_STATE_UNCONNECTED)
    pw_context_disconnect (context);

  spa_list_for_each_safe (stream, t1, &context->stream_list, link)
    pw_stream_destroy (stream);
  spa_list_for_each_safe (proxy, t2, &context->proxy_list, link)
    pw_proxy_destroy (proxy);

  pw_map_clear (&context->objects);

  free (context->name);
  if (context->properties)
    pw_properties_free (context->properties);
  free (context->error);
  free (impl);
}

/**
 * pw_context_connect:
 * @context: a #struct pw_context
 *
 * Connect to the daemon
 *
 * Returns: %TRUE on success.
 */
bool
pw_context_connect (struct pw_context     *context,
                    enum pw_context_flags  flags)
{
  struct sockaddr_un addr;
  socklen_t size;
  const char *runtime_dir, *name = NULL;
  int name_size, fd;

  if ((runtime_dir = getenv ("XDG_RUNTIME_DIR")) == NULL) {
    context_set_state (context,
                       PW_CONTEXT_STATE_ERROR,
                       "connect failed: XDG_RUNTIME_DIR not set in the environment");
    return false;
  }

  if (name == NULL)
    name = getenv("PIPEWIRE_CORE");
  if (name == NULL)
    name = "pipewire-0";

  if ((fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
    return false;

  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_LOCAL;
  name_size = snprintf (addr.sun_path, sizeof (addr.sun_path),
                        "%s/%s", runtime_dir, name) + 1;

  if (name_size > (int)sizeof addr.sun_path) {
    pw_log_error ("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
                     runtime_dir, name);
    goto error_close;
  };

  size = offsetof (struct sockaddr_un, sun_path) + name_size;

  if (connect (fd, (struct sockaddr *) &addr, size) < 0) {
    context_set_state (context,
                       PW_CONTEXT_STATE_ERROR,
                       "connect failed: %s", strerror (errno));
    goto error_close;
  }

  return pw_context_connect_fd (context, flags, fd);

error_close:
  close (fd);
  return false;
}

/**
 * pw_context_connect_fd:
 * @context: a #struct pw_context
 * @fd: FD of a connected PipeWire socket
 *
 * Connect to a daemon. @fd should already be connected to a PipeWire socket.
 *
 * Returns: %TRUE on success.
 */
bool
pw_context_connect_fd (struct pw_context      *context,
                          enum pw_context_flags  flags,
                          int                fd)
{
  struct context *impl = SPA_CONTAINER_OF (context, struct context, this);

  context_set_state (context, PW_CONTEXT_STATE_CONNECTING, NULL);

  impl->connection = pw_connection_new (fd);
  if (impl->connection == NULL)
    goto error_close;

  context->protocol_private = impl->connection;

  pw_signal_add (&impl->connection->need_flush,
                 &impl->need_flush,
                 on_need_flush);

  impl->fd = fd;

  impl->source = pw_loop_add_io (context->loop,
                                 fd,
                                 SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR,
                                 false,
                                 on_context_data,
                                 impl);

  context->core_proxy = pw_proxy_new (context,
                                      0,
                                      context->type.core);
  if (context->core_proxy == NULL)
    goto no_proxy;

  context->core_proxy->implementation = &core_events;

  pw_core_do_client_update (context->core_proxy,
                            &context->properties->dict);

  if (!(flags & PW_CONTEXT_FLAG_NO_REGISTRY)) {
    context->registry_proxy = pw_proxy_new (context,
                                            SPA_ID_INVALID,
                                            context->type.registry);
    if (context->registry_proxy == NULL)
      goto no_registry;

    context->registry_proxy->implementation = &registry_events;

    pw_core_do_get_registry (context->core_proxy,
                             context->registry_proxy->id);

  }
  impl->no_proxy = !!(flags & PW_CONTEXT_FLAG_NO_PROXY);

  pw_core_do_sync (context->core_proxy, 0);

  return true;

no_registry:
  pw_proxy_destroy (context->core_proxy);
no_proxy:
  pw_loop_destroy_source (context->loop, impl->source);
  pw_connection_destroy (impl->connection);
error_close:
  close (fd);
  return false;
}

/**
 * pw_context_disconnect:
 * @context: a #struct pw_context
 *
 * Disonnect from the daemon.
 *
 * Returns: %TRUE on success.
 */
bool
pw_context_disconnect (struct pw_context *context)
{
  struct context *impl = SPA_CONTAINER_OF (context, struct context, this);

  impl->disconnecting = true;

  if (impl->source)
    pw_loop_destroy_source (context->loop, impl->source);
  impl->source = NULL;

  if (context->registry_proxy)
    pw_proxy_destroy (context->registry_proxy);
  context->registry_proxy = NULL;

  if (context->core_proxy)
    pw_proxy_destroy (context->core_proxy);
  context->core_proxy = NULL;

  if (impl->connection)
    pw_connection_destroy (impl->connection);
  impl->connection = NULL;
  context->protocol_private = NULL;

  if (impl->fd != -1)
    close (impl->fd);
  impl->fd = -1;

  context_set_state (context, PW_CONTEXT_STATE_UNCONNECTED, NULL);

  return true;
}

void
pw_context_get_core_info (struct pw_context   *context,
                          pw_core_info_cb_t    cb,
                          void                *user_data)
{
  struct pw_proxy *proxy;

  proxy = pw_map_lookup (&context->objects, 0);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->type.core && proxy->user_data) {
    struct pw_core_info *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}

typedef void (*list_func_t) (struct pw_context *, int, void *, void *);

static void
do_list (struct pw_context            *context,
         uint32_t                 type,
         list_func_t                 cb,
         void                    *user_data)
{
  union pw_map_item *item;

  pw_array_for_each (item, &context->objects.items) {
    struct pw_proxy *proxy;

    if (pw_map_item_is_free (item))
      continue;

    proxy = item->data;
    if (proxy->type != type)
      continue;

    if (proxy->user_data)
      cb (context, SPA_RESULT_OK, proxy->user_data, user_data);
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}


void
pw_context_list_module_info (struct pw_context   *context,
                             pw_module_info_cb_t  cb,
                             void                *user_data)
{
  do_list (context, context->type.module, (list_func_t) cb, user_data);
}

void
pw_context_get_module_info_by_id (struct pw_context   *context,
                                  uint32_t             id,
                                  pw_module_info_cb_t  cb,
                                  void                *user_data)
{
  struct pw_proxy *proxy;

  proxy = pw_map_lookup (&context->objects, id);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->type.module && proxy->user_data) {
    struct pw_module_info *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}

void
pw_context_list_client_info (struct pw_context   *context,
                             pw_client_info_cb_t  cb,
                             void                *user_data)
{
  do_list (context, context->type.client, (list_func_t) cb, user_data);
}

void
pw_context_get_client_info_by_id (struct pw_context   *context,
                                  uint32_t             id,
                                  pw_client_info_cb_t  cb,
                                  void                *user_data)
{
  struct pw_proxy *proxy;

  proxy = pw_map_lookup (&context->objects, id);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->type.client && proxy->user_data) {
    struct pw_client_info *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}

void
pw_context_list_node_info (struct pw_context *context,
                           pw_node_info_cb_t  cb,
                           void              *user_data)
{
  do_list (context, context->type.node, (list_func_t) cb, user_data);
}

void
pw_context_get_node_info_by_id (struct pw_context *context,
                                uint32_t           id,
                                pw_node_info_cb_t  cb,
                                void              *user_data)
{
  struct pw_proxy *proxy;

  proxy = pw_map_lookup (&context->objects, id);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->type.node && proxy->user_data) {
    struct pw_node_info *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}

void
pw_context_list_link_info (struct pw_context *context,
                           pw_link_info_cb_t  cb,
                           void              *user_data)
{
  do_list (context, context->type.link, (list_func_t) cb, user_data);
}

void
pw_context_get_link_info_by_id (struct pw_context *context,
                                uint32_t           id,
                                pw_link_info_cb_t  cb,
                                void              *user_data)
{
  struct pw_proxy *proxy;

  proxy = pw_map_lookup (&context->objects, id);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->type.link && proxy->user_data) {
    struct pw_link_info *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}