// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared wire layout: only fixed-width fields; usable by the freestanding loader.
#ifndef UMI_DISPLAY_PROBE_H
#define UMI_DISPLAY_PROBE_H
#define UMI_DISPLAY_PROBE_GUID \
  {0x8e4060d2, 0x31bc, 0x4d78, {0x8e,0x73,0xa1,0x62,0x4c,0x95,0x2b,0x09}}
#define UMI_DISPLAY_PROBE_MAGIC 0x32504455u
#define UMI_DISPLAY_PROBE_VERSION 1
#define UMI_DISPLAY_PROBE_CAPACITY 32
#define UMI_DISPLAY_PROBE_REGS 9
// GCC only: shared AHB CMD/CFG, display/UFS AHB, display HF/SF/XO,
// MMSS BCR and UFS PHY BCR. Never read DISPCC, MDSS or DSI here.
#define UMI_DISPLAY_PROBE_OFFSETS \
  {0x5050, 0x5054, 0xb00c, 0x77018, 0xb034, 0xb038, 0xb044, 0xb000, 0x77000}
#define UMI_DISPLAY_PROBE_STAGES \
  {"invalid", "before-clock", "after-clock", "before-diskio", \
   "after-diskio", "after-partition", "before-ufs", "after-ufs", \
   "after-simplefb", "end-of-dxe", "ready-to-boot", "before-mm-vote", "after-mm-vote"}

typedef struct {
  unsigned int Stage;
  unsigned int Gcc[UMI_DISPLAY_PROBE_REGS];
} UMI_DISPLAY_PROBE_RECORD;

typedef struct {
  unsigned int Magic;
  unsigned int Version;
  unsigned int Count;
  unsigned int EventErrors; // bit 0: EndOfDxe, bit 1: ReadyToBoot registration
  unsigned long long VoteStatus; // only used by the optional rail experiment
  UMI_DISPLAY_PROBE_RECORD Records[UMI_DISPLAY_PROBE_CAPACITY];
} UMI_DISPLAY_PROBE_TABLE;
#endif
