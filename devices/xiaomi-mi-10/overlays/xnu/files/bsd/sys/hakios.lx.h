/*
 * The whole of what the Wi-Fi driver's object asks of the kernel it is linked into. That object
 * (tools/mk-ath11k.sh in the repository: Linux's ath11k on FreeBSD's LinuxKPI headers and our
 * wifi/lx) is compiled as FreeBSD kernel code, drivers/wifi/hakios.lx.c on this side as XNU's;
 * neither sees the other's headers, so nothing but C's own types crosses (and the station
 * boundary's, hakios.sta.h, which is written for both). Everything the
 * driver's side knows of locks, timers, work queues and buffers is built in wifi/lx/src on the
 * few calls here.
 *
 * Threads: the driver's code runs on threads made by lx_os_thread() and on the kernel's own
 * threads that call into it; never in an interrupt.
 */
#ifndef _SYS_HAKIOS_LX_H_
#define _SYS_HAKIOS_LX_H_

#include "hakios.sta.h"

/* ---- memory ---- */
void	*lx_os_alloc(unsigned long size);			/* zeroed; NULL when there is none */
void	 lx_os_free(void *memory, unsigned long size);
/*
 * Memory the radio itself reads and writes: whole pages, zeroed, which the radio knows from
 * *address on (the machine says where: the radio sees memory through an SMMU). What either
 * side writes the other sees at once: the processor's cache is not in between, which makes
 * reading it slow. NULL when there is none; never given back.
 */
void	*lx_os_radio_memory(unsigned long size, unsigned long long *address);
void	*lx_os_radio_memory_at(unsigned long size, unsigned long long address);
/* UMI's preallocated stock WLAN regions: DDR=1, M3=3, CAL=4. */
int	 lx_os_radio_firmware_memory(unsigned type, unsigned long size, void **memory, unsigned long long *address);
void	*lx_os_map_registers(unsigned long long physical, unsigned long size);	/* never unmapped */

/* ---- saying things ---- */
void	 lx_os_print(const char *text);				/* for whoever looks for a fault: may be kept back */
void	 lx_os_tell(const char *text);				/* for whoever runs the machine: always said */
void	 lx_os_panic(const char *text) __attribute__((noreturn));
/* USB RAM progress probes; identical stage/subject/value is deduplicated for 1 s.
 * No payloads, addresses, keys or driver locks enter this path. */
void lx_os_ap_trace(const char *stage, unsigned subject, int value);

/* ---- time, threads, sleeping ---- */
unsigned long long lx_os_nanoseconds(void);			/* since some moment; never goes back */
void	*lx_os_thread_self(void);				/* a value that tells the kernel's threads apart */
/* a thread that runs function(argument) and ends when that returns: 0, or an errno */
int	 lx_os_thread(const char *name, void (*function)(void *), void *argument);
/*
 * One lock, and sleeping under it as with a condition variable: lx_os_sleep() is called with the
 * lock held, gives it up while asleep and has it again when it returns -- 0 when woken by
 * lx_os_wakeup() on the same address, 1 when the time (nanoseconds; 0: no limit) ran out.
 * The lock is held for a few instructions at a time and never across a call out of the layer.
 */
void	 lx_os_lock(void);
void	 lx_os_unlock(void);
int	 lx_os_sleep(const void *address, unsigned long long nanoseconds);
void	 lx_os_wakeup(const void *address);			/* everyone asleep on it */

/* ---- the rest of the machine ---- */
void	 lx_os_random(void *buffer, unsigned long size);	/* bytes nobody can guess */
/* a number given to the kernel when it was started ("wifi_lx=2"); 0 if it was not */
unsigned lx_os_option(const char *name);
/* a file the driver asks for by the name Linux has for it, in place: 0, or an errno (ENOENT) */
int	 lx_os_firmware(const char *name, const void **data, unsigned long *size);
/* the processor that runs the radio's firmware: started, and up when this returns; 0 or an errno */
int	 lx_os_rproc_boot(void);
void	 lx_os_rproc_shutdown(void);
/* the radio's own address (six bytes), if the machine has one for it: 1, or 0 and nothing written */
int	 lx_os_radio_address(unsigned char *address);
/*
 * The network the radio is to join, if the machine was given one: its name and its passphrase,
 * into buffers whose sizes come in and whose contents' lengths go out. 1, or 0.
 */
int	 lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length);

/* ---- frames: the radio as what carries the machine's network interface ---- */
void	 lx_os_net_attach(const unsigned char *address);	/* there is a radio, and this is its address */
void	 lx_os_net_link(int up);				/* frames can pass now (a network is joined) / no longer */
void	 lx_os_net_input(const unsigned char *frame, unsigned long length);	/* an Ethernet frame has arrived */

/* AP-only radio; credentials are transient, never exposed by status/sysctl. */
struct lx_ap_config {
	unsigned char ssid[32];
	unsigned ssid_length, channel;
	unsigned security, passphrase_length; /* 0 open (explicit legacy helper), 1 WPA2/CCMP */
	unsigned char passphrase[64], pmk[32];
};
struct lx_ap_status {
	unsigned version, active, security, channel, ssid_length, associated, authorized;
	unsigned char ssid[32], address[6], peer[6];
	unsigned hidden;
};
#define LX_AP_STATIONS 128 /* Native Get 42 ABI capacity; live limit comes from firmware. */
struct lx_ap_station { unsigned char address[6]; int rssi; };
struct lx_ap_ops {
	int (*configure)(const struct lx_ap_config *); /* NULL: stop and restore STA */
	int (*send)(const unsigned char *, unsigned long);
	int (*set_hidden)(unsigned);
 unsigned (*stations)(struct lx_ap_station *, unsigned);
	void (*shutdown)(void);                     /* stop without restoring STA */
};
void lx_os_ap_register(const struct lx_ap_ops *ops); /* NULL: quiesce before driver removal */
int lx_os_ap_attach(const unsigned char *address);  /* normal BSD ap1, initially down */
void lx_os_ap_link(int up);
void lx_os_ap_input(const unsigned char *frame, unsigned long length);
void lx_os_ap_peer(unsigned associated, unsigned authorized, const unsigned char *address);
/* XNU control entry points; CREATE does not start the radio. */
int hakios_wifi_ap_create(void);
int hakios_wifi_ap_delete(void);
int hakios_wifi_ap_hidden(unsigned hidden);
int hakios_wifi_ap_configure(const struct lx_ap_config *config);
void hakios_wifi_ap_status(struct lx_ap_status *status);
unsigned hakios_wifi_ap_stations(struct lx_ap_station *, unsigned);
void hakios_apple80211_ap_event(unsigned event, const unsigned char *address);

/*
 * ---- the radio's interrupts (wifi_irq=1). The radio raises one by writing a number to an address
 * (an "MSI": the GIC's register that raises the SPI of that number); vector v of its 32 is data + v.
 * The driver's work for one is done in thread context, on the kernel's thread for them: the
 * function given for a vector is called there after it has fired, with the vector masked until
 * lx_os_irq_enable(). Nothing of the driver runs in an interrupt.
 */
int	 lx_os_irq_msi(unsigned long long *address, unsigned *data);	/* where and what: 1, or 0 (no interrupts) */
int	 lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument);	/* masked to begin with; 0 or an errno */
void	 lx_os_irq_enable(unsigned vector);
void	 lx_os_irq_disable(unsigned vector);			/* returns when its function is not running */
void	 lx_os_irq_detach_all(void);				/* returns when no function is running */
unsigned long long lx_os_irq_count(unsigned vector);		/* how many have arrived */

/* ---- the station: scanning and joining, for whoever asks the kernel (hakios.sta.h) ---- */
void	 lx_os_station(const struct hakios_sta_ops *ops);	/* here it is, to be asked; NULL: no longer (returns when no call is under way) */
void	 lx_os_station_lost(int error);				/* the network joined is gone by itself */

/*
 * ---- the radio's last accesses of the chip's registers, for whoever looks after a hang.
 *
 * A run of this port has ended with the whole kernel stopping at once, with no panic and no line
 * in the log (work/v29f, work/v29g), and the one thing that would tell those deaths apart is the
 * access that never came back. The bus (wifi/lx/src/lx_bus.c) writes every access it makes into
 * a small ring of this shape before making it, and marks it done after; the kernel passes the
 * ring to the loader once the driver is started (lx_os_mmio_ring()), and the loader -- which is
 * still alive on another processor, see osfmk/arm64/loader_return.c -- prints it when the kernel
 * has been silent for SILENCE_SECONDS: on the phone's screen, and in its log.
 *
 * The entry still marked pending is the access that never returned. The entry's "cpu" is the low
 * three bytes of MPIDR_EL1 (affinities 0-2), and "caller" is the address the access was made
 * from, in the kernel's address space (subtract the loader's slide and symbolicate: keepsyms).
 *
 * The loader has a copy of this structure (devices/xiaomi-mi-10/loader/loader.c, struct
 * lx_mmio_ring); the two must stay identical, and the ring says its size so that a loader of
 * another shape refuses it rather than reading it wrong.
 */
#define LX_MMIO_ACCESSES	16			/* the entries in the ring */
#define LX_MMIO_VERSION		1
#define LX_MMIO_MAGIC		0x47524f494d4d584cull	/* "LXMMIORG" */
#define LX_MMIO_READ		1			/* the driver read a register */
#define LX_MMIO_WRITE		2			/* ... wrote one */
#define LX_MMIO_WINSEL		3			/* ... chose one of the chip's windows (a write and a read-back) */

struct lx_mmio_access {
	unsigned long long seq;		/* 1, 2, 3, ... in the order the accesses began (0: this entry is unused) */
	unsigned long long when_ns;	/* lx_os_nanoseconds() when it began */
	unsigned long long caller;	/* where the access was made from, in the kernel's address space */
	unsigned long long value;	/* a write's value; a read's answer; a window select's new window */
	unsigned long long answer;	/* a window select's read-back */
	unsigned cpu;			/* the low 24 bits of MPIDR_EL1 (affinities 0-2) */
	unsigned what;			/* LX_MMIO_READ / LX_MMIO_WRITE / LX_MMIO_WINSEL */
	unsigned offset;		/* the register, as the driver named it */
	unsigned at;			/* where in the mapped block it was reached */
	unsigned window;		/* which of the chip's windows that is (0: its first 512K) */
	unsigned pending;		/* 1: the access has not come back */
};

struct lx_mmio_ring {
	unsigned long long magic;	/* LX_MMIO_MAGIC */
	unsigned long long seq;		/* the sequence number handed out last */
	unsigned long long lost;	/* accesses not written down because their entry was still pending */
	unsigned entries;		/* LX_MMIO_ACCESSES */
	unsigned size;			/* sizeof(struct lx_mmio_access) */
	unsigned version;		/* LX_MMIO_VERSION */
	unsigned unused;
	struct lx_mmio_access access[LX_MMIO_ACCESSES];
};

/* The ring, offered to the kernel once, when the driver starts; the driver writes it, the kernel
 * only passes the pointer on. NULL or another shape: nothing is offered. */
void	 lx_os_mmio_ring(struct lx_mmio_ring *ring, unsigned long size);

/*
 * The bus is inside one access to the chip's registers, on this processor: said before the access
 * is made and taken back once it has come back (lx_bus.c, mmio_begin()/mmio_end()). The kernel
 * writes the processor's own breadcrumb (osfmk/arm64/umi_cpu_pc.h), so its hang report can put
 * the ring's pending entry beside the processor that was making it -- and tell one that never
 * came back from one that was finished just before the machine stopped
 * (work/v29i/INVESTIGATION-hotspot-hang-v29z.md §2.4). `seq` is the ring entry's own number
 * (beginning at 1); `when_ns` is lx_os_nanoseconds() when the access began. Two stores, no lock,
 * no clock read (the caller already has the time), safe from any context that may touch the bus.
 */
void	 lx_os_mmio_begin(unsigned long long seq, unsigned long long when_ns);
void	 lx_os_mmio_end(unsigned long long seq);

/*
 * ---- the names the object shows the kernel. lx_start(): bring the driver up on the radio and
 * do what the kernel's options ask (scan; join a network). 0, or an errno. Returns when that is
 * over or has failed; with a network joined the driver stays up, lx_transmit() sends an
 * Ethernet frame through it (0, or an errno; called by one thread, the kernel's own for that),
 * and lx_stop() leaves the network and takes the driver down in order (and does nothing if it
 * is not up).
 */
int	 lx_start(void);
void	 lx_stop(void);
/*
 * No register of the radio is touched from now on (reads and writes alike): called before the
 * endpoint is reset or its clock taken away, from a path that may not sleep or take a lock. A
 * pending ring pointer written into a dead link never comes back and stalls the whole SoC
 * (work/v29i/INVESTIGATION-wifi-srng-hang.md §6.2). Safe to call more than once.
 */
void	 lx_quiesce_registers(void);
int	 lx_transmit(const unsigned char *frame, unsigned long length);
/* before lx_start(), if the kernel wants it early: take the memory the radio will be shown. 0, or an errno. */
int	 lx_prepare(void);

#endif
