/* UMI production ABI1. Included by loader_return.c via xnu-device.patch.
 * The loader requires this capability before setting loader_stub_slot[2].
 * Debug loaders still write a checkpoint (normally zero), so all hooks are inert.
 */
#define UMI_PROD_MAGIC 0x31444f5250494d55ULL
#define UMI_PROD_SLOT  0x50524f4400000001ULL
#define UMI_SYSTEM_OFF   0x84000008u
#define UMI_SYSTEM_RESET 0x84000009u

static const volatile uint64_t umi_prod_capability[2] __attribute__((aligned(8))) = {
    UMI_PROD_MAGIC, 1
};
static uint32_t umi_prod_power = UMI_SYSTEM_RESET; /* panic/guard: reset, never return */

bool loader_is_production(void);
void loader_prod_halt(boolean_t reboot);

bool
loader_is_production(void)
{
    return loader_stub_slot[2] == UMI_PROD_SLOT && umi_prod_capability[1] == 1;
}

/* Called by reboot_kernel after its existing sync/unmount/driver-stop path,
 * or by halt_all_cpus. Publish the intent before stop_others hands off to master_cpu.
 */
void
loader_prod_halt(boolean_t reboot)
{
    if (!loader_is_production()) return;
    os_atomic_store(&umi_prod_power, reboot ? UMI_SYSTEM_RESET : UMI_SYSTEM_OFF, release);
    loader_return();
    for (;;) { __asm__ volatile ("wfe"); }
}

static void
umi_prod_finish(void)
{
    if (!loader_is_production()) return;
    /* UMI's loader platform uses SMC (not HVC). Never restore UEFI on failure.
     * Heartbeat stops here; CPU0 must stop feeding and let the watchdog bite.
     */
    register uint64_t x0 __asm__("x0") = os_atomic_load(&umi_prod_power, acquire);
    register uint64_t x1 __asm__("x1") = 0;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile ("msr daifset, #0xf\nsmc #0" :
        "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
        "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    for (;;) { __asm__ volatile ("wfe"); }
}
