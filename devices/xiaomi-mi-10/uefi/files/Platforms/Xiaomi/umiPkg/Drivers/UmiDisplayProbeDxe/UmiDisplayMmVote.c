// SPDX-License-Identifier: BSD-2-Clause-Patent
// Experimental ownership of the inherited display rail. No raw power writes.
#define UMI_PROBE_STAGE 11
#define UmiDisplayProbeEntry UmiDisplayProbeInitEntry
#include "UmiDisplayProbe.c"
#undef UmiDisplayProbeEntry

/* UMI NpaDxe revision 0x10002, table RVA 0x110a8. The wrappers have no
 * This argument: +0x10 creates a synchronous client, +0x28 issues its vote.
 * UFSDxe uses these same slots at RVAs 0x142fc / 0x14380. */
typedef struct {
  UINT64 Revision;
  VOID *Init;
  EFI_STATUS (EFIAPI *CreateSyncClient)(CONST CHAR8 *, CONST CHAR8 *, UINT32,
                                      UINT32, VOID *, VOID **);
  VOID *DestroyClient;
  VOID *DefineNode;
  EFI_STATUS (EFIAPI *IssueRequiredRequest)(VOID *, UINT32);
} UMI_NPA_PROTOCOL;

STATIC EFI_GUID mNpaGuid =
  {0x79d6c870, 0x725e, 0x489e, {0xa0,0xa1,0x27,0xe7,0xa5,0xd0,0xcb,0x35}};
STATIC VOID *mClient;

EFI_STATUS EFIAPI
UmiDisplayMmVoteEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  UMI_NPA_PROTOCOL *Npa;
  EFI_STATUS Status = UmiDisplayProbeInitEntry (ImageHandle, SystemTable);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->LocateProtocol (&mNpaGuid, NULL, (VOID **)&Npa);
  if (!EFI_ERROR (Status)) {
    if (Npa->Revision != 0x10002) {
      Status = EFI_UNSUPPORTED;
    } else {
      // NPA_CLIENT_REQUIRED=0x40; independent client, never release while resident.
      Status = Npa->CreateSyncClient ("/vcs/vdd_mm", "umi-display", 0x40, 0, NULL, &mClient);
      if (!EFI_ERROR (Status)) {
        if (mClient == NULL) {
          Status = EFI_NOT_FOUND;
        } else {
          // ClockDxe's own boot-default level for this rail is 0x100 (NOMINAL),
          // table RVA 0x311f8. VcsDxe maps the resource to mmcx.lvl.
          Status = Npa->IssueRequiredRequest (mClient, 0x100);
        }
      }
    }
  }
  mTable->VoteStatus = Status;
  Snapshot (12);
  // Keep the installed table and event callbacks alive even when voting failed.
  return EFI_SUCCESS;
}
