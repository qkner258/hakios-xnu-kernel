#include <dlfcn.h>
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
int main(int c,char**v){void*h=dlopen("/usr/lib/libMobileGestalt.dylib",RTLD_NOW);
CFTypeRef(*a)(CFStringRef,void*)=dlsym(h,"MGCopyAnswer");
const char*k[]={"HasBaseband","telephony","cellular-data","contains-cellular-radio","sms","BasebandClass","SIMTrayStatus","DeviceSupports5G","RegionCode","gps","voice-mail","HardwareModel","BasebandChipset","DeviceSupportsDualSIM","SupportsSIMTray",0};
for(int i=0;k[i];i++){CFStringRef s=CFStringCreateWithCString(0,k[i],kCFStringEncodingUTF8);CFTypeRef r=a(s,0);
char b[200]="(null)";if(r){CFStringRef d=CFCopyDescription(r);CFStringGetCString(d,b,200,kCFStringEncodingUTF8);}
printf("%s = %s\n",k[i],b);}return 0;}
