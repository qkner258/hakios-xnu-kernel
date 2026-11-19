/* Keep the existing return-to-loader hook, including after panic, responsible
 * for stopping both external PCIe endpoints before kernel memory is reused. */
#define umi_wifi_quiesce umi_wifi_quiesce_impl
#include "umi_wifi.c"
#undef umi_wifi_quiesce
extern void umi_modem_quiesce(void);
void umi_wifi_quiesce(void);
void umi_wifi_quiesce(void)
{
    umi_modem_quiesce();
    umi_wifi_quiesce_impl();
}
