#!/usr/bin/env python3
"""Independent MDC1200 encoder vectors for checking firmware/SDR captures."""


def crc16_mdc(data):
    crc = 0
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def encode(op, arg, unit_id):
    info = [op, arg, unit_id >> 8, unit_id & 0xFF]
    crc = crc16_mdc(info)
    info += [crc & 0xFF, crc >> 8, 0]

    shift = 0
    parity = []
    for value in info:
        encoded = 0
        for bit in range(8):
            shift = ((shift << 1) | ((value >> bit) & 1)) & 0xFF
            encoded |= (
                ((shift >> 6) ^ (shift >> 5) ^ (shift >> 2) ^ shift) & 1
            ) << bit
        parity.append(encoded)

    coded = info + parity
    interleaved_bits = [0] * 112
    index = 0
    for value in coded:
        for bit in range(8):
            interleaved_bits[index] = (value >> bit) & 1
            index += 16
            if index >= 112:
                index -= 111

    payload = []
    for byte_index in range(14):
        value = 0
        for bit in range(8):
            value |= interleaved_bits[byte_index * 8 + bit] << (7 - bit)
        payload.append(value)

    # Exact 9M2RTX framing: 00 FF preamble and no trailing postamble.
    logical = [0, 0xFF, 0x07, 0x09, 0x2A, 0x44, 0x6F]
    logical += payload

    previous = 0
    on_air = []
    for value in logical:
        encoded = 0
        for bit in range(7, -1, -1):
            current = (value >> bit) & 1
            if current != previous:
                encoded |= 1 << bit
            previous = current
        on_air.append(encoded ^ 0xFF)
    return crc, coded, payload, on_air


def hexes(values):
    return " ".join(f"{value:02X}" for value in values)


def main():
    crc, coded, payload, on_air = encode(0x01, 0x80, 0x1234)
    assert crc == 0x3E2E
    assert hexes(coded) == "01 80 12 34 2E 3E 00 65 80 A8 62 DD 88 08"
    assert hexes(payload) == "8E 94 2A 2E 06 99 22 04 00 1C 18 A2 2C 88"
    assert hexes(on_air) == (
        "FF 7F 7B 72 40 99 A7 B6 21 C0 C6 FA 2A 4C F9 "
        "FF ED EB 0C C5 33"
    )
    print("MDC1200 golden vector: PASS")
    print("logical CRC: 3E2E")
    print("on-air bytes:", hexes(on_air))


if __name__ == "__main__":
    main()
