// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFIClock.h>

/*
 * SimpleFbDxe inherits the panel without acquiring any ClockDxe votes.
 * In the UMI XBL ClockDxe, gcc_disp_ahb_clk and gcc_ufs_phy_ahb_clk share
 * the domain controlling GCC + 0x5050. UFS clock gating can therefore
 * release the inherited display's parent even while its CBCR is enabled.
 *
 * Dispatch immediately after ClockDxe, BEFORE UFSDxe. Keep a vote on the
 * display branch for the lifetime of this firmware. HakiOS keeps boot
 * services resident, so releasing it at ReadyToBoot would be incorrect.
 * Do not change the inherited frequency, reset MDSS, or read DISPCC/DSI:
 * accessing a downstream block while its bus clock is stopped can hang.
 */
EFI_STATUS
EFIAPI
UmiDisplayClockEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_CLOCK_PROTOCOL *Clock;
  EFI_STATUS          Status;
  UINTN               ClockId;
  BOOLEAN             IsOn;

  Status = gBS->LocateProtocol (&gEfiClockProtocolGuid, NULL, (VOID **)&Clock);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UMI display clock: LocateProtocol failed: %r\n", Status));
    return Status;
  }

  Status = Clock->GetClockID (Clock, "gcc_disp_ahb_clk", &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UMI display clock: GetClockID failed: %r\n", Status));
    return Status;
  }

  // Acquire ownership even if hardware already reports that the clock is on.
  Status = Clock->EnableClock (Clock, ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UMI display clock: EnableClock failed: %r\n", Status));
    return Status;
  }

  IsOn = FALSE;
  Status = Clock->IsClockOn (Clock, ClockId, &IsOn);
  if (EFI_ERROR (Status) || !IsOn) {
    DEBUG ((DEBUG_ERROR, "UMI display clock: vote held but clock not running: %r, on=%u\n", Status, IsOn));
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "UMI display clock: holding gcc_disp_ahb_clk before UFS dispatch\n"));
  return EFI_SUCCESS;
}
