/*
 * Xiaomi Mi 10 (umi / SM8250): WCD9385 + Bolero 1.2 capture.
 * Fork of drivers/audio/hakios.mic.c; never stage the Redwood MMIO on UMI.
 * Evidence: work/umi-mic/live.dts and Xiaomi audio-kernel umi-q-oss
 * 7efacc8c137d0c8b56610f61823dee2154a8349f (see MIC-umi.md).
 *
 * umi_mic=1: APR votes and TX clocks only, then release. NO mic MMIO.
 * umi_mic=2: bounded MMIO, reset GPIO32, LPI0/1/2, SWR v1.5.1 enumeration;
 *            read chip IDs; no ADC or mic bias; then release.
 * umi_mic=3: ten-second mono ADC1 -> SWR master port 2 -> TX DEC1 ->
 *            AFE 0xb037 -> ADM live recording -> ASM session 2 capture.
 * umi_mic=4: same mono path on demand via /dev/hakios-mic for the existing HAL.
 *
 * UMI's static handset mixer uses ADC1+ADC4, DEC1+DEC0, two channels.
 * This first capture variant deliberately takes only ADC1 (MIC BIAS1, 2750mV),
 * at the static overlay's gain 6, with no speaker/headphone codec path enabled.
 * Stage3/4 check BOB through the read-only SPMI observer before codec init.
 * Only a proven OFF BOB may receive an owned, bounded bobc1 RPMh vote.
 * Digital supplies remain inherited; enumeration failure stops initialization.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <kern/kalloc.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static LCK_GRP_DECLARE(mic_lock_group, "hakios-mic");
static LCK_MTX_DECLARE(mic_lock, &mic_lock_group);      /* what is asked for, and given back */

static uint32_t level;
static volatile bool running;
static bool voice_claimed;
#include "umi_voice_audio.h"

static int say(const char *format, ...) __printflike(1, 2);

/*
 * Every line is kept as well, and all of them said again at the run's end (hakios_mic_quiesce()):
 * the log of a run keeps its beginning and its end, and this happens in between.
 */
#define KEPT_LINES              400
static char kept[KEPT_LINES][320];
static unsigned kept_count;

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	strlcpy(kept[kept_count++ % KEPT_LINES], line, sizeof(kept[0]));
	return length;
}

/* A line that says all went as it should: not said again in iOS's later recordings (umi_mic=4), where it is the same each time. */
static bool quiet;
#define note(...)       do { if (!quiet) { say(__VA_ARGS__); } } while (0)

void
hakios_mic_quiesce(void)
{
	if (kept_count == 0) {
		return;
	}
	for (const char *c = "umi-mic: what happened in this run, again:\n"; *c != 0; c++) {
		loader_log_putc(*c);
	}
	for (unsigned i = kept_count > KEPT_LINES ? kept_count - KEPT_LINES : 0; i < kept_count; i++) {
		loader_log_putc('>');
		loader_log_putc(' ');
		for (const char *c = kept[i % KEPT_LINES]; *c != 0; c++) {
			loader_log_putc(*c);
		}
		if (strlen(kept[i % KEPT_LINES]) && kept[i % KEPT_LINES][strlen(kept[i % KEPT_LINES]) - 1] != '\n') {
			loader_log_putc('\n');
		}
	}
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]\n\tdsb sy" : : "r" (value), "r" (address) : "memory");
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static bool broken;                     /* a write that did not read back: nothing more is written, all is undone */
static bool cleanup_failed;
static char last_write[120] = "nothing written";

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static bool
mapped(struct block *b, uint32_t offset)
{
	if ((offset & 3) || b->size < 4 || offset > b->size - 4) {
		say("umi-mic: MMIO BOUNDS FAILED: %s + %#x (size %#x)\n", b->name, offset, b->size);
		broken = true;
		return false;
	}
	if (!b->base) b->base = ml_io_map(b->phys, b->size);
	if (!b->base) {
		say("umi-mic: MMIO MAP FAILED: %s at %#x\n", b->name, b->phys);
		broken = true;
		return false;
	}
	return true;
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	if (!mapped(b, offset)) return 0;
	note("umi-mic: read %s %s at %#x\n", b->name, name, b->phys + offset);
	uint32_t value = rd(b->base + offset);
	note("umi-mic: %s %s (%#x) = %#x\n", b->name, name, b->phys + offset, value);
	return value;
}

static bool
put(struct block *b, const char *name, uint32_t offset, uint32_t value, uint32_t check)
{
	if (broken || !mapped(b, offset)) return false;
	snprintf(last_write, sizeof(last_write), "%#x to %s %s (%#x)", value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
	uint32_t back = rd(b->base + offset);
	if ((back & check) != (value & check)) {
		say("umi-mic: %s %s (%#x) <- %#x, reads %#x: STOPPING HERE\n", b->name, name, b->phys + offset, value, back);
		broken = true;
		return false;
	}
	note("umi-mic: %s %s (%#x) <- %#x%s\n", b->name, name, b->phys + offset, value, check ? ", reads back" : "");
	return true;
}

static bool
put_bits(struct block *b, const char *name, uint32_t offset, uint32_t mask, uint32_t value)
{
	if (broken || !mapped(b, offset)) return false;
	return put(b, name, offset, (rd(b->base + offset) & ~mask) | (value & mask), mask);
}

/* Try every saved register even if an earlier restoration failed. */
static void
restore(struct block *b, uint32_t offset, uint32_t value, uint32_t check)
{
	broken = false;
	if (!put(b, "restored", offset, value, check)) cleanup_failed = true;
}

/* ---- the DSP: votes and clocks ------------------------------------------------------------------- */

#define APR_SVC_AFE                             4
#define APR_BASIC_RSP_RESULT                    0x000110e8u
#define APR_WAIT_MS                             1000
#define AFE_SVC_CMD_SET_PARAM                   0x000100f3u
#define AFE_MODULE_CLOCK_SET                    0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET                  0x00010290u
#define AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST       0x000100f4u
#define AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST   0x000100f5u
#define AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST     0x000100f6u
#define LPASS_HW_MACRO_BLOCK                    3
#define LPASS_HW_DCODEC_BLOCK                   4
#define CLOCK_TX_CORE                           0x30c
#define CLOCK_TX_NPL                            0x30d
#define CLOCK_HZ                                19200000
#define CLOCK_COUPLE_NO                         1

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected", "panic",
		                             "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted",
		                             "preempted", "continue", "immediate", "not implemented", "need more", "no memory",
		                             "not there" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "an error this does not know";
}

static bool
command(const char *what, uint8_t service, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted,
    struct hakios_apr_answer *answer)
{
	struct hakios_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	bzero(answer, sizeof(*answer));
	if (__atomic_load_n(&voice_claimed, __ATOMIC_ACQUIRE))
        say("voice-audio: afe send svc=%u op=0x%x bytes=%u\n", service, opcode, length);
    bool answered = hakios_adsp_apr(service, 0, 0, opcode, payload, length, APR_WAIT_MS, answer);
    if (__atomic_load_n(&voice_claimed, __ATOMIC_ACQUIRE))
        say("voice-audio: afe result svc=%u op=0x%x reply=0x%x rc=%d dsp=%u\n", service, opcode,
            answered ? answer->opcode : 0u, answered ? 0 : ETIMEDOUT,
            answered && answer->opcode == APR_BASIC_RSP_RESULT && answer->length >= 8 ? answer->words[1] : UINT32_MAX);
    if (!answered) {
		say("umi-mic: %s (opcode %#x): NO ANSWER\n", what, opcode);
		return false;
	}
	if (answer->opcode == APR_BASIC_RSP_RESULT && answer->length >= 8 && answer->words[0] == opcode) {
		if (answer->words[1] == 0) {
			note("umi-mic: %s: done\n", what);
		} else {
			say("umi-mic: %s: REFUSED: %s\n", what, dsp_error(answer->words[1]));
		}
		return answer->words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && answer->opcode == wanted && answer->length >= 4) {
		return true;
	}
	say("umi-mic: %s: an answer not expected: opcode %#x, %u bytes: %#x %#x\n", what, answer->opcode, answer->length,
	    answer->words[0], answer->words[1]);
	return false;
}

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

static bool
afe_service_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	uint32_t *head = (uint32_t *)packet;
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	head[0] = (uint32_t)sizeof(*p) + size;          /* payload size; address 0 and handle 0: in band */
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, AFE_SVC_CMD_SET_PARAM, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

static bool
afe_clock(const char *what, uint32_t id, bool on)
{
	struct afe_clock_set clock = { 1, id, CLOCK_HZ, CLOCK_COUPLE_NO, 0, on };

	return afe_service_param(what, AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock));
}

static uint32_t vote_handle[5];
static bool voted[5], core_clock, npl_clock, core_attempted, npl_attempted;

static bool
vote(uint32_t block, const char *client)
{
	struct {
		uint32_t block;
		char client[8];
	} __attribute__((packed)) request = { block, { 0 } };
	struct hakios_apr_answer answer = { 0 };
	char what[80];

	strlcpy(request.client, client, sizeof(request.client));
	snprintf(what, sizeof(what), "the LPASS's vote for block %u (\"%s\")", block, client);
	if (!command(what, APR_SVC_AFE, AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, &request, sizeof(request),
	    AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, &answer)) {
		/* A lost reply may conceal a vote whose handle we cannot return. */
		cleanup_failed = true;
		return false;
	}
	vote_handle[block] = answer.words[0];
	if (!vote_handle[block]) {
		say("umi-mic: invalid zero vote handle for block %u; blocks untouched\n", block);
		cleanup_failed = true;
		return false;
	}
	voted[block] = true;
	say("umi-mic: %s: given, handle %u\n", what, vote_handle[block]);
	return true;
}

static void
devote(uint32_t block)
{
	uint32_t request[2] = { block, vote_handle[block] };
	char what[64];

	if (!voted[block]) {
		return;
	}
	snprintf(what, sizeof(what), "the LPASS's vote for block %u, given back", block);
	if (!command(what, APR_SVC_AFE, AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST, request, sizeof(request), 0, NULL))
		cleanup_failed = true;
	voted[block] = false;
}

static bool
clocks_on(void)
{
	if (!vote(LPASS_HW_MACRO_BLOCK, "LPASS_HW_MACRO") || !vote(LPASS_HW_DCODEC_BLOCK, "LPASS_HW_DCODEC")) {
		return false;
	}
	core_attempted = true;
	core_clock = afe_clock("the TX macro's core clock (0x30c) at 19.2 MHz, on", CLOCK_TX_CORE, true);
	if (!core_clock) {
		return false;
	}
	npl_attempted = true;
	npl_clock = afe_clock("the TX macro's NPL clock (0x30d) at 19.2 MHz, on", CLOCK_TX_NPL, true);
	return npl_clock;
}

static void
clocks_off(void)
{
	if (npl_attempted) {
		if (!afe_clock("the TX macro's NPL clock, off", CLOCK_TX_NPL, false)) cleanup_failed = true;
		npl_clock = npl_attempted = false;
	}
	if (core_attempted) {
		if (!afe_clock("the TX macro's core clock, off", CLOCK_TX_CORE, false)) cleanup_failed = true;
		core_clock = core_attempted = false;
	}
	devote(LPASS_HW_DCODEC_BLOCK);
	devote(LPASS_HW_MACRO_BLOCK);
}

/* ---- the blocks, read ------------------------------------------------------------------------------ */

#include "umi_mic_bob.h"

static struct block tx_macro = { "UMI TX macro", 0x03220000, 0x1000 };
static struct block va_macro = { "UMI VA macro", 0x03370000, 0x1000 };
static struct block tx_swr = { "UMI TX SoundWire master", 0x03230000, 0x2000 };
static struct block lpi = { "UMI LPASS pins 0..2", 0x033c0000, 0x3000 };
static struct block tlmm = { "UMI GPIO32 codec reset", 0x0f520000, 0x1000 };

/* ---- the codec's reset, the master's pins ------------------------------------------------------------ */

#define TLMM_RESET_CFG             0
#define TLMM_RESET_IO              4
#define TLMM_RESET_GPIO_CFG        0x3c0u /* GPIO mux0, 16mA, output, no pull */
static uint32_t found_reset_cfg, found_reset_io;
static bool reset_taken;

static bool
reset_release(void)
{
	found_reset_cfg = get(&tlmm, "reset config as found", TLMM_RESET_CFG);
	found_reset_io = get(&tlmm, "reset I/O as found", TLMM_RESET_IO);
	if (broken) return false;
	reset_taken = true;
	/* DT's "func2" is rejected by Android's Kona pinctrl (see MIC-umi.md).
	 * It is NOT a valid hardware mux2: GPIO32's slot2 is NA. GPIO mode0 is
	 * required for the reset output latch to reach the pad. Keep high bits. */
	if (!put(&tlmm, "gpio32 out (the codec's reset)", TLMM_RESET_IO, 0, 2) ||
	    !put(&tlmm, "gpio32 config: GPIO mux0, 16 mA, no pull, output", TLMM_RESET_CFG,
	    (found_reset_cfg & ~0x3ffu) | TLMM_RESET_GPIO_CFG, 0x3ff)) {
		return false;
	}
	/* Android requires >=20us low/high. Allow 1ms low and 10ms after high. */
	sleep_ms(1);
	uint32_t low = get(&tlmm, "reset pad low", TLMM_RESET_IO);
	if (!put(&tlmm, "gpio32 out: high (the codec out of reset)", TLMM_RESET_IO, 2, 2)) {
		return false;
	}
	sleep_ms(10);
	uint32_t high = get(&tlmm, "reset pad high", TLMM_RESET_IO);
	say("umi-mic: RESET PAD: mux0 low=%#x high=%#x (pad bit0, latch bit1)\n", low, high);
	if (broken || (low & 1) || (high & 3) != 3) {
		say("umi-mic: RESET PAD FAILED: GPIO32 did not sample low -> high; master untouched\n");
		return false;
	}
	say("umi-mic: RESET PAD PASS: GPIO32 sampled low -> high\n");
	return true;
}

static void
reset_restore(void)
{
	if (!reset_taken) {
		return;
	}
	broken = false;         /* (putting back is done whatever went before) */
	restore(&tlmm, TLMM_RESET_IO, 0, 2);
	restore(&tlmm, TLMM_RESET_CFG, found_reset_cfg, 0x3ff);
	restore(&tlmm, TLMM_RESET_IO, found_reset_io & 2, 2);
	reset_taken = false;
}

static struct block lpi_slew = { "the LPASS pins' slew rates", 0x0355a000, 0x1000 };
static const struct { unsigned pin, slew_bit; uint32_t pull; const char *name; } swr_pins[] = {
	{ 0, 0, 0, "gpio0 (SoundWire TX clock)" }, { 1, 2, 2, "gpio1 (data 0)" }, { 2, 4, 2, "gpio2 (data 1)" },
};
#define SWR_PINS                (sizeof(swr_pins) / sizeof(swr_pins[0]))
static uint32_t found_pin_cfg[SWR_PINS], found_pin_dir[SWR_PINS], found_slew;
static bool pins_taken;

static bool
pins_on(void)
{
	uint32_t slew;

	for (unsigned i = 0; i < SWR_PINS; i++) {
		found_pin_cfg[i] = get(&lpi, "pin config as found", 0x1000 * swr_pins[i].pin);
		found_pin_dir[i] = get(&lpi, "pin direction as found", 0x1000 * swr_pins[i].pin + 4);
	}
	found_slew = get(&lpi_slew, "slew as found", 0);
	if (broken) return false;
	pins_taken = true;
	slew = found_slew;
	for (unsigned i = 0; i < SWR_PINS; i++) {
		slew = (slew & ~(3u << swr_pins[i].slew_bit)) | (1u << swr_pins[i].slew_bit);
	}
	if (!put(&lpi_slew, "slew rates: 1 for the three", 0, slew, 0x3f)) {
		return false;
	}
	for (unsigned i = 0; i < SWR_PINS; i++) {
		/* [1:0] pull, [5:2] function, [8:6] drive (mA / 2 - 1), [9] output: function 1, 2 mA, not an output */
		uint32_t cfg = (found_pin_cfg[i] & ~0x3ffu) | swr_pins[i].pull | 1u << 2;

		if (!put(&lpi, swr_pins[i].name, 0x1000 * swr_pins[i].pin, cfg, 0x3ff) ||
		    !put(&lpi, "its direction: in", 0x1000 * swr_pins[i].pin + 4, 0, 2)) {
			return false;
		}
	}
	return true;
}

static void
pins_restore(void)
{
	if (!pins_taken) {
		return;
	}
	broken = false;
	for (unsigned i = 0; i < SWR_PINS; i++) {
		restore(&lpi, 0x1000 * swr_pins[i].pin, found_pin_cfg[i], 0x3ff);
		restore(&lpi, 0x1000 * swr_pins[i].pin + 4, found_pin_dir[i], 2);
	}
	restore(&lpi_slew, 0, found_slew, 0x3f);
	pins_taken = false;
}

/* ---- the TX macro's clocks, the SoundWire master ----------------------------------------------------- */

#define TX_MCLK_CONTROL         0x000
#define TX_FS_CNT_CONTROL       0x004
#define TX_SWR_CONTROL          0x008
#define TX_FREQ_MCLK            0x090
#define VA_MCLK_CONTROL         0x000
#define VA_FS_CNT_CONTROL       0x004
#define VA_TOP_CFG0             0x080

#define SWRM_COMP_CFG                   0x0004
#define SWRM_COMP_SW_RESET              0x0008
#define SWRM_COMP_STATUS                0x0014
#define SWRM_INTERRUPT_STATUS           0x0200
#define SWRM_INTERRUPT_EN               0x0204
#define SWRM_INTERRUPT_CLEAR            0x0208
#define SWRM_CPU1_INTERRUPT_EN          0x0210
#define SWRM_CMD_FIFO_WR_CMD            0x0300
#define SWRM_CMD_FIFO_RD_CMD            0x0304
#define SWRM_CMD_FIFO_CMD               0x0308
#define SWRM_CMD_FIFO_STATUS            0x030c
#define SWRM_CMD_FIFO_CFG               0x0314
#define SWRM_CMD_FIFO_RD_FIFO           0x0318
#define SWRM_ENUMERATOR_CFG             0x0500
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_1(m) (0x0530 + 0x8 * (m))
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_2(m) (0x0534 + 0x8 * (m))
#define SWRM_MCP_FRAME_CTRL_BANK0       0x101c
#define SWRM_MCP_BUS_CTRL               0x1044
#define SWRM_MCP_CFG                    0x1048
#define SWRM_MCP_SLV_STATUS             0x1090
#define SWRM_INTERRUPT_STATUS_MASK      0x1fdfd
#define SWR_BUS_HZ                      9600000         /* MCLK_FREQ: FREQ_MCLK 1 is 9.6 MHz */
#define SWR_ROWS                        50
#define SWR_COLUMNS                     2
#define SWR_FRAME_SYNC_HZ               4000
#define CODEC_TX_ID                     0x000d01170223ull       /* the tree's wcd938x-tx-slave */
#define SWRS_SCP_INT_STATUS_MASK_1      0x41

static bool tx_clocks_taken, master_taken;
static uint32_t found_tx_clock[4], found_fs[3];
static uint8_t read_id = 0;

static bool
tx_clocks_on(void)
{
	const uint32_t tx[] = { TX_MCLK_CONTROL, TX_FS_CNT_CONTROL, TX_SWR_CONTROL, TX_FREQ_MCLK };
	const uint32_t va[] = { VA_MCLK_CONTROL, VA_FS_CNT_CONTROL, VA_TOP_CFG0 };
	for (unsigned i = 0; i < 4; i++) found_tx_clock[i] = get(&tx_macro, "clock as found", tx[i]);
	for (unsigned i = 0; i < 3; i++) found_fs[i] = get(&va_macro, "fs-gen as found", va[i]);
	if (broken) return false;
	tx_clocks_taken = true;
	/* bolero_clk_rsc_fs_gen_request(): the tree's qcom,fs-gen-sequence, register, mask, value */
	return put_bits(&va_macro, "MCLK_CONTROL (fs-gen)", VA_MCLK_CONTROL, 0x01, 0x01) &&
	       put_bits(&va_macro, "FS_CNT_CONTROL (fs-gen)", VA_FS_CNT_CONTROL, 0x01, 0x01) &&
	       put_bits(&va_macro, "TOP_CFG0 (fs-gen)", VA_TOP_CFG0, 0x02, 0x02) &&
	       /* FREQ_MCLK readback was unreliable on Redwood; do not assume it on UMI. */
	       put(&tx_macro, "FREQ_MCLK: 9.6 MHz (readback not enforced)", TX_FREQ_MCLK, 1, 0) &&
	       put_bits(&tx_macro, "MCLK_CONTROL: on", TX_MCLK_CONTROL, 0x01, 0x01) &&
	       put_bits(&tx_macro, "FS_CNT_CONTROL: on", TX_FS_CNT_CONTROL, 0x01, 0x01) &&
	       /* tx_macro_tx_va_mclk_enable(): the SoundWire clock, with the first reset */
	       put_bits(&tx_macro, "SWR_CONTROL: reset", TX_SWR_CONTROL, 0x02, 0x02) &&
	       put_bits(&tx_macro, "SWR_CONTROL: clock on", TX_SWR_CONTROL, 0x01, 0x01) &&
	       put_bits(&tx_macro, "SWR_CONTROL: reset over", TX_SWR_CONTROL, 0x02, 0x00);
}

static void
tx_clocks_off(void)
{
	if (!tx_clocks_taken) return;
	broken = false;
	const uint32_t tx[] = { TX_MCLK_CONTROL, TX_FS_CNT_CONTROL, TX_SWR_CONTROL, TX_FREQ_MCLK };
	const uint32_t va[] = { VA_MCLK_CONTROL, VA_FS_CNT_CONTROL, VA_TOP_CFG0 };
	const unsigned order[] = { 2, 1, 0, 3 };
	for (unsigned i = 0; i < 4; i++) restore(&tx_macro, tx[order[i]], found_tx_clock[order[i]], 0);
	for (int i = 2; i >= 0; i--) restore(&va_macro, va[i], found_fs[i], 0);
	tx_clocks_taken = false;
}

static struct block *swr_bus = &tx_swr;

static uint32_t
swr_rd(uint32_t offset)
{
	if (!mapped(swr_bus, offset)) return 0;
	return rd(swr_bus->base + offset);
}

static bool
master_on(void)
{
	uint32_t ssp = SWR_BUS_HZ * 2 / (SWR_ROWS * SWR_COLUMNS * SWR_FRAME_SYNC_HZ), cfg;
	uint32_t version = swr_rd(0);
	say("umi-mic: SWR version %#x (UMI expects 0x1050001); no HCTL access\n", version);
	if (version != 0x01050001) {
		say("umi-mic: unexpected SWR version; master untouched\n");
		return false;
	}
	master_taken = true;
	/* swrm_runtime_resume() */
	if (!put(swr_bus, "COMP_SW_RESET", SWRM_COMP_SW_RESET, 1, 0) || !put(swr_bus, "COMP_SW_RESET", SWRM_COMP_SW_RESET, 1, 0)) {
		return false;
	}
	/* swrm_master_init() */
	cfg = (swr_rd(SWRM_MCP_CFG) & ~0x3e0000u) | 0x1fu << 17;
	if (!put(swr_bus, "MCP_FRAME_CTRL_BANK0: 50 rows, 2 columns", SWRM_MCP_FRAME_CTRL_BANK0, 1u << 3 | 0u | (ssp - 1) << 16,
	    0xffffffff) ||
	    !put(swr_bus, "ENUMERATOR_CFG: auto", SWRM_ENUMERATOR_CFG, 1, 1) ||
	    !put(swr_bus, "MCP_CFG: 31 pings", SWRM_MCP_CFG, cfg, 0x3e0000) ||
	    !put(swr_bus, "CMD_FIFO_CFG: 3 retries", SWRM_CMD_FIFO_CFG, 3, 0xff) ||
	    !put(swr_bus, "MCP_BUS_CTRL", SWRM_MCP_BUS_CTRL, 2, 0) ||
	    !put(swr_bus, "COMP_CFG: interrupts as pulses", SWRM_COMP_CFG, 2, 0) ||
	    !put(swr_bus, "COMP_CFG: on", SWRM_COMP_CFG, 3, 0) ||
	    !put(swr_bus, "INTERRUPT_CLEAR", SWRM_INTERRUPT_CLEAR, 0xffffffff, 0) ||
	    !put(swr_bus, "INTERRUPT_EN", SWRM_INTERRUPT_EN, SWRM_INTERRUPT_STATUS_MASK, 0) ||
	    !put(swr_bus, "CPU1_INTERRUPT_EN", SWRM_CPU1_INTERRUPT_EN, SWRM_INTERRUPT_STATUS_MASK, 0)) {
		return false;
	}
	say("umi-mic: SWR v1.5.1 initialized; enumeration, not COMP_STATUS, proves link\n");

	return put(swr_bus, "CMD_FIFO_CFG: | 0x80000000", SWRM_CMD_FIFO_CFG, swr_rd(SWRM_CMD_FIFO_CFG) | 0x80000000u, 0);
}

static void
master_off(void)
{
	if (!master_taken) {
		return;
	}
	broken = false;
	restore(swr_bus, SWRM_COMP_CFG, 0, 0);
	restore(swr_bus, SWRM_INTERRUPT_EN, 0, 0);
	restore(swr_bus, SWRM_CPU1_INTERRUPT_EN, 0, 0);
	restore(swr_bus, SWRM_COMP_SW_RESET, 1, 0);
	master_taken = false;
}

/* A command to a device on the bus (SWR_REG_VAL_PACK): a register written. */
static bool
swr_write(uint8_t device, uint16_t reg, uint8_t value, uint8_t id)
{
	uint32_t command = reg | (uint32_t)id << 16 | (uint32_t)device << 20 | (uint32_t)value << 24;

	if (broken || !mapped(swr_bus, SWRM_CMD_FIFO_WR_CMD)) return false;
	for (int i = 0; i < 20 && ((swr_rd(SWRM_CMD_FIFO_STATUS) >> 8) & 0x1f) >= 16; i++) {
		spin_us(500);
	}
	if (((swr_rd(SWRM_CMD_FIFO_STATUS) >> 8) & 0x1f) >= 16) {
		say("umi-mic: SoundWire write FIFO full: STOPPING HERE\n");
		broken = true;
		return false;
	}
	snprintf(last_write, sizeof(last_write), "SoundWire: device %u register %#x <- %#x", device, reg, value);
	wr(swr_bus->base + SWRM_CMD_FIFO_WR_CMD, command);
	spin_us(155);
	return true;
}

/* ... and one read: false if the answer does not come (or is not this command's) */
static bool
swr_read(uint8_t device, uint16_t reg, uint8_t *value)
{
	uint32_t answer = 0;

	if (broken || !mapped(swr_bus, SWRM_CMD_FIFO_RD_CMD)) return false;
	read_id = read_id < 14 ? read_id + 1 : 0;
	/* umi-q-oss swrm_cmd_fifo_rd_cmd(): delays plus matching response ID.
	 * Do not assume Redwood's undocumented response-FIFO count field. */
	spin_us(105);
	wr(swr_bus->base + SWRM_CMD_FIFO_RD_CMD, reg | (uint32_t)read_id << 16 | (uint32_t)device << 20 | 1u << 24);
	spin_us(255);
	for (int tries = 0; tries < 4; tries++) {
		answer = swr_rd(SWRM_CMD_FIFO_RD_FIFO);
		if (((answer >> 8) & 0xf) == read_id) {
			*value = (uint8_t)answer;
			return true;
		}
		spin_us(505);
	}
	say("umi-mic: SoundWire: device %u register %#x: no answer (fifo %#x, last %#x)\n", device, reg, swr_rd(SWRM_CMD_FIFO_STATUS),
	    answer);
	broken = true;
	return false;
}

static uint8_t codec_device;

/* the codec's TX device, enumerated: its device number, 0 if it did not show up */
static uint8_t
codec_wait(void)
{
	for (int waited = 0; waited < 1000; waited++) {
		for (uint8_t d = 1; d <= 11; d++) {
			uint64_t id = (uint64_t)swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_2(d)) << 32 | swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_1(d));
			uint32_t status = (swr_rd(SWRM_MCP_SLV_STATUS) >> (2 * d)) & 3;

			if ((id & 0xffffffffffffull) == CODEC_TX_ID && (status == 1 || status == 2)) {
				say("umi-mic: the codec's TX device is device %u on the bus (ID %#llx, status %u) after %d ms\n", d,
				    id & 0xffffffffffffull, status, waited);
				return d;
			}
		}
		if (waited == 0 || waited == 100 || waited == 500) {
			say("umi-mic: ENUM wait %d ms: slaves=%#x irq=%#x comp=%#x fifo=%#x\n", waited,
			    swr_rd(SWRM_MCP_SLV_STATUS), swr_rd(SWRM_INTERRUPT_STATUS),
			    swr_rd(SWRM_COMP_STATUS), swr_rd(SWRM_CMD_FIFO_STATUS));
		}
		sleep_ms(1);
	}
	say("umi-mic: the codec's TX device did not show up; the master has seen:\n");
	for (uint8_t d = 1; d <= 11; d++) {
		uint64_t id = (uint64_t)swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_2(d)) << 32 | swr_rd(SWRM_ENUMERATOR_SLAVE_DEV_ID_1(d));

		if (id != 0) {
			say("umi-mic:   device %u: ID %#llx\n", d, id);
		}
	}
	say("umi-mic:   slave status %#x\n", swr_rd(SWRM_MCP_SLV_STATUS));
	say("umi-mic: ENUM FAILED after 1000 ms: irq=%#x comp=%#x fifo=%#x reset-pad=%#x\n",
	    swr_rd(SWRM_INTERRUPT_STATUS), swr_rd(SWRM_COMP_STATUS), swr_rd(SWRM_CMD_FIFO_STATUS),
	    get(&tlmm, "reset pad at enumeration failure", TLMM_RESET_IO));
	return 0;
}

static bool
codec_look(void)
{
	static const uint16_t regs[] = { 0x3401, 0x3402, 0x3403, 0x3404, 0x34b0 };
	uint8_t value = 0;
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		if (!swr_read(codec_device, regs[i], &value)) return false;
		say("umi-mic: WCD chip register %#x = %#x\n", regs[i], value);
	}
	if (((value & 0x1e) >> 1) != 5) {
		say("umi-mic: codec variant is not WCD9385: STOPPING HERE\n");
		return false;
	}
	say("umi-mic: ENUMERATION PASS: WCD9385 TX slave %#llx\n", CODEC_TX_ID);
	return true;
}

/* ---- umi_mic=3: the codec set up as Android sets it up for the main microphone ----------------------------- */

#define WCD_ANA_BIAS                    0x3001
#define WCD_ANA_TX_CH1                  0x300e
#define WCD_ANA_TX_CH2                  0x300f
#define WCD_ANA_MICB1                   0x3022
#define WCD_ANA_MICB2                   0x3023
#define WCD_ANA_MICB3                   0x3025
#define WCD_ANA_MICB4                   0x3026
#define WCD_BIAS_VBG_FINE_ADJ           0x3029
#define WCD_MICB1_TEST_CTL_1            0x306b
#define WCD_MICB1_TEST_CTL_2            0x306c
#define WCD_MICB2_TEST_CTL_1            0x306e
#define WCD_MICB2_TEST_CTL_2            0x306f
#define WCD_MICB3_TEST_CTL_1            0x3071
#define WCD_MICB3_TEST_CTL_2            0x3072
#define WCD_MICB4_TEST_CTL_1            0x3074
#define WCD_MICB4_TEST_CTL_2            0x3075
#define WCD_TX_3_4_TEST_BLK_EN2         0x308f
#define WCD_HPH_REFBUFF_UHQA_CTL        0x30dd
#define WCD_HPH_SURGE_HPHLR_SURGE_EN    0x30e2
#define WCD_SLEEP_CTL                   0x3103
#define WCD_LDORXTX_CONFIG              0x312a
#define WCD_HPH_NEW_INT_RDAC_GAIN_CTL   0x3132
#define WCD_HPH_NEW_INT_RDAC_HD2_CTL_L  0x3140
#define WCD_HPH_NEW_INT_RDAC_HD2_CTL_R  0x3141
#define WCD_TXFE_ICTRL_STG2MAIN_ULP     0x31df
#define WCD_TXFE_ICTRL_STG2CASC_ULP     0x31e1
#define WCD_SWR_TX_CLK_RATE             0x3405
#define WCD_CDC_ANA_CLK_CTL             0x3408
#define WCD_CDC_DIG_CLK_CTL             0x3409
#define WCD_CDC_TX_ANA_MODE_0_1         0x3410
#define WCD_CDC_ANA_TX_CLK_CTL          0x3417
#define WCD_CDC_REQ_CTL                 0x3457
#define WCD_CDC_DMIC_CTL                0x345b
#define WCD_INTR_LEVEL_0                0x3474
#define WCD_EFUSE_REG_30                0x34ce
#define WCD_TX_REQ_FB_CTL_0             0x34d0
#define SWRS_SCP_CONTROL                0x0044
#define SWRS_SCP_FRAME_CTRL_BANK(m)     (0x0060 + 0x10 * (m))
#define SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(m) (0x00e0 + 0x10 * (m))
#define SWRS_DP_CHANNEL_ENABLE_BANK(n, m) (0x120 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_SAMPLE_CONTROL_1_BANK(n, m) (0x122 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_SAMPLE_CONTROL_2_BANK(n, m) (0x123 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_OFFSET_CONTROL_1_BANK(n, m) (0x124 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_OFFSET_CONTROL_2_BANK(n, m) (0x125 + 0x100 * (n) + 0x10 * (m))
#define SWRS_DP_LANE_CONTROL_BANK(n, m) (0x128 + 0x100 * (n) + 0x10 * (m))
#define SWRM_MCP_STATUS                 0x104c
#define SWRM_MCP_FRAME_CTRL_BANK(m)     (0x101c + 0x40 * (m))
#define SWRM_DP_PORT_CTRL_BANK(n, m)    (0x1024 + 0x100 * (n) + 0x40 * (m))
#define SWRM_DP_PORT_CTRL_2_BANK(n, m)  (0x1028 + 0x100 * (n) + 0x40 * (m))
#define SWRM_DP_PORT_HCTRL_BANK(n, m)   (0x1034 + 0x100 * (n) + 0x40 * (m))
#define SWRM_DP_SAMPLECTRL2_BANK(n, m)  (0x103c + 0x100 * (n) + 0x40 * (m))

static uint8_t write_id;

/* a register of the codec's (device codec_device on the bus) written and, unless `check' is 0, read back */
static bool
codec_put(uint16_t reg, uint8_t value, uint8_t check)
{
	uint8_t back = 0;

	if (broken) {
		return false;
	}
	write_id = write_id < 14 ? write_id + 1 : 0;
	if (!swr_write(codec_device, reg, value, write_id)) return false;
	if (check == 0) {
		note("umi-mic: the codec's %#06x <- %#04x\n", reg, value);
		return true;
	}
	if (!swr_read(codec_device, reg, &back) || (back & check) != (value & check)) {
		say("umi-mic: the codec's %#06x <- %#04x, but it reads %#04x: STOPPING HERE\n", reg, value, back);
		broken = true;
		return false;
	}
	note("umi-mic: the codec's %#06x <- %#04x, reads back\n", reg, value);
	return true;
}

static bool
codec_bits(uint16_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now = 0;

	if (broken || !swr_read(codec_device, reg, &now)) {
		broken = true;
		return false;
	}
	return (now & mask) == (value & mask) || codec_put(reg, (uint8_t)((now & ~mask) | (value & mask)), mask);
}

/* a command to all devices on the bus (device 15, command id 15: Android's broadcasts) */
static void
broadcast(uint16_t reg, uint8_t value)
{
	swr_write(0xf, reg, value, 0xf);
	note("umi-mic: SoundWire, to all: %#06x <- %#04x\n", reg, value);
	sleep_ms(10);           /* ("10 ms for the broadcast to complete", swrm_cmd_fifo_wr_cmd()) */
}

/* to all devices, with an ordinary command id (swrm_cmd_fifo_wr_cmd(..., 0xF, 0x00, ...)) */
static void
to_all(uint16_t reg, uint8_t value)
{
	write_id = write_id < 14 ? write_id + 1 : 0;
	swr_write(0xf, reg, value, write_id);
	note("umi-mic: SoundWire, to all: %#06x <- %#04x\n", reg, value);
}

/* wcd938x_bind(), wcd938x_set_micbias_data(), wcd938x_init_reg() */
static bool codec_initialized;

static bool
codec_init(void)
{
	codec_initialized = true;
	/* the tree's qcom,cdc-micbias1..4-mv: 2750 mV, (2750 - 1000) / 50 */
	const uint8_t vout = (2750 - 1000) / 50;

	if (!codec_put(WCD_INTR_LEVEL_0, 0, 0xff) || !codec_put(WCD_INTR_LEVEL_0 + 1, 0, 0xff) ||
	    !codec_put(WCD_INTR_LEVEL_0 + 2, 0, 0xff) ||
	    !codec_bits(WCD_ANA_MICB1, 0x3f, vout) || !codec_bits(WCD_ANA_MICB2, 0x3f, vout) ||
	    !codec_bits(WCD_ANA_MICB3, 0x3f, vout) || !codec_bits(WCD_ANA_MICB4, 0x3f, vout) ||
	    !codec_bits(WCD_SLEEP_CTL, 0x0e, 0x0e) || !codec_bits(WCD_SLEEP_CTL, 0x80, 0x80)) {
		return false;
	}
	sleep_ms(1);
	if (!codec_bits(WCD_SLEEP_CTL, 0x40, 0x40)) {
		return false;
	}
	sleep_ms(1);
	if (!codec_bits(WCD_LDORXTX_CONFIG, 0x10, 0x00) || !codec_bits(WCD_BIAS_VBG_FINE_ADJ, 0xf0, 0x80) ||
	    !codec_bits(WCD_ANA_BIAS, 0x80, 0x80) || !codec_bits(WCD_ANA_BIAS, 0x40, 0x40)) {
		return false;
	}
	sleep_ms(10);
	if (!codec_bits(WCD_ANA_BIAS, 0x40, 0x00) || !codec_bits(WCD_HPH_NEW_INT_RDAC_GAIN_CTL, 0xf0, 0x00) ||
	    !codec_bits(WCD_HPH_NEW_INT_RDAC_HD2_CTL_L, 0x1f, 0x15) || !codec_bits(WCD_HPH_NEW_INT_RDAC_HD2_CTL_R, 0x1f, 0x15) ||
	    !codec_bits(WCD_HPH_REFBUFF_UHQA_CTL, 0xc0, 0x80) || !codec_bits(WCD_CDC_DMIC_CTL, 0x02, 0x02) ||
	    !codec_bits(WCD_TXFE_ICTRL_STG2CASC_ULP, 0xff, 0x14) || !codec_bits(WCD_TXFE_ICTRL_STG2MAIN_ULP, 0x1f, 0x08) ||
	    !codec_bits(WCD_TX_REQ_FB_CTL_0, 0xff, 0x55) || !codec_bits(WCD_TX_REQ_FB_CTL_0 + 1, 0xff, 0x44) ||
	    !codec_bits(WCD_TX_REQ_FB_CTL_0 + 2, 0xff, 0x11) || !codec_bits(WCD_TX_REQ_FB_CTL_0 + 3, 0xff, 0x00) ||
	    !codec_bits(WCD_TX_REQ_FB_CTL_0 + 4, 0xff, 0x00) ||
	    !codec_bits(WCD_MICB1_TEST_CTL_1, 0xe0, 0xe0) || !codec_bits(WCD_MICB2_TEST_CTL_1, 0xe0, 0xe0) ||
	    !codec_bits(WCD_MICB3_TEST_CTL_1, 0xe0, 0xe0) || !codec_bits(WCD_MICB4_TEST_CTL_1, 0xe0, 0xe0) ||
	    !codec_bits(WCD_TX_3_4_TEST_BLK_EN2, 0x01, 0x00)) {
		return false;
	}
	say("umi-mic: codec init: Xiaomi umi-q-oss wcd938x_init_reg() done\n");
	return true;
}

/* All touched codec widgets are off before releasing an owned BOB vote.
 * Reset_restore() restores the original GPIO pad, which may be HIGH; it is
 * not a substitute for explicitly disabling MICBIAS1 here.
 */
static void
codec_off(void)
{
	if (!codec_initialized) return;
	static const struct { uint16_t reg; uint8_t mask; } off[] = {
		{ WCD_ANA_MICB1, 0xc0 }, { WCD_CDC_TX_ANA_MODE_0_1, 0x0f },
		{ WCD_CDC_DIG_CLK_CTL, 0xf0 }, { WCD_CDC_ANA_CLK_CTL, 0x18 },
		{ WCD_CDC_ANA_TX_CLK_CTL, 0x01 },
	};
	for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
		broken = false;
		if (!codec_bits(off[i].reg, off[i].mask, 0)) cleanup_failed = true;
	}
	codec_initialized = false;
	say("umi-mic: ADC1/MICBIAS1 off before SoundWire/BOB cleanup\n");
}

/* Kona default table: ADC1 slave port0/DP1 -> master2, mask1,
 * SI=1, offset1=1, offset2=0, lane0, bus 9.6MHz, 50x16.
 * asoc/kona-port-config.h tx_frame_params_default[1]; live DT port maps.
 */
static uint8_t bank_now;

static bool
port_config(uint8_t bank, bool first)
{
	if (first) to_all(SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(bank), 1);
	if (!codec_put(SWRS_DP_CHANNEL_ENABLE_BANK(0, bank), 1, 0xff) ||
	    !codec_put(SWRS_DP_SAMPLE_CONTROL_1_BANK(0, bank), 1, 0xff) ||
	    !codec_put(SWRS_DP_OFFSET_CONTROL_1_BANK(0, bank), 1, 0xff) ||
	    !codec_put(SWRS_DP_OFFSET_CONTROL_2_BANK(0, bank), 0, 0xff) ||
	    !codec_put(SWRS_DP_LANE_CONTROL_BANK(0, bank), 0, 0xff)) return false;
	return put(&tx_swr, "master port2 channel1 SI1 offsets1,0", SWRM_DP_PORT_CTRL_BANK(2, bank),
	           0x01000101, 0xffffffff) &&
	       put(&tx_swr, "master port2 lane0", SWRM_DP_PORT_CTRL_2_BANK(2, bank), 0, 0xff) &&
	       put(&tx_swr, "master port2 HCTRL", SWRM_DP_PORT_HCTRL_BANK(2, bank), 0xf0, 0);
}

static bool
datapath_on(void)
{
	uint8_t bank = (swr_rd(SWRM_MCP_STATUS) & 1) ? 0 : 1;
	if (!codec_bits(WCD_SWR_TX_CLK_RATE, 0xff, 0) || !port_config(bank, true)) return false;
	uint32_t frame = (swr_rd(SWRM_MCP_FRAME_CTRL_BANK(bank)) & ~0xff07ffu) | 1u << 3 | 7u | 5u << 16;
	if (!put(&tx_swr, "Kona 50x16, 9.6MHz", SWRM_MCP_FRAME_CTRL_BANK(bank), frame, 0xff07ff)) return false;
	broadcast(SWRS_SCP_FRAME_CTRL_BANK(bank), 1u << 3 | 7u);
	bank_now = swr_rd(SWRM_MCP_STATUS) & 1;
	say("umi-mic: bank switch: asked %u, got %u\n", bank, bank_now);
	if (broken || bank_now != bank || !port_config(bank ? 0 : 1, false)) return false;
	say("umi-mic: ADC1 DP1 -> SWR master2 ch1, SI1 offset1 lane0, 9.6MHz\n");
	return true;
}

/* "MIC BIAS1" (wcd938x_micbias_control(MICB_ENABLE)), then ADC1's request (wcd938x_enable_req()) in normal mode */
static bool
adc1_on(void)
{
	return codec_bits(WCD_CDC_DIG_CLK_CTL, 0xf0, 0xf0) && codec_bits(WCD_CDC_ANA_CLK_CTL, 0x10, 0x10) &&
	       codec_bits(WCD_CDC_ANA_TX_CLK_CTL, 0x01, 0x01) && codec_bits(WCD_MICB1_TEST_CTL_2, 0x01, 0x01) &&
	       codec_bits(WCD_MICB2_TEST_CTL_2, 0x01, 0x01) && codec_bits(WCD_MICB3_TEST_CTL_2, 0x01, 0x01) &&
	       codec_bits(WCD_MICB4_TEST_CTL_2, 0x01, 0x01) && codec_bits(WCD_ANA_MICB1, 0xc0, 0x40) &&
	       /* "ADC1 Volume" 6 (Xiaomi's mixer paths) */
	       codec_bits(WCD_ANA_TX_CH1, 0x1f, 6) &&
	       codec_bits(WCD_CDC_ANA_CLK_CTL, 0x08, 0x08) && codec_bits(WCD_CDC_ANA_CLK_CTL, 0x10, 0x10) &&
	       codec_bits(WCD_CDC_REQ_CTL, 0x02, 0x02) && codec_bits(WCD_CDC_REQ_CTL, 0x01, 0x00) &&
	       codec_bits(WCD_ANA_TX_CH2, 0x40, 0x40) && codec_bits(WCD_CDC_TX_ANA_MODE_0_1, 0x0f, 0x03) &&
	       codec_bits(WCD_CDC_DIG_CLK_CTL, 0x10, 0x10) && codec_bits(WCD_ANA_TX_CH2, 0x40, 0x00);
}

/* ---- the TX macro's decimator 1 ------------------------------------------------------------------------- */

#define TX_ADC_MUX1_CFG0                0x108
#define TX_ADC_MUX1_CFG1                0x10c
#define TX0_PATH_SEC7                   0x42c
#define TX1_PATH_CTL                    0x480
#define TX1_PATH_CFG0                   0x484
#define TX1_VOL_CTL                     0x48c
#define TX1_PATH_SEC2                   0x498

static uint32_t found_dec[7];
static bool dec_taken;
static uint8_t hpf_cut_off;
static const uint32_t dec_regs[] = {
	TX_ADC_MUX1_CFG0, TX_ADC_MUX1_CFG1, TX1_PATH_CTL, TX1_PATH_CFG0,
	TX1_VOL_CTL, TX1_PATH_SEC2, TX0_PATH_SEC7,
};

/* Bolero 1.2: "TX SMIC MUX1" ADC0, "TX DEC1 MUX" SWR_MIC, rate and decimator PMU. */
static bool
dec1_on(void)
{
	uint32_t cfg0;

	for (size_t i = 0; i < sizeof(dec_regs) / sizeof(dec_regs[0]); i++) {
		found_dec[i] = get(&tx_macro, "(as found)", dec_regs[i]);
	}
	if (broken) return false;
	dec_taken = true;
	if (!put_bits(&tx_macro, "Bolero1.2 init TX0_SEC7", TX0_PATH_SEC7, 0x3f, 0x06) ||
	    !put_bits(&tx_macro, "TX1 HPF enabled", TX1_PATH_SEC2, 0x01, 0x00) ||
	    !put_bits(&tx_macro, "TX1_TX_PATH_CFG0: an analog mic", TX1_PATH_CFG0, 0x80, 0x00) ||
	    !put_bits(&tx_macro, "ADC_MUX1_CFG0: UMI ADC0 (WCD ADC1)", TX_ADC_MUX1_CFG0,
	    0x0f, 1) ||
	    !put_bits(&tx_macro, "ADC_MUX1_CFG1: SWR_MIC", TX_ADC_MUX1_CFG1, 0x03, 0x01) ||
	    !put_bits(&tx_macro, "TX1_TX_PATH_CTL: 48 kHz", TX1_PATH_CTL, 0x0f, 0x04) ||
	    /* PRE_PMU: the decimator's mode (0), its PGA muted */
	    !put_bits(&tx_macro, "TX1_TX_PATH_CFG0: mode 0", TX1_PATH_CFG0, 0x06, 0x00) ||
	    !put_bits(&tx_macro, "TX1_TX_PATH_CTL: muted", TX1_PATH_CTL, 0x10, 0x10) ||
	    /* POST_PMU: on; the high-pass filter at 150 Hz while it settles */
	    !put_bits(&tx_macro, "TX1_TX_PATH_CTL: on", TX1_PATH_CTL, 0x20, 0x20)) {
		return false;
	}
	cfg0 = get(&tx_macro, "TX1_TX_PATH_CFG0", TX1_PATH_CFG0);
	hpf_cut_off = (cfg0 & 0x60) >> 5;
	if (hpf_cut_off != 2 && !put_bits(&tx_macro, "TX1_TX_PATH_CFG0: high-pass 150 Hz", TX1_PATH_CFG0, 0x60, 2u << 5)) {
		return false;
	}
	if (hpf_cut_off != 2) {
		if (!put_bits(&tx_macro, "TX1_TX_PATH_SEC2: filter gate", TX1_PATH_SEC2, 0x03, 0x02)) {
			return false;
		}
		sleep_ms(6);
		if (!put_bits(&tx_macro, "TX1_TX_PATH_SEC2", TX1_PATH_SEC2, 0x02, 0x00)) {
			return false;
		}
	}
	/* the gain, written again after the decimator is on ("TX_DEC1 Volume" 84: 0 dB) */
	if (!put_bits(&tx_macro, "TX1_TX_VOL_CTL: 0 dB", TX1_VOL_CTL, 0xff, 0)) {
		return false;
	}
	return true;
}

/* after TX_MACRO_AMIC_UNMUTE_DELAY_MS: the PGA unmuted */
static bool
dec1_unmute(void)
{
	return put_bits(&tx_macro, "TX1_TX_PATH_CTL: unmuted", TX1_PATH_CTL, 0x10, 0x00);
}

/* after TX_MACRO_AMIC_HPF_DELAY_MS: the codec's hold on ADC1 let go, the filter back to where it was */
static bool
dec1_settled(void)
{
	return codec_bits(WCD_ANA_TX_CH2, 0x40, 0x00) &&
	       put_bits(&tx_macro, "TX1_TX_PATH_CFG0: high-pass as it was", TX1_PATH_CFG0, 0x60, (uint32_t)hpf_cut_off << 5) &&
	       put_bits(&tx_macro, "TX1_TX_PATH_SEC2: filter gate", TX1_PATH_SEC2, 0x03, 0x02) &&
	       (spin_us(21), put_bits(&tx_macro, "TX1_TX_PATH_SEC2", TX1_PATH_SEC2, 0x03, 0x01));
}

static void
dec1_off(void)
{
	if (!dec_taken) {
		return;
	}
	broken = false;
	(void)put_bits(&tx_macro, "TX1_TX_PATH_CTL: muted", TX1_PATH_CTL, 0x10, 0x10);
	if (broken) cleanup_failed = true;
	broken = false;
	(void)put_bits(&tx_macro, "TX1_TX_PATH_CTL: off", TX1_PATH_CTL, 0x20, 0x00);
	if (broken) cleanup_failed = true;
	for (size_t i = 0; i < sizeof(dec_regs) / sizeof(dec_regs[0]); i++) {
		restore(&tx_macro, dec_regs[i], found_dec[i], 0xffffffff);
	}
	dec_taken = false;
}

/* ---- the DSP: the codec DMA port, a device path for recording, a stream that is read ------------------ */

#define APR_SVC_ASM                             7
#define APR_SVC_ADM                             8
#define AFE_PORT_CMD_DEVICE_START               0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP                0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2               0x000100efu
#define AFE_MODULE_AUDIO_DEV_INTERFACE          0x0001020cu
#define AFE_PARAM_ID_CODEC_DMA_CONFIG           0x000102b8u
#define MIC_PORT                                0xb037          /* TX_CDC_DMA_TX_3 */
#define ADM_CMD_DEVICE_OPEN_V5                  0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5               0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5                 0x00010327u
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5          0x00010325u
#define ADM_PATH_LIVE_RECORDING                 2
#define ADM_MATRIX_AUDIO_TX                     1
#define ADM_TOPOLOGY_NONE                       0x00010312u
#define ASM_CMD_SHARED_MEM_MAP_REGIONS          0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS       0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS        0x00010d94u
#define ASM_STREAM_CMD_OPEN_READ_V3             0x00010db4u
#define ASM_STREAM_CMD_SET_ENCDEC_PARAM         0x00010c10u
#define ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2      0x00010da3u
#define ASM_SESSION_CMD_RUN_V2                  0x00010daau
#define ASM_DATA_CMD_READ_V2                    0x00010dacu
#define ASM_DATA_EVENT_READ_DONE_V2             0x00010d9au
#define ASM_STREAM_CMD_CLOSE                    0x00010bcdu
#define ASM_TOPOLOGY_NONE                       0x00010c68u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2      0x00010da5u
#define ASM_SHMEM_4K_POOL                       3
#define MIC_SESSION                             2               /* (the speakers' is 1) */
#define MIC_STREAM_PORT                         (MIC_SESSION << 8 | 1)
#define READ_TOKEN                              0x40000000u     /* | the period */
#define PCM_CHANNEL_FC                          3
#define MIC_RATE                                48000
#define MIC_PERIODS                             8
#define MIC_PERIOD_FRAMES                       960             /* 20 ms, one channel of 16 bits */
#define MIC_PERIOD_BYTES                        (MIC_PERIOD_FRAMES * 2)

static uintptr_t mic_memory;
static uint64_t mic_address;
static bool memory_taken, mic_mapped, mic_copp_open, mic_stream_open, mic_port_on;
/* Lost map/ADM-open replies leave no handle to close. Keep the SMMU mapping. */
static bool dsp_unknown;
static uint32_t mic_map_handle, mic_copp;
static volatile uint32_t read_ready, read_errors, reads_done;
static volatile uint32_t read_sizes[MIC_PERIODS];
static volatile uint32_t read_offsets[MIC_PERIODS];
static uint16_t read_poison[MIC_PERIODS];
static uint32_t read_serial;

static bool
mic_events(uint8_t service, __unused uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
	if (service != APR_SVC_ASM || !(token & READ_TOKEN) || (token & 0x3fffffff) >= MIC_PERIODS) {
		return false;
	}
	if (opcode == ASM_DATA_EVENT_READ_DONE_V2) {
		unsigned period = token & 0x3fffffff;
		uint64_t expected = mic_address + 4096 * period;
		bool valid = length >= 24 && payload[0] == 0 &&
		    payload[1] == (uint32_t)expected && payload[2] == (uint32_t)(expected >> 32) &&
		    payload[3] == mic_map_handle && payload[5] <= MIC_PERIOD_BYTES &&
		    payload[4] <= MIC_PERIOD_BYTES - payload[5] && !(payload[4] & 1) && !(payload[5] & 1);
		if (!valid) {
			OSIncrementAtomic((volatile SInt32 *)&read_errors);
		}
		read_offsets[period] = valid ? payload[5] : 0;
		read_sizes[period] = valid ? payload[4] : 0;
		OSIncrementAtomic((volatile SInt32 *)&reads_done);
		OSBitOrAtomic(1u << (token & 31), &read_ready);
		return true;
	}
	if (opcode == APR_BASIC_RSP_RESULT) {
		OSIncrementAtomic((volatile SInt32 *)&read_errors);
		OSBitOrAtomic(1u << (token & 31), &read_ready);
		read_sizes[token & 0x3fffffff] = 0;
		return true;
	}
	return false;
}

static bool
mic_read(unsigned period)
{
	/* Every submission receives a fresh, nonzero poison word. A completed
	 * buffer still equal to this word was not overwritten by capture DMA.
	 * CPU owns this slot until its read request is submitted.
	 */
	if (period >= MIC_PERIODS || !mic_memory) return false;
	read_poison[period] = (uint16_t)(0x5a00 | (++read_serial & 255));
	int16_t *samples = (int16_t *)(mic_memory + 4096 * period);
	for (unsigned i = 0; i < MIC_PERIOD_FRAMES; i++) samples[i] = (int16_t)read_poison[period];
	for (uintptr_t line = (uintptr_t)samples; line < (uintptr_t)samples + MIC_PERIOD_BYTES; line += 64)
		__asm__ volatile ("dc cvac, %0" : : "r" (line) : "memory");
	__asm__ volatile ("dsb sy" ::: "memory");
	uint32_t read[5] = { (uint32_t)(mic_address + 4096 * period), (uint32_t)((mic_address + 4096 * period) >> 32),
		             mic_map_handle, MIC_PERIOD_BYTES, period };

	return hakios_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, READ_TOKEN | period, ASM_DATA_CMD_READ_V2, read, sizeof(read), 0, NULL);
}

static bool
afe_port_param(uint32_t module, uint32_t param, const void *data, uint16_t size, const char *what)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	*(uint16_t *)packet = MIC_PORT;
	*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, AFE_PORT_CMD_SET_PARAM_V2, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

/* the stream and the path, opened, mapped and routed (msm-pcm-q6 and q6adm/q6asm at a recording's open and prepare) */
static bool
dsp_path_on(void)
{
	struct {
		uint16_t pool, regions;
		uint32_t property, low, high, size;
	} __attribute__((packed)) map = { ASM_SHMEM_4K_POOL, 1, 0, 0, 0, 0 };
	struct {
		uint16_t flags, mode, endpoint1, endpoint2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[8];
	} __attribute__((packed)) open = { 0, ADM_PATH_LIVE_RECORDING, MIC_PORT, 0xffff, ADM_TOPOLOGY_NONE, 1, 16, MIC_RATE,
		                           { PCM_CHANNEL_FC } };
	struct {
		uint32_t mode, endpoint, topology, format;
		uint16_t bits, reserved;
	} __attribute__((packed)) reading = { 0, 0, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2, 16, 0 };
	struct {
		uint32_t param, size, frames_per_buffer, block_size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t map[8];
	} __attribute__((packed)) encoder = { ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2, 28, 0, 20, 1, 16, MIC_RATE, 1, 0, { PCM_CHANNEL_FC } };
	struct {
		uint32_t matrix, sessions;
		uint16_t session, copps;
		uint16_t copp[2];
	} __attribute__((packed)) route = { ADM_MATRIX_AUDIO_TX, 1, MIC_SESSION, 1, { 0, 0 } };
	struct hakios_apr_answer answer = { 0 };
	uint32_t bytes = 0;

	if (!hakios_audio_dsp_memory(&mic_memory, &mic_address, &bytes)) {
		say("umi-mic: no memory the DSP can see\n");
		return false;
	}
	memory_taken = true;
	if (bytes < MIC_PERIODS * 4096) {
		say("umi-mic: DSP memory too small: %u bytes\n", bytes);
		return false;
	}
	map.low = (uint32_t)mic_address;
	map.high = (uint32_t)(mic_address >> 32);
	map.size = MIC_PERIODS * 4096;
	hakios_adsp_apr_events_second(mic_events);
	if (!command("our periods for the microphone, mapped for the DSP", APR_SVC_ASM, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map,
	    sizeof(map), ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, &answer)) {
		dsp_unknown = cleanup_failed = true;
		return false;
	}
	mic_mapped = true;
	mic_map_handle = answer.words[0];
	if (!mic_map_handle) {
		say("umi-mic: invalid zero DSP map handle; retaining memory\n");
		dsp_unknown = cleanup_failed = true;
		return false;
	}
	say("umi-mic: our %u KB at %#llx are the DSP's handle %#x\n", MIC_PERIODS * 4, mic_address, mic_map_handle);
	/* Even after a lost OPEN reply, CLOSE this reserved session before unmap. */
	mic_stream_open = true;
	if (!hakios_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, 0, ASM_STREAM_CMD_OPEN_READ_V3, &reading, sizeof(reading), APR_WAIT_MS,
	    &answer) || answer.opcode != APR_BASIC_RSP_RESULT || answer.length < 8 ||
	    answer.words[0] != ASM_STREAM_CMD_OPEN_READ_V3 || answer.words[1] != 0) {
		say("umi-mic: the stream, opened for reading: %s (%#x %#x)\n", "REFUSED OR NO ANSWER", answer.opcode, answer.words[1]);
		return false;
	}
	say("umi-mic: the stream (session 2), opened for reading 16-bit linear PCM, no processing: done\n");
	if (!hakios_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, 0, ASM_STREAM_CMD_SET_ENCDEC_PARAM, &encoder, sizeof(encoder), APR_WAIT_MS,
	    &answer) || answer.opcode != APR_BASIC_RSP_RESULT || answer.length < 8 ||
	    answer.words[0] != ASM_STREAM_CMD_SET_ENCDEC_PARAM || answer.words[1] != 0) {
		say("umi-mic: the stream's format: REFUSED OR NO ANSWER (%#x %#x)\n", answer.opcode, answer.words[1]);
		return false;
	}
	say("umi-mic: the stream's format: 1 channel of 16 bits at 48 kHz: done\n");
	if (!hakios_adsp_apr(APR_SVC_ADM, MIC_PORT, 0, ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open), APR_WAIT_MS, &answer) ||
	    answer.opcode != ADM_CMDRSP_DEVICE_OPEN_V5 || answer.length < 8 || answer.words[0] != 0) {
		say("umi-mic: a device path from the port: REFUSED OR NO ANSWER (%#x %#x)\n", answer.opcode, answer.words[0]);
		dsp_unknown = cleanup_failed = true;
		return false;
	}
	mic_copp_open = true;
	mic_copp = answer.words[1] & 0xffff;
	route.copp[0] = (uint16_t)mic_copp;
	say("umi-mic: the device path from port %#x is open: COPP %u (live recording, no processing, 1 channel, 16 bits, 48 kHz)\n",
	    MIC_PORT, mic_copp);
	return command("the device path routed to session 2", APR_SVC_ADM, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &route, sizeof(route), 0,
	           NULL);
}

/* the codec DMA port: TX_CDC_DMA_TX_3, one channel of 16 bits at 48 kHz from decimator 1 */
static bool
dsp_port_on(void)
{
	struct {
		uint32_t version, rate;
		uint16_t bits, format, channels, mask;
	} __attribute__((packed)) config = { 1, MIC_RATE, 16, 0, 1, 1u << 1 };
	uint16_t start[2] = { MIC_PORT, 0 };

	if (!afe_port_param(AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_CODEC_DMA_CONFIG, &config, sizeof(config),
	    "the port TX_CDC_DMA_TX_3 (0xb037): 1 channel of 16 bits at 48 kHz, decimator 1")) {
		return false;
	}
	/* STOP is required even if START's reply is lost. */
	mic_port_on = true;
	return command("the port 0xb037, started", APR_SVC_AFE, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0, NULL);
}

static void
dsp_off(void)
{
	uint16_t stop[2] = { MIC_PORT, 0 };
	struct hakios_apr_answer answer = { 0 };

	if (mic_stream_open) {
		if (hakios_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, 0, ASM_STREAM_CMD_CLOSE, NULL, 0, APR_WAIT_MS, &answer) &&
		    answer.opcode == APR_BASIC_RSP_RESULT && answer.length >= 8 &&
		    answer.words[0] == ASM_STREAM_CMD_CLOSE && !answer.words[1]) {
			say("umi-mic: the stream, closed\n");
			mic_stream_open = false;
		} else {
			say("umi-mic: stream CLOSE FAILED; retaining DSP memory until shutdown\n");
			cleanup_failed = true;
		}
	}
	if (mic_copp_open) {
		if (hakios_adsp_apr(APR_SVC_ADM, (uint16_t)mic_copp, 0, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, APR_WAIT_MS, &answer) &&
		    answer.opcode == APR_BASIC_RSP_RESULT && answer.length >= 8 &&
		    answer.words[0] == ADM_CMD_DEVICE_CLOSE_V5 && !answer.words[1]) {
			say("umi-mic: the device path, closed\n");
			mic_copp_open = false;
		} else {
			say("umi-mic: device CLOSE FAILED\n");
			cleanup_failed = true;
		}
	}
	if (mic_port_on) {
		if (command("the port 0xb037, stopped", APR_SVC_AFE, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0, NULL))
			mic_port_on = false;
		else cleanup_failed = true;
	}
	if (dsp_unknown || mic_stream_open || mic_copp_open || mic_port_on) {
		say("umi-mic: DSP state unresolved; retaining DSP memory until shutdown\n");
		return;
	}
	if (mic_mapped) {
		if (!command("our periods, unmapped", APR_SVC_ASM, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &mic_map_handle,
		    sizeof(mic_map_handle), 0, NULL)) {
			cleanup_failed = true;
			return;
		}
		mic_mapped = false;
	}
	hakios_adsp_apr_events_second(NULL);
	if (memory_taken) {
		hakios_audio_dsp_memory_put();
		memory_taken = false;
	}
}

/* ---- umi_mic=4: the microphone for iOS ------------------------------------------------------------------- */

/*
 * What the microphone hears goes into a ring, and /dev/hakios-mic gives it to iOS's audio server
 * (HakiosAudio.driver's device "Codec", userland/src/audio_plugin.c): 16-bit samples, one channel,
 * 48 kHz. The path is up only while the device is open (the audio server opens it when its input
 * starts and closes it when it stops), and a read never waits: it gives what has come, keeping the
 * reader about 40 ms behind the DSP (it comes in 20 ms periods), and nothing until that much is
 * there; the audio server makes up the rest with silence.
 */
#define RING_FRAMES             16384           /* 341 ms; a power of two */
#define RING_KEEP               1920            /* 40 ms: how far behind the DSP a reader is kept */
#define RING_MOST               4800            /* 100 ms: more than this waiting, and the oldest is skipped */

static int16_t *ring;
static volatile uint64_t ring_written;          /* frames put into the ring since the device was made */
static uint64_t ring_read;                      /* frames taken from it (one reader: the audio server) */
static bool ring_primed;                        /* the reader is RING_KEEP behind; false after it ran dry */
static uint64_t ring_skipped, ring_dry;         /* frames skipped, reads that found too little: said when iOS stops */
static volatile uint32_t ios_wants;             /* the device is open */
static volatile bool stopping;                  /* the run ends (hakios_mic_dsp_down()) */

static void
ring_put(const int16_t *samples, uint32_t count)
{
	uint64_t at = ring_written;

	for (uint32_t i = 0; i < count; i++) {
		ring[(at + i) % RING_FRAMES] = samples[i];
	}
	__atomic_store_n(&ring_written, at + count, __ATOMIC_RELEASE);
}

static int
mic_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint64_t written = __atomic_load_n(&ring_written, __ATOMIC_ACQUIRE), avail, want;
	int error = 0;

	if (ring == NULL || uio_resid(uio) < 2) {
		return 0;
	}
	want = (uint64_t)uio_resid(uio) / 2;
	avail = written - ring_read;
	if (avail > RING_MOST) {
		ring_skipped += avail - RING_KEEP;
		ring_read = written - RING_KEEP;
		avail = RING_KEEP;
	}
	if (!ring_primed) {
		if (avail < RING_KEEP) {
			return 0;
		}
		ring_primed = true;
	}
	if (avail < want) {
		ring_dry++;
		ring_primed = false;
	}
	while (want > 0 && avail > 0 && error == 0) {
		uint64_t at = ring_read % RING_FRAMES, n = want < avail ? want : avail;

		n = n < RING_FRAMES - at ? n : RING_FRAMES - at;
		error = uiomove((caddr_t)&ring[at], (int)(n * 2), uio);
		ring_read += n;
		want -= n;
		avail -= n;
	}
	return error;
}

static int
mic_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_wants = 1;
	return 0;
}

/* (on the last close) */
static int
mic_dev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_wants = 0;
	return 0;
}

static const struct cdevsw mic_cdevsw = {
	.d_open = mic_dev_open, .d_close = mic_dev_close, .d_read = mic_dev_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ---- listening ---------------------------------------------------------------------------------------------- */

static uint32_t mic_secs = 10;

/*
 * The stream run, read for mic_secs (for_ios: while iOS has the device open): how loud it is, every
 * second (for iOS: the first three, then every 30th); into the recording with rec_sound_mic=1; for
 * iOS into the ring. False if it could not get going (the run refused, a read not sent, no read ever came
 * back): not a failure that sets `broken', so for_ios() counts it from this.
 */
#include "umi_mic_stats.h"

static void
mic_stats_report(const char *what, unsigned index, const struct mic_stats *s)
{
	uint32_t rms = s->frames ? mic_sqrt(s->squares / s->frames) : 0, ac = mic_ac_milli(s);
	int db = mic_db10(ac);
	say("umi-mic: %s %u: %llu frames rms=%u ac_rms=%u.%03u peak=%u mean=%lld AC=%s%d.%d dBFS\n",
	    what, index, s->frames, rms, ac / 1000, ac % 1000, s->peak, s->frames ? s->sum / (int64_t)s->frames : 0,
	    db < 0 ? "-" : "", db < 0 ? -db / 10 : db / 10, db < 0 ? -db % 10 : db % 10);
	say("umi-mic: PCM %s: min=%d max=%d changes=%llu nonzero=%llu/%llu repeats adjacent=%llu/%llu slot=%llu/%llu\n",
	    mic_signal_class(s), s->minimum, s->maximum, s->changes, s->nonzero, s->frames,
	    s->adjacent, s->periods, s->slot_repeats, s->periods);
}

static bool
listen(bool for_ios)
{
	uint32_t run[3] = { 0, 0, 0 }, seconds = 0, empty = 0;
	struct mic_stats window = { 0 }, signal = { 0 };
	uint64_t slot_hash[MIC_PERIODS] = { 0 }, previous_hash = 0;
	uint32_t slot_size[MIC_PERIODS] = { 0 }, previous_size = 0;
	unsigned next = 0;
	bool unmuted = false, settled = false;
	uint64_t began, now, elapsed_ms = 0, captured = 0, last_period_ms = 0, settling_frames = 0;
	struct hakios_apr_answer answer = { 0 };

	read_ready = 0;
	reads_done = read_errors = 0;
	if (!hakios_adsp_apr(APR_SVC_ASM, MIC_STREAM_PORT, 0, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), APR_WAIT_MS, &answer) ||
	    answer.opcode != APR_BASIC_RSP_RESULT || answer.length < 8 ||
	    answer.words[0] != ASM_SESSION_CMD_RUN_V2 || answer.words[1] != 0) {
		say("umi-mic: the session, run: REFUSED OR NO ANSWER (%#x %#x)\n", answer.opcode, answer.words[1]);
		return false;
	}
	began = mach_absolute_time();
	say("umi-mic: the session 2, run: done\n");
	for (unsigned p = 0; p < MIC_PERIODS; p++) {
		if (!mic_read(p)) {
			say("umi-mic: a read could not be sent\n");
			return false;
		}
	}
	say("umi-mic: LISTENING%s: %u reads of 20 ms; %s; no speech required for noise-floor diagnostics\n",
	    for_ios ? " for iOS" : "", MIC_PERIODS, for_ios ? "while iOS records" : "10 s ambient test");
	for (;;) {
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &now);
		elapsed_ms = now / NSEC_PER_MSEC;
		if (!unmuted && elapsed_ms >= 200) unmuted = dec1_unmute();
		if (!settled && elapsed_ms >= 300) settled = dec1_settled();
		if ((for_ios ? !ios_wants : elapsed_ms >= mic_secs * 1000ull) || stopping || broken || __atomic_load_n(&voice_claimed, __ATOMIC_ACQUIRE)) break;
		if (read_errors) {
			say("umi-mic: invalid/failed DSP read completion: %u errors\n", read_errors);
			return false;
		}
		if (!(__atomic_load_n(&read_ready, __ATOMIC_ACQUIRE) & (1u << next))) {
			if (elapsed_ms - last_period_ms > 1500) {
				say("umi-mic: NO READ HAS COME BACK in 1.5 s (completed %u)\n", reads_done);
				return false;
			}
			sleep_ms(2);
			continue;
		}
		OSBitAndAtomic(~(1u << next), &read_ready);
		uintptr_t buffer_start = mic_memory + 4096 * next;
		int16_t *samples = (int16_t *)(buffer_start + read_offsets[next]);
		uint32_t got = read_sizes[next] / 2;
		int16_t stereo[2 * 240];
		for (vm_offset_t line = buffer_start; line < buffer_start + MIC_PERIOD_BYTES; line += 64)
			__asm__ volatile ("dc ivac, %0" : : "r" (line) : "memory");
		__asm__ volatile ("dsb sy" ::: "memory");
		got = got > MIC_PERIOD_FRAMES ? MIC_PERIOD_FRAMES : got;
		empty += got == 0;
		bool poison;
		uint64_t hash = mic_period_hash(samples, got, read_poison[next], &poison);
		if (poison) {
			say("umi-mic: CAPTURE FAIL: UNCHANGED DMA poison slot=%u size=%u; no data delivered to iOS\n", next, got * 2);
			return false;
		}
		bool adjacent = got && previous_size == got && previous_hash == hash;
		bool repeated = got && slot_size[next] == got && slot_hash[next] == hash;
		mic_stats_add(&window, samples, got, adjacent, repeated);
		/* Exclude first 500 ms of PCM plus hardware settling. Bound energy
		 * accumulation for long stage4 recordings to their first 60 s.
		 */
		if (captured >= MIC_RATE / 2 && unmuted && settled) {
			if (signal.frames < MIC_RATE * 60) mic_stats_add(&signal, samples, got, adjacent, repeated);
		} else settling_frames += got;
		captured += got;
		if (got) {
			last_period_ms = elapsed_ms;
			previous_hash = slot_hash[next] = hash;
			previous_size = slot_size[next] = got;
		}
		if (for_ios) ring_put(samples, got);
		if (hakios_audio_recording_mic()) {
			for (uint32_t i = 0; i < got; i += 240) {
				uint32_t n = got - i < 240 ? got - i : 240;
				for (uint32_t j = 0; j < n; j++) stereo[2 * j] = stereo[2 * j + 1] = samples[i + j];
				hakios_audio_record_fed(stereo, n);
			}
		}
		if (!mic_read(next)) {
			say("umi-mic: DSP read requeue failed\n");
			return false;
		}
		next = (next + 1) % MIC_PERIODS;
		if (window.frames >= MIC_RATE) {
			seconds++;
			if (!for_ios || seconds <= 3 || seconds % 30 == 0) mic_stats_report("second", seconds, &window);
			bzero(&window, sizeof(window));
		}
	}
	mic_stats_report("POST-SETTLE", 0, &signal);
	bool dma_ok = !read_errors && !broken && (for_ios ||
	    (captured >= (uint64_t)MIC_RATE * 8 && captured <= (uint64_t)MIC_RATE * 12));
	bool signal_ok = signal.frames >= MIC_RATE && strcmp(mic_signal_class(&signal), "VARYING") == 0 && mic_ac_milli(&signal) > 0;
	say("umi-mic: CAPTURE %s: %llu frames in %llu ms, %u empty, %u DSP errors; settling excluded=%llu\n",
	    dma_ok ? "DMA PASS" : "FAIL", captured, elapsed_ms, empty, read_errors, settling_frames);
	say("umi-mic: SIGNAL %s: %s; first <=60 s after settling; variation alone is not acoustic proof\n",
	    signal_ok ? "VARYING" : "INCONCLUSIVE", mic_signal_class(&signal));
	return dma_ok && (for_ios || signal_ok);
}

static void
undo(void)
{
	dsp_off();
	dec1_off();
	codec_off();
	master_off();
	tx_clocks_off();
	pins_restore();
	reset_restore();
	bob_off();
	clocks_off();
	say("umi-mic: CLEANUP %s\n", cleanup_failed ? "FAILED; no further recordings this boot" : "PASS");
}

/* Everything up to the stream's run, in a recording's order; false (and `broken') if any of it fails. */
static bool
path_up(void)
{
	if (!clocks_on()) {
		say("umi-mic: the clocks were not all given: the blocks are not touched\n");
		return false;
	}
	if (!mapped(&tlmm, TLMM_RESET_IO) || !mapped(&lpi, 0x2004) || !mapped(&lpi_slew, 0) ||
	    !mapped(&tx_macro, 0) || !mapped(&va_macro, 0) || !mapped(&tx_swr, 0x1ffc) ||
	    !reset_release() || !pins_on() || !tx_clocks_on() || !master_on()) {
		return false;
	}
	sleep_ms(5);    /* ("5 ms for SoundWire's auto-enumeration", wcd938x_bind()) */
	if ((codec_device = codec_wait()) == 0) {
		return false;
	}
	swr_write(0xf, SWRS_SCP_INT_STATUS_MASK_1, 0x04, 0xf);  /* (swrm_runtime_resume(): broadcast) */
	sleep_ms(10);
	if (!codec_look()) return false;
	if (level < 3) {
		return true;
	}
	/* the stream and path, the port's format and start, the codec's widgets */
	return bob_on() && codec_init() && (voice_claimed || dsp_path_on()) && dsp_port_on() && dec1_on() && adc1_on() && datapath_on();
}

/*
 * umi_mic=4: the path up whenever iOS opens /dev/hakios-mic, down when it closes it; after three
 * recordings that broke off, no more until the run ends.
 */
static void
for_ios(void)
{
	int major = cdevsw_add(-1, &mic_cdevsw);
	unsigned recordings = 0, failures = 0;

	ring = kalloc_data(RING_FRAMES * sizeof(int16_t), Z_WAITOK | Z_ZERO);
	if (ring == NULL || major < 0 ||
	    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-mic") == NULL) {
		say("umi-mic: /dev/hakios-mic cannot be made\n");
		return;
	}
	say("umi-mic: THE MICROPHONE IS IOS'S (/dev/hakios-mic) from here on, until the run ends: it listens only while iOS records\n");
	while (!stopping) {
		uint64_t began, ns;
		bool failed, up;

		if (!ios_wants || __atomic_load_n(&voice_claimed, __ATOMIC_ACQUIRE)) {
			sleep_ms(20);
			continue;
		}
		lck_mtx_lock(&mic_lock);
		if (stopping || __atomic_load_n(&voice_claimed, __ATOMIC_ACQUIRE)) {
			lck_mtx_unlock(&mic_lock);
			if (!stopping) continue;
			break;
		}
		recordings++;
		quiet = recordings > 1;
		ring_primed = false;
		ring_skipped = ring_dry = 0;
		ring_read = __atomic_load_n(&ring_written, __ATOMIC_ACQUIRE);
		began = mach_absolute_time();
		say("umi-mic: iOS records (%u): the path up%s\n", recordings, quiet ? " (the lines that say all went well are not said again)" :
		    "");
		/*
		 * (a bring-up or a start that fails without setting `broken' counts as a failure too: otherwise the three-recordings
		 * limit below never applies and, while iOS keeps the device open, the path is brought up and undone for ever)
		 */
		up = path_up() && listen(true);
		failed = broken || !up;
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		say("umi-mic: iOS's recording %u %s after %llu.%llu s; last: %s; %llu frames skipped as too late, %llu reads found too little\n",
		    recordings, failed ? "BROKE OFF" : "ended", ns / NSEC_PER_SEC, ns % NSEC_PER_SEC / (NSEC_PER_SEC / 10), last_write,
		    ring_skipped, ring_dry);
		undo();
		lck_mtx_unlock(&mic_lock);
		if (cleanup_failed) break;
		if (failed) {
			broken = false;
			if (++failures >= 3) {
				say("umi-mic: three recordings broke off: the microphone stays off until the run ends\n");
				break;
			}
			/* (not again until iOS has closed the device and opened it again) */
			while (ios_wants && !stopping) {
				sleep_ms(20);
			}
		}
	}
}

static void
mic_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("umi-mic: level %u: %s\n", level, level == 1 ? "APR votes and clocks only; NO mic MMIO" :
	    level == 2 ? "the codec brought up to answer on SoundWire, nothing of it set; then all undone" :
	    level == 3 ? "the main microphone, as Android records from it, listened to; then all undone" :
	    "the main microphone for iOS (/dev/hakios-mic), up while iOS records");
	if (level == 4) {
		for_ios();
	} else {
		lck_mtx_lock(&mic_lock);
		if (stopping || __atomic_load_n(&voice_claimed, __ATOMIC_ACQUIRE)) {
			lck_mtx_unlock(&mic_lock);
			running = false;
			thread_terminate(current_thread());
			return;
		}
		if (level == 1) {
			if (clocks_on()) {
				say("umi-mic: CLOCKS PASS: both votes, TX core/NPL at 19.2MHz; NO mic MMIO\n");
			} else {
				say("umi-mic: the clocks were not all given: the blocks are not touched\n");
			}
		} else {
			bool ok = path_up();
			if (ok && level >= 3) ok = listen(false);
			say("umi-mic: stage %u %s; last: %s\n", level, ok && !broken ? "PASS" : "FAIL", last_write);
		}
		undo();
		lck_mtx_unlock(&mic_lock);
	}
	running = false;
	say("umi-mic: done\n");
	thread_terminate(current_thread());
}

void
hakios_mic_dsp_up(void)
{
	thread_t thread;

	uint32_t legacy = 0;
	if (PE_parse_boot_argn("mic", &legacy, sizeof(legacy)) && legacy)
		say("umi-mic: legacy mic= ignored; use umi_mic=1..4 (Redwood MMIO disabled)\n");
	if (!PE_parse_boot_argn("umi_mic", &level, sizeof(level)) || level == 0) return;
	if (level > 4) {
		say("umi-mic: invalid stage %u; disabled\n", level);
		return;
	}
	stopping = false;
	running = true;
	if (kernel_thread_start(mic_thread, NULL, &thread) != KERN_SUCCESS) {
		running = false;
		return;
	}
	thread_deallocate(thread);
}

void
hakios_mic_dsp_down(void)
{
	stopping = true;
	if (running) {
		lck_mtx_lock(&mic_lock);
		undo();
		lck_mtx_unlock(&mic_lock);
	}
}

/* The voice worker claims before waiting for an in-progress media capture.
 * listen() leaves normally, closes ASM/ADM and restores its hardware first. */
int umi_voice_mic_take(void)
{
 if (!running || level != 4) return ENXIO;
 __atomic_store_n(&voice_claimed, true, __ATOMIC_RELEASE);
 lck_mtx_lock(&mic_lock);
 int error = cleanup_failed ? EIO : path_up() ? 0 : EIO;
 if (!error) {
  sleep_ms(200);
  if (!dec1_unmute() || !dec1_settled()) error = EIO;
 }
 lck_mtx_unlock(&mic_lock);
 return error;
}
int umi_voice_mic_release(void)
{
 if (!__atomic_load_n(&voice_claimed, __ATOMIC_ACQUIRE)) return 0;
 lck_mtx_lock(&mic_lock);
 undo();
 int error = cleanup_failed ? EIO : 0;
 lck_mtx_unlock(&mic_lock);
 if (!error) __atomic_store_n(&voice_claimed, false, __ATOMIC_RELEASE);
 return error;
}
#include "umi_voice_audio_receiver.inc"
