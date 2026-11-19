/* Actual accepted HID events only: render/vsync and touch polling are not
 * user activity. A held contact/button inhibits the idle fallback until UP. */
static void
umi_idle_note_input(uint32_t type, uint32_t finger)
{
	static bool initialized, enabled;
	static int token = NOTIFY_TOKEN_INVALID;
	static uint32_t contacts, buttons;
	static uint64_t last_post;
	static mach_timebase_info_data_t tb;
	if (!initialized) {
		char args[1024] = ""; size_t len = sizeof(args) - 1;
		initialized = true;
		if (!sysctlbyname("kern.bootargs", args, &len, NULL, 0)) {
			const char *arg = args;
			while ((arg = strstr(arg, "al=1")) != NULL && ((arg != args && arg[-1] != ' ') || (arg[4] && arg[4] != ' ')))
				arg += 4;               /* (not "metal=1") */
			enabled = arg != NULL;
		}
		if (!enabled) return;
		mach_timebase_info(&tb);
		if (notify_register_check("com.hakios.input.activity", &token) != NOTIFY_STATUS_OK) {
			enabled = false; say("hakios: umi-idle input notification failed\n"); return;
		}
		say("hakios: umi-idle real HID activity publisher enabled\n");
	}
	if (!enabled || finger >= 32) return;
	uint32_t bit = 1u << finger;
	if (type == HAKIOS_TOUCH_DOWN) contacts |= bit;
	if (type == HAKIOS_TOUCH_UP) contacts &= ~bit;
	if (type == HAKIOS_BUTTON_DOWN) buttons |= bit;
	if (type == HAKIOS_BUTTON_UP) buttons &= ~bit;
	uint64_t now = mach_absolute_time();
	if (type == HAKIOS_TOUCH_MOVE && (double)(now - last_post) * tb.numer / tb.denom < 100000000) return;
	last_post = now;
	notify_set_state(token, (now & ~(1ULL << 63)) | ((uint64_t)(contacts || buttons) << 63));
	notify_post("com.hakios.input.activity");
}
