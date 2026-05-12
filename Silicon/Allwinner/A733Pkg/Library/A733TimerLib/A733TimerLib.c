/** @file
  ARM Generic Timer library for Allwinner A733.

  The Cortex-A76 and Cortex-A55 cores both implement the ARMv8 architectural
  system timer via system registers (CNTP_*, CNTV_*, CNTFRQ_EL0).  No MMIO
  is required; this is identical to the upstream ArmPkg/Library/ArmGenericTimerCounterLib
  but adds A733-specific frequency enforcement.

  TF-A BL31 is expected to write CNTFRQ_EL0 = 24000000 (24 MHz HOSC) and
  enable the counter before handing off to EDK2.  If it does not, the
  TimerConstructor() below will do it from EL2.

  Copyright (c) 2024, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/ArmLib.h>
#include <Library/TimerLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>

#define A733_TIMER_FREQ  FixedPcdGet32 (PcdArmArchTimerFreqInHz)

/**
  Constructor: ensure the counter is running and CNTFRQ is set.
  This runs before any UINTN DivU64x32 calls.
**/
RETURN_STATUS
EFIAPI
A733TimerConstructor (VOID)
{
  // Write CNTFRQ_EL0 if it has not been set by TF-A.
  // (Writing from EL2 is legal; from EL1 it is read-only if EL2 is present.)
  if (ArmReadCntFrq () != A733_TIMER_FREQ) {
    ArmWriteCntFrq (A733_TIMER_FREQ);
  }
  return RETURN_SUCCESS;
}

/**
  Stall the CPU for at least MicroSeconds microseconds.
**/
UINTN
EFIAPI
MicroSecondDelay (
  IN UINTN MicroSeconds
  )
{
  UINT64 Ticks;
  UINT64 Start;
  UINT64 Freq;

  Freq  = (UINT64)ArmReadCntFrq ();
  Ticks = DivU64x32 (MultU64x64 (MicroSeconds, Freq), 1000000U);
  Start = ArmReadCntPct ();

  while ((ArmReadCntPct () - Start) < Ticks) {
    ArmCallWFE ();  // low-power spin (WFE exits on timer events)
  }
  return MicroSeconds;
}

/**
  Stall the CPU for at least NanoSeconds nanoseconds.
**/
UINTN
EFIAPI
NanoSecondDelay (
  IN UINTN NanoSeconds
  )
{
  UINT64 Ticks;
  UINT64 Start;
  UINT64 Freq;

  Freq  = (UINT64)ArmReadCntFrq ();
  Ticks = DivU64x32 (MultU64x64 (NanoSeconds, Freq), 1000000000U);
  Start = ArmReadCntPct ();

  while ((ArmReadCntPct () - Start) < Ticks) {}
  return NanoSeconds;
}

/**
  Return current timer tick count.
**/
UINT64
EFIAPI
GetPerformanceCounter (VOID)
{
  return ArmReadCntPct ();
}

/**
  Return counter properties (start, end, frequency).
**/
UINT64
EFIAPI
GetPerformanceCounterProperties (
  OUT UINT64  *StartValue   OPTIONAL,
  OUT UINT64  *EndValue     OPTIONAL
  )
{
  if (StartValue != NULL) {
    *StartValue = 0ULL;
  }
  if (EndValue != NULL) {
    *EndValue = MAX_UINT64;
  }
  return (UINT64)ArmReadCntFrq ();
}

/**
  Convert ticks to nanoseconds.
**/
UINT64
EFIAPI
GetTimeInNanoSecond (
  IN UINT64 Ticks
  )
{
  return DivU64x32 (MultU64x64 (Ticks, 1000000000ULL),
                    (UINT32)ArmReadCntFrq ());
}
