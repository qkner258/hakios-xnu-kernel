/* Included by inject.c. Kernel bl=1 is the single gate; old payloads keep the
 * legacy permille sysctl, new payloads also understand CoreBrightness fades. */
static bool umi_bl_bridge;
static pthread_mutex_t umi_bl_bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned umi_bl_fade_ms = 400, umi_bl_bridge_logs;

static int
umi_bl_send(unsigned kind, unsigned value, unsigned fade_ms, const char *source)
{
	uint32_t words[4] = {1, kind, value, fade_ms};
	int result = sysctlbyname("kern.hakios.brightness_command", NULL, NULL, words, sizeof(words));
	int error = result ? errno : 0;
	if (umi_bl_bridge_logs++ < 96)
		say("hakios: umi-bl %s kind=%u value=%u fade_ms=%u result=%d\n", source, kind, value, fade_ms, error);
	return error;
}

/* Called after the original implementation, outside any CoreBrightness lock.
 * A physical minimum is a floor, not a new slider value or an off request. */
static bool
umi_bl_property(id prop, id key)
{
	if (!umi_bl_bridge || !prop || !key || CFGetTypeID((CFTypeRef)key) != CFStringGetTypeID()) return false;
	bool period = CFEqual(key, CFSTR("DisplayBrightnessFadePeriod"));
	bool factor = CFEqual(key, CFSTR("DisplayBrightnessFactor"));
	bool factor_fade = CFEqual(key, CFSTR("DisplayBrightnessFactorWithFade"));
	bool minimum = CFEqual(key, CFSTR("BrightnessMinPhysical"));
	bool minimum_fade = CFEqual(key, CFSTR("BrightnessMinPhysicalWithFade"));
	if (!period && !factor && !factor_fade && !minimum && !minimum_fade) return false;
	if (CFGetTypeID((CFTypeRef)prop) != CFNumberGetTypeID()) return false;
	double value = -1;
	if (!CFNumberGetValue((CFNumberRef)prop, kCFNumberDoubleType, &value) || !(value >= 0)) return false;
	if (value > (period ? 10 : (minimum || minimum_fade) ? 500 : 1)) return false;
	pthread_mutex_lock(&umi_bl_bridge_lock);
	int error = 0;
	if (period) umi_bl_fade_ms = (unsigned)(value * 1000 + 0.5);
	else {
		char name[80] = "?";
		CFStringGetCString((CFStringRef)key, name, sizeof(name), kCFStringEncodingUTF8);
		error = umi_bl_send((minimum || minimum_fade) ? 3 : 2, (unsigned)(value * 1000 + 0.5),
		    (factor_fade || minimum_fade) ? umi_bl_fade_ms : 0, name);
	}
	pthread_mutex_unlock(&umi_bl_bridge_lock);
	return error == 0;
}
