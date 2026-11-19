/*
 * loc_test: is iOS's locationd there, does it answer, and what does it say (devices/xiaomi-mi-10/LOCATION-umi.md).
 *
 *   loc_test [SECONDS [BUNDLE]]   status, then SECONDS (default 0) of location updates
 *   loc_test enable               turn Location Services (the master switch in Settings > Privacy) on, as Settings does
 *
 * +[CLLocationManager locationServicesEnabled] is a synchronous call on com.apple.locationd.synchronous, so
 * its answer is the liveness test: a locationd that is up and serving answers in milliseconds, a missing
 * job fails at once (launchd: "failed lookup"), and a locationd that hangs is caught by the alarm below,
 * which prints "loc_test: TIMEOUT" -- the one thing a hang would leave in the log.
 *
 * Authorization. A command-line program has no bundle, so locationd can never ask the user about it: in v27
 * it went "not determined" and every request ended in kCLErrorDenied (1). So the updates are asked for on
 * behalf of a system location bundle (-initWithEffectiveBundlePath:, entitlement
 * com.apple.locationd.effective_bundle), by default MobileWiFi's, which locationd authorizes by itself as
 * soon as Location Services is on (v27: wifid "new status: 3" the moment the user switched it on). BUNDLE
 * names another; "-" asks as this program, the old way. Without the entitlement (the program unsigned)
 * locationd treats it as "-".
 *
 * No coordinates are printed, ever: a fix is shown by its accuracy, age and speed, which is enough to tell
 * a Wi-Fi fix (tens to hundreds of metres) from a cell fix (kilometres) from a GNSS one (metres).
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_BUNDLE "/System/Library/LocationBundles/MobileWiFi.framework"

@interface CLLocationManager (HakiosPrivate)
+ (void)setLocationServicesEnabled:(BOOL)enabled;
+ (int)authorizationStatusForBundlePath:(NSString *)path;
- (instancetype)initWithEffectiveBundlePath:(NSString *)path;
@end

static void
timeout(int sig)
{
	static const char m[] = "loc_test: TIMEOUT: locationd did not answer\n";

	(void)sig;
	write(1, m, sizeof(m) - 1);
	_exit(2);
}

static const char *
error_name(NSError *error)
{
	if (![error.domain isEqualToString:kCLErrorDomain]) {
		return "";
	}
	switch (error.code) {
	case kCLErrorLocationUnknown: return " (location unknown: locationd has no source that answers yet)";
	case kCLErrorDenied: return " (denied: Location Services off, or this client not authorized)";
	case kCLErrorNetwork: return " (network: the position server could not be reached)";
	default: return "";
	}
}

@interface LocProbe : NSObject <CLLocationManagerDelegate>
@property unsigned fixes;
@property double best;
@end

@implementation LocProbe
- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)manager
{
	printf("loc_test: authorization = %d (0 not determined, 2 denied, 3 always, 4 when in use)\n",
	    (int)manager.authorizationStatus);
	fflush(stdout);
}

- (void)locationManager:(CLLocationManager *)manager didUpdateLocations:(NSArray<CLLocation *> *)locations
{
	CLLocation *l = locations.lastObject;
	double h = l.horizontalAccuracy;

	(void)manager;
	if (self.fixes == 0 || (h >= 0 && h < self.best)) {
		self.best = h;
	}
	/* every fix for the first 5, then one in 10: a 10-minute run at 1 Hz stays short */
	if (self.fixes++ < 5 || self.fixes % 10 == 0) {
		printf("loc_test: fix %u: horizontal accuracy %.0f m (%s), vertical %.0f m, age %.1f s, speed %.1f m/s, "
		    "simulated %d\n", self.fixes, h, h < 0 ? "invalid" : h <= 30 ? "GNSS-like" : h <= 300 ? "Wi-Fi-like" :
		    "cell-like", l.verticalAccuracy, -l.timestamp.timeIntervalSinceNow, l.speed,
		    (int)l.sourceInformation.isSimulatedBySoftware);
		fflush(stdout);
	}
}

- (void)locationManager:(CLLocationManager *)manager didFailWithError:(NSError *)error
{
	(void)manager;
	printf("loc_test: error %s %ld%s\n", error.domain.UTF8String, (long)error.code, error_name(error));
	fflush(stdout);
}
@end

int
main(int argc, char **argv)
{
	@autoreleasepool {
		int enable = argc > 1 && strcmp(argv[1], "enable") == 0;
		int seconds = argc > 1 && !enable ? atoi(argv[1]) : 0;
		const char *bundle = argc > 2 ? argv[2] : DEFAULT_BUNDLE;

		signal(SIGALRM, timeout);
		alarm(seconds + 20);
		printf("loc_test: asking locationd\n");
		fflush(stdout);
		if (enable) {
			[CLLocationManager setLocationServicesEnabled:YES];
		}
		printf("loc_test: locationServicesEnabled = %d\n", (int)[CLLocationManager locationServicesEnabled]);
		fflush(stdout);
		if (seconds <= 0) {
			return 0;
		}
		LocProbe *probe = [LocProbe new];
		CLLocationManager *manager;
		if (strcmp(bundle, "-") != 0) {
			NSString *path = [NSString stringWithUTF8String:bundle];
			printf("loc_test: asking on behalf of %s (its authorization: %d)\n", bundle,
			    [CLLocationManager respondsToSelector:@selector(authorizationStatusForBundlePath:)] ?
			    [CLLocationManager authorizationStatusForBundlePath:path] : -1);
			manager = [[CLLocationManager alloc] initWithEffectiveBundlePath:path];
		} else {
			manager = [CLLocationManager new];
		}
		manager.delegate = probe;
		manager.desiredAccuracy = kCLLocationAccuracyBest;
		[manager startUpdatingLocation];
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
		[manager stopUpdatingLocation];
		if (probe.fixes) {
			printf("loc_test: done: %u fixes, best horizontal accuracy %.0f m\n", probe.fixes, probe.best);
		} else {
			printf("loc_test: done: no fix in %d s\n", seconds);
		}
	}
	return 0;
}
