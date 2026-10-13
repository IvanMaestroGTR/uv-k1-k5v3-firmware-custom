/* Talk Permit Tone sequences ported from TrueMDC.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tpt.h"

#include "audio.h"
#include "driver/bk4819.h"
#include "driver/system.h"
#include "settings.h"

typedef struct {
    uint16_t frequency;
    uint16_t durationMs;
} TPT_Step;

static const TPT_Step xts[] = {
    {910, 100}, {0, 20}, {910, 30}, {0, 20}, {910, 60}
};
static const TPT_Step trbo[] = {
    {1570, 100}, {1050, 40}, {1570, 40}, {1317, 40}
};
static const TPT_Step kenwood[] = {
    {1215, 100}, {1629, 30}, {1215, 30}, {1629, 30}
};

void TPT_Play(uint8_t mode)
{
    const TPT_Step *steps = 0;
    unsigned int count = 0;

    if (mode == TPT_MODE_AUTO) {
        mode = gEeprom.MDC1200_TX_MODE == MDC1200_TX_OFF ? TPT_MODE_XTS : TPT_MODE_TRBO;
    }

    switch (mode) {
    case TPT_MODE_XTS:
        steps = xts;
        count = sizeof(xts) / sizeof(xts[0]);
        break;
    case TPT_MODE_TRBO:
        steps = trbo;
        count = sizeof(trbo) / sizeof(trbo[0]);
        break;
    case TPT_MODE_KENWOOD:
        steps = kenwood;
        count = sizeof(kenwood) / sizeof(kenwood[0]);
        break;
    default:
        return;
    }

    BK4819_EnterTxMute();
    AUDIO_AudioPathOn();
    BK4819_SetAF(BK4819_AF_BEEP);
    BK4819_EnableTXLink();

    for (unsigned int i = 0; i < count; ++i) {
        if (steps[i].frequency == 0) {
            BK4819_WriteRegister(BK4819_REG_70, 0);
        } else {
            const uint16_t controlWord = (uint16_t)
                ((((uint32_t)steps[i].frequency * 1353245u) + (1u << 16)) >> 17);
            BK4819_WriteRegister(BK4819_REG_71, controlWord);
            BK4819_WriteRegister(BK4819_REG_70,
                BK4819_REG_70_ENABLE_TONE1 |
                (55u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
            BK4819_ExitTxMute();
        }
        SYSTEM_DelayMs(steps[i].durationMs);
        BK4819_EnterTxMute();
    }

    BK4819_WriteRegister(BK4819_REG_70, 0);
    AUDIO_AudioPathOff();
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);
    BK4819_ExitTxMute();
}

