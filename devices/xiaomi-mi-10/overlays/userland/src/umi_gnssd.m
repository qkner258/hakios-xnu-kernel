/* SPDX-License-Identifier: BSD-2-Clause
 * Opt-in X55 fixes -> iOS 16.5 CLSimulationManager -> locationd -> apps.
 * These fixes are simulated. Never print a CLLocation, NSError description,
 * raw sysctl data, or coordinates. No simulation connection until a fresh fix.
 */
#import <Foundation/Foundation.h>
#import <CoreLocation/CoreLocation.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "umi_gnss_abi.h"
#define GNSS_SWITCH "/var/db/hakios-location/gnss-on"
#define GNSS_SIM_ACTIVE "/var/db/hakios-location/simulation-active"
/* Selectors and type encodings verified in the 20F66 CoreLocation class's
 * relative method list. These forward to com.apple.locationd.simulation.
 */
@interface UmiSimulation : NSObject
- (void)appendSimulatedLocation:(CLLocation *)location;
- (void)clearSimulatedLocations;
- (void)startLocationSimulation;
- (void)stopLocationSimulation;
- (void)setLocationRepeatBehavior:(unsigned char)behavior;
@end
static volatile sig_atomic_t quitting;
static void quit(int sig) { (void)sig; quitting = 1; }
static void hung(int sig)
{
    (void)sig;
    const char s[] = "gnss: helper IPC timeout; exiting (kernel lease expires in 15 s)\n";
    write(STDOUT_FILENO, s, sizeof s - 1); _exit(2);
}
static int root_file(const char *path)
{
    struct stat s;
    return lstat(path, &s) == 0 && S_ISREG(s.st_mode) && s.st_uid == 0 && !(s.st_mode & 022);
}
static double double_bits(uint64_t v) { double d; memcpy(&d, &v, sizeof d); return d; }
static double float_bits(uint32_t v) { float f; memcpy(&f, &v, sizeof f); return f; }
static UmiSimulation *simulation(void)
{
    Class cls = NSClassFromString(@"CLSimulationManager");
    UmiSimulation *sim = (UmiSimulation *)[[cls alloc] init];
    if (!sim || ![sim respondsToSelector:@selector(appendSimulatedLocation:)]) {
        printf("gnss: iOS simulation API unavailable\n");
        return nil;
    }
    return sim;
}
static void stop_simulation(UmiSimulation *sim)
{
    alarm(10);
    [sim stopLocationSimulation];
    [sim clearSimulatedLocations];
    alarm(0);
    unlink(GNSS_SIM_ACTIVE);
    printf("gnss: simulation stop submitted; other location sources may resume\n");
}
int main(void)
{
    if (geteuid() != 0) { printf("gnss: umi_gnssd requires root\n"); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, quit); signal(SIGINT, quit); signal(SIGALRM, hung);
    @autoreleasepool {
        UmiSimulation *sim = nil;
        BOOL running = NO, leased = NO;
        unsigned tick = 0, fixes = 0;
        uint32_t sequence = 0;
        int last_error = 0;
        /* A killed helper can leave locationd's global simulation enabled. Only
         * recover a session we marked as ours; a normal disabled boot stays idle.
         */
        if (root_file(GNSS_SIM_ACTIVE)) {
            sim = simulation();
            if (!sim) return 1;
            stop_simulation(sim);
        }
        printf("gnss: helper ready; idle without root-owned gnss-on switch\n");
        while (!quitting) {
            @autoreleasepool {
                BOOL on = root_file(GNSS_SWITCH);
                /* Renew only while enabled; kernel stops on missing heartbeat after a crash. */
                if (on != leased || (on && tick % 5 == 0)) {
                    int value = on;
                    if (sysctlbyname("kern.hakios.gnss_enable", NULL, NULL, &value, sizeof value) == 0) {
                        if (leased != on) printf("gnss: switch %s; kernel session %s requested\n", on ? "on" : "off", on ? "start" : "stop");
                        leased = on; last_error = 0;
                        if (!on) sequence = 0;
                    } else if (last_error != errno) {
                        last_error = errno;
                        printf("gnss: kernel control errno %d\n", last_error);
                    }
                }
                if (!on || !leased) {
                    if (running) { stop_simulation(sim); running = NO; }
                } else {
                    struct umi_gnss_fix f = {0};
                    size_t n = sizeof f;
                    int error = sysctlbyname("kern.hakios.gnss_fix", &f, &n, NULL, 0) < 0 ? errno : 0;
                    if (tick % 30 == 0 && (error || n != sizeof f || f.version != UMI_GNSS_ABI))
                        printf("gnss: kernel fix read errno %d ABI %u bytes %zu\n", error, f.version, n);
                    BOOL fresh = !error && n == sizeof f && f.version == UMI_GNSS_ABI &&
                                 (f.flags & UMI_GNSS_POSITION) && f.age_ms <= 10000;
                    NSDate *date = [NSDate dateWithTimeIntervalSinceNow:-(double)f.age_ms / 1000];
                    if (fresh && (f.flags & UMI_GNSS_UTC)) {
                        date = [NSDate dateWithTimeIntervalSince1970:(double)f.utc_ms / 1000];
                        /* Never replay an old UTC report or mislabel it with the current time. */
                        fresh = fabs(date.timeIntervalSinceNow) <= 10;
                    }
                    if (!fresh) {
                        if (running) { stop_simulation(sim); running = NO; }
                    } else if (f.sequence != sequence) {
                        double lat = double_bits(f.latitude_bits), lon = double_bits(f.longitude_bits);
                        double accuracy = float_bits(f.accuracy_bits);
                        if (!isfinite(lat) || !isfinite(lon) || fabs(lat) > 90 || fabs(lon) > 180 ||
                            !isfinite(accuracy) || accuracy <= 0) { printf("gnss: invalid fix rejected\n"); break; }
                        if (!sim) {
                            sim = simulation();
                            if (!sim) break;
                        }
                        double altitude = f.flags & UMI_GNSS_ALTITUDE ? float_bits(f.altitude_bits) : 0;
                        double vertical = f.flags & UMI_GNSS_ALTITUDE ? float_bits(f.vertical_accuracy_bits) : -1;
                        CLLocation *location = [[CLLocation alloc] initWithCoordinate:CLLocationCoordinate2DMake(lat, lon)
                            altitude:altitude horizontalAccuracy:accuracy verticalAccuracy:vertical
                            timestamp:date];
                        alarm(10);
                        if (!running) {
                            int fd = open(GNSS_SIM_ACTIVE, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
                            if (fd < 0) { alarm(0); printf("gnss: simulation ownership marker errno %d\n", errno); break; }
                            close(fd);
                            [sim clearSimulatedLocations];
                            [sim setLocationRepeatBehavior:0]; /* no repeating a stale last fix */
                        }
                        [sim appendSimulatedLocation:location];
                        if (!running) {
                            [sim startLocationSimulation];
                            printf("gnss: simulation start submitted; app fixes are simulated by software\n");
                        }
                        /* These are asynchronous void SPI calls. In 16.5 flush
                         * is an obsolete no-op; loc_test verifies delivery.
                         */
                        alarm(0);
                        running = YES; sequence = f.sequence;
                        if (++fixes <= 3 || fixes % 30 == 0)
                            printf("gnss: submitted fix count %u accuracy %.0f m age %u ms satellites view %u/%u used %u/%u (count/known)\n",
                                fixes, accuracy, f.age_ms, f.satellites_view, !!(f.flags & UMI_GNSS_VIEW),
                                f.satellites_used, !!(f.flags & UMI_GNSS_USED));
                    }
                }
            }
            tick++;
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
        }
        int off = 0;
        if (leased) sysctlbyname("kern.hakios.gnss_enable", NULL, NULL, &off, sizeof off);
        if (running) stop_simulation(sim);
        printf("gnss: helper stopped; submitted fixes %u\n", fixes);
    }
    return 0;
}
