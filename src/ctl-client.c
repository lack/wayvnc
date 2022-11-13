/*
 * Copyright (c) 2022 Jim Ramsay
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <signal.h>
#include <jansson.h>

#include "json-ipc.h"
#include "ctl-client.h"
#include "ctl-server.h"
#include "strlcpy.h"
#include "util.h"

#define WARN(fmt, ...) fprintf(stderr, "[WARNING] " fmt "\n", ##__VA_ARGS__)

static bool do_debug = false;

#define DEBUG(fmt, ...) \
	if (do_debug) \
		fprintf(stderr, "[%s:%d] " fmt "\n", __FILE__, __LINE__, \
			##__VA_ARGS__);

#define FAILED_TO(action) WARN("Failed to " action ": %m");

struct ctl_client {
	void* userdata;

	char read_buffer[512];
	size_t read_len;

	bool wait_for_events;

	int fd;
};

void ctl_client_debug_log(bool enable)
{
	do_debug = enable;
}

struct ctl_client* ctl_client_new(const char* socket_path, void* userdata)
{
	if (!socket_path)
		socket_path = default_ctl_socket_path();
	struct ctl_client* new = calloc(1, sizeof(*new));
	new->userdata = userdata;

	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};

	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		FAILED_TO("create unix socket");
		goto socket_failure;
	}
	strcpy(addr.sun_path, socket_path);

	new->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (new->fd < 0) {
		FAILED_TO("create unix socket");
		goto socket_failure;
	}

	if (connect(new->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		FAILED_TO("connect to unix socket");
		goto connect_failure;
	}

	return new;

connect_failure:
	close(new->fd);
socket_failure:
	free(new);
	return NULL;
}

void ctl_client_destroy(struct ctl_client* self)
{
	close(self->fd);
	free(self);
}

void* ctl_client_userdata(struct ctl_client* self)
{
	return self->userdata;
}

static struct jsonipc_request* ctl_client_parse_args(struct ctl_client* self,
						     int argc, char* argv[])
{
	struct jsonipc_request* request = NULL;
	const char* method = argv[0];
	json_t* params = json_object();
	bool show_usage = false;
	for (int i = 1; i < argc; ++i) {
		char* key = argv[i];
		char* value = NULL;
		if (strcmp(key, "--help") == 0 || strcmp(key, "-h") == 0) {
			show_usage = true;
			continue;
		}
		if (key[0] == '-' && key[1] == '-')
			key += 2;
		char* delim = strchr(key, '=');
		if (delim) {
			*delim = '\0';
			value = delim + 1;
		} else if (++i < argc) {
			value = argv[i];
		} else {
			WARN("Argument must be of the format --key=value or --key value");
			goto failure;
		}
		json_object_set_new(params, key, json_string(value));
	}
	if (show_usage) {
		// Special case for "foo --help"; convert into "help --command=foo"
		json_object_clear(params);
		json_object_set_new(params, "command", json_string(method));
		method = "help";
	}
	request = jsonipc_request_new(method, params);

failure:
	json_decref(params);
	return request;
}

static ssize_t ctl_client_send_request(struct ctl_client* self,
				       struct jsonipc_request* request)
{
	json_error_t err;
	json_t* packed = jsonipc_request_pack(request, &err);
	if (!packed) {
		WARN("Could not encode json: %s", err.text);
		return -1;
	}
	char buffer[512];
	int len = json_dumpb(packed, buffer, sizeof(buffer), JSON_COMPACT);
	json_decref(packed);
	DEBUG(">> %.*s", len, buffer);
	return send(self->fd, buffer, len, MSG_NOSIGNAL);
}

static json_t* json_from_buffer(struct ctl_client* self)
{
	if (self->read_len == 0) {
		DEBUG("Read buffer is empty");
		errno = ENODATA;
		return NULL;
	}
	json_error_t err;
	json_t* root = json_loadb(self->read_buffer, self->read_len, 0, &err);
	if (root) {
		advance_read_buffer(&self->read_buffer, &self->read_len,
				    err.position);
	} else if (json_error_code(&err) == json_error_premature_end_of_input) {
		DEBUG("Awaiting more data");
		errno = ENODATA;
	} else {
		WARN("Json parsing failed: %s", err.text);
		errno = EINVAL;
	}
	return root;
}

static json_t* read_one_object(struct ctl_client* self, int timeout_ms)
{
	json_t* root = json_from_buffer(self);
	if (root)
		return root;
	if (errno != ENODATA)
		return NULL;
	struct pollfd pfd = { .fd = self->fd, .events = POLLIN, .revents = 0 };
	while (root == NULL) {
		int n = poll(&pfd, 1, timeout_ms);
		if (n == -1) {
			if (errno == EINTR && self->wait_for_events)
				continue;
			WARN("Error waiting for a response: %m");
			break;
		} else if (n == 0) {
			WARN("Timeout waiting for a response");
			break;
		}
		char* readptr = self->read_buffer + self->read_len;
		size_t remainder = sizeof(self->read_buffer) - self->read_len;
		n = recv(self->fd, readptr, remainder, 0);
		if (n == -1) {
			WARN("Read failed: %m");
			break;
		} else if (n == 0) {
			WARN("Disconnected");
			errno = ECONNRESET;
			break;
		}
		DEBUG("Read %d bytes", n);
		DEBUG("<< %.*s", n, readptr);
		self->read_len += n;
		root = json_from_buffer(self);
		if (!root && errno != ENODATA)
			break;
	}
	return root;
}

static struct jsonipc_response* ctl_client_wait_for_response(
	struct ctl_client* self)
{
	DEBUG("Waiting for a response");
	json_t* root = read_one_object(self, 1000);
	if (!root)
		return NULL;
	struct jsonipc_error jipc_err = JSONIPC_ERR_INIT;
	struct jsonipc_response* response =
		jsonipc_response_parse_new(root, &jipc_err);
	if (!response) {
		char* msg = json_dumps(jipc_err.data, JSON_EMBED);
		WARN("Could not parse json: %s", msg);
		free(msg);
	}
	json_decref(root);
	jsonipc_error_cleanup(&jipc_err);
	return response;
}

static void print_error(struct jsonipc_response* response, const char* method)
{
	printf("Error (%d)", response->code);
	if (!response->data)
		goto out;
	json_t* data = response->data;
	if (json_is_string(data))
		printf(": %s", json_string_value(data));
	else if (json_is_object(data)
		 && json_is_string(json_object_get(data, "error")))
		printf(": %s",
		       json_string_value(json_object_get(data, "error")));
	else
		json_dumpf(response->data, stdout, JSON_INDENT(2));
out:
	printf("\n");
}

static void print_command_usage(const char* name, json_t* data)
{
	char* desc = NULL;
	json_t* params = NULL;
	json_unpack(data, "{s:s, s?o}", "description", &desc, "params",
		    &params);
	printf("Usage: wayvncctl [options] %s%s\n\n%s\n", name,
	       params ? " [params]" : "", desc);
	if (params) {
		printf("\nParameters:");
		const char* param_name;
		json_t* param_value;
		json_object_foreach (params, param_name, param_value) {
			printf("\n  --%s=...\n    %s\n", param_name,
			       json_string_value(param_value));
		}
	}
	printf("\nRun 'wayvncctl --help' for allowed Options\n");
}

static void print_event_details(const char* name, json_t* data)
{
	char* desc = NULL;
	json_t* params = NULL;
	json_unpack(data, "{s:s, s?o}", "description", &desc, "params",
		    &params);
	printf("Event: %s\n\n%s\n", name, desc);
	if (params) {
		printf("\nParameters:");
		const char* param_name;
		json_t* param_value;
		json_object_foreach (params, param_name, param_value) {
			printf("\n  %s:...\n    %s\n", param_name,
			       json_string_value(param_value));
		}
	}
}

static void print_help(json_t* data, json_t* request)
{
	if (json_object_get(data, "commands")) {
		printf("Allowed commands:\n");
		json_t* cmd_list = json_object_get(data, "commands");

		size_t index;
		json_t* value;
		json_array_foreach (cmd_list, index, value) {
			printf("  - %s\n", json_string_value(value));
		}
		printf("\nRun 'wayvncctl command-name --help' for command-specific details.\n");

		printf("\nSupported events:\n");
		json_t* evt_list = json_object_get(data, "events");
		json_array_foreach (evt_list, index, value) {
			printf("  - %s\n", json_string_value(value));
		}
		printf("\nRun 'wayvncctl help --event=event-name' for event-specific details.\n");
		return;
	}

	bool is_command = json_object_get(request, "command");
	const char* key;
	json_t* value;
	json_object_foreach (data, key, value) {
		if (is_command)
			print_command_usage(key, value);
		else
			print_event_details(key, value);
	}
}

static void pretty_version(json_t* data)
{
	printf("wayvnc is running:\n");
	const char* key;
	json_t* value;
	json_object_foreach (data, key, value)
		printf("  %s: %s\n", key, json_string_value(value));
}

static void pretty_print(json_t* data, struct jsonipc_request* request)
{
	const char* method = request->method;
	if (strcmp(method, "help") == 0)
		print_help(data, request->params);
	else if (strcmp(method, "version") == 0)
		pretty_version(data);
	else
		json_dumpf(data, stdout, JSON_INDENT(2));
}

static void print_compact_json(json_t* data)
{
	json_dumpf(data, stdout, JSON_COMPACT);
	printf("\n");
}

static int ctl_client_print_response(struct ctl_client* self,
				     struct jsonipc_request* request,
				     struct jsonipc_response* response,
				     unsigned flags)
{
	DEBUG("Response code: %d", response->code);
	if (response->data) {
		if (flags & PRINT_JSON)
			print_compact_json(response->data);
		else if (response->code == 0)
			pretty_print(response->data, request);
		else
			print_error(response, request->method);
	}
	return response->code;
}

static struct ctl_client* sig_target = NULL;
static void stop_loop(int signal)
{
	sig_target->wait_for_events = false;
}

static void setup_signals(struct ctl_client* self)
{
	sig_target = self;
	struct sigaction sa = { 0 };
	sa.sa_handler = stop_loop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static void print_indent(int level)
{
	for (int i = 0; i < level; ++i)
		printf("  ");
}

static bool json_has_content(json_t* root)
{
	if (!root)
		return false;
	size_t i;
	const char* key;
	json_t* value;
	switch (json_typeof(root)) {
	case JSON_NULL:
		return false;
	case JSON_INTEGER:
	case JSON_REAL:
	case JSON_TRUE:
	case JSON_FALSE:
		return true;
	case JSON_STRING:
		return json_string_value(root)[0] != '\0';
	case JSON_OBJECT:
		json_object_foreach (root, key, value)
			if (json_has_content(value))
				return true;
		return false;
	case JSON_ARRAY:
		json_array_foreach (root, i, value)
			if (json_has_content(value))
				return true;
		return false;
	}
	return false;
}

static void print_as_yaml(json_t* data, int level, bool needs_leading_newline)
{
	size_t i;
	const char* key;
	json_t* value;
	bool needs_indent = needs_leading_newline;
	switch (json_typeof(data)) {
	case JSON_NULL:
		printf("<null>\n");
		break;
	case JSON_OBJECT:
		if (json_object_size(data) > 0 && needs_leading_newline)
			printf("\n");
		json_object_foreach (data, key, value) {
			if (!json_has_content(value))
				continue;
			if (needs_indent)
				print_indent(level);
			else
				needs_indent = true;
			printf("%s: ", key);
			print_as_yaml(value, level + 1, true);
		}
		break;
	case JSON_ARRAY:
		if (json_array_size(data) > 0 && needs_leading_newline)
			printf("\n");
		json_array_foreach (data, i, value) {
			if (!json_has_content(value))
				continue;
			print_indent(level);
			printf("- ");
			print_as_yaml(value, level + 1, json_is_array(value));
		}
		break;
	case JSON_STRING:
		printf("%s\n", json_string_value(data));
		break;
	case JSON_INTEGER:
		printf("%" JSON_INTEGER_FORMAT "\n", json_integer_value(data));
		break;
	case JSON_REAL:
		printf("%f\n", json_real_value(data));
		break;
	case JSON_TRUE:
		printf("true\n");
		break;
	case JSON_FALSE:
		printf("false\n");
		break;
	}
}

static void print_event(struct jsonipc_request* event, unsigned flags)
{
	if (flags & PRINT_JSON) {
		print_compact_json(event->json);
	} else {
		printf("\n%s:", event->method);
		print_as_yaml(event->params, 1, true);
	}
	fflush(stdout);
}

static void ctl_client_event_loop(struct ctl_client* self, unsigned flags)
{
	self->wait_for_events = true;
	setup_signals(self);
	while (self->wait_for_events) {
		DEBUG("Waiting for an event");
		json_t* root = read_one_object(self, -1);
		if (!root)
			break;
		struct jsonipc_error err = JSONIPC_ERR_INIT;
		struct jsonipc_request* event =
			jsonipc_event_parse_new(root, &err);
		json_decref(root);
		print_event(event, flags);
		jsonipc_request_destroy(event);
	}
}

int ctl_client_run_command(struct ctl_client* self, int argc, char* argv[],
			   unsigned flags)
{
	int result = -1;
	struct jsonipc_request* request =
		ctl_client_parse_args(self, argc, argv);
	if (!request)
		goto parse_failure;

	if (ctl_client_send_request(self, request) < 0)
		goto send_failure;

	struct jsonipc_response* response = ctl_client_wait_for_response(self);
	if (!response)
		goto receive_failure;

	result = ctl_client_print_response(self, request, response, flags);

	if (result == 0 && strcmp(request->method, "event-receive") == 0)
		ctl_client_event_loop(self, flags);

	jsonipc_response_destroy(response);
receive_failure:
send_failure:
	jsonipc_request_destroy(request);
parse_failure:
	return result;
}