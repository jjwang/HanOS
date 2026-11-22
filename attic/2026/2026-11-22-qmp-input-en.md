# Inject test input through QMP

`sendkey` through the HMP monitor dropped the second of two repeated
characters: a typed `root` arrived as `rot`, so the login test could not
authenticate. `scripts/stress-test.sh` now opens a QMP socket and calls
`input-send-event` with an explicit key down/up for every character, which does
not coalesce repeats.
