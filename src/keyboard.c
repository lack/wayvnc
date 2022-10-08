/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
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
 *
 * Acknowledgements: Reading Josef Gajdusek's wvnc code helped me understand
 * how to use the xkbcommon API to interface with the wayland virtual keyboard
 * interface.
 */

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>
#include <neatvnc.h>

#include "virtual-keyboard-unstable-v1.h"
#include "keyboard.h"
#include "shm.h"
#include "intset.h"

#define MAYBE_UNUSED __attribute__((unused))

struct table_entry {
	xkb_keysym_t symbol;
	xkb_keycode_t code;
	int level;
};

struct kb_mods {
	xkb_mod_mask_t depressed, latched, locked;
};

static void append_entry(struct keyboard* self, xkb_keysym_t symbol,
                         xkb_keycode_t code, int level)
{
	if (self->lookup_table_size <= self->lookup_table_length) {
		size_t new_size = self->lookup_table_size * 2;
		struct table_entry* table =
			realloc(self->lookup_table, new_size * sizeof(*table));
		if (!table)
			return; // TODO: Report this

		self->lookup_table_size = new_size;
		self->lookup_table = table;
	}

	struct table_entry* entry =
		&self->lookup_table[self->lookup_table_length++];

	entry->symbol = symbol;
	entry->code = code;
	entry->level = level;
}

static void key_iter(struct xkb_keymap* map, xkb_keycode_t code, void* userdata)
{
	struct keyboard* self = userdata;

	size_t n_levels = xkb_keymap_num_levels_for_key(map, code, 0);

	for (size_t level = 0; level < n_levels; level++) {
		const xkb_keysym_t* symbols;
		size_t n_syms = xkb_keymap_key_get_syms_by_level(map, code, 0,
				                                 level,
				                                 &symbols);

		for (size_t sym_idx = 0; sym_idx < n_syms; sym_idx++)
			append_entry(self, symbols[sym_idx], code, level);
	}
}

static int compare_symbols(const void* a, const void* b)
{
	const struct table_entry* x = a;
	const struct table_entry* y = b;

	if (x->symbol == y->symbol)
		return x->code < y->code ? -1 : x->code > y->code;

	return x->symbol < y->symbol ? -1 : x->symbol > y->symbol;
}

static int compare_symbols2(const void* a, const void* b)
{
	const struct table_entry* x = a;
	const struct table_entry* y = b;

	return x->symbol < y->symbol ? -1 : x->symbol > y->symbol;
}

static int create_lookup_table(struct keyboard* self)
{
	self->lookup_table_length = 0;
	self->lookup_table_size = 128;

	self->lookup_table =
		malloc(self->lookup_table_size * sizeof(*self->lookup_table));
	if (!self->lookup_table)
		return -1;

	xkb_keymap_key_for_each(self->keymap, key_iter, self);

	qsort(self->lookup_table, self->lookup_table_length,
	      sizeof(*self->lookup_table), compare_symbols);

	return 0;
}

static char* get_symbol_name(xkb_keysym_t sym, char* dst, size_t size)
{
	if (xkb_keysym_get_name(sym, dst, size) >= 0)
		return dst;

	snprintf(dst, size, "UNKNOWN (%x)", sym);
	return dst;
}

static void keyboard__dump_entry(const struct keyboard* self,
                                 const struct table_entry* entry)
{
	char sym_name[256];
	get_symbol_name(entry->symbol, sym_name, sizeof(sym_name));

	const char* code_name MAYBE_UNUSED =
		xkb_keymap_key_get_name(self->keymap, entry->code);

	bool is_pressed MAYBE_UNUSED =
		intset_is_set(&self->key_state, entry->code);

	nvnc_log(NVNC_LOG_DEBUG, "symbol=%s level=%d code=%s %s", sym_name, entry->level,
	          code_name, is_pressed ? "pressed" : "released");
}

void keyboard_dump_lookup_table(const struct keyboard* self)
{
	for (size_t i = 0; i < self->lookup_table_length; i++)
		keyboard__dump_entry(self, &self->lookup_table[i]);
}

int keyboard_init(struct keyboard* self, const struct xkb_rule_names* rule_names)
{
	self->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!self->context)
		return -1;

	if (intset_init(&self->key_state, 0) < 0)
		goto key_state_failure;

	if (intset_init(&self->active_keybinds, 0) < 0)
		goto active_keybinds_failure;

	wl_list_init(&self->keybinds);

	self->keymap = xkb_keymap_new_from_names(self->context, rule_names, 0);
	if (!self->keymap)
		goto keymap_failure;

	if (xkb_keymap_num_layouts(self->keymap) > 1)
		nvnc_log(NVNC_LOG_WARNING, "Multiple keyboard layouts have been specified, but only one is supported.");

	self->state = xkb_state_new(self->keymap);
	if (!self->state)
		goto state_failure;

	if (create_lookup_table(self) < 0)
		goto table_failure;

//	keyboard_dump_lookup_table(self);

	char* keymap_string =
		xkb_keymap_get_as_string(self->keymap,
		                         XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!keymap_string)
		goto keymap_string_failure;

	size_t keymap_size = strlen(keymap_string) + 1;

	int keymap_fd = shm_alloc_fd(keymap_size);
	if (keymap_fd < 0)
		goto fd_failure;

	size_t written = 0;
	while (written < keymap_size) {
		ssize_t ret = write(keymap_fd, keymap_string + written, keymap_size - written);
		if (ret == -1 && errno == EINTR)
			continue;
		if (ret == -1)
			goto write_failure;
		written += ret;
	}

	free(keymap_string);

	zwp_virtual_keyboard_v1_keymap(self->virtual_keyboard,
	                               WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
	                               keymap_fd, keymap_size);

	close(keymap_fd);

	return 0;

write_failure:
	close(keymap_fd);
fd_failure:
	free(keymap_string);
keymap_string_failure:
	free(self->lookup_table);
table_failure:
	xkb_state_unref(self->state);
state_failure:
	xkb_keymap_unref(self->keymap);
keymap_failure:
	intset_destroy(&self->active_keybinds);
active_keybinds_failure:
	intset_destroy(&self->key_state);
key_state_failure:
	xkb_context_unref(self->context);
	return -1;
}

void keybind_remove(struct keybind*);

void keyboard_destroy(struct keyboard* self)
{
	free(self->lookup_table);
	xkb_state_unref(self->state);
	xkb_keymap_unref(self->keymap);
	intset_destroy(&self->key_state);
	intset_destroy(&self->active_keybinds);
	struct keybind* keybind;
	struct keybind* temp;
	wl_list_for_each_safe(keybind, temp, &self->keybinds, link)
		keybind_remove(keybind);
	xkb_context_unref(self->context);
}

struct table_entry* keyboard_find_symbol(const struct keyboard* self,
                                         xkb_keysym_t symbol)
{
	struct table_entry cmp = { .symbol = symbol };

	struct table_entry* entry =
		bsearch(&cmp, self->lookup_table, self->lookup_table_length,
		        sizeof(*self->lookup_table), compare_symbols2);

	if (!entry)
		return NULL;

	while (entry != self->lookup_table && (entry - 1)->symbol == symbol)
		--entry;

	return entry;
}

static void keyboard_send_mods(struct keyboard* self)
{
	xkb_mod_mask_t depressed, latched, locked, group;

	depressed = xkb_state_serialize_mods(self->state, XKB_STATE_MODS_DEPRESSED);
	latched = xkb_state_serialize_mods(self->state, XKB_STATE_MODS_LATCHED);
	locked = xkb_state_serialize_mods(self->state, XKB_STATE_MODS_LOCKED);
	group = xkb_state_serialize_mods(self->state, XKB_STATE_MODS_EFFECTIVE);

	zwp_virtual_keyboard_v1_modifiers(self->virtual_keyboard, depressed,
	                                  latched, locked, group);
}

static void keyboard_apply_mods(struct keyboard* self, xkb_keycode_t code,
                                bool is_pressed)
{
	enum xkb_state_component comp, compmask;

	comp = xkb_state_update_key(self->state, code,
	                            is_pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

	compmask = XKB_STATE_MODS_DEPRESSED |
	           XKB_STATE_MODS_LATCHED |
	           XKB_STATE_MODS_LOCKED |
	           XKB_STATE_MODS_EFFECTIVE;

	if (!(comp & compmask))
		return;

	keyboard_send_mods(self);
}

static struct table_entry* match_level(struct keyboard* self,
                                       struct table_entry* entry)
{
	xkb_keysym_t symbol = entry->symbol;

	while (true) {
		int level;

		level = xkb_state_key_get_level(self->state, entry->code, 0);

		if (entry->level == level)
			return entry;

		if (++entry >= &self->lookup_table[self->lookup_table_length] ||
		    entry->symbol != symbol)
			break;
	}

	return NULL;
}

static bool keyboard_symbol_is_mod(xkb_keysym_t symbol)
{
	switch (symbol) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Shift_Lock:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Hyper_L:
	case XKB_KEY_Hyper_R:
	case XKB_KEY_ISO_Level5_Shift:
	case XKB_KEY_ISO_Level5_Lock:
		return true;
	}

	return false;
}

bool keybind_consumed(struct keyboard* self, xkb_keycode_t code, bool is_pressed)
{
	if (!is_pressed) {
		if (!intset_is_set(&self->active_keybinds, code))
			return false;
		nvnc_log(NVNC_LOG_DEBUG, "Intercepted reciprocal keyrelease for keybind of key 0x%x", code);
		intset_clear(&self->active_keybinds, code);
		return true;
	}
	bool handled = false;
	struct keybind* keybind;
	wl_list_for_each(keybind, &self->keybinds, link) {
		if (keybind->code != code)
			continue;
		xkb_mod_mask_t mods = xkb_state_serialize_mods(self->state,
				XKB_STATE_MODS_EFFECTIVE);
		nvnc_log(NVNC_LOG_DEBUG, "Saw potential keybind for 0x%x. required mods: 0x%x current mods: 0x%x",
				keybind->code, keybind->mods, mods);
		if ((keybind->mods & mods) == keybind->mods) {
			nvnc_log(NVNC_LOG_INFO, "Intercepted keypress for keybind %s (0x%x)",
					keybind->name, code);
			intset_set(&self->active_keybinds, code);
			keybind->on_press(keybind);
			handled = true;
			break;
		}
	}
	return handled;
}

static void send_key(struct keyboard* self, xkb_keycode_t code, bool is_pressed)
{

	if (keybind_consumed(self, code, is_pressed))
		return;

	zwp_virtual_keyboard_v1_key(self->virtual_keyboard, 0, code - 8,
	                            is_pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
	                                       : WL_KEYBOARD_KEY_STATE_RELEASED);
}

static void save_mods(struct keyboard* self, struct kb_mods* mods)
{
	mods->depressed = xkb_state_serialize_mods(self->state,
			XKB_STATE_MODS_DEPRESSED);
	mods->latched = xkb_state_serialize_mods(self->state,
			XKB_STATE_MODS_LATCHED);
	mods->locked = xkb_state_serialize_mods(self->state,
			XKB_STATE_MODS_LOCKED);
}

static void restore_mods(struct keyboard* self, struct kb_mods* mods)
{
	xkb_state_update_mask(self->state, mods->depressed, mods->latched,
			mods->locked, XKB_STATE_MODS_DEPRESSED,
			XKB_STATE_MODS_LATCHED, XKB_STATE_MODS_LOCKED);
}

static void send_key_with_level(struct keyboard* self, xkb_keycode_t code,
		bool is_pressed, int level)
{
	struct kb_mods save;
	save_mods(self, &save);

	xkb_mod_mask_t mods = 0;
	xkb_keymap_key_get_mods_for_level(self->keymap, code, 0, level,
			&mods, 1);
	xkb_state_update_mask(self->state, mods, 0, 0, XKB_STATE_MODS_DEPRESSED,
			XKB_STATE_MODS_LATCHED, XKB_STATE_MODS_LOCKED);
	keyboard_send_mods(self);

	nvnc_log(NVNC_LOG_DEBUG, "send key with level: old mods: %x, new mods: %x",
			save.latched | save.locked | save.depressed, mods);

	send_key(self, code, is_pressed);

	restore_mods(self, &save);
	keyboard_send_mods(self);
}

static bool update_key_state(struct keyboard* self, xkb_keycode_t code,
		bool is_pressed)
{
	bool was_pressed = intset_is_set(&self->key_state, code);
	if (was_pressed == is_pressed)
		return false;

	if (is_pressed)
		intset_set(&self->key_state, code);
	else
		intset_clear(&self->key_state, code);

	return true;
}

void keyboard_feed(struct keyboard* self, xkb_keysym_t symbol, bool is_pressed)
{
	struct table_entry* entry = keyboard_find_symbol(self, symbol);
	if (!entry) {
		char name[256];
		nvnc_log(NVNC_LOG_ERROR, "Failed to look up keyboard symbol: %s",
		          get_symbol_name(symbol, name, sizeof(name)));
		return;
	}

	bool level_is_match = true;

	if (!keyboard_symbol_is_mod(symbol)) {
		struct table_entry* level_entry = match_level(self, entry);
		if (level_entry)
			entry = level_entry;
		else
			level_is_match = false;
	}

#ifndef NDEBUG
	keyboard__dump_entry(self, entry);
#endif

	if (!update_key_state(self, entry->code, is_pressed))
		return;

	keyboard_apply_mods(self, entry->code, is_pressed);

	if (level_is_match)
		send_key(self, entry->code, is_pressed);
	else
		send_key_with_level(self, entry->code, is_pressed,
				entry->level);
}

void keyboard_feed_code(struct keyboard* self, xkb_keycode_t code,
		bool is_pressed)
{
	if (update_key_state(self, code, is_pressed)) {
		keyboard_apply_mods(self, code, is_pressed);
		send_key(self, code, is_pressed);
	}
}

xkb_keycode_t parse_keycode(struct keyboard* self, const char* name)
{
	xkb_keysym_t code = xkb_keymap_key_by_name(self->keymap, name);
	if (code != XKB_KEYCODE_INVALID) {
		nvnc_log(NVNC_LOG_DEBUG, "Mapped %s to keycode 0x%x (%s)", name, code,
		xkb_keymap_key_get_name(self->keymap, code));
		return code;
	}

	xkb_keysym_t keysym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
	if (keysym == XKB_KEY_NoSymbol) {
		nvnc_log(NVNC_LOG_WARNING, "%s is not a recognised keycode or keysym", name);
		return XKB_KEYCODE_INVALID;
	}
	struct table_entry* entry = keyboard_find_symbol(self, keysym);
	if (entry == NULL) {
		nvnc_log(NVNC_LOG_WARNING, "%s is a valid keysym, but could not be mapped to a keycode", name);
		return XKB_KEYCODE_INVALID;
	}
	// TODO: what about levels??
	char buffer[64];
	nvnc_log(NVNC_LOG_DEBUG, "Mapped %s (aka %s) to keycode 0x%x (%s)", name,
		get_symbol_name(keysym, buffer, sizeof(buffer)),
		entry->code,
		xkb_keymap_key_get_name(self->keymap, entry->code));
	return entry->code;
}

struct mod_table_entry {
	const char* modname;
	const char* alternatives[5];
};

static struct mod_table_entry mod_table[] = {
	{
		.modname = XKB_MOD_NAME_CTRL,
		.alternatives = { "control", "ctrl", NULL },
	},
	{
		.modname = XKB_MOD_NAME_ALT,
		.alternatives = { "alt", "mod1", NULL },
	},
	{
		.modname = XKB_MOD_NAME_LOGO,
		.alternatives = { "win", "logo", "super", "mod4", NULL },
	},
};

xkb_mod_index_t parse_keymod(struct keyboard* self, const char* name)
{
	nvnc_log(NVNC_LOG_DEBUG, "Searching for modifier %s");
	const char* modname = name;
	for(int i = 0; i < sizeof(mod_table)/sizeof(mod_table[0]); ++i) {
		for (const char** alt = mod_table[i].alternatives;
				*alt != NULL; ++alt)
		{
			if (strcasecmp(name, *alt) == 0) {
				modname = mod_table[i].modname;
				nvnc_log(NVNC_LOG_DEBUG, "Mapped %s to modifier %s", name, modname);
				goto done_mod_search;
			}
		}
	}
done_mod_search:
	return xkb_keymap_mod_get_index(self->keymap, modname);
}

int keyboard_add_keybind(struct keyboard* self, const char* key_name,
    void (*on_press)(struct keybind*), void* userdata)
{
	struct keybind* keybind = calloc(1, sizeof(*keybind));
	if (!keybind) {
		nvnc_log(NVNC_LOG_ERROR, "OOM");
		return -1;
	}
	strncpy(keybind->name, key_name, sizeof(keybind->name));
	keybind->on_press = on_press;
	keybind->userdata = userdata;

	char* part;
	char* i = keybind->name;;
	while((part = strsep(&i, "+"))) {
		if (i == NULL) {
			// Last word; must be the key
			keybind->code = parse_keycode(self, part);
			if (keybind->code == XKB_KEYCODE_INVALID) {
				nvnc_log(NVNC_LOG_ERROR, "Keybinding for %s failed: %s is not a valid key",
						key_name, part);
				goto parse_failure;
			}
		} else {
			// Must be a modifier
			xkb_mod_index_t mod = parse_keymod(self, part);
			if (mod == XKB_MOD_INVALID) {
				nvnc_log(NVNC_LOG_ERROR, "Keybinding for %s failed: %s is not a valid modifier",
						key_name, part);
				goto parse_failure;
			}
			keybind->mods |= (1 << mod);
		}
	}
	// Recopy the original name now that strsep is done
	strncpy(keybind->name, key_name, 128);

	nvnc_log(NVNC_LOG_DEBUG, "Adding keybind for %s: 0x%x 0x%x",
			keybind->name, keybind->code, keybind->mods);

	wl_list_insert(&self->keybinds, &keybind->link);

	return 0;

parse_failure:
	free(keybind);
	return -1;
}

void keybind_remove(struct keybind* self)
{
	wl_list_remove(&self->link);
	free(self);
}
