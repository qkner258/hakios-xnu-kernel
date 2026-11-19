/* Host-only parser/ABI tests. Never calls a network ioctl or AP sysctl. */
#define main wifi_ap_unused_main
#include "../overlays/userland/src/umi_wifi_ap.c"
#undef main
#include <assert.h>

static void check(const char *input, size_t length, const char *expected)
{
	char data[100] = { 0 }, mode[40] = { 0 };
	assert(length <= sizeof(data));
	memcpy(data, input, length);
	int result = parse_config(data, length, mode, sizeof(mode));
	if (expected) { assert(result == 0); assert(!strcmp(mode, expected)); }
	else assert(result == -1);
}

int main(void)
{
	const struct { const char *input, *output; } cases[] = {
		{ "HakiOS-AP\n6\n", "6 HakiOS-AP" },
		{ "space in SSID\n1", "1 space in SSID" },
		{ "12345678901234567890123456789012\n11\n", "11 12345678901234567890123456789012" },
		{ "HakiOS-AP\n6\nexample-secret\n", NULL },
		{ "HakiOS-AP\n6\n\n", NULL },
		{ "HakiOS-AP\n\n6\n", NULL },
		{ "HakiOS-AP\n 6\n", NULL },
		{ "HakiOS-AP\n+6\n", NULL },
		{ "HakiOS-AP\n0006\n", NULL },
		{ "HakiOS-AP\n0\n", NULL },
		{ "HakiOS-AP\n12\n", NULL },
		{ "HakiOS-AP\n36\n", NULL },
		{ "HakiOS-AP\n6x\n", NULL },
		{ "\n6\n", NULL },
		{ "123456789012345678901234567890123\n6\n", NULL },
		{ "HakiOS-AP", NULL },
		{ "", NULL },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		check(cases[i].input, strlen(cases[i].input), cases[i].output);
	check("a\0b\n6\n", 7, NULL);
	char oversized[80]; memset(oversized, 'x', sizeof(oversized));
	check(oversized, sizeof(oversized), NULL);
	char small_data[] = "HakiOS-AP\n6\n", small_mode[4];
	assert(parse_config(small_data, strlen(small_data), small_mode, sizeof(small_mode)) == -1);
	assert(sizeof(struct ap_ifaliasreq) == 64);
	assert(AP_SIOCAIFADDR == 0x8040691aUL);
	puts("hotspot-ap: 20 parser cases and Darwin IPv4 ioctl ABI passed");
	return 0;
}
