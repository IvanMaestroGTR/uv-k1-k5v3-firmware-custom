#ifndef APP_MDC1200_H
#define APP_MDC1200_H

#include <stdbool.h>
#include <stdint.h>

#define MDC1200_FEC_K 7u

enum {
    MDC1200_OP_PTT_ID = 0x01,
    MDC1200_ARG_PRE_ID = 0x80,
    MDC1200_ARG_POST_ID = 0x00,
};

extern uint8_t  gMdc1200LastOp;
extern uint8_t  gMdc1200LastArg;
extern uint16_t gMdc1200LastUnitId;
extern uint8_t  gMdc1200RxDisplayTicks;

void MDC1200_Init(void);
void MDC1200_FormatUnitId(char output[5], uint16_t unitId);
unsigned int MDC1200_EncodeSinglePacket(void *data, uint8_t op, uint8_t arg, uint16_t unitId);
bool MDC1200_ProcessRxData(const void *buffer, unsigned int size,
                          uint8_t *op, uint8_t *arg, uint16_t *unitId);

/* Selectable BK4829 hardware FFSK (classic) or tone-generator (rail) TX. */
bool MDC1200_Send(uint8_t op, uint8_t arg, uint16_t unitId,
                  uint8_t preambleCycles, uint8_t toneMode);

/* Software 1200/1800 Hz FFSK receiver.  It samples discriminator audio on
 * PA4 with ADC1/TIM3 and therefore does not use the BK4829 packet modem. */
void MDC1200_AppTick10ms(void);
void MDC1200_AppTick500ms(void);
void MDC1200_AppDisableRx(void);
void MDC1200_AppNoteOwnTx(void);
bool MDC1200_AppRxSampling(void);

#endif
