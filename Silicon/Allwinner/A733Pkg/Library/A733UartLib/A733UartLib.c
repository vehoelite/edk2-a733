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

/**
  Initialize the UART at 115200-8N1.
  The clock gating for UART0 must already be enabled by TF-A / SPL before
  EDK2 is entered; doing CCU writes here is unnecessary for most sunxi ports.
**/
RETURN_STATUS
EFIAPI
SerialPortInitialize (VOID)
{
  UINT32 Divisor;

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
