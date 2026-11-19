/* Host only: run production fallback logic against ordinary ObjC fake timers. */
#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <sys/sysctl.h>
#include <pthread.h>
#include <stdbool.h>
#include <float.h>
#include <assert.h>
#define say(...) printf(__VA_ARGS__)
static uintptr_t slide;
static IMP replace_method(const char *c, const char *s, uintptr_t a, IMP r) { (void)c;(void)s;(void)a;(void)r;return NULL; }
#include "umi_idle_timer.h"
/* Test the production MC API call sites without accessing host preferences. */
static NSNumber *mc_user, *mc_effective;
static unsigned mc_reads;
@interface MCProfileConnection : NSObject
+ (id)sharedConnection;
- (id)userValueForSetting:(id)key;
- (id)effectiveValueForSetting:(id)key;
@end
@implementation MCProfileConnection
+ (id)sharedConnection { static id connection; if (!connection) connection = [self new]; return connection; }
- (id)userValueForSetting:(id)key { assert([key isEqual:@"maxInactivity"]); mc_reads++; return mc_user; }
- (id)effectiveValueForSetting:(id)key { assert([key isEqual:@"maxInactivity"]); mc_reads++; return mc_effective; }
@end
@interface TestTimeout : NSObject
@property double duration;
@property unsigned long long identifier;
@end
@implementation TestTimeout
@end
@interface TestDescriptor : NSObject
@property(retain) NSArray *timeouts;
@end
@implementation TestDescriptor
@end
@interface TestAttention : NSObject
@property(retain) TestDescriptor *descriptor;
@property(getter=isEnabled) BOOL enabled;
@end
@implementation TestAttention
@end
@interface TestTimer : NSObject {
@public TestAttention *_attentionAwareTimer;
}
@property(getter=isActivated) BOOL activated;
@property(getter=isDisabled) BOOL disabled;
@property BOOL requiresManualReset;
@property(copy) NSDictionary *descriptor;
- (void)reset;
@end
@implementation TestTimer
- (void)reset { umi_idle_reconfigure(self, NULL, YES); }
@end
static unsigned delivered, stages[4], native_reconfigures;
static void delivered_timeout(id self, SEL cmd, id timer, id timeout) {
 (void)self;(void)cmd;(void)timer;unsigned long long ident=[(TestTimeout *)timeout identifier];
 assert(ident>=1 && ident<=3);stages[ident]++;delivered++;
}
static void reconfigured(id self, SEL cmd, BOOL reset) { (void)self;(void)cmd;(void)reset;native_reconfigures++; }
static void attention(id self, SEL cmd, id timer, unsigned long long event, double at) { (void)self;(void)cmd;(void)timer;(void)event;(void)at; }
static void set_elapsed(struct umi_idle_slot *slot, double seconds) {
 slot->reset_at=mach_absolute_time()-(uint64_t)(seconds*1e9*umi_idle_timebase.denom/umi_idle_timebase.numer);
}
int main(void) { @autoreleasepool {
 char value[80]; umi_idle_describe(nil,value,sizeof(value)); assert(!strcmp(value,"<unset>"));
 mc_user=nil;mc_effective=@60;umi_idle_log_settings("test-default");assert(mc_reads==2);
 mc_user=@30;mc_effective=@30;umi_idle_log_settings("test-change");assert(mc_reads==4);
 mc_user=@2147483647;mc_effective=@30;umi_idle_log_settings("test-restriction");assert(mc_reads==6);
 mach_timebase_info(&umi_idle_timebase);
 umi_idle_orig_timeout=delivered_timeout;
 umi_idle_orig_reconfigure=reconfigured;umi_idle_orig_attention=attention;
 umi_idle_attention_ivar=class_getInstanceVariable([TestTimer class],"_attentionAwareTimer");
 TestTimer *timer=[TestTimer new];timer.activated=YES;
 timer->_attentionAwareTimer=[TestAttention new];timer->_attentionAwareTimer.enabled=YES;
 TestDescriptor *desc=[TestDescriptor new];timer->_attentionAwareTimer.descriptor=desc;
 TestTimeout *timeout=[TestTimeout new];timeout.identifier=1;timeout.duration=1;
 desc.timeouts=@[timeout];
 struct umi_idle_slot *slot=umi_idle_slot_for(timer,true);
 set_elapsed(slot,0.1);umi_idle_tick();assert(delivered==0);
 set_elapsed(slot,2);timer->_attentionAwareTimer.enabled=NO;umi_idle_tick();assert(delivered==0);
 timer->_attentionAwareTimer.enabled=YES;desc.timeouts=@[];umi_idle_tick();assert(delivered==0);
 desc.timeouts=@[timeout];timer.disabled=YES;umi_idle_tick();assert(delivered==0);
 timer.disabled=NO;timer.activated=NO;set_elapsed(slot,2);umi_idle_tick();assert(delivered==0);
 timer.activated=YES;timer.requiresManualReset=YES;set_elapsed(slot,2);umi_idle_tick();assert(delivered==0);
 timer.requiresManualReset=NO;timeout.duration=DBL_MAX;set_elapsed(slot,2);umi_idle_tick();assert(delivered==0);
 timeout.duration=1;umi_idle_held=true;set_elapsed(slot,2);umi_idle_tick();assert(delivered==0);
 umi_idle_held=false;umi_idle_tick();assert(delivered==0); /* hold reset the clock */
 set_elapsed(slot,2);umi_idle_tick();assert(delivered==1);
 umi_idle_tick();assert(delivered==1); /* duplicate fallback */
 umi_idle_timeout(timer,NULL,timer->_attentionAwareTimer,timeout);assert(delivered==1); /* late native duplicate */
 slot->fired=0;set_elapsed(slot,2);
 umi_idle_timeout(timer,NULL,timer->_attentionAwareTimer,timeout);assert(delivered==2);
 umi_idle_tick();assert(delivered==2); /* native wins */
 slot->fired=0;timeout.duration=60;umi_idle_tick();assert(delivered==2); /* new Settings timeout */
 puts("PASS: MC default/change/restriction reads, unset reporting, disabled/empty backend, native deadline, inactive/disabled/manual/Never/held suppression, held release, fallback delivery, late native dedup, updated Settings");
 /* v7 regression: one unchanged reset per second must not starve 20/22/30 s. */
 TestTimeout *quick=[TestTimeout new], *expire=[TestTimeout new];
 timeout.duration=20;quick.identifier=2;quick.duration=22;expire.identifier=3;expire.duration=30;
 desc.timeouts=@[timeout,quick,expire];timer.descriptor=@{@"mode":@1,@"total":@30};
 umi_idle_reconfigure(timer,NULL,NO);assert(slot->config_resets==1);
 unsigned before[4];memcpy(before,stages,sizeof(stages));
 for (unsigned second=1;second<=40;second++) {
  /* New equal objects are configuration refreshes too. */
  timer.descriptor=[NSDictionary dictionaryWithObjectsAndKeys:@1,@"mode",@30,@"total",nil];
  set_elapsed(slot,second);uint64_t epoch=slot->reset_at;unsigned fired=slot->fired;
  [timer reset];assert(slot->reset_at==epoch && slot->fired==fired && slot->config_resets==1);
  umi_idle_tick();
  assert(stages[1]-before[1]==(second>=21));
  assert(stages[2]-before[2]==(second>=23));
  assert(stages[3]-before[3]==(second>=31));
 }
 assert(native_reconfigures==41); /* native code is still called */
 umi_idle_timeout(timer,NULL,timer->_attentionAwareTimer,expire);assert(stages[3]==before[3]+1);
 umi_idle_input(0);assert(slot->fired==0 && umi_idle_input_count==1);umi_idle_tick();assert(stages[1]==before[1]+1);
 set_elapsed(slot,21);umi_idle_tick();assert(stages[1]==before[1]+2);
 /* Touch after dim starts a fresh full interval; a held finger cannot expire. */
 umi_idle_input(1ULL<<63);assert(slot->fired==0);
 set_elapsed(slot,40);umi_idle_tick();assert(stages[3]==before[3]+1);
 umi_idle_input(0);umi_idle_tick();assert(stages[1]==before[1]+2);
 set_elapsed(slot,25);slot->fired=2;umi_idle_attention(timer,NULL,nil,8,0);
 assert(slot->fired==0 && slot->attention_resets==1 && slot->last_attention==8);
 /* A real configuration change resets even when the native call says reset=NO. */
 set_elapsed(slot,25);timer.descriptor=@{@"mode":@1,@"total":@60};
 umi_idle_reconfigure(timer,NULL,NO);assert(slot->config_resets==2 && slot->fired==0);
 umi_idle_tick();assert(stages[1]==before[1]+2);
 timer.descriptor=@{@"mode":@1,@"total":@2147483647};desc.timeouts=@[];
 umi_idle_reconfigure(timer,NULL,YES);set_elapsed(slot,1000);umi_idle_tick();assert(stages[3]==before[3]+1);
 /* Assertions release with a fresh epoch, never an immediate overdue callback. */
 desc.timeouts=@[timeout,quick,expire];timer.activated=NO;set_elapsed(slot,1000);umi_idle_tick();
 timer.activated=YES;umi_idle_tick();assert(stages[3]==before[3]+1);
 puts("PASS: unchanged reset storm reaches 20/22/30 s once; equal descriptor copies preserve epoch; HID after dim, hold/release, attention, changed configuration, Never and assertion release");
 CFRelease((CFTypeRef)slot->configuration);slot->configuration=nil;[quick release];[expire release];
 objc_storeWeak(&slot->weak_timer,nil);[timer release];[desc release];[timeout release];
 }return 0;
}
