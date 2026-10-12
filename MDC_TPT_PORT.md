# MDC1200 and TPT port for UV-K1 / UV-K5 V3

This tree is based on F4HWN v6.1.0 for the PY32F071/BK4829 platform, from
upstream commit `daec4b82e8c54813402f324074f54cb5c0a77025` (2026-10-10).

## Current status

- MDC1200 single-packet encoder and decoder (CRC, FEC, interleave and
  differential/XOR line coding)
- configurable unit ID
- PRE, POST and PRE+POST PTT ID modes
- configurable MDC preamble, zero to eight cycles
- software FFSK receive setting and six-second last-ID display integration
- Talk Permit Tone modes: XTS, TRBO, Kenwood and Auto
- settings persisted in the PY25Q16 config bank at `0x00A178..0x00A17F`

The feature is controlled by `ENABLE_MDC1200` and is enabled in the supplied
CMake presets. All settings default to OFF after the first boot.

MDC **transmit is enabled in normal builds** and deliberately bypasses the FSK
packet FIFO. It sends the finished line-coded bits through the BK4829 tone
generator, one symbol every 40,000 CPU cycles: bit 1 is 1200 Hz and bit 0 is
1800 Hz. This is the same hardware/timing approach already used by F4HWN's APRS
transmitter and does not assume that BK4819 and BK4829 serialize `REG_5F` alike.

MDC receive is enabled in normal builds when `MDC Rx` is switched on. It does
not use `REG_58..REG_5F`: discriminator audio on PA4 is sampled by ADC1 at
14.4 kHz, then a fixed-point 1200/1800 Hz correlator and DPLL recover the bit
stream. This path was adapted from Dondji's K1/K5-V3 implementation.

This separation is intentional:

- the MDC protocol bytes can be verified on a host and over discriminator/IQ
  capture without depending on a chip-specific packet value;
- TPT uses the BK4829 tone generator and is independent of MDC packet framing;
- neither direction depends on BK4829 FSK FIFO serialization.

## Important CRC correction

TrueMDC's `compute_crc()` used a hardware CRC block configured for bit-reversed
input, reversed output and output inversion. F4HWN's software
`CRC_Calculate()` is plain CRC-16/XMODEM despite the similar function name.
They are not interchangeable. The port now uses a private MDC CRC routine; the
known vector `01 80 12 34` evaluates to `0x3E2E`.

## Protocol layers and the BK4819/BK4829 warning

Three values must not be confused:

1. The logical packet (`op`, `arg`, unit ID, CRC and FEC).
2. The differential/XOR line-coded bit stream actually mapped to 1200/1800 Hz.
3. A transceiver chip's 16-bit `REG_5F` FIFO write value and its private byte/
   bit serialization.

Only the first two are MDC1200. Number 3 may differ between BK4819 and BK4829,
even when a crude fixed table produces a similar sound. The old F4HWN
`FSK_RogerTable` is only a fixed seven-word sound effect: it uses `55 55 55 55`
as sync rather than MDC's `07 09 2A 44 6F`, so it must not be treated as an MDC
packet reference.

For `op=01`, `arg=80`, ID `1234`, the golden values are:

- CRC: `3E2E` (stored low byte first: `2E 3E`)
- information + FEC: `01 80 12 34 2E 3E 00 65 80 A8 62 DD 88 08`
- interleaved payload: `8E 94 2A 2E 06 99 22 04 00 1C 18 A2 2C 88`
- complete 21-byte 9M2RTX line-coded stream, excluding the optional raw-tone
  `MDCPre` prefix:
  `FF 7F 7B 72 40 99 A7 B6 21 C0 C6 FA 2A 4C F9 FF ED EB 0C C5 33`

Run `python tools/verify_mdc1200.py` to reproduce these values independently.

## Radio menus

| Menu | Purpose |
| --- | --- |
| RxLED | OFF, SOLID or the 9M2RTX 80 ms / 500 ms BLINK pattern |
| MDCID | Four hexadecimal digits, 0000 to FFFF; STAR enters/cycles A to F |
| MDCTx | OFF, PRE, POST or PRE+POST |
| MDCRx | Enable PA4/ADC software receive decoding |
| MDCMod | MDC original lead cadence or the existing RAIL sound |
| MDC Pre | Additional long-preamble cycles, 0 to 8 |
| TPT | OFF, XTS, TRBO, KENWOOD or AUTO |

The legacy Roger menu and end-of-transmission call are removed so they cannot
overlap MDC POST.  Status LEDs follow the 9M2RTX state machine: red while the
PTT signalling sequence is still running, green after talk permit, yellow for
3.6 seconds after an FM call (or until dual watch resumes), plus the original
power-save battery pulse timings.

`AUTO` selects XTS when MDC transmit is disabled and TRBO when MDC transmit is
enabled.

## Build

Use the normal upstream build flow, for example:

```sh
./compile-firmware.sh Fusion
```

The expected result is `build/Fusion/f4hwn.fusion.bin`.

## BK4829 hardware validation checklist

Before transmitting, back up calibration and the complete external flash with
UV Studio. Test into a dummy load or at minimum power where appropriate.

1. Initially keep receive off and set `MDC Tx` to PRE.
2. Set a known unit ID (`0x1234`) and preamble value 2.
3. Capture a known-good Motorola/BK4819 transmission and the BK4829 candidate as
   discriminator audio or IQ, then demodulate both to bits.
4. Compare the candidate against the golden stream above. Do not accept an
   audio-only comparison.
5. Verify symbol time is 833.333 us and that frequency changes remain phase
   continuous. Check mark at 1200 Hz and space at 1800 Hz.
6. Verify PRE uses argument `0x80` and POST uses argument `0x00`.
7. Check deviation in wide and narrow bandwidth; adjust `MDC_TONE_GAIN`
   in `App/app/mdc1200.c` only if measurements require it.
8. Enable receive and verify ID popup, normal squelch, CTCSS/DCS and dual-watch
   behavior before regular use. ADC software RX is unavailable when the Voice
   feature owns PA4.

The circulated 2023 BK4829 register table describes `REG_58..REG_5F`, but its
FSK section is nearly verbatim the older BK4819 v3 table and does not specify
FIFO bit/byte serialization. The upstream F4HWN discussion also records that a
straight BK4819-style MDC RX setup did not generate a BK4829 FSK interrupt.
Accordingly, TX avoids that ambiguity and RX bypasses the modem entirely.

## Source and licensing

The protocol implementation was adapted from the Apache-2.0 TrueMDC project:

<https://github.com/IvanMaestroGTR/TrueMDC-9M2RTX-Losehu-UVK5-Firmware>

The platform base is the Apache-2.0 F4HWN K1/K5-V3 project:

<https://github.com/armel/uv-k1-k5v3-firmware-custom>

The PA4/ADC software demodulator was adapted from the Apache-2.0 Dondji project:

<https://github.com/EthanYan6/Dondji>
