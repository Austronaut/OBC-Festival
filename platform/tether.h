/**
 * @file tether.h
 * @brief Blocking tether demo API for MSP430FR5994 + MCP4822 + A4988.
 *
 * Call HAL_Clock_Init(), then HAL_SPI_Init(), then Tether_Init().
 * The tether and CC1101 share eUSCI_B1 SPI (P5.0/P5.2), but use
 * independent chip selects (DAC P3.7; CC1101 P4.4).
 *
 * Every operation is synchronous: callers must wait until it returns.
 * No interrupts, timer peripherals, dynamic allocation, or background tasks
 * are used by this module.
 */

#ifndef TETHER_H
#define TETHER_H

#include <stdint.h>

/**
 * @brief Initialise tether GPIO and set DAC A = 0 mV, DAC B = 2000 mV.
 * @pre HAL_Clock_Init() and HAL_SPI_Init() have completed; DAC and motor
 *      supplies are safely connected. HAL_SPI_Init() unlocks LOCKLPM5.
 * @post A4988 is initially disabled and asleep; DAC outputs are initialised.
 * @par Blocking
 *      Yes. Includes a ~100 ms DAC power-up settling delay at 1 MHz.
 * @par ISR-safe
 *      No.
 */
void Tether_Init(void);

/**
 * @brief Set MCP4822 channel A to an approximate voltage in millivolts.
 * @param[in] millivolts Value from 0 to 4095; larger values are clamped.
 * @pre Tether_Init() has completed; no other SPI transaction is active.
 * @par Blocking
 *      Yes. One 16-bit SPI transaction.
 * @par ISR-safe
 *      No.
 */
void Tether_SetVoltageA_mV(uint16_t millivolts);

/**
 * @brief Set MCP4822 channel B to an approximate voltage in millivolts.
 * @param[in] millivolts Value from 0 to 4095; larger values are clamped.
 * @pre Tether_Init() has completed; no other SPI transaction is active.
 * @par Blocking
 *      Yes. One 16-bit SPI transaction.
 * @par ISR-safe
 *      No.
 */
void Tether_SetVoltageB_mV(uint16_t millivolts);

/**
 * @brief Sweep channel A from 0 to ~4 V and return it to 0 V.
 *
 * Channel B is not changed; Tether_Init() sets it to 2 V.
 * @pre Tether_Init() has completed; no other SPI transaction is active.
 * @par Blocking
 *      Yes. About one second at 1 MHz, plus SPI/software overhead.
 * @par ISR-safe
 *      No.
 */
void Tether_SweepVoltageA(void);

/**
 * @brief Rotate the stepper in the configured uncoil direction.
 * @param[in] steps Number of FULL steps, e.g. 200 for one nominal revolution.
 * @pre Tether_Init() has completed; motor supply and current limit are safe.
 * @post Motor remains enabled, awake and holding position.
 * @par Blocking
 *      Yes. Approximately 10 ms per step (~0.5 rev/s at 200 steps/rev).
 * @par ISR-safe
 *      No.
 */
void Tether_Uncoil(uint16_t steps);

/**
 * @brief Rotate the stepper in the opposite (coil) direction.
 * @param[in] steps Number of FULL steps.
 * @pre Tether_Init() has completed; motor supply and current limit are safe.
 * @post Motor remains enabled, awake and holding position.
 * @par Blocking
 *      Yes. Approximately 10 ms per step.
 * @par ISR-safe
 *      No.
 */
void Tether_Coil(uint16_t steps);

/**
 * @brief Run one combined demo: ~1 rev uncoil, pause, ~1 rev coil, pause.
 *
 * During movement AND pauses, DAC A sweeps 0-4 V repeatedly while
 * DAC B remains at 2 V. The motor is stepped ~once per 10 DAC updates.
 * @pre Tether_Init() has completed; motor and DAC hardware are connected.
 * @post Motor remains enabled/awake; DAC A returns to 0 V.
 * @par Blocking
 *      Yes. Roughly 4.5 seconds, plus SPI/software overhead.
 * @par ISR-safe
 *      No.
 */
void Tether_DemoCycle(void);

/**
 * @brief Disable stepper outputs and put the A4988 to sleep.
 *
 * This releases holding torque. It cannot interrupt an ongoing blocking
 * operation from the same single-threaded main loop.
 * @pre Tether_Init() has completed.
 * @par Blocking
 *      No.
 * @par ISR-safe
 *      No.
 */
void Tether_DisableMotor(void);

#endif /* TETHER_H */
