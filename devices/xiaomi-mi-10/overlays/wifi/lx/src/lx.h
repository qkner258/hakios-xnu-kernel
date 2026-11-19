/* What the files of wifi/lx/src know of one another. */
#ifndef _LX_H_
#define _LX_H_

int	lx_base_init(void);		/* lx_base.c: the thread that counts the ticks and runs the timers */
int	lx_pool_init(void);		/* lx_base.c: the memory the radio reads and writes; 0, or an errno */
long	lx_stub(const char *name);	/* lx_base.c: what a function that is not there yet does */
void	lx_tell(const char *format, ...) __printflike(1, 2);	/* lx_base.c: a line that is always said (printf: may be kept back) */
void	lx_memory_say(void);		/* lx_base.c: what the layer holds of memory */
void	lx_pause_ms(unsigned ms);	/* lx_base.c: sleep */
void	lx_mark(const char *what);	/* lx_base.c: a point reached, said and waited at if the kernel was asked to */
int	lx_station_scan(void);		/* lx_80211.c: an interface in station mode, a scan, what was heard; 0, or an errno */
int	lx_station_join(void);		/* lx_80211.c: join the network the kernel was given; 0 when frames can pass */
void	lx_station_say(void);		/* lx_80211.c: what has passed since */
void	lx_station_leave(void);		/* lx_80211.c: leave it, and the interface down */
int	lx_station_send(const unsigned char *frame, unsigned long length);	/* lx_80211.c: an Ethernet frame for the network */
int	lx_station_offer(void);	/* lx_80211.c: the station given to the kernel to ask (sys/hakios.sta.h); 0, or an errno */
void	lx_station_withdraw(void);	/* lx_80211.c: ... and taken back */
/* lx_bus.c: the chip's registers refused from now on, reads and writes alike: no lock, no sleep
 * (the kernel's way back to the loader, once an endpoint has been reset or its clock taken away) */
void	lx_quiesce_registers(void);

/*
 * What the asynchronous path has done, for the line that says the layer is still running
 * (lx_base.c's ticker, once a second): a run that ends with no word leaves these as its account
 * of how far the driver got -- whether frames were still coming from the firmware when the log
 * stopped, and in which state the last scan was left.
 */
struct lx_counts {
	unsigned long long frames;		/* frames received from the firmware in all (not only beacons) */
	unsigned long long heard;		/* of them, beacons and answers to probes */
	unsigned long long scans;		/* scans asked of the driver */
	unsigned long long scans_ended;		/* ... and the driver said that they were over */
	unsigned long long scans_given_up;	/* ... and that it gave them up (an aborted scan) */
	unsigned long long scans_cancelled;	/* scans that were asked to stop and did not in time */
	unsigned long long polls;		/* passes of the thread that looks at the copy engines */
	unsigned long long served;		/* data path entries it took care of, added up */
	unsigned long long complaints;		/* the driver's warnings and errors (ath11k_warn/err) */
	int scan;				/* 0: none, 1: running, 2: over, 3: given up by the driver */
};
void	lx_station_counts(struct lx_counts *counts);	/* lx_80211.c: the station's part of the above */
void	lx_counts_get(struct lx_counts *counts);	/* lx_bus.c: all of it, as it is now; no lock, no sleep */

#endif
