# 2026-10-17: Haswell keyboard hang, APIC ID read and AHCI port bring-up

## Symptom

The Haswell-ULT laptop (i5-4300U) boots, starts the shell and reads scancodes on the kernel side. The userspace input server never delivers a key, so the shell stays silent. The cursor keeps blinking and kernel threads keep running.

QEMU and the Skylake laptop run the same image with a working keyboard.

## Root cause

`serial_init_rx()` probes COM1 through the scratch register. COM1 does not answer on this Haswell machine. The probe reads `0xff`, fails and returns. The function did not clear `serial_base`.

`serial_drain()` then reads the line status register at 0x3F8. A floating bus returns `0xff`. Bit 0 (data ready) stays set. The unbounded `for (;;)` loop called `sys_ioport_access()` again and again and never exited.

A spurious IRQ1 during boot woke the input server. The handler ran `ps2_drain()` and then entered `serial_drain()`. The server stayed in that loop and never returned to the receive loop. Every later keyboard notification queued without a reader.

## Fix

### Serial console (`userspace/servers/input.c`)

- Clear `serial_base` when the scratch-register probe fails.
- Bound `serial_drain()` to 32 bytes.
- Treat a `0xff` data byte as a floating bus and leave the loop.
- Bound `ps2_drain()` to 32 bytes.

### IRQ1 byte forwarding (`kernel/arch/x64/isr.c`, `kernel/ipc/irq.c`, `kernel/ipc/irq.h`)

- Read the i8042 data port in the IRQ1 handler after a bounded wait for the output buffer.
- Forward the byte in `words[1]` of the IRQ notification through `irq_deliver_data()`.
- The handler comment records that on some machines the byte reaches the i8042 after the interrupt.

### APIC ID read (`kernel/arch/x64/smp.c`)

- Read the local APIC ID from bits 24..31 of the ID register, matching Linux `read_apic_id()`.
- The previous code compared the whole register with the MADT APIC ID. Only ID 0 matched.

### AHCI (`userspace/servers/block.c`)

- Set ICC to Active in `port_start()` before clearing ST.
- Add `port_bringup()`. It sends a COMRESET to a port with a device signature while DET stays below 3, then waits for DET to reach 3.
- Run `port_bringup()` on every port in the PI bitmap before probing.

## Files Changed

| File | Change |
|------|--------|
| `userspace/servers/input.c` | clear `serial_base` on probe failure; bound both drains; drop a `0xff` byte |
| `kernel/arch/x64/isr.c` | read the i8042 byte in IRQ1 and forward it |
| `kernel/ipc/irq.c`, `kernel/ipc/irq.h` | `irq_deliver_data()` carries one word |
| `kernel/arch/x64/smp.c` | shift the APIC ID register right by 24 |
| `userspace/servers/block.c` | ICC Active; COMRESET port bring-up |

## Testing

- Kernel and userspace build with no `-Wall -Wextra` warnings.
- QEMU `-smp 1`, `-smp 2` and `-smp 4`: keyboard input executes `/bin/ls`; no panic, exception or corrupt line.
- Haswell: keyboard input executes commands after the fix.
- Skylake: boots and executes commands.
- AHCI changes wait for a physical SATA disk test.
