/**
 * @file tether.c
 * @brief Synchronous MCP4822 DAC and A4988 stepper demo implementation.
 *
 * Depends on HAL_Clock_Init() (8 MHz, see HAL_clock.h) and HAL_SPI_Init()
 * (eUSCI_B1, ~500 kHz, SPI mode 0, MSB first). Do NOT reconfigure B1 here.
 *
 * Important: P4.4 is CC1101 CS; P3.7 is DAC CS. The two CS lines must
 * never be asserted together. This module does not access radio registers.
 */

#include "tether.h"

#include <stdint.h>

#include <driverlib.h>
#include <msp430.h>

#include "HAL_SPI.h"
#include "HAL_clock.h"

/* Pin assignments: keep these consistent with the proven tether wiring. */
#define TETHER_MOTOR_PORT          GPIO_PORT_P3
#define TETHER_MS_PIN              GPIO_PIN3   /* P3.3 -> MS1/2/3 */
#define TETHER_STEP_PIN            GPIO_PIN4   /* P3.4 -> STEP */
#define TETHER_DIR_PIN             GPIO_PIN5   /* P3.5 -> DIR */
#define TETHER_ENABLE_PIN          GPIO_PIN6   /* P3.6 -> ENABLE, active low  DONE */
#define TETHER_DAC_CS_PIN          GPIO_PIN7   /* P3.7 -> MCP4822 CS */
#define TETHER_SLEEP_PORT          GPIO_PORT_P7
#define TETHER_SLEEP_PIN           GPIO_PIN3   /* P7.3 -> SLEEP, active low */

/* Delays in CPU cycles, derived from the MCLK frequency in HAL_clock.h so
 * the timing stays the same if the clock changes. __delay_cycles() needs a
 * compile-time constant, which these are. */
#define TETHER_US_TO_CYCLES(us)    (HAL_CLOCK_CYCLES_PER_US * (unsigned long)(us))
#define TETHER_DAC_STARTUP_CYCLES  TETHER_US_TO_CYCLES(100000UL) /* 100 ms */
#define TETHER_WAKE_CYCLES         TETHER_US_TO_CYCLES(2000UL)   /* 2 ms */
#define TETHER_STEP_HIGH_CYCLES    TETHER_US_TO_CYCLES(10UL)     /* 10 us */
#define TETHER_STEP_PERIOD_CYCLES  TETHER_US_TO_CYCLES(10000UL)  /* 10 ms + pulse/code time */
#define TETHER_DAC_TICK_CYCLES     TETHER_US_TO_CYCLES(1000UL)   /* 1 ms + SPI/code time */
#define TETHER_PAUSE_TICKS         250U       /* ~250 ms + SPI/code time */

#define TETHER_DAC_MAX_MV          4095U
#define TETHER_SWEEP_MAX_MV        4000U
#define TETHER_SWEEP_STEP_MV       4U
#define TETHER_DAC_B_HOLD_MV       2000U
#define TETHER_STEPS_PER_REV       200U
#define TETHER_TICKS_PER_STEP      10U


/* Swap the following values if the actual motor wiring reverses meaning. */
#define TETHER_DIR_UNCOIL_HIGH     1U
#define TETHER_DIR_COIL_HIGH       0U

static uint8_t tether_initialised;
static uint8_t motor_awake;

static void dac_write(uint8_t channel_b, uint16_t millivolts)
{
    uint16_t command;

    if (millivolts > TETHER_DAC_MAX_MV) {
        millivolts = TETHER_DAC_MAX_MV;
    }

    /* MCP4822: bit 15 channel; GA=0 (2x); SHDN=1 (active). */
    command = (uint16_t)(0x1000U | millivolts);
    if (channel_b != 0U) {
        command |= 0x8000U;
    }

    /* Ensure CC1101 is deselected (its CS is on P4.4). */
    HAL_SPI_CS_Deassert();

    /* DAC uses a different CS. Reuse the HAL's raw 8-bit SPI transfer. */
    GPIO_setOutputLowOnPin(TETHER_MOTOR_PORT, TETHER_DAC_CS_PIN);
    (void)SPI_TransferByte((uint8_t)(command >> 8));
    (void)SPI_TransferByte((uint8_t)(command & 0xFFU));

    /* Don't raise CS until the second byte has actually left the wire. */
    while (EUSCI_B_SPI_isBusy(SPI_EUSCI_BASE)) {
    }
    GPIO_setOutputHighOnPin(TETHER_MOTOR_PORT, TETHER_DAC_CS_PIN);
}

static void motor_prepare(void)
{
    if (motor_awake == 0U) {
        GPIO_setOutputHighOnPin(TETHER_SLEEP_PORT, TETHER_SLEEP_PIN);
        __delay_cycles(TETHER_WAKE_CYCLES);
        motor_awake = 1U;
    }
    GPIO_setOutputLowOnPin(TETHER_MOTOR_PORT, TETHER_ENABLE_PIN);
}

static void motor_set_direction(uint8_t direction_high)
{
    if (direction_high != 0U) {
        GPIO_setOutputHighOnPin(TETHER_MOTOR_PORT, TETHER_DIR_PIN);
    } else {
        GPIO_setOutputLowOnPin(TETHER_MOTOR_PORT, TETHER_DIR_PIN);
    }
}

static void motor_pulse(void)
{
    GPIO_setOutputHighOnPin(TETHER_MOTOR_PORT, TETHER_STEP_PIN);
    __delay_cycles(TETHER_STEP_HIGH_CYCLES);
    GPIO_setOutputLowOnPin(TETHER_MOTOR_PORT, TETHER_STEP_PIN);
}

/* Blocking full-step movement WITHOUT DAC updates. */
static void motor_move(uint8_t direction_high, uint16_t steps)
{
    uint16_t i;

    if (tether_initialised == 0U || steps == 0U) {
        return;
    }

    motor_prepare();
    motor_set_direction(direction_high);

    /* Provide ample DIR setup time before the first rising STEP edge. */
    __delay_cycles(TETHER_WAKE_CYCLES);

    for (i = 0U; i < steps; ++i) {
        motor_pulse();
        __delay_cycles(TETHER_STEP_PERIOD_CYCLES);
    }
}

/* One blocking DAC update + nominal 1 ms pause. */
static void demo_tick(uint16_t *voltage_mv)
{
    dac_write(0U, *voltage_mv);
    *voltage_mv = (uint16_t)(*voltage_mv + TETHER_SWEEP_STEP_MV);
    if (*voltage_mv > TETHER_SWEEP_MAX_MV) {
        *voltage_mv = 0U;
    }
    __delay_cycles(TETHER_DAC_TICK_CYCLES);
}

/* Blocking motor movement, interleaving ten DAC updates per motor step. */
static void demo_move(uint8_t direction_high,
                      uint16_t steps,
                      uint16_t *voltage_mv)
{
    uint16_t step;
    uint16_t tick;

    motor_prepare();
    motor_set_direction(direction_high);

    for (step = 0U; step < steps; ++step) {
        for (tick = 0U; tick < TETHER_TICKS_PER_STEP; ++tick) {
            demo_tick(voltage_mv);
        }
        motor_pulse();
    }
}

static void demo_pause(uint16_t *voltage_mv)
{
    uint16_t tick;
    for (tick = 0U; tick < TETHER_PAUSE_TICKS; ++tick) {
        demo_tick(voltage_mv);
    }
}

void Tether_Init(void)
{
    /* Set safe output levels BEFORE enabling GPIO output drivers. */
    GPIO_setOutputHighOnPin(TETHER_MOTOR_PORT,
                            TETHER_ENABLE_PIN | TETHER_DAC_CS_PIN);
    GPIO_setOutputLowOnPin(TETHER_MOTOR_PORT,
                           TETHER_MS_PIN | TETHER_STEP_PIN | TETHER_DIR_PIN);
    GPIO_setAsOutputPin(TETHER_MOTOR_PORT,
                        TETHER_MS_PIN | TETHER_STEP_PIN | TETHER_DIR_PIN |
                        TETHER_ENABLE_PIN | TETHER_DAC_CS_PIN);

    GPIO_setOutputLowOnPin(TETHER_SLEEP_PORT, TETHER_SLEEP_PIN);
    GPIO_setAsOutputPin(TETHER_SLEEP_PORT, TETHER_SLEEP_PIN);

    motor_awake = 0U;
    tether_initialised = 0U;

    /* Preserve the proven post-initialisation DAC start-up delay. */
    __delay_cycles(TETHER_DAC_STARTUP_CYCLES);

    dac_write(0U, 0U);
    dac_write(1U, TETHER_DAC_B_HOLD_MV);
    tether_initialised = 1U;
}

void Tether_SetVoltageA_mV(uint16_t millivolts)
{
    if (tether_initialised != 0U) {
        dac_write(0U, millivolts);
    }
}

void Tether_SetVoltageB_mV(uint16_t millivolts)
{
    if (tether_initialised != 0U) {
        dac_write(1U, millivolts);
    }
}

void Tether_SweepVoltageA(void)
{
    uint16_t millivolts;

    if (tether_initialised == 0U) {
        return;
    }

    for (millivolts = 0U;
         millivolts <= TETHER_SWEEP_MAX_MV;
         millivolts = (uint16_t)(millivolts + TETHER_SWEEP_STEP_MV)) {
        dac_write(0U, millivolts);
        __delay_cycles(TETHER_DAC_TICK_CYCLES);
    }
    dac_write(0U, 0U);
}

void Tether_Uncoil(uint16_t steps)
{
    motor_move(TETHER_DIR_UNCOIL_HIGH, steps);
}

void Tether_Coil(uint16_t steps)
{
    motor_move(TETHER_DIR_COIL_HIGH, steps);
}

void Tether_DemoCycle(void)
{
    uint16_t voltage_mv;

    if (tether_initialised == 0U) {
        return;
    }

    voltage_mv = 0U;
    dac_write(1U, TETHER_DAC_B_HOLD_MV);

    /* One nominal full revolution in each direction, DAC A sweeping. */
    demo_move(TETHER_DIR_UNCOIL_HIGH, TETHER_STEPS_PER_REV, &voltage_mv);
    demo_pause(&voltage_mv);
    demo_move(TETHER_DIR_COIL_HIGH, TETHER_STEPS_PER_REV, &voltage_mv);
    demo_pause(&voltage_mv);

    dac_write(0U, 0U);
}

void Tether_DisableMotor(void)
{
    GPIO_setOutputHighOnPin(TETHER_MOTOR_PORT, TETHER_ENABLE_PIN);
    GPIO_setOutputLowOnPin(TETHER_SLEEP_PORT, TETHER_SLEEP_PIN);
    motor_awake = 0U;
}
