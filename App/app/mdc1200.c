/* MDC1200 protocol codec derived from TrueMDC.
 *
 * TX deliberately does not use the BK4819/BK4829 FSK packet FIFO.  The two
 * chips do not have a proven compatible FIFO serializer for this mode.  The
 * already line-coded MDC bits are instead sent through the BK4829 tone
 * generator at an exact 1200 symbols/s, as F4HWN's APRS transmitter does.
 * RX still uses the undocumented packet modem and remains experimental.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mdc1200.h"

#include <string.h>

#include "driver/bk4819.h"
#include "driver/system.h"
#include "misc.h"
#include "settings.h"

/* Keep the 9M2RTX packet framing verbatim: its packet starts with 00 FF,
 * followed by the 40-bit sync and 112-bit coded payload.  It does not append
 * a postamble after the payload. */
static const uint8_t mdcPreamble[] = {0x00, 0xFF};
static const uint8_t mdcSync[] = {0x07, 0x09, 0x2A, 0x44, 0x6F};

/* SysTick runs from the 48 MHz core clock with a 10 ms reload. */
#define MDC_SYST_LOAD       (*(volatile uint32_t *)0xE000E014u)
#define MDC_SYST_VAL        (*(volatile uint32_t *)0xE000E018u)
#define MDC_CYCLES_PER_BIT  (48000000u / 1200u)
#define MDC_TONE_MARK       12389u /* 1200 Hz, BK4829 scale_freq() result */
#define MDC_TONE_SPACE      18584u /* 1800 Hz, BK4829 scale_freq() result */
#define MDC_TONE_GAIN       66u

uint8_t  gMdc1200LastOp;
uint8_t  gMdc1200LastArg;
uint16_t gMdc1200LastUnitId;
uint8_t  gMdc1200RxDisplayTicks;

typedef struct {
    uint8_t bit;
    uint8_t xorBit;
    uint64_t shiftRegister;
    unsigned int bitCount;
    unsigned int stage;
    unsigned int dataIndex;
    uint8_t data[MDC1200_FEC_K * 2u];
} MDC1200_RxState;

static MDC1200_RxState rx;

typedef struct {
    uint32_t previous;
    uint32_t elapsed;
} MDC1200_Clock;

static void MDC1200_ClockStart(MDC1200_Clock *clock)
{
    clock->previous = MDC_SYST_VAL;
    clock->elapsed = 0;
}

static uint32_t MDC1200_ClockCycles(MDC1200_Clock *clock)
{
    const uint32_t value = MDC_SYST_VAL;
    clock->elapsed += value <= clock->previous
                    ? clock->previous - value
                    : clock->previous + (MDC_SYST_LOAD + 1u - value);
    clock->previous = value;
    return clock->elapsed;
}

static uint16_t MDC1200_ReverseBits(uint16_t value, unsigned int bits)
{
    uint16_t reversed = 0;
    while (bits-- != 0u) {
        reversed = (uint16_t)((reversed << 1) | (value & 1u));
        value >>= 1;
    }
    return reversed;
}

/* Motorola MDC-1200 CRC-16.  This is deliberately local: F4HWN's generic
 * CRC_Calculate() implements CRC-16/XMODEM and is not wire-compatible. */
static uint16_t MDC1200_CalculateCrc(const uint8_t *data, unsigned int size)
{
    uint16_t crc = 0;

    for (unsigned int i = 0; i < size; ++i) {
        const uint16_t input = MDC1200_ReverseBits(data[i], 8);
        for (uint16_t mask = 0x80u; mask != 0u; mask >>= 1) {
            uint16_t feedback = crc & 0x8000u;
            crc <<= 1;
            if ((input & mask) != 0u)
                feedback ^= 0x8000u;
            if (feedback != 0u)
                crc ^= 0x1021u;
        }
    }

    return (uint16_t)(MDC1200_ReverseBits(crc, 16) ^ 0xFFFFu);
}

void MDC1200_FormatUnitId(char output[5], uint16_t unitId)
{
    static const char hex[] = "0123456789ABCDEF";
    output[0] = hex[(unitId >> 12) & 0x0Fu];
    output[1] = hex[(unitId >> 8) & 0x0Fu];
    output[2] = hex[(unitId >> 4) & 0x0Fu];
    output[3] = hex[unitId & 0x0Fu];
    output[4] = '\0';
}

static void MDC1200_ResetRx(void)
{
    memset(&rx, 0, sizeof(rx));
}

static void MDC1200_ErrorCorrection(uint8_t *data)
{
    uint8_t shiftRegister = 0;
    uint8_t syndrome = 0;

    for (unsigned int i = 0; i < MDC1200_FEC_K; ++i) {
        const uint8_t input = data[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            unsigned int errors = 0;
            shiftRegister = (uint8_t)((shiftRegister << 1) | ((input >> bit) & 1u));
            const uint8_t parity = (uint8_t)(((shiftRegister >> 6) ^
                                               (shiftRegister >> 5) ^
                                               (shiftRegister >> 2) ^ shiftRegister) & 1u);
            syndrome = (uint8_t)((syndrome << 1) |
                       ((parity ^ (data[i + MDC1200_FEC_K] >> bit)) & 1u));

            errors += (syndrome >> 7) & 1u;
            errors += (syndrome >> 5) & 1u;
            errors += (syndrome >> 2) & 1u;
            errors += (syndrome >> 1) & 1u;
            if (errors >= 3u) {
                int byteIndex = (int)i;
                int bitIndex = (int)bit - 7;
                if (bitIndex < 0) {
                    bitIndex += 8;
                    --byteIndex;
                }
                if (byteIndex >= 0)
                    data[byteIndex] ^= (uint8_t)(1u << bitIndex);
                syndrome ^= 0xA6u;
            }
        }
    }
}

static bool MDC1200_DecodeData(uint8_t *data)
{
    uint8_t encoded[MDC1200_FEC_K * 2u];
    uint8_t bits[MDC1200_FEC_K * 2u * 8u];
    unsigned int out = 0;

    for (unsigned int column = 0; column < 16u; ++column) {
        for (unsigned int row = 0; row < MDC1200_FEC_K; ++row) {
            const unsigned int n = row * 16u + column;
            bits[out++] = (uint8_t)((data[n >> 3] >> ((7u - n) & 7u)) & 1u);
        }
    }

    out = 0;
    for (unsigned int i = 0; i < sizeof(encoded); ++i) {
        uint8_t value = 0;
        for (unsigned int bit = 0; bit < 8u; ++bit)
            value |= (uint8_t)(bits[out++] << bit);
        data[i] = value;
    }

    memcpy(encoded, data, sizeof(encoded));
    MDC1200_ErrorCorrection(data);
    if (MDC1200_CalculateCrc(data, 4) == ((uint16_t)data[5] << 8 | data[4]))
        return true;

    /* The original decoder retries one residual coded-bit error. */
    for (unsigned int bit = 0; bit < sizeof(encoded) * 8u; ++bit) {
        memcpy(data, encoded, sizeof(encoded));
        data[bit >> 3] ^= (uint8_t)(1u << (bit & 7u));
        MDC1200_ErrorCorrection(data);
        if (MDC1200_CalculateCrc(data, 4) == ((uint16_t)data[5] << 8 | data[4]))
            return true;
    }

    memcpy(data, encoded, sizeof(encoded));
    return false;
}

static void MDC1200_XorModulation(uint8_t *data, unsigned int size)
{
    uint8_t previous = 0;
    for (unsigned int i = 0; i < size; ++i) {
        const uint8_t input = data[i];
        uint8_t output = 0;
        for (int bit = 7; bit >= 0; --bit) {
            const uint8_t next = (uint8_t)((input >> bit) & 1u);
            if (next != previous)
                output |= (uint8_t)(1u << bit);
            previous = next;
        }
        data[i] = (uint8_t)(output ^ 0xFFu);
    }
}

static uint8_t *MDC1200_EncodeData(uint8_t *data)
{
    uint8_t shiftRegister = 0;
    for (unsigned int i = 0; i < MDC1200_FEC_K; ++i) {
        const uint8_t input = data[i];
        uint8_t parity = 0;
        for (unsigned int bit = 0; bit < 8u; ++bit) {
            shiftRegister = (uint8_t)((shiftRegister << 1) | ((input >> bit) & 1u));
            parity |= (uint8_t)((((shiftRegister >> 6) ^ (shiftRegister >> 5) ^
                                  (shiftRegister >> 2) ^ shiftRegister) & 1u) << bit);
        }
        data[MDC1200_FEC_K + i] = parity;
    }

    uint8_t interleaved[MDC1200_FEC_K * 2u * 8u];
    unsigned int index = 0;
    for (unsigned int i = 0; i < MDC1200_FEC_K * 2u; ++i) {
        for (unsigned int bit = 0; bit < 8u; ++bit) {
            interleaved[index] = (uint8_t)((data[i] >> bit) & 1u);
            index += 16u;
            if (index >= sizeof(interleaved))
                index -= sizeof(interleaved) - 1u;
        }
    }

    index = 0;
    for (unsigned int i = 0; i < MDC1200_FEC_K * 2u; ++i) {
        uint8_t value = 0;
        for (int bit = 7; bit >= 0; --bit)
            value |= (uint8_t)(interleaved[index++] << bit);
        data[i] = value;
    }
    return data + MDC1200_FEC_K * 2u;
}

unsigned int MDC1200_EncodeSinglePacket(void *buffer, uint8_t op, uint8_t arg, uint16_t unitId)
{
    uint8_t *const start = (uint8_t *)buffer;
    uint8_t *p = start;

    memcpy(p, mdcPreamble, sizeof(mdcPreamble));
    p += sizeof(mdcPreamble);
    memcpy(p, mdcSync, sizeof(mdcSync));
    p += sizeof(mdcSync);

    p[0] = op;
    p[1] = arg;
    p[2] = (uint8_t)(unitId >> 8);
    p[3] = (uint8_t)unitId;
    const uint16_t crc = MDC1200_CalculateCrc(p, 4);
    p[4] = (uint8_t)crc;
    p[5] = (uint8_t)(crc >> 8);
    p[6] = 0;
    p = MDC1200_EncodeData(p);

    const unsigned int size = (unsigned int)(p - start);
    MDC1200_XorModulation(start, size);
    return size;
}

bool MDC1200_ProcessRxData(const void *buffer, unsigned int size,
                          uint8_t *op, uint8_t *arg, uint16_t *unitId)
{
    const uint8_t *input = (const uint8_t *)buffer;
    MDC1200_ResetRx();

    for (unsigned int i = 0; i < size; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            rx.bit = (uint8_t)((input[i] >> bit) & 1u);
            rx.xorBit = (uint8_t)((rx.xorBit ^ rx.bit) & 1u);
            rx.shiftRegister = (rx.shiftRegister << 1) | rx.xorBit;
            ++rx.bitCount;

            if (rx.stage == 0u) {
                if (rx.bitCount >= 40u) {
                    uint64_t normal = 0x07092A446Full ^ rx.shiftRegister;
                    uint64_t inverted = (0xFFFFFFFFFFull ^ 0x07092A446Full) ^ rx.shiftRegister;
                    unsigned int normalMatches = 40u;
                    unsigned int invertedMatches = 40u;
                    for (unsigned int n = 0; n < 40u; ++n) {
                        normalMatches -= (unsigned int)((normal >> n) & 1u);
                        invertedMatches -= (unsigned int)((inverted >> n) & 1u);
                    }
                    if (normalMatches >= 32u || invertedMatches >= 32u) {
                        rx.dataIndex = 0;
                        rx.bitCount = 0;
                        rx.stage = 1;
                    }
                }
                continue;
            }

            if (rx.bitCount < 8u)
                continue;
            rx.bitCount = 0;
            rx.data[rx.dataIndex++] = (uint8_t)rx.shiftRegister;
            if (rx.dataIndex < sizeof(rx.data))
                continue;

            if (MDC1200_DecodeData(rx.data)) {
                *op = rx.data[0];
                *arg = rx.data[1];
                *unitId = (uint16_t)((uint16_t)rx.data[2] << 8 | rx.data[3]);
                MDC1200_ResetRx();
                return true;
            }
            MDC1200_ResetRx();
        }
    }
    MDC1200_ResetRx();
    return false;
}

bool MDC1200_Send(uint8_t op, uint8_t arg, uint16_t unitId,
                  uint8_t preambleCycles, uint8_t toneMode)
{
    uint8_t packet[96];
    unsigned int prefixSize = 0;
    if (preambleCycles > 8u)
        preambleCycles = 8u;

    if (toneMode == MDC1200_TONE_CLASSIC) {
        /* Exact 9M2RTX lead pattern: the first 1800-Hz run is one byte,
         * later runs are four bytes, and every 1200-Hz run is three bytes.
         * The original builds this outside the MDC differential encoder too. */
        for (unsigned int cycle = 0; cycle < preambleCycles; ++cycle) {
            const unsigned int spaceBytes = cycle == 0u ? 1u : 4u;
            memset(packet + prefixSize, 0x00, spaceBytes);
            prefixSize += spaceBytes;
            memset(packet + prefixSize, 0xFF, 3u);
            prefixSize += 3u;
        }
    } else {
        /* Existing K1/V3 sound: each step adds 32 continuous mark symbols. */
        prefixSize = (unsigned int)preambleCycles * 4u;
        memset(packet, 0xFF, prefixSize);
    }
    const unsigned int size = prefixSize +
        MDC1200_EncodeSinglePacket(packet + prefixSize, op, arg, unitId);

    /* The line-coded byte is transmitted MSB first: 1 = 1200 Hz mark,
     * 0 = 1800 Hz space.  Rewriting REG_71 changes frequency without turning
     * the tone generator off, preserving its phase accumulator. */
    const uint8_t firstBit = (uint8_t)(packet[0] >> 7);
    BK4819_EnterTxMute();
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_71,
                         firstBit != 0u ? MDC_TONE_MARK : MDC_TONE_SPACE);
    BK4819_WriteRegister(BK4819_REG_70,
                         BK4819_REG_70_ENABLE_TONE1 |
                         ((toneMode == MDC1200_TONE_CLASSIC ? 96u : MDC_TONE_GAIN)
                          << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
    BK4819_ExitTxMute();

    MDC1200_Clock clock;
    MDC1200_ClockStart(&clock);
    uint32_t boundary = MDC_CYCLES_PER_BIT;
    uint8_t previousBit = firstBit;
    const unsigned int bitCount = size * 8u;

    for (unsigned int bitIndex = 1; bitIndex < bitCount; ++bitIndex) {
        while ((int32_t)(MDC1200_ClockCycles(&clock) - boundary) < 0) {}
        const uint8_t bit = (uint8_t)
            ((packet[bitIndex >> 3] >> (7u - (bitIndex & 7u))) & 1u);
        if (bit != previousBit) {
            BK4819_WriteRegister(BK4819_REG_71,
                                 bit != 0u ? MDC_TONE_MARK : MDC_TONE_SPACE);
            previousBit = bit;
        }
        boundary += MDC_CYCLES_PER_BIT;
    }
    while ((int32_t)(MDC1200_ClockCycles(&clock) - boundary) < 0) {}

    BK4819_EnterTxMute();
    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_ExitTxMute();
    MDC1200_AppNoteOwnTx();
    return true;
}

bool MDC1200_SendRepeated(uint8_t op, uint8_t arg, uint16_t unitId,
                          uint8_t preambleCycles, uint8_t toneMode,
                          uint8_t repeatCount)
{
    if (repeatCount < 1u)
        repeatCount = 1u;
    if (repeatCount > 5u)
        repeatCount = 5u;

    bool sent = false;
    for (uint8_t repeat = 0; repeat < repeatCount; ++repeat)
        sent |= MDC1200_Send(op, arg, unitId, preambleCycles, toneMode);
    return sent;
}

void MDC1200_Init(void)
{
    gMdc1200LastOp = 0;
    gMdc1200LastArg = 0;
    gMdc1200LastUnitId = 0;
    gMdc1200RxDisplayTicks = 0;
    MDC1200_ResetRx();
}

