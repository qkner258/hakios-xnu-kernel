/* iOS 16.5/20F66: SMS receipt through presentation, audio and haptic diagnostics.
 * No policy overrides. XPC services do not inherit a launchd job's console logging.
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>
/* This SDK ships libxpc's export stub without its public headers. These are the
 * public C declarations used below; no private dynamically looked-up symbols. */
typedef void *xpc_object_t;
typedef struct _xpc_connection_s *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern const struct _xpc_type_s _xpc_type_dictionary, _xpc_type_connection,
    _xpc_type_error, _xpc_type_int64, _xpc_type_uint64, _xpc_type_bool, _xpc_type_double;
extern struct _xpc_dictionary_s _xpc_error_connection_interrupted,
    _xpc_error_connection_invalid, _xpc_error_termination_imminent;
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_ERROR (&_xpc_type_error)
#define XPC_TYPE_INT64 (&_xpc_type_int64)
#define XPC_TYPE_UINT64 (&_xpc_type_uint64)
#define XPC_TYPE_BOOL (&_xpc_type_bool)
#define XPC_TYPE_DOUBLE (&_xpc_type_double)
#define XPC_ERROR_CONNECTION_INTERRUPTED ((xpc_object_t)&_xpc_error_connection_interrupted)
#define XPC_ERROR_CONNECTION_INVALID ((xpc_object_t)&_xpc_error_connection_invalid)
#define XPC_ERROR_TERMINATION_IMMINENT ((xpc_object_t)&_xpc_error_termination_imminent)
extern const char *xpc_connection_get_name(xpc_connection_t);
extern xpc_type_t xpc_get_type(xpc_object_t);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t, const char *);
extern int64_t xpc_int64_get_value(xpc_object_t);
extern uint64_t xpc_uint64_get_value(xpc_object_t);
extern bool xpc_bool_get_value(xpc_object_t);
extern double xpc_double_get_value(xpc_object_t);
#include <execinfo.h>
#include <fcntl.h>
#include <notify.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GET(type, obj, sel) ((type (*)(id, SEL))objc_msgSend)(obj, sel_registerName(sel))
static int console;

static void note(const char *fmt, ...)
{
    char line[640];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int at = snprintf(line, sizeof(line), "hakios: notify: pid=%d t=%ld.%03ld ",
                      getpid(), ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + at, sizeof(line) - at, fmt, ap);
    va_end(ap);
    write(console, line, strlen(line));
}

/* Each entry is installed once, including frameworks loaded after our constructor.
 * Block IMPs are made beforehand, outside the dyld image callback. */
typedef struct {
    const char *cls, *sel, *types;
    IMP replacement, *original;
    BOOL installed, wrong_type;
} Hook;
static Hook legacy[32];
static size_t legacy_count;
static void register_hook(const char *cls, const char *sel, IMP replacement,
                          IMP *original, const char *types)
{
    legacy[legacy_count++] = (Hook){cls, sel, types, replacement, original, NO, NO};
}

/* No total log budget. State belongs to the observed object and dies with it.
 * Numbers are local anonymous ordinals, never notification/message identifiers. */
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static char trace_key;
static unsigned long next_object;
static id process_state;
static void trace(id object, const char *stage, const char *fmt, ...)
{
    char value[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(value, sizeof(value), fmt, ap);
    va_end(ap);
    @autoreleasepool {
        pthread_mutex_lock(&trace_lock);
        if (!object) object = process_state;
        NSMutableDictionary *state = objc_getAssociatedObject(object, &trace_key);
        if (!state) {
            state = [NSMutableDictionary dictionary];
            state[@"ordinal"] = @(++next_object);
            objc_setAssociatedObject(object, &trace_key, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        NSString *key = [NSString stringWithUTF8String:stage];
        NSString *v = [NSString stringWithUTF8String:value];
        BOOL changed = ![state[key] isEqualToString:v];
        if (changed) state[key] = v;
        unsigned long ordinal = [state[@"ordinal"] unsignedLongValue];
        pthread_mutex_unlock(&trace_lock);
        if (changed) note("obj=%lu %s %s\n", ordinal, stage, value);
    }
}

static void store_state(const char *stage)
{
    struct stat st;
    int rc = stat("/var/mobile/Library/Application Support/com.apple.remotemanagementd", &st);
    int error = rc ? errno : 0;
    trace(nil, stage, "store exists=%d errno=%d inode=%llu uid=%u gid=%u mode=%o", rc == 0, error,
          rc ? 0ULL : (unsigned long long)st.st_ino, rc ? 0 : st.st_uid,
          rc ? 0 : st.st_gid, rc ? 0 : st.st_mode & 07777);
}

static void settings(id center)
{
    ((void (*)(id, SEL, id))objc_msgSend)(center,
        sel_registerName("getNotificationSettingsWithCompletionHandler:"), ^(id s) {
        trace(nil, "settings", "present=%d auth=%ld alert=%ld style=%ld lock=%ld sound=%ld scheduled=%ld",
             s != nil, GET(long, s, "authorizationStatus"), GET(long, s, "alertSetting"),
             GET(long, s, "alertStyle"), GET(long, s, "lockScreenSetting"),
             GET(long, s, "soundSetting"), GET(long, s, "scheduledDeliverySetting"));
    });
}

/* Query once after SpringBoard startup and again when an SMS reaches its server. */
static void presentation_state(void)
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            id center = ((id (*)(id, SEL, id))objc_msgSend)(
                GET(id, objc_getClass("UNUserNotificationCenter"), "alloc"),
                sel_registerName("initWithBundleIdentifier:"), @"com.apple.MobileSMS");
            settings(center);
            [center release];
            id service = ((id (*)(id, SEL, id))objc_msgSend)(objc_getClass("DNDStateService"),
                sel_registerName("serviceForClientIdentifier:"), @"com.apple.springboard");
            NSError *error = nil;
            id state = ((id (*)(id, SEL, NSError **))objc_msgSend)(service,
                sel_registerName("queryCurrentStateWithError:"), &error);
            trace(nil, "focus-sample", "present=%d active=%d suppress=%d error=%ld", state != nil,
                 GET(BOOL, state, "isActive"), GET(BOOL, state, "willSuppressInterruptions"),
                 (long)error.code);
            int token;
            uint64_t ring = 0;
            uint32_t status = notify_register_check("com.apple.springboard.ringerstate", &token);
            if (status == NOTIFY_STATUS_OK) {
                status = notify_get_state(token, &ring);
                notify_cancel(token);
            }
            float volume = 0;
            id audio = GET(id, objc_getClass("AVSystemController"), "sharedAVSystemController");
            BOOL got_volume = ((BOOL (*)(id, SEL, float *, id))objc_msgSend)(audio,
                sel_registerName("getVolume:forCategory:"), &volume, @"Ringtone");
            trace(nil, "ringer", "status=%u on=%llu volumeValid=%d volume=%.3f", status,
                 (unsigned long long)ring, got_volume, volume);
        }
    });
}

static void (*post_original)(id, SEL);
static void post(id self, SEL sel)
{
    note("imd-post\n");
    settings(GET(id, self, "notificationCenter"));
    post_original(self, sel);
}

static unsigned long (*st_original)(id, SEL, id);
static unsigned long screen_time(id self, SEL sel, id context)
{
    unsigned long options = st_original(self, sel, context);
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:@"com.apple.ScreenTimeAgent"];
    trace(context, "st", "enabled=%d context=%d allowed=%d limited=%d options=%lu",
         [defaults boolForKey:@"ScreenTimeEnabled"], context != nil,
         GET(BOOL, context, "allowedByScreenTime"),
         GET(BOOL, context, "applicationCurrentlyLimited"), options);
    [defaults release];
    store_state("sms-gate");
    return options;
}

static BOOL (*eligible_original)(id, SEL, id, id);
static BOOL eligible(id self, SEL sel, id chat, id message)
{
    BOOL result = eligible_original(self, sel, chat, message);
    trace(message, "imd-eligible", "result=%d", result);
    return result;
}

static id (*generate_original)(id, SEL, id, id, id, BOOL, BOOL, BOOL, BOOL *);
static id generate(id self, SEL sel, id record, id message, id chat,
                   BOOL urgent, BOOL carousel, BOOL active, BOOL *advance)
{
    id request = generate_original(self, sel, record, message, chat, urgent, carousel, active, advance);
    trace(request ?: record, "imd-generated", "present=%d mostActive=%d sound=%d", request != nil, active,
         GET(id, GET(id, request, "content"), "sound") != nil);
    return request;
}

/* Generation is followed by a separate migration/repost check. None of the
 * server/presentation probes run if that check refuses or never calls back. */
static BOOL (*migrated_original)(id, SEL);
static BOOL migrated(id self, SEL sel)
{
    BOOL ready = migrated_original(self, sel);
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:@"com.apple.imdsmsrecordstore"];
    id build = [defaults objectForKey:@"kIMDPersistenceLastBuild"];
    trace(self, "imd-migration", "ready=%d markerPresent=%d markerMatches20F66=%d",
          ready, build != nil, [build isEqual:@"20F66"]);
    [defaults release];
    return ready;
}

static void (*submit_check_original)(id, SEL, id, id);
static void submit_check(id self, SEL sel, id request, void (^completion)(BOOL))
{
    trace(request, "imd-submit-check", "enter=1");
    submit_check_original(self, sel, request, ^(BOOL should_post) {
        trace(request, "imd-submit-decision", "post=%d", should_post);
        completion(should_post);
    });
}

static void (*delivered_original)(id, SEL, id);
static void delivered(id self, SEL sel, void (^completion)(NSArray *))
{
    note("imd-delivered-query\n");
    delivered_original(self, sel, ^(NSArray *notifications) {
        note("imd-delivered-complete present=%d count=%lu\n", notifications != nil,
             (unsigned long)notifications.count);
        completion(notifications);
    });
}

static void (*client_add_original)(id, SEL, id, id);
static void client_add(id self, SEL sel, id request, void (^completion)(NSError *))
{
    trace(request, "imd-client-add", "sound=%d destinations=%lu", GET(id, GET(id, request, "content"), "sound") != nil, GET(unsigned long, request, "destinations"));
    client_add_original(self, sel, request, ^(NSError *error) {
        trace(request, "imd-client-add-complete", "error=%ld", (long)error.code);
        if (completion) completion(error);
    });
}

static BOOL sms(id bundle) { return [bundle isEqualToString:@"com.apple.MobileSMS"]; }
static BOOL sms_request(id request) { return sms(GET(id, request, "sectionIdentifier")); }

static void (*add_original)(id, SEL, id, id, id);
static void add(id self, SEL sel, id request, id bundle, void (^completion)(NSError *))
{
    if (!sms(bundle)) { add_original(self, sel, request, bundle, completion); return; }
    trace(request, "server-add", "sound=%d", GET(id, GET(id, request, "content"), "sound") != nil);
    presentation_state();
    add_original(self, sel, request, bundle, ^(NSError *error) {
        trace(request, "server-add-complete", "error=%ld", (long)error.code);
        if (completion) completion(error);
    });
}

static id (*section_original)(id, SEL, id, BOOL);
static id section(id self, SEL sel, id bundle, BOOL effective)
{
    id info = section_original(self, sel, bundle, effective);
    if (sms(bundle))
        trace(self, effective ? "bb-section-effective" : "bb-section-raw", "effective=%d present=%d allows=%d alertType=%lu lock=%ld push=%lu",
             effective, info != nil, GET(BOOL, info, "allowsNotifications"),
             GET(unsigned long, info, "alertType"), GET(long, info, "lockScreenSetting"),
             GET(unsigned long, info, "pushSettings"));
    return info;
}

static BOOL (*banner_original)(id, SEL, id);
static BOOL banner(id self, SEL sel, id request)
{
    BOOL result = banner_original(self, sel, request);
    if (sms_request(request)) trace(request, "banner-canReceive", "result=%d", result);
    return result;
}

static BOOL (*sound_original)(id, SEL, id);
static BOOL sound(id self, SEL sel, id request)
{
    BOOL result = sound_original(self, sel, request);
    if (sms_request(request))
        trace(request, "sound-canPlay", "result=%d suppress=%d hasSound=%d", result,
             GET(BOOL, GET(id, request, "alertOptions"), "shouldSuppress"),
             GET(id, request, "sound") != nil);
    return result;
}

static void (*play_original)(id, SEL, id, id);
static void play(id self, SEL sel, id request, id destination)
{
    if (sms_request(request)) trace(request, "sound-play", "destination=%s", object_getClassName(destination));
    play_original(self, sel, request, destination);
}

static void (*vault_original)(id, SEL);
static void vault(id self, SEL sel)
{
    store_state("before-vault");
    vault_original(self, sel);
    store_state("after-vault");
}

static void (*loaded_original)(id, SEL, id, id, id);
static void loaded(id self, SEL sel, id description, NSError *error, id completion)
{
    note("st-store-loaded error=%ld\n", (long)error.code);
    store_state("store-loaded");
    loaded_original(self, sel, description, error, completion);
}

static BOOL store_path(NSString *path)
{
    NSString *root = @"/var/mobile/Library/Application Support/com.apple.remotemanagementd";
    if ([path hasPrefix:@"/private/"]) path = [path substringFromIndex:8];
    return [path isEqualToString:root] || [path hasPrefix:[root stringByAppendingString:@"/"]];
}

static void removal_trace(void)
{
    void *frames[8];
    int n = backtrace(frames, 8);
    note("store-remove frames=%d\n", n);
    for (int i = 1; i < n; i++) note("store-remove frame=%d pc=%p\n", i, frames[i]);
}

static BOOL (*remove_url_original)(id, SEL, NSURL *, NSError **);
static BOOL remove_url(id self, SEL sel, NSURL *url, NSError **error)
{
    BOOL tracked = store_path(url.path);
    if (tracked) removal_trace();
    BOOL result = remove_url_original(self, sel, url, error);
    if (tracked) { note("store-remove-url result=%d\n", result); store_state("after-remove"); }
    return result;
}

static BOOL (*remove_path_original)(id, SEL, NSString *, NSError **);
static BOOL remove_path(id self, SEL sel, NSString *path, NSError **error)
{
    BOOL tracked = store_path(path);
    if (tracked) removal_trace();
    BOOL result = remove_path_original(self, sel, path, error);
    if (tracked) { note("store-remove-path result=%d\n", result); store_state("after-remove"); }
    return result;
}

static void (*tone_original)(id, SEL, id, id);
static void tone(id self, SEL sel, id alert, id completion)
{
    id config = GET(id, alert, "configuration");
    trace(alert, "tone-systemsound", "type=%ld vibration=%d externalVibration=%d customVolume=%d volume=%.3g ignoreRinger=%d forceUserVolume=%d ignoreDisabledVibration=%d",
          GET(long, alert, "type"), GET(id, alert, "vibrationIdentifier") != nil,
          GET(id, config, "externalVibrationPattern") != nil || GET(id, config, "externalVibrationPatternFileURL") != nil,
          GET(BOOL, config, "_hasCustomAudioVolume"), (double)GET(float, config, "audioVolume"),
          GET(BOOL, config, "shouldIgnoreRingerSwitch"), GET(BOOL, config, "shouldForcePlayingAtUserSelectedAudioVolume"),
          GET(BOOL, config, "shouldIgnoreAccessibilityDisabledVibrationSetting"));
    tone_original(self, sel, alert, completion);
}


enum { P_IM=1, P_SB=2, P_ST=4, P_RM=8, P_MEDIA=16, P_IMAGENT=32 };
typedef enum { BOOL0, BOOL1, BOOL2, BOOL1BOOL, NC_BOOL1, NC_BOOL2, NC_VOID1, VOID1, DEST_VOID1, REG_VOID1, DEST_BOOL, REG_BOOL, REG_REGISTER, PRESENT, PRESENT_BOOL, REG_RESOLVE, OBJECT_ERROR, FOCUS_STATE, FOCUS_EVENT, ST_UPDATE, STATE_BOOL, TONE_FLAGS, TONE_VIBRATION, CONVERSATION, TONE_TASK, TONE_REMOVE, TONE_COMPLETIONS, SB_SETUP, SB_UPDATE, SOUND_IF, REFUSED, FOCUS_UPDATE, BB_PUBLISH, BB_PUBLISH_SECTION, RECEIVE, IM_STORE, IM_BATCH, STORE_DECISION, STORE_DESTROY, RESOLVE, SAVE_REQUEST, SAVE_RECORD, SAVE_RECORD_QUEUE, OBSERVERS, DELIVERED_SERVER } ProbeKind;
typedef struct {
    unsigned processes;
    const char *cls, *sel, *types;
    ProbeKind kind;
    const char *stage;
    BOOL meta;
    IMP original, replacement;
    BOOL installed, wrong_type;
} Probe;
static unsigned process_mask;
static Probe probes[] = {
    {P_SB, "SBNotificationBannerDestination", "_isInSetupMode", "B16@0:8", BOOL0, "_isInSetupMode", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNotificationBannerDestination", "_isDeviceAuthenticated", "B16@0:8", BOOL0, "_isDeviceAuthenticated", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNotificationBannerDestination", "_isUILocked", "B16@0:8", BOOL0, "_isUILocked", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNCNotificationDispatcher", "_lockScreenWantsBanners", "B16@0:8", BOOL0, "_lockScreenWantsBanners", NO, NULL, NULL, NO, NO},
    {P_ST, "STDaemonPersistenceController", "_shouldDestroyStoreGivenError:", "B24@0:8@16", STORE_DECISION, "_shouldDestroyStoreGivenError:", NO, NULL, NULL, NO, NO},
    {P_ST, "STDaemonPersistenceController", "_isFatalSQLiteError:", "B24@0:8@16", STORE_DECISION, "_isFatalSQLiteError:", NO, NULL, NULL, NO, NO},
    {P_ST, "STDaemonPersistenceController", "_isFatalMigrationError:", "B24@0:8@16", STORE_DECISION, "_isFatalMigrationError:", NO, NULL, NULL, NO, NO},
    {P_ST, "STDaemonPersistenceController", "_destroyPersistentStoreWithDescription:", "@24@0:8@16", STORE_DESTROY, "store-destroy", NO, NULL, NULL, NO, NO},
    {P_IMAGENT | P_IM, "IMDMessageStore", "storeMessage:forceReplace:modifyError:modifyFlags:flagMask:updateMessageCache:calculateUnreadCount:reindexMessage:", "@56@0:8@16B24B28B32Q36B44B48B52", IM_STORE, "im-message-store", NO, NULL, NULL, NO, NO},
    {P_IM, "IMDNotificationsController", "_registerUserNotificationsForMessageRecords:newerThanDate:isUrgentMessageTrigger:isCarouselUITriggered:isMostActiveDevice:", "v44@0:8@16q24B32B36B40", IM_BATCH, "imd-register-batch", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDispatcher", "destinationDidBecomeReadyToReceiveNotifications:", "v24@0:8@16", DEST_VOID1, "destination-ready", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "destinationDidBecomeReadyToReceiveNotifications:", "v24@0:8@16", DEST_VOID1, "destination-ready", NO, NULL, NULL, NO, NO},
    {P_IMAGENT, "IMDServiceSession", "didReceiveMessage:forChat:style:account:fromIDSID:", "v52@0:8@16@24C32@36@44", RECEIVE, "imagent-received", NO, NULL, NULL, NO, NO},
    {P_IM, "IMDNotificationsController", "_chatHasDNDSet:", "B24@0:8@16", BOOL1, "_chatHasDNDSet:", NO, NULL, NULL, NO, NO},
    {P_IM, "IMDNotificationsController", "_messageShouldBeSilentlyDeliveredForBusinessChat:", "B24@0:8@16", BOOL1, "_messageShouldBeSilentlyDeliveredForBusinessChat:", NO, NULL, NULL, NO, NO},
    {P_IM, "IMDNotificationsController", "_areAllMessagePartsRetractedForMessageDictionary:", "B24@0:8@16", BOOL1, "_areAllMessagePartsRetractedForMessageDictionary:", NO, NULL, NULL, NO, NO},
    {P_IM, "IMDNotificationsController", "_deviceUnderFirstUnlock", "B16@0:8", BOOL0, "_deviceUnderFirstUnlock", NO, NULL, NULL, NO, NO},
    {P_IM, "IMDNotificationsController", "_notificationIsFromAFilteredSender:messageDictionary:", "B32@0:8@16@24", BOOL2, "_notificationIsFromAFilteredSender:messageDictionary:", NO, NULL, NULL, NO, NO},
    {P_IM, "IMDNotificationsController", "_shouldRepostNotification:withAlreadyPostedNotification:", "B32@0:8@16@24", BOOL2, "_shouldRepostNotification:withAlreadyPostedNotification:", NO, NULL, NULL, NO, NO},
    {P_IM, "IMDNotificationsController", "_chatHasCategoryMuted:isFromContact:", "B28@0:8@16B24", BOOL1BOOL, "_chatHasCategoryMuted:isFromContact:", NO, NULL, NULL, NO, NO},
    {P_IM, "IMDNotificationsController", "_isFacetimeHighlighted:", "B24@0:8@16", BOOL1, "_isFacetimeHighlighted:", YES, NULL, NULL, NO, NO},
    {P_IM, "STConversation", "allowableByContactsHandles:", "@24@0:8@16", CONVERSATION, "st-contact-context", NO, NULL, NULL, NO, NO},
    {P_IM | P_SB | P_ST | P_RM, "STConversationContext", "updateShouldBeAllowedDuringGeneralScreenTime:shouldBeAllowedByScreenTimeWhenLimited:currentApplicationState:emergencyModeEnabled:", "v36@0:8B16B20Q24B32", ST_UPDATE, "st-context-update", NO, NULL, NULL, NO, NO},
    {P_IM | P_SB | P_ST | P_RM, "STManagementState", "communicationPoliciesWithError:", "@24@0:8^@16", OBJECT_ERROR, "communicationPoliciesWithError:", NO, NULL, NULL, NO, NO},
    {P_IM | P_SB | P_ST | P_RM, "DMFCommunicationPolicyMonitor", "requestPoliciesByBundleIdentifierWithError:", "@24@0:8^@16", OBJECT_ERROR, "requestPoliciesByBundleIdentifierWithError:", NO, NULL, NULL, NO, NO},
    {P_IM | P_SB | P_ST | P_RM, "DMFEmergencyModeMonitor", "emergencyModeStatusWithError:", "@24@0:8^@16", OBJECT_ERROR, "emergencyModeStatusWithError:", NO, NULL, NULL, NO, NO},
    {P_IM | P_SB | P_ST | P_RM, "DNDStateService", "queryCurrentStateWithError:", "@24@0:8^@16", FOCUS_STATE, "focus-native-query", NO, NULL, NULL, NO, NO},
    {P_IM | P_SB | P_ST | P_RM, "DNDStateService", "remoteService:didReceiveDoNotDisturbStateUpdate:", "v32@0:8@16@24", FOCUS_UPDATE, "focus-native-update", NO, NULL, NULL, NO, NO},
    {P_IM | P_SB | P_ST | P_RM, "DNDEventBehaviorResolutionService", "resolveBehaviorForEventDetails:error:", "@32@0:8@16^@24", FOCUS_EVENT, "focus-event-decision", NO, NULL, NULL, NO, NO},
    {P_SB, "UNSUserNotificationServerConnectionListener", "_serviceResolvedNotificationRequest:forBundleIdentifier:error:", "@40@0:8@16@24o^@32", RESOLVE, "server-resolved", NO, NULL, NULL, NO, NO},
    {P_SB, "UNSUserNotificationServerConnectionListener", "_saveNotificationRequest:shouldRepost:deliveredDate:forBundleIdentifier:withCompletionHandler:", "v52@0:8@16B24@28@36@?44", SAVE_REQUEST, "server-save-request", NO, NULL, NULL, NO, NO},
    {P_SB, "UNSUserNotificationServerConnectionListener", "getDeliveredNotificationsForBundleIdentifier:withCompletionHandler:", "v32@0:8@16@?24", DELIVERED_SERVER, "server-delivered-query", NO, NULL, NULL, NO, NO},
    {P_SB, "UNSNotificationRepository", "saveNotificationRecord:shouldRepost:forBundleIdentifier:withCompletionHandler:", "v44@0:8@16B24@28@?36", SAVE_RECORD, "server-save-record", NO, NULL, NULL, NO, NO},
    {P_SB, "UNSNotificationRepository", "_queue_saveNotificationRecord:shouldRepost:withOptions:forBundleIdentifier:", "v44@0:8@16B24Q28@36", SAVE_RECORD_QUEUE, "server-repository-save", NO, NULL, NULL, NO, NO},
    {P_SB, "UNSNotificationRepository", "_queue_notifyObserversNotificationsDidAddNotifications:replaceNotifications:replacementNotifications:removedNotifications:shouldRepost:forBundleIdentifier:", "v60@0:8@16@24@32@40B48@52", OBSERVERS, "server-repository-observers", NO, NULL, NULL, NO, NO},
    {P_SB, "BBServer", "publishBulletin:destinations:", "v32@0:8@16Q24", BB_PUBLISH, "publishBulletin:destinations:", NO, NULL, NULL, NO, NO},
    {P_SB, "BBServer", "publishBulletinRequest:destinations:", "v32@0:8@16Q24", BB_PUBLISH, "publishBulletinRequest:destinations:", NO, NULL, NULL, NO, NO},
    {P_SB, "BBServer", "_publishBulletinRequest:forSectionID:forDestinations:", "v40@0:8@16@24Q32", BB_PUBLISH_SECTION, "_publishBulletinRequest:forSectionID:forDestinations:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDispatcher", "postNotificationWithRequest:", "v24@0:8@16", NC_VOID1, "postNotificationWithRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "postNotificationRequest:", "v24@0:8@16", NC_VOID1, "postNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNotificationBannerDestination", "postNotificationRequest:", "v24@0:8@16", NC_VOID1, "postNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNotificationBannerDestination", "_publishDidPresentBannerForNotificationRequest:", "v24@0:8@16", NC_VOID1, "_publishDidPresentBannerForNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationMasterList", "insertNotificationRequest:", "v24@0:8@16", NC_VOID1, "insertNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationStructuredListViewController", "insertNotificationRequest:", "v24@0:8@16", NC_VOID1, "insertNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNCSoundController", "stopSoundForNotificationRequest:", "v24@0:8@16", NC_VOID1, "stopSoundForNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDispatcher", "_shouldPostNotificationRequest:", "B24@0:8@16", NC_BOOL1, "_shouldPostNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "_readyToReceiveForNotificationRequest:", "B24@0:8@16", NC_BOOL1, "_readyToReceiveForNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "_postNotificationRequest:", "B24@0:8@16", NC_BOOL1, "_postNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNotificationBannerDestination", "_canReceiveNotificationRequestIfLocked:", "B24@0:8@16", NC_BOOL1, "_canReceiveNotificationRequestIfLocked:", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNotificationBannerDestination", "_isContentSuppressedForNotificationRequest:", "B24@0:8@16", NC_BOOL1, "_isContentSuppressedForNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNotificationBannerDestination", "_shouldScreenTimeSuppressNotificationRequest:", "B24@0:8@16", NC_BOOL1, "_shouldScreenTimeSuppressNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "_destinations:canReceiveNotificationRequest:", "B32@0:8@16@24", NC_BOOL2, "_destinations:canReceiveNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDispatcher", "registerDestination:", "v24@0:8@16", DEST_VOID1, "registerDestination:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDispatcher", "unregisterDestination:", "v24@0:8@16", DEST_VOID1, "unregisterDestination:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDispatcher", "setDestination:enabled:", "v28@0:8@16B24", DEST_BOOL, "setDestination:enabled:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDispatcher", "destination:willPresentNotificationRequest:suppressAlerts:", "v36@0:8@16@24B32", PRESENT_BOOL, "destination:willPresentNotificationRequest:suppressAlerts:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDispatcher", "destination:didPresentNotificationRequest:", "v32@0:8@16@24", PRESENT, "destination:didPresentNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "registerDestination:", "v24@0:8@16", DEST_VOID1, "registerDestination:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "unregisterDestination:", "v24@0:8@16", DEST_VOID1, "unregisterDestination:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "setDestination:enabled:", "v28@0:8@16B24", DEST_BOOL, "setDestination:enabled:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "destination:willPresentNotificationRequest:suppressAlerts:", "v36@0:8@16@24B32", PRESENT_BOOL, "destination:willPresentNotificationRequest:suppressAlerts:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "destination:didPresentNotificationRequest:", "v32@0:8@16@24", PRESENT, "destination:didPresentNotificationRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationAlertQueue", "setDestination:ready:", "v28@0:8@16B24", DEST_BOOL, "setDestination:ready:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "registerDestination:", "v24@0:8@16", REG_VOID1, "registerDestination:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "unregisterDestination:", "v24@0:8@16", REG_VOID1, "unregisterDestination:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "unregisterDestinationForIdentifier:", "v24@0:8@16", REG_VOID1, "unregisterDestinationForIdentifier:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "registerDestination:forIdentifier:", "v32@0:8@16@24", REG_REGISTER, "registerDestination:forIdentifier:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "setDestination:enabled:", "v28@0:8@16B24", REG_BOOL, "setDestination:enabled:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "setDestination:ready:", "v28@0:8@16B24", REG_BOOL, "setDestination:ready:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "setDestinationWithIdentifier:enabled:", "v28@0:8@16B24", REG_BOOL, "setDestinationWithIdentifier:enabled:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "hasActiveDestinationsForRequest:", "B24@0:8@16", NC_BOOL1, "hasActiveDestinationsForRequest:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "destinationsForRequestDestinations:", "@24@0:8@16", REG_RESOLVE, "destinationsForRequestDestinations:", NO, NULL, NULL, NO, NO},
    {P_SB, "NCNotificationDestinationsRegistry", "readyDestinationsForRequestDestinations:", "@24@0:8@16", REG_RESOLVE, "readyDestinationsForRequestDestinations:", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNCNotificationDispatcher", "_setupNewDestinationsForDispatcher:", "v24@0:8@16", SB_SETUP, "_setupNewDestinationsForDispatcher:", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNCNotificationDispatcher", "_updateActiveDestinations", "v16@0:8", SB_UPDATE, "_updateActiveDestinations", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNCSoundController", "playSoundIfPossibleForNotificationRequest:presentingDestination:", "v32@0:8@16@24", SOUND_IF, "playSoundIfPossibleForNotificationRequest:presentingDestination:", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNCSoundController", "_isDeviceUILocked", "B16@0:8", BOOL0, "_isDeviceUILocked", NO, NULL, NULL, NO, NO},
    {P_SB, "SBNotificationBannerDestination", "presentableWillNotAppearAsBanner:withReason:", "v32@0:8@16@24", REFUSED, "banner-refused", NO, NULL, NULL, NO, NO},
    {P_SB | P_MEDIA, "TLAlertSystemSoundController", "_playTaskDescriptorForAlert:withSound:alertSystemSoundContext:", "@40@0:8@16@24@32", TONE_TASK, "tone-task", NO, NULL, NULL, NO, NO},
    {P_SB | P_MEDIA, "TLAlertSystemSoundController", "_didCompletePlaybackForAlert:", "v24@0:8@16", VOID1, "_didCompletePlaybackForAlert:", NO, NULL, NULL, NO, NO},
    {P_SB | P_MEDIA, "TLAlertSystemSoundController", "_processPlaybackCompletionContexts:", "v24@0:8@16", TONE_COMPLETIONS, "_processPlaybackCompletionContexts:", NO, NULL, NULL, NO, NO},
    {P_SB | P_MEDIA, "TLAlertSystemSoundController", "_removeAlert:alertSystemSoundContext:didFailToPrepareSound:appendingPlaybackCompletionContextToArray:", "v44@0:8@16@24B32@36", TONE_REMOVE, "tone-remove", NO, NULL, NULL, NO, NO},
    {P_SB | P_MEDIA, "TLAlertSystemSoundController", "_componentSuppressionFlagsForAlert:", "I24@0:8@16", TONE_FLAGS, "tone-components", YES, NULL, NULL, NO, NO},
    {P_SB | P_MEDIA, "TLAlertSystemSoundController", "_vibrationPatternForAlert:withSound:", "@32@0:8@16@24", TONE_VIBRATION, "tone-vibration", YES, NULL, NULL, NO, NO},
    {P_SB | P_MEDIA, "TLAlertSystemSoundContext", "setHasPlaybackStarted:", "v20@0:8B16", STATE_BOOL, "setHasPlaybackStarted:", NO, NULL, NULL, NO, NO},
    {P_SB | P_MEDIA, "TLAlertSystemSoundContext", "setLoadingSound:", "v20@0:8B16", STATE_BOOL, "setLoadingSound:", NO, NULL, NULL, NO, NO},
    {P_SB | P_MEDIA, "TLAlertSystemSoundContext", "setBeingInterrupted:", "v20@0:8B16", STATE_BOOL, "setBeingInterrupted:", NO, NULL, NULL, NO, NO},
};

static NSArray *members(id collection)
{
    if ([collection isKindOfClass:[NSDictionary class]]) return [collection allValues];
    if ([collection isKindOfClass:[NSSet class]]) return [collection allObjects];
    if ([collection isKindOfClass:[NSArray class]]) return collection;
    return nil;
}

static void registry_state(id registry)
{
    if (!registry) return;
    NSArray *all = members(GET(id, registry, "destinations"));
    NSArray *registered = members(GET(id, registry, "registeredDestinations"));
    NSArray *ready = members(GET(id, registry, "readyDestinations"));
    NSArray *active = members(GET(id, registry, "activeDestinations"));
    trace(registry, "registry", "all=%lu registered=%lu ready=%lu active=%lu",
          (unsigned long)all.count, (unsigned long)registered.count,
          (unsigned long)ready.count, (unsigned long)active.count);
    for (id destination in all) {
        char stage[160];
        snprintf(stage, sizeof(stage), "registry-member-%s", object_getClassName(destination));
        trace(registry, stage, "class=%s registered=%d ready=%d active=%d",
              object_getClassName(destination), [registered containsObject:destination],
              [ready containsObject:destination], [active containsObject:destination]);
    }
}

static void sb_destinations(id owner)
{
    trace(owner, "sb-destinations", "banner=%s lockScreen=%s center=%s",
          object_getClassName(GET(id, owner, "bannerDestination")),
          object_getClassName(GET(id, owner, "dashBoardDestination")),
          object_getClassName(GET(id, owner, "notificationCenterDestination")));
}

static void nc_request_state(id request)
{
    trace(request, "nc-options", "destinations=%lu sound=%d canPlay=%d suppress=%d suppression=%lu activeAppSuppress=%d",
          (unsigned long)members(GET(id, request, "requestDestinations")).count,
          GET(id, request, "sound") != nil,
          GET(BOOL, GET(id, request, "options"), "canPlaySound"),
          GET(BOOL, GET(id, request, "alertOptions"), "shouldSuppress"),
          GET(unsigned long, GET(id, request, "alertOptions"), "suppression"),
          GET(BOOL, GET(id, request, "options"), "suppressesAlertsWhenAppIsActive"));
    /* Only allowlisted destination strings become labels; no arbitrary identifier output. */
    NSArray *wanted = members(GET(id, request, "requestDestinations"));
    for (NSString *fixed in @[@"Banner", @"banner", @"LockScreen", @"lockscreen",
                              @"NotificationCenter", @"DashBoard", @"CoverSheet", @"coverSheet"]) {
        if ([wanted containsObject:fixed]) trace(request, fixed.UTF8String, "requested=1");
    }
}

static void context_state(id context, const char *stage)
{
    trace(context, stage, "present=%d allowed=%d limited=%d general=%d limitedAllowed=%d state=%lu emergency=%d",
          context != nil, GET(BOOL, context, "allowedByScreenTime"),
          GET(BOOL, context, "applicationCurrentlyLimited"),
          GET(BOOL, context, "shouldBeAllowedDuringGeneralScreenTime"),
          GET(BOOL, context, "shouldBeAllowedByScreenTimeWhenLimited"),
          GET(unsigned long, context, "currentApplicationState"),
          GET(BOOL, context, "emergencyModeEnabled"));
}

/* Runtime signatures are checked before any of these typed wrappers is installed.
 * Opaque completion blocks remain opaque unless their ABI has been verified. */
static IMP make_probe(Probe *p)
{
    SEL sel = sel_registerName(p->sel);
    switch (p->kind) {
    case BOOL0:
        return imp_implementationWithBlock(^BOOL(id self) {
            BOOL result = ((BOOL (*)(id, SEL))p->original)(self, sel);
            trace(self, p->stage, "result=%d", result); return result;
        });
    case BOOL1: case NC_BOOL1:
        return imp_implementationWithBlock(^BOOL(id self, id arg) {
            BOOL result = ((BOOL (*)(id, SEL, id))p->original)(self, sel, arg);
            if (p->kind == BOOL1 || sms_request(arg)) {
                trace(arg ?: self, p->stage, "result=%d", result);
                if (p->kind == NC_BOOL1) nc_request_state(arg);
            }
            return result;
        });
    case BOOL2: case NC_BOOL2:
        return imp_implementationWithBlock(^BOOL(id self, id a, id b) {
            BOOL result = ((BOOL (*)(id, SEL, id, id))p->original)(self, sel, a, b);
            if (p->kind == BOOL2 || sms_request(b))
                trace(b ?: a, p->stage, "result=%d", result);
            return result;
        });
    case BOOL1BOOL:
        return imp_implementationWithBlock(^BOOL(id self, id arg, BOOL flag) {
            BOOL result = ((BOOL (*)(id, SEL, id, BOOL))p->original)(self, sel, arg, flag);
            trace(arg, p->stage, "result=%d contact=%d", result, flag); return result;
        });
    case NC_VOID1: case VOID1:
        return imp_implementationWithBlock(^(id self, id arg) {
            if (p->kind == VOID1 || sms_request(arg)) {
                trace(arg, p->stage, "class=%s", object_getClassName(self));
                if (p->kind == NC_VOID1) nc_request_state(arg);
            }
            ((void (*)(id, SEL, id))p->original)(self, sel, arg);
        });
    case DEST_VOID1: case REG_VOID1:
        return imp_implementationWithBlock(^(id self, id arg) {
            ((void (*)(id, SEL, id))p->original)(self, sel, arg);
            trace(arg ?: self, p->stage, "owner=%s destinationClass=%s", object_getClassName(self), object_getClassName(arg));
            registry_state(p->kind == REG_VOID1 ? self : GET(id, self, "destinationsRegistry"));
        });
    case DEST_BOOL: case REG_BOOL:
        return imp_implementationWithBlock(^(id self, id arg, BOOL flag) {
            ((void (*)(id, SEL, id, BOOL))p->original)(self, sel, arg, flag);
            trace(arg ?: self, p->stage, "owner=%s class=%s value=%d", object_getClassName(self), object_getClassName(arg), flag);
            registry_state(p->kind == REG_BOOL ? self : GET(id, self, "destinationsRegistry"));
        });
    case REG_REGISTER:
        return imp_implementationWithBlock(^(id self, id dest, id identifier) {
            ((void (*)(id, SEL, id, id))p->original)(self, sel, dest, identifier);
            trace(dest, "destination-registered", "class=%s", object_getClassName(dest));
            registry_state(self);
        });
    case REG_RESOLVE:
        return imp_implementationWithBlock(^id(id self, id requested) {
            id result = ((id (*)(id, SEL, id))p->original)(self, sel, requested);
            trace(self, p->stage, "requested=%lu resolved=%lu", (unsigned long)members(requested).count,
                  (unsigned long)members(result).count);
            for (id dest in members(result))
                trace(dest, p->stage, "class=%s", object_getClassName(dest));
            registry_state(self); return result;
        });
    case PRESENT:
        return imp_implementationWithBlock(^(id self, id dest, id request) {
            if (sms_request(request)) trace(request, p->stage, "owner=%s destination=%s", object_getClassName(self), object_getClassName(dest));
            ((void (*)(id, SEL, id, id))p->original)(self, sel, dest, request);
        });
    case PRESENT_BOOL:
        return imp_implementationWithBlock(^(id self, id dest, id request, BOOL suppressed) {
            if (sms_request(request)) trace(request, p->stage, "owner=%s destination=%s suppress=%d", object_getClassName(self), object_getClassName(dest), suppressed);
            ((void (*)(id, SEL, id, id, BOOL))p->original)(self, sel, dest, request, suppressed);
        });
    case SB_SETUP:
        return imp_implementationWithBlock(^(id self, id dispatcher) {
            ((void (*)(id, SEL, id))p->original)(self, sel, dispatcher);
            sb_destinations(self);
            registry_state(GET(id, dispatcher, "destinationsRegistry"));
        });
    case SB_UPDATE:
        return imp_implementationWithBlock(^(id self) {
            ((void (*)(id, SEL))p->original)(self, sel); sb_destinations(self);
        });
    case CONVERSATION:
        return imp_implementationWithBlock(^id(id self, id handles) {
            id result = ((id (*)(id, SEL, id))p->original)(self, sel, handles);
            trace(self, "st-initial-state", "application=%d emergency=%d policy=%d state=%lu emergencyOn=%d",
                  GET(BOOL, self, "didFetchInitialApplicationState"), GET(BOOL, self, "didFetchInitialEmergencyModeState"),
                  GET(BOOL, self, "didFetchInitialScreenTimePolicyState"), GET(unsigned long, self, "currentApplicationState"),
                  GET(BOOL, self, "emergencyModeEnabled"));
            context_state(result, p->stage); return result;
        });
    case ST_UPDATE:
        return imp_implementationWithBlock(^(id self, BOOL general, BOOL limited, unsigned long state, BOOL emergency) {
            ((void (*)(id, SEL, BOOL, BOOL, unsigned long, BOOL))p->original)(self, sel, general, limited, state, emergency);
            context_state(self, p->stage);
        });
    case OBJECT_ERROR: case FOCUS_STATE:
        return imp_implementationWithBlock(^id(id self, NSError **error) {
            id result = ((id (*)(id, SEL, NSError **))p->original)(self, sel, error);
            if (p->kind == FOCUS_STATE)
                trace(self, p->stage, "present=%d active=%d suppress=%d error=%ld", result != nil,
                      GET(BOOL, result, "isActive"), GET(BOOL, result, "willSuppressInterruptions"), !result && error ? (long)(*error).code : 0L);
            else trace(self, p->stage, "present=%d class=%s error=%ld", result != nil,
                       object_getClassName(result), !result && error ? (long)(*error).code : 0L);
            return result;
        });
    case FOCUS_EVENT:
        return imp_implementationWithBlock(^id(id self, id details, NSError **error) {
            id result = ((id (*)(id, SEL, id, NSError **))p->original)(self, sel, details, error);
            trace(details, p->stage, "present=%d suppression=%lu reason=%lu error=%ld", result != nil,
                  GET(unsigned long, result, "interruptionSuppression"), GET(unsigned long, result, "resolutionReason"),
                  !result && error ? (long)(*error).code : 0L); return result;
        });
    case FOCUS_UPDATE:
        return imp_implementationWithBlock(^(id self, id service, id update) {
            id state = GET(id, update, "state");
            trace(self, p->stage, "present=%d active=%d suppress=%d reason=%lu", state != nil,
                  GET(BOOL, state, "isActive"), GET(BOOL, state, "willSuppressInterruptions"), GET(unsigned long, update, "reason"));
            ((void (*)(id, SEL, id, id))p->original)(self, sel, service, update);
        });
    case RECEIVE:
        return imp_implementationWithBlock(^(id self, id message, id chat, unsigned char style, id account, id ids) {
            trace(message, p->stage, "received=1");
            ((void (*)(id, SEL, id, id, unsigned char, id, id))p->original)(self, sel, message, chat, style, account, ids);
        });
    case IM_STORE:
        return imp_implementationWithBlock(^id(id self, id message, BOOL replace, BOOL modifyError, BOOL modifyFlags,
                                                unsigned long mask, BOOL cache, BOOL unread, BOOL reindex) {
            id result = ((id (*)(id, SEL, id, BOOL, BOOL, BOOL, unsigned long, BOOL, BOOL, BOOL))p->original)(
                self, sel, message, replace, modifyError, modifyFlags, mask, cache, unread, reindex);
            trace(message, p->stage, "stored=%d", result != nil); return result;
        });
    case IM_BATCH:
        return imp_implementationWithBlock(^(id self, id records, long long date, BOOL urgent, BOOL carousel, BOOL active) {
            note("imd-register-batch count=%lu urgent=%d carousel=%d mostActive=%d\n",
                 (unsigned long)[records count], urgent, carousel, active);
            ((void (*)(id, SEL, id, long long, BOOL, BOOL, BOOL))p->original)(self, sel, records, date, urgent, carousel, active);
        });
    case STORE_DECISION:
        return imp_implementationWithBlock(^BOOL(id self, NSError *error) {
            BOOL result = ((BOOL (*)(id, SEL, id))p->original)(self, sel, error);
            trace(self, p->stage, "error=%ld result=%d", (long)error.code, result); return result;
        });
    case STORE_DESTROY:
        return imp_implementationWithBlock(^id(id self, id description) {
            store_state("before-destroy"); removal_trace();
            id result = ((id (*)(id, SEL, id))p->original)(self, sel, description);
            trace(self, p->stage, "resultPresent=%d class=%s", result != nil, object_getClassName(result)); store_state("after-destroy"); return result;
        });
    case RESOLVE:
        return imp_implementationWithBlock(^id(id self, id request, id bundle, NSError **error) {
            id result = ((id (*)(id, SEL, id, id, NSError **))p->original)(self, sel, request, bundle, error);
            if (sms(bundle)) trace(request, p->stage, "present=%d error=%ld", result != nil, !result && error ? (long)(*error).code : 0L);
            return result;
        });
    case SAVE_REQUEST:
        return imp_implementationWithBlock(^(id self, id request, BOOL repost, id date, id bundle, id completion) {
            if (sms(bundle)) trace(request, p->stage, "repost=%d destinations=%lu", repost, GET(unsigned long, request, "destinations"));
            ((void (*)(id, SEL, id, BOOL, id, id, id))p->original)(self, sel, request, repost, date, bundle, completion);
        });
    case SAVE_RECORD:
        return imp_implementationWithBlock(^(id self, id record, BOOL repost, id bundle, id completion) {
            if (sms(bundle)) trace(record, p->stage, "repost=%d hasAlert=%d presentAlert=%d hasSound=%d playSound=%d", repost,
                GET(BOOL, record, "hasAlertContent"), GET(BOOL, record, "shouldPresentAlert"),
                GET(BOOL, record, "hasSound"), GET(BOOL, record, "shouldPlaySound"));
            ((void (*)(id, SEL, id, BOOL, id, id))p->original)(self, sel, record, repost, bundle, completion);
        });
    case SAVE_RECORD_QUEUE:
        return imp_implementationWithBlock(^(id self, id record, BOOL repost, unsigned long options, id bundle) {
            if (sms(bundle)) trace(record, p->stage, "repost=%d options=%lu", repost, options);
            ((void (*)(id, SEL, id, BOOL, unsigned long, id))p->original)(self, sel, record, repost, options, bundle);
        });
    case OBSERVERS:
        return imp_implementationWithBlock(^(id self, id added, id replaced, id replacements, id removed, BOOL repost, id bundle) {
            if (sms(bundle)) for (id record in added) trace(record, p->stage, "added=1 repost=%d", repost);
            ((void (*)(id, SEL, id, id, id, id, BOOL, id))p->original)(self, sel, added, replaced, replacements, removed, repost, bundle);
        });
    case DELIVERED_SERVER:
        return imp_implementationWithBlock(^(id self, id bundle, id completion) {
            if (sms(bundle)) note("server-delivered-query\n");
            ((void (*)(id, SEL, id, id))p->original)(self, sel, bundle, completion);
        });
    case BB_PUBLISH:
        return imp_implementationWithBlock(^(id self, id bulletin, unsigned long destinations) {
            if (sms(GET(id, bulletin, "sectionID"))) trace(bulletin, p->stage, "destinations=%lu sound=%d level=%lu activeSuppress=%d", destinations,
                GET(id, bulletin, "sound") != nil, GET(unsigned long, bulletin, "interruptionLevel"), GET(BOOL, bulletin, "suppressesAlertsWhenAppIsActive"));
            ((void (*)(id, SEL, id, unsigned long))p->original)(self, sel, bulletin, destinations);
        });
    case BB_PUBLISH_SECTION:
        return imp_implementationWithBlock(^(id self, id bulletin, id sectionID, unsigned long destinations) {
            if (sms(sectionID)) trace(bulletin, p->stage, "destinations=%lu", destinations);
            ((void (*)(id, SEL, id, id, unsigned long))p->original)(self, sel, bulletin, sectionID, destinations);
        });
    case SOUND_IF:
        return imp_implementationWithBlock(^(id self, id request, id destination) {
            if (sms_request(request)) { trace(request, p->stage, "destination=%s", object_getClassName(destination)); presentation_state(); }
            ((void (*)(id, SEL, id, id))p->original)(self, sel, request, destination);
        });
    case REFUSED:
        return imp_implementationWithBlock(^(id self, id presentable, id reason) {
            trace(presentable, p->stage, "reasonPresent=%d reasonClass=%s", reason != nil, object_getClassName(reason));
            ((void (*)(id, SEL, id, id))p->original)(self, sel, presentable, reason);
        });
    case TONE_TASK:
        return imp_implementationWithBlock(^id(id self, id alert, id soundObject, id context) {
            id result = ((id (*)(id, SEL, id, id, id))p->original)(self, sel, alert, soundObject, context);
            trace(alert, p->stage, "task=%d sound=%d loading=%d started=%d interrupted=%d", result != nil, soundObject != nil,
                GET(BOOL, context, "isLoadingSound"), GET(BOOL, context, "hasPlaybackStarted"), GET(BOOL, context, "isBeingInterrupted"));
            return result;
        });
    case TONE_REMOVE:
        return imp_implementationWithBlock(^(id self, id alert, id context, BOOL failed, id completions) {
            trace(alert, p->stage, "prepareFailed=%d started=%d interrupted=%d", failed,
                  GET(BOOL, context, "hasPlaybackStarted"), GET(BOOL, context, "isBeingInterrupted"));
            ((void (*)(id, SEL, id, id, BOOL, id))p->original)(self, sel, alert, context, failed, completions);
        });
    case TONE_COMPLETIONS:
        return imp_implementationWithBlock(^(id self, id contexts) {
            for (id context in contexts) trace(context, p->stage, "type=%ld error=%ld", GET(long, context, "playbackCompletionType"),
                  (long)[GET(id, context, "error") code]);
            ((void (*)(id, SEL, id))p->original)(self, sel, contexts);
        });
    case TONE_FLAGS:
        return imp_implementationWithBlock(^unsigned int(id self, id alert) {
            unsigned int flags = ((unsigned int (*)(id, SEL, id))p->original)(self, sel, alert);
            trace(alert, p->stage, "suppression=%u", flags); return flags;
        });
    case TONE_VIBRATION:
        return imp_implementationWithBlock(^id(id self, id alert, id soundObject) {
            id result = ((id (*)(id, SEL, id, id))p->original)(self, sel, alert, soundObject);
            trace(alert, p->stage, "pattern=%d", result != nil); return result;
        });
    case STATE_BOOL:
        return imp_implementationWithBlock(^(id self, BOOL flag) {
            ((void (*)(id, SEL, BOOL))p->original)(self, sel, flag);
            trace(self, p->stage, "value=%d", flag);
        });
    }
}

static void install_hook(Hook *h)
{
    if (h->installed || h->wrong_type) return;
    Class cls = objc_getClass(h->cls);
    SEL sel = sel_registerName(h->sel);
    Method method = class_getInstanceMethod(cls, sel);
    if (!method) return;
    if (strcmp(method_getTypeEncoding(method), h->types)) {
        h->wrong_type = YES;
        note("hook-type-mismatch %s %s\n", h->cls, h->sel); return;
    }
    *h->original = method_getImplementation(method);
    /* Isolate inherited implementations instead of replacing a superclass. */
    if (!class_addMethod(cls, sel, h->replacement, h->types)) method_setImplementation(method, h->replacement);
    h->installed = YES;
    note("hook-installed %s %s\n", h->cls, h->sel);
}

/* dyld add-image callback: objc_addLoadImageFunc runs its callback under the
 * ObjC runtimeLock, so class_getInstanceMethod there recursively locks and traps. */
static void image_loaded(const struct mach_header *header, intptr_t slide)
{
    (void)header; (void)slide;
    for (size_t i = 0; i < legacy_count; i++) install_hook(&legacy[i]);
    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        Probe *p = &probes[i];
        if (!(p->processes & process_mask) || p->installed || p->wrong_type) continue;
        Class cls = objc_getClass(p->cls);
        if (p->meta) cls = object_getClass(cls);
        Hook h = {p->cls, p->sel, p->types, p->replacement, &p->original, NO, NO};
        /* Class-method entries use the metaclass, not an instance method with the same name. */
        if (p->meta) {
            Method m = class_getInstanceMethod(cls, sel_registerName(p->sel));
            if (!m) continue;
            if (strcmp(method_getTypeEncoding(m), p->types)) {
                p->wrong_type = YES; note("hook-type-mismatch +%s %s\n", p->cls, p->sel); continue;
            }
            p->original = method_getImplementation(m);
            method_setImplementation(m, p->replacement);
            p->installed = YES; note("hook-installed +%s %s\n", p->cls, p->sel);
        } else {
            install_hook(&h); p->installed = h.installed; p->wrong_type = h.wrong_type;
        }
    }
}

static void audit_hooks(void)
{
    for (size_t i = 0; i < legacy_count; i++) if (!legacy[i].installed)
        note("hook-unavailable %s %s typeMismatch=%d\n", legacy[i].cls, legacy[i].sel, legacy[i].wrong_type);
    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        Probe *p = &probes[i];
        if ((p->processes & process_mask) && !p->installed)
            note("hook-unavailable %s %s typeMismatch=%d\n", p->cls, p->sel, p->wrong_type);
    }
    note("hook-audit-complete\n");
}

/* Calls inside the shared cache branch directly to AudioToolbox/libxpc entries;
 * dyld interposition alone misses them. Relocate ONLY these audited 20F66 arm64
 * non-PC-relative prologues, using private COW text and an RX trampoline. */
static void absolute_branch(void *where, const void *target)
{
    const uint32_t branch[] = {0x58000050, 0xd61f0200};
    memcpy(where, branch, sizeof(branch));
    memcpy((char *)where + 8, &target, sizeof(target));
}

static void entry_hook(void *library, const char *name, const uint32_t prologue[4], void *replacement, void **original)
{
    void *entry = dlsym(library, name);
    if (!entry || memcmp(entry, prologue, 16)) {
        note("c-hook-unavailable %s prologueMismatch=%d\n", name, entry != NULL); return;
    }
    vm_address_t trampoline = 0;
    kern_return_t kr = vm_allocate(mach_task_self(), &trampoline, vm_page_size, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) { note("c-hook-failed %s allocate=%d\n", name, kr); return; }
    memcpy((void *)trampoline, prologue, 16);
    absolute_branch((void *)(trampoline + 16), (char *)entry + 16);
    kr = vm_protect(mach_task_self(), trampoline, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) goto fail;
    sys_icache_invalidate((void *)trampoline, 32);
    vm_address_t page = (vm_address_t)entry & ~((vm_address_t)vm_page_size - 1);
    vm_size_t size = (((vm_address_t)entry + 16 - page + vm_page_size - 1) / vm_page_size) * vm_page_size;
    kr = vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) goto fail;
    *original = (void *)trampoline;
    absolute_branch(entry, replacement);
    kr = vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        note("c-hook-failed %s restoreRX=%d\n", name, kr); _exit(1);
    }
    sys_icache_invalidate(entry, 16);
    note("c-hook-installed %s\n", name); return;
fail:
    note("c-hook-failed %s protect=%d\n", name, kr);
    vm_deallocate(mach_task_self(), trampoline, vm_page_size);
}

static void (*audio_play_original)(uint32_t, CFDictionaryRef, void (^)(void));
static void (*audio_stop_original)(uint32_t, uint32_t);
static int32_t (*audio_create_original)(CFDictionaryRef, uint32_t *);
static unsigned long audio_event;
static void audio_play(uint32_t soundID, CFDictionaryRef options, void (^completion)(void))
{
    unsigned long event = __atomic_add_fetch(&audio_event, 1, __ATOMIC_RELAXED);
    NSDictionary *opts = (NSDictionary *)options;
    note("audio-play event=%lu soundID=%u vibration=%d behavior=%ld flags=%lu loop=%d completion=%d volumePresent=%d volume=%.3g ignoreRinger=%d\n",
         event, soundID, opts[@"PlaySystemSoundOption_VibrationPattern"] != nil,
         (long)[opts[@"PlaySystemSoundOption_Behavior"] integerValue],
         (unsigned long)[opts[@"PlaySystemSoundOption_Flags"] unsignedIntegerValue],
         [opts[@"PlaySystemSoundOption_Loop"] boolValue], completion != nil,
         opts[@"PlaySystemSoundOption_Volume"] != nil, [opts[@"PlaySystemSoundOption_Volume"] doubleValue],
         [opts[@"PlaySystemSoundOption_IgnoreRingerSwitch"] boolValue]);
    /* Preserve a nil completion: it affects the server's completion-token path. */
    if (!completion) { audio_play_original(soundID, options, nil); return; }
    audio_play_original(soundID, options, ^{
        note("audio-complete event=%lu soundID=%u\n", event, soundID); completion();
    });
}

static void audio_stop(uint32_t soundID, uint32_t immediately)
{
    note("audio-stop soundID=%u immediately=%u\n", soundID, immediately);
    audio_stop_original(soundID, immediately);
}

static int32_t audio_create(CFDictionaryRef options, uint32_t *soundID)
{
    int32_t status = audio_create_original(options, soundID);
    note("audio-create status=%d soundID=%u\n", status, status == 0 ? *soundID : 0);
    return status;
}

typedef struct SoundPeer {
    uintptr_t connection;
    unsigned long ordinal;
    char last[4][480];
    struct SoundPeer *next;
} SoundPeer;
static SoundPeer *sound_peers;
static unsigned long next_peer;
static pthread_mutex_t peer_lock = PTHREAD_MUTEX_INITIALIZER;

static BOOL sound_connection(xpc_connection_t connection)
{
    const char *name = xpc_connection_get_name(connection);
    if (name && !strcmp(name, "com.apple.audio.SystemSoundServer-iOS")) return YES;
    BOOL found = NO;
    pthread_mutex_lock(&peer_lock);
    for (SoundPeer *p = sound_peers; p; p = p->next)
        if (p->connection == (uintptr_t)connection) { found = YES; break; }
    pthread_mutex_unlock(&peer_lock); return found;
}

static void add_sound_peer(xpc_connection_t connection)
{
    pthread_mutex_lock(&peer_lock);
    for (SoundPeer *p = sound_peers; p; p = p->next) {
        if (p->connection == (uintptr_t)connection) { pthread_mutex_unlock(&peer_lock); return; }
    }
    SoundPeer *p = calloc(1, sizeof(*p));
    p->connection = (uintptr_t)connection; p->ordinal = ++next_peer;
    p->next = sound_peers; sound_peers = p;
    unsigned long ordinal = p->ordinal;
    pthread_mutex_unlock(&peer_lock);
    note("systemsound-peer peer=%lu\n", ordinal);
}

static void remove_sound_peer(xpc_connection_t connection)
{
    pthread_mutex_lock(&peer_lock);
    SoundPeer **at = &sound_peers;
    while (*at) {
        SoundPeer *p = *at;
        if (p->connection == (uintptr_t)connection) { *at = p->next; free(p); break; }
        at = &p->next;
    }
    pthread_mutex_unlock(&peer_lock);
}

/* Return a fixed type tag and a numeric value only. No XPC string/data/description
 * can reach the logger, including bundle IDs, URLs, tone names or client metadata. */
static const char *xpc_number(xpc_object_t object, long long *value)
{
    *value = 0;
    if (!object) return "absent";
    xpc_type_t t = xpc_get_type(object);
    if (t == XPC_TYPE_INT64) { *value = xpc_int64_get_value(object); return "int"; }
    if (t == XPC_TYPE_UINT64) { *value = (long long)xpc_uint64_get_value(object); return "uint"; }
    if (t == XPC_TYPE_BOOL) { *value = xpc_bool_get_value(object); return "bool"; }
    return "opaque";
}

static void sound_xpc_preferences(xpc_object_t message, char *output, size_t size)
{
    const char *keys[] = {"kSSXPCMessageKey_Prefs_Volume", "kSSXPCMessageKey_Prefs_AlertAudioOn",
                          "kSSXPCMessageKey_Prefs_UIAudioOn", "kSSXPCMessageKey_Prefs_AlertVisualOn",
                          "kSSXPCMessageKey_ShouldCompletePlayback", "kSSXPCMessageKey_StopImmediately"};
    size_t at = 0;
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        xpc_object_t value = xpc_dictionary_get_value(message, keys[i]);
        long long number;
        const char *type = xpc_number(value, &number);
        if (value && xpc_get_type(value) == XPC_TYPE_DOUBLE)
            at += snprintf(output + at, size - at, " p%zu=double:%.3g", i, xpc_double_get_value(value));
        else at += snprintf(output + at, size - at, " p%zu=%s:%lld", i, type, number);
    }
}

static void sound_xpc_message(xpc_connection_t connection, unsigned phase, xpc_object_t message)
{
    char value[480];
    xpc_type_t type = xpc_get_type(message);
    if (type == XPC_TYPE_DICTIONARY) {
        long long action, actionID, completion, activate;
        const char *ta = xpc_number(xpc_dictionary_get_value(message, "kSSXPCMessageKey_Action"), &action);
        const char *ti = xpc_number(xpc_dictionary_get_value(message, "kSSXPCMessageKey_ActionID"), &actionID);
        const char *tc = xpc_number(xpc_dictionary_get_value(message, "kSSXPCMessageKey_ClientCompletionToken"), &completion);
        const char *ts = xpc_number(xpc_dictionary_get_value(message, "kSSXPCMessageKey_ServerActivateState"), &activate);
        snprintf(value, sizeof(value), "action=%s:%lld actionID=%s:%lld completion=%s:%lld activate=%s:%lld audio=%d vibration=%d options=%d",
                 ta, action, ti, actionID, tc, completion, ts, activate,
                 xpc_dictionary_get_value(message, "kSSXPCMessageKey_AudioData") != NULL,
                 xpc_dictionary_get_value(message, "kSSXPCMessageKey_VibrationData") != NULL,
                 xpc_dictionary_get_value(message, "kSSXPCMessageKey_ClientPlayOptions") != NULL);
    } else if (type == XPC_TYPE_ERROR) {
        int error = message == XPC_ERROR_CONNECTION_INTERRUPTED ? 1 :
                    message == XPC_ERROR_CONNECTION_INVALID ? 2 :
                    message == XPC_ERROR_TERMINATION_IMMINENT ? 3 : 4;
        snprintf(value, sizeof(value), "error=%d", error);
    } else {
        long long number; const char *tag = xpc_number(message, &number);
        snprintf(value, sizeof(value), "scalar=%s:%lld", tag, number);
    }
    if (type == XPC_TYPE_DICTIONARY) {
        size_t at = strlen(value);
        sound_xpc_preferences(message, value + at, sizeof(value) - at);
    }
    unsigned long ordinal = 0; BOOL changed = NO;
    pthread_mutex_lock(&peer_lock);
    for (SoundPeer *p = sound_peers; p; p = p->next) {
        if (p->connection != (uintptr_t)connection) continue;
        ordinal = p->ordinal; changed = strcmp(p->last[phase], value) != 0;
        if (changed) strlcpy(p->last[phase], value, sizeof(p->last[phase]));
        break;
    }
    pthread_mutex_unlock(&peer_lock);
    if (changed) note("systemsound-xpc peer=%lu phase=%u %s\n", ordinal, phase, value);
}

static void (*xpc_handler_original)(xpc_connection_t, xpc_handler_t);
static void (*xpc_send_original)(xpc_connection_t, xpc_object_t);
static void (*xpc_reply_original)(xpc_connection_t, xpc_object_t, dispatch_queue_t, xpc_handler_t);
static xpc_object_t (*xpc_sync_original)(xpc_connection_t, xpc_object_t);

static void sound_xpc_handler(xpc_connection_t connection, xpc_handler_t handler)
{
    if (!sound_connection(connection) || !handler) { xpc_handler_original(connection, handler); return; }
    add_sound_peer(connection);
    /* Capture a number rather than retaining the connection in its own handler. */
    uintptr_t address = (uintptr_t)connection;
    xpc_handler_original(connection, ^(xpc_object_t event) {
        xpc_connection_t peer = (xpc_connection_t)address;
        if (xpc_get_type(event) == XPC_TYPE_CONNECTION) add_sound_peer((xpc_connection_t)event);
        else sound_xpc_message(peer, 0, event);
        handler(event);
        if (event == XPC_ERROR_CONNECTION_INVALID) remove_sound_peer(peer);
    });
}

static void sound_xpc_send(xpc_connection_t connection, xpc_object_t message)
{
    if (sound_connection(connection)) { add_sound_peer(connection); sound_xpc_message(connection, 1, message); }
    xpc_send_original(connection, message);
}

static void sound_xpc_reply(xpc_connection_t connection, xpc_object_t message, dispatch_queue_t queue, xpc_handler_t handler)
{
    if (!sound_connection(connection) || !handler) { xpc_reply_original(connection, message, queue, handler); return; }
    add_sound_peer(connection); sound_xpc_message(connection, 1, message);
    uintptr_t address = (uintptr_t)connection;
    xpc_reply_original(connection, message, queue, ^(xpc_object_t reply) {
        sound_xpc_message((xpc_connection_t)address, 2, reply); handler(reply);
    });
}

/* A synchronous XPC call blocks its sender until the service answers, so a service launchd has to
 * start first appears here as the whole hitch: 444 ms to bring up com.apple.wifi.ThreeBarsXPCService
 * inside SpringBoard's 684 ms turn (work/v29i/INVESTIGATION-sb-launch-freeze.md). Name the
 * connection, the wait and who waited, at most one line per name per 10 s, with the number of slow
 * calls since that line. The name is the Mach service label; never message content. */
#define XPC_SLOW_US 20000
#define XPC_SLOW_NAMES 8
#define XPC_SLOW_QUIET_US 10000000ULL

typedef struct {
    char name[64];
    uint64_t last_us;   /* when this name last had a line (0: never) */
    unsigned repeats;   /* slow calls of this name since that line */
} SlowXpc;
static SlowXpc slow_xpc[XPC_SLOW_NAMES];
static unsigned slow_xpc_next;
static pthread_mutex_t slow_xpc_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t monotonic_us(void)
{
    static mach_timebase_info_data_t tb;

    if (tb.denom == 0) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

/* The first frame up the stack that is neither this library's nor libxpc's: whoever waited. Its
 * image and offset always, its symbol too where the shared cache exports one. */
static void xpc_caller(char *out, size_t size, char *symbol, size_t symbol_size)
{
    void *frames[16];
    int n = backtrace(frames, 16);

    out[0] = 0;
    symbol[0] = 0;
    for (int i = 1; i < n; i++) {
        Dl_info info;
        const char *image;

        if (dladdr(frames[i], &info) == 0 || info.dli_fname == NULL) continue;
        image = strrchr(info.dli_fname, '/');
        image = image ? image + 1 : info.dli_fname;
        if (!strcmp(image, "libhakios.notif.dylib") || !strcmp(image, "libxpc.dylib")) continue;
        snprintf(out, size, "%s+%#lx", image, (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase));
        /* iOS 16.5's shared cache answers "<redacted>" for a private symbol: no symbol either. */
        if (info.dli_sname && info.dli_sname[0] && strcmp(info.dli_sname, "<redacted>"))
            snprintf(symbol, symbol_size, "%s", info.dli_sname);
        return;
    }
    snprintf(out, size, "-");
}

static void xpc_slow_report(xpc_connection_t connection, uint64_t us)
{
    char caller[160], symbol[160], held[sizeof(slow_xpc[0].name)];
    const char *name;
    unsigned slot = XPC_SLOW_NAMES, repeats;
    uint64_t now;

    if (us < XPC_SLOW_US) return;
    name = xpc_connection_get_name(connection);
    if (name == NULL) name = "-";
    now = monotonic_us();
    pthread_mutex_lock(&slow_xpc_lock);
    for (unsigned i = 0; i < XPC_SLOW_NAMES; i++)
        if (slow_xpc[i].name[0] && !strcmp(slow_xpc[i].name, name)) { slot = i; break; }
    if (slot == XPC_SLOW_NAMES) {
        slot = slow_xpc_next++ % XPC_SLOW_NAMES;
        strlcpy(slow_xpc[slot].name, name, sizeof(slow_xpc[slot].name));
        slow_xpc[slot].last_us = 0;
        slow_xpc[slot].repeats = 0;
    }
    slow_xpc[slot].repeats++;
    if (slow_xpc[slot].last_us != 0 && now - slow_xpc[slot].last_us < XPC_SLOW_QUIET_US) {
        pthread_mutex_unlock(&slow_xpc_lock);
        return;
    }
    repeats = slow_xpc[slot].repeats;
    slow_xpc[slot].last_us = now;
    slow_xpc[slot].repeats = 0;
    strlcpy(held, slow_xpc[slot].name, sizeof(held));
    pthread_mutex_unlock(&slow_xpc_lock);
    xpc_caller(caller, sizeof(caller), symbol, sizeof(symbol));
    if (symbol[0]) note("xpc-sync ms=%llu name=%s caller=%s caller-sym=%s repeats=%u\n",
                        us / 1000, held, caller, symbol, repeats);
    else note("xpc-sync ms=%llu name=%s caller=%s repeats=%u\n", us / 1000, held, caller, repeats);
}

static xpc_object_t sound_xpc_sync(xpc_connection_t connection, xpc_object_t message)
{
    BOOL tracked = sound_connection(connection);
    if (tracked) { add_sound_peer(connection); sound_xpc_message(connection, 1, message); }
    uint64_t began = monotonic_us();
    xpc_object_t result = xpc_sync_original(connection, message);
    uint64_t waited = monotonic_us() - began;
    if (tracked) sound_xpc_message(connection, 3, result);
    xpc_slow_report(connection, waited);
    return result;
}

static void install_audio_probes(void)
{
    /* Entry instructions verified from the raw 20F66 arm64 cache. */
    const uint32_t handler[] = {0xa9be4ff4,0xa9017bfd,0x910043fd,0xaa0003f3};
    const uint32_t send[] = {0xa9bd57f6,0xa9014ff4,0xa9027bfd,0x910083fd};
    const uint32_t reply[] = {0xa9bb67fa,0xa9015ff8,0xa90257f6,0xa9034ff4};
    const uint32_t sync[] = {0xd10143ff,0xa9015ff8,0xa90257f6,0xa9034ff4};
    entry_hook(RTLD_DEFAULT, "xpc_connection_set_event_handler", handler, (void *)sound_xpc_handler, (void **)&xpc_handler_original);
    entry_hook(RTLD_DEFAULT, "xpc_connection_send_message", send, (void *)sound_xpc_send, (void **)&xpc_send_original);
    entry_hook(RTLD_DEFAULT, "xpc_connection_send_message_with_reply", reply, (void *)sound_xpc_reply, (void **)&xpc_reply_original);
    entry_hook(RTLD_DEFAULT, "xpc_connection_send_message_with_reply_sync", sync, (void *)sound_xpc_sync, (void **)&xpc_sync_original);
    void *audio = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW | RTLD_LOCAL);
    note("audio-framework loaded=%d\n", audio != NULL);
    if (!audio) return;
    const uint32_t play[] = {0xd10303ff,0xa9085ff8,0xa90957f6,0xa90a4ff4};
    const uint32_t stop[] = {0xd101c3ff,0xa9035ff8,0xa90457f6,0xa9054ff4};
    const uint32_t create[] = {0xd10343ff,0xa9095ff8,0xa90a57f6,0xa90b4ff4};
    entry_hook(audio, "AudioServicesPlaySystemSoundWithOptions", play, (void *)audio_play, (void **)&audio_play_original);
    entry_hook(audio, "AudioServicesStopSystemSound", stop, (void *)audio_stop, (void **)&audio_stop_original);
    entry_hook(audio, "AudioServicesCreateSystemSoundIDWithOptions", create, (void *)audio_create, (void **)&audio_create_original);
}

/* Parent vnode events observe root removal/replacement even when the remover
 * uses POSIX filesystem calls. The logged path is always this fixed store. */
static void watch_store(void)
{
    const char *parent = "/var/mobile/Library/Application Support";
    int fd = open(parent, O_EVTONLY);
    if (fd < 0) { note("store-watch unavailable errno=%d\n", errno); return; }
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, fd,
        DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_REVOKE,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_event_handler(source, ^{ @autoreleasepool { store_state("store-vnode"); } });
    dispatch_source_set_cancel_handler(source, ^{ close(fd); });
    dispatch_resume(source);
    note("store-watch installed=1\n"); store_state("store-vnode");
}
__attribute__((constructor)) static void notification_init(void)
{
    console = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (console < 0) console = STDERR_FILENO;
    process_state = [NSObject new];
    const char *name = getprogname();
    process_mask = !strcmp(name, "SpringBoard") ? P_SB :
                   !strcmp(name, "IMDPersistenceAgent") ? P_IM :
                   !strcmp(name, "imagent") ? P_IMAGENT :
                   !strcmp(name, "ScreenTimeAgent") ? P_ST :
                   !strcmp(name, "mediaserverd") ? P_MEDIA : P_RM;
    note("init process=%s\n", name);
    register_hook("NSFileManager", "removeItemAtURL:error:", (IMP)remove_url, (IMP *)&remove_url_original, "B32@0:8@16^@24");
    register_hook("NSFileManager", "removeItemAtPath:error:", (IMP)remove_path, (IMP *)&remove_path_original, "B32@0:8@16^@24");
    if (!strcmp(name, "IMDPersistenceAgent")) {
        register_hook("IMDNotificationsController", "_postNotifications", (IMP)post, (IMP *)&post_original, "v16@0:8");
        register_hook("IMDNotificationsController", "screenTimeNotificationOptionsForContext:", (IMP)screen_time, (IMP *)&st_original, "Q24@0:8@16");
        register_hook("IMDNotificationsController", "_shouldPostNotificationForChat:messageDictionary:", (IMP)eligible, (IMP *)&eligible_original, "B32@0:8@16@24");
        register_hook("IMDNotificationsController", "_generateNotificationRequestForMessageRecord:messageDictionary:chatDictionary:isUrgentMessageTrigger:isCarouselUITriggered:isMostActive:shouldAdvanceLastAlertedMessageDate:", (IMP)generate, (IMP *)&generate_original, "@60@0:8@16@24@32B40B44B48^B52");
        register_hook("IMDNotificationsController", "_haveMigrated", (IMP)migrated, (IMP *)&migrated_original, "B16@0:8");
        register_hook("IMDNotificationsController", "_shouldPostNotificationRequest:withCompletionHandler:", (IMP)submit_check, (IMP *)&submit_check_original, "v32@0:8@16@?24");
        register_hook("UNUserNotificationCenter", "getDeliveredNotificationsWithCompletionHandler:", (IMP)delivered, (IMP *)&delivered_original, "v24@0:8@?16");
        register_hook("UNUserNotificationCenter", "addNotificationRequest:withCompletionHandler:", (IMP)client_add, (IMP *)&client_add_original, "v32@0:8@16@?24");
    } else if (!strcmp(name, "SpringBoard")) {
        register_hook("UNSUserNotificationServerConnectionListener", "addNotificationRequest:forBundleIdentifier:withCompletionHandler:", (IMP)add, (IMP *)&add_original, "v40@0:8@16@24@?32");
        register_hook("BBServer", "_sectionInfoForSectionID:effective:", (IMP)section, (IMP *)&section_original, "@28@0:8@16B24");
        register_hook("SBNotificationBannerDestination", "canReceiveNotificationRequest:", (IMP)banner, (IMP *)&banner_original, "B24@0:8@16");
        register_hook("SBNCSoundController", "canPlaySoundForNotificationRequest:", (IMP)sound, (IMP *)&sound_original, "B24@0:8@16");
        register_hook("SBNCSoundController", "playSoundForNotificationRequest:presentingDestination:", (IMP)play, (IMP *)&play_original, "v32@0:8@16@24");
        register_hook("TLAlertSystemSoundController", "playAlert:withCompletionHandler:", (IMP)tone, (IMP *)&tone_original, "v32@0:8@16@?24");
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC),
                       dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ presentation_state(); });
    } else if (!strcmp(name, "ScreenTimeAgent")) {
        register_hook("STDaemonPersistenceController", "_createDataVaultDirectory", (IMP)vault, (IMP *)&vault_original, "v16@0:8");
        register_hook("STDaemonPersistenceController", "_didAddPersistentStoreDescription:error:completionHandler:", (IMP)loaded, (IMP *)&loaded_original, "v40@0:8@16@24@?32");
        store_state("init");
    }
    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++)
        if (probes[i].processes & process_mask) probes[i].replacement = make_probe(&probes[i]);
    _dyld_register_func_for_add_image(image_loaded);
    if (process_mask & (P_SB | P_MEDIA)) install_audio_probes();
    if (process_mask == P_MEDIA) {
        /* The kernel inserts one library per process. Preserve the existing media
         * payload through this notification entry, without changing its source. */
        void *media = dlopen("/redwood/lib/libhakios.media.dylib", RTLD_NOW | RTLD_LOCAL);
        note("media-payload loaded=%d\n", media != NULL);
    }
    if (process_mask & (P_IM | P_ST | P_RM)) watch_store();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC),
                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ audit_hooks(); });

}
