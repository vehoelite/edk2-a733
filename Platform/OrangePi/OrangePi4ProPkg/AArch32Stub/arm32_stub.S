/*
 * arm32_stub.S - ARM32 entry trampoline for AArch64 EDK2 under AArch32 U-Boot
 *
 * U-Boot (AArch32) jumps here via "bootm" do_bootm_standalone.
 * We use PSCI CPU_ON (SMC) to launch a secondary CPU in AArch64 mode at the
 * real EDK2 entry point (immediately after this 4KB stub page), then halt CPU0.
 *
 * Memory layout after mkimage wraps it:
 *   0x44000000 : this ARM32 stub (4096 bytes, padded)
 *   0x44001000 : ORANGEPI4PRO_EFI.fd (4MB EDK2 FD, AArch64 entry at offset 0)
 *
 * PSCI AArch32 CPU_ON: SMC #0 with r0=0x84000003, r1=MPIDR, r2=entry, r3=ctx
 * CPU 1 MPIDR on A733: Aff1=0, Aff0=1 → MPIDR = 0x00000001
 * CPU 0 MPIDR: 0x00000000
 *
 * TF-A will launch the target CPU in AArch64 mode because SCR_EL3.RW is set
 * for AArch64 non-secure world (same as it does for the Linux kernel).
 */

    .syntax unified
    .arm

    .global _start
_start:
    /* PSCI_CPU_ON (SMC32): wake CPU1 at the AArch64 EDK2 entry point */
    ldr     r0, =0x84000003         /* PSCI function: CPU_ON (32-bit) */
    ldr     r1, =0x00000001         /* target_cpu: MPIDR of CPU1 (Aff0=1) */
    ldr     r2, =0x44001000         /* entry_point: EDK2 FD start (AArch64) */
    mov     r3, #0                  /* context_id */
    smc     #0

    /* CPU0: disable interrupts and spin in WFI */
    cpsid   aif
1:  wfi
    b       1b

    /* Pad to 4096 bytes so EDK2 FD starts at a clean 4KB boundary */
    .balign 4096, 0
