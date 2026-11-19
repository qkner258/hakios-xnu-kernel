#include <stdio.h>
#include <string.h>
#include "umi_ims_protocol.h"

static int failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failed = 1; } } while (0)

static unsigned response(unsigned char *p, unsigned msg, unsigned error)
{
    const unsigned char header[] = {2, 1, 0, 0, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0};
    memcpy(p, header, sizeof header);
    umi_ims_wr16(p + 3, msg);
    umi_ims_wr16(p + 10, error != 0);
    umi_ims_wr16(p + 12, error);
    return sizeof header;
}

static unsigned append(unsigned char *p, unsigned n, unsigned tag, const unsigned char *v, unsigned len)
{
    p[n] = (unsigned char)tag;
    umi_ims_wr16(p + n + 1, len);
    memcpy(p + n + 3, v, len);
    umi_ims_wr16(p + 5, n + 3 + len - 7);
    return n + 3 + len;
}

static unsigned start(unsigned char *p, unsigned family, const char *apn)
{
    unsigned n = 7;
    p[0] = 0; p[1] = 1; p[2] = 0; p[3] = UMI_IMS_START; p[4] = 0;
    p[n++] = UMI_IMS_APN_TLV; p[n++] = 3; p[n++] = 0;
    p[n++] = (unsigned char)apn[0]; p[n++] = (unsigned char)apn[1]; p[n++] = (unsigned char)apn[2];
    p[n++] = UMI_IMS_FAMILY_TLV; p[n++] = 1; p[n++] = 0; p[n++] = (unsigned char)family;
    umi_ims_wr16(p + 5, n - 7);
    return n;
}

int main(void)
{
    unsigned char in[128], out[128];
    unsigned n = start(in, 3, "IMS");
    struct umi_ims_request_fix f = umi_ims_prepare(UMI_IMS_WDS, in, n, out, sizeof out);
    CHECK(f.is_ims && f.len == n && f.family_before == 3 && f.family_after == 6 && f.family_changed);
    unsigned family_len = 0;
    const unsigned char *family = umi_ims_find(out, f.len, UMI_IMS_FAMILY_TLV, &family_len);
    CHECK(family && family_len == 1 && family[0] == 6);
    n = start(in, 3, "cmn");
    memset(out, 0xa5, sizeof out);
    f = umi_ims_prepare(UMI_IMS_WDS, in, n, out, sizeof out);
    CHECK(!f.is_ims && !f.len && out[0] == 0xa5);
    n = start(in, 6, "ims");
    f = umi_ims_prepare(UMI_IMS_WDS, in, n, out, sizeof out);
    CHECK(f.is_ims && f.family_after == 6 && !f.family_changed);
    in[n - 2] = 0xff;
    f = umi_ims_prepare(UMI_IMS_WDS, in, n, out, sizeof out);
    CHECK(!f.is_ims && !f.len);
    n = response(in, UMI_IMS_GET_SETTINGS, 0);
    const unsigned char own_ipv6[17] = {0x20, 1};
    n = append(in, n, 0x25, own_ipv6, sizeof own_ipv6);
    const unsigned char marker[] = {1};
    n = append(in, n, 0x22, marker, sizeof marker);
    CHECK(!umi_ims_has_pcscf(in, n));
    // Qualcomm WDS 0x2e is the IPv6 P-CSCF list; 0x25 above is our address.
    unsigned char pcscf6[33] = {2};
    unsigned ipv6_n = response(in, UMI_IMS_GET_SETTINGS, 0);
    ipv6_n = append(in, ipv6_n, 0x2e, pcscf6, sizeof pcscf6);
    CHECK(!umi_ims_has_pcscf(in, ipv6_n));
    in[ipv6_n - 1] = 1;
    CHECK(umi_ims_has_pcscf(in, ipv6_n));
    CHECK(!umi_ims_has_pcscf(in, ipv6_n - 1));
    in[17] = 3; // Count/array mismatch with a well-framed TLV.
    CHECK(!umi_ims_has_pcscf(in, ipv6_n));
    n = response(in, UMI_IMS_GET_SETTINGS, 0);
    const unsigned char ipv4[] = {1, 192, 0, 2, 1};
    n = append(in, n, 0x23, ipv4, sizeof ipv4);
    CHECK(umi_ims_has_pcscf(in, n));
    CHECK(!umi_ims_has_pcscf(in, n - 1));   // (a reply shorter than its declared length is an overrun)
    umi_ims_wr16(in + 10, 1);
    umi_ims_wr16(in + 12, 3);
    CHECK(!umi_ims_has_pcscf(in, n) && umi_ims_result(in, n) == 3);
    // A 0x2d reply with padding past its last TLV: the SDU's declared length is a floor, as cbd_sdu_ok()
    // reads it, so the DAD capture still finds the address. A TLV that runs past the declared end is
    // still an overrun, padding or not.
    n = response(in, UMI_IMS_GET_SETTINGS, 0);
    n = append(in, n, 0x25, own_ipv6, sizeof own_ipv6);
    const unsigned declared = n;
    memset(in + n, 0, 5);
    unsigned len25 = 0;
    const unsigned char *padded25 = umi_ims_find(in, n + 5, 0x25, &len25);
    CHECK(umi_ims_tlvs_ok(in, n + 5));
    CHECK(padded25 && len25 == sizeof own_ipv6 && memcmp(padded25, own_ipv6, sizeof own_ipv6) == 0);
    CHECK(!umi_ims_tlvs_ok(in, declared - 1));
    const unsigned tlv_len = umi_ims_rd16(in + 8);
    umi_ims_wr16(in + 8, 30);
    CHECK(!umi_ims_tlvs_ok(in, n + 5) && !umi_ims_find(in, n + 5, 0x25, &len25));
    umi_ims_wr16(in + 8, tlv_len);
    n = response(in, UMI_IMS_GET_SETTINGS, 0);
    const unsigned char empty[] = {0};
    n = append(in, n, 0x23, empty, sizeof empty);
    CHECK(!umi_ims_has_pcscf(in, n));
    const unsigned char domain[] = {1, 3, 0, 'i', 'm', 's'};
    n = append(in, n, 0x24, domain, sizeof domain);
    CHECK(umi_ims_has_pcscf(in, n));
    in[n - 5] = 4;
    CHECK(!umi_ims_has_pcscf(in, n));

    struct umi_ims_bearer bearer = {0};
    n = start(in, 6, "ims");
    umi_ims_observe(&bearer, in, n);
    CHECK(bearer.selected && !bearer.active && !bearer.pcscf && bearer.family == 6);
    n = response(in, UMI_IMS_START, 0);
    const unsigned char handle[] = {1, 0, 0, 0};
    n = append(in, n, 1, handle, sizeof handle);
    in[1] = 2;
    umi_ims_observe(&bearer, in, n);
    CHECK(!bearer.active);
    in[1] = 1;
    umi_ims_observe(&bearer, in, n);
    CHECK(bearer.active && !bearer.pcscf);
    n = response(in, UMI_IMS_GET_SETTINGS, 0);
    n = append(in, n, 0x23, ipv4, sizeof ipv4);
    umi_ims_observe(&bearer, in, n);
    CHECK(bearer.active && bearer.pcscf);
    n = response(in, UMI_IMS_STOP, 3);
    umi_ims_observe(&bearer, in, n);
    CHECK(bearer.active);
    n = response(in, UMI_IMS_STOP, 0);
    umi_ims_observe(&bearer, in, n);
    CHECK(!bearer.active && !bearer.pcscf);
    n = start(in, 6, "cmn");
    umi_ims_observe(&bearer, in, n);
    CHECK(!bearer.selected);

    n = response(in, UMI_IMS_GET_REGISTRATION, 0);
    CHECK(umi_ims_registration(in, n) == -1);
    n = append(in, n, 0x10, empty, sizeof empty);
    CHECK(umi_ims_registration(in, n) == 0);
    in[n - 1] = 1;
    CHECK(umi_ims_registration(in, n) == 1);
    const unsigned char registering[] = {1, 0, 0, 0};
    n = append(in, n, 0x12, registering, sizeof registering);
    in[n - 8] = 0; // Legacy flag idle, new state registering takes precedence.
    CHECK(umi_ims_registration(in, n) == 1);
    CHECK(umi_ims_registration(in, n - 1) == -1);
    CHECK(umi_ims_modem_registration_command(0x12, 0x20));
    CHECK(umi_ims_modem_registration_command(0x21, 0x23));
    CHECK(!umi_ims_modem_registration_command(0x21, 0x20));
    CHECK(!umi_ims_modem_registration_command(1, 0x20));
    if (!failed) puts("PASS IMS protocol: IPv6 bearer, P-CSCF schema, lifecycle, modem registration exclusion");
    return failed;
}
