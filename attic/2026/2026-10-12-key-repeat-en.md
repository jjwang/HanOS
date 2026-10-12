# 2026-10-12: Key repeat on a physical keyboard

## Symptom

On a machine with a PS/2 keyboard, one key press often shows several copies of
the character. QEMU hides it: the monitor `sendkey` sends a make and a break
back to back, so the software repeat never runs.

## Root causes

### 1. The repeat armed on a fixed delay

`key_press` scheduled the first repeat 250 ms after the press. A press that
lasted past 250 ms repeated even when the user meant a single tap.

### 2. A make after a recent break counted as a new press

The input server sent a character whenever a make arrived for a key that was
not down. A bouncy contact, or a stray make, delivers make, break, make in a
few milliseconds; the second make looked like a fresh press and added a copy.

## Implementation

### Gate the repeat on the typematic (ed600ac)

- `key_press` delivers the character and arms the repeat but does not schedule
  it.
- The repeat starts only after the keyboard's own typematic sends a second make
  for the held key. A real typematic repeats at its rate, so one stray make does
  not start a repeat.
- The software period (40 ms) caps the rate whatever the keyboard sends; the
  confirm window still stops a lost break.

### Debounce the make (ed600ac)

- Track the last break time per key.
- Drop a make that follows a break of the same key within 25 ms.

## Files Changed

| File | Change |
|------|--------|
| `userspace/servers/input.c` | debounce makes; start the repeat on the typematic |

## Testing

- Kernel and userspace build with no warnings.
- Boot on `-smp 2` and `-smp 4`: no panic, exception or corrupt line.
- Interactive `ls` and `pwd` run; `snake` loads and exits 0.

## Not yet done

- Confirm on the physical keyboard: a single tap gives one character, and a
  held key repeats.
