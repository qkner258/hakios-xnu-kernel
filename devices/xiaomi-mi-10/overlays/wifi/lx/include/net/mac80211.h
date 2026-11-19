/* UMI AP implementation of the imported LinuxKPI inline beacon stub.
 * Keep the pinned FreeBSD header and all STA helpers unchanged. */
#ifndef UMI_MAC80211_AP_H
#define UMI_MAC80211_AP_H
#include_next <net/mac80211.h>
struct sk_buff *hakios_ieee80211_beacon_get_template(struct ieee80211_hw *,
    struct ieee80211_vif *, struct ieee80211_mutable_offsets *, uint32_t);
#define ieee80211_beacon_get_template hakios_ieee80211_beacon_get_template
void hakios_ieee80211_report_low_ack(struct ieee80211_sta *, int);
#define ieee80211_report_low_ack hakios_ieee80211_report_low_ack
#endif
