/*
 * UMI only: umi_charge_diag=1 (default OFF). Observer reads only; no PMIC
 * write channels, interrupt acknowledgements, ownership or charger writes.
 * Included by hakios.battery.c, built with CONFIG_HAKIOS_BATTERY.
 *
 * Register facts/scales: MiCode Xiaomi_Kernel_OpenSource umi-q-oss,
 * db67ca6001320efc1f945c270635434fc403a9d4, drivers/power/supply/qcom/
 * smb5-reg.h, qpnp-smb5.c (smb5_pm8150b_params), fg-reg.h, fg-util.c.
 * Arbiter v5 observer command layout: drivers/spmi/spmi-pmic-arb.c.
 * UMI live DT: SID2 CHGR=1000 DCDC=1100 BATIF=1200 USBIN=1300 MISC=1600,
 * FG_SOC=4000 FG_INFO=4100. Gen4 temperature uses SRAM word 328, NOT the
 * Gen3 temperature conversion. Never use the UEFI cache to authorize charging.
 */
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t thread);

static bool umi_charge_diag_enabled;
static vm_offset_t umi_chg_core, umi_chg_observer;
static const uint16_t umi_chg_ppids[] = { 0x210, 0x211, 0x212, 0x213, 0x216, 0x240, 0x241 };
static uint16_t umi_chg_channels[7];

static void umi_chg_say(const char *format, ...) __printflike(1, 2);
static void
umi_chg_say(const char *format, ...)
{
	char line[400];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	for (const char *p = line; *p; p++) loader_log_putc(*p);
	printf("%s", line);
}

static uint32_t
umi_chg_rd(vm_offset_t p)
{
	uint32_t v = *(volatile uint32_t *)p;
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

/* Only the observer's READ command register is written. No PMIC writes. */
static bool
umi_chg_read(uint16_t address, uint8_t *out, unsigned count)
{
	unsigned index;
	uint32_t status, data[2];
	uint64_t deadline, interval;
	if (!count || count > 8 || (address & 255) + count > 256) return false;
	for (index = 0; index < 7; index++) {
		if (umi_chg_ppids[index] == (0x200 | (address >> 8))) break;
	}
	if (index == 7 || umi_chg_channels[index] == 0xffff) return false;
	vm_offset_t ch = umi_chg_observer + 0x80UL * umi_chg_channels[index];
	nanoseconds_to_absolutetime(2000ULL * NSEC_PER_USEC, &interval);
	hakios_spmi_lock();
	deadline = mach_absolute_time() + interval;
	*(volatile uint32_t *)ch = (1u << 27) | ((address & 255) << 4) | (count - 1);
	__asm__ volatile("dsb osh" ::: "memory");
	do {
		status = umi_chg_rd(ch + 8);
	} while (!(status & 1) && mach_absolute_time() < deadline);
	data[0] = umi_chg_rd(ch + 0x18);
	data[1] = count > 4 ? umi_chg_rd(ch + 0x1c) : 0;
	hakios_spmi_unlock();
	if ((status & 15) != 1) return false;
	for (unsigned n = 0; n < count; n++) out[n] = (uint8_t)(data[n / 4] >> (8 * (n % 4)));
	return true;
}

static void
umi_chg_sample(void)
{
	static const uint16_t addr[] = {
		0x1006, 0x1007, 0x100b, 0x100d, 0x1042, 0x1043, 0x1061, 0x1070, 0x1090,
		0x1106, 0x1107, 0x1108, 0x110a, 0x110b, 0x1210, 0x1310, 0x1340, 0x1366,
		0x1370, 0x1606, 0x1607, 0x1608, 0x1609, 0x160a, 0x1610, 0x1651, 0x1653
	};
	uint8_t v[sizeof(addr) / sizeof(addr[0])], main[4], shadow[4], soc[2];
	bool ok = true;
	for (unsigned n = 0; n < sizeof(addr) / sizeof(addr[0]); n++) {
		if (!umi_chg_read(addr[n], &v[n], 1)) {
			umi_chg_say("umi-charge: READ FAILED SID2:%04x; charger sample discarded; control=OFF\n", addr[n]);
			ok = false;
			break;
		}
	}
	if (ok) {
		umi_chg_say("umi-charge: state=%u err=%02x modes=%02x temp_status=%02x enable=%u pause=%u "
		    "FCC=%u mA FV=%u mV JEITA=%02x\n", v[0] & 7, v[1], v[2], v[3], v[4] & 1, v[5] & 1,
		    v[6] * 50, 3600 + v[7] * 10, v[8]);
		umi_chg_say("umi-charge: ICLmax=%u ICL1107=%u AICL=%u mA aicl_status=%02x path=%02x "
		    "BATIF=%02x USBIN=%02x suspend=%u usb_options=%02x ICLcfg=%u mA\n",
		    v[9] * 50, v[10] * 50, v[11] * 50, v[12], v[13], v[14], v[15], v[16] & 1, v[17], v[18] * 50);
		umi_chg_say("umi-charge: thermal=%02x/%02x/%02x/%02x/%02x IRQ1610=%02x "
		    "WD1651=%02x WD1653=%02x; control=OFF (no watchdog pet)\n",
		    v[19], v[20], v[21], v[22], v[23], v[24], v[25], v[26]);
	}
	/* Gen4 instantaneous voltage/current must match their hardware shadows. */
	for (unsigned retry = 0; retry < 3; retry++) {
		if (!umi_chg_read(0x41a0, main, 4) || !umi_chg_read(0x41a6, shadow, 4)) break;
		if (memcmp(main, shadow, 4) != 0) continue;
		unsigned vr = main[0] | ((unsigned)main[1] << 8);
		int ir = main[2] | ((unsigned)main[3] << 8);
		if (ir & 0x8000) ir -= 65536;
		int uv = (int)((uint64_t)vr * 122070 / 1000);
		int ua = (int)((int64_t)ir * 488281 / 1000);
		if (uv < 2500000 || uv > 5000000) break;
		umi_chg_say("umi-charge: FG V=%d uV I=%d uA (%s) raw=%04x/%04x; "
		    "T=unavailable (Gen4 SRAM required)\n", uv, ua,
		    ua < -50000 ? "NET INTO BATTERY" : ua > 50000 ? "NET OUT OF BATTERY" : "near zero",
		    vr, (unsigned)(uint16_t)ir);
		if (umi_chg_read(0x4009, soc, 2) && soc[0] == soc[1]) {
			umi_chg_say("umi-charge: FG monotonic_SOC_raw=%u/255 (not Android UI SOC)\n", soc[0]);
		}
		return;
	}
	umi_chg_say("umi-charge: FG INVALID (read/shadow/voltage); no current conclusion; control=OFF\n");
}

static void
umi_chg_thread(__unused void *parameter, __unused wait_result_t result)
{
	umi_chg_core = ml_io_map(0x0c440000UL, 0x1100);
	umi_chg_observer = ml_io_map(0x0e600000UL, 0x10000);
	if (!umi_chg_core || !umi_chg_observer) {
		umi_chg_say("umi-charge: map failed; stopped\n");
		goto stopped;
	}
	uint32_t version = umi_chg_rd(umi_chg_core);
	uint32_t channels = umi_chg_rd(umi_chg_core + 4) & 0x7ff;
	if (version != 0x50010000 || channels != 512) {
		umi_chg_say("umi-charge: unsupported arbiter=%08x channels=%u; stopped\n", version, channels);
		goto stopped;
	}
	for (unsigned i = 0; i < 7; i++) umi_chg_channels[i] = 0xffff;
	for (unsigned n = 0; n < channels; n++) {
		uint32_t map = umi_chg_rd(umi_chg_core + 0x900 + 4 * n);
		for (unsigned i = 0; i < 7; i++) {
			if (map && ((map >> 8) & 0xfff) == umi_chg_ppids[i] && umi_chg_channels[i] == 0xffff)
				umi_chg_channels[i] = (uint16_t)n;
		}
	}
	uint8_t subtype;
	if (!umi_chg_read(0x4105, &subtype, 1) || subtype != 0x11) {
		umi_chg_say("umi-charge: Gen4 FG_INFO subtype not verified; stopped\n");
		goto stopped;
	}
	umi_chg_say("umi-charge: read-only SMB5/Gen4 observer every 10 s; negative FG current = into battery; control=OFF\n");
	for (;;) {
		umi_chg_sample();
		delay_for_interval(10000, NSEC_PER_MSEC);
	}
stopped:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static void
umi_charger_diag_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;
	if (!PE_parse_boot_argn("umi_charge_diag", &enabled, sizeof(enabled)) || enabled != 1) return;
	umi_charge_diag_enabled = true;
	if (kernel_thread_start(umi_chg_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
	else umi_chg_say("umi-charge: thread start failed; stopped\n");
}
