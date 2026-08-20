/** @file
  Serial port library for Allwinner A733 NS16550-compatible UART.

  Allwinner sunxi UARTs are register-compatible with the NS16550A but use
  32-bit wide registers at 4-byte stride (unlike the original 1-byte stride).
  This wrapper configures baud rate and delegates to the upstream
  MdeModulePkg NS16550SerialPortLib via FixedPcdGet macros.

  Call flow (EDK2 boot):
    SEC phase  → SerialPortInitialize() called early (before memory)
    PEI/DXE    → serial console via EFI_SERIAL_IO_PROTOCOL

  Copyright (c) 2024, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/SerialPortLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>

// NS16550 register offsets (× STRIDE = × 4 for A733)
#define UART_RBR    0x00    // Receive Buffer Register (read)
#define UART_THR    0x00    // Transmit Holding Register (write)
#define UART_DLL    0x00    // Divisor Latch Low (when LCR.DLAB=1)
#define UART_IER    0x04    // Interrupt Enable Register
#define UART_DLH    0x04    // Divisor Latch High (when LCR.DLAB=1)
#define UART_FCR    0x08    // FIFO Control Register
#define UART_LCR    0x0C    // Line Control Register
#define UART_LSR    0x14    // Line Status Register

#define LCR_DLAB    BIT7    // Divisor Latch Access Bit
#define LCR_8N1     0x03    // 8 data bits, no parity, 1 stop
#define FCR_ENABLE  BIT0    // Enable FIFO
#define FCR_CLRRCV  BIT1    // Clear Receive FIFO
#define FCR_CLRXMT  BIT2    // Clear Transmit FIFO
#define LSR_THRE    BIT5    // Transmit Holding Register Empty
#define LSR_DR      BIT0    // Data Ready

#define UART_BASE  ((UINTN)FixedPcdGet64 (PcdSerialRegisterBase))
#define UART_CLK   ((UINT32)FixedPcdGet32 (PcdSerialClockRate))
#define UART_BAUD  ((UINT32)FixedPcdGet64 (PcdSerialBaudRate))

STATIC UINT32
UartRead32 (UINTN Offset)
{
  return MmioRead32 (UART_BASE + Offset);
}

STATIC VOID
UartWrite32 (UINTN Offset, UINT32 Value)
{
  MmioWrite32 (UART_BASE + Offset, Value);
}

//
// CPUS-domain register block, needed only when the console is UART7.
//
// UART0 sits in the main SoC domain and is already clocked and pin-muxed by
// TF-A/SPL before EDK2 runs. UART7 gets no such treatment -- nothing in the
// boot chain touches it -- so its clock must be ungated and its pins muxed
// here, before the first UART register write.
//
// These values were read back from a running Linux kernel that had UART7
// working, not derived from a datasheet:
//   R_CCU 0x0701018C : bit0 = UART7 clock gate, bit16 = reset deassert.
//                      Observed 0x00010000 gated -> 0x00010001 ungated.
//   R_PIO 0x07025000 : PL bank CFG0. Nibbles 6 and 7 select the PL6/PL7
//                      function; both read 3 (s_uart0) on a working system.
//
#define A733_UART7_BASE      0x07080000
#define A733_R_CCU_UART7     0x0701018C
#define A733_R_PIO_PL_CFG0   0x07025000
#define A733_PL67_MUX_MASK   0xFF000000
#define A733_PL67_MUX_UART   0x33000000

/**
  Ungate and pin-mux UART7. No-op for any other UART base, so the main-domain
  UART0 configuration is unaffected. UART_BASE is a FixedPcd, so the test below
  folds away at compile time.
**/
STATIC VOID
CpusUartBringUp (VOID)
{
  if (UART_BASE != A733_UART7_BASE) {
    return;
  }

  // Deassert reset and ungate the clock.
  MmioOr32 (A733_R_CCU_UART7, BIT16 | BIT0);

  // Mux PL6 (TX) and PL7 (RX) to s_uart0, leaving the other PL pins untouched.
  MmioAndThenOr32 (
    A733_R_PIO_PL_CFG0,
    (UINT32)~A733_PL67_MUX_MASK,
    A733_PL67_MUX_UART
    );
}

/**
  Initialize the UART at 115200-8N1.
  For UART0 the clock gating is already done by TF-A / SPL. For UART7 it is
  not, so CpusUartBringUp() handles that case before any register access.
**/
RETURN_STATUS
EFIAPI
SerialPortInitialize (VOID)
{
  UINT32 Divisor;

  // UART7 needs its clock and pins set up first; no-op for UART0.
  CpusUartBringUp ();

  // Disable interrupts
  UartWrite32 (UART_IER, 0x00);

  // Enable and clear FIFOs
  UartWrite32 (UART_FCR, FCR_ENABLE | FCR_CLRRCV | FCR_CLRXMT);

  // Set baud rate
  Divisor = UART_CLK / (16 * UART_BAUD);
  UartWrite32 (UART_LCR, LCR_DLAB);          // enable divisor latch
  UartWrite32 (UART_DLL, Divisor & 0xFF);
  UartWrite32 (UART_DLH, (Divisor >> 8) & 0xFF);

  // 8N1, clear DLAB
  UartWrite32 (UART_LCR, LCR_8N1);

  return RETURN_SUCCESS;
}

/**
  Write Buffer to serial port.
**/
UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN   NumberOfBytes
  )
{
  UINTN Count;

  for (Count = 0; Count < NumberOfBytes; Count++) {
    // Wait for TX FIFO empty
    while ((UartRead32 (UART_LSR) & LSR_THRE) == 0) {}
    UartWrite32 (UART_THR, Buffer[Count]);
    // Translate '\n' to '\r\n' for terminal emulators
    if (Buffer[Count] == '\n') {
      while ((UartRead32 (UART_LSR) & LSR_THRE) == 0) {}
      UartWrite32 (UART_THR, '\r');
    }
  }
  return Count;
}

/**
  Read bytes from serial port (non-blocking).
**/
UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN   NumberOfBytes
  )
{
  UINTN Count = 0;

  while (Count < NumberOfBytes) {
    if ((UartRead32 (UART_LSR) & LSR_DR) == 0) {
      break;
    }
    Buffer[Count++] = (UINT8)UartRead32 (UART_RBR);
  }
  return Count;
}

BOOLEAN
EFIAPI
SerialPortPoll (VOID)
{
  return (BOOLEAN)((UartRead32 (UART_LSR) & LSR_DR) != 0);
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (IN UINT32 Control)
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (OUT UINT32 *Control)
{
  *Control = 0;
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  return RETURN_UNSUPPORTED;
}
