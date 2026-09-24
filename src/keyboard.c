/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */

#include "keyd.h"

static long process_event(struct keyboard *kbd, uint8_t code, int pressed, long time);

/*
 * Here be tiny dragons.
 */

static long get_time(void)
{
	/* Close enough :/. Using a syscall is unnecessary. */
	static long time = 1;
	return time++;
}

static int cache_set(struct keyboard *kbd, uint8_t code, struct cache_entry *ent)
{
	size_t i;
	int slot = -1;

	for (i = 0; i < CACHE_SIZE; i++)
		if (kbd->cache[i].code == code) {
			slot = i;
			break;
		} else if (!kbd->cache[i].code) {
			slot = i;
		}

	if (slot == -1)
		return -1;

	if (ent == NULL) {
		kbd->cache[slot].code = 0;
	} else {
		kbd->cache[slot] = *ent;
		kbd->cache[slot].code = code;
	}

	return 0;
}

static struct cache_entry *cache_get(struct keyboard *kbd, uint8_t code)
{
	size_t i;

	for (i = 0; i < CACHE_SIZE; i++)
		if (kbd->cache[i].code == code)
			return &kbd->cache[i];

	return NULL;
}

static void reset_keystate(struct keyboard *kbd)
{
	size_t i;

	for (i = 0; i < 256; i++) {
		if (kbd->keystate[i]) {
			kbd->output.send_key(i, 0);
			kbd->keystate[i] = 0;
		}
	}

}

static void send_key(struct keyboard *kbd, uint8_t code, uint8_t pressed)
{
	if (code == KEYD_NOOP || code == KEYD_EXTERNAL_MOUSE_BUTTON)
		return;

	if (pressed)
		kbd->last_pressed_output_code = code;

	if (kbd->keystate[code] != pressed) {
		kbd->keystate[code] = pressed;
		kbd->output.send_key(code, pressed);
	}
}

static void send_key_macro_wrapper(void *kbd, uint8_t code, uint8_t pressed)
{
	send_key(kbd, code, pressed);
}

static void clear_mod(struct keyboard *kbd, uint8_t code)
{
	/*
	 * Some modifiers have a special meaning when used in
	 * isolation (e.g meta in Gnome, alt in Firefox).
	 * In order to prevent spurious key presses we
	 * avoid adjacent down/up pairs by interposing
	 * additional control sequences.
	 */
	int guard = (((kbd->last_pressed_output_code == code) &&
			(code == KEYD_LEFTMETA ||
			 code == KEYD_LEFTALT ||
			 code == KEYD_RIGHTALT)) &&
		       !kbd->inhibit_modifier_guard &&
		       !kbd->config.disable_modifier_guard);

	if (guard && !kbd->keystate[KEYD_LEFTCTRL]) {
		send_key(kbd, KEYD_LEFTCTRL, 1);
		send_key(kbd, code, 0);
		send_key(kbd, KEYD_LEFTCTRL, 0);
	} else {
		send_key(kbd, code, 0);
	}
}

static void set_mods(struct keyboard *kbd, uint8_t mods)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(modifiers); i++) {
		uint8_t mask = modifiers[i].mask;
		uint8_t code = modifiers[i].key;

		if (mask & mods) {
			if (!kbd->keystate[code])
				send_key(kbd, code, 1);
		} else {
			if (kbd->keystate[code])
				clear_mod(kbd, code);
		}
	}
}

// Returns the resultant mod mask.
static uint8_t update_mods(struct keyboard *kbd, int excluded_layer_idx, uint8_t mods)
{
	size_t i;
	struct layer *excluded_layer = excluded_layer_idx == -1 ?
					NULL :
					&kbd->config.layers[excluded_layer_idx];

	for (i = 0; i < kbd->config.nr_layers; i++) {
		struct layer *layer = &kbd->config.layers[i];
		int excluded = 0;

		if (!kbd->layer_state[i].active)
			continue;

		if (layer == excluded_layer) {
			excluded = 1;
		} else if (excluded_layer && excluded_layer->type == LT_COMPOSITE) {
			size_t j;

			for (j = 0; j < excluded_layer->nr_constituents; j++)
				if ((size_t)excluded_layer->constituents[j] == i)
					excluded = 1;
		}

		if (!excluded)
			mods |= layer->mods;
	}

	set_mods(kbd, mods);

	return mods;
}

static long execute_macro(struct keyboard *kbd, int dl, const struct macro *macro)
{
	long time = 0;

	/* Minimize redundant modifier strokes for simple key sequences. */
	if (macro->sz == 1 && macro->entries[0].type == MACRO_KEYSEQUENCE) {
		uint8_t code = macro->entries[0].data;
		uint8_t mods = macro->entries[0].data >> 8;

		update_mods(kbd, dl, mods);
		send_key(kbd, code, 1);
		send_key(kbd, code, 0);
	} else {
		update_mods(kbd, dl, 0);
		time = macro_execute(send_key_macro_wrapper, kbd, macro, kbd->config.macro_sequence_timeout);
	}

	update_mods(kbd, -1, 0);
	return time;
}

static void lookup_descriptor(struct keyboard *kbd, uint8_t code,
			      struct descriptor *d, int *dl)
{
	size_t max;
	size_t i;

	d->op = 0;

	long maxts = 0;

	if (code >= KEYD_CHORD_1 && code <= KEYD_CHORD_MAX) {
		size_t idx = code - KEYD_CHORD_1;

		*d = kbd->active_chords[idx].chord.d;
		*dl = kbd->active_chords[idx].layer;

		return;
	}

	for (i = 0; i < kbd->config.nr_layers; i++) {
		struct layer *layer = &kbd->config.layers[i];

		if (kbd->layer_state[i].active) {
			long activation_time = kbd->layer_state[i].activation_time;

			if (layer->keymap[code].op && activation_time >= maxts) {
				maxts = activation_time;
				*d = layer->keymap[code];
				*dl = i;
			}
		}
	}

	max = 0;
	/* Scan for any composite matches (which take precedence). */
	for (i = 0; i < kbd->config.nr_layers; i++) {
		struct layer *layer = &kbd->config.layers[i];

		if (layer->type == LT_COMPOSITE) {
			size_t j;
			int match = 1;
			uint8_t mods = 0;

			for (j = 0; j < layer->nr_constituents; j++) {
				if (kbd->layer_state[layer->constituents[j]].active)
					mods |= kbd->config.layers[layer->constituents[j]].mods;
				else
					match = 0;
			}

			if (match && layer->keymap[code].op && (layer->nr_constituents > max)) {
				*d = layer->keymap[code];
				*dl = i;

				max = layer->nr_constituents;
			}
		}
	}

	if (!d->op) {
		d->op = OP_KEYSEQUENCE;
		d->args[0].code = code;
		d->args[1].mods = 0;
		*dl = 0;
	}
}

static void deactivate_layer(struct keyboard *kbd, int idx)
{
	dbg("Deactivating layer %s", kbd->config.layers[idx].name);

	assert(kbd->layer_state[idx].active > 0);
	kbd->layer_state[idx].active--;

	kbd->output.on_layer_change(kbd, &kbd->config.layers[idx], 0);
}

/*
 * NOTE: Every activation call *must* be paired with a
 * corresponding deactivation call.
 */

static void activate_layer(struct keyboard *kbd, uint8_t code, int idx)
{
	dbg("Activating layer %s", kbd->config.layers[idx].name);
	struct cache_entry *ce;

	kbd->layer_state[idx].activation_time = get_time();
	kbd->layer_state[idx].active++;

	if ((ce = cache_get(kbd, code)))
		ce->layer = idx;

	kbd->output.on_layer_change(kbd, &kbd->config.layers[idx], 1);
}

/* Returns:
 *  0 on no match
 *  1 on partial match
 *  2 on exact match
 */
static int chord_event_match(struct chord *chord, struct key_event *events, size_t nevents)
{
	size_t i, j;
	size_t n = 0;
	size_t npressed = 0;

	if (!nevents)
		return 0;

	for (i = 0; i < nevents; i++)
		if (events[i].pressed) {
			int found = 0;

			npressed++;
			for (j = 0; j < chord->sz; j++)
				if (chord->keys[j] == events[i].code)
					found = 1;

			if (!found)
				return 0;
			else
				n++;
		}

	if (npressed == 0)
		return 0;
	else
		return n == chord->sz ? 2 : 1;
}

static void enqueue_chord_event(struct keyboard *kbd, uint8_t code, uint8_t pressed, long time)
{
	if (!code)
		return;

	assert(kbd->chord.queue_sz < ARRAY_SIZE(kbd->chord.queue));

	kbd->chord.queue[kbd->chord.queue_sz].code = code;
	kbd->chord.queue[kbd->chord.queue_sz].pressed = pressed;
	kbd->chord.queue[kbd->chord.queue_sz].timestamp = time;

	kbd->chord.queue_sz++;
}

/* Returns:
 *  0 in the case of no match
 *  1 in the case of a partial match
 *  2 in the case of an unambiguous match (populating chord and layer)
 *  3 in the case of an ambiguous match (populating chord and layer)
 */
static int check_chord_match(struct keyboard *kbd, const struct chord **chord, int *chord_layer)
{
	size_t idx;
	int full_match = 0;
	int partial_match = 0;
	long maxts = -1;

	for (idx = 0; idx < kbd->config.nr_layers; idx++) {
		size_t i;
		struct layer *layer = &kbd->config.layers[idx];

		if (!kbd->layer_state[idx].active)
			continue;

		for (i = 0; i < layer->nr_chords; i++) {
			int ret = chord_event_match(&layer->chords[i],
						    kbd->chord.queue,
						    kbd->chord.queue_sz);

			if (ret == 2 &&
				maxts <= kbd->layer_state[idx].activation_time) {
				*chord_layer = (int)idx;
				*chord = &layer->chords[i];

				full_match = 1;
				maxts = kbd->layer_state[idx].activation_time;
			} else if (ret == 1) {
				partial_match = 1;
			}
		}
	}

	if (full_match)
		return partial_match ? 3 : 2;
	else if (partial_match)
		return 1;
	else
		return 0;
}

static void execute_command(const char *cmd)
{
	int fd;

	dbg("executing command: %s", cmd);

	if (fork()) {
		wait(NULL);
		return;
	}
	if (fork())
		exit(0);

	fd = open("/dev/null", O_RDWR);

	if (fd < 0) {
		perror("open");
		exit(-1);
	}

	close(0);
	close(1);
	close(2);

	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);

	execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
}

static int is_modifier_layer(struct keyboard *kbd, int idx)
{
	const char *name = kbd->config.layers[idx].name;

	return name && (!strcmp(name, "shift") || !strcmp(name, "control") || !strcmp(name, "meta") || !strcmp(name, "alt") || !strcmp(name, "altgr"));
}

static void clear_mod_layers(struct keyboard *kbd)
{
	for (int j = 0; j < kbd->mod_count; j++) {
		int idx = kbd->mod_idx[j];

		if (kbd->layer_state[idx].toggled)
			kbd->layer_state[idx].toggled = 0;

		kbd->mod_idx[j] = -1;
	}

	kbd->mod_count = 0;
}

static void add_mod_layer(struct keyboard *kbd, int idx)
{
	for (int i = 0; i < kbd->mod_count; i++) {
		if (kbd->mod_idx[i] == idx)
			return;
	}

	if (kbd->mod_count < MAX_DESCRIPTOR_ARGS)
		kbd->mod_idx[kbd->mod_count++] = idx;
}

static void toggle_mod_layer(struct keyboard *kbd, int idx)
{
	for (int i = 0; i < kbd->mod_count; i++) {
		if (kbd->mod_idx[i] == idx) {
			for (int j = i; j < kbd->mod_count - 1; j++)
				kbd->mod_idx[j] = kbd->mod_idx[j + 1];

			kbd->mod_count--;
			kbd->mod_idx[kbd->mod_count] = -1;
			kbd->layer_state[idx].toggled = 0;
			kbd->output.on_layer_change(kbd, &kbd->config.layers[idx], 0);
			return;
		}
	}

	if (kbd->mod_count < MAX_DESCRIPTOR_ARGS) {
		kbd->mod_idx[kbd->mod_count++] = idx;
		kbd->layer_state[idx].toggled = 1;
		kbd->output.on_layer_change(kbd, &kbd->config.layers[idx], 1);
	}
}

static int add_descriptor_mod_layers(struct keyboard *kbd, struct descriptor *d)
{
	int added = 0;

	for (int j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
		int idx = d->args[j].idx;

		if (idx == -1)
			break;

		if (is_modifier_layer(kbd, idx)) {
			add_mod_layer(kbd, idx);
			added = 1;
		}
	}

	return added;
}

static void remove_toggle_idx(struct layer_trigger *lt, int idx)
{
	for (size_t i = 0; i < lt->toggle_count; i++) {
		if (lt->toggle_idx[i] != idx)
			continue;

		lt->toggle_idx[i] = lt->toggle_idx[--lt->toggle_count];
		return;
	}
}

static void clear_oneshot(struct keyboard *kbd)
{
	size_t i = 0;

	for (i = 0; i < kbd->config.nr_layers; i++)
		while (kbd->layer_state[i].oneshot_depth) {

			if (kbd->layer_state[i].toggled) {
				kbd->layer_state[i].toggled = 0;
				kbd->layer_prefix = NULL;
			}

			deactivate_layer(kbd, i);
			kbd->layer_state[i].oneshot_depth--;
			kbd->activation = NONE;
			clear_mod_layers(kbd);
		}

	kbd->oneshot_latch = 0;
	kbd->oneshot_timeout = 0;
}

static void clear(struct keyboard *kbd)
{
	size_t i;
	clear_oneshot(kbd);
	for (i = 1; i < kbd->config.nr_layers; i++) {
		struct layer *layer = &kbd->config.layers[i];

		if (layer->type != LT_LAYOUT) {
			if (kbd->layer_state[i].toggled) {
				kbd->layer_state[i].toggled = 0;
				deactivate_layer(kbd, i);
			}
		}
	}

	kbd->active_macro = NULL;
	kbd->active_idx[0] = -1;
	kbd->layer_trigger_depth = 0;
	kbd->activation = NONE;
	kbd->layer_prefix = NULL;

	clear_mod_layers(kbd);
	reset_keystate(kbd);
}

static void setlayout(struct keyboard *kbd, uint8_t idx)
{
	clear(kbd);
	/* Only only layout may be active at a time, with the exception of main. */
	size_t i;
	for (i = 1; i < kbd->config.nr_layers; i++) {
		struct layer *layer = &kbd->config.layers[i];

		if (layer->type == LT_LAYOUT)
			kbd->layer_state[i].active = 0;
	}

	// Setting the layout to main is equivalent to clearing all occluding layouts.
	if (idx != 0) {
		kbd->layer_state[idx].activation_time = 1;
		kbd->layer_state[idx].active = 1;
	}

	kbd->output.on_layer_change(kbd, &kbd->config.layers[idx], 1);
}


static void schedule_timeout(struct keyboard *kbd, long timeout)
{
	assert(kbd->nr_timeouts < ARRAY_SIZE(kbd->timeouts));
	kbd->timeouts[kbd->nr_timeouts++] = timeout;
}

static long calculate_main_loop_timeout(struct keyboard *kbd, long time)
{
	size_t i;
	long timeout = 0;
	size_t n = 0;

	for (i = 0; i < kbd->nr_timeouts; i++)
		if (kbd->timeouts[i] > time) {
			if (!timeout || kbd->timeouts[i] < timeout)
				timeout = kbd->timeouts[i];

			kbd->timeouts[n++] = kbd->timeouts[i];
		}

	kbd->nr_timeouts = n;
	return timeout ? timeout - time : 0;
}

static uint8_t reverse_key(uint8_t code)
{
	switch (code) {
		REVERSE_KEY(KEYD_W, KEYD_B)
		REVERSE_KEY(KEYD_N, KEYD_P)
		REVERSE_KEY(KEYD_RIGHTBRACE, KEYD_LEFTBRACE)
		REVERSE_KEY(KEYD_SEMICOLON, KEYD_COMMA)

		REVERSE_KEY(KEYD_SCROLL_LEFT, KEYD_SCROLL_RIGHT)
		REVERSE_KEY(KEYD_SCROLL_UP, KEYD_SCROLL_DOWN)
		REVERSE_KEY(KEYD_UP, KEYD_DOWN)
		REVERSE_KEY(KEYD_LEFT, KEYD_RIGHT)
		REVERSE_KEY(KEYD_PAGEUP, KEYD_PAGEDOWN)
		REVERSE_KEY(KEYD_HOME, KEYD_END)

	default:
		return code;
	}
}

static void reverse_descriptor(struct keyboard *kbd, struct descriptor *d)
{
	if (d->op == OP_KEYSEQUENCE) {
		REVERSE_SHIFT_KEY(KEYD_TAB);
		REVERSE_SHIFT_KEY(KEYD_F3);
		REVERSE_SHIFT_KEY(KEYD_Z);

		REVERSE_MOD_KEY(MOD_SHIFT, KEYD_RIGHTBRACE, KEYD_LEFTBRACE);
		REVERSE_MOD_KEY(MOD_SHIFT, KEYD_COMMA, KEYD_DOT);
		REVERSE_MOD_KEY(MOD_SHIFT, KEYD_3, KEYD_8);
		REVERSE_MOD_KEY(MOD_SHIFT, KEYD_4, KEYD_6);
		REVERSE_MOD_KEY(MOD_SHIFT, KEYD_0, KEYD_9);
		REVERSE_MOD_KEY(MOD_SHIFT, KEYD_W, KEYD_B);
		REVERSE_MOD_KEY(MOD_CTRL, KEYD_F, KEYD_B);
		REVERSE_MOD_KEY(MOD_CTRL, KEYD_E, KEYD_A);
		REVERSE_MOD_KEY(MOD_CTRL, KEYD_N, KEYD_P);
		REVERSE_MOD_KEY(MOD_CTRL, KEYD_D, KEYD_U);
		REVERSE_MOD_KEY(MOD_CTRL, KEYD_G, KEYD_T);
		REVERSE_MOD_KEY(MOD_CTRL, KEYD_J, KEYD_K);
		REVERSE_MOD_KEY(MOD_CTRL, KEYD_I, KEYD_O);

		d->args[0].code = reverse_key(d->args[0].code);
		return;
	}

	if (d->op == OP_MACRO && d->args[0].idx == REPEAT_MACRO_IDX) {
		struct macro *macro = &kbd->config.macros[REPEAT_MACRO_IDX];

		if (macro->sz == 2 && macro->entries[0].type == MACRO_KEYSEQUENCE) {
			/*
			 * Reverse:
			 * ]b -> [b "|" [b -> ]b
			 * C-a n -> C-a p "|" C-a p -> C-a n
			 */
			uint8_t prefix_code = macro->entries[0].data & 0xff;
			uint8_t prefix_mods = (macro->entries[0].data >> 8) & 0xff;
			uint8_t code = macro->entries[1].data & 0xff;
			if (prefix_mods) {
				code = reverse_key(code);
				macro->entries[1].data = code | (macro->entries[1].data & 0xff00);
			} else {
				code = reverse_key(prefix_code);
				macro->entries[0].data = code | (macro->entries[0].data & 0xff00);
			}
		}

		d->args[0].idx = REPEAT_MACRO_IDX;
	}
}

static int is_repeat_prefix(uint8_t code, uint8_t mods)
{
	if ((code == KEYD_LEFTBRACE || code == KEYD_RIGHTBRACE) && mods == 0)
		return 1;

	if (code == KEYD_B && (mods & MOD_CTRL))
		return 1;

	return 0;
}

static void resolve_toggle_on_layer(struct keyboard *kbd, struct layer_trigger *lt, long time)
{
	int should_update_mod = 0;

	for (size_t i = 0; i < lt->toggle_count; i++) {
		int idx = lt->toggle_idx[i];

		if (lt->other_key_pressed) {
			kbd->layer_state[idx].toggled = 0;
			kbd->layer_prefix = NULL;
			deactivate_layer(kbd, idx);
			clear_mod_layers(kbd);
			should_update_mod = 1;
		} else {
			kbd->layer_state[idx].oneshot_depth++;

			if (kbd->config.oneshot_timeout) {
				kbd->oneshot_timeout = time + kbd->config.oneshot_timeout;
				schedule_timeout(kbd, kbd->oneshot_timeout);
			}
		}
	}

	if (should_update_mod)
		update_mods(kbd, -1, 0);
}

static long process_descriptor(struct keyboard *kbd, uint8_t code,
			       const struct descriptor *d, int dl,
			       int pressed, long time)
{
	int i;
	int timeout = 0;

	if (pressed) {
		struct macro *macro;

		switch (d->op) {
		case OP_LAYERM:
		case OP_ONESHOTM:
		case OP_TOGGLEM:
			macro = &kbd->config.macros[d->args[1].idx];
			execute_macro(kbd, dl, macro);
			break;
		default:
			break;
		}
	}

	switch (d->op) {
		int idx;
		struct macro *macro;
		struct descriptor *action;
		uint8_t mods;
		uint8_t new_code;
		struct pending_timeout *pt;

	case OP_KEYSEQUENCE:
		new_code = d->args[0].code;
		mods = d->args[1].mods;
		if (pressed) {
			uint8_t active_mods;

			/*
			 * Permit variations of the same key
			 * to be actuated next to each other
			 * E.G [/{
			 */
			if (kbd->keystate[new_code])
				send_key(kbd, new_code, 0);

			active_mods = update_mods(kbd, dl, mods);

			kbd->last_repeatable_action = *d;
			kbd->last_repeatable_action.args[1].mods = active_mods;

			send_key(kbd, new_code, 1);
			clear_oneshot(kbd);
		} else {
			send_key(kbd, new_code, 0);

			if (kbd->repeat_prefix_code) {
				struct macro *macro = &kbd->config.macros[REPEAT_MACRO_IDX];
				macro->sz = 2;

				macro->entries[0].type = MACRO_KEYSEQUENCE;
				macro->entries[0].data = kbd->repeat_prefix_code | ((uint16_t)kbd->repeat_prefix_mods << 8);

				macro->entries[1].type = MACRO_KEYSEQUENCE;
				macro->entries[1].data = new_code | ((uint16_t)kbd->last_repeatable_action.args[1].mods << 8);

				kbd->last_repeatable_action.op = OP_MACRO;
				kbd->last_repeatable_action.args[0].idx = REPEAT_MACRO_IDX;

				kbd->repeat_prefix_code = 0;
				kbd->repeat_prefix_mods = 0;

			} else if (is_repeat_prefix(new_code, kbd->last_repeatable_action.args[1].mods)) {
				kbd->repeat_prefix_code = new_code;
				kbd->repeat_prefix_mods = kbd->last_repeatable_action.args[1].mods;
			}

			update_mods(kbd, -1, 0);
		}

		if (!mods || mods == MOD_SHIFT)
			kbd->last_simple_key_time = time;

		break;
	case OP_SCROLL:
		kbd->scroll.sensitivity = d->args[0].sensitivity;
		if (pressed)
			kbd->scroll.active = 1;
		else
			kbd->scroll.active = 0;
		break;
	case OP_SCROLL_TOGGLE_ON:
		kbd->scroll.sensitivity = d->args[0].sensitivity;
		kbd->scroll.active = 1;
		break;
	case OP_SCROLL_TOGGLE_OFF:
		if (pressed)
			kbd->scroll.active = 0;
		break;
	case OP_SCROLL_TOGGLE:
		kbd->scroll.sensitivity = d->args[0].sensitivity;
		if (pressed)
			kbd->scroll.active = !kbd->scroll.active;
		break;
	case OP_OVERLOAD_IDLE_TIMEOUT:
		if (pressed) {
			struct descriptor *action;
			long timeout = d->args[2].timeout;

			if (((time - kbd->last_simple_key_time) >= timeout))
				action = &kbd->config.descriptors[d->args[1].idx];
			else
				action = &kbd->config.descriptors[d->args[0].idx];

			process_descriptor(kbd, code, action, dl, 1, time);
			for (i = 0; i < CACHE_SIZE; i++) {
				if (code == kbd->cache[i].code) {
					kbd->cache[i].d = *action;
					break;
				}
			}
		}
		break;
	case OP_OVERLOAD_TIMEOUT_TAP:
	case OP_OVERLOAD_TIMEOUT:
		if (pressed) {
			uint8_t layer = d->args[0].idx;
			struct descriptor *action = &kbd->config.descriptors[d->args[1].idx];

			kbd->pending_overload.code = code;
			kbd->pending_overload.resolve_on_interrupt = d->op == OP_OVERLOAD_TIMEOUT_TAP;

			kbd->pending_overload.dl = dl;
			kbd->pending_overload.action1 = *action;
			kbd->pending_overload.action2.op = OP_LAYER;
			kbd->pending_overload.action2.args[0].idx = layer;
			kbd->pending_overload.expiration = time + d->args[2].timeout;

			schedule_timeout(kbd, kbd->pending_overload.expiration);
		}

		break;
	case OP_LAYOUT:
		if (pressed)
			setlayout(kbd, d->args[0].idx);

		break;
	case OP_LAYERM:
	case OP_LAYER:
		idx = d->args[0].idx;

		if (pressed) {
			activate_layer(kbd, code, idx);
		} else {
			deactivate_layer(kbd, idx);
		}

		if (kbd->last_pressed_code == code) {
			kbd->inhibit_modifier_guard = 1;
			update_mods(kbd, -1, 0);
			kbd->inhibit_modifier_guard = 0;
		} else {
			update_mods(kbd, -1, 0);
		}

		break;
	case OP_CLEARM:
		if(pressed) {
			clear(kbd);
			macro = &kbd->config.macros[d->args[0].idx];
			execute_macro(kbd, dl, macro);
		}
		break;
	case OP_REPEAT:
		if(pressed) {
			process_descriptor(kbd, code, &kbd->last_repeatable_action, dl, 1, time);

			for (i = 0; i < CACHE_SIZE; i++)
				if (kbd->cache[i].code == code)
					kbd->cache[i].d = kbd->last_repeatable_action;
		}
		break;
	case OP_REPEAT_REVERSE:
		if (pressed) {
			struct descriptor original = kbd->last_repeatable_action;
			kbd->repeat_reverse_action = original;
			reverse_descriptor(kbd, &kbd->repeat_reverse_action);
			process_descriptor(kbd, code, &kbd->repeat_reverse_action, dl, 1, time);
			kbd->last_repeatable_action = original;
		} else {
			struct descriptor original = kbd->last_repeatable_action;
			process_descriptor(kbd, code, &kbd->repeat_reverse_action, dl, 0, time);
			kbd->last_repeatable_action = original;
		}
		break;
	case OP_CLEAR:
		if(pressed)
			clear(kbd);
		break;
	case OP_OVERLOAD:
		idx = d->args[0].idx;
		action = &kbd->config.descriptors[d->args[1].idx];

		if (pressed) {
			kbd->overload_start_time = time;
			activate_layer(kbd, code, idx);
			update_mods(kbd, -1, 0);
		} else {
			deactivate_layer(kbd, idx);
			update_mods(kbd, -1, 0);

			if (kbd->last_pressed_code == code &&
			    (!kbd->config.overload_tap_timeout ||
			     ((time - kbd->overload_start_time) < kbd->config.overload_tap_timeout))) {
				if (action->op == OP_MACRO) {
					/*
					 * Macro release relies on event logic, so we can't just synthesize a
					 * descriptor release.
					 */
					struct macro *macro = &kbd->config.macros[action->args[0].idx];
					execute_macro(kbd, dl, macro);
				} else {
					process_descriptor(kbd, code, action, dl, 1, time);
					process_descriptor(kbd, code, action, dl, 0, time);
				}
			}
		}

		break;
	case OP_ONESHOTM:
	case OP_ONESHOTK:
	case OP_ONESHOT:
		idx = d->args[0].idx;

		if (pressed) {
			if (d->op == OP_ONESHOTK)
				process_descriptor(kbd, code, &kbd->config.descriptors[d->args[1].idx], dl, 1, time);

			activate_layer(kbd, code, idx);
			update_mods(kbd, dl, 0);
			kbd->oneshot_latch = 1;
		} else {
			if (d->op == OP_ONESHOTK)
				process_descriptor(kbd, code, &kbd->config.descriptors[d->args[1].idx], dl, 0, time);

			if (kbd->oneshot_latch) {
				kbd->layer_state[idx].oneshot_depth++;
				if (kbd->config.oneshot_timeout) {
					kbd->oneshot_timeout = time + kbd->config.oneshot_timeout;
					schedule_timeout(kbd, kbd->oneshot_timeout);
				}
			} else {
				deactivate_layer(kbd, idx);
				update_mods(kbd, -1, 0);
			}
		}

		break;
	case OP_MACRO2:
	case OP_MACRO:
		if (pressed) {
			long execution_time;

			if (d->op == OP_MACRO2) {
				macro = &kbd->config.macros[d->args[2].idx];

				timeout = d->args[0].timeout;
				kbd->macro_repeat_interval = d->args[1].timeout;
			} else {
				macro = &kbd->config.macros[d->args[0].idx];

				timeout = kbd->config.macro_timeout;
				kbd->macro_repeat_interval = kbd->config.macro_repeat_timeout;
			}

			clear_oneshot(kbd);

			execution_time = execute_macro(kbd, dl, macro);
			kbd->active_macro = macro;
			kbd->active_macro_layer = dl;

			kbd->macro_timeout = execution_time + time + timeout;
			schedule_timeout(kbd, kbd->macro_timeout);

			kbd->last_repeatable_action = *d;
		}

		break;
	case OP_TOGGLEM:
	case OP_TOGGLE:
		idx = d->args[0].idx;

		if (pressed) {
			kbd->layer_state[idx].toggled = !kbd->layer_state[idx].toggled;

			if (kbd->layer_state[idx].toggled)
				activate_layer(kbd, code, idx);
			else
				deactivate_layer(kbd, idx);

			update_mods(kbd, -1, 0);
			clear_oneshot(kbd);
		}

		break;
	case OP_TAPHOLD3:
		/*
		 * tap / hold / double-tap on a single key.
		 *
		 * Composing the existing actions cannot express this: overloadi
		 * resolves on key-down while timeout needs the release to tell a
		 * tap from a hold, so whichever is nested inside the other is
		 * starved. Doing both here avoids dispatching one timing action
		 * into another.
		 *
		 * A double tap is keyed on the previous press of THIS key rather
		 * than on the last unmodified key emitted (which is how overloadi
		 * does it). That avoids a quick press of some neighbouring key
		 * counting as the first half of a double tap, and avoids treating
		 * the very first press as a double tap because the timestamp is
		 * still zero.
		 */
		pt = &kbd->pending_timeout;

		if (pressed) {
			int is_double = kbd->taphold3_last_code == code &&
				(time - kbd->taphold3_last_time) <
					kbd->config.taphold3_double_timeout;

			kbd->taphold3_last_code = code;
			kbd->taphold3_last_time = time;

			if (is_double) {
				/* Reset so a third tap starts a fresh pair. */
				kbd->taphold3_last_code = 0;
				struct descriptor action =
					kbd->config.descriptors[d->args[2].idx];

				cache_set(kbd, code, &(struct cache_entry){
					.code = code,
					.dl = dl,
					.d = action,
				});

				process_descriptor(kbd, code, &action, dl, 1, time);
				break;
			}

			pt->code = code;
			pt->dl = dl;

			pt->action1 = kbd->config.descriptors[d->args[0].idx];
			pt->expiration = time + kbd->config.taphold3_hold_timeout;
			pt->action2 = kbd->config.descriptors[d->args[1].idx];

			pt->activation_time = time;
			pt->spontaneous = 0;

			schedule_timeout(kbd, pt->expiration);
		} else if (time == kbd->pending_timeout.activation_time) {
			pt->spontaneous = 1;
		}
		break;
	case OP_TIMEOUT:
		pt = &kbd->pending_timeout;

		if (pressed) {
			pt->code = code;
			pt->dl = dl;

			pt->action1 = kbd->config.descriptors[d->args[0].idx];
			pt->expiration = time + d->args[1].timeout;
			pt->action2 = kbd->config.descriptors[d->args[2].idx];

			pt->activation_time = time;
			pt->spontaneous = 0;

			schedule_timeout(kbd, pt->expiration);
		} else if (time == kbd->pending_timeout.activation_time) {
			pt->spontaneous = 1;
		}
		break;
	case OP_COMMAND:
		if (pressed) {
			execute_command(kbd->config.commands[d->args[0].idx].cmd);
			clear_oneshot(kbd);
			update_mods(kbd, -1, 0);
		}
		break;
	case OP_SWAP:
	case OP_SWAPM:
		idx = d->args[0].idx;
		macro = d->op == OP_SWAPM ?  &kbd->config.macros[d->args[1].idx] : NULL;

		if (pressed) {
			size_t i;
			struct cache_entry *ce = NULL;

			if (kbd->layer_state[dl].toggled) {
				deactivate_layer(kbd, dl);
				kbd->layer_state[dl].toggled = 0;

				activate_layer(kbd, 0, idx);
				kbd->layer_state[idx].toggled = 1;
				update_mods(kbd, -1, 0);
			} else if (kbd->layer_state[dl].oneshot_depth) {
				deactivate_layer(kbd, dl);
				kbd->layer_state[dl].oneshot_depth--;

				activate_layer(kbd, 0, idx);
				kbd->layer_state[idx].oneshot_depth++;
				update_mods(kbd, -1, 0);
			} else {
				for (i = 0; i < CACHE_SIZE; i++) {
					uint8_t code = kbd->cache[i].code;
					int layer = kbd->cache[i].layer;
					int type = kbd->config.layers[layer].type;

					if (code && layer == dl && type == LT_NORMAL && layer != 0) {
						ce = &kbd->cache[i];
						break;
					}
				}

				if (ce) {
					ce->d.op = OP_LAYER;
					ce->d.args[0].idx = idx;

					deactivate_layer(kbd, dl);
					activate_layer(kbd, ce->code, idx);

					update_mods(kbd, -1, 0);
				}
			}

			if (macro)
				execute_macro(kbd, dl, macro);
		} else {
			if (macro &&
			    macro->sz == 1 &&
			    macro->entries[0].type == MACRO_KEYSEQUENCE) {
				uint8_t code = macro->entries[0].data;

				send_key(kbd, code, 0);
				update_mods(kbd, -1, 0);
			}
		}

		break;
	case OP_PREFIX: {
		int j;
		struct descriptor *action = &kbd->config.descriptors[d->args[0].idx];

		if (pressed) {
			size_t i;
			struct cache_entry *ce = NULL;

			for (i = 0; i < CACHE_SIZE; i++) {
				uint8_t code = kbd->cache[i].code;
				int layer = kbd->cache[i].layer;
				int type = kbd->config.layers[layer].type;

				if (code && layer == dl && type == LT_NORMAL && layer != 0) {
					ce = &kbd->cache[i];
					break;
				}
			}

			if (ce) {
				ce->d.op = OP_LAYERL;
				ce->d.args[0].idx = -1;
				deactivate_layer(kbd, dl);
				update_mods(kbd, -1, 0);
				kbd->layer_prefix = action;
			}
		}

		break;
	}
	case OP_PREFIXL: {
		int j;

		int nr_layers = d->nr_layers;
		struct descriptor *action = &kbd->config.descriptors[d->args[nr_layers].idx];
		int prefix_oneshot = 0;
		if (pressed) {

			struct layer_trigger *lt = NULL;

			if (kbd->layer_trigger_depth > 0)
				lt = &kbd->layer_trigger_stack[kbd->layer_trigger_depth - 1];

			for (int j = 0; j < nr_layers; j++) {
				idx = d->args[j].idx;
				if (idx == -1)
					break;

				if (is_modifier_layer(kbd, idx)) {
					add_mod_layer(kbd, idx);
					continue;
				}

				kbd->layer_state[idx].toggled = !kbd->layer_state[idx].toggled;

				if (kbd->layer_state[idx].toggled) {
					activate_layer(kbd, code, idx);

					if (lt && lt->toggle_count < MAX_DESCRIPTOR_ARGS)
						lt->toggle_idx[lt->toggle_count++] = idx;

					kbd->layer_prefix = action;
				} else {
					deactivate_layer(kbd, idx);

					if (lt)
						remove_toggle_idx(lt, idx);

					prefix_oneshot = 1;
				}
			}

			if (prefix_oneshot) {
				kbd->layer_prefix = NULL;
				clear_mod_layers(kbd);
			}

			update_mods(kbd, -1, 0);
			clear_oneshot(kbd);
		}
		break;
	}
	case OP_ONESHOTL: {
		int j;
		int should_update_mod = 0;
		int should_clear_mod = 0;

		if (pressed) {
			for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
				idx = d->args[j].idx;

				if (idx == -1)
					break;

				if (kbd->layer_prefix && is_modifier_layer(kbd, idx)) {
					add_mod_layer(kbd, idx);
				} else {
					should_update_mod = 1;
					activate_layer(kbd, code, idx);
				}
			}

			if (should_update_mod)
				update_mods(kbd, dl, 0);

			kbd->oneshot_latch = 1;

			if (kbd->layer_prefix)
				kbd->activation = HELD;

		} else {

			if (kbd->oneshot_latch) {

				if (kbd->layer_prefix)
					kbd->activation = TAP;

				for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
					idx = d->args[j].idx;

					if (idx == -1)
						break;

					if (kbd->layer_prefix && is_modifier_layer(kbd, idx)) {
						add_mod_layer(kbd, idx);
					} else {
						kbd->layer_state[idx].oneshot_depth++;
					}
				}

				if (kbd->config.oneshot_timeout) {
					kbd->oneshot_timeout = time + kbd->config.oneshot_timeout;
					schedule_timeout(kbd, kbd->oneshot_timeout);
				}
			} else {
				for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
					idx = d->args[j].idx;

					if (idx == -1)
						break;

					if (kbd->layer_prefix && is_modifier_layer(kbd, idx)) {
						should_clear_mod = 1;
					} else {
						should_update_mod = 1;
						deactivate_layer(kbd, idx);
					}
				}

				if (should_update_mod)
					update_mods(kbd, dl, 0);

				if (should_clear_mod)
					clear_mod_layers(kbd);
			}
		}

		break;
	}
	case OP_TOGGLEL: {
		int j;
		int should_update_mod = 0;
		int should_clear_mod = 0;

		if (pressed) {
			struct layer_trigger *lt = NULL;

			if (kbd->layer_trigger_depth > 0)
				lt = &kbd->layer_trigger_stack[kbd->layer_trigger_depth - 1];

			for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
				idx = d->args[j].idx;

				if (idx == -1)
					break;

				if (kbd->layer_prefix && is_modifier_layer(kbd, idx)) {
					toggle_mod_layer(kbd, idx);
				} else {

					should_update_mod = 1;
					int was_active = 0;

					for (size_t k = 0; k < MAX_DESCRIPTOR_ARGS; k++) {
						if (kbd->active_idx[k] == idx) {

							if (kbd->layer_state[idx].active > 0)
								deactivate_layer(kbd, idx);
							else
								activate_layer(kbd, code, idx);

							was_active = 1;
							break;
						}
					}

					if (was_active)
						continue;

					if (kbd->layer_state[idx].oneshot_depth) {
						if (kbd->layer_state[idx].toggled)
							kbd->layer_state[idx].toggled = 0;
						deactivate_layer(kbd, idx);
						kbd->layer_state[idx].oneshot_depth--;
					}

					kbd->layer_state[idx].toggled = !kbd->layer_state[idx].toggled;

					if (kbd->layer_state[idx].toggled) {
						activate_layer(kbd, code, idx);
						if (lt && lt->toggle_count < MAX_DESCRIPTOR_ARGS)
							lt->toggle_idx[lt->toggle_count++] = idx;
					} else {
						deactivate_layer(kbd, idx);
						if (lt)
							remove_toggle_idx(lt, idx);
					}
				}
			}

			if (should_update_mod) {
				update_mods(kbd, -1, 0);
				clear_oneshot(kbd);
			}

			if (should_clear_mod)
				clear_mod_layers(kbd);
		}
		break;
	}
	case OP_SWAPL: {
		int j;

		if (pressed) {
			size_t i;
			struct cache_entry *ce = NULL;

			if (kbd->layer_state[dl].toggled) {
				deactivate_layer(kbd, dl);
				kbd->layer_state[dl].toggled = 0;

				for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
					idx = d->args[j].idx;
					if (idx == -1)
						break;

					activate_layer(kbd, code, idx);
					kbd->layer_state[idx].toggled = 1;
				}

				update_mods(kbd, -1, 0);

			} else if (kbd->layer_state[dl].oneshot_depth) {
				deactivate_layer(kbd, dl);
				kbd->layer_state[dl].oneshot_depth--;

				for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
					idx = d->args[j].idx;
					if (idx == -1)
						break;

					activate_layer(kbd, code, idx);
					kbd->layer_state[idx].oneshot_depth++;
				}

				update_mods(kbd, -1, 0);

			} else {
				for (i = 0; i < CACHE_SIZE; i++) {
					uint8_t code = kbd->cache[i].code;
					int layer = kbd->cache[i].layer;
					int type = kbd->config.layers[layer].type;

					if (code && layer == dl && type == LT_NORMAL && layer != 0) {
						ce = &kbd->cache[i];
						break;
					}
				}

				if (ce) {
					ce->d.op = OP_LAYERL;

					for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++)
						ce->d.args[j].idx = d->args[j].idx;

					deactivate_layer(kbd, dl);

					for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
						idx = ce->d.args[j].idx;
						if (idx == -1)
							break;

						activate_layer(kbd, ce->code, idx);
					}
					update_mods(kbd, -1, 0);
				}
			}
		}

		break;
	}
	case OP_LAYERL: {
		int j;
		struct layer_trigger *lt;

		if (pressed) {
			lt = &kbd->layer_trigger_stack[kbd->layer_trigger_depth++];
			lt->other_key_pressed = 0;
			lt->toggle_count = 0;
			kbd->activation = HELD;

			for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
				idx = d->args[j].idx;
				if (idx == -1)
					break;
				kbd->active_idx[j] = -1;

				const char *name = kbd->config.layers[idx].name;
				activate_layer(kbd, code, idx);
				kbd->active_idx[j] = idx;
			}

		} else {
			kbd->activation = NONE;
			int should_clear_toggle = 0;
			lt = &kbd->layer_trigger_stack[kbd->layer_trigger_depth - 1];

			for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
				idx = d->args[j].idx;
				if (idx == -1)
					break;

				const char *name = kbd->config.layers[idx].name;
				deactivate_layer(kbd, idx);

				if (!kbd->layer_state[idx].toggled) {
					kbd->active_idx[j] = -1;
					should_clear_toggle = 1;
				}
			}

			if (should_clear_toggle)
				resolve_toggle_on_layer(kbd, lt, time);

			if (d->args[0].idx == -1)
				kbd->layer_prefix = NULL;

			kbd->layer_trigger_depth--;
		}

		if (kbd->last_pressed_code == code) {
			kbd->inhibit_modifier_guard = 1;
			update_mods(kbd, -1, 0);
			kbd->inhibit_modifier_guard = 0;
		} else {
			update_mods(kbd, -1, 0);
		}

		break;
	}
	case OP_OVERLOADL: {
		int j;
		int nr_layers = d->nr_layers;
		struct layer_trigger *lt;
		action = &kbd->config.descriptors[d->args[nr_layers].idx];

		if (pressed) {
			kbd->overload_start_time = time;

			lt = &kbd->layer_trigger_stack[kbd->layer_trigger_depth++];
			lt->other_key_pressed = 0;
			lt->toggle_count = 0;
			kbd->activation = HELD;

			for (j = 0; j < nr_layers; j++) {
				idx = d->args[j].idx;
				if (idx == -1)
					break;
				kbd->active_idx[j] = -1;
				activate_layer(kbd, code, d->args[j].idx);
				kbd->active_idx[j] = idx;
			}

			update_mods(kbd, -1, 0);

		} else {
			kbd->activation = NONE;
			int should_clear_toggle = 0;
			lt = &kbd->layer_trigger_stack[kbd->layer_trigger_depth - 1];

			for (j = 0; j < nr_layers; j++) {
				idx = d->args[j].idx;
				if (idx == -1)
					break;

				const char *name = kbd->config.layers[idx].name;
				deactivate_layer(kbd, idx);

				if (!kbd->layer_state[idx].toggled) {
					kbd->active_idx[j] = -1;
					should_clear_toggle = 1;
				}
			}

			update_mods(kbd, -1, 0);

			kbd->layer_trigger_depth--;

			if (kbd->last_pressed_code == code && (!kbd->config.overload_tap_timeout || ((time - kbd->overload_start_time) < kbd->config.overload_tap_timeout))) {
				if (action->op == OP_MACRO) {
					/*
					 * Macro release relies on event logic, so we can't just synthesize a
					 * descriptor release.
					 */
					struct macro *macro = &kbd->config.macros[action->args[0].idx];
					execute_macro(kbd, dl, macro);
				} else {
					process_descriptor(kbd, code, action, dl, 1, time);
					process_descriptor(kbd, code, action, dl, 0, time);
				}
			} else {
				if (should_clear_toggle)
					resolve_toggle_on_layer(kbd, lt, time);
			}
		}
		break;
	}
	case OP_OVERLOAD_TIMEOUT_TAPL:
	case OP_OVERLOAD_TIMEOUTL:
		if (pressed) {
			int j;
			int nr_layers = d->nr_layers;
			struct descriptor *action = &kbd->config.descriptors[d->args[nr_layers].idx];

			if (kbd->activation == HELD && d->op == OP_OVERLOAD_TIMEOUT_TAPL) {
				process_descriptor(kbd, code, action, dl, 1, time);
				process_descriptor(kbd, code, action, dl, 0, time);
			} else {
				kbd->pending_overload.code = code;
				kbd->pending_overload.resolve_on_interrupt = d->op == OP_OVERLOAD_TIMEOUT_TAPL;

				kbd->pending_overload.dl = dl;
				kbd->pending_overload.action1 = *action;

				kbd->pending_overload.action2 = (struct descriptor){0};
				kbd->pending_overload.action2.op = OP_LAYERL;

				for (j = 0; j < nr_layers; j++)
					kbd->pending_overload.action2.args[j].idx = d->args[j].idx;

				kbd->pending_overload.action2.args[nr_layers].idx = -1;

				kbd->pending_overload.expiration = time + kbd->config.overload_tap_timeout;

				schedule_timeout(kbd, kbd->pending_overload.expiration);
			}
		}
		break;
	}

	if (pressed)
		kbd->last_pressed_code = code;

	return timeout;
}

struct keyboard *new_keyboard(struct config *config, const struct output *output)
{
	size_t i;
	struct keyboard *kbd;

	kbd = calloc(1, sizeof(struct keyboard));

	kbd->original_config = config;
	memcpy(&kbd->config, kbd->original_config, sizeof(struct config));

	kbd->output = *output;
	kbd->layer_state[0].active = 1;
	kbd->layer_state[0].activation_time = 0;

	if (kbd->config.default_layout[0]) {
		int found = 0;
		for (i = 0; i < kbd->config.nr_layers; i++) {
			struct layer *layer = &kbd->config.layers[i];

			if (layer->type == LT_LAYOUT &&
			    !strcmp(layer->name,
				    kbd->config.default_layout)) {
				kbd->layer_state[i].active = 1;
				kbd->layer_state[i].activation_time = 1;
				found = 1;
				break;
			}
		}

		if (!found)
			keyd_log("\tWARNING: could not find default layout %s.\n",
				kbd->config.default_layout);
	}

	kbd->chord.queue_sz = 0;
	kbd->chord.state = CHORD_INACTIVE;

	return kbd;
}

static int resolve_chord(struct keyboard *kbd)
{
	size_t queue_offset = 0;
	const struct chord *chord = kbd->chord.match;

	kbd->chord.state = CHORD_RESOLVING;

	if (chord) {
		size_t i;
		uint8_t code = 0;

		for (i = 0; i < ARRAY_SIZE(kbd->active_chords); i++) {
			struct active_chord *ac = &kbd->active_chords[i];
			if (!ac->active) {
				ac->active = 1;
				ac->chord = *chord;
				ac->layer = kbd->chord.match_layer;
				code = KEYD_CHORD_1 + i;

				break;
			}
		}

		assert(code);

		queue_offset = chord->sz;
		process_event(kbd, code, 1, kbd->chord.last_code_time);
	}


	kbd_process_events(kbd,
			   kbd->chord.queue + queue_offset,
			   kbd->chord.queue_sz - queue_offset);
	kbd->chord.state = CHORD_INACTIVE;
	return 1;
}

static int abort_chord(struct keyboard *kbd)
{
	kbd->chord.match = NULL;
	return resolve_chord(kbd);
}

static int handle_chord(struct keyboard *kbd,
			uint8_t code, int pressed, long time)
{
	size_t i;
	const long interkey_timeout = kbd->config.chord_interkey_timeout;
	const long hold_timeout = kbd->config.chord_hold_timeout;

	if (code && !pressed) {
		for (i = 0; i < ARRAY_SIZE(kbd->active_chords); i++) {
			struct active_chord *ac = &kbd->active_chords[i];
			uint8_t chord_code = KEYD_CHORD_1 + i;

			if (ac->active) {
				size_t i;
				int nremaining = 0;
				int found = 0;

				for (i = 0; i < ac->chord.sz; i++) {
					if (ac->chord.keys[i] == code) {
						ac->chord.keys[i] = 0;
						found = 1;
					}

					if (ac->chord.keys[i])
						nremaining++;
				}

				if (found) {
					if (nremaining == 0) {
						ac->active = 0;
						process_event(kbd, chord_code, 0, time);
					}

					return 1;
				}
			}
		}
	}

	switch (kbd->chord.state) {
	case CHORD_RESOLVING:
		return 0;
	case CHORD_INACTIVE:
		kbd->chord.queue_sz = 0;
		kbd->chord.match = NULL;
		kbd->chord.start_code = code;

		enqueue_chord_event(kbd, code, pressed, time);
		switch (check_chord_match(kbd, &kbd->chord.match, &kbd->chord.match_layer)) {
			case 0:
				return 0;
			case 3:
			case 1:
				kbd->chord.state = CHORD_PENDING_DISAMBIGUATION;
				kbd->chord.last_code_time = time;
				schedule_timeout(kbd, time + interkey_timeout);
				return 1;
			default:
			case 2:
				kbd->chord.last_code_time = time;

				if (hold_timeout) {
					kbd->chord.state = CHORD_PENDING_HOLD_TIMEOUT;
					schedule_timeout(kbd, time + hold_timeout);
				} else {
					return resolve_chord(kbd);
				}
				return 1;
		}
	case CHORD_PENDING_DISAMBIGUATION:
		if (!code) {
			if ((time - kbd->chord.last_code_time) >= interkey_timeout) {
				if (kbd->chord.match) {
					long timeleft = hold_timeout - interkey_timeout;
					if (timeleft > 0) {
						schedule_timeout(kbd, time + timeleft);
						kbd->chord.state = CHORD_PENDING_HOLD_TIMEOUT;
					} else {
						return resolve_chord(kbd);
					}
				} else {
					return abort_chord(kbd);
				}

				return 1;
			}

			return 0;
		}

		enqueue_chord_event(kbd, code, pressed, time);

		if (!pressed)
			return abort_chord(kbd);

		switch (check_chord_match(kbd, &kbd->chord.match, &kbd->chord.match_layer)) {
			case 0:
				return abort_chord(kbd);
			case 3:
			case 1:
				kbd->chord.last_code_time = time;

				kbd->chord.state = CHORD_PENDING_DISAMBIGUATION;
				schedule_timeout(kbd, time + interkey_timeout);
				return 1;
			default:
			case 2:
				kbd->chord.last_code_time = time;

				if (hold_timeout) {
					kbd->chord.state = CHORD_PENDING_HOLD_TIMEOUT;
					schedule_timeout(kbd, time + hold_timeout);
				} else {
					return resolve_chord(kbd);
				}
				return 1;
		}
	case CHORD_PENDING_HOLD_TIMEOUT:
		if (!code) {
			if ((time - kbd->chord.last_code_time) >= hold_timeout)
				return resolve_chord(kbd);

			return 0;
		}

		enqueue_chord_event(kbd, code, pressed, time);

		if (!pressed) {
			size_t i;

			for (i = 0; i < kbd->chord.match->sz; i++)
				if (kbd->chord.match->keys[i] == code)
					return abort_chord(kbd);
		}

		return 1;
	}

	return 0;
}

int handle_pending_timeout(struct keyboard *kbd, uint8_t event_code, int pressed, long time)
{
	struct pending_timeout pt = kbd->pending_timeout;

	if (!pt.code || (!pressed && pt.code == event_code && time == pt.activation_time))
		return 0;

	if (pt.spontaneous) {
		if ((time >= pt.expiration) || event_code) {
			struct descriptor action = time >= pt.expiration ? pt.action2 : pt.action1;
			kbd->pending_timeout.code = 0;

			process_descriptor(kbd, pt.code, &action, pt.dl, 1, time);
			process_descriptor(kbd, pt.code, &action, pt.dl, 0, time);
		}
	} else if (time >= pt.expiration || (event_code && (pressed || event_code == pt.code))) {
		struct descriptor action = time >= pt.expiration ? pt.action2 : pt.action1;
		kbd->pending_timeout.code = 0;

		cache_set(kbd, pt.code, &(struct cache_entry){
			.code = pt.code,
			.dl = pt.dl,
			.d = action,
		});
		process_descriptor(kbd, pt.code, &action, pt.dl, 1, time);
	}

	return 0;
}

int handle_pending_overload(struct keyboard *kbd, uint8_t code, int pressed, long time)
{
	struct descriptor action;

	if (!kbd->pending_overload.code)
		return 0;

	if (code) {
		struct key_event *ev;

		assert(kbd->pending_overload.queue_sz < ARRAY_SIZE(kbd->pending_overload.queue));

		if (!pressed) {
			size_t i;
			int found = 0;

			for (i = 0; i < kbd->pending_overload.queue_sz; i++)
				if (kbd->pending_overload.queue[i].code == code)
					found = 1;

			/* Propagate key up events for keys which were struck before the pending key. */
			if (!found && code != kbd->pending_overload.code)
				return 0;
		}

		ev = &kbd->pending_overload.queue[kbd->pending_overload.queue_sz];
		ev->code = code;
		ev->pressed = pressed;
		ev->timestamp = time;

		kbd->pending_overload.queue_sz++;
	}


	if (time >= kbd->pending_overload.expiration)
		action = kbd->pending_overload.action2;
	else if (code == kbd->pending_overload.code)
		action = kbd->pending_overload.action1;
	else if (kbd->pending_overload.resolve_on_interrupt && !pressed)
		action = kbd->pending_overload.action2;
	else
		action.op = 0;

	if (action.op) {
		/* Create a copy of the queue on the stack to
		   allow for recursive pending key processing. */
		struct key_event queue[ARRAY_SIZE(kbd->pending_overload.queue)];
		size_t queue_sz = kbd->pending_overload.queue_sz;

		uint8_t code = kbd->pending_overload.code;
		int dl = kbd->pending_overload.dl;

		memcpy(queue, kbd->pending_overload.queue, sizeof kbd->pending_overload.queue);

		kbd->pending_overload.code = 0;
		kbd->pending_overload.queue_sz = 0;

		cache_set(kbd, code, &(struct cache_entry) {
			.d = action,
			.dl = dl,
			.layer = 0,
		});
		process_descriptor(kbd, code, &action, dl, 1, time);

		/* Flush queued events */
		kbd_process_events(kbd, queue, queue_sz);
	}

	return 1;
}

/*
 * `code` may be 0 in the event of a timeout.
 *
 * The return value corresponds to a timeout before which the next invocation
 * of process_event must take place. A return value of 0 permits the
 * main loop to call at liberty.
 */
static long process_event(struct keyboard *kbd, uint8_t code, int pressed, long time)
{
	int dl = -1;
	struct descriptor d;

	if (handle_chord(kbd, code, pressed, time))
		goto exit;

	if (handle_pending_timeout(kbd, code, pressed, time))
		goto exit;

	if (handle_pending_overload(kbd, code, pressed, time))
		goto exit;

	if (kbd->oneshot_timeout && time >= kbd->oneshot_timeout) {
		clear_oneshot(kbd);
		update_mods(kbd, -1, 0);
	}

	if (kbd->active_macro) {
		if (code) {
			kbd->active_macro = NULL;
			update_mods(kbd, -1, 0);
		} else if (time >= kbd->macro_timeout) {
			long execution_time = execute_macro(kbd, kbd->active_macro_layer, kbd->active_macro);

			kbd->macro_timeout = execution_time + time + kbd->macro_repeat_interval;
			schedule_timeout(kbd, kbd->macro_timeout);
		}
	}

	if (code) {
		struct descriptor d;
		int dl = 0;

		if (pressed) {
			/*
			 * Guard against successive key down events
			 * of the same key code. This can be caused
			 * by unorthodox hardware or by different
			 * devices mapped to the same config.
			 */
			if (cache_get(kbd, code))
				goto exit;

			lookup_descriptor(kbd, code, &d, &dl);

			if (kbd->layer_trigger_depth > 0 && d.op != OP_TOGGLEL && d.op != OP_OVERLOAD_TIMEOUT_TAPL && d.op != OP_PREFIXL && d.op != OP_OVERLOADL) {
				struct layer_trigger *lt = &kbd->layer_trigger_stack[kbd->layer_trigger_depth - 1];
				lt->other_key_pressed = 1;
			}
			if (cache_set(kbd, code, &(struct cache_entry) { .d = d, .dl = dl, .layer = 0 }))
				goto exit;
		} else {
			struct cache_entry *ce;
			if (!(ce = cache_get(kbd, code)))
				goto exit;

			cache_set(kbd, code, NULL);

			d = ce->d;
			dl = ce->dl;
		}

		process_descriptor(kbd, code, &d, dl, pressed, time);
	}


exit:
	return calculate_main_loop_timeout(kbd, time);
}


long kbd_process_events(struct keyboard *kbd, const struct key_event *events, size_t n)
{
	size_t i = 0;
	int timeout = 0;
	int timeout_ts = 0;

	while (i != n) {
		const struct key_event *ev = &events[i];

		if (timeout > 0 && timeout_ts <= ev->timestamp) {
			timeout = process_event(kbd, 0, 0, timeout_ts);
			timeout_ts = timeout_ts + timeout;
		} else {
			timeout = process_event(kbd, ev->code, ev->pressed, ev->timestamp);
			timeout_ts = ev->timestamp + timeout;
			i++;
		}
	}

	return timeout;
}

int kbd_eval(struct keyboard *kbd, const char *exp)
{
	if (!strcmp(exp, "reset")) {
		memcpy(&kbd->config, kbd->original_config, sizeof(struct config));
		return 0;
	} else {
		return config_add_entry(&kbd->config, exp);
	}
}
