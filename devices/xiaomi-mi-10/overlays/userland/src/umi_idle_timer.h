/* iOS 16.5 (20F66), al=1. Observe the native effective timer; supplement missing
 * attention timeout delivery, not SpringBoard's policy or Settings preferences.
 * All tracked state is confined to the main queue. Slots hold zeroing weak refs.
 */
#include <execinfo.h>
extern id objc_storeWeak(id *, id);
extern id objc_loadWeakRetained(id *);
struct umi_idle_slot { id weak_timer, configuration; uint64_t reset_at; unsigned fired, config_resets, attention_resets, reconfigures; unsigned long long last_attention; };
static struct umi_idle_slot umi_idle_slots[8];
static Ivar umi_idle_attention_ivar, umi_idle_monitor_ivar;
static id umi_idle_weak_coordinator;
static unsigned umi_idle_input_count;
static int umi_idle_notify = NOTIFY_TOKEN_INVALID;
static bool umi_idle_held;
static unsigned umi_idle_log_count;
static mach_timebase_info_data_t umi_idle_timebase;
static void (*umi_idle_orig_reconfigure)(id, SEL, BOOL);
static void (*umi_idle_orig_timeout)(id, SEL, id, id);
static void (*umi_idle_orig_attention)(id, SEL, id, unsigned long long, double);
static void (*umi_idle_orig_coordinator)(id, SEL, id, id);

static id umi_idle_get(id object, const char *name)
{
	return ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName(name));
}
static bool umi_idle_bool(id object, const char *name)
{
	return ((BOOL (*)(id, SEL))objc_msgSend)(object, sel_registerName(name));
}
static void umi_idle_describe(id object, char *text, size_t size)
{
	if (!object) { snprintf(text, size, "<unset>"); return; }
	id desc = umi_idle_get(object, "description");
	if (!desc || !CFStringGetCString((CFStringRef)desc, text, size, kCFStringEncodingUTF8))
		snprintf(text, size, "<description too long or invalid UTF-8>");
}
/* 20F66 DBSSettingsController screenLock:/setScreenLock:specifier: and
 * SBIdleTimerGlobalStateMonitor all use MCFeatureAutoLockTime = maxInactivity.
 * CFPreferences SBAutoLockTime is not the setting used by these callers. */
static void umi_idle_log_monitor(const char *reason)
{
	id coordinator = objc_loadWeakRetained(&umi_idle_weak_coordinator);
	id monitor = coordinator && umi_idle_monitor_ivar ? object_getIvar(coordinator, umi_idle_monitor_ivar) : nil;
	if (monitor) {
		char value[80];
		umi_idle_describe(umi_idle_get(monitor, "autoLockTimeout"), value, sizeof(value));
		say("hakios: umi-idle effective reason=%s autoLockTimeout=%s onAC=%u dontDimOrLockOnAC=%u disableAutoDim=%u dontLockEver=%u disableAttentionAwareness=%u batterySaver=%u thermalBlocked=%u coordinatorDisabled=%u\n",
		    reason, value, umi_idle_bool(monitor, "isOnACPower"), umi_idle_bool(monitor, "dontDimOrLockOnAC"),
		    umi_idle_bool(monitor, "isAutoDimDisabled"), umi_idle_bool(monitor, "dontLockEver"),
		    umi_idle_bool(monitor, "disableAttentionAwareness"), umi_idle_bool(monitor, "isBatterySaverModeActive"),
		    umi_idle_bool(monitor, "isThermalBlocked"), umi_idle_bool(coordinator, "isIdleTimerDisabled"));
	} else {
		say("hakios: umi-idle effective reason=%s coordinator/monitor not observed yet\n", reason);
	}
	if (coordinator) CFRelease((CFTypeRef)coordinator);
}
static void umi_idle_log_settings(const char *reason)
{
	Class cls = objc_getClass("MCProfileConnection");
	if (!cls || !class_getClassMethod(cls, sel_registerName("sharedConnection")) ||
	    !class_getInstanceMethod(cls, sel_registerName("userValueForSetting:")) ||
	    !class_getInstanceMethod(cls, sel_registerName("effectiveValueForSetting:"))) {
		say("hakios: umi-idle MC API unavailable reason=%s\n", reason);
		return;
	}
	id connection = umi_idle_get((id)cls, "sharedConnection");
	id key = (id)CFSTR("maxInactivity");
	id user = ((id (*)(id, SEL, id))objc_msgSend)(connection, sel_registerName("userValueForSetting:"), key);
	id effective = ((id (*)(id, SEL, id))objc_msgSend)(connection, sel_registerName("effectiveValueForSetting:"), key);
	char u[80], e[80];
	umi_idle_describe(user, u, sizeof(u)); umi_idle_describe(effective, e, sizeof(e));
	say("hakios: umi-idle MC reason=%s connection=%p maxInactivity user=%s effective=%s seconds (2147483647=Never)\n",
	    reason, connection, u, e);
	umi_idle_log_monitor(reason);
}
static void umi_idle_observe_settings(void)
{
	static int tokens[2] = {NOTIFY_TOKEN_INVALID, NOTIFY_TOKEN_INVALID};
	static const char *names[] = {
		"com.apple.managedconfiguration.settingschanged",
		"com.apple.managedconfiguration.effectivesettingschanged"
	};
	for (unsigned i = 0; i < 2; i++) {
		const char *name = names[i];
		uint32_t result = notify_register_dispatch(name, &tokens[i], dispatch_get_main_queue(), ^(int token) {
			(void)token;
			say("hakios: umi-idle settings notification=%s\n", name);
			/* Allow the framework's own notification handlers to update the
			 * cached policy before sampling it. No preference writes or resets. */
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4), dispatch_get_main_queue(), ^{
				umi_idle_log_settings(name);
			});
		});
		say("hakios: umi-idle observe %s result=%u\n", name, result);
	}
	/* Run after SpringBoard's constructor has returned. */
	dispatch_async(dispatch_get_main_queue(), ^{ umi_idle_log_settings("startup"); });
}

static struct umi_idle_slot *umi_idle_slot_for(id timer, bool create)
{
	struct umi_idle_slot *empty = NULL;
	for (unsigned i = 0; i < 8; i++) {
		struct umi_idle_slot *slot = &umi_idle_slots[i];
		id obj = objc_loadWeakRetained(&slot->weak_timer);
		bool match = obj == timer;
		if (!obj && !empty) empty = slot;
		if (obj) CFRelease((CFTypeRef)obj);
		if (match) return slot;
	}
	if (!create || !empty) return NULL;
	if (empty->configuration) CFRelease((CFTypeRef)empty->configuration);
	empty->configuration = nil;
	objc_storeWeak(&empty->weak_timer, timer);
	empty->reset_at = mach_absolute_time();
	empty->fired = empty->config_resets = empty->attention_resets = empty->reconfigures = 0;
	empty->last_attention = 0;
	return empty;
}
static void umi_idle_reconfigure(id self, SEL cmd, BOOL reset)
{
	umi_idle_orig_reconfigure(self, cmd, reset);
	if (!pthread_main_np()) return; /* never guess cross-queue object lifetimes */
	struct umi_idle_slot *slot = umi_idle_slot_for(self, true);
	id configuration = umi_idle_get(self, "descriptor");
	if (slot) {
		slot->reconfigures++;
		/* reset:YES also comes from SBBacklightIdleTimer.reset with unchanged
		 * policy. It is not evidence of user activity or a configuration change.
		 * SBIdleTimerDescriptor.isEqual: compares policy, excluding audit reasons. */
		if (!((BOOL (*)(id, SEL, id))objc_msgSend)(configuration, sel_registerName("isEqual:"), slot->configuration)) {
			if (slot->configuration) CFRelease((CFTypeRef)slot->configuration);
			slot->configuration = umi_idle_get(configuration, "copy");
			slot->reset_at = mach_absolute_time(); slot->fired = 0; slot->config_resets++;
		} else if (reset && !umi_idle_held) {
			/* One idle-time stack identifies the upstream reset source on hardware. */
			static bool traced;
			double idle = (double)(mach_absolute_time() - slot->reset_at) * umi_idle_timebase.numer / umi_idle_timebase.denom / 1e9;
			if (!traced && idle > 2) {
				void *pcs[12]; int count = backtrace(pcs, 12); traced = true;
				for (int i = 0; i < count; i++)
					say("hakios: umi-idle unchanged-reset stack[%d]=%#lx (unslid)\n", i, (uintptr_t)pcs[i] - slide);
			}
		}
	}
	if (umi_idle_log_count++ < 80) {
		char desc[400];
		umi_idle_describe(umi_idle_get(self, "descriptor"), desc, sizeof(desc));
		say("hakios: umi-idle configure timer=%p reset=%u active=%u disabled=%u %s\n",
		    self, reset, umi_idle_bool(self, "isActivated"), umi_idle_bool(self, "isDisabled"), desc);
	}
}
static void umi_idle_timeout(id self, SEL cmd, id timer, id timeout)
{
	unsigned long long ident = ((unsigned long long (*)(id, SEL))objc_msgSend)(timeout, sel_registerName("identifier"));
	if (pthread_main_np()) {
		struct umi_idle_slot *slot = umi_idle_slot_for(self, false);
		if (slot && ident < 32) {
			/* Native delivery can arrive after our grace interval. Deliver once. */
			if (slot->fired & (1u << ident)) return;
			slot->fired |= 1u << ident;
		}
	}
	say("hakios: umi-idle native timeout timer=%p id=%llu\n", self, ident);
	umi_idle_orig_timeout(self, cmd, timer, timeout);
}
static void umi_idle_attention(id self, SEL cmd, id timer, unsigned long long event, double at)
{
	if (pthread_main_np()) {
		struct umi_idle_slot *slot = umi_idle_slot_for(self, false);
		if (slot) { slot->reset_at = mach_absolute_time(); slot->fired = 0; slot->attention_resets++; slot->last_attention = event; }
	}
	umi_idle_orig_attention(self, cmd, timer, event, at);
}
static void umi_idle_coordinator(id self, SEL cmd, id descriptor, id reason)
{
	if (pthread_main_np()) {
		id previous = objc_loadWeakRetained(&umi_idle_weak_coordinator);
		bool first = previous != self;
		if (previous) CFRelease((CFTypeRef)previous);
		objc_storeWeak(&umi_idle_weak_coordinator, self);
		if (first) dispatch_async(dispatch_get_main_queue(), ^{ umi_idle_log_monitor("coordinator"); });
	}
	char desc[400], why[160];
	umi_idle_describe(descriptor, desc, sizeof(desc));
	umi_idle_describe(reason, why, sizeof(why));
	if (umi_idle_log_count++ < 80)
		say("hakios: umi-idle policy reason=%s descriptor=%s\n", why, desc);
	umi_idle_orig_coordinator(self, cmd, descriptor, reason);
}

static void umi_idle_input(uint64_t state)
{
	umi_idle_held = (state >> 63) != 0;
	umi_idle_input_count++;
	for (unsigned i = 0; i < 8; i++) {
		struct umi_idle_slot *slot = &umi_idle_slots[i];
		id timer = objc_loadWeakRetained(&slot->weak_timer);
		if (!timer) continue;
		slot->reset_at = mach_absolute_time(); slot->fired = 0;
		if (umi_idle_bool(timer, "isActivated") && !umi_idle_bool(timer, "isDisabled"))
			((void (*)(id, SEL))objc_msgSend)(timer, sel_registerName("reset"));
		CFRelease((CFTypeRef)timer);
	}
}

static void umi_idle_tick(void)
{
	static unsigned ticks;
	bool report = ++ticks % 120 == 0;
	unsigned tracked = 0;
	for (unsigned i = 0; i < 8; i++) {
		struct umi_idle_slot *slot = &umi_idle_slots[i];
		id self = objc_loadWeakRetained(&slot->weak_timer);
		if (!self) continue;
		tracked++;
		id timer = object_getIvar(self, umi_idle_attention_ivar);
		id desc = umi_idle_get(timer, "descriptor");
		id timeouts = umi_idle_get(desc, "timeouts");
		unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(timeouts, sel_registerName("count"));
		uint64_t now = mach_absolute_time();
		double elapsed = (double)(now - slot->reset_at) * umi_idle_timebase.numer / umi_idle_timebase.denom / 1e9;
		if (report) {
			say("hakios: umi-idle status timer=%p active=%u disabled=%u manual=%u held=%u fired=%#x idle=%.2f backend=%p enabled=%u timeouts=%lu resets(config/attention)=%u/%u lastAttention=%llu inputs=%u reconfigures=%u\n",
			    self, umi_idle_bool(self, "isActivated"), umi_idle_bool(self, "isDisabled"),
			    umi_idle_bool(self, "requiresManualReset"), umi_idle_held, slot->fired, elapsed,
			    timer, umi_idle_bool(timer, "isEnabled"), count, slot->config_resets, slot->attention_resets,
			    slot->last_attention, umi_idle_input_count, slot->reconfigures);
			for (unsigned long j = 0; j < count && j < 8; j++) {
				id timeout = ((id (*)(id, SEL, unsigned long))objc_msgSend)(timeouts, sel_registerName("objectAtIndex:"), j);
				say("hakios: umi-idle deadline id=%llu seconds=%.3f\n",
				    ((unsigned long long (*)(id, SEL))objc_msgSend)(timeout, sel_registerName("identifier")),
				    ((double (*)(id, SEL))objc_msgSend)(timeout, sel_registerName("duration")));
			}
		}
		if (!umi_idle_bool(self, "isActivated") || umi_idle_bool(self, "isDisabled") ||
		    umi_idle_bool(self, "requiresManualReset")) {
			slot->reset_at = now; slot->fired = 0; goto next;
		}
		if (umi_idle_held) { slot->reset_at = now; goto next; }
		if (!timer || !umi_idle_bool(timer, "isEnabled") || count > 8) goto next;
		for (unsigned long j = 0; j < count; j++) {
			id timeout = ((id (*)(id, SEL, unsigned long))objc_msgSend)(timeouts, sel_registerName("objectAtIndex:"), j);
			double duration = ((double (*)(id, SEL))objc_msgSend)(timeout, sel_registerName("duration"));
			unsigned long long ident = ((unsigned long long (*)(id, SEL))objc_msgSend)(timeout, sel_registerName("identifier"));
			/* The native list already implements MC/Settings, Never and assertions.
			 * IDs are warn=1, quick-unwarn=2, expire=3, verified in 20F66. */
			if (ident < 1 || ident > 3 || !(duration > 0 && duration < 86400) ||
			    (slot->fired & (1u << ident)) || elapsed < duration + 0.5) continue;
			slot->fired |= 1u << ident;
			say("hakios: umi-idle missing native timeout: timer=%p id=%llu idle=%.2f deadline=%.2f; deliver native callback\n",
			    self, ident, elapsed, duration);
			umi_idle_orig_timeout(self, sel_registerName("idleTimer:attentionLostTimeoutDidExpire:"), timer, timeout);
			break; /* the callback may change the descriptor or disable the timer */
		}
next:
		CFRelease((CFTypeRef)self);
	}
	if (report) umi_idle_log_monitor("status");
	if (report && !tracked) say("hakios: umi-idle status: no configured SBBacklightIdleTimer observed\n");
}

/* 20F66 -[SBTelephonyManager _serverConnectionDidError:] only schedules
 * _handleTelephonyDaemonRestart 1.0 s later. com.apple.commcenter.xpc is not
 * registered here, so every reconnect errors again: a permanent 1 Hz loop of
 * __NSFireDelayedPerform -> _handleTelephonyDaemonRestart -> handleCallControlFailure:
 * -> updateCalls -> updateSpringBoard -> resetIdleTimer (v8 unchanged-reset stack).
 * SBBacklightIdleTimer.reset posts idleTimerDidRefresh:, and SBIdleTimerPolicyAggregator
 * then calls _undimFromSource:8 while screenIsDim: the dim -> full -> off bounce.
 * A real CommCenter restart still arrives by kCTTelephonyCenterDaemonRestartNotification. */
static void umi_idle_telephony_error(id self, SEL cmd, id error)
{
	static bool said;
	(void)self; (void)cmd;
	if (!said) { said = true; say("hakios: umi-idle telephony reconnect retry dropped (no CommCenter): %p\n", error); }
}

static void umi_idle_install(void)
{
	char args[1024] = ""; size_t len = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0)) return;
	const char *arg = args;
	while ((arg = strstr(arg, "al=1")) != NULL && ((arg != args && arg[-1] != ' ') || (arg[4] && arg[4] != ' ')))
		arg += 4;                       /* (not "metal=1") */
	if (!arg) return;
	umi_idle_monitor_ivar = class_getInstanceVariable(objc_getClass("SBIdleTimerGlobalCoordinator"), "_globalStateMonitor");
	umi_idle_observe_settings();
	Class cls = objc_getClass("SBBacklightIdleTimer");
	umi_idle_attention_ivar = cls ? class_getInstanceVariable(cls, "_attentionAwareTimer") : NULL;
	if (!umi_idle_attention_ivar) { say("hakios: umi-idle unavailable: attention timer ivar\n"); return; }
	/* Check every entry before changing any, so an ABI mismatch leaves stock code. */
	struct { const char *cls, *sel; uintptr_t address; } methods[] = {
		{"SBBacklightIdleTimer", "_reconfigureAttentionClientAndReset:", 0x1c4d6c1ccUL},
		{"SBBacklightIdleTimer", "idleTimer:attentionLostTimeoutDidExpire:", 0x1c4d6b6f0UL},
		{"SBBacklightIdleTimer", "idleTimerDidReset:forUserAttention:at:", 0x1c4d6bafcUL},
		{"SBIdleTimerGlobalCoordinator", "_setIdleTimerWithDescriptor:forReason:", 0x1c4a2dfc0UL},
		{"SBTelephonyManager", "_serverConnectionDidError:", 0x1c4b278dcUL}
	};
	for (unsigned i = 0; i < 5; i++) {
		Method m = class_getInstanceMethod(objc_getClass(methods[i].cls), sel_registerName(methods[i].sel));
		if (!m || (uintptr_t)method_getImplementation(m) != methods[i].address + slide) {
			say("hakios: umi-idle ABI mismatch %s; off\n", methods[i].sel); return;
		}
	}
	mach_timebase_info(&umi_idle_timebase);
	if (notify_register_dispatch("com.hakios.input.activity", &umi_idle_notify, dispatch_get_main_queue(), ^(int token) {
		uint64_t state = 0;
		if (notify_get_state(token, &state) != NOTIFY_STATUS_OK) return;
		umi_idle_input(state);
	}) != NOTIFY_STATUS_OK) { say("hakios: umi-idle input notification unavailable; off\n"); return; }
	umi_idle_orig_reconfigure = (void *)replace_method(methods[0].cls, methods[0].sel, methods[0].address, (IMP)umi_idle_reconfigure);
	umi_idle_orig_timeout = (void *)replace_method(methods[1].cls, methods[1].sel, methods[1].address, (IMP)umi_idle_timeout);
	umi_idle_orig_attention = (void *)replace_method(methods[2].cls, methods[2].sel, methods[2].address, (IMP)umi_idle_attention);
	umi_idle_orig_coordinator = (void *)replace_method(methods[3].cls, methods[3].sel, methods[3].address, (IMP)umi_idle_coordinator);
	replace_method(methods[4].cls, methods[4].sel, methods[4].address, (IMP)umi_idle_telephony_error);
	static dispatch_source_t timer;
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC / 4, NSEC_PER_SEC / 20);
	dispatch_source_set_event_handler(timer, ^{ umi_idle_tick(); });
	dispatch_resume(timer);
	say("hakios: umi-idle al=1 v9: unchanged reconfiguration preserves idle; HID/attention/configuration reset epoch; no 1 Hz telephony reset\n");
}
