#!/usr/bin/env python3
"""Compile real NCM16 framing under ASan/UBSan, including malformed NTBs."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
h=Path(__file__).parent/'sources/umi_usb_ntb.h'
if not h.exists(): h=root/'overlays/xnu/files/bsd/dev/umi_usb_ntb.h'
s='''#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
'''+h.read_text()+r'''
int main(void) {
 uint8_t p[NCM_NTB_MAX], q[NCM_NTB_MAX], frame[NCM_FRAME_MAX]; struct ncm_datagram d[NCM_DATAGRAMS]; struct ncm_decode_info info;
 memset(frame,0xab,sizeof(frame));
 for(unsigned n=14;n<=NCM_FRAME_MAX;++n) {
  unsigned size=ncm_encode(p,frame,n,65535);
  assert(size<=2048 && size%512 && ncm_le16(p+6)==65535);
  assert(ncm_decode(p,size,d,&info)==1 && d[0].offset==28 && d[0].length==n);
  assert(!memcmp(p+d[0].offset,frame,n));
 }
 /* TX short-packet padding is outside the datagram, inside wBlockLength;
  * prefill DMA memory so a missing zero write cannot pass accidentally. */
 for(unsigned n=484;n<=1508;n+=512) {
  memset(p,0xa5,sizeof(p));unsigned bytes=ncm_encode(p,frame,n,65536);
  assert(bytes==29+n && ncm_le16(p+8)==bytes && ncm_le16(p+6)==0);
  assert(p[bytes-1]==0 && p[bytes]==0xa5 && ncm_le16(p+22)==n);
 }
 /* A real ARP reply remains exactly 42 bytes, no Ethernet FCS/padding in DPE. */
 const uint8_t arp[42]={2,0x42,0,0,0,2,2,0x42,0,0,0,1,8,6,
  0,1,8,0,6,4,0,2,2,0x42,0,0,0,1,172,16,42,1,2,0x42,0,0,0,2,172,16,42,2};
 assert(ncm_encode(p,arp,sizeof(arp),7)==70 && ncm_le16(p+22)==42);
 assert(ncm_decode(p,70,d,&info)==1 && !memcmp(p+d[0].offset,arp,42));
 memset(p,0xa5,sizeof(p));
 assert(!ncm_encode(p,frame,13,0) && !ncm_encode(p,frame,NCM_FRAME_MAX+1,0) && p[0]==0xa5);
 unsigned size=ncm_encode(p,frame,100,1); memcpy(q,p,size);
 assert(ncm_decode(p,size-1,d,&info)<0);
 ncm_put16(p+10,4);assert(ncm_decode(p,size,d,&info)<0);memcpy(p,q,size);
 ncm_put16(p+18,12);assert(ncm_decode(p,size,d,&info)<0);memcpy(p,q,size); /* cycle */
 ncm_put16(p+20,12);assert(ncm_decode(p,size,d,&info)<0);memcpy(p,q,size); /* data overlaps NDP */
 ncm_put16(p+22,1515);assert(ncm_decode(p,size,d,&info)<0);memcpy(p,q,size);
 ncm_put32(p+12,0x314d434e);assert(ncm_decode(p,size,d,&info)<0); /* CRC mode is unsupported */

 /* Host interoperability shapes, independently constructed (not captured Mac bytes). */
 memcpy(p,q,size); ncm_put16(p+8,0);
 assert(ncm_decode(p,size,d,&info)==1 && info.zero_block==1);
 memcpy(p,q,size);p[size]=0;
 assert(ncm_decode(p,size+1,d,&info)==1 && info.pad==1);
 p[size]=1;assert(ncm_decode(p,size+1,d,&info)==-NCM_SIZE);
 memcpy(p,q,size);ncm_put16(p+8,size+1);
 assert(ncm_decode(p,size,d,&info)==-NCM_BLOCK_LENGTH);
 memcpy(p,q,size);ncm_put16(p+8,size-1);
 assert(ncm_decode(p,size,d,&info)==-NCM_FRAME_LENGTH); /* Never read frame into trailing pad. */
 /* A frame at offset30 aligns its IP payload, not the Ethernet header. */
 memcpy(p,q,size);memmove(p+30,p+28,100);ncm_put16(p+20,30);ncm_put16(p+8,size+2);
 assert(ncm_decode(p,size+2,d,&info)==1 && info.unaligned==1 && d[0].offset==30);
 ncm_put16(p+8,0);assert(ncm_decode(p,size+2,d,&info)==1 && info.zero_block==1);
 /* Either zero in the terminal DPE stops parsing, as in Linux. */
 ncm_put16(p+24,30);assert(ncm_decode(p,size+2,d,&info)==1 && info.half_null==1);
 ncm_put16(p+24,0);ncm_put16(p+26,100);assert(ncm_decode(p,size+2,d,&info)==1 && info.half_null==1);
 /* A single USB transfer may contain two complete NTBs. */
 memcpy(p,q,size);memcpy(p+size,q,size);
 assert(ncm_decode(p,2*size,d,&info)==2 && info.blocks==2 && d[1].offset==size+28);
 p[2*size]=0;assert(ncm_decode(p,2*size+1,d,&info)==2 && info.pad==1);
 p[size]=0;assert(ncm_decode(p,2*size,d,&info)==-NCM_NTH_SIGNATURE);
 /* Full advertised receive size is valid, with or without explicit block length. */
 memset(p,0,sizeof(p));memcpy(p,q,size);ncm_put16(p+8,NCM_NTB_MAX);
 assert(ncm_decode(p,sizeof(p),d,&info)==1);
 ncm_put16(p+8,0);assert(ncm_decode(p,sizeof(p),d,&info)==1);
 memcpy(p,q,size);ncm_put32(p,0x686d636e);assert(ncm_decode(p,size,d,&info)==-NCM_NTH32);
 memcpy(p,q,size);ncm_put32(p+12,0x314d434e);assert(ncm_decode(p,size,d,&info)==-NCM_NDP_CRC);
 memcpy(p,q,size);ncm_put16(p+10,13);assert(ncm_decode(p,size,d,&info)==-NCM_NDP_INDEX);
 memcpy(p,q,size);ncm_put16(p+16,12);assert(ncm_decode(p,size,d,&info)==-NCM_TERMINATOR);
 /* A legitimate pair of chained NDP tables, two distinct frames. */
 memset(p,0,256);ncm_put32(p,0x484d434e);ncm_put16(p+4,12);ncm_put16(p+8,256);ncm_put16(p+10,12);
 ncm_put32(p+12,0x304d434e);ncm_put16(p+16,16);ncm_put16(p+18,28);ncm_put16(p+20,44);ncm_put16(p+22,60);
 ncm_put32(p+28,0x304d434e);ncm_put16(p+32,16);ncm_put16(p+36,104);ncm_put16(p+38,60);
 assert(ncm_decode(p,256,d,&info)==2);
 ncm_put16(p+36,44);assert(ncm_decode(p,256,d,&info)<0); /* overlapping frames */
 /* Deterministic adversarial buffers: valid headers plus arbitrary length/index tables. */
 uint32_t rng=1;
 for(unsigned i=0;i<100000;i++) {
  for(unsigned j=0;j<256;j++) {rng=rng*1664525+1013904223;p[j]=(uint8_t)(rng>>24);}
  if(i&1) {ncm_put32(p,0x484d434e);ncm_put16(p+4,12);ncm_put16(p+8,256);ncm_put16(p+10,12);}
  (void)ncm_decode(p,256,d,&info);
 }
 puts("PASS: all Ethernet lengths, short-packet boundary, chained NDP, CRC/overlap/cycle/truncation rejection, zero block, trailing pad, unaligned frame, half-null terminator, concatenated NTBs, 16KiB receive, 100000 malformed NTBs (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-ntb-') as t:
 c=Path(t)/'test.c';exe=Path(t)/'test';c.write_text(s)
 subprocess.run([os.environ.get('CC','clang'),'-std=c11','-O1','-g','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
