// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "UmiDisplayProbe.h"

STATIC EFI_GUID mProbeGuid = UMI_DISPLAY_PROBE_GUID;
STATIC UMI_DISPLAY_PROBE_TABLE *mTable;

STATIC VOID
Snapshot (UINT32 Stage)
{
  STATIC CONST UINT32 Offsets[] = UMI_DISPLAY_PROBE_OFFSETS;
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mTable->Count < UMI_DISPLAY_PROBE_CAPACITY) {
    UMI_DISPLAY_PROBE_RECORD *Record = &mTable->Records[mTable->Count];
    Record->Stage = Stage;
    for (UINTN i = 0; i < UMI_DISPLAY_PROBE_REGS; ++i) {
      Record->Gcc[i] = MmioRead32 (0x100000 + Offsets[i]);
    }
    ++mTable->Count;
  }
  gBS->RestoreTPL (OldTpl);
}

STATIC VOID EFIAPI
OnBootEvent (EFI_EVENT Event, VOID *Context)
{
  Snapshot ((UINT32)(UINTN)Context);
}

EFI_STATUS EFIAPI
UmiDisplayProbeEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_EVENT Event;
  for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; ++i) {
    if (CompareGuid (&SystemTable->ConfigurationTable[i].VendorGuid, &mProbeGuid)) {
      mTable = SystemTable->ConfigurationTable[i].VendorTable;
      break;
    }
  }
  if (mTable == NULL) {
    mTable = AllocateRuntimeZeroPool (sizeof (*mTable));
    if (mTable == NULL) return EFI_OUT_OF_RESOURCES;
    mTable->Magic = UMI_DISPLAY_PROBE_MAGIC;
    mTable->Version = UMI_DISPLAY_PROBE_VERSION;
    mTable->VoteStatus = EFI_NOT_STARTED;
    Status = gBS->InstallConfigurationTable (&mProbeGuid, mTable);
    if (EFI_ERROR (Status)) {
      FreePool (mTable);
      return Status;
    }
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnBootEvent,
                               (VOID *)(UINTN)9, &gEfiEndOfDxeEventGroupGuid, &Event);
    if (EFI_ERROR (Status)) mTable->EventErrors |= 1;
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnBootEvent,
                               (VOID *)(UINTN)10, &gEfiEventReadyToBootGuid, &Event);
    if (EFI_ERROR (Status)) mTable->EventErrors |= 2;
  }
  Snapshot (UMI_PROBE_STAGE);
  return EFI_SUCCESS;
}
