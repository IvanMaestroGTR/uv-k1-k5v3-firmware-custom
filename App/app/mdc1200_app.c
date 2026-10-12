/*
 * MDC1200 software FFSK receiver for PY32F071/BK4829 radios.
 *
 * The discriminator audio on PA4 is sampled at 14.4 kHz.  A fixed-point
 * band-pass, two sliding I/Q correlators and a small DPLL recover the raw
 * 1200-baud 1200/1800 Hz stream.  This deliberately avoids the undocumented
 * and BK4819-incompatible BK4829 packet modem.
 *
 * Demodulator design adapted from EthanYan6/Dondji (Apache-2.0).
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app/mdc1200.h"

#include <string.h>

#include "board.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "py32f0xx.h"

#ifdef ENABLE_FMRADIO_EMBEDDED
    #include "app/fm.h"
#endif

#define BP_B0       4087
#define BP_A1       (-19700)
#define BP_A2       8202
#define PLL_STEP    5461
#define PLL_CTR     45056
#define PLL_SHIFT   2
#define AGC_ATT     5
#define AGC_DEC     10
#define PK_MIN      16
#define CAP_SIZE    128u
#define CAP_MIN     19u

static const int8_t mdcCos[20] = {
    127, 110, 64, 0, -64, -110, -127, -110, -64, 0, 64, 110,
    127, 90, 0, -90, -127, -90, 0, 90,
};

typedef struct {
    int32_t dc;
    int32_t x1, x2, y1, y2;
    int32_t sum[4];
    int32_t markPeak, spacePeak;
    uint32_t ringIndex;
    int16_t ring[24][4];
    int32_t previousDecision, phase;
    uint8_t accumulator, bitCount;
} MDC1200_Demodulator;

static MDC1200_Demodulator demod;
static volatile bool sampling;
static volatile uint8_t sampleHead;
static volatile uint8_t sampleTail;
static uint8_t sampleRing[64];
static uint8_t capture[CAP_SIZE];
static uint8_t captureLength;
static bool ignoreOwnRx;
static uint8_t ignoreOwnTicks;

static int32_t MDC1200_Magnitude(int32_t i, int32_t q)
{
    if (i < 0) i = -i;
    if (q < 0) q = -q;
    if (i < q) {
        const int32_t t = i;
        i = q;
        q = t;
    }
    return i + (q >> 2) + (q >> 3);
}

static int32_t MDC1200_ToneLevel(int32_t i, int32_t q, int32_t *peak)
{
    const int32_t magnitude = MDC1200_Magnitude(i, q);
    int32_t p = *peak;
    p += magnitude > p ? (magnitude - p) >> AGC_ATT : -(p >> AGC_DEC);
    if (p < PK_MIN)
        p = PK_MIN;
    *peak = p;
    return magnitude;
}

void ADC_COMP_IRQHandler(void)
{
    const int32_t adc = (int32_t)(ADC1->DR & 0xFFFu);
    MDC1200_Demodulator *m = &demod;

    m->dc += ((adc << 4) - m->dc) >> 6;
    const int32_t x = adc - (m->dc >> 4);
    const int32_t y =
        (BP_B0 * (x - m->x2) - BP_A1 * m->y1 - BP_A2 * m->y2) >> 14;
    m->x2 = m->x1;
    m->x1 = x;
    m->y2 = m->y1;
    m->y1 = y;

    uint32_t p12 = m->ringIndex;
    if (p12 >= 12u)
        p12 -= 12u;
    uint32_t q12 = p12 + 9u;
    if (q12 >= 12u)
        q12 -= 12u;
    const uint32_t p8 = m->ringIndex & 7u;
    const uint8_t coefficient[4] = {
        (uint8_t)p12,
        (uint8_t)q12,
        (uint8_t)(12u + p8),
        (uint8_t)(12u + ((p8 + 6u) & 7u)),
    };
    int16_t *old = m->ring[m->ringIndex];
    for (uint32_t i = 0; i < 4u; ++i) {
        const int32_t product = (y * (int32_t)mdcCos[coefficient[i]]) >> 8;
        m->sum[i] += product - old[i];
        old[i] = (int16_t)product;
    }
    if (++m->ringIndex >= 24u)
        m->ringIndex = 0;

    const int32_t mark = MDC1200_ToneLevel(m->sum[0], m->sum[1], &m->markPeak);
    const int32_t space = MDC1200_ToneLevel(m->sum[2], m->sum[3], &m->spacePeak);
    const int32_t decision = mark * m->spacePeak - space * m->markPeak;

    m->phase += PLL_STEP;
    if ((decision > 0) != (m->previousDecision > 0))
        m->phase += (PLL_CTR - m->phase) >> PLL_SHIFT;
    m->previousDecision = decision;
    if (m->phase >= 65536) {
        m->phase -= 65536;
        m->accumulator =
            (uint8_t)((m->accumulator << 1) | (decision > 0 ? 1u : 0u));
        if (++m->bitCount >= 8u) {
            m->bitCount = 0;
            const uint8_t next = (uint8_t)((sampleHead + 1u) & 63u);
            if (next != sampleTail) {
                sampleRing[sampleHead] = m->accumulator;
                sampleHead = next;
            }
        }
    }
}

static void MDC1200_StartSampling(void)
{
    if (sampling)
        return;

    memset(&demod, 0, sizeof(demod));
    sampleHead = 0;
    sampleTail = 0;
    captureLength = 0;

    GPIOA->MODER = (GPIOA->MODER & ~(3u << 8)) | (3u << 8);
    RCC->APBENR1 |= RCC_APBENR1_DAC1EN | RCC_APBENR1_TIM3EN;
    DAC1->CR = (1u << 0) | (1u << 1) | (1u << 2) | (7u << 3);
    DAC1->DHR12R1 = 2048;
    DAC1->SWTRIGR = 1;

    TIM3->CR2 = TIM_CR2_MMS_1;
    TIM3->PSC = 0;
    TIM3->ARR = (SystemCoreClock / 14400u) - 1u;

    ADC1->CR2 = 0;
    ADC1->SMPR3 = 5u << 12;
    ADC1->SQR3 = 4u;
    ADC1->SR = 0;
    ADC1->CR1 = ADC_CR1_EOCIE;
    ADC1->CR2 = ADC_CR2_EXTSEL_2 | ADC_CR2_EXTTRIG | ADC_CR2_ADON;

    NVIC_SetPriority(ADC_COMP_IRQn, 2);
    NVIC_EnableIRQ(ADC_COMP_IRQn);
    TIM3->CR1 = TIM_CR1_CEN;
    sampling = true;
}

static void MDC1200_StopSampling(void)
{
    if (!sampling)
        return;
    sampling = false;
    NVIC_DisableIRQ(ADC_COMP_IRQn);
    ADC1->CR1 &= ~ADC_CR1_EOCIE;
    ADC1->CR2 &= ~ADC_CR2_ADON;
    TIM3->CR1 = 0;
    BOARD_ADC_Init();
    DAC1->CR = 0;
    RCC->APBENR1 &= ~(RCC_APBENR1_DAC1EN | RCC_APBENR1_TIM3EN);
    GPIOA->MODER &= ~(3u << 8);
}

static bool MDC1200_TryDecode(void)
{
    if (captureLength < CAP_MIN)
        return false;
    if (MDC1200_ProcessRxData(capture, captureLength,
                              &gMdc1200LastOp, &gMdc1200LastArg,
                              &gMdc1200LastUnitId))
        return true;

    capture[0] ^= 0x80u;
    const bool decoded = MDC1200_ProcessRxData(capture, captureLength,
                                                &gMdc1200LastOp,
                                                &gMdc1200LastArg,
                                                &gMdc1200LastUnitId);
    capture[0] ^= 0x80u;
    return decoded;
}

static void MDC1200_Accept(void)
{
    if (!MDC1200_TryDecode())
        return;
    if (ignoreOwnRx && gMdc1200LastUnitId == gEeprom.MDC1200_ID) {
        ignoreOwnRx = false;
        ignoreOwnTicks = 0;
        return;
    }
    gMdc1200RxDisplayTicks = 12;
    gUpdateDisplay = true;
}

static void MDC1200_Drain(void)
{
    while (sampleTail != sampleHead) {
        const uint8_t byte = sampleRing[sampleTail];
        sampleTail = (uint8_t)((sampleTail + 1u) & 63u);
        if (captureLength < CAP_SIZE)
            capture[captureLength++] = (uint8_t)(byte ^ 0xFFu);
    }
    if (captureLength >= CAP_SIZE) {
        MDC1200_Accept();
        memmove(capture, capture + 64, 64);
        captureLength = 64;
    } else if (captureLength >= CAP_MIN && (captureLength & 15u) == 0u) {
        MDC1200_Accept();
    }
}

void MDC1200_AppDisableRx(void)
{
    MDC1200_StopSampling();
    captureLength = 0;
}

void MDC1200_AppNoteOwnTx(void)
{
    MDC1200_AppDisableRx();
    ignoreOwnRx = true;
    ignoreOwnTicks = 1;
}

void MDC1200_AppTick10ms(void)
{
    bool enabled = gEeprom.MDC1200_RX_ENABLE;
#ifdef ENABLE_VOICE
    enabled = false;
#endif
#ifdef ENABLE_FMRADIO_EMBEDDED
    if (gFmRadioMode)
        enabled = false;
#endif
    if (gRxVfo == NULL || gRxVfo->Modulation != MODULATION_FM)
        enabled = false;
    if (!enabled || gCurrentFunction == FUNCTION_TRANSMIT) {
        MDC1200_AppDisableRx();
        return;
    }

    const bool gate = g_SquelchLost || gCurrentFunction == FUNCTION_MONITOR;
    if (gate) {
        if (!sampling)
            MDC1200_StartSampling();
        else
            MDC1200_Drain();
    } else if (sampling) {
        MDC1200_Drain();
        MDC1200_StopSampling();
        MDC1200_Accept();
        captureLength = 0;
    }
}

void MDC1200_AppTick500ms(void)
{
    if (gMdc1200RxDisplayTicks > 0 && --gMdc1200RxDisplayTicks == 0)
        gUpdateDisplay = true;
    if (ignoreOwnRx && ignoreOwnTicks > 0 && --ignoreOwnTicks == 0)
        ignoreOwnRx = false;
}

bool MDC1200_AppRxSampling(void)
{
    return sampling;
}
