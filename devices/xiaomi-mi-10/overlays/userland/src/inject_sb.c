/*
 * libhakios.sb.dylib: code of ours inside SpringBoard (see inject.c for how it gets there).
 *
 *   - The phone is "activated". SpringBoard asks mobileactivationd, which is not running (and
 *     could not talk to Apple anyway); without an answer it treats the device as bricked and
 *     shows the first-boot setup ("hello") instead of the lock screen.
 *   - The setup assistant is never needed, whatever the reason: nobody is going to go through
 *     it here.
 *   - SpringBoard starts a few seconds late where the kernel's boot-args say so (sb_delay=N;
 *     "xl run" does for the phone). /private/var is empty at every boot, so the applications
 *     are registered anew each time, one after another; one that is registered after
 *     SpringBoard has laid out the home screen ends up on a later page, and which one that is
 *     differs from boot to boot. On the phone that happens; under QEMU, where everything takes
 *     five times as long, it does not -- and there the delay is harmful: for a reason not yet
 *     found the window server then never takes our touch screen service for its own.
 *     (A wrapper program in the launchd job would be the obvious way to wait; launchd panics
 *     the system when a system job runs anything that is not Apple's.)
 *   - The picture SpringBoard keeps of an application that leaves the screen (for the application
 *     switcher and the next launch) is saved as JPEG or PNG, not as an ASTC texture: see
 *     snapshot_output_format().
 *   - Where CoreAnimation draws with its software renderer (QEMU; the phone without gpu=1) the
 *     "vibrant" views draw their content as it is: see plain_vibrancy().
 *   - A press of the side button that is long over is not taken for a long press (Siri) because
 *     SpringBoard was busy waking the display: see lock_long_press().
 *   - A long press on the lock screen's wallpaper does not open the poster switcher, which the
 *     window server here shows as a black screen: see poster_switcher_allowed().
 *   - Darwin notifications as a remote control, for scripts (rwtouch notify NAME):
 *       com.hakios.unlock      what the lock screen does after "swipe up to open"
 *       com.hakios.layers      write down the layers SpringBoard has on the screen: see layers()
 *       com.hakios.switcher    open the switcher of running applications: see switcher()
 *     In any other program given this library (INJECT in build.sh) only the last of them is
 *     answered, with that program's layers.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include <Block.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* where what is said goes: the standard error, which is the console's for SpringBoard (an application's is nowhere) */
static int say_fd = 2;

static void
say(const char *format, ...)
{
	char line[1280];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(say_fd, line, (size_t)n);
}

/*
 * Boot-arg touch_lat=1: a line for every turn of SpringBoard's main run loop -- when it woke up (a
 * touch, a timer, the display link ...), when CoreAnimation's commit of what that turn changed
 * began and when it was done (our observers just before and after its own, of order 2000000), in
 * microseconds of mach_absolute_time, as the other "tl:" lines (tools/latency.py, tools/frames.py):
 *   tl: a WOKE COMMIT DONE
 * (a commit with nothing in it takes some microseconds; one that sends changes, a good deal more).
 * A turn longer than 100 ms is a hitch, and ends with one more line -- never one line per sample:
 *   tl: A turn_ms=684 commit_ms=0 samples=13 top=NAME:COUNT,... last=NAME|NAME|NAME|...
 * (the frames the main thread was sampled in most often, and the whole walk of the last sample)
 */
#define CA_COMMIT_ORDER 2000000
#define STALL_LONG_TURN_US 100000       /* a turn this long is the hitch: one summary line for it */
#define STALL_SAMPLES      64           /* samples kept per turn: at 25 ms into it, then every 50 ms */
#define STALL_STACK        8            /* frames kept per sample: enough to see past the waiting call */
static uint64_t main_woke, commit_began;
static volatile uint64_t turn_began;    /* mach_absolute_time at the start of the turn; 0 between turns (stall_watch()) */
static uintptr_t main_stack_top, main_stack_bottom;
static mach_timebase_info_data_t timebase;

/* What the sampler saw in the turn being measured, read by the main thread when the turn ends.
 * turn: the mach_absolute_time that turn woke at (turn_began), so a sample taken just as the turn
 * ended (and written into the next one's list) is never counted for the wrong turn.
 * stall_seq is odd while a sample is half-written: the reader waits for it (stall_samples_of). */
struct stall_sample {
	uint64_t turn;
	unsigned depth;
	uintptr_t frame[STALL_STACK];
};
static struct stall_sample stall_samples[STALL_SAMPLES];
static volatile uint32_t stall_seq;     /* 2 x (samples written) + (1 while writing one) */

static uint64_t
now_us(void)
{
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000;
}

/*
 * One frame: "image+offset", or its symbol when the image exports one. iOS 16.5's shared cache
 * answers dladdr for a private symbol with the literal "<redacted>", not NULL, and
 * "offset from dli_saddr" is then meaningless too; that is no symbol either, so the frame falls
 * back to the image and offset, which still identifies it (against the 20F66 symbol tables).
 */
static void
stall_frame(uintptr_t pc, char *out, size_t size)
{
	Dl_info info;
	const char *image;

	if (dladdr((void *)pc, &info) != 0 && info.dli_fname != NULL) {
		const char *symbol = info.dli_sname;

		if (symbol != NULL && (symbol[0] == 0 || strcmp(symbol, "<redacted>") == 0)) {
			symbol = NULL;
		}
		if (symbol != NULL && pc - (uintptr_t)info.dli_saddr < 0x4000) {
			snprintf(out, size, "%s", symbol);
			return;
		}
		image = strrchr(info.dli_fname, '/');
		snprintf(out, size, "%s+%#lx", image != NULL ? image + 1 : info.dli_fname, pc - (uintptr_t)info.dli_fbase);
		return;
	}
	snprintf(out, size, "%#lx", pc);
}

/* The main thread's program counters, the one it is in first; it is stopped only for this walk. */
static unsigned
stall_stack(thread_t thread, uintptr_t *pcs, unsigned max)
{
	arm_thread_state64_t state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	unsigned depth = 0;

	if (thread_suspend(thread) != KERN_SUCCESS) {
		return 0;
	}
	if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS) {
		uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state), fp = (uintptr_t)arm_thread_state64_get_fp(state);

		pcs[depth++] = pc;
		while (depth < max && fp >= main_stack_bottom && fp + 16 <= main_stack_top && (fp & 15) == 0) {
			uintptr_t next = ((uintptr_t *)fp)[0], lr = ((uintptr_t *)fp)[1];

			if (lr == 0) {
				break;
			}
			pcs[depth++] = lr;
			if (next <= fp) {
				break;
			}
			fp = next;
		}
	}
	thread_resume(thread);
	return depth;
}

/*
 * One sample of the main thread, kept for the summary of its turn. Nothing is printed here: one
 * line per long turn is what a reader can count (the per-sample "tl: t" lines were 540 in a session).
 */
static void
stall_sample(thread_t thread, uint64_t began)
{
	uintptr_t pcs[STALL_STACK];
	unsigned depth = stall_stack(thread, pcs, STALL_STACK);
	uint32_t seq;
	unsigned slot;

	if (depth == 0) {
		return;
	}
	seq = __atomic_load_n(&stall_seq, __ATOMIC_RELAXED);
	slot = seq >> 1;
	__atomic_store_n(&stall_seq, seq + 1, __ATOMIC_RELAXED);
	if (slot < STALL_SAMPLES) {
		stall_samples[slot].turn = began;
		for (unsigned i = 0; i < depth; i++) {
			stall_samples[slot].frame[i] = pcs[i];
		}
		stall_samples[slot].depth = depth;
	}
	__atomic_store_n(&stall_seq, seq + 2, __ATOMIC_RELAXED);
}

/* The samples of this turn, copied out; the sampler's half-written one is waited for (microseconds). */
static unsigned
stall_samples_of(uint64_t began, struct stall_sample *out)
{
	for (unsigned tries = 0; tries < 1000; tries++) {
		uint32_t before = __atomic_load_n(&stall_seq, __ATOMIC_ACQUIRE), after;
		unsigned written, count = 0;

		if ((before & 1) != 0) {
			continue;
		}
		written = before >> 1;
		if (written > STALL_SAMPLES) {
			written = STALL_SAMPLES;
		}
		for (unsigned i = 0; i < written; i++) {
			if (stall_samples[i].turn == began) {
				out[count++] = stall_samples[i];
			}
		}
		after = __atomic_load_n(&stall_seq, __ATOMIC_ACQUIRE);
		if (after == before) {
			return count;
		}
	}
	return 0;
}

/*
 * One line about a turn that took long: how long it took, the three frames the main thread was
 * sampled in most often (the frame it is in at the sample: the waiting system call at the top of
 * one says little on its own, which is why the frames of the last sample are given too), and the
 * number of samples, so a turn that ended before the sampler looked at it is not mistaken for one
 * that was idle.
 */
static void
stall_summary(uint64_t began, uint64_t turn_ms, uint64_t commit_ms)
{
	static struct stall_sample samples[STALL_SAMPLES];  /* main thread only, and a few KB */
	uintptr_t frame[STALL_SAMPLES];
	unsigned count[STALL_SAMPLES];
	bool taken[STALL_SAMPLES];
	unsigned total = stall_samples_of(began, samples), distinct = 0, shown = 0;
	char line[1024], name[160];
	int at;

	for (unsigned i = 0; i < total; i++) {
		unsigned slot;

		for (slot = 0; slot < distinct && frame[slot] != samples[i].frame[0]; slot++) {
		}
		if (slot == distinct) {
			frame[distinct] = samples[i].frame[0];
			count[distinct] = 0;
			taken[distinct] = false;
			distinct++;
		}
		count[slot]++;
	}
	at = snprintf(line, sizeof(line), "tl: A turn_ms=%llu commit_ms=%llu samples=%u top=", turn_ms, commit_ms, total);
	for (unsigned n = 0; n < 3 && n < distinct; n++) {
		unsigned best = STALL_SAMPLES;

		for (unsigned i = 0; i < distinct; i++) {
			if (!taken[i] && (best == STALL_SAMPLES || count[i] > count[best])) {
				best = i;
			}
		}
		if (best == STALL_SAMPLES) {
			break;
		}
		taken[best] = true;
		stall_frame(frame[best], name, sizeof(name));
		at += snprintf(line + at, sizeof(line) - at, "%s%s:%u", shown++ ? "," : "", name, count[best]);
	}
	at += snprintf(line + at, sizeof(line) - at, "%s", shown ? "" : "-");
	at += snprintf(line + at, sizeof(line) - at, " last=");
	if (total > 0) {
		const struct stall_sample *last = &samples[total - 1];

		for (unsigned k = 0; k < last->depth && at < (int)sizeof(line) - 96; k++) {
			stall_frame(last->frame[k], name, sizeof(name));
			at += snprintf(line + at, sizeof(line) - at, "%s%s", k ? "|" : "", name);
		}
	} else {
		at += snprintf(line + at, sizeof(line) - at, "-");
	}
	say("%s\n", line);
}

static void
main_loop_turn(__unused CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
	if (activity == kCFRunLoopAfterWaiting) {
		main_woke = now_us();
		turn_began = mach_absolute_time();
		__atomic_store_n(&stall_seq, 0, __ATOMIC_RELEASE); /* a new turn: forget the last one's samples */
	} else if (activity == kCFRunLoopBeforeWaiting && info == NULL) {
		commit_began = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && main_woke != 0) {
		uint64_t began = turn_began;
		uint64_t now = now_us();

		say("tl: a %llu %llu %llu\n", main_woke, commit_began, now);
		if (commit_began >= main_woke + STALL_LONG_TURN_US) {
			stall_summary(began, (commit_began - main_woke) / 1000, (now - commit_began) / 1000);
		}
		main_woke = 0;
		turn_began = 0;
	}
}

/*
 * What SpringBoard's main thread is doing when a turn of its run loop takes long (with touch_lat=1): turns of 100 to 300 ms come
 * 40 to 100 ms after a finger comes down, before their commit, and that is what jerks at the start of a swipe. A thread looks every
 * 10 ms; at 25 ms into a turn, and every 50 ms after, it stops the main thread for the moment of walking its frame pointers, and
 * keeps the walk. The line about it comes from main_loop_turn when the turn ends (stall_summary).
 */
static void *
stall_watch(void *argument)
{
	thread_t main_thread = (thread_t)(uintptr_t)argument;
	uint64_t said_turn = 0, next_ms = 25;

	for (;;) {
		uint64_t began = turn_began;

		usleep(10000);
		if (began == 0 || began != turn_began) {
			continue;
		}
		uint64_t ms = (mach_absolute_time() - began) * timebase.numer / timebase.denom / 1000000;

		if (began != said_turn) {
			said_turn = began;
			next_ms = 25;
		}
		if (ms >= next_ms && ms < 2000) {
			stall_sample(main_thread, began);
			next_ms = ms + 50;
		}
	}
	return NULL;
}

static void
watch_main_loop(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *arg;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (arg = strstr(args, "touch_lat=1")) == NULL ||
	    (arg != args && arg[-1] != ' ')) {
		return;
	}
	mach_timebase_info(&timebase);
	{
		pthread_t watcher;

		main_stack_top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
		main_stack_bottom = main_stack_top - pthread_get_stacksize_np(pthread_self());
		if (pthread_create(&watcher, NULL, stall_watch, (void *)(uintptr_t)mach_thread_self()) == 0) {
			pthread_detach(watcher);
		}
	}
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting, true, LONG_MIN,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, CA_COMMIT_ORDER - 1,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	{
		CFRunLoopObserverContext after = { .info = (void *)1 };

		CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true,
		    CA_COMMIT_ORDER + 1, main_loop_turn, &after), kCFRunLoopCommonModes);
	}
}

/* ---- methods of SpringBoard's classes, replaced --------------------------------------------------- */

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/*
 * Replace -[CLASS SELECTOR] (or +[CLASS SELECTOR]: `kind` is that first character), whose code must now
 * be at the unslid address `expected` (Darwin userland 16.5, 20F66: what a replacement does depends on what the
 * original did). Returns the original.
 * (dyld interposing of the C functions underneath does not reach calls made from inside the
 * shared cache: there one library branches straight into the other.)
 */
static IMP
replace_method_of_kind(char kind, const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls == NULL ? NULL : kind == '+' ? class_getClassMethod(cls, sel_registerName(selector)) :
	    class_getInstanceMethod(cls, sel_registerName(selector));
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != expected + slide) {
		say("hakios: %c[%s %s]: found %#lx, expected %#lx; left alone\n", kind, class_name, selector,
		    original ? (uintptr_t)original - slide : 0, expected);
		return NULL;
	}
	method_setImplementation(method, replacement);
	return original;
}

static IMP
replace_method(const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	return replace_method_of_kind('-', class_name, selector, expected, replacement);
}

/*
 * -[SBLockdownManager _setupActivationState] asks MobileActivation and keeps the answer in the
 * int at +8: 2 = activated, 1 = not, 3 = unknown; it stays 0 when there is no answer.
 */
#define LOCKDOWN_STATE(manager)         (*(int *)((char *)(manager) + 8))
#define LOCKDOWN_ACTIVATED              2

static void
setup_activation_state(id self, __unused SEL selector)
{
	LOCKDOWN_STATE(self) = LOCKDOWN_ACTIVATED;
}

/* -[SBSetupManager _setSetupRequiredReason:]: 0 = no need for the setup assistant */
static void (*original_set_setup_required_reason)(id self, SEL selector, long reason);

static void
set_setup_required_reason(id self, SEL selector, long reason)
{
	if (reason != 0) {
		say("hakios: SpringBoard wants the setup assistant (reason %ld); not here\n", reason);
	}
	original_set_setup_required_reason(self, selector, 0);
}

/*
 * +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] chooses the file format of the picture
 * SpringBoard takes of an application that leaves the screen: 0 = PNG, 1 and 2 = ATX (an ASTC texture),
 * 3 = JPEG. A device with a wide colour display that can decode ASTC gets ATX, and Darwin compresses it on
 * the processor: fast with the vector code of Apple's cores, but here it takes one core more than ten
 * seconds per picture (under QEMU; seconds on the phone) -- and all that time the application waits for
 * the answer, until the watchdog for scene updates (10 s, there is no setting to scale it) kills it:
 * no application survived going to the background. JPEG takes a fraction of a second; it has no
 * transparency, so a picture that needs it becomes a PNG (which is what Darwin itself does on a device
 * without ASTC).
 */
#define SNAPSHOT_FORMAT_PNG             0
#define SNAPSHOT_FORMAT_JPEG            3

static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");

	if (snapshot != NULL && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		return SNAPSHOT_FORMAT_JPEG;
	}
	return SNAPSHOT_FORMAT_PNG;
}

/* ---- battery state from redwood's Qualcomm charger stack ----------------------------------------- */

/*
 * Darwin reads battery level through UIDevice.batteryLevel and UIDevice.batteryState; SpringBoard's
 * status bar reads it the same way via SBBatteryController. Where the battery has no power source
 * of its own in the kernel (battery=1, the loader's way through Qualcomm's UEFI charger protocols),
 * we hook the UIDevice accessors to return what XNU publishes as kern.hakios.battery_soc (0..100)
 * and kern.hakios.battery_charging.
 * When the sysctl is missing (battery=1 not in boot-args, or the driver never started) we hand
 * back the value the real implementation computed; the status bar then shows whatever Darwin last
 * knew, which is better than crashing.
 * With adsp=1 the battery is a power source (the service HakiosBattery, an IOPMPowerSource:
 * drivers/power/hakios.battery.cpp), which powerd reads like an iPhone's own, and from it Darwin knows
 * everything by itself; once that service is there, the hooks hand everything to Darwin's own code.
 */
static float  (*original_battery_level)(id self, SEL sel);
static long   (*original_battery_state)(id self, SEL sel);
static BOOL   (*original_battery_view_shows_percentage)(id self, SEL sel);
static void   (*original_battery_view_set_shows_percentage)(id self, SEL sel, BOOL shows);
static BOOL   (*original_battery_view_should_show_bolt)(id self, SEL sel);
static double (*original_battery_view_charge_percent)(id self, SEL sel);
static void   (*original_battery_view_set_charge_percent)(id self, SEL sel, double p);
static double (*original_ring_charging_level)(id self, SEL sel);

static int
sysctl_int(const char *name)
{
	int value = -1;
	size_t size = sizeof(value);

	sysctlbyname(name, &value, &size, NULL, 0);
	return value;
}

/* Is the battery a power source that Darwin reads by itself (see above)? Once it is, it stays one. */
static bool
battery_power_source(void)
{
	static bool found;
	io_service_t service;

	if (!found && (service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosBattery"))) != 0) {
		IOObjectRelease(service);
		found = true;
	}
	return found;
}

/* ---- legacy cellular status-bar hooks (opt in only) ---------------------------------------------- */

/*
 * CommCenter/SystemStatus now supplies the signal data on umi.  These older hooks were written for
 * the pre-CommCenter bridge and are therefore disabled unless this file exists before SpringBoard
 * starts.  The switch is deliberately in /var so a phone run can restore the old path without a
 * boot-arg or a payload rebuild.
 */
#define CELLULAR_LEGACY_SWITCH "/var/umi-statusbar-signal.legacy"

static bool
cellular_legacy_enabled(void)
{
	return access(CELLULAR_LEGACY_SWITCH, F_OK) == 0;
}

/*
 * SpringBoard's SBStatusBarStateAggregator fills the raw status bar data from CommCenter/CoreTelephony.
 * The old item updaters take the data and a getter and setter for the field
 * (Darwin userland 16.5: -[SBStatusBarStateAggregator _updateSignalStrengthItem:withData:SIMInfo:barsGetter:
 * barsSetter:], and the like for the service string): the signal one is replaced so that, when the
 * kernel's modem is registered (sysctl kern.hakios.cellular_registered, _bars: hakios.mpss.c),
 * the bars are set and the item switched on (-[... _setItem:enabled:]). The aggregator's whole
 * method list is said once in the log, for the posting to be found; and the signal item is asked
 * to update itself every few seconds while the kernel's state changes.
 */
static void (*original_update_signal_item)(id self, SEL sel, long item, void *data, id sim, void *getter, void *setter);
static id cellular_aggregator;
static int cellular_last_bars = -2, cellular_said;

/* The getters and setters are blocks (their type is "@?"): called through the block's own invoke pointer, with the block first. */
struct block_layout {
	void *isa;
	int flags, reserved;
	void (*invoke)(void *, ...);
};
#define BLOCK_CALL(block, type, ...) ((type)((struct block_layout *)(block))->invoke)((block), __VA_ARGS__)

static void
update_signal_item(id self, SEL sel, long item, void *data, id sim, void *getter, void *setter)
{
	int up = sysctl_int("kern.hakios.cellular_up"), registered = sysctl_int("kern.hakios.cellular_registered");
	int bars = sysctl_int("kern.hakios.cellular_bars"), dbm = sysctl_int("kern.hakios.cellular_dbm");
	int shown;

	cellular_aggregator = self;
	if (original_update_signal_item) {
		original_update_signal_item(self, sel, item, data, sim, getter, setter);
	}
	if (cellular_said++ < 8) {
		say("hakios: cellular: signal item %ld: data %p, SIM info %p, getter %p, setter %p; the kernel says up %d registered %d bars %d dbm %d\n",
		    item, data, (void *)sim, getter, setter, up, registered, bars, dbm);
	}
	/*
	 * Force the signal item on for the old, opt-in path. Darwin normally gets this from CommCenter;
	 * this fallback uses the kernel's modem state, so once it is up the item is shown here with the kernel's bars; when the
	 * kernel has a reading but zero bars (a brief "no signal" sample, or a reading just after
	 * registration) at least one bar is forced so the icon is visibly present rather than empty.
	 * modem_bars_off=1 (as a sysctl) is not honoured; set kern.hakios.cellular_up=0 to hide it.
	 */
	/*
	 * The kernel's NAS state alternates between two reports -- "registered, signal not read" and
	 * "not registered, -86 dBm" -- so at the moments it says registered the dBm reads 0. The last
	 * real reading (within a minute) stands in for it.
	 */
	{
		static int last_dbm;
		static time_t last_dbm_at;
		time_t now = time(NULL);

		if (dbm < 0) {
			last_dbm = dbm;
			last_dbm_at = now;
		} else if (last_dbm < 0 && now - last_dbm_at < 60) {
			dbm = last_dbm;
		}
	}
	shown = bars;
	if (shown <= 0 && dbm < 0) {
		shown = dbm >= -95 ? 4 : dbm >= -105 ? 3 : dbm >= -115 ? 2 : 1;   /* derive from dBm when bars is 0 */
	}
	if (shown <= 0 && (up > 0 || registered > 0)) {
		shown = 1;                      /* up but no reading yet: still show the icon */
	}
	if (shown > 5) {
		shown = 5;
	}
	if ((up > 0 || registered > 0) && shown > 0 && setter != NULL && data != NULL) {
		/* The iOS 16.5 bars setter block is (^)(int); data is captured by the block. */
		BLOCK_CALL(setter, void (*)(void *, int), shown);
		((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
		if (shown != cellular_last_bars) {
			say("hakios: cellular: %d bars shown (item %ld; kernel bars %d, %d dBm, registered %d)\n", shown, item, bars, dbm, registered);
		}
	}
	cellular_last_bars = shown;
}

static void (*original_update_service_item)(id self, SEL sel, long item, void *data, id sim, id other, void *g1, void *s1, void *g2, void *s2,
    void *g3, void *s3, void *g4, void *s4);

/*
 * The service item ("No SIM", the network's name): once the kernel's modem is registered, the
 * network's name (sysctl kern.hakios.cellular_operator) goes in through the item's string setter
 * and the item is switched on.
 */
static void
update_service_item(id self, SEL sel, long item, void *data, id sim, id other, void *g1, void *s1, void *g2, void *s2, void *g3, void *s3,
    void *g4, void *s4)
{
	static int said;
	static char shown[64];
	char name[64] = "";
	size_t size = sizeof(name) - 1;
	int registered = sysctl_int("kern.hakios.cellular_registered");

	if (original_update_service_item) {
		original_update_service_item(self, sel, item, data, sim, other, g1, s1, g2, s2, g3, s3, g4, s4);
	}
	sysctlbyname("kern.hakios.cellular_operator", name, &size, NULL, 0);
	if (said++ < 4) {
		say("hakios: cellular: service item %ld: data %p, SIM info %p, other %p; string getter %p setter %p, content type getter %p "
		    "setter %p; the kernel's network \"%s\"\n", item, data, (void *)sim, (void *)other, g1, s1, g3, s3, name);
	}
	if (registered > 0 && name[0] != 0 && s1 != NULL && data != NULL) {
		CFStringRef string = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);

		if (string != NULL) {
			BLOCK_CALL(s1, void (*)(void *, void *, CFStringRef), data, string);
			((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
			if (strcmp(shown, name) != 0) {
				snprintf(shown, sizeof(shown), "%s", name);
				say("hakios: cellular: service \"%s\" shown (item %ld)\n", name, item);
			}
			CFRelease(string);
		}
	}
}

/*
 * The bars are drawn by UIKit's signal view, whose "signal mode" says how: inactive (all bars grey),
 * searching, or active (the bars that count dark). Darwin's cellular stack (CommCenter) puts it in
 * active mode once there is service; without it the bars stay grey whatever their
 * number. So for the cellular signal view (not the Wi-Fi one), while the kernel says the modem is
 * registered, the mode is made active. The modes seen are said in the log once each.
 */
static void (*original_set_signal_mode)(id self, SEL sel, long mode);
static long cellular_active_mode = 2;  /* _UIStatusBarSignalView: 0 inactive, 1 searching, 2 active */
static void umi_signal_log_view_state(id signal_view, long requested, long applied);

static void
set_signal_mode(id self, SEL sel, long mode)
{
	static unsigned said_modes;
	const char *name = object_getClassName(self);
	bool cellular = name != NULL && strstr(name, "Cellular") != NULL;
	long wanted = mode;

	if (cellular && mode < 8 && !(said_modes & (1u << mode))) {
		said_modes |= 1u << mode;
		say("hakios: cellular: %s asked for signal mode %ld\n", name, mode);
	}
	if (cellular && sysctl_int("kern.hakios.cellular_registered") > 0 && mode != cellular_active_mode) {
		wanted = cellular_active_mode;
	}
	umi_signal_log_view_state(self, mode, wanted);
	original_set_signal_mode(self, sel, wanted);
}

static void
signal_view_hook(void)
{
	static const char *const classes[] = { "_UIStatusBarCellularSignalView", "_UIStatusBarSignalView" };

	for (unsigned c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
		Class cls = objc_getClass(classes[c]);
		Method method = cls ? class_getInstanceMethod(cls, sel_registerName("setSignalMode:")) : NULL;

		if (method == NULL) {
			say("hakios: cellular: no -[%s setSignalMode:]\n", classes[c]);
			continue;
		}
		original_set_signal_mode = (void (*)(id, SEL, long))method_getImplementation(method);
		method_setImplementation(method, (IMP)set_signal_mode);
		say("hakios: cellular: -[%s setSignalMode:] replaced (types \"%s\"): active while the kernel says registered\n",
		    classes[c], method_getTypeEncoding(method));
		return;         /* the base class's method serves the subclasses */
	}
}

static void
cellular_hook(void)
{
	if (!cellular_legacy_enabled()) {
		say("hakios: cellular: legacy hooks disabled; native CommCenter/SystemStatus owns signal state "
		    "(create %s before SpringBoard to restore)\n", CELLULAR_LEGACY_SWITCH);
		return;
	}
	say("hakios: cellular: legacy hooks enabled by %s\n", CELLULAR_LEGACY_SWITCH);
	signal_view_hook();
	{
		const char *service_selector = "_updateServiceItem:withData:SIMInfo:otherSIMInfo:serviceStringGetter:serviceStringSetter:"
		    "serviceCrossfadeStringGetter:serviceCrossfadeStringSetter:serviceContentTypeGetter:serviceContentTypeSetter:"
		    "serviceBadgeStringGetter:serviceBadgeStringSetter:";
		Class cls = objc_getClass("SBStatusBarStateAggregator");
		Method method = cls ? class_getInstanceMethod(cls, sel_registerName(service_selector)) : NULL;

		if (method) {
			say("hakios: cellular: -[SBStatusBarStateAggregator %s] at %#lx, types \"%s\": looked at\n", service_selector,
			    (uintptr_t)method_getImplementation(method) - slide, method_getTypeEncoding(method));
			original_update_service_item = (void (*)(id, SEL, long, void *, id, id, void *, void *, void *, void *, void *, void *, void *,
			    void *))method_getImplementation(method);
			method_setImplementation(method, (IMP)update_service_item);
		}
	}
	const char *selector = "_updateSignalStrengthItem:withData:SIMInfo:barsGetter:barsSetter:";
	Class cls = objc_getClass("SBStatusBarStateAggregator");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (method == NULL) {
		say("hakios: cellular: no -[SBStatusBarStateAggregator %s]\n", selector);
		return;
	}
	say("hakios: cellular: -[SBStatusBarStateAggregator %s] at %#lx, types \"%s\": replaced\n", selector,
	    (uintptr_t)method_getImplementation(method) - slide, method_getTypeEncoding(method));
	original_update_signal_item = (void (*)(id, SEL, long, void *, id, void *, void *))method_getImplementation(method);
	method_setImplementation(method, (IMP)update_signal_item);
	{
		/* the whole class, once: which updaters and posters there are */
		unsigned count = 0;
		Method *list = class_copyMethodList(cls, &count);
		char line[1024] = "";
		size_t at = 0;

		for (unsigned i = 0; list && i < count; i++) {
			const char *name = sel_getName(method_getName(list[i]));
			size_t n = strlen(name);

			if (at + n + 2 > sizeof(line)) {
				say("hakios: cellular: aggregator methods: %s\n", line);
				line[0] = 0;
				at = 0;
			}
			memcpy(line + at, name, n);
			at += n;
			line[at++] = ' ';
			line[at] = 0;
		}
		if (at) {
			say("hakios: cellular: aggregator methods: %s\n", line);
		}
		free(list);
	}
	/* every 3 s: when the kernel's registration or bars change, the item is asked to update itself */
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());

	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), 3 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
	dispatch_source_set_event_handler(timer, ^{
		static int last_registered = -2, last_bars = -2;
		int registered = sysctl_int("kern.hakios.cellular_registered"), bars = sysctl_int("kern.hakios.cellular_bars");

		if ((registered != last_registered || bars != last_bars) && cellular_aggregator != nil) {
			SEL update = sel_registerName("_updateSignalStrengthItem");

			last_registered = registered;
			last_bars = bars;
			if (class_getInstanceMethod(object_getClass(cellular_aggregator), update)) {
				((void (*)(id, SEL))objc_msgSend)(cellular_aggregator, update);
				say("hakios: cellular: the signal item asked to update (registered %d, bars %d)\n", registered, bars);
			}
		}
	});
	dispatch_resume(timer);
}

/* UIDevice.batteryLevel: 0.0..1.0; -1.0 if monitoring not enabled */
static float
battery_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_level ? original_battery_level(self, sel) : -1.0f;
	}
	return (float)soc / 100.0f;
}

/* UIDevice.batteryState: 0=unknown 1=unplugged 2=charging 3=full */
static long
battery_state(id self, SEL sel)
{
	int charging = sysctl_int("kern.hakios.battery_charging");
	int present = sysctl_int("kern.hakios.battery_present");
	int plugged = sysctl_int("kern.hakios.battery_charger_present");
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (charging == -1 || present == -1 || plugged == -1 || soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_state ? original_battery_state(self, sel) : 0;
	}
	if (!present) return 0;
	if (plugged && soc >= 100) return 3;
	return charging ? 2 : 1;
}

/*
 * UIKitCore's status-bar view has its own presentation policy, separate from UIDevice. With the power
 * source it is Darwin's own again: the switch Battery Percentage in Settings > Battery, which the image
 * turns on (SBShowBatteryPercentage in userland/var/mobile/Library/Preferences/com.apple.springboard.plist).
 */
static BOOL
battery_view_shows_percentage(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return YES;
	return original_battery_view_shows_percentage ? original_battery_view_shows_percentage(self, sel) : NO;
}

static void
battery_view_set_shows_percentage(id self, SEL sel, BOOL shows)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	original_battery_view_set_shows_percentage(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? YES : shows);
}

static BOOL
battery_view_should_show_bolt(id self, SEL sel)
{
	int charging = sysctl_int("kern.hakios.battery_charging");
	int present = sysctl_int("kern.hakios.battery_present");
	int plugged = sysctl_int("kern.hakios.battery_charger_present");

	if (charging != -1 && present != -1 && plugged != -1 && !battery_power_source()) return charging && present && plugged;
	return original_battery_view_should_show_bolt ? original_battery_view_should_show_bolt(self, sel) : NO;
}

/*
 * The actual number the status bar draws. _UIBatteryView keeps the fill fraction (0.0..1.0) in
 * chargePercent; it is set through setChargePercent: and read back for layout. Darwin feeds it from
 * BatteryCenter / IOPowerSources, which is empty on this phone, so without us it sits near zero
 * (the "1%" the user saw). We override both sides: setChargePercent: so the stored value is ours
 * no matter who writes it, and the getter so any layout path that reads it agrees.
 */
static double
battery_view_charge_percent(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_battery_view_charge_percent ? original_battery_view_charge_percent(self, sel) : 0.0;
}

static void
battery_view_set_charge_percent(id self, SEL sel, double p)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	original_battery_view_set_charge_percent(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? (double)soc / 100.0 : p);
}

/* The big charging ring on the lock screen (CoverSheet) reads its fill level the same way. */
static double
ring_charging_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_ring_charging_level ? original_ring_charging_level(self, sel) : 0.0;
}

/* Keep SpringBoard's normal battery observers awake while the XNU cache changes. */
static void
battery_post_notifications(void)
{
	Class uidev = objc_getClass("UIDevice");
	Class notification_center = objc_getClass("NSNotificationCenter");
	id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, sel_registerName("currentDevice")) : nil;
	id center = notification_center ? ((id (*)(Class, SEL))objc_msgSend)(notification_center,
	    sel_registerName("defaultCenter")) : nil;

	if (center != nil && !battery_power_source()) {
		SEL post = sel_registerName("postNotificationName:object:");
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryLevelDidChangeNotification"), device);
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryStateDidChangeNotification"), device);
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		battery_post_notifications();
	});
}

/* ---- the lock screen's clock without a GPU ------------------------------------------------------- */

#define HAKIOS_GL "/redwood/lib/libhakios.gl.dylib"

/* Does the window server draw with OpenGL ES? The same question, asked the same way, as opengl_es() in inject.c. */
static bool
gpu_draws(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;

	/* ... or through Metal (boot-arg metal=1 with the GPU kept: HakiosMetal.bundle, an image built with METAL_PLUGIN) */
	if (access("/redwood/lib/HakiosMetal.bundle/HakiosMetal", R_OK) == 0 &&
	    sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (argument = arguments; (argument = strstr(argument, "metal=")) != NULL; argument += 6) {
			if ((argument == arguments || argument[-1] == ' ') && argument[6] != '0') {
				gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
				if (gpu != 0) {
					IOObjectRelease(gpu);
					return true;
				}
			}
		}
	}
	size = sizeof(arguments) - 1;
	if (access(HAKIOS_GL, R_OK) != 0) {
		return false;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				return argument[3] != '0';
			}
		}
	}
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
	if (gpu != 0) {
		IOObjectRelease(gpu);
	}
	return gpu != 0;
}

/*
 * The clock of the lock screen is a "vibrant" view (BSUIVibrancyEffectView in BaseBoardUI): its
 * digits are not drawn, they are a mask -- a copy of them through a colour matrix that keeps red
 * as alpha -- through which the filter vibrantColorMatrix lets the wallpaper behind come out
 * lighter; and a gradient over the whole screen, red at the top, tones the wallpaper down the
 * same way. On the GPU that is what is seen (eagl.c draws the two filters). CoreAnimation's
 * software renderer shows neither: the digits come out red, and the top of the wallpaper as it
 * was. Why was not found out; that renderer is in QuartzCore, not here.
 * Such views have a switch, the user default BSUIVibrancyEffectViewDisabled: with it they show
 * their content as it is drawn, the clock in plain white. It is set for this run of SpringBoard
 * alone (nothing is written), and only where the software renderer draws.
 */
static void
plain_vibrancy(void)
{
	if (gpu_draws()) {
		return;
	}
	CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
	say("hakios: SpringBoard: no GPU draws, vibrant views are plain (the lock screen's clock is white)\n");
}

/*
 * Boot-arg ui_lite=1: the system's own "Reduce Transparency" and "Reduce Motion" (Settings, Accessibility), for this
 * boot and, through var_store, the next. CoreAnimation then draws the dock, folders and widgets' platters in plain
 * colours instead of blurring what is behind them every frame, and opens and closes applications with a fade instead
 * of a zoom of the whole home screen -- the two things the Adreno spends a frame's GPU time on through Metal (2
 * November: 6 to 10 ms a frame in motion, ~30 ms for an application's close). ui_lite=0 turns both off again (what
 * var_store kept from a ui_lite=1 boot); without the boot-arg the settings are left alone.
 */
static void
ui_lite(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	CFBooleanRef on;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 ||
	    (argument = strstr(arguments, "ui_lite=")) == NULL || (argument != arguments && argument[-1] != ' ')) {
		return;
	}
	on = argument[8] == '0' ? kCFBooleanFalse : kCFBooleanTrue;
	CFPreferencesSetValue(CFSTR("EnhancedBackgroundContrastEnabled"), on, CFSTR("com.apple.Accessibility"),
	    kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	CFPreferencesSetValue(CFSTR("ReduceMotionEnabled"), on, CFSTR("com.apple.Accessibility"),
	    kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	CFPreferencesSynchronize(CFSTR("com.apple.Accessibility"), kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	notify_post("com.apple.accessibility.cache.enhance.background.contrast");
	notify_post("com.apple.accessibility.cache.reduce.motion");
	say("hakios: SpringBoard: ui_lite=%c: Reduce Transparency and Reduce Motion %s\n", argument[8], on == kCFBooleanTrue ? "on" : "off");
}

/* ---- the side button's long press ---------------------------------------------------------------- */

/*
 * -[SBLockHardwareButton longPress:] is what the side button's long press recognizer calls: Siri
 * (here, its "turn on Siri" sheet, nobody having turned it on). The recognizer is a timer of
 * SpringBoard's own: started when the main thread sees the press, stopped when it sees the release
 * -- and a release it has not got round to is no release. A press of the side button with the
 * display off wakes the display from inside the handling of that very press, which on an iPhone
 * takes no time. Here it takes as long as the software renderer needs for the first picture of the
 * lock screen: a tenth of a second on the phone, a second under QEMU. That is longer than a long
 * press, so there every wake by the button (and by a tap, which is a press of that button made in
 * inject.c) ended with Siri's sheet over the lock screen, however short the press had been.
 * Where the button's events come from -- inject.c, in backboardd -- it is known whether the button
 * is still held; a long press of a button that is not is dropped here.
 */
#define GESTURE_BEGAN                   1       /* UIGestureRecognizerState */
#define GESTURE_ENDED                   3       /* and cancelled, failed after it */

static void (*original_lock_long_press)(id self, SEL selector, id recognizer);

static bool
lock_button_held(void)
{
	static int token = NOTIFY_TOKEN_INVALID;
	uint64_t state = 0;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return true;
	}
	/* 1 = released; held, or not known: as SpringBoard has it */
	return notify_get_state(token, &state) != NOTIFY_STATUS_OK || state != 1;
}

static void
lock_long_press(id self, SEL selector, id recognizer)
{
	static bool dropping;
	SEL state_selector = sel_registerName("state");
	long state = recognizer != NULL && class_getInstanceMethod(object_getClass(recognizer), state_selector) != NULL ?
	    ((long (*)(id, SEL))objc_msgSend)(recognizer, state_selector) : 0;

	if (state == GESTURE_BEGAN) {
		dropping = !lock_button_held();
		if (dropping) {
			say("hakios: SpringBoard: a long press of the side button, which was released long ago: dropped\n");
		}
	}
	if (dropping) {
		if (state >= GESTURE_ENDED) {
			dropping = false;
		}
		return;
	}
	original_lock_long_press(self, selector, recognizer);
}

/* ---- remote control ------------------------------------------------------------------------------ */

#define UNLOCK_SOURCE_COVER_SHEET       24      /* what the lock screen itself passes */

static void
unlock(void)
{
	Class cls = objc_getClass("SBLockScreenManager");
	SEL shared = sel_registerName("sharedInstance"), locked = sel_registerName("isUILocked");
	SEL unlock_ui = sel_registerName("unlockUIFromSource:withOptions:");
	id manager;

	if (cls == NULL || class_getClassMethod(cls, shared) == NULL || class_getInstanceMethod(cls, locked) == NULL ||
	    class_getInstanceMethod(cls, unlock_ui) == NULL) {
		say("hakios: unlock: SBLockScreenManager is not what it was\n");
		return;
	}
	manager = ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
	if (manager == NULL) {
		say("hakios: unlock: no lock screen manager yet\n");
	} else if (!((BOOL (*)(id, SEL))objc_msgSend)(manager, locked)) {
		say("hakios: unlock: not locked\n");
	} else {
		BOOL done = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, NULL);

		say("hakios: unlock: asked, answer %d\n", done);
	}
}

/*
 * The poster switcher: on Darwin userland 16 a long press on the lock screen's wallpaper shrinks it into a
 * row of posters to choose from or customize. The row is PosterBoard's, an application scene of
 * that process which the lock screen hosts (CSPosterSwitcherViewController, a CALayerHost of the
 * context of PosterBoard's window over an opaque black layer). PosterBoard does lay it out -- its
 * window has the posters, "Customize" and "+", the hosted context is that window's, the scene is
 * in the foreground -- but the window server draws nothing of that context, with either
 * renderer, so what is left is the black layer under it, until a swipe up takes the switcher
 * away. Why is not known: the display is not in secure mode, SpringBoard counts the user as
 * authenticated, the same nesting of hosted contexts works for the wallpaper itself.
 * Until it is, the switcher is not offered, the way SpringBoard itself withholds it where it has
 * none (an iPad, Guided Access): its policy says the capability is not there, and the long press
 * does nothing.
 */
static BOOL
poster_switcher_allowed(__unused id self, __unused SEL selector, id *explanation)
{
	if (explanation != NULL) {
		*explanation = (id)CFSTR("the poster switcher is not shown here (see inject_sb.c)");
	}
	return NO;
}

/*
 * What SpringBoard has put on the screen, as CoreAnimation has it: every layer of every window, a
 * line each -- class and name, place, opacity, what it shows (an image, a colour, another
 * process's layers), and what is done to it: its filters by type with their inputs, in order,
 * the compositing filter, the mask. That is where a picture that comes out wrong on one renderer
 * and right on another is described: the same description goes to both.
 */
#define SEND(type, object, name)        ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))
#define SEND1(type, object, name, argument_type, argument) \
	((type (*)(id, SEL, argument_type))objc_msgSend)((id)(object), sel_registerName(name), argument)

static const char *layers_of = "";               /* in another program than SpringBoard: " of" and its name */

static const char *
text_of(id object)
{
	id text = object ? SEND(id, object, "description") : NULL;

	return text ? SEND(const char *, text, "UTF8String") : "-";
}

/*
 * Boot-arg wallpaper_cache=1: the window that shows the wallpaper rasterized by the window server (CALayer's
 * shouldRasterize), drawn from one texture instead of composed again every frame. iOS 16's Collections poster is
 * eight to twelve full-screen layers blended together (textures of 1271 x 1823, 1326 x 1326, 1326 x 1160, ...), 1.5 to
 * 2.4 ms each on the Adreno through Metal: 15 to 20 ms of every frame in which something moves over it (2 November,
 * metal_fps=mainpass), for a picture that does not change -- nothing here moves it (no parallax without a gyroscope).
 * Looked for every 15 s from 20 s on, for windows whose class names a wallpaper or a poster.
 */
static void
wallpaper_cache_pass(void)
{
	static unsigned listed;
	id app = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id windows = app != NULL ? SEND(id, app, "windows") : NULL;
	unsigned long count = windows != NULL ? SEND(unsigned long, windows, "count") : 0;
	id screen = SEND(id, objc_getClass("UIScreen"), "mainScreen");
	double scale = screen != NULL ? ((double (*)(id, SEL))objc_msgSend)(screen, sel_registerName("scale")) : 3.0;

	for (unsigned long i = 0; i < count; i++) {
		id window = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);
		const char *name = object_getClassName(window);
		id layer = SEND(id, window, "layer");

		if (listed < 12) {
			id sub = SEND(id, layer, "sublayers");
			say("hakios: SpringBoard: window %lu of %lu: %s, layer %s with %lu sublayers%s\n", i, count, name,
			    layer != NULL ? object_getClassName(layer) : "-", sub != NULL ? SEND(unsigned long, sub, "count") : 0UL,
			    layer != NULL && SEND(BOOL, layer, "shouldRasterize") ? ", rasterized" : "");
		}
		if ((strcasestr(name, "wallpaper") != NULL || strcasestr(name, "poster") != NULL) && layer != NULL &&
		    !SEND(BOOL, layer, "shouldRasterize")) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(layer, sel_registerName("setShouldRasterize:"), YES);
			((void (*)(id, SEL, double))objc_msgSend)(layer, sel_registerName("setRasterizationScale:"), scale);
			say("hakios: SpringBoard: %s rasterized at %g (wallpaper_cache=1)\n", name, scale);
		}
	}
	listed++;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		wallpaper_cache_pass();
	});
}

/*
 * Boot-arg wallpaper_freeze=1: the wallpaper as one still picture. Rasterizing its window (wallpaper_cache=1) did not
 * keep it from being composed again: the poster is another process's layers (PosterBoard's, through a layer host), and
 * its gradients (a 256 x 1 half-float ramp and a 32 x 32 dither each) were still drawn 3509 times in a run at 0.6 ms
 * each, the rasterized copy 1460 times at 1.2 to 3.2 ms (3 November, metal_fps=draws). The home button's animation came
 * at 28 frames a second with it and 78 with the wallpaper hidden (wallpaper_freeze=2).
 *
 * -drawViewHierarchyInRect: answers one colour for it here, secure window or not. So the picture is what is really on
 * the panel: at the home screen, with the window of the icons hidden for a moment (they blink once per boot), the pixels of the IOSurface the window server last showed (kern.hakios.fb_surface) become one opaque view in
 * the wallpaper's window, and what was there is hidden. What that gives up: the wallpaper does not change until the
 * next boot, and the lock screen shows the same picture.
 */
typedef struct { double x, y, width, height; } hk_rect;

static bool wallpaper_hide_only;        /* wallpaper_freeze=2: no picture, the wallpaper hidden (what it costs, measured) */
static const void *wallpaper_frozen_key = &wallpaper_frozen_key;

static id
wallpaper_window(void)
{
	id app = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id windows = app != NULL ? SEND(id, app, "windows") : NULL;
	unsigned long count = windows != NULL ? SEND(unsigned long, windows, "count") : 0;

	for (unsigned long i = 0; i < count; i++) {
		id window = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);
		const char *name = object_getClassName(window);

		if (strcasestr(name, "wallpaper") != NULL || strcasestr(name, "poster") != NULL) {
			return window;
		}
	}
	return NULL;
}

/*
 * The picture is kept in /private/var (var_store keeps that across boots), for a screen of another class (fbscale=2
 * fbsubtype=1792: 1.85 times fewer pixels for the GPU), on which the poster draws nothing at all: black.
 */
#define WALLPAPER_KEPT "/var/mobile/Library/hakios-wallpaper.raw"
struct wallpaper_kept { uint32_t magic, width, height, row; };
#define WALLPAPER_MAGIC 0x57504b31

static void
wallpaper_keep(const uint8_t *pixels, size_t width, size_t height, size_t row)
{
	struct wallpaper_kept head = { WALLPAPER_MAGIC, (uint32_t)width, (uint32_t)height, (uint32_t)row };
	int fd = open(WALLPAPER_KEPT ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0) {
		say("hakios: SpringBoard: wallpaper_freeze: %s cannot be written\n", WALLPAPER_KEPT);
		return;
	}
	bool whole = write(fd, &head, sizeof(head)) == (ssize_t)sizeof(head) &&
	    write(fd, pixels, row * height) == (ssize_t)(row * height);
	close(fd);
	if (whole) {
		rename(WALLPAPER_KEPT ".new", WALLPAPER_KEPT);
		say("hakios: SpringBoard: wallpaper_freeze: the picture kept, %zux%zu\n", width, height);
	}
}

/* The picture kept, redrawn at this screen's pixels (a texture of the screen's own size to sample); NULL: none. */
static id
wallpaper_kept_picture(double width_points, double height_points, double scale)
{
	struct wallpaper_kept head;
	int fd = open(WALLPAPER_KEPT, O_RDONLY);
	id image = NULL;

	if (fd < 0) {
		return NULL;
	}
	if (read(fd, &head, sizeof(head)) == (ssize_t)sizeof(head) && head.magic == WALLPAPER_MAGIC && head.width != 0 && head.height != 0 &&
	    head.row >= head.width * 4 && head.height < 16384) {
		size_t bytes = (size_t)head.row * head.height;
		uint8_t *pixels = malloc(bytes);

		if (pixels != NULL && read(fd, pixels, bytes) == (ssize_t)bytes) {
			size_t w = (size_t)(width_points * scale + 0.5), h = (size_t)(height_points * scale + 0.5);
			CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
			CGBitmapInfo info = kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst;
			CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, pixels, bytes, kCFAllocatorNull);
			CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
			CGImageRef kept = CGImageCreate(head.width, head.height, 8, 32, head.row, space, info, provider, NULL, false, kCGRenderingIntentDefault);
			CGContextRef context = CGBitmapContextCreate(NULL, w, h, 8, 0, space, info);

			if (kept != NULL && context != NULL) {
				CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
				CGContextDrawImage(context, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), kept);
				CGImageRef made = CGBitmapContextCreateImage(context);

				if (made != NULL) {
					image = ((id (*)(id, SEL, CGImageRef, double, long))objc_msgSend)((id)objc_getClass("UIImage"),
					    sel_registerName("imageWithCGImage:scale:orientation:"), made, scale, 0L);
					CGImageRelease(made);
				}
			}
			say("hakios: SpringBoard: wallpaper_freeze: the picture kept (%ux%u) drawn at %zux%zu: %s\n", head.width, head.height, w, h,
			    image != NULL ? "yes" : "NO");
			if (context != NULL) CGContextRelease(context);
			if (kept != NULL) CGImageRelease(kept);
			CGDataProviderRelease(provider);
			CFRelease(data);
			CGColorSpaceRelease(space);
		}
		free(pixels);
	}
	close(fd);
	return image;
}

/* A picture of the surface on the panel, opaque, in points of the screen's scale; NULL: none, or all one colour. */
static id
wallpaper_picture(double scale)
{
	uint32_t sid = 0;
	size_t size = sizeof(sid);
	IOSurfaceRef surface;
	id image = NULL;

	if (sysctlbyname("kern.hakios.fb_surface", &sid, &size, NULL, 0) != 0 || sid == 0 ||
	    (surface = IOSurfaceLookup(sid)) == NULL) {
		say("hakios: SpringBoard: wallpaper_freeze=1: no surface on the panel to read (%u)\n", sid);
		return NULL;
	}
	size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface), row = IOSurfaceGetBytesPerRow(surface);

	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	CFDataRef pixels = CFDataCreate(NULL, IOSurfaceGetBaseAddress(surface), (CFIndex)(row * height));
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	unsigned distinct = 0;
	if (pixels != NULL) {
		const uint8_t *at = CFDataGetBytePtr(pixels);
		uint32_t first = *(const uint32_t *)at;

		for (unsigned k = 0; k < 97; k++) {
			distinct += *(const uint32_t *)(at + (height * k / 97) * row + (width * k / 97) * 4) != first;
		}
	}
	say("hakios: SpringBoard: wallpaper_freeze=1: surface %u, %zux%zu, %zu bytes a row, format %#x: %u of 97 samples unlike the first (%#x)\n",
	    sid, width, height, row, IOSurfaceGetPixelFormat(surface), distinct, pixels != NULL ? *(const uint32_t *)CFDataGetBytePtr(pixels) : 0);
	if (pixels != NULL && distinct >= 10) {
		wallpaper_keep(CFDataGetBytePtr(pixels), width, height, row);
		CGDataProviderRef provider = CGDataProviderCreateWithCFData(pixels);
		CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
		CGImageRef cg = CGImageCreate(width, height, 8, 32, row, space, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
		    provider, NULL, false, kCGRenderingIntentDefault);

		if (cg != NULL) {
			image = ((id (*)(id, SEL, CGImageRef, double, long))objc_msgSend)((id)objc_getClass("UIImage"),
			    sel_registerName("imageWithCGImage:scale:orientation:"), cg, scale, 0L);
			CGImageRelease(cg);
		}
		CGColorSpaceRelease(space);
		CGDataProviderRelease(provider);
	}
	if (pixels != NULL) {
		CFRelease(pixels);
	}
	CFRelease(surface);
	return image;
}

/* The picture into the wallpaper's window, in place of what was shown there (none: only hidden). */
static void
wallpaper_put(id window, id image)
{
	id layer = SEND(id, window, "layer");
	id sublayers = SEND(id, SEND(id, layer, "sublayers"), "copy");
	unsigned long count = sublayers != NULL ? SEND(unsigned long, sublayers, "count") : 0;
	hk_rect bounds = ((hk_rect (*)(id, SEL))objc_msgSend)(window, sel_registerName("bounds"));

	if (image != NULL) {
		id view = ((id (*)(id, SEL, id))objc_msgSend)(SEND(id, objc_getClass("UIImageView"), "alloc"),
		    sel_registerName("initWithImage:"), image);

		((void (*)(id, SEL, hk_rect))objc_msgSend)(view, sel_registerName("setFrame:"), bounds);
		((void (*)(id, SEL, BOOL))objc_msgSend)(view, sel_registerName("setOpaque:"), YES);
		((void (*)(id, SEL, id))objc_msgSend)(window, sel_registerName("addSubview:"), view);
		objc_setAssociatedObject(window, wallpaper_frozen_key, view, OBJC_ASSOCIATION_RETAIN);
	} else {
		objc_setAssociatedObject(window, wallpaper_frozen_key, window, OBJC_ASSOCIATION_ASSIGN);
	}
	for (unsigned long k = 0; k < count; k++) {
		id sub = ((id (*)(id, SEL, unsigned long))objc_msgSend)(sublayers, sel_registerName("objectAtIndex:"), k);

		((void (*)(id, SEL, BOOL))objc_msgSend)(sub, sel_registerName("setHidden:"), YES);
	}
	say("hakios: SpringBoard: wallpaper_freeze: %s %s (%lu layers hidden)\n", object_getClassName(window),
	    image != NULL ? "is one picture now" : "hidden", count);
}

/* Whether the home screen is what is showing: unlocked, no application in front. */
static bool
wallpaper_home_showing(void)
{
	Class manager = objc_getClass("SBLockScreenManager");
	id shared = manager != NULL ? SEND(id, manager, "sharedInstance") : NULL;
	id app = SEND(id, objc_getClass("UIApplication"), "sharedApplication");

	if (shared == NULL || SEND(BOOL, shared, "isUILocked")) {
		return false;
	}
	/* (a selector nobody answers would abort SpringBoard: unlocked is then all that is known) */
	if (app == NULL || !class_respondsToSelector(object_getClass(app), sel_registerName("_accessibilityFrontMostApplication"))) {
		return app != NULL;
	}
	return SEND(id, app, "_accessibilityFrontMostApplication") == NULL;
}

static void
wallpaper_freeze_pass(unsigned attempt)
{
	id window = wallpaper_window();
	id screen = SEND(id, objc_getClass("UIScreen"), "mainScreen");
	double scale = screen != NULL ? ((double (*)(id, SEL))objc_msgSend)(screen, sel_registerName("scale")) : 3.0;

	if (window != NULL && objc_getAssociatedObject(window, wallpaper_frozen_key) != NULL) {
		return;
	}
	if (window != NULL && !wallpaper_hide_only) {
		hk_rect bounds = ((hk_rect (*)(id, SEL))objc_msgSend)(window, sel_registerName("bounds"));

		if (bounds.width != 375 || bounds.height != 812 || scale != 3.0) {
			id image = wallpaper_kept_picture(bounds.width, bounds.height, scale);

			if (image != NULL) {
				wallpaper_put(window, image);
				return;
			}
			say("hakios: SpringBoard: wallpaper_freeze: no picture kept for this screen (%gx%g at %g): a boot at the iPhone X's screen first\n",
			    bounds.width, bounds.height, scale);
			return;
		}
	}
	if (window == NULL || !wallpaper_home_showing()) {
		if (attempt < 720) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
				wallpaper_freeze_pass(attempt + 1);
			});
		}
		return;
	}
	if (wallpaper_hide_only) {
		wallpaper_put(window, NULL);
		return;
	}
	/* a screen of another class than the iPhone X's: no poster to read off the panel, the picture kept instead */
	{
		hk_rect bounds = ((hk_rect (*)(id, SEL))objc_msgSend)(window, sel_registerName("bounds"));

		if (bounds.width != 375 || bounds.height != 812 || scale != 3.0) {
			id image = wallpaper_kept_picture(bounds.width, bounds.height, scale);

			if (image != NULL) {
				wallpaper_put(window, image);
			} else {
				say("hakios: SpringBoard: wallpaper_freeze: no picture kept for this screen: a boot at the iPhone X's first\n");
			}
			return;
		}
	}
	/* the icons out of the way until the window server has drawn the wallpaper without them */
	id app = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id windows = SEND(id, app, "windows");
	unsigned long count = SEND(unsigned long, windows, "count");
	CFMutableArrayRef hidden = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	for (unsigned long i = 0; i < count; i++) {
		id other = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);

		/*
		 * Only the window of the icons and the dock: with SpringBoard's other windows hidden as well the screen was all
		 * black (0xff000000 in all 97 samples, 4 November), the wallpaper gone with them.
		 */
		if (strcmp(object_getClassName(other), "SBHomeScreenWindow") == 0 && !SEND(BOOL, other, "isHidden")) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(other, sel_registerName("setHidden:"), YES);
			CFArrayAppendValue(hidden, other);
		}
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
		id image = wallpaper_picture(scale);

		for (CFIndex i = 0; i < CFArrayGetCount(hidden); i++) {
			((void (*)(id, SEL, BOOL))objc_msgSend)((id)CFArrayGetValueAtIndex(hidden, i), sel_registerName("setHidden:"), NO);
		}
		CFRelease(hidden);
		if (image != NULL) {
			wallpaper_put(window, image);
		} else if (attempt < 720) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
				wallpaper_freeze_pass(attempt + 1);
			});
		}
	});
}

static void
wallpaper_freeze_say(unsigned times)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		id window = wallpaper_window();

		say("hakios: SpringBoard: wallpaper_freeze: the wallpaper's window %s is %s\n", window != NULL ? object_getClassName(window) : "(none)",
		    window != NULL && objc_getAssociatedObject(window, wallpaper_frozen_key) != NULL ? "one picture" : "not frozen");
		if (times > 1) {
			wallpaper_freeze_say(times - 1);
		}
	});
}

static void
wallpaper_freeze(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 ||
	    (argument = strstr(arguments, "wallpaper_freeze=")) == NULL || (argument != arguments && argument[-1] != ' ') ||
	    (argument[17] != '1' && argument[17] != '2')) {
		return;
	}
	wallpaper_hide_only = argument[17] == '2';
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		wallpaper_freeze_pass(0);
	});
	/* (said again and again: the log of a long run keeps its beginning and its end) */
	wallpaper_freeze_say(15);
}

/*
 * The iPhone X's 375 x 812 points at 2 pixels a point (fbscale=2: the layout as it is at 3, with fewer pixels to draw):
 * UIKit has no status bar for that and shows its fallback, a red bar reading rdar://45025538; nor for this panel's own
 * 360 x 800 points at 3 (fbwidth=1080 fbheight=2400, a pixel of the picture for a pixel of the panel). The status bar of the
 * iPhone of about that width does for it, it goes by points: the fallback class answers its class methods as that
 * iPhone's does, and what is allocated of it is that iPhone's (the red bar is gone with this, 4 November; a hook on
 * +visualProviderSubclassForScreen:visualProviderInfo: alone left it).
 */
static Class status_bar_split;

static id
status_bar_fallback_alloc(id self __unused, SEL selector)
{
	static unsigned said;

	if (said++ < 2) {
		say("hakios: SpringBoard: the status bar's fallback asked for: %s it is\n", class_getName(status_bar_split));
	}
	return ((id (*)(Class, SEL))objc_msgSend)(status_bar_split, selector);
}

static id
status_bar_fallback_alloc_zone(id self __unused, SEL selector, void *zone)
{
	return ((id (*)(Class, SEL, void *))objc_msgSend)(status_bar_split, selector, zone);
}

static void
status_bar_fallback_is_split(void)
{
	Class fallback = objc_getClass("_UIStatusBarVisualProvider_Fallback");
	/* (no UIScreen here: this runs in the library's constructor, before UIKit is up -- +[UIScreen mainScreen] asserted) */
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *at;
	unsigned width = 1125;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "fbwidth=")) != NULL) {
		width = (unsigned)atoi(at + 8);
	}
	status_bar_split = objc_getClass(width == 1080 ? "_UIStatusBarVisualProvider_Split1080" : "_UIStatusBarVisualProvider_Split1125");
	if (fallback == NULL || status_bar_split == NULL) {
		say("hakios: SpringBoard: no status bar fallback or split provider class (%p %p)\n", (void *)fallback, (void *)status_bar_split);
		return;
	}
	/* the class methods from the split provider up to what the two have in common, the nearest last to win */
	Class chain[8];
	unsigned depth = 0, moved = 0;

	for (Class c = status_bar_split; c != NULL && depth < 8; c = class_getSuperclass(c)) {
		bool common = false;

		for (Class f = fallback; f != NULL; f = class_getSuperclass(f)) {
			common = common || f == c;
		}
		if (common) {
			say("hakios: SpringBoard: the status bar's providers have %s in common, the fallback's parent is %s\n", class_getName(c),
			    class_getName(class_getSuperclass(fallback)));
			break;
		}
		chain[depth++] = c;
	}
	while (depth-- > 0) {
		unsigned count = 0;
		Method *methods = class_copyMethodList(object_getClass(chain[depth]), &count);

		for (unsigned i = 0; i < count; i++) {
			const char *name = sel_getName(method_getName(methods[i]));

			if (strcmp(name, "load") != 0 && strcmp(name, "initialize") != 0 && strcmp(name, "alloc") != 0 && strcmp(name, "allocWithZone:") != 0) {
				class_replaceMethod(object_getClass(fallback), method_getName(methods[i]), method_getImplementation(methods[i]),
				    method_getTypeEncoding(methods[i]));
				moved++;
			}
		}
		free(methods);
	}
	class_replaceMethod(object_getClass(fallback), sel_registerName("alloc"), (IMP)status_bar_fallback_alloc, "@16@0:8");
	class_replaceMethod(object_getClass(fallback), sel_registerName("allocWithZone:"), (IMP)status_bar_fallback_alloc_zone, "@24@0:8^v16");
	say("hakios: SpringBoard: the status bar's fallback provider is %s from now on (%u class methods)\n", class_getName(status_bar_split), moved);
}


static void
wallpaper_cache(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 ||
	    (argument = strstr(arguments, "wallpaper_cache=1")) == NULL || (argument != arguments && argument[-1] != ' ')) {
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		wallpaper_cache_pass();
	});
}

/*
 * The switcher of running applications lays a wash of wallpaper over every card behind the front
 * one, above the dimming: -[SBAppSwitcherPageView _createOrDestroyWallpaperOverlayView] makes it,
 * PaperBoardUI fills it from the image provider of the wallpaper, and the result is drawn at about
 * fifteen per cent (the layer tree: SBWallpaperEffectView at opacity 0.1497 -> PBUIFakeBlurView ->
 * PBUIWallpaperView -> PBUISnapshotReplicaView -> a UIImageView).
 *
 * Here there is no picture of the wallpaper to put in it. The wallpaper itself is drawn live by
 * PosterBoard, in another process, and hosted into SpringBoard's layer tree; SpringBoard has no
 * bitmap of it, and nothing ever bakes one -- after three and a half minutes of an idle phone
 * /var/mobile/Library holds sixty directories and not one of them is PaperBoard, PosterBoard or
 * Wallpaper. So the provider hands over a placeholder: 846 x 1827 pixels of one colour,
 * (179, 77, 179), which CoreAnimation compresses to ASTC and draws over the two cards on the left
 * at 14.9 per cent. That is what made them purple instead of merely dimmed.
 *
 * The dimming itself is right and stays: two draws of a flat black at 11.4 per cent, which is the
 * shadow one expects to see. Only the wash is left out, because a wash of a colour that is not the
 * wallpaper's is worse than no wash at all. When something here can give PaperBoardUI a real
 * picture of the wallpaper, this hook is what should go -- and until then switcher_wallpaper=1
 * among the boot-args puts it back as Darwin asks for it, so that whoever works on the wallpaper can
 * see at once whether their picture arrives, without rebuilding an image to find out.
 */
static void (*original_switcher_wallpaper_overlay)(id self, SEL selector);

static void
switcher_wallpaper_overlay(id self, SEL selector)
{
	static int wanted = -1;         /* boot-arg switcher_wallpaper=1: have it back, placeholder and all */
	static unsigned told;

	if (wanted < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		wanted = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "switcher_wallpaper=1")) != NULL && (at == args || at[-1] == ' ');
	}
	if (told++ == 0) {
		say("hakios: the switcher's wash of wallpaper is %s\n", wanted ?
		    "drawn as Darwin asks (switcher_wallpaper=1)" : "left out: there is no picture of the wallpaper for it");
	}
	if (wanted) {
		original_switcher_wallpaper_overlay(self, selector);
	}
}

static void
layer_filter(id filter, const char *what, int depth)
{
	static const char *const inputs[] = { "inputColorMatrix", "inputRadius", "inputAmount", "inputColor", "inputAngle",
		                              "inputPremultipliedValues", "inputNormalizeEdges", "inputReversed" };
	char line[1024];
	size_t n;

	if (!class_respondsToSelector(object_getClass(filter), sel_registerName("type"))) {
		say("hakios: layers%s: %*s  %s %s\n", layers_of, 2 * depth, "", what, text_of(filter));     /* a name alone */
		return;
	}
	n = (size_t)snprintf(line, sizeof(line), "%s %s \"%s\"%s", what, text_of(SEND(id, filter, "type")),
	    text_of(SEND(id, filter, "name")), SEND(BOOL, filter, "isEnabled") ? "" : " (disabled)");
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]) && n < sizeof(line); i++) {
		id key = SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, inputs[i]);
		id value = SEND1(id, filter, "valueForKey:", id, key);
		float m[20];

		if (value == NULL) {
			continue;
		}
		if (i == 0 && class_respondsToSelector(object_getClass(value), sel_registerName("CAColorMatrixValue"))) {
			SEND1(void, value, "getValue:", void *, m);
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s [%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g | "
			    "%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g]", inputs[i], m[0], m[1], m[2], m[3], m[4], m[5], m[6],
			    m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19]);
		} else {
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s %s", inputs[i], text_of(value));
		}
	}
	say("hakios: layers%s: %*s  %s\n", layers_of, 2 * depth, "", line);
}

static void
layer_line(id layer, const char *role, int depth)
{
	struct { double x, y, w, h; } frame = SEND(typeof(frame), layer, "frame");
	const char *class_name = object_getClassName(layer);
	id contents = SEND(id, layer, "contents"), filters = SEND(id, layer, "filters"), mask = SEND(id, layer, "mask");
	id compositing = SEND(id, layer, "compositingFilter"), sublayers = SEND(id, layer, "sublayers");
	id background = SEND(id, layer, "backgroundFilters");
	void *colour = SEND(void *, layer, "backgroundColor");
	char more[1024] = "";
	size_t n = 0;

	if (SEND(BOOL, layer, "isHidden")) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HIDDEN");
	}
	if (contents != NULL) {
		id text = (id)CFCopyDescription((CFTypeRef)contents);
		const char *from = text_of(text);
		char flat[300];
		size_t k = 0;

		/*
		 * What a CGImage says about itself runs over several lines -- its colour space, how big
		 * it is, how its pixels are laid out -- and all of that is worth having on the one line,
		 * because the size and the colour space are the first things to look at when a picture
		 * comes out wrong. Every run of spaces and newlines becomes one space.
		 */
		while (from != NULL && *from != 0 && k + 1 < sizeof(flat)) {
			if (*from == ' ' || *from == '\n' || *from == '\t' || *from == '\r') {
				if (k > 0 && flat[k - 1] != ' ') {
					flat[k++] = ' ';
				}
				from++;
				continue;
			}
			flat[k++] = *from++;
		}
		flat[k] = 0;
		n += (size_t)snprintf(more + n, sizeof(more) - n, " contents %s", flat);
		CFRelease((CFTypeRef)text);
	}
	if (contents != NULL && class_respondsToSelector(object_getClass(layer), sel_registerName("contentsMultiplyColor")) &&
	    SEND(void *, layer, "contentsMultiplyColor") != NULL) {
		id text = (id)CFCopyDescription(SEND(void *, layer, "contentsMultiplyColor"));
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " times %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (colour != NULL) {
		id text = (id)CFCopyDescription(colour);
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " colour %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (strcmp(class_name, "CAPortalLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " PORTAL of %p%s%s", (void *)SEND(id, layer, "sourceLayer"),
		    SEND(BOOL, layer, "hidesSourceLayer") ? " (hides it)" : "", SEND(BOOL, layer, "matchesOpacity") ? " (its opacity)" : "");
	} else if (strcmp(class_name, "CABackdropLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " BACKDROP group \"%s\" scale %g%s", text_of(SEND(id, layer, "groupName")),
		    SEND(double, layer, "scale"), SEND(BOOL, layer, "captureOnly") ? " (capture only)" : "");
	} else if (strcmp(class_name, "CAGradientLayer") == 0) {
		struct { double x, y; } from = SEND(typeof(from), layer, "startPoint"), to = SEND(typeof(to), layer, "endPoint");
		id colours = SEND(id, layer, "colors");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " GRADIENT %s from %g,%g to %g,%g at %.40s:", text_of(SEND(id, layer, "type")),
		    from.x, from.y, to.x, to.y, text_of(SEND(id, layer, "locations")));
		for (long i = 0; colours != NULL && i < SEND(long, colours, "count") && n < sizeof(more); i++) {
			id text = (id)CFCopyDescription((CFTypeRef)SEND1(id, colours, "objectAtIndex:", long, i));
			const char *numbers = strstr(text_of(text), "( ");

			n += (size_t)snprintf(more + n, sizeof(more) - n, " %.30s", numbers ? numbers : "?");
			CFRelease((CFTypeRef)text);
		}
	} else if (strcmp(class_name, "CAShapeLayer") == 0) {
		void *stroke = SEND(void *, layer, "strokeColor"), *fill = SEND(void *, layer, "fillColor");
		id text_stroke = stroke ? (id)CFCopyDescription(stroke) : NULL, text_fill = fill ? (id)CFCopyDescription(fill) : NULL;
		const char *ns = text_stroke ? strstr(text_of(text_stroke), "( ") : NULL, *nf = text_fill ? strstr(text_of(text_fill), "( ") : NULL;

		n += (size_t)snprintf(more + n, sizeof(more) - n, " SHAPE line %g stroke %.30s fill %.30s ends %g-%g", SEND(double, layer,
		    "lineWidth"), ns ? ns : "-", nf ? nf : "-", SEND(double, layer, "strokeStart"), SEND(double, layer, "strokeEnd"));
		if (text_stroke != NULL) {
			CFRelease((CFTypeRef)text_stroke);
		}
		if (text_fill != NULL) {
			CFRelease((CFTypeRef)text_fill);
		}
	} else if (strcmp(class_name, "CALayerHost") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HOST of context %#x%s%s", SEND(unsigned, layer, "contextId"),
		    SEND(BOOL, layer, "inheritsSecurity") ? " (inherits security)" : "",
		    SEND(BOOL, layer, "rendersAsynchronously") ? " (asynchronously)" : "");
	}
	{
		/* the view a layer belongs to, which is the only thing that names a layer UIKit left
		   unnamed -- and the switcher's wash of wallpaper is such a layer */
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " of %s", object_getClassName(delegate));
		}
	}
	{
		id keys = SEND(id, layer, "animationKeys");

		if (SEND(BOOL, layer, "shouldRasterize")) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " RASTERIZED");
		}
		for (long i = 0; keys != NULL && i < SEND(long, keys, "count") && n < sizeof(more); i++) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, "%s%s", i == 0 ? " ANIMATIONS " : ",",
			    text_of(SEND1(id, keys, "objectAtIndex:", long, i)));
		}
	}
	say("hakios: layers%s: %*s%s%s %p \"%s\" %g,%g %gx%g opacity %g%s\n", layers_of, 2 * depth, "", role, class_name, (void *)layer,
	    text_of(SEND(id, layer, "name")), frame.x, frame.y, frame.w, frame.h, (double)SEND(float, layer, "opacity"), more);
	for (long i = 0; filters != NULL && i < SEND(long, filters, "count"); i++) {
		layer_filter(SEND1(id, filters, "objectAtIndex:", long, i), "filter", depth);
	}
	for (long i = 0; background != NULL && i < SEND(long, background, "count"); i++) {
		layer_filter(SEND1(id, background, "objectAtIndex:", long, i), "background filter", depth);
	}
	if (compositing != NULL) {
		layer_filter(compositing, "compositing filter", depth);
	}
	if (mask != NULL) {
		layer_line(mask, "MASK ", depth + 1);
	}
	for (long i = 0; sublayers != NULL && i < SEND(long, sublayers, "count"); i++) {
		layer_line(SEND1(id, sublayers, "objectAtIndex:", long, i), "", depth + 1);
	}
}

/* Every WKWebView under a view: whether WebKit takes it for one in the background, its web process and whether that answers. */
static void
web_views(id view, int depth)
{
	Class web = objc_getClass("WKWebView");
	id subviews;
	bool is_web = false;

	if (view == NULL || depth > 40) {
		return;
	}

	for (Class c = object_getClass(view); web != NULL && c != NULL && !is_web; c = class_getSuperclass(c)) {
		is_web = c == web;
	}
	if (is_web) {
		SEL background = sel_registerName("_isBackground"), pid = sel_registerName("_webProcessIdentifier");
		SEL responsive = sel_registerName("_webProcessIsResponsive");

		say("hakios: layers%s: a WKWebView %p: in the background %s, web process %d, responsive %s, in a window %s, hidden %s\n",
		    layers_of, (void *)view, class_respondsToSelector(object_getClass(view), background) ?
		    (SEND(BOOL, view, "_isBackground") ? "YES" : "no") : "?", class_respondsToSelector(object_getClass(view), pid) ?
		    SEND(int, view, "_webProcessIdentifier") : -1, class_respondsToSelector(object_getClass(view), responsive) ?
		    (SEND(BOOL, view, "_webProcessIsResponsive") ? "yes" : "NO") : "?", SEND(id, view, "window") ? "yes" : "NO",
		    SEND(BOOL, view, "isHidden") ? "YES" : "no");
	}
	subviews = SEND(id, view, "subviews");
	for (long i = 0; subviews != NULL && i < SEND(long, subviews, "count"); i++) {
		web_views(SEND1(id, subviews, "objectAtIndex:", long, i), depth + 1);
	}
}

static void
layers(void)
{
	Class window = objc_getClass("UIWindow");
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	id windows = window && class_getClassMethod(window, all) ? ((id (*)(Class, SEL, BOOL, BOOL))objc_msgSend)(window, all, 1, 1) : NULL;
	id application = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id authentication = application && class_respondsToSelector(object_getClass(application),
	    sel_registerName("authenticationController")) ? SEND(id, application, "authenticationController") : NULL;

	say("hakios: layers%s: %ld windows\n", layers_of, windows ? SEND(long, windows, "count") : 0L);
	if (authentication != NULL) {
		say("hakios: layers: the user is %sauthenticated, %s passcode, the display %sin secure mode\n",
		    SEND(BOOL, authentication, "isAuthenticated") ? "" : "NOT ", SEND(BOOL, authentication, "hasPasscodeSet") ? "a" : "no",
		    SEND(BOOL, authentication, "inSecureDisplayMode") ? "" : "not ");
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		id w = SEND1(id, windows, "objectAtIndex:", long, i);
		id scene = class_respondsToSelector(object_getClass(w), sel_registerName("windowScene")) ? SEND(id, w, "windowScene") : NULL;
		id context = class_respondsToSelector(object_getClass(w), sel_registerName("_boundContext")) ? SEND(id, w, "_boundContext") : NULL;

		say("hakios: layers%s: window %s level %g, context %#x (%ssecure, %u commits), scene %s (activation state %ld)\n",
		    layers_of, object_getClassName(w), SEND(double, w, "windowLevel"), context ? SEND(unsigned, context, "contextId") : 0,
		    context && SEND(BOOL, context, "isSecure") ? "" : "not ", context ? (unsigned)SEND(unsigned long long, context, "commitId") : 0,
		    scene ? text_of(SEND(id, scene, "_persistenceIdentifier")) : "none", scene ? SEND(long, scene, "activationState") : -1L);
		layer_line(SEND(id, w, "layer"), "", 1);
	}
	if (application != NULL) {
		say("hakios: layers%s: the application's state %ld (0 active, 1 inactive, 2 in the background)\n", layers_of,
		    SEND(long, application, "applicationState"));
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		web_views(SEND1(id, windows, "objectAtIndex:", long, i), 0);
	}
	say("hakios: layers%s: end\n", layers_of);
}

/*
 * The switcher of running applications, on the notification com.hakios.switcher (rwtouch notify).
 * A drag up from the bottom edge is the way to it on a phone, but the touches a script puts in do
 * not reach Darwin's system gestures -- nothing comes of such a drag, not even going home -- and two
 * presses of the home button only go home (there is no home button on a phone of this shape, and
 * what is sent as one goes home and no more). SpringBoard opens it itself for anything that is
 * not a gesture (a hardware keyboard, accessibility): SBMainSwitcherControllerCoordinator, which
 * is one for the whole program, takes "activate the main switcher not interactively, from this
 * source". The source is an analytics label; zero is "none", which is what the ones that have no
 * gesture behind them pass.
 * As it stands this is not enough: with three applications running it answers that it took the
 * request (source 0, animated and not animated alike) and the screen does not change -- the
 * application in front stays, and SpringBoard says nothing more. So the switcher still cannot be
 * reached from a script; what is left to try is the transition with a window scene
 * (_activateSwitcherValidatorWithEventLabel:animated:windowScene:), which is what its own
 * gestures use.
 */
static void
switcher(void)
{
	Class coordinator = objc_getClass("SBMainSwitcherControllerCoordinator");
	SEL activate = sel_registerName("activateMainSwitcherNoninteractivelyWithSource:animated:");
	id shared = coordinator != NULL ? SEND(id, coordinator, "sharedInstance") : NULL;
	Method method = shared != NULL ? class_getInstanceMethod(object_getClass(shared), activate) : NULL;

	if (method == NULL) {
		say("hakios: the switcher: %s has no %s\n",
		    coordinator != NULL ? "SBMainSwitcherControllerCoordinator" : "nothing", sel_getName(activate));
		return;
	}
	say("hakios: the switcher: opening it (%s, %s)\n", object_getClassName(shared),
	    method_getTypeEncoding(method) != NULL ? method_getTypeEncoding(method) : "?");
	/*
	 * It answers whether it took the request. The source is an analytics label and not every one
	 * of them is allowed from every state, so they are tried in turn until one is taken.
	 */
	/*
	 * Not animated: the animated one is taken and then nothing comes of it -- the transition it
	 * starts is the gesture's to carry, and there is no gesture here.
	 */
	for (long source = 0; source < 10; source++) {
		BOOL taken = ((BOOL (*)(id, SEL, long, BOOL))objc_msgSend)(shared, activate, source, NO);

		say("hakios: the switcher: from source %ld: %s\n", source, taken ? "taken" : "refused");
		if (taken) {
			return;
		}
	}
}

/*
 * Control Center's glyphs whole from its first frames. Its modules and their parts fade in as it
 * comes down: opacity animations, mostly springs that take up to a second to settle at 1. While a
 * layer's opacity is below 1, the window server's OpenGL ES renderer draws the vibrant glyphs in it
 * from a flattened copy that lacks the strokes composited against what is behind them, and draws them
 * whole only at full opacity: the strokes of the media controls, orientation lock, screen mirroring
 * and Focus came in only when the springs had settled, at the end of the presentation. (On an iPhone
 * they are whole all along.) So in Control Center's window an opacity animation is replaced with an
 * eased-out fade of cc_fade=MS between the same values: the modules still fade in and out, and are at
 * full opacity, glyphs whole, after a few frames. Off by default (Darwin's own animations stay as they
 * are; the real fix belongs in the drawing, not in the animations): a workaround, for checks only.
 */
#define CC_FADE_MS      0
static void (*original_add_animation_cc)(id self, SEL selector, id animation, id key);
static double cc_fade_s = CC_FADE_MS / 1000.0;
static unsigned cc_fades;

static bool
in_control_center(id layer)
{
	for (int depth = 0; layer != NULL && depth < 64; depth++) {
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL && strcmp(object_getClassName(delegate), "SBControlCenterWindow") == 0) {
			return true;
		}
		layer = SEND(id, layer, "superlayer");
	}
	return false;
}

static void
add_animation_cc(id self, SEL selector, id animation, id key)
{
	Class basic = objc_getClass("CABasicAnimation");
	id path = animation != NULL && class_respondsToSelector(object_getClass(animation), sel_registerName("keyPath")) ?
	    SEND(id, animation, "keyPath") : NULL;

	if (path != NULL && strcmp(text_of(path), "opacity") == 0 &&
	    class_respondsToSelector(object_getClass(animation), sel_registerName("fromValue")) && in_control_center(self)) {
		id fade = SEND1(id, basic, "animationWithKeyPath:", id, path);
		id ease = SEND1(id, objc_getClass("CAMediaTimingFunction"), "functionWithName:", id,
		    SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, "easeOut"));

		SEND1(void, fade, "setFromValue:", id, SEND(id, animation, "fromValue"));
		SEND1(void, fade, "setToValue:", id, SEND(id, animation, "toValue"));
		SEND1(void, fade, "setByValue:", id, SEND(id, animation, "byValue"));
		SEND1(void, fade, "setAdditive:", BOOL, SEND(BOOL, animation, "isAdditive"));
		SEND1(void, fade, "setBeginTime:", double, SEND(double, animation, "beginTime"));
		SEND1(void, fade, "setFillMode:", id, SEND(id, animation, "fillMode"));
		SEND1(void, fade, "setRemovedOnCompletion:", BOOL, SEND(BOOL, animation, "isRemovedOnCompletion"));
		SEND1(void, fade, "setDelegate:", id, SEND(id, animation, "delegate"));
		SEND1(void, fade, "setDuration:", double, cc_fade_s);
		SEND1(void, fade, "setTimingFunction:", id, ease);
		if (cc_fades++ < 3) {
			say("hakios: Control Center: an opacity animation of %.3f s (%s) made a fade of %.3f s\n",
			    SEND(double, animation, "duration"), object_getClassName(animation), cc_fade_s);
		}
		animation = fade;
	}
	original_add_animation_cc(self, selector, animation, key);
}

static void
control_center_fades(const char *arguments)
{
	const char *setting = strstr(arguments, "cc_fade=");
	Method add = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("addAnimation:forKey:"));

	if (setting != NULL) {
		cc_fade_s = atof(setting + 8) / 1000.0;
	}
	if (cc_fade_s <= 0 || add == NULL) {
		return;
	}
	original_add_animation_cc = (void (*)(id, SEL, id, id))method_getImplementation(add);
	method_setImplementation(add, (IMP)add_animation_cc);
}

/*
 * The frame rates SpringBoard's display links ask for. After a page of the home screen settles, something of
 * SpringBoard's goes on drawing at exactly 30 a second for most of a second (its run loop and the window server's
 * frames 33.3 ms apart, tl: a and tl: g). It is UIKit's: the display link of UIViewInProcessAnimationManager, which
 * runs the animations done in the application's own process, asks for 20..30. With dl_max=1 (the default) the rates
 * asked of that one are dropped and it runs at the panel's rate; on an iPhone with ProMotion the lower rate saves
 * power, here the panel refreshes at 120 whatever is drawn. The other display links keep what they ask (the clock's
 * 10 and 60, the motion effects' 32): run at 120 they woke SpringBoard six times as often for nothing.
 * With touch_lat=1 every display link made and every rate asked of one is said ("hakios: display link ...").
 */
typedef struct {
	float minimum, maximum, preferred;
} frame_rate_range;

static id (*original_display_link_with)(id self, SEL selector, id target, SEL action);
static void (*original_set_fps)(id self, SEL selector, long fps);
static void (*original_set_range)(id self, SEL selector, frame_rate_range range);
static bool dl_say, dl_max = true;
static unsigned dl_said;
#define DL_SAY_MAX 300
#define DL_FULL_FOR "UIViewInProcessAnimationManager"
static char dl_full;                    /* the key of the mark a display link made for DL_FULL_FOR carries */

/* (a mark on the link itself: UIKit makes one of these for every animation, and a list of them filled up at once) */
static bool
full_rate(id link)
{
	return dl_max && objc_getAssociatedObject(link, &dl_full) != NULL;
}

static const char *
caller(const void *address, char *buffer, size_t size)
{
	Dl_info info;
	const char *image;

	if (dladdr(address, &info) == 0 || info.dli_fname == NULL) {
		snprintf(buffer, size, "%p", address);
		return buffer;
	}
	image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
	snprintf(buffer, size, "%s+%#lx", image, (unsigned long)((uintptr_t)address - (uintptr_t)info.dli_fbase));
	return buffer;
}

static id
display_link_with(id self, SEL selector, id target, SEL action)
{
	id link = original_display_link_with(self, selector, target, action);
	char who[256];

	if (link != NULL && target != NULL && strcmp(object_getClassName(target), DL_FULL_FOR) == 0) {
		objc_setAssociatedObject(link, &dl_full, (id)kCFBooleanTrue, OBJC_ASSOCIATION_ASSIGN);
	}
	if (dl_say && dl_said++ < DL_SAY_MAX) {
		say("hakios: display link %p made for -[%s %s] by %s\n", (void *)link, target ? object_getClassName(target) : "nil",
		    action ? sel_getName(action) : "", caller(__builtin_return_address(0), who, sizeof(who)));
	}
	return link;
}

static void
set_fps(id self, SEL selector, long fps)
{
	char who[256];

	if (dl_say && dl_said++ < DL_SAY_MAX) {
		say("hakios: display link %p asked for %ld a second%s by %s\n", (void *)self, fps, full_rate(self) ? " (dropped)" : "",
		    caller(__builtin_return_address(0), who, sizeof(who)));
	}
	original_set_fps(self, selector, full_rate(self) ? 0 : fps);
}

static void
set_range(id self, SEL selector, frame_rate_range range)
{
	char who[256];

	if (dl_say && dl_said++ < DL_SAY_MAX) {
		say("hakios: display link %p asked for %.0f..%.0f, %.0f a second%s by %s\n", (void *)self, range.minimum,
		    range.maximum, range.preferred, full_rate(self) ? " (dropped)" : "", caller(__builtin_return_address(0), who, sizeof(who)));
	}
	if (full_rate(self)) {
		range = (frame_rate_range){ 0, 0, 0 };
	}
	original_set_range(self, selector, range);
}

static bool
boot_arg(const char *arguments, const char *name)
{
	const char *at = strstr(arguments, name);

	return at != NULL && (at == arguments || at[-1] == ' ');
}

/*
 * The animations SpringBoard hands the window server (an application opening from its icon and closing to it, the
 * home screen coming back, folders): the window server moves them at the rate each asks for (CAAnimation's
 * preferredFrameRateRange). Through Metal (METAL_PLUGIN=ca) it keeps to that, and what asks for nothing went at 24 a
 * second -- frames exactly 41.7 ms apart with the GPU 0.3 ms busy each; through OpenGL ES it did not. With anim_max=1
 * every animation added to a layer of SpringBoard's asks for 80..120 (a copy of it: one already added is read-only,
 * and setting it raised). touch_lat=1 says the first ones and what they asked.
 */
static void (*original_add_animation)(id self, SEL selector, id animation, id key);
static unsigned anim_said;

static void
add_animation_full_rate(id self, SEL selector, id animation, id key)
{
	SEL range_of = sel_registerName("preferredFrameRateRange"), set_range_of = sel_registerName("setPreferredFrameRateRange:");

	if (animation != NULL && class_respondsToSelector(object_getClass(animation), set_range_of)) {
		frame_rate_range asked = ((frame_rate_range (*)(id, SEL))objc_msgSend)(animation, range_of);

		if (dl_say && anim_said++ < 40) {
			say("hakios: animation %s (%s) asked for %.0f..%.0f, %.0f a second\n", object_getClassName(animation),
			    key != NULL ? text_of(key) : "", asked.minimum, asked.maximum, asked.preferred);
		}
		if (asked.maximum == 0 || asked.maximum < 120 || asked.minimum < 80) {
			id copy = SEND(id, animation, "copy");

			((void (*)(id, SEL, frame_rate_range))objc_msgSend)(copy, set_range_of, (frame_rate_range){ 80, 120, 120 });
			original_add_animation(self, selector, copy, key);
			SEND(void, copy, "release");
			return;
		}
	}
	original_add_animation(self, selector, animation, key);
}

static void
animation_rates(const char *arguments)
{
	Method add = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("addAnimation:forKey:"));

	if (!boot_arg(arguments, "anim_max=1") || add == NULL) {
		return;
	}
	original_add_animation = (void (*)(id, SEL, id, id))method_setImplementation(add, (IMP)add_animation_full_rate);
	say("hakios: SpringBoard's animations ask for 80..120 a second (anim_max=1)\n");
}

static void
display_link_rates(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	Class link = objc_getClass("CADisplayLink");
	Method made, fps, range;

	if (link == NULL || sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	dl_say = boot_arg(arguments, "touch_lat=1");
	dl_max = !boot_arg(arguments, "dl_max=0");
	animation_rates(arguments);
	if (!dl_say && !dl_max) {
		return;
	}
	if ((made = class_getClassMethod(link, sel_registerName("displayLinkWithTarget:selector:"))) != NULL) {
		original_display_link_with = (id (*)(id, SEL, id, SEL))method_setImplementation(made, (IMP)display_link_with);
	}
	if ((fps = class_getInstanceMethod(link, sel_registerName("setPreferredFramesPerSecond:"))) != NULL) {
		original_set_fps = (void (*)(id, SEL, long))method_setImplementation(fps, (IMP)set_fps);
	}
	if ((range = class_getInstanceMethod(link, sel_registerName("setPreferredFrameRateRange:"))) != NULL) {
		original_set_range = (void (*)(id, SEL, frame_rate_range))method_setImplementation(range, (IMP)set_range);
	}
	say("hakios: display links: made %s, fps %s, range %s%s\n", made ? "ok" : "NO", fps ? "ok" : "NO", range ? "ok" : "NO",
	    dl_max ? "; " DL_FULL_FOR "'s at the panel's rate (dl_max=1)" : "");
}

/*
 * Boot-args ax_rt=0|1 and ax_rm=0|1: Settings' Reduce Transparency and Reduce Motion, set as SpringBoard starts
 * (libAccessibility's own setters, which keep them in its preferences as Settings does). Without them nothing is
 * set. Reduce Transparency puts plain colours where CoreAnimation would blur what is behind (the dock, folders, an
 * application opening over the home screen): the blurs are what the GPU spends most of an application's opening on.
 */
static void
accessibility_settings(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	static const struct {
		const char *arg, *setter, *what;
	} settings[] = {
		{ "ax_rt=", "_AXSSetEnhanceBackgroundContrastEnabled", "Reduce Transparency" },
		{ "ax_rm=", "_AXSSetReduceMotionEnabled", "Reduce Motion" },
	};
	void *library;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
		const char *at = strstr(arguments, settings[i].arg);
		void (*set)(Boolean);

		if (at == NULL || (at != arguments && at[-1] != ' ')) {
			continue;
		}
		if ((library = dlopen("/usr/lib/libAccessibility.dylib", RTLD_NOW)) == NULL ||
		    (set = (void (*)(Boolean))dlsym(library, settings[i].setter)) == NULL) {
			say("hakios: %s: no %s in libAccessibility\n", settings[i].what, settings[i].setter);
			continue;
		}
		set(atoi(at + strlen(settings[i].arg)) != 0);
		say("hakios: %s %s (%s%d)\n", settings[i].what, atoi(at + strlen(settings[i].arg)) ? "on" : "off", settings[i].arg,
		    atoi(at + strlen(settings[i].arg)));
	}
}

/*
 * Open the tweak loader of the package manager's prefix, when there is one. Nothing here depends on
 * it: without a prefix (or without ElleKit in it) the file is not there and this does nothing.
 */
#define HAKIOS_TWEAK_LOADER    "/redwood/lib/libhakios.tweaks.dylib"

static void
load_tweaks(void)
{
	struct stat st;

	if (stat("/var/jb/usr/lib/TweakInject", &st) != 0) {
		return;                 /* no package manager's prefix here, so no tweaks */
	}
	if (dlopen(HAKIOS_TWEAK_LOADER, RTLD_NOW | RTLD_GLOBAL) == NULL) {
		say("hakios: the tweak loader %s: %s\n", HAKIOS_TWEAK_LOADER, dlerror());
	} else {
		say("hakios: tweaks: %s opened\n", HAKIOS_TWEAK_LOADER);
	}
}

/*
 * Boot-arg caenv=NAME=VALUE: CoreAnimation's switches here as in the window server (inject.c), set before QuartzCore
 * reads them. The ones that hold the frame rate back (CA_MINIMUM_FRAME_DURATION and its kind) are read by the
 * process that animates as well as by the one that composes, and SpringBoard animates the home screen.
 */
static void
caenv(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	for (const char *argument = arguments; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
		char setting[128];
		size_t length = strcspn(argument + 6, " ");
		char *equals;

		if ((argument != arguments && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
			continue;
		}
		memcpy(setting, argument + 6, length);
		setting[length] = 0;
		if ((equals = strchr(setting, '=')) != NULL) {
			*equals = 0;
			/* a fraction is written 0p25: the boot-arguments lose what follows a "." of a number (0.008 came as 0) */
			for (char *point = equals + 1; *point != 0; point++) {
				if (*point == 'p' && point > equals + 1 && point[-1] >= '0' && point[-1] <= '9' && point[1] >= '0' && point[1] <= '9') {
					*point = '.';
					break;
				}
			}
			setenv(setting, equals + 1, 1);
			say("hakios: caenv: %s=%s for SpringBoard\n", setting, equals + 1);
		}
	}
}

/* ---- the wallpaper's material pictures (PaperBoardUI) ------------------------------------------- */

/*
 * The purple of the materials (PURPLE-umi.md). PaperBoardUI makes the pictures its fake-blur materials,
 * Reachability's wallpaper and the wallpaper previews show by "treating" a snapshot of the wallpaper:
 * -[PBUIMaterialCompositor applyEffect:toImage:] and its kin send a PBUITreatImageRequest to a renderer
 * (PBUICARemoteRenderer, the SBRendererService XPC service, from SpringBoard; PBUICALocalRenderer in the
 * service), which lays the picture into a PBUIImageContainerLayer whose background is magentaColor
 * (-[PBUITreatImageRequest configureState:error:]), puts the material's filters on it and draws it with
 * CARenderer into a Metal texture. The service has no Metal device here (HakiosMetalCA.m shows it to the
 * window server alone, unless metal_rs=1), so what comes back is no picture, or the magenta background
 * alone -- blurred and tinted by the material into the flat purples of the logs (RGBA 89,20,89; 179,77,179;
 * 249,198,249: red = blue, which the materials' symmetric matrices keep). PaperBoardUI then writes such a
 * picture into its snapshot cache (PBUIURLBackedSnapshotSource, ATX: the flat ASTC blocks the GL logs
 * caught), which a persistent /var keeps from boot to boot.
 *
 * Here, in SpringBoard:
 *   - a treated picture that comes back as nothing, or flat magenta or flat clear, is made on the processor
 *     instead: the request's own picture (or, when that is unusable too, the one wallpaper_freeze=1 keeps)
 *     drawn at the size asked for and blurred (three box blurs, the material's tint left out);
 *   - a snapshot read from the cache that is flat magenta is thrown away and asked for again;
 *   - a new snapshot that is flat magenta or clear is replaced by the wallpaper_freeze picture, if one is kept.
 * The addresses are Darwin userland 16.5 (20F66), read off PaperBoardUI's own method lists.
 */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include "HakiosMetalPixels.h"

/* IMAGE drawn into 16 x 16 pixels (RGBA, premultiplied): whether it is one colour (every pixel within 3 of the
 * first) and that colour; NO for an image that cannot be drawn */
static bool
picture_flat(CGImageRef image, uint8_t rgba[4])
{
	uint8_t px[16 * 16 * 4];
	CGColorSpaceRef space;
	CGContextRef context;
	bool flat = true;

	if (image == NULL || CGImageGetWidth(image) < 32 || CGImageGetHeight(image) < 32) {
		return false;
	}
	space = CGColorSpaceCreateDeviceRGB();
	context = CGBitmapContextCreate(px, 16, 16, 8, 64, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	CGColorSpaceRelease(space);
	if (context == NULL) {
		return false;
	}
	memset(px, 0, sizeof(px));
	CGContextSetInterpolationQuality(context, kCGInterpolationLow);
	CGContextDrawImage(context, CGRectMake(0, 0, 16, 16), image);
	CGContextRelease(context);
	for (unsigned i = 1; i < 16 * 16 && flat; i++) {
		for (unsigned c = 0; c < 4 && flat; c++) {
			flat = abs((int)px[4 * i + c] - (int)px[c]) <= 3;
		}
	}
	memcpy(rgba, px, 4);
	return flat;
}

/* a picture that stands for a failed treatment: flat magenta, or flat and clear */
static bool
picture_failed(CGImageRef image, uint8_t rgba[4])
{
	return picture_flat(image, rgba) && (hm_rgba_magenta(rgba) || rgba[3] == 0);
}

/*
 * A snapshot of the wallpaper that is purple: flat purple or clear (as above), or with an eighth or more of it the
 * magenta of a texture the GPU could not read -- (255, 0, 255), the error colour of Mesa's ASTC decoder, which the
 * GL renderer of earlier boots drew where it failed and which RuntimeSnapshot-home.atx still held on 10 November.
 * *SHARE: that part, in 1024ths (a 32 x 32 sample).
 */
static bool
snapshot_bad(CGImageRef image, uint8_t rgba[4], unsigned *share)
{
	uint8_t px[32 * 32 * 4];
	CGColorSpaceRef space;
	CGContextRef context;
	unsigned magenta = 0;

	*share = 0;
	if (picture_failed(image, rgba)) {
		*share = hm_rgba_magenta(rgba) ? 1024 : 0;
		return true;
	}
	if (image == NULL || CGImageGetWidth(image) < 32 || CGImageGetHeight(image) < 32) {
		return false;
	}
	space = CGColorSpaceCreateDeviceRGB();
	context = CGBitmapContextCreate(px, 32, 32, 8, 128, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	CGColorSpaceRelease(space);
	if (context == NULL) {
		return false;
	}
	memset(px, 0, sizeof(px));
	/* (no interpolation: a sample is a pixel of the picture, not an average with its neighbours) */
	CGContextSetInterpolationQuality(context, kCGInterpolationNone);
	CGContextDrawImage(context, CGRectMake(0, 0, 32, 32), image);
	CGContextRelease(context);
	for (unsigned i = 0; i < 32 * 32; i++) {
		const uint8_t *q = px + 4 * i;

		magenta += q[3] >= 200 && q[0] >= 200 && q[2] >= 200 && q[1] <= 64 && abs((int)q[0] - (int)q[2]) <= 48;
	}
	*share = magenta;
	return magenta >= 1024 / 8;
}

/* the same of a renderer's answer, a 'BGRA' or 'RGhA' IOSurface (what PBUICALocalRenderer draws into: Metal's
 * BGRA8Unorm, or RGBA16Float for a wide colour space; another format is taken as it is) */
static bool
surface_failed(IOSurfaceRef surface, uint8_t rgba[4])
{
	const OSType fourcc = surface != NULL ? IOSurfaceGetPixelFormat(surface) : 0;
	const unsigned format = fourcc == 0x42475241 ? 80 : fourcc == 0x52476841 ? 115 : 0;
	bool failed = false;

	if (format == 0 || IOSurfaceGetPlaneCount(surface) > 1 || IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
		return false;
	}
	if (hm_picture_flat(format, IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface), IOSurfaceGetBaseAddress(surface),
	    IOSurfaceGetBytesPerRow(surface), rgba, NULL)) {
		failed = hm_rgba_magenta(rgba) || rgba[3] == 0;
	}
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	return failed;
}

/* the picture wallpaper_freeze=1 keeps (WALLPAPER_KEPT), as a CGImage; NULL when none is kept */
static CGImageRef
kept_wallpaper_image(void)
{
	struct wallpaper_kept head;
	int fd = open(WALLPAPER_KEPT, O_RDONLY);
	CGImageRef image = NULL;

	if (fd < 0) {
		return NULL;
	}
	if (read(fd, &head, sizeof(head)) == (ssize_t)sizeof(head) && head.magic == WALLPAPER_MAGIC && head.width != 0 &&
	    head.height != 0 && head.row >= head.width * 4 && head.height < 16384) {
		CFMutableDataRef data = CFDataCreateMutable(NULL, (CFIndex)head.row * head.height);

		if (data != NULL) {
			CFDataSetLength(data, (CFIndex)head.row * head.height);
			if (read(fd, CFDataGetMutableBytePtr(data), (size_t)head.row * head.height) == (ssize_t)((size_t)head.row * head.height)) {
				CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
				CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);

				image = CGImageCreate(head.width, head.height, 8, 32, head.row, space,
				    kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, NULL, false, kCGRenderingIntentDefault);
				CGDataProviderRelease(provider);
				CGColorSpaceRelease(space);
			}
			CFRelease(data);
		}
	}
	close(fd);
	return image;
}

/* a box blur of radius R along rows (STEP 4) or columns (STEP ROW), in place, every channel of BGRA */
static void
box_pass(uint8_t *pixels, size_t lines, size_t length, size_t line_step, size_t step, int r, uint8_t *scratch)
{
	const int n = 2 * r + 1;

	for (size_t l = 0; l < lines; l++) {
		uint8_t *p = pixels + l * line_step;

		for (size_t i = 0; i < length; i++) {
			memcpy(scratch + 4 * i, p + i * step, 4);
		}
		for (int c = 0; c < 4; c++) {
			int sum = 0;

			for (int k = -r; k <= r; k++) {
				const long at = k < 0 ? 0 : k >= (long)length ? (long)length - 1 : k;

				sum += scratch[4 * at + c];
			}
			for (size_t i = 0; i < length; i++) {
				const long out = (long)i - r, in = (long)i + r + 1;

				p[i * step + c] = (uint8_t)((sum + n / 2) / n);
				sum -= scratch[4 * (out < 0 ? 0 : out) + c];
				sum += scratch[4 * (in >= (long)length ? (long)length - 1 : in) + c];
			}
		}
	}
}

/* SOURCE drawn over W x H BGRA pixels at PIXELS (rows ROW bytes apart), filling them with the middle kept, then
 * blurred by three box blurs of RADIUS (0: not blurred) */
static bool
paint_picture(CGImageRef source, size_t w, size_t h, uint8_t *pixels, size_t row, int radius)
{
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef context = CGBitmapContextCreate(pixels, w, h, 8, row, space, kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
	uint8_t *scratch = radius > 0 ? malloc(4 * (w > h ? w : h)) : NULL;

	CGColorSpaceRelease(space);
	if (context == NULL || (radius > 0 && scratch == NULL)) {
		if (context != NULL) CGContextRelease(context);
		free(scratch);
		return false;
	}
	const double sw = (double)CGImageGetWidth(source), sh = (double)CGImageGetHeight(source);
	const double fill = fmax((double)w / sw, (double)h / sh);

	CGContextSetRGBFillColor(context, 0, 0, 0, 1);
	CGContextFillRect(context, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h));
	CGContextSetInterpolationQuality(context, kCGInterpolationMedium);
	CGContextDrawImage(context, CGRectMake(((double)w - sw * fill) / 2, ((double)h - sh * fill) / 2, sw * fill, sh * fill), source);
	CGContextRelease(context);
	for (int pass = 0; radius > 0 && pass < 3; pass++) {
		box_pass(pixels, h, w, row, 4, radius, scratch);              /* rows */
		box_pass(pixels, w, h, 4, row, radius, scratch);              /* columns */
	}
	free(scratch);
	return true;
}

/* the material's radius here: a fortieth of the picture's width (the materials blur by 20 to 60 points) */
static int
material_radius(size_t w)
{
	return (int)(w > 160 ? w / 40 : 4);
}

/* SOURCE as a new 'BGRA' IOSurface of W x H pixels, blurred as a material is */
static IOSurfaceRef
blurred_surface(CGImageRef source, size_t w, size_t h)
{
	const int32_t width = (int32_t)w, height = (int32_t)h, bpe = 4, format = 0x42475241, bpr = (int32_t)((w * 4 + 63) & ~(size_t)63);
	const void *keys[] = { CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"), CFSTR("IOSurfaceBytesPerElement"),
		CFSTR("IOSurfaceBytesPerRow"), CFSTR("IOSurfacePixelFormat") };
	const void *values[5];
	CFDictionaryRef properties;
	IOSurfaceRef surface;

	values[0] = CFNumberCreate(NULL, kCFNumberSInt32Type, &width);
	values[1] = CFNumberCreate(NULL, kCFNumberSInt32Type, &height);
	values[2] = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe);
	values[3] = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpr);
	values[4] = CFNumberCreate(NULL, kCFNumberSInt32Type, &format);
	properties = CFDictionaryCreate(NULL, keys, values, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	for (int i = 0; i < 5; i++) {
		CFRelease(values[i]);
	}
	surface = properties != NULL ? IOSurfaceCreate(properties) : NULL;
	if (properties != NULL) {
		CFRelease(properties);
	}
	if (surface == NULL || IOSurfaceLock(surface, 0, NULL) != kIOReturnSuccess) {
		if (surface != NULL) CFRelease(surface);
		return NULL;
	}
	const bool painted = paint_picture(source, w, h, IOSurfaceGetBaseAddress(surface), IOSurfaceGetBytesPerRow(surface), material_radius(w));

	IOSurfaceUnlock(surface, 0, NULL);
	if (!painted) {
		CFRelease(surface);
		return NULL;
	}
	return surface;
}

/* SOURCE as a new CGImage of W x H pixels, not blurred (a snapshot of the wallpaper itself) */
static CGImageRef
fitted_image(CGImageRef source, size_t w, size_t h)
{
	const size_t row = w * 4;
	uint8_t *pixels = malloc(row * h);
	CGImageRef image = NULL;

	if (pixels != NULL && paint_picture(source, w, h, pixels, row, 0)) {
		CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
		CGContextRef context = CGBitmapContextCreate(pixels, w, h, 8, row, space, kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);

		CGColorSpaceRelease(space);
		if (context != NULL) {
			image = CGBitmapContextCreateImage(context);   /* (a copy: the pixels are freed below) */
			CGContextRelease(context);
		}
	}
	free(pixels);
	return image;
}

/*
 * The wallpaper as one picture, made here on the processor, for whatever has no real picture of it: an empty or purple
 * snapshot, or a treatment of one. The picture wallpaper_freeze=1 keeps, if there is one; else the system's default
 * wallpaper for this screen (/Library/Wallpaper/DefaultWallpapers~iphone.plist: 7400, WWDC 2022, in
 * Collections/iOS_16~iphone.wallpaperCollection), whose .ca packages -- background, foreground, floating -- are laid
 * one over the other at the screen's pixels (draw_layer). That is the poster this phone shows
 * unless somebody chose another (CollectionsPoster draws the same packages); filters and animation are not applied.
 * Made once and kept. NULL when neither can be had.
 */
typedef struct { double x, y, width, height; } hk_frame;

static id
plist_object(id dictionary, const char *key)
{
	if (dictionary == NULL) {
		return NULL;
	}
	id k = SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, key);

	return SEND1(id, dictionary, "objectForKey:", id, k);
}

/* the default wallpaper's directory for SCREEN_CLASS ("375w-812h@3x~iphone"), into OUT */
static bool
default_wallpaper_directory(const char *screen_class, char *out, size_t size)
{
	id plist = SEND1(id, objc_getClass("NSDictionary"), "dictionaryWithContentsOfFile:", id,
	    (id)CFSTR("/Library/Wallpaper/DefaultWallpapers~iphone.plist"));
	id identifier = plist_object(plist_object(plist, "default"), "wallpaperIdentifier");
	const long wanted = identifier != NULL ? SEND(long, identifier, "longValue") : 7400;
	char prefix[32], suffix[96];
	DIR *collections = opendir("/Library/Wallpaper/Collections");
	struct dirent *c;
	bool found = false;

	snprintf(prefix, sizeof(prefix), "%ld.", wanted);
	snprintf(suffix, sizeof(suffix), "-%s.wallpaper", screen_class);
	while (!found && collections != NULL && (c = readdir(collections)) != NULL) {
		char wallpapers[512];
		DIR *d;
		struct dirent *w;

		if (c->d_name[0] == '.' || snprintf(wallpapers, sizeof(wallpapers), "/Library/Wallpaper/Collections/%s/Wallpapers", c->d_name) >=
		    (int)sizeof(wallpapers) || (d = opendir(wallpapers)) == NULL) {
			continue;
		}
		while (!found && (w = readdir(d)) != NULL) {
			const size_t n = strlen(w->d_name), m = strlen(suffix);

			if (strncmp(w->d_name, prefix, strlen(prefix)) == 0 && n > m && strcmp(w->d_name + n - m, suffix) == 0) {
				found = snprintf(out, size, "%s/%s", wallpapers, w->d_name) < (int)size;
			}
		}
		closedir(d);
	}
	if (collections != NULL) closedir(collections);
	return found;
}

static int
package_rank(const char *name)
{
	return strstr(name, "Background") != NULL ? 0 : strstr(name, "Foreground") != NULL ? 1 : strstr(name, "Floating") != NULL ? 2 : 3;
}

/*
 * A CALayer tree drawn on the processor, by hand: -[CALayer renderInContext:] leaves out compositing filters, and the
 * wallpaper's head is drawn sourceAtop its mask (Background.ca: "Head.HEIC" inside "Head Mask.HEIC"), which it then drew
 * as a plain rectangle. What these packages use is drawn: geometry (position, anchor, bounds, an affine transform and
 * sublayer transform, geometryFlipped), opacity, hidden, background colours, CGImage contents, linear gradients
 * (CAGradientLayer) and the sourceAtop compositing filter. Filters and animations are not applied. Checked on the Mac
 * against the 20F66 rootfs's packages: the picture is the iOS 16 wallpaper.
 */
typedef struct { double m[16]; } hk_transform;   /* CATransform3D: m11 m12 m13 m14 m21 ... m44 */
typedef struct { double x, y; } hk_point;

static bool
transform_affine(const hk_transform *t, CGAffineTransform *out)
{
	const double *m = t->m;

	if (m[2] != 0 || m[3] != 0 || m[6] != 0 || m[7] != 0 || m[8] != 0 || m[9] != 0 || m[10] != 1 || m[11] != 0 || m[14] != 0 || m[15] != 1) {
		return false;
	}
	*out = CGAffineTransformMake(m[0], m[1], m[4], m[5], m[12], m[13]);
	return true;
}

static bool
transform_identity(const hk_transform *t)
{
	CGAffineTransform a;

	return transform_affine(t, &a) && CGAffineTransformIsIdentity(a);
}

/* what draw_layer met: contents drawn, and contents of a kind it does not draw (said with the picture) */
static unsigned layers_drawn, layers_skipped;

/* an IOSurface as contents (CoreAnimation decodes a package's HEIC images into surfaces: 'BGRA', or '420f' and '420v'
 * as the poster's own were): a CGImage of it, opaque; NULL for another format */
static CGImageRef
surface_image(IOSurfaceRef surface)
{
	const OSType fourcc = IOSurfaceGetPixelFormat(surface);
	const size_t w = IOSurfaceGetWidth(surface), h = IOSurfaceGetHeight(surface);
	CGImageRef image = NULL;

	if (w == 0 || h == 0 || w > 8192 || h > 8192 || IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
		return NULL;
	}
	uint8_t *pixels = malloc(w * h * 4);

	if (pixels != NULL && fourcc == 0x42475241 && IOSurfaceGetPlaneCount(surface) <= 1) {          /* 'BGRA' */
		const uint8_t *from = IOSurfaceGetBaseAddress(surface);

		for (size_t j = 0; j < h; j++) {
			memcpy(pixels + j * w * 4, from + j * IOSurfaceGetBytesPerRow(surface), w * 4);
		}
	} else if (pixels != NULL && (fourcc == 0x34323066 || fourcc == 0x34323076) && IOSurfaceGetPlaneCount(surface) == 2) {   /* '420f', '420v' */
		const hm_ycc_planes planes = { IOSurfaceGetBaseAddressOfPlane(surface, 0), IOSurfaceGetBytesPerRowOfPlane(surface, 0),
			IOSurfaceGetBaseAddressOfPlane(surface, 1), IOSurfaceGetBytesPerRowOfPlane(surface, 1),
			IOSurfaceGetWidthOfPlane(surface, 1), IOSurfaceGetHeightOfPlane(surface, 1) };

		hm_ycc_convert(&planes, w, h, HM_YCC_601, fourcc == 0x34323066, 0, pixels, w * 4);
		for (size_t i = 0; i < w * h; i++) {                       /* RGBA -> BGRA */
			const uint8_t r = pixels[4 * i];

			pixels[4 * i] = pixels[4 * i + 2];
			pixels[4 * i + 2] = r;
		}
	} else {
		free(pixels);
		pixels = NULL;
	}
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	if (pixels != NULL) {
		CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
		CGContextRef context = CGBitmapContextCreate(pixels, w, h, 8, w * 4, space, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);

		CGColorSpaceRelease(space);
		if (context != NULL) {
			image = CGBitmapContextCreateImage(context);
			CGContextRelease(context);
		}
		free(pixels);
	}
	return image;
}

/* LAYER drawn into CONTEXT, whose space is its superlayer's (top-left origin); ROOT: the package's root, whose
 * geometryFlipped is the authoring tool's (the Mac's bottom-left origin made top-left), not a flip here */
static void
draw_layer(id layer, CGContextRef context, bool root, unsigned depth)
{
	const float opacity = SEND(float, layer, "opacity");

	if (depth > 32 || SEND(BOOL, layer, "isHidden") || opacity <= 0) {
		return;
	}
	const hk_frame b = ((hk_frame (*)(id, SEL))objc_msgSend)(layer, sel_registerName("bounds"));
	const hk_point anchor = ((hk_point (*)(id, SEL))objc_msgSend)(layer, sel_registerName("anchorPoint"));
	const hk_point position = ((hk_point (*)(id, SEL))objc_msgSend)(layer, sel_registerName("position"));
	const hk_transform transform = ((hk_transform (*)(id, SEL))objc_msgSend)(layer, sel_registerName("transform"));
	const hk_transform sublayer_transform = ((hk_transform (*)(id, SEL))objc_msgSend)(layer, sel_registerName("sublayerTransform"));
	CGAffineTransform affine;

	CGContextSaveGState(context);
	CGContextTranslateCTM(context, (CGFloat)position.x, (CGFloat)position.y);
	if (transform_affine(&transform, &affine)) {
		CGContextConcatCTM(context, affine);
	}
	CGContextTranslateCTM(context, -(CGFloat)(b.x + anchor.x * b.width), -(CGFloat)(b.y + anchor.y * b.height));
	CGContextSetAlpha(context, opacity);
	CGContextBeginTransparencyLayer(context, NULL);
	CGColorRef background = SEND(CGColorRef, layer, "backgroundColor");
	if (background != NULL) {
		CGContextSetFillColorWithColor(context, background);
		CGContextFillRect(context, CGRectMake(b.x, b.y, b.width, b.height));
	}
	if (SEND1(BOOL, layer, "isKindOfClass:", id, (id)objc_getClass("CAGradientLayer"))) {
		id colors = SEND(id, layer, "colors"), locations = SEND(id, layer, "locations");
		const unsigned long n = colors != NULL ? SEND(unsigned long, colors, "count") : 0;
		CGFloat stops[16];

		if (n >= 2 && n <= 16) {
			for (unsigned long i = 0; i < n; i++) {
				id l = locations != NULL && SEND(unsigned long, locations, "count") == n ?
				    ((id (*)(id, SEL, unsigned long))objc_msgSend)(locations, sel_registerName("objectAtIndex:"), i) : NULL;

				stops[i] = l != NULL ? (CGFloat)SEND(double, l, "doubleValue") : (CGFloat)i / (CGFloat)(n - 1);
			}
			CGGradientRef gradient = CGGradientCreateWithColors(NULL, (CFArrayRef)colors, stops);
			const hk_point s0 = ((hk_point (*)(id, SEL))objc_msgSend)(layer, sel_registerName("startPoint"));
			const hk_point s1 = ((hk_point (*)(id, SEL))objc_msgSend)(layer, sel_registerName("endPoint"));

			if (gradient != NULL) {
				CGContextSaveGState(context);
				CGContextClipToRect(context, CGRectMake(b.x, b.y, b.width, b.height));
				CGContextDrawLinearGradient(context, gradient, CGPointMake(b.x + s0.x * b.width, b.y + s0.y * b.height),
				    CGPointMake(b.x + s1.x * b.width, b.y + s1.y * b.height), kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
				CGContextRestoreGState(context);
				CGGradientRelease(gradient);
			}
		}
	}
	id contents = SEND(id, layer, "contents");
	CGImageRef image = NULL, made = NULL;
	if (contents != NULL && CFGetTypeID((CFTypeRef)contents) == CGImageGetTypeID()) {
		image = (CGImageRef)contents;
	} else if (contents != NULL && CFGetTypeID((CFTypeRef)contents) == IOSurfaceGetTypeID()) {
		image = made = surface_image((IOSurfaceRef)contents);
	}
	if (image != NULL) {
		CGContextSaveGState(context);
		CGContextTranslateCTM(context, 0, (CGFloat)(2 * b.y + b.height));   /* (an image is drawn upright in a bottom-left space) */
		CGContextScaleCTM(context, 1, -1);
		CGContextDrawImage(context, CGRectMake(b.x, b.y, b.width, b.height), image);
		CGContextRestoreGState(context);
		layers_drawn++;
	} else if (contents != NULL) {
		layers_skipped++;
	}
	if (made != NULL) CGImageRelease(made);
	CGContextSaveGState(context);
	if ((bool)SEND(BOOL, layer, "isGeometryFlipped") != root) {
		CGContextTranslateCTM(context, 0, (CGFloat)(2 * b.y + b.height));
		CGContextScaleCTM(context, 1, -1);
	}
	if (!transform_identity(&sublayer_transform) && transform_affine(&sublayer_transform, &affine)) {
		const CGFloat cx = (CGFloat)(b.x + anchor.x * b.width), cy = (CGFloat)(b.y + anchor.y * b.height);

		CGContextTranslateCTM(context, cx, cy);
		CGContextConcatCTM(context, affine);
		CGContextTranslateCTM(context, -cx, -cy);
	}
	id sublayers = SEND(id, layer, "sublayers");
	const unsigned long count = sublayers != NULL ? SEND(unsigned long, sublayers, "count") : 0;

	for (unsigned long i = 0; i < count; i++) {
		id sublayer = ((id (*)(id, SEL, unsigned long))objc_msgSend)(sublayers, sel_registerName("objectAtIndex:"), i);
		id filter = SEND(id, sublayer, "compositingFilter");
		const char *name = NULL;

		if (filter != NULL) {
			id text = SEND1(BOOL, filter, "respondsToSelector:", SEL, sel_registerName("type")) ? SEND(id, filter, "type") : SEND(id, filter, "description");
			name = text != NULL ? SEND(const char *, text, "UTF8String") : NULL;
		}
		CGContextSetBlendMode(context, name != NULL && strstr(name, "sourceAtop") != NULL ? kCGBlendModeSourceAtop : kCGBlendModeNormal);
		draw_layer(sublayer, context, false, depth + 1);
	}
	CGContextRestoreGState(context);
	CGContextEndTransparencyLayer(context);
	CGContextRestoreGState(context);
}

static CGImageRef
default_wallpaper_image(size_t w, size_t h, double points_w, double points_h, double scale)
{
	char screen_class[64], directory[768];
	CFStringRef *type = dlsym(RTLD_DEFAULT, "kCAPackageTypeCAMLBundle");
	char packages[4][256];
	unsigned count = 0;
	DIR *d;
	struct dirent *e;

	snprintf(screen_class, sizeof(screen_class), "%.0fw-%.0fh@%.0fx~iphone", points_w, points_h, scale);
	if (type == NULL || !default_wallpaper_directory(screen_class, directory, sizeof(directory)) || (d = opendir(directory)) == NULL) {
		say("hakios: SpringBoard: no default wallpaper for %s to draw\n", screen_class);
		return NULL;
	}
	while ((e = readdir(d)) != NULL && count < 4) {
		const size_t n = strlen(e->d_name);

		if (n > 3 && strcmp(e->d_name + n - 3, ".ca") == 0) {
			strlcpy(packages[count++], e->d_name, sizeof(packages[0]));
		}
	}
	closedir(d);
	for (unsigned i = 0; i < count; i++) {               /* background first, floating last */
		for (unsigned j = i + 1; j < count; j++) {
			if (package_rank(packages[j]) < package_rank(packages[i])) {
				char t[256];

				memcpy(t, packages[i], sizeof(t));
				memcpy(packages[i], packages[j], sizeof(t));
				memcpy(packages[j], t, sizeof(t));
			}
		}
	}
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef context = CGBitmapContextCreate(NULL, w, h, 8, 0, space, kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
	CGImageRef image = NULL;
	unsigned drawn = 0;

	CGColorSpaceRelease(space);
	if (context == NULL) {
		return NULL;
	}
	CGContextSetRGBFillColor(context, 0, 0, 0, 1);
	CGContextFillRect(context, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h));
	/* (UIKit's way up: the origin at the top, a point of the layers SCALE pixels) */
	CGContextTranslateCTM(context, 0, (CGFloat)h);
	CGContextScaleCTM(context, (CGFloat)w / points_w, -(CGFloat)h / points_h);
	for (unsigned i = 0; i < count; i++) {
		char path[1100];
		id error = NULL;

		snprintf(path, sizeof(path), "%s/%s", directory, packages[i]);
		id url = SEND1(id, objc_getClass("NSURL"), "fileURLWithPath:", id,
		    SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, path));
		id package = ((id (*)(id, SEL, id, id, id, id *))objc_msgSend)((id)objc_getClass("CAPackage"),
		    sel_registerName("packageWithContentsOfURL:type:options:error:"), url, (id)*type, NULL, &error);
		id root = package != NULL ? SEND(id, package, "rootLayer") : NULL;

		if (root == NULL) {
			say("hakios: SpringBoard: the default wallpaper's %s cannot be read: %s\n", packages[i], text_of(error));
			continue;
		}
		const hk_frame bounds = ((hk_frame (*)(id, SEL))objc_msgSend)(root, sel_registerName("bounds"));
		const hk_point anchor = ((hk_point (*)(id, SEL))objc_msgSend)(root, sel_registerName("anchorPoint"));
		const hk_point position = ((hk_point (*)(id, SEL))objc_msgSend)(root, sel_registerName("position"));

		CGContextSaveGState(context);
		if (bounds.width > 0 && bounds.height > 0) {
			CGContextScaleCTM(context, points_w / bounds.width, points_h / bounds.height);
		}
		/* (the root's frame at the screen's origin, wherever the package put it) */
		CGContextTranslateCTM(context, (CGFloat)(bounds.width * anchor.x - position.x), (CGFloat)(bounds.height * anchor.y - position.y));
		draw_layer(root, context, true, 0);
		CGContextRestoreGState(context);
		drawn++;
	}
	if (drawn != 0) {
		image = CGBitmapContextCreateImage(context);
	}
	CGContextRelease(context);
	say("hakios: SpringBoard: the default wallpaper (%s) drawn here from %u of %u packages, %zux%zu: %u pictures drawn, %u of a kind "
	    "not drawn\n", directory, drawn, count, w, h, layers_drawn, layers_skipped);
	return image;
}

/* the wallpaper's picture for a stand-in (retained: the caller releases it), and where it came from */
static CGImageRef
wallpaper_fallback_image(const char **from)
{
	static CGImageRef made;
	static const char *made_from;
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		id screen = SEND(id, objc_getClass("UIScreen"), "mainScreen");
		const hk_frame bounds = screen != NULL ? ((hk_frame (*)(id, SEL))objc_msgSend)(screen, sel_registerName("bounds")) : (hk_frame){ 0, 0, 375, 812 };
		const double scale = screen != NULL ? ((double (*)(id, SEL))objc_msgSend)(screen, sel_registerName("scale")) : 3.0;
		uint8_t rgba[4];

		made = kept_wallpaper_image();
		made_from = "the picture wallpaper_freeze=1 keeps";
		if (made == NULL && bounds.width > 0 && bounds.height > 0) {
			made = default_wallpaper_image((size_t)(bounds.width * scale + 0.5), (size_t)(bounds.height * scale + 0.5), bounds.width,
			    bounds.height, scale);
			made_from = "the default wallpaper drawn here";
			if (made != NULL && picture_flat(made, rgba)) {
				say("hakios: SpringBoard: the default wallpaper drawn here is one colour (%u, %u, %u): not used\n", rgba[0], rgba[1], rgba[2]);
				CGImageRelease(made);
				made = NULL;
			}
		}
	});
	*from = made_from;
	return made != NULL ? CGImageRetain(made) : NULL;
}

typedef struct { double width, height; } hk_size;

static id (*original_remote_render)(id self, SEL selector, id request, id *error);
static id (*original_local_render)(id self, SEL selector, id request, id *error);

static id
treated_or_made_here(id request, id result, id *error, const char *renderer)
{
	static unsigned told, asked;
	uint8_t rgba[4] = { 0, 0, 0, 0 };
	const bool nothing = result == NULL;

	if (asked++ < 4) {
		say("hakios: SpringBoard: %s asked of %s: %s %s\n", request != NULL ? object_getClassName(request) : "nothing", renderer,
		    nothing ? "no answer" : "an answer", nothing ? "" : object_getClassName(result));
	}
	if (request == NULL || strcmp(object_getClassName(request), "PBUITreatImageRequest") != 0 ||
	    (!nothing && (CFGetTypeID((CFTypeRef)result) != IOSurfaceGetTypeID() || !surface_failed((IOSurfaceRef)result, rgba)))) {
		return result;
	}
	const hk_size size = ((hk_size (*)(id, SEL))objc_msgSend)(request, sel_registerName("requestedOutputSizeInPixels"));
	CGImageRef source = SEND(CGImageRef, request, "image");
	CGImageRef kept = NULL;
	uint8_t source_rgba[4];
	const char *from = "its own picture";

	if (source == NULL || picture_failed(source, source_rgba)) {
		source = kept = wallpaper_fallback_image(&from);
	}
	if (source == NULL || size.width < 1 || size.height < 1 || size.width > 8192 || size.height > 8192) {
		if (told++ < 8) {
			say("hakios: SpringBoard: the wallpaper's material picture from %s is %s, and there is nothing here to make it from\n",
			    renderer, nothing ? "nothing" : "flat purple");
		}
		return result;
	}
	IOSurfaceRef made = blurred_surface(source, (size_t)size.width, (size_t)size.height);

	if (kept != NULL) CGImageRelease(kept);
	if (made == NULL) {
		return result;
	}
	if (told++ < 8) {
		say("hakios: SpringBoard: the wallpaper's material picture from %s was %s (%u, %u, %u, %u); made here, %gx%g, "
		    "blurred from %s\n", renderer, nothing ? "nothing" : "flat purple or clear", rgba[0], rgba[1], rgba[2], rgba[3],
		    size.width, size.height, from);
	}
	if (error != NULL) {
		*error = NULL;
	}
	return (id)CFAutorelease(made);
}

static id
remote_render(id self, SEL selector, id request, id *error)
{
	return treated_or_made_here(request, original_remote_render(self, selector, request, error), error, "SBRendererService");
}

static id
local_render(id self, SEL selector, id request, id *error)
{
	return treated_or_made_here(request, original_local_render(self, selector, request, error), error, "SpringBoard's own renderer");
}

static id
snapshot_error(const char *what)
{
	id text = SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, what);
	id info = ((id (*)(id, SEL, id, id))objc_msgSend)((id)objc_getClass("NSDictionary"), sel_registerName("dictionaryWithObject:forKey:"),
	    text, (id)CFSTR("NSLocalizedDescription"));

	return ((id (*)(id, SEL, id, long, id))objc_msgSend)((id)objc_getClass("NSError"), sel_registerName("errorWithDomain:code:userInfo:"),
	    (id)CFSTR("com.hakios.paperboard"), 1L, info);
}

static void
remove_url(id url)
{
	const char *path = url != NULL ? SEND(const char *, SEND(id, url, "path"), "fileSystemRepresentation") : NULL;

	if (path != NULL && unlink(path) != 0 && errno != ENOENT) {
		say("hakios: SpringBoard: %s cannot be removed: %s\n", path, strerror(errno));
	}
}

static id (*original_snapshot_read)(id self, SEL selector, id *error);

static id
snapshot_read(id self, SEL selector, id *error)
{
	static unsigned told;
	static unsigned reads;
	id image = original_snapshot_read(self, selector, error);
	uint8_t rgba[4] = { 0, 0, 0, 0 };
	unsigned share = 0;
	const bool bad = image != NULL && snapshot_bad(SEND(CGImageRef, image, "CGImage"), rgba, &share);

	if (reads++ < 6) {
		say("hakios: SpringBoard: a wallpaper snapshot read from %s: %s, %u/1024 of it magenta\n", text_of(SEND(id, self, "url")),
		    image != NULL ? "a picture" : "none", share);
	}
	if (!bad) {
		return image;
	}
	if (told++ < 8) {
		say("hakios: SpringBoard: a cached wallpaper snapshot (%s) is purple (%u/1024 magenta; first pixel %u, %u, %u, %u): thrown away, "
		    "a new one asked for\n", text_of(SEND(id, self, "url")), share, rgba[0], rgba[1], rgba[2], rgba[3]);
	}
	remove_url(SEND(id, self, "url"));
	remove_url(SEND(id, self, "metadataURL"));
	if (error != NULL) {
		*error = snapshot_error("hakios: a flat purple snapshot, made where the renderer had no GPU");
	}
	return NULL;
}

static BOOL (*original_snapshot_update)(id self, SEL selector, id image, id statistics, id *error);

static BOOL
snapshot_update(id self, SEL selector, id image, id statistics, id *error)
{
	static unsigned told, updates;
	uint8_t rgba[4] = { 0, 0, 0, 0 };
	unsigned share = 0;
	CGImageRef cg = image != NULL ? SEND(CGImageRef, image, "CGImage") : NULL;
	/* (a scene's snapshot is a UIImage over the IOSurface the window server drew into, with no CGImage: -[UIImage
	 * ioSurface], which -[PBUIImageEncoder saveUIImage:error:] takes when there is no CGImage) */
	IOSurfaceRef surface = cg == NULL && image != NULL && SEND1(BOOL, image, "respondsToSelector:", SEL, sel_registerName("ioSurface")) ?
	    (IOSurfaceRef)SEND(id, image, "ioSurface") : NULL;
	/* (an empty snapshot -- no picture, or one of no pixels -- is no better) */
	const bool empty = (cg == NULL || CGImageGetWidth(cg) == 0 || CGImageGetHeight(cg) == 0) &&
	    (surface == NULL || IOSurfaceGetWidth(surface) == 0 || IOSurfaceGetHeight(surface) == 0);
	const bool bad = empty || (cg != NULL ? snapshot_bad(cg, rgba, &share) : surface_failed(surface, rgba));

	if (updates++ < 6) {
		const OSType fourcc = surface != NULL ? IOSurfaceGetPixelFormat(surface) : 0;

		say("hakios: SpringBoard: a new wallpaper snapshot for %s: %zux%zu%s%c%c%c%c, %u/1024 of it magenta%s\n", text_of(SEND(id, self, "url")),
		    cg != NULL ? CGImageGetWidth(cg) : surface != NULL ? IOSurfaceGetWidth(surface) : 0,
		    cg != NULL ? CGImageGetHeight(cg) : surface != NULL ? IOSurfaceGetHeight(surface) : 0, surface != NULL ? " IOSurface " : "",
		    surface ? (char)(fourcc >> 24) : ' ', surface ? (char)(fourcc >> 16) : ' ', surface ? (char)(fourcc >> 8) : ' ', surface ? (char)fourcc : ' ',
		    share, empty ? " (empty)" : bad ? " (purple or clear)" : "");
	}
	if (bad) {
		const char *from = "nothing";
		CGImageRef kept = wallpaper_fallback_image(&from);
		id screen = SEND(id, objc_getClass("UIScreen"), "mainScreen");
		const hk_frame screen_bounds = screen != NULL ? ((hk_frame (*)(id, SEL))objc_msgSend)(screen, sel_registerName("bounds")) : (hk_frame){ 0, 0, 0, 0 };
		const double screen_scale = screen != NULL ? ((double (*)(id, SEL))objc_msgSend)(screen, sel_registerName("scale")) : 3.0;
		const hk_size size = empty || image == NULL ? (hk_size){ screen_bounds.width, screen_bounds.height } :
		    ((hk_size (*)(id, SEL))objc_msgSend)(image, sel_registerName("size"));
		const double scale = empty || image == NULL ? screen_scale : ((double (*)(id, SEL))objc_msgSend)(image, sel_registerName("scale"));
		const size_t w = (size_t)(size.width * scale + 0.5), h = (size_t)(size.height * scale + 0.5);
		CGImageRef made = kept != NULL && w > 0 && h > 0 && w <= 8192 && h <= 8192 ? fitted_image(kept, w, h) : NULL;
		id replacement = NULL;

		if (made != NULL) {
			replacement = ((id (*)(id, SEL, CGImageRef, double, long))objc_msgSend)((id)objc_getClass("UIImage"),
			    sel_registerName("imageWithCGImage:scale:orientation:"), made, scale, 0L);
			CGImageRelease(made);
		}
		if (kept != NULL) CGImageRelease(kept);
		if (told++ < 8) {
			say("hakios: SpringBoard: a new wallpaper snapshot is %s (%u/1024 magenta; %u, %u, %u, %u): %s%s\n", empty ? "empty" : "purple or clear",
			    share, rgba[0], rgba[1], rgba[2], rgba[3], replacement != NULL ? "stored instead: " : "", replacement != NULL ? from :
			    empty ? "not stored (nothing here to stand in for it)" : "stored as it is (nothing here to stand in for it)");
		}
		if (replacement != NULL) {
			image = replacement;
		} else if (empty) {
			if (error != NULL) {
				*error = snapshot_error("hakios: an empty snapshot, and no picture of the wallpaper to stand in for it");
			}
			return NO;
		}
	}
	return original_snapshot_update(self, selector, image, statistics, error);
}

/* -[PBUIPosterVariantViewController evaluateSnapshotPreconditions]: when a snapshot asked for may be taken (the poster's
 * scene with content, settings, out of an extended render session, suspended); said when its answer changes, the
 * first 16 times, so that a snapshot thrown away above and not made again says why */
static BOOL (*original_snapshot_preconditions)(id self, SEL selector);

static BOOL
snapshot_preconditions(id self, SEL selector)
{
	static unsigned told;
	static int last = -1;
	const BOOL ok = original_snapshot_preconditions(self, selector);

	if ((int)ok != last && told++ < 16) {
		say("hakios: SpringBoard: the poster's snapshot preconditions (%p): %s\n", (void *)self, ok ? "met, a snapshot may be taken" :
		    "not met (no content, mid-update, render session or not suspended)");
	}
	last = ok;
	return ok;
}

/* -[_FBSSnapshot capture] (FrontBoardServices): the scene's snapshot, which the window server draws into an IOSurface
 * (CARenderServer). Said the first 8 times: whether it worked, at what size, into what surface -- an empty snapshot of
 * the poster (10 November: 0x0) is either no capture or a surface nothing was drawn into */
static BOOL (*original_snapshot_capture)(id self, SEL selector);

static BOOL
snapshot_capture(id self, SEL selector)
{
	static unsigned told;
	const BOOL ok = original_snapshot_capture(self, selector);

	if (told++ < 8) {
		id context = SEND(id, self, "context");
		const hk_size size = context != NULL ? ((hk_size (*)(id, SEL))objc_msgSend)(context, sel_registerName("snapshotSize")) : (hk_size){ 0, 0 };
		id layers = context != NULL ? SEND(id, context, "layers") : NULL;
		IOSurfaceRef surface = (IOSurfaceRef)SEND(id, self, "IOSurface");
		CGImageRef image = SEND(CGImageRef, self, "CGImage");
		const OSType fourcc = surface != NULL ? IOSurfaceGetPixelFormat(surface) : 0;

		say("hakios: SpringBoard: a scene snapshot (%gx%g, %lu layers, %s): %s; surface %s%c%c%c%c %zux%zu, image %zux%zu\n", size.width,
		    size.height, layers != NULL ? SEND(unsigned long, layers, "count") : 0UL,
		    context != NULL && SEND(BOOL, context, "isOpaque") ? "opaque" : "not opaque", ok ? "captured" : "NOT captured",
		    surface != NULL ? "'" : "none ", surface ? (char)(fourcc >> 24) : ' ', surface ? (char)(fourcc >> 16) : ' ',
		    surface ? (char)(fourcc >> 8) : ' ', surface ? (char)fourcc : ' ', surface ? IOSurfaceGetWidth(surface) : 0,
		    surface ? IOSurfaceGetHeight(surface) : 0, image ? CGImageGetWidth(image) : 0, image ? CGImageGetHeight(image) : 0);
	}
	return ok;
}

/*
 * Once (a marker file says it was done): every RuntimeSnapshot of a poster in PosterBoard's extension data store
 * removed, so that each is taken again under Metal. They are PaperBoardUI's snapshots of the lock and home screens
 * (RuntimeSnapshot-lock.atx, -home.atx and their metadata) under
 * /var/mobile/Library/Application Support/PRBPosterExtensionDataStore/<n>/Extensions/<poster>/configurations/<id>/versions/<v>,
 * and the persistent /var kept the ones the GL renderer of earlier boots made -- magenta where it could not read a
 * texture. A snapshot that is missing is taken again (-snapshotSource:failedToReadSnapshotAtURL:error: asks for it).
 */
#define POSTER_STORE "/var/mobile/Library/Application Support/PRBPosterExtensionDataStore"
#define POSTER_PURGED "/var/mobile/Library/hakios-poster-snapshots-purged-1"

static unsigned
purge_snapshots_in(const char *directory, unsigned depth)
{
	DIR *d = depth > 12 ? NULL : opendir(directory);
	struct dirent *e;
	unsigned removed = 0;

	if (d == NULL) {
		return 0;
	}
	while ((e = readdir(d)) != NULL) {
		char path[1024];

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
		    snprintf(path, sizeof(path), "%s/%s", directory, e->d_name) >= (int)sizeof(path)) {
			continue;
		}
		if (e->d_type == DT_DIR) {
			removed += purge_snapshots_in(path, depth + 1);
		} else if (strncmp(e->d_name, "RuntimeSnapshot", 15) == 0) {
			if (unlink(path) == 0) {
				removed++;
			} else {
				say("hakios: SpringBoard: %s cannot be removed: %s\n", path, strerror(errno));
			}
		}
	}
	closedir(d);
	return removed;
}

static void
purge_poster_snapshots(void)
{
	struct stat st;
	int fd;

	if (stat(POSTER_PURGED, &st) == 0 || stat(POSTER_STORE, &st) != 0) {
		return;
	}
	const unsigned removed = purge_snapshots_in(POSTER_STORE, 0);

	say("hakios: SpringBoard: the posters' snapshots of earlier boots removed (%u files), to be taken again (once: %s)\n", removed,
	    POSTER_PURGED);
	if ((fd = open(POSTER_PURGED, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		close(fd);
	}
}

/*
 * PaperBoardUI keeps its pictures of the wallpaper on disk as ATX (+[PBUIImageOnDiskFormat defaultATX]: ASTC 4x4,
 * twiddled), compressed and read back on the processor. Every such file read back here was the ASTC error colour,
 * (255, 0, 255) all over -- RuntimeSnapshot-home.atx on 10 November, RuntimeSnapshot-lock.atx written by the run
 * before -- whatever went into it; the ASTC compressor or its reader does not work in this SpringBoard (no GPU, the
 * twiddled layout). PNG is PaperBoardUI's own other format (+png): the same pictures, kept as PNG.
 * PosterBoard keeps its own snapshots of the posters in the same format (PBFPosterSnapshotManager's onDiskFormat,
 * +defaultATX), and Settings' home-screen preview is drawn from one: its ASTC 4x4 picture of 1125x2436
 * ("t3=1125x2436 fmt 204" in PosterBoard's trace) came out in black blocks. So PosterBoard is given this too.
 */
static id (*original_format_png)(id self, SEL selector);

static id
format_png_instead(id self, SEL selector)
{
	static unsigned told;

	if (told++ == 0) {
		say("hakios: %s: PaperBoardUI's pictures of the wallpaper kept as PNG instead of ATX (+[PBUIImageOnDiskFormat %s])\n",
		    getprogname(), sel_getName(selector));
	}
	return original_format_png != NULL ? original_format_png(self, sel_registerName("png")) : NULL;
}

static id
format_png_instead_of_atx(id self, SEL selector, unsigned block_size, BOOL twiddling)
{
	(void)block_size;
	(void)twiddling;
	return format_png_instead(self, selector);
}

static void
pictures_as_png(void)
{
	Class format = objc_getClass("PBUIImageOnDiskFormat");
	Method png = format != NULL ? class_getClassMethod(format, sel_registerName("png")) : NULL;

	original_format_png = png != NULL && (uintptr_t)method_getImplementation(png) == 0x1b0cc5250UL + slide ?
	    (id (*)(id, SEL))method_getImplementation(png) : NULL;
	if (original_format_png != NULL) {
		IMP atx = replace_method_of_kind('+', "PBUIImageOnDiskFormat", "defaultATX", 0x1b0cc5244UL, (IMP)format_png_instead);
		IMP atx_sized = replace_method_of_kind('+', "PBUIImageOnDiskFormat", "atxWithBlockSize:twiddling:", 0x1b0cc510cUL,
		    (IMP)format_png_instead_of_atx);

		say("hakios: %s: PaperBoardUI's ATX formats %s/%s\n", getprogname(), atx ? "made PNG" : "NOT changed",
		    atx_sized ? "made PNG" : "NOT changed");
	} else {
		say("hakios: %s: +[PBUIImageOnDiskFormat png] not where it was expected: its pictures stay ATX\n", getprogname());
	}
}

static void
material_pictures(void)
{
	pictures_as_png();
	purge_poster_snapshots();
	original_snapshot_preconditions = (BOOL (*)(id, SEL))replace_method("PBUIPosterVariantViewController", "evaluateSnapshotPreconditions",
	    0x1b0cef9f4UL, (IMP)snapshot_preconditions);
	original_snapshot_capture = (BOOL (*)(id, SEL))replace_method("_FBSSnapshot", "capture", 0x19b93aa48UL, (IMP)snapshot_capture);
	original_remote_render = (id (*)(id, SEL, id, id *))replace_method("PBUICARemoteRenderer", "renderRequest:error:", 0x1b0cf3400UL,
	    (IMP)remote_render);
	original_local_render = (id (*)(id, SEL, id, id *))replace_method("PBUICALocalRenderer", "renderRequest:error:", 0x1b0ce2df4UL,
	    (IMP)local_render);
	original_snapshot_read = (id (*)(id, SEL, id *))replace_method("PBUIURLBackedSnapshotSource", "readFromDiskWithError:",
	    0x1b0cf1d70UL, (IMP)snapshot_read);
	original_snapshot_update = (BOOL (*)(id, SEL, id, id, id *))replace_method("PBUIURLBackedSnapshotSource",
	    "updateWithImage:contentColorStatistics:error:", 0x1b0cf1af0UL, (IMP)snapshot_update);
	say("hakios: SpringBoard: the wallpaper's material pictures: renderers %s/%s, cached snapshots %s, new snapshots %s, "
	    "snapshot preconditions %s, scene captures %s\n", original_remote_render ? "checked" : "NOT checked",
	    original_local_render ? "checked" : "NOT checked", original_snapshot_read ? "checked" : "NOT checked",
	    original_snapshot_update ? "checked" : "NOT checked", original_snapshot_preconditions ? "said" : "NOT said",
	    original_snapshot_capture ? "said" : "NOT said");
}

#include "umi_idle_timer.h"
#include "umi_statusbar_signal.h"

/* ---- the home screen's data-network type ("4G") after boot --------------------------------------- */

/*
 * Why the "4G" is missing at first and a respring brings it back (v29g trace, INVESTIGATION-sb-trace-abort.md §4):
 * the home bar's type item is item 9 of SBStatusBarStateAggregator's raw data (primary; 10 is the secondary
 * SIM), and it is switched on by -_updateDataNetworkItem, which reads the SystemStatus telephony domain.
 * SpringBoard's CommCenter client registers for its data-status subscriptions a few seconds into the
 * process; the aggregator's first data snapshot lands before that, leaves itemIsEnabled[9] = 0, and no
 * later notification re-evaluates it. A respring builds the bar after the data is there, so the item
 * is on from the start.
 *
 * -[SBStatusBarStateAggregator updateStatusBarItem:] (20F66, 0x1c4a079fc) is the aggregator's own
 * dispatch method: its jump table (0x1c519a8c2) routes 9 to -_updateDataNetworkItem (0x1c4a07aa8)
 * and 10 to -_updateSecondaryDataNetworkItem (0x1c4a07ab4), wrapping both in begin/endCoalescentBlock
 * so the result is posted. SpringBoard itself calls it for 9 and 10 in the telephony-domain changed
 * handler installed by _registerForNotifications (the block reached from the observeDataWithBlock:
 * at 0x1c5041794, 0x1c5042bb4, updates 4,5,6,7,9,10,...), and in _block_invoke_24 (0x1c5042d64,
 * 9 and 10 only). Calling it re-reads the domain and is therefore the native way to make the item
 * re-evaluate; it changes no state by itself when the domain still has nothing to show.
 *
 * The nudge is bounded: first at 7 s, then every 6 s, at most six times, and only while item 9 is
 * still disabled. The table is read from the `_data` ivar (offset 424, whose first member is
 * _Bool itemIsEnabled[46]); -_statusBarData, 0x1c5040778, is just "add x0, x0, #0x1a8; ret" and is
 * declared returning a *const* struct, so its encoding starts with the qualifier 'r'
 * (r^{?=[46B]...}16@0:8) and is only a fallback. Retries stop as soon as item 9 is enabled (also
 * after a nudge that left it off: one made before the modem registered can't bring it up, a later
 * one can), else after six attempts. Main thread only, no hooks, no UIKit views. When Wi-Fi is
 * active item 9 is disabled by design, so all six attempts run and each logs its result.
 */
#define UMI_DATA_TYPE_NUDGES 6

/* The SpringBoard app's aggregator. -[SpringBoard statusBarStateAggregator] asserts while the ivar
 * is nil, so the ivar is read instead (typed "@" by -umi_signal_ivar_object); the app keeps it for
 * the process lifetime. SBMainStatusBarStateProvider (the HOME bar's provider) gets the same object
 * from [SBApp statusBarStateAggregator] in its -init, 0x1c504e9a4. */
static id
umi_status_bar_aggregator(void)
{
	Class app_class = objc_getClass("UIApplication");
	id app = app_class ? ((id (*)(Class, SEL))objc_msgSend)(app_class, sel_registerName("sharedApplication")) : nil;

	return umi_signal_ivar_object(app, "_statusBarStateAggregator");
}

/* itemIsEnabled[item], or -1 when neither the `_data` ivar nor the getter can be read.
 *
 * 20F66: -[SBStatusBarStateAggregator _statusBarData] (0x1c5040778) is "add x0, x0, #0x1a8; ret"
 * and is declared returning a *const* struct, so its encoding begins with the qualifier 'r':
 *   r^{?=[46B][64c]...}16@0:8
 * so read the ivar it returns (same bytes, same shape) and only fall back to the getter, skipping
 * the qualifiers before checking for '^'.
 */
static const char umi_sb_data_table_prefix[] = "{?=\"itemIsEnabled\"[46B]";

static int
umi_status_bar_item_enabled(id aggregator, int item)
{
	const unsigned char *data;
	char type[64] = "";

	if (aggregator == NULL || item < 0 || item >= 46) {
		return -1;
	}
	if ((data = umi_signal_ivar(aggregator, "_data", umi_sb_data_table_prefix)) != NULL) {
		return data[item];
	}
	{
		Method method = class_getInstanceMethod(object_getClass(aggregator), sel_registerName("_statusBarData"));
		const char *p = type;

		if (method == NULL) {
			return -1;
		}
		method_getReturnType(method, type, sizeof(type));
		while (*p != 0 && strchr("rnNoORV", *p) != NULL) {
			p++;
		}
		if (*p != '^') {
			return -1;
		}
		data = ((const unsigned char *(*)(id, SEL))objc_msgSend)(aggregator, sel_registerName("_statusBarData"));
		return data != NULL ? data[item] : -1;
	}
}

/* What umi_status_bar_item_enabled() had to work with, for the one-line failure log. */
static const char *
umi_status_bar_table_source(id aggregator)
{
	Ivar table;
	const char *type;

	if (aggregator == NULL) {
		return "no aggregator";
	}
	table = class_getInstanceVariable(object_getClass(aggregator), "_data");
	type = table != NULL ? ivar_getTypeEncoding(table) : NULL;
	if (type != NULL && strncmp(type, umi_sb_data_table_prefix, sizeof(umi_sb_data_table_prefix) - 1) == 0) {
		return "the _data ivar is readable";
	}
	if (class_getInstanceMethod(object_getClass(aggregator), sel_registerName("_statusBarData")) != NULL) {
		return "_data ivar absent or mistyped, _statusBarData present but unusable";
	}
	return "_data ivar and _statusBarData both absent";
}

static void
umi_data_type_nudge(int attempt)
{
	id aggregator = umi_status_bar_aggregator();
	Method update = aggregator ? class_getInstanceMethod(object_getClass(aggregator), sel_registerName("updateStatusBarItem:")) : NULL;
	int before = umi_status_bar_item_enabled(aggregator, 9);
	char type[16] = "", argument[16] = "";
	bool usable = false, performed = false;

	if (before == 1) {
		return;         /* the item is on: nothing to nudge */
	}
	if (update != NULL) {
		const char *p = argument;

		method_getReturnType(update, type, sizeof(type));
		method_getArgumentType(update, 2, argument, sizeof(argument));
		while (*p && strchr("rnNoORV", *p) != NULL) {
			p++;
		}
		/* -updateStatusBarItem: is (void)(id, SEL, int) in 20F66: v@:i (w2 indexes the jump table). */
		usable = type[0] == 'v' && method_getNumberOfArguments(update) == 3 && *p != 0 && strchr("iIqQ", *p) != NULL;
	}
	if (aggregator == nil) {
		say("hakios: sb-data-type nudge %d: no aggregator\n", attempt);
	} else if (before < 0) {
		/* The aggregator's data table could not be read: "the aggregator exists and its table is readable"
		 * is the only cheap readiness proof there is, so do not call into it. The retry below still runs. */
		say("hakios: sb-data-type nudge %d: no item table (class %s: %s)\n", attempt, object_getClassName(aggregator),
		    umi_status_bar_table_source(aggregator));
	} else if (!usable) {
		say("hakios: sb-data-type nudge %d: no -updateStatusBarItem: (item 9 %d)\n", attempt, before);
	} else {
		((void (*)(id, SEL, int))objc_msgSend)(aggregator, sel_registerName("updateStatusBarItem:"), 9);
		((void (*)(id, SEL, int))objc_msgSend)(aggregator, sel_registerName("updateStatusBarItem:"), 10);
		performed = true;
		say("hakios: sb-data-type nudge %d: item 9 %d->%d, item 10 %d\n", attempt, before,
		    umi_status_bar_item_enabled(aggregator, 9), umi_status_bar_item_enabled(aggregator, 10));
	}
	/* Keep going until item 9 is on (before == 1 returned above): a nudge made before the modem
	 * registered leaves it off (0->0), and only a later one can bring it up. */
	if (attempt < UMI_DATA_TYPE_NUDGES && !(performed && umi_status_bar_item_enabled(aggregator, 9) == 1)) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			umi_data_type_nudge(attempt + 1);
		});
	}
}

__attribute__((constructor))
static void
hakios_springboard_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	IMP activation, snapshot_format, battery_lvl, battery_st, poster_switcher;
	int token;

	if (name == NULL) {
		return;
	}
	if (strcmp(name, "SpringBoard") != 0) {
		static char who[64];

		snprintf(who, sizeof(who), " of %s", name);
		layers_of = who;
		if ((say_fd = open("/dev/console", O_WRONLY | O_NOCTTY)) < 0) {
			say_fd = 2;
		}
		say("hakios: in %s[%d]: its layers are said on the notification com.hakios.layers\n", name, getpid());
		notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
			layers();
		});
		/* (PosterBoard's snapshots of the posters as PNG, as SpringBoard's: pictures_as_png) */
		if (strcmp(name, "PosterBoard") == 0) {
			slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
			pictures_as_png();
		}
		return;
	}
	caenv();
	/* Shared, privacy-safe SMS gate diagnostics; one kernel-inserted library per process. */
	if (!dlopen("/redwood/lib/libhakios.notif.dylib", RTLD_NOW | RTLD_LOCAL)) {
		say("hakios: notify: SpringBoard diagnostics load failed\n");
	}
	display_link_rates();
	accessibility_settings();
	/*
	 * The tweaks of the package manager's prefix, if one is installed: ElleKit's loader
	 * (/var/jb/usr/lib/TweakLoader.dylib) reads /var/jb/usr/lib/TweakInject and opens the tweaks
	 * whose Filter names this process. The kernel could insert it as well (etc/inject.conf), but
	 * only one inserted library per program is reliable here, and SpringBoard's is this one; so
	 * this one opens it, the way a jailbreak's own hook does.
	 */
	load_tweaks();
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	umi_idle_install();
	activation = replace_method("SBLockdownManager", "_setupActivationState", 0x1c4bc8dfcUL, (IMP)setup_activation_state);
	original_set_setup_required_reason = (void (*)(id, SEL, long))replace_method("SBSetupManager",
	    "_setSetupRequiredReason:", 0x1c4cdd3a4UL, (IMP)set_setup_required_reason);
	snapshot_format = replace_method_of_kind('+', "XBApplicationSnapshotManifestImpl", "_outputFormatForSnapshot:",
	    0x1f1c91480UL, (IMP)snapshot_output_format);
	plain_vibrancy();
	ui_lite();
	wallpaper_cache();
	status_bar_fallback_is_split();
	wallpaper_freeze();
	original_lock_long_press = (void (*)(id, SEL, id))replace_method("SBLockHardwareButton", "longPress:", 0x1c4f6ab44UL,
	    (IMP)lock_long_press);
	poster_switcher = replace_method("SBMainDisplayPolicyAggregator", "_allowsCapabilityPosterSwitcherWithExplanation:",
	    0x1c507e0fcUL, (IMP)poster_switcher_allowed);
	original_switcher_wallpaper_overlay = (void (*)(id, SEL))replace_method("SBAppSwitcherPageView",
	    "_createOrDestroyWallpaperOverlayView", 0x1c4af7860UL, (IMP)switcher_wallpaper_overlay);
	material_pictures();
	/*
	 * Battery hooks on UIDevice: these are the methods every app and SpringBoard itself calls, so
	 * hooking them here in the SpringBoard process covers both the status bar and any app that
	 * reads [UIDevice currentDevice].batteryLevel. The addresses are for Darwin userland 16.5 (20F66).
	 */
	original_battery_level = (float (*)(id, SEL))replace_method("UIDevice", "batteryLevel", 0x1897021b0UL, (IMP)battery_level);
	original_battery_state = (long (*)(id, SEL))replace_method("UIDevice", "batteryState", 0x189209794UL, (IMP)battery_state);
	original_battery_view_shows_percentage = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "showsPercentage",
	    0x189a9370cUL, (IMP)battery_view_shows_percentage);
	original_battery_view_set_shows_percentage = (void (*)(id, SEL, BOOL))replace_method("_UIBatteryView", "setShowsPercentage:",
	    0x18926e150UL, (IMP)battery_view_set_shows_percentage);
	original_battery_view_should_show_bolt = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "_shouldShowBolt",
	    0x189203e28UL, (IMP)battery_view_should_show_bolt);
	original_battery_view_charge_percent = (double (*)(id, SEL))replace_method("_UIBatteryView", "chargePercent",
	    0x189a935d8UL, (IMP)battery_view_charge_percent);
	original_battery_view_set_charge_percent = (void (*)(id, SEL, double))replace_method("_UIBatteryView", "setChargePercent:",
	    0x18928d310UL, (IMP)battery_view_set_charge_percent);
	original_ring_charging_level = (double (*)(id, SEL))replace_method("CSBatteryChargingRingView", "batteryChargingLevel",
	    0x1c48c2cf4UL, (IMP)ring_charging_level);
	battery_lvl = (IMP)original_battery_level;
	battery_st  = (IMP)original_battery_state;
	say("hakios: SpringBoard: activation %s, setup assistant %s, application snapshots %s, the side button's long press %s, "
	    "battery level %s, battery state %s, poster switcher %s\n",
	    activation ? "answered here" : "NOT replaced", original_set_setup_required_reason ? "refused" : "NOT replaced",
	    snapshot_format ? "as JPEG" : "NOT changed", original_lock_long_press ? "checked" : "NOT checked",
	    battery_lvl ? "from QG" : "NOT replaced", battery_st ? "from QG" : "NOT replaced",
	    poster_switcher ? "not offered" : "NOT withheld");
	/*
	 * Cellular, a survey first (the modem's bars come from the kernel: sysctl kern.hakios.cellular_*,
	 * drivers/modem/hakios.mpss.c): where UIKit makes the status bar's cellular entry from the raw data,
	 * and with which signature -- said in the log with the unslid address, for the hook to come.
	 */
	{
		static const char *const classes[] = { "_UIStatusBarDataCellularEntry", "_UIStatusBarData", "_UIStatusBarDataEntry" };
		const char *selector = "_cellularEntryWithData:signalStrengthEnabled:serviceEnabled:dataNetworkEnabled:serviceString:"
		    "serviceCrossfadeString:serviceBadgeString:serviceContentType:dataNetworkType:gsmSignalStrengthRaw:"
		    "gsmSignalStrengthBars:callForwardingType:lowDataModeActive:";

		for (unsigned c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
			Class cls = objc_getClass(classes[c]);
			Method plus = cls ? class_getClassMethod(cls, sel_registerName(selector)) : NULL;
			Method minus = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

			if (plus) {
				say("hakios: cellular: +[%s %s] at %#lx, types \"%s\"\n", classes[c], selector,
				    (uintptr_t)method_getImplementation(plus) - slide, method_getTypeEncoding(plus));
			}
			if (minus) {
				say("hakios: cellular: -[%s %s] at %#lx, types \"%s\"\n", classes[c], selector,
				    (uintptr_t)method_getImplementation(minus) - slide, method_getTypeEncoding(minus));
			}
			if (cls && !plus && !minus) {
				say("hakios: cellular: %s has no such method\n", classes[c]);
			}
		}
		say("hakios: cellular: the kernel says up=%d registered=%d bars=%d dbm=%d radio=%d\n", sysctl_int("kern.hakios.cellular_up"),
		    sysctl_int("kern.hakios.cellular_registered"), sysctl_int("kern.hakios.cellular_bars"),
		    sysctl_int("kern.hakios.cellular_dbm"), sysctl_int("kern.hakios.cellular_radio"));
	}
	cellular_hook();
	umi_statusbar_signal_trace_install();
	say("hakios: battery UI: shows%% %s, set-shows%% %s, bolt %s, chargePercent %s, setChargePercent %s, lockscreen ring %s\n",
	    original_battery_view_shows_percentage ? "ok" : "NO",
	    original_battery_view_set_shows_percentage ? "ok" : "NO",
	    original_battery_view_should_show_bolt ? "ok" : "NO",
	    original_battery_view_charge_percent ? "ok" : "NO",
	    original_battery_view_set_charge_percent ? "ok" : "NO",
	    original_ring_charging_level ? "ok" : "NO");
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *delay;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (delay = strstr(args, "sb_delay=")) != NULL &&
		    (delay == args || delay[-1] == ' ')) {
			usleep((useconds_t)(atof(delay + 9) * 1e6));
		}
	}
	/*
	 * UIDevice won't call batteryLevel/batteryState unless monitoring is enabled. Flip it on once
	 * SpringBoard's run loop is running (the dispatch_after fires on the main queue, which means
	 * we are past UIApplication startup).
	 */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		Class uidev = objc_getClass("UIDevice");
		SEL current = sel_registerName("currentDevice");
		SEL monitoring = sel_registerName("setBatteryMonitoringEnabled:");
		id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, current) : nil;

		if (device && class_getInstanceMethod(object_getClass(device), monitoring)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(device, monitoring, YES);
		}
		watch_main_loop();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
				control_center_fades(arguments);
			}
		}
		say("hakios: battery: SOC=%d%% battery=%d charger=%d charging=%d\n",
		    sysctl_int("kern.hakios.battery_soc"), sysctl_int("kern.hakios.battery_present"),
		    sysctl_int("kern.hakios.battery_charger_present"), sysctl_int("kern.hakios.battery_charging"));
		battery_post_notifications();
	});
	notify_register_dispatch("com.hakios.unlock", &token, dispatch_get_main_queue(), ^(__unused int t) {
		unlock();
	});
	notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
		layers();
	});
	notify_register_dispatch("com.hakios.switcher", &token, dispatch_get_main_queue(), ^(__unused int t) {
		switcher();
	});
	/* The home bar's "4G" (see umi_data_type_nudge): re-evaluate the data-network item once telephony
	 * is ready, at most six times 6 s apart, and only while the item is still off. */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 7 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		umi_data_type_nudge(1);
	});
	/*
	 * Belt-and-suspenders unlock. The user can always swipe up to open the home screen -- that
	 * works fine when the touch driver is doing its job. But if touch is still probing, has
	 * failed, or the panel is a FocalTech and the firmware upload is still chewing through
	 * ~114 KB of SPI (that takes a hot second during bring-up), the user ends up staring at a
	 * dark lock screen with nothing to swipe. So we just forcibly dismiss it after 10 seconds
	 * so Darwin always lands on the home screen even in the worst case. Harmless if touch is
	 * already working: the user just doesn't need to swipe themselves.
	 * With autounlock=0 among the kernel's arguments the lock screen stays until it is swiped away
	 * (to show the phone starting the way an iPhone does).
	 */
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *arg;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (arg = strstr(args, "autounlock=0")) != NULL &&
		    (arg == args || arg[-1] == ' ')) {
			say("hakios: the lock screen stays until it is swiped away (autounlock=0)\n");
			return;
		}
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		unlock();
	});
}
