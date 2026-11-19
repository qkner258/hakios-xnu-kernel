/* Run the actual AFE owner transfer with a media-thread and firmware model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "umi_voice_audio.h"
#define APR_SVC_AFE 4
#define AFE_PORT_CMD_DEVICE_STOP 0x100e6
static bool umi_profile=true,running=true,taken_down,voice_claimed,voice_feeder_parked,voice_watcher_parked;
static bool port_on=true,clock_on=true,stream_open=true,uva_mic,receiver_on,speaker_on;
static unsigned level=4,fail,step,frames_resumed;
static uint32_t ring_tail,ring_head=100;
static uint64_t last_sound_us=100;
static uint16_t active_port=0x1004;
/* Include uses these declarations itself. */
#define uva_media_suspended route_suspended
#define uva_mic_claim route_mic_claim
#define uva_output_on route_output_on
static int sound_lock;
static bool locked;
static void lck_mtx_lock(int *p){assert(p==&sound_lock&&!locked);locked=true;}
static void lck_mtx_unlock(int *p){assert(p==&sound_lock&&locked);locked=false;}
static void say(const char *p,...){(void)p;}
static bool umi_speaker_healthy(void){return true;}
static bool result(void){return ++step!=fail;}
static void sleep_ms(unsigned ms){assert(ms==1);voice_feeder_parked=voice_watcher_parked=true;}
static bool stream_off(void){assert(locked&&voice_claimed&&voice_feeder_parked&&voice_watcher_parked);if(!result())return false;stream_open=false;return true;}
static bool stream_on(void){assert(locked&&!stream_open&&port_on&&!uva_mic&&!receiver_on&&!speaker_on);if(!result())return false;stream_open=true;frames_resumed++;return true;}
static bool command(const char *what,int service,int port,int op,const void *p,unsigned n,int wanted,void *answer){
 (void)what;(void)wanted;(void)answer;
 assert(service==4&&port==0&&op==0x100e6&&n==4&&((const uint16_t *)p)[0]==active_port);
 assert(!stream_open&&voice_claimed);return result();
}
static bool afe_clock(bool on){assert(!on);return result();}
static bool afe_on(void){assert(voice_claimed&&!stream_open);if(!result())return false;clock_on=port_on=true;return true;}
static bool speaker_start(void){assert(port_on&&!stream_open&&!receiver_on);speaker_on=true;return result();}
static void speaker_stop(void){speaker_on=false;}
static uint32_t sound_attenuation(uint32_t extra){return 96+extra;}
static bool speaker_set_volume(uint32_t n,unsigned ms){assert(n>=240&&ms==1&&!stream_open);return result();}
int umi_voice_mic_take(void){assert(!stream_open&&!port_on&&!clock_on);uva_mic=true;return result()?0:EIO;}
int umi_voice_mic_release(void){assert(!receiver_on&&!speaker_on);if(!result())return EIO;uva_mic=false;return 0;}
int umi_voice_receiver(int on,uint32_t percent){assert(!stream_open&&!speaker_on&&percent<=100);if(!result())return EIO;receiver_on=on!=0;return 0;}
#include "umi_voice_audio_route.inc"
static void reset(void){
 voice_claimed=voice_feeder_parked=voice_watcher_parked=false;
 port_on=clock_on=stream_open=true;uva_mic=receiver_on=speaker_on=false;
 route_suspended=route_mic_claim=route_output_on=false;
 fail=step=frames_resumed=0;
}
int main(void){
 reset();assert(!umi_voice_route_take());assert(!stream_open&&!port_on&&uva_mic);
 assert(!umi_voice_route_set(UMI_VOICE_RECEIVER,70)&&receiver_on);
 assert(!umi_voice_route_set(UMI_VOICE_SPEAKER,100)&&speaker_on&&!receiver_on);
 assert(!umi_voice_route_set(UMI_VOICE_SPEAKER,0));
 assert(!umi_voice_route_release()&&stream_open&&port_on&&!voice_claimed&&!uva_mic&&!speaker_on);
 assert(ring_tail==ring_head&&!last_sound_us&&frames_resumed==1);
 for(unsigned n=1;n<=4;n++){
  reset();fail=n;assert(umi_voice_route_take()==EIO);
  fail=0;assert(!umi_voice_route_release());assert(stream_open&&!voice_claimed&&!uva_mic);
 }
 // Failure after media is suspended restores both physical paths.
 reset();assert(!umi_voice_route_take());fail=step+1;assert(umi_voice_route_set(UMI_VOICE_RECEIVER,70)==EIO);
 fail=0;assert(!umi_voice_route_release()&&stream_open&&!voice_claimed);
 reset();assert(!umi_voice_route_take());assert(!umi_voice_route_set(UMI_VOICE_SPEAKER,70));
 fail=step+1;assert(umi_voice_route_release()==EIO&&voice_claimed&&!stream_open);
 fail=0;assert(!umi_voice_route_release()&&!voice_claimed&&stream_open);
 puts("PASS voice route: media threads park before ASM/ADM/AFE close, exclusive mic/receiver/speaker ownership, protected gain ceiling, switch and partial-failure restoration");
 return 0;
}
