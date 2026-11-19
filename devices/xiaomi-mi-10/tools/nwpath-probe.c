// nwpath-probe: print what Network.framework / SCNetworkReachability think of connectivity.
// Apps such as Weather show "no network" when the nw_path is not satisfied, even if sockets work.
// Build: clang -isysroot $SDK -arch arm64 -miphoneos-version-min=16.0 -framework Network \
//        -framework SystemConfiguration -o nwpath-probe nwpath-probe.c && codesign -s - nwpath-probe
#include <Network/Network.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>

static const char *st(nw_path_status_t s) {
    switch (s) {
    case nw_path_status_satisfied: return "satisfied";
    case nw_path_status_unsatisfied: return "UNSATISFIED";
    case nw_path_status_satisfiable: return "satisfiable";
    default: return "invalid";
    }
}

static void dump(const char *tag, nw_path_t p) {
    printf("%s: status=%s expensive=%d constrained=%d ipv4=%d ipv6=%d dns=%d", tag, st(nw_path_get_status(p)),
           nw_path_is_expensive(p), nw_path_is_constrained(p), nw_path_has_ipv4(p), nw_path_has_ipv6(p),
           nw_path_has_dns(p));
    if (__builtin_available(iOS 14.2, *))
        printf(" unsatisfied_reason=%d", (int)nw_path_get_unsatisfied_reason(p));
    printf("\n");
    nw_path_enumerate_interfaces(p, ^bool(nw_interface_t i) {
        printf("  if %s type=%d\n", nw_interface_get_name(i), (int)nw_interface_get_type(i));
        return true;
    });
}

int main(void) {
    struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET};
    SCNetworkReachabilityRef r = SCNetworkReachabilityCreateWithAddress(NULL, (struct sockaddr *)&a);
    SCNetworkReachabilityFlags f = 0;
    Boolean ok = r ? SCNetworkReachabilityGetFlags(r, &f) : 0;
    printf("SCNetworkReachability ok=%d flags=0x%x reachable=%d wwan=%d\n", ok, f,
           !!(f & kSCNetworkReachabilityFlagsReachable), !!(f & kSCNetworkReachabilityFlagsIsWWAN));

    dispatch_queue_t q = dispatch_queue_create("nwpath", NULL);
    __block int n = 0;
    nw_path_monitor_t m = nw_path_monitor_create();
    nw_path_monitor_set_queue(m, q);
    nw_path_monitor_set_update_handler(m, ^(nw_path_t p) { dump("monitor(any)", p); n++; });
    nw_path_monitor_start(m);
    nw_path_monitor_t mw = nw_path_monitor_create_with_type(nw_interface_type_wifi);
    nw_path_monitor_set_queue(mw, q);
    nw_path_monitor_set_update_handler(mw, ^(nw_path_t p) { dump("monitor(wifi)", p); });
    nw_path_monitor_start(mw);
    nw_path_monitor_t mc = nw_path_monitor_create_with_type(nw_interface_type_cellular);
    nw_path_monitor_set_queue(mc, q);
    nw_path_monitor_set_update_handler(mc, ^(nw_path_t p) { dump("monitor(cell)", p); });
    nw_path_monitor_start(mc);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (!n) printf("monitor(any): NO UPDATE in 5 s\n");
        exit(0);
    });
    dispatch_main();
}
