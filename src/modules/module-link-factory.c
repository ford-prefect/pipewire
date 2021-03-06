/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include <pipewire/pipewire.h>
#include "pipewire/private.h"

#define NAME "link-factory"

#define FACTORY_USAGE	PW_KEY_LINK_OUTPUT_NODE"=<output-node> "	\
			"["PW_KEY_LINK_OUTPUT_PORT"=<output-port>] "	\
			PW_KEY_LINK_INPUT_NODE"=<input-node "		\
			"["PW_KEY_LINK_INPUT_PORT"=<input-port>] "	\
			"["PW_KEY_OBJECT_LINGER"=<bool>] "		\
			"["PW_KEY_LINK_PASSIVE"=<bool>]"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Allow clients to create links" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_module *module;
	struct pw_factory *this;

	struct spa_list link_list;

	struct spa_hook module_listener;
};

struct link_data {
	struct factory_data *data;
	struct spa_list l;
	struct pw_link *link;
	struct spa_hook link_listener;
	struct pw_resource *resource;
	struct spa_hook resource_listener;
	struct pw_global *global;
	struct spa_hook global_listener;
};

static void resource_destroy(void *data)
{
	struct link_data *ld = data;
	spa_hook_remove(&ld->resource_listener);
	ld->resource = NULL;
	if (ld->global)
		pw_global_destroy(ld->global);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy
};

static void link_destroy(void *data)
{
	struct link_data *ld = data;
	spa_list_remove(&ld->l);
	if (ld->global)
		spa_hook_remove(&ld->global_listener);
	if (ld->resource)
		spa_hook_remove(&ld->resource_listener);
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.destroy = link_destroy
};

static void global_destroy(void *data)
{
	struct link_data *ld = data;
	spa_hook_remove(&ld->global_listener);
	ld->global = NULL;
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy
};

static struct pw_port *get_port(struct pw_node *node, enum spa_direction direction)
{
	struct pw_port *p;
	int res;

	p = pw_node_find_port(node, direction, SPA_ID_INVALID);

	if (p == NULL || pw_port_is_linked(p)) {
		uint32_t port_id;

		port_id = pw_node_get_free_port_id(node, direction);
		if (port_id == SPA_ID_INVALID)
			return NULL;

		p = pw_port_new(direction, port_id, NULL, 0);
		if (p == NULL)
			return NULL;

		if ((res = pw_port_add(p, node)) < 0) {
			pw_log_warn("can't add port: %s", spa_strerror(res));
			errno = -res;
			return NULL;
		}
	}
	return p;
}


static void *create_object(void *_data,
			   struct pw_resource *resource,
			   uint32_t type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *d = _data;
	struct pw_client *client = NULL;
	struct pw_node *output_node, *input_node;
	struct pw_port *outport, *inport;
	struct pw_core *core;
	struct pw_global *global;
	struct pw_link *link;
	uint32_t output_node_id, input_node_id;
	uint32_t output_port_id, input_port_id;
	struct link_data *ld;
	const char *str;
	int res;
	bool linger;

	client = pw_resource_get_client(resource);
	core = pw_client_get_core(client);

	if (properties == NULL)
		goto error_properties;

	if ((str = pw_properties_get(properties, PW_KEY_LINK_OUTPUT_NODE)) == NULL)
		goto error_properties;

	output_node_id = pw_properties_parse_int(str);

	if ((str = pw_properties_get(properties, PW_KEY_LINK_INPUT_NODE)) == NULL)
		goto error_properties;

	input_node_id = pw_properties_parse_int(str);

	str = pw_properties_get(properties, PW_KEY_LINK_OUTPUT_PORT);
	output_port_id = str ? pw_properties_parse_int(str) : -1;

	str = pw_properties_get(properties, PW_KEY_LINK_INPUT_PORT);
	input_port_id = str ? pw_properties_parse_int(str) : -1;

	global = pw_core_find_global(core, output_node_id);
	if (global == NULL || pw_global_get_type(global) != PW_TYPE_INTERFACE_Node)
		goto error_output;

	output_node = pw_global_get_object(global);

	global = pw_core_find_global(core, input_node_id);
	if (global == NULL || pw_global_get_type(global) != PW_TYPE_INTERFACE_Node)
		goto error_input;

	input_node = pw_global_get_object(global);

	if (output_port_id == SPA_ID_INVALID) {
		outport = get_port(output_node, SPA_DIRECTION_OUTPUT);
	}
	else {
		global = pw_core_find_global(core, output_port_id);
		if (global == NULL || pw_global_get_type(global) != PW_TYPE_INTERFACE_Port)
			goto error_output_port;

		outport = pw_global_get_object(global);
	}
	if (outport == NULL)
		goto error_output_port;

	if (input_port_id == SPA_ID_INVALID)
		inport = get_port(input_node, SPA_DIRECTION_INPUT);
	else {
		global = pw_core_find_global(core, input_port_id);
		if (global == NULL || pw_global_get_type(global) != PW_TYPE_INTERFACE_Port)
			goto error_input_port;

		inport = pw_global_get_object(global);
	}
	if (inport == NULL)
		goto error_input_port;

	str = pw_properties_get(properties, PW_KEY_OBJECT_LINGER);
	linger = str ? pw_properties_parse_bool(str) : false;

	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d", d->this->global->id);
	if (!linger)
		pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d", client->global->id);


	link = pw_link_new(core, outport, inport, NULL, properties, sizeof(struct link_data));
	properties = NULL;
	if (link == NULL) {
		res = -errno;
		goto error_create_link;
	}

	ld = pw_link_get_user_data(link);
	ld->data = d;
	ld->link = link;
	spa_list_append(&d->link_list, &ld->l);

	pw_link_add_listener(link, &ld->link_listener, &link_events, ld);
	if ((res = pw_link_register(link, NULL)) < 0)
		goto error_link_register;

	ld->global = pw_link_get_global(link);
	pw_global_add_listener(ld->global, &ld->global_listener, &global_events, ld);

	res = pw_global_bind(ld->global, client, PW_PERM_RWX, PW_VERSION_LINK_PROXY, new_id);
	if (res < 0)
		goto error_bind;

	if (!linger) {
		ld->resource = pw_client_find_resource(client, new_id);
		if (ld->resource == NULL) {
			res = -ENOENT;
			goto error_bind;
		}
		pw_resource_add_listener(ld->resource, &ld->resource_listener, &resource_events, ld);
	}

	return link;

error_properties:
	res = -EINVAL;
	pw_log_error("link-factory usage:" FACTORY_USAGE);
	pw_resource_error(resource, res, "no properties");
	goto error_exit;
error_output:
	res = -EINVAL;
	pw_log_error("link-factory unknown output node %u", output_node_id);
	pw_resource_error(resource, res, "unknown output node %u", output_node_id);
	goto error_exit;
error_input:
	res = -EINVAL;
	pw_log_error("link-factory unknown input node %u", input_node_id);
	pw_resource_error(resource, res, "unknown input node %u", input_node_id);
	goto error_exit;
error_output_port:
	res = -EINVAL;
	pw_log_error("link-factory unknown output port %u", output_port_id);
	pw_resource_error(resource, res, "unknown output port %u", output_port_id);
	goto error_exit;
error_input_port:
	res = -EINVAL;
	pw_log_error("link-factory unknown input port %u", input_port_id);
	pw_resource_error(resource, res, "unknown input port %u", input_port_id);
	goto error_exit;
error_create_link:
	pw_log_error("can't create link: %s", spa_strerror(res));
	pw_resource_error(resource, res, "can't create link: %s", spa_strerror(res));
	goto error_exit;
error_link_register:
	pw_log_error("can't register link: %s", spa_strerror(res));
	pw_resource_error(resource, res, "can't register link: %s", spa_strerror(res));
	goto error_exit;
error_bind:
	pw_resource_error(resource, res, "can't bind link: %s", spa_strerror(res));
	goto error_exit;
error_exit:
	if (properties)
		pw_properties_free(properties);
	errno = -res;
	return NULL;
}

static const struct pw_factory_implementation impl_factory = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;
	struct link_data *ld, *t;

	spa_hook_remove(&d->module_listener);

	spa_list_for_each_safe(ld, t, &d->link_list, l)
		pw_link_destroy(ld->link);

	pw_factory_destroy(d->this);
}

static void module_registered(void *data)
{
	struct factory_data *d = data;
	struct pw_module *module = d->module;
	struct pw_factory *factory = d->this;
	struct spa_dict_item items[1];
	char id[16];
	int res;

	snprintf(id, sizeof(id), "%d", pw_global_get_id(pw_module_get_global(module)));
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MODULE_ID, id);
	pw_factory_update_properties(factory, &SPA_DICT_INIT(items, 1));

	if ((res = pw_factory_register(factory, NULL)) < 0) {
		pw_log_error(NAME" %p: can't register factory: %s", factory, spa_strerror(res));
	}
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
	.registered = module_registered,
};

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_factory *factory;
	struct factory_data *data;

	factory = pw_factory_new(core,
				 "link-factory",
				 PW_TYPE_INTERFACE_Link,
				 PW_VERSION_LINK_PROXY,
				 pw_properties_new(
					 PW_KEY_FACTORY_USAGE, FACTORY_USAGE,
					 NULL),
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_factory_get_user_data(factory);
	data->this = factory;
	data->module = module;
	spa_list_init(&data->link_list);

	pw_log_debug("module %p: new", module);

	pw_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	pw_module_add_listener(module, &data->module_listener, &module_events, data);

	return 0;
}
