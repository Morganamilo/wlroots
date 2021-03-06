#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include "types/wlr_seat.h"
#include "util/signal.h"

static void seat_handle_get_pointer(struct wl_client *client,
		struct wl_resource *seat_resource, uint32_t id) {
	struct wlr_seat_client *seat_client =
		wlr_seat_client_from_resource(seat_resource);
	if (!(seat_client->seat->capabilities & WL_SEAT_CAPABILITY_POINTER)) {
		return;
	}

	uint32_t version = wl_resource_get_version(seat_resource);
	seat_client_create_pointer(seat_client, version, id);
}

static void seat_handle_get_keyboard(struct wl_client *client,
		struct wl_resource *seat_resource, uint32_t id) {
	struct wlr_seat_client *seat_client =
		wlr_seat_client_from_resource(seat_resource);
	if (!(seat_client->seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
		return;
	}

	uint32_t version = wl_resource_get_version(seat_resource);
	seat_client_create_keyboard(seat_client, version, id);
}

static void seat_handle_get_touch(struct wl_client *client,
		struct wl_resource *seat_resource, uint32_t id) {
	struct wlr_seat_client *seat_client =
		wlr_seat_client_from_resource(seat_resource);
	if (!(seat_client->seat->capabilities & WL_SEAT_CAPABILITY_TOUCH)) {
		return;
	}

	uint32_t version = wl_resource_get_version(seat_resource);
	seat_client_create_touch(seat_client, version, id);
}

static void seat_client_resource_destroy(struct wl_resource *seat_resource) {
	struct wlr_seat_client *client =
		wlr_seat_client_from_resource(seat_resource);
	wlr_signal_emit_safe(&client->events.destroy, client);

	if (client == client->seat->pointer_state.focused_client) {
		client->seat->pointer_state.focused_client = NULL;
	}
	if (client == client->seat->keyboard_state.focused_client) {
		client->seat->keyboard_state.focused_client = NULL;
	}

	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &client->pointers) {
		wl_resource_destroy(resource);
	}
	wl_resource_for_each_safe(resource, tmp, &client->keyboards) {
		wl_resource_destroy(resource);
	}
	wl_resource_for_each_safe(resource, tmp, &client->touches) {
		wl_resource_destroy(resource);
	}
	wl_resource_for_each_safe(resource, tmp, &client->data_devices) {
		wl_resource_destroy(resource);
	}
	wl_resource_for_each_safe(resource, tmp,
			&client->primary_selection_devices) {
		wl_resource_destroy(resource);
	}

	wl_list_remove(&client->link);
	free(client);
}

static void seat_handle_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_handle_get_pointer,
	.get_keyboard = seat_handle_get_keyboard,
	.get_touch = seat_handle_get_touch,
	.release = seat_handle_release,
};

static void seat_handle_bind(struct wl_client *client, void *_wlr_seat,
		uint32_t version, uint32_t id) {
	struct wlr_seat *wlr_seat = _wlr_seat;
	assert(client && wlr_seat);

	struct wlr_seat_client *seat_client =
		calloc(1, sizeof(struct wlr_seat_client));
	if (seat_client == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	seat_client->wl_resource =
		wl_resource_create(client, &wl_seat_interface, version, id);
	if (seat_client->wl_resource == NULL) {
		free(seat_client);
		wl_client_post_no_memory(client);
		return;
	}
	seat_client->client = client;
	seat_client->seat = wlr_seat;
	wl_list_init(&seat_client->pointers);
	wl_list_init(&seat_client->keyboards);
	wl_list_init(&seat_client->touches);
	wl_list_init(&seat_client->data_devices);
	wl_list_init(&seat_client->primary_selection_devices);
	wl_resource_set_implementation(seat_client->wl_resource, &seat_impl,
		seat_client, seat_client_resource_destroy);
	wl_list_insert(&wlr_seat->clients, &seat_client->link);
	if (version >= WL_SEAT_NAME_SINCE_VERSION) {
		wl_seat_send_name(seat_client->wl_resource, wlr_seat->name);
	}
	wl_seat_send_capabilities(seat_client->wl_resource, wlr_seat->capabilities);
	wl_signal_init(&seat_client->events.destroy);
}

void wlr_seat_destroy(struct wlr_seat *seat) {
	if (!seat) {
		return;
	}

	wlr_signal_emit_safe(&seat->events.destroy, seat);

	wl_list_remove(&seat->display_destroy.link);

	if (seat->selection_source) {
		wl_list_remove(&seat->selection_source_destroy.link);
		wlr_data_source_cancel(seat->selection_source);
		seat->selection_source = NULL;
	}
	if (seat->primary_selection_source) {
		seat->primary_selection_source->cancel(seat->primary_selection_source);
		seat->primary_selection_source = NULL;
		wl_list_remove(&seat->primary_selection_source_destroy.link);
	}

	struct wlr_seat_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &seat->clients, link) {
		// will destroy other resources as well
		wl_resource_destroy(client->wl_resource);
	}

	wl_global_destroy(seat->wl_global);
	free(seat->pointer_state.default_grab);
	free(seat->keyboard_state.default_grab);
	free(seat->touch_state.default_grab);
	free(seat->name);
	free(seat);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_seat *seat =
		wl_container_of(listener, seat, display_destroy);
	wlr_seat_destroy(seat);
}

struct wlr_seat *wlr_seat_create(struct wl_display *display, const char *name) {
	struct wlr_seat *wlr_seat = calloc(1, sizeof(struct wlr_seat));
	if (!wlr_seat) {
		return NULL;
	}

	// pointer state
	wlr_seat->pointer_state.seat = wlr_seat;
	wl_list_init(&wlr_seat->pointer_state.surface_destroy.link);

	struct wlr_seat_pointer_grab *pointer_grab =
		calloc(1, sizeof(struct wlr_seat_pointer_grab));
	if (!pointer_grab) {
		free(wlr_seat);
		return NULL;
	}
	pointer_grab->interface = &default_pointer_grab_impl;
	pointer_grab->seat = wlr_seat;
	wlr_seat->pointer_state.default_grab = pointer_grab;
	wlr_seat->pointer_state.grab = pointer_grab;

	// keyboard state
	struct wlr_seat_keyboard_grab *keyboard_grab =
		calloc(1, sizeof(struct wlr_seat_keyboard_grab));
	if (!keyboard_grab) {
		free(pointer_grab);
		free(wlr_seat);
		return NULL;
	}
	keyboard_grab->interface = &default_keyboard_grab_impl;
	keyboard_grab->seat = wlr_seat;
	wlr_seat->keyboard_state.default_grab = keyboard_grab;
	wlr_seat->keyboard_state.grab = keyboard_grab;

	wlr_seat->keyboard_state.seat = wlr_seat;
	wl_list_init(&wlr_seat->keyboard_state.surface_destroy.link);

	// touch state
	struct wlr_seat_touch_grab *touch_grab =
		calloc(1, sizeof(struct wlr_seat_touch_grab));
	if (!touch_grab) {
		free(pointer_grab);
		free(keyboard_grab);
		free(wlr_seat);
		return NULL;
	}
	touch_grab->interface = &default_touch_grab_impl;
	touch_grab->seat = wlr_seat;
	wlr_seat->touch_state.default_grab = touch_grab;
	wlr_seat->touch_state.grab = touch_grab;

	wlr_seat->touch_state.seat = wlr_seat;
	wl_list_init(&wlr_seat->touch_state.touch_points);

	struct wl_global *wl_global = wl_global_create(display,
		&wl_seat_interface, 6, wlr_seat, seat_handle_bind);
	if (!wl_global) {
		free(wlr_seat);
		return NULL;
	}
	wlr_seat->wl_global = wl_global;
	wlr_seat->display = display;
	wlr_seat->name = strdup(name);
	wl_list_init(&wlr_seat->clients);
	wl_list_init(&wlr_seat->drag_icons);

	wl_signal_init(&wlr_seat->events.start_drag);
	wl_signal_init(&wlr_seat->events.new_drag_icon);

	wl_signal_init(&wlr_seat->events.request_set_cursor);

	wl_signal_init(&wlr_seat->events.selection);
	wl_signal_init(&wlr_seat->events.primary_selection);

	wl_signal_init(&wlr_seat->events.pointer_grab_begin);
	wl_signal_init(&wlr_seat->events.pointer_grab_end);

	wl_signal_init(&wlr_seat->events.keyboard_grab_begin);
	wl_signal_init(&wlr_seat->events.keyboard_grab_end);

	wl_signal_init(&wlr_seat->events.touch_grab_begin);
	wl_signal_init(&wlr_seat->events.touch_grab_end);

	wl_signal_init(&wlr_seat->events.destroy);

	wlr_seat->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &wlr_seat->display_destroy);

	return wlr_seat;
}

struct wlr_seat_client *wlr_seat_client_for_wl_client(struct wlr_seat *wlr_seat,
		struct wl_client *wl_client) {
	assert(wlr_seat);
	struct wlr_seat_client *seat_client;
	wl_list_for_each(seat_client, &wlr_seat->clients, link) {
		if (seat_client->client == wl_client) {
			return seat_client;
		}
	}
	return NULL;
}

void wlr_seat_set_capabilities(struct wlr_seat *wlr_seat,
		uint32_t capabilities) {
	wlr_seat->capabilities = capabilities;
	struct wlr_seat_client *client;
	wl_list_for_each(client, &wlr_seat->clients, link) {
		wl_seat_send_capabilities(client->wl_resource, capabilities);
	}
}

void wlr_seat_set_name(struct wlr_seat *wlr_seat, const char *name) {
	free(wlr_seat->name);
	wlr_seat->name = strdup(name);
	struct wlr_seat_client *client;
	wl_list_for_each(client, &wlr_seat->clients, link) {
		wl_seat_send_name(client->wl_resource, name);
	}
}

struct wlr_seat_client *wlr_seat_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_seat_interface,
		&seat_impl));
	return wl_resource_get_user_data(resource);
}

bool wlr_seat_validate_grab_serial(struct wlr_seat *seat, uint32_t serial) {
	// TODO
	//return serial == seat->pointer_state.grab_serial ||
	//	serial == seat->touch_state.grab_serial;
	return true;
}
