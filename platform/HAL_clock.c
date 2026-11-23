#include "HAL_clock.h"

#include <driverlib.h>

void HAL_Clock_Init(void)
{
    /* DCO = 8 MHz. DriverLib unlocks the CS registers, temporarily divides
     * MCLK/SMCLK by 4 while the DCO changes (the DCO can briefly overshoot,
     * TI erratum CS12), waits for it to settle, then restores the dividers. */
#if HAL_CLOCK_MHZ == 8
    CS_setDCOFreq(CS_DCORSEL_0, CS_DCOFSEL_6);   /* 8 MHz */
#elif HAL_CLOCK_MHZ == 1
    CS_setDCOFreq(CS_DCORSEL_0, CS_DCOFSEL_0);   /* 1 MHz */
#else
#error "HAL_CLOCK_MHZ must be 1 or 8"
#endif

    /* Route MCLK and SMCLK from the DCO with no division. ACLK from the
     * VLO (about 10 kHz). */
    CS_initClockSignal(CS_MCLK,  CS_DCOCLK_SELECT, CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_SMCLK, CS_DCOCLK_SELECT, CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_ACLK,  CS_VLOCLK_SELECT, CS_CLOCK_DIVIDER_1);
}
