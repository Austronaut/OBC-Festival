/**
 * @file   HAL_clock.h
 * @brief  System clock setup (CS module) and cycle-based delay helpers.
 *
 * Call HAL_Clock_Init() once, right after stopping the watchdog and before
 * any peripheral init. HAL_SPI_Init() and the tether/CC1101 code derive their
 * dividers and delays from the frequencies below, so changing the clock means
 * changing HAL_Clock_Init() AND these constants together.
 */

#ifndef HAL_CLOCK_H
#define HAL_CLOCK_H

/* MCLK (CPU) and SMCLK (peripherals) both run from the DCO, no division.
 * Supported values: 8 (normal) or 1 (DEBUG: the old 1 MHz setup, for A/B
 * testing the SPI corruption). Everything else derives from this.
 * 8 MHz is the highest MCLK the FR5994's FRAM supports without wait
 * states: going faster also needs FRCTL0.NWAITS set. */
#define HAL_CLOCK_MHZ          8

#define HAL_CLOCK_MCLK_HZ      (HAL_CLOCK_MHZ * 1000000UL)
#define HAL_CLOCK_SMCLK_HZ     (HAL_CLOCK_MHZ * 1000000UL)

/* CPU cycles per microsecond. The delay macros below only work for whole MHz. */
#define HAL_CLOCK_CYCLES_PER_US  (HAL_CLOCK_MCLK_HZ / 1000000UL)

#if (HAL_CLOCK_MCLK_HZ % 1000000UL) != 0
#error "HAL_CLOCK_MCLK_HZ must be a whole number of MHz for HAL_DELAY_US"
#endif

/* Busy-wait delays. __delay_cycles() is a TI compiler intrinsic that burns
 * exactly N MCLK cycles, so it needs no <msp430.h> include here. Its
 * argument must be a compile-time constant: pass literals or macros only,
 * never a variable. Interrupts still run during the delay, which would make
 * it longer. */
#define HAL_DELAY_US(us)  __delay_cycles(HAL_CLOCK_CYCLES_PER_US * (unsigned long)(us))
#define HAL_DELAY_MS(ms)  __delay_cycles(HAL_CLOCK_CYCLES_PER_US * 1000UL * (unsigned long)(ms))

/**
 * @brief  Sets MCLK and SMCLK to 8 MHz from the DCO, and ACLK to the VLO.
 *
 * @pre    Watchdog stopped. No peripheral has been configured yet.
 * @post   MCLK = SMCLK = HAL_CLOCK_MCLK_HZ (DCO, factory trimmed to about
 *         +/-3.5%). ACLK = VLO, about 10 kHz (nothing uses it yet).
 *
 * @par Blocking
 *      Yes. About 10 us while the DCO settles.
 * @par ISR-safe
 *      No.
 */
void HAL_Clock_Init(void);

#endif /* HAL_CLOCK_H */
