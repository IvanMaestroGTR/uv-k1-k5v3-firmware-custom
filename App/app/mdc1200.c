#include "driver/crc.h"
#include "mdc1200.h"
#include <string.h>

// Keep all original constants (TX-only)
static const uint8_t mdc1200_pre_amble[] = {0x00, 0xFF};
const uint8_t mdc1200_sync[5] = {0x07, 0x09, 0x2a, 0x44, 0x6f};

void xor_modulation(void *data, const unsigned int size) {
    unsigned int i;
    uint8_t *data8 = (uint8_t *) data;
    uint8_t prev_bit = 0;
    for (i = 0; i < size; i++) {
        int bit_num;
        uint8_t in = data8[i];
        uint8_t out = 0;
        for (bit_num = 7; bit_num >= 0; bit_num--) {
            const uint8_t new_bit = (in >> bit_num) & 1u;
            if (new_bit != prev_bit)
                out |= 1u << bit_num;
            prev_bit = new_bit;
        }
        data8[i] = out ^ 0xff;
    }
}

uint8_t *encode_data(void *data) {
    uint8_t *data8 = (uint8_t *) data;
    {
        unsigned int i;
        uint8_t shift_reg = 0;
        for (i = 0; i < MDC1200_FEC_K; i++) {
            unsigned int bit_num;
            const uint8_t bi = data8[i];
            uint8_t bo = 0;
            for (bit_num = 0; bit_num < 8; bit_num++) {
                shift_reg = (shift_reg << 1) | ((bi >> bit_num) & 1u);
                bo |= (((shift_reg >> 6) ^ (shift_reg >> 5) ^ (shift_reg >> 2) ^ (shift_reg >> 0)) & 1u) << bit_num;
            }
            data8[MDC1200_FEC_K + i] = bo;
        }
    }

    {
        unsigned int i, k;
        uint8_t interleaved[(MDC1200_FEC_K * 2) * 8];
        for (i = 0, k = 0; i < (MDC1200_FEC_K * 2); i++) {
            unsigned int bit_num;
            const uint8_t b = data8[i];
            for (bit_num = 0; bit_num < 8; bit_num++) {
                interleaved[k] = (b >> bit_num) & 1u;
                k += 16;
                if (k >= sizeof(interleaved))
                    k -= sizeof(interleaved) - 1;
            }
        }
        for (i = 0, k = 0; i < (MDC1200_FEC_K * 2); i++) {
            int bit_num;
            uint8_t b = 0;
            for (bit_num = 7; bit_num >= 0; bit_num--)
                if (interleaved[k++])
                    b |= 1u << bit_num;
            data8[i] = b;
        }
    }
    return data8 + (MDC1200_FEC_K * 2);
}

unsigned int MDC1200_encode_single_packet(void *data, const uint8_t op, const uint8_t arg, const uint16_t unit_id) {
    unsigned int size;
    uint16_t crc;
    uint8_t *p = (uint8_t *) data;

    memcpy(p, mdc1200_pre_amble, sizeof(mdc1200_pre_amble));
    p += sizeof(mdc1200_pre_amble);
    memcpy(p, mdc1200_sync, sizeof(mdc1200_sync));
    p += sizeof(mdc1200_sync);

    p[0] = op;
    p[1] = arg;
    p[2] = (unit_id >> 8) & 0x00ff;
    p[3] = (unit_id >> 0) & 0x00ff;
    crc = CRC_Calculate(p, 4);
    p[4] = (crc >> 0) & 0x00ff;
    p[5] = (crc >> 8) & 0x00ff;
    p[6] = 0;

    p = encode_data(p);
    size = (unsigned int) (p - (uint8_t *) data);
    xor_modulation(data, size);
    return size;
}
