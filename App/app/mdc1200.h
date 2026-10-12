#ifndef APP_MDC1200_H
#define APP_MDC1200_H

#include <stdbool.h>
#include <stdint.h>

#define MDC1200_FEC_K 7u
#define MDC1200_MAX_PACKET_SIZE 32u

/* Encodes one MDC1200 codeword. Output includes the software preamble and sync. */
unsigned int MDC1200_encode_single_packet(void *data, uint8_t op, uint8_t arg, uint16_t unit_id);

/* Incremental decoder: pass each newly received raw FFSK byte chunk. */
bool MDC1200_process_rx_data(const void *buffer, unsigned int size,
                             uint8_t *op, uint8_t *arg, uint16_t *unit_id);
void MDC1200_reset_rx(void);

#endif
