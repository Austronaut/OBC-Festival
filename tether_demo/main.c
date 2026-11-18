/**
 * @file    main.c
 * @brief   MSP430FR5994 tether subsystem bench demonstration.
 *
 * Demo-only application. Direct register access here should eventually move
 * behind the project's SPI, GPIO and timer HAL interfaces.
 *
 * Requires hal_clock.c / hal_clock.h configured for 8 MHz MCLK and SMCLK.
 */

#include <stdbool.h>
#include <stdint.h>

#include <msp430.h>

#include "hal/hal_clock.h"

/* 0 = proven autonomous demo; 1 = two-button command-entry demonstration. */
#define DEMO_USE_BUTTONS          0

/* Timer_A0: 1 ms scheduling tick with 8 MHz SMCLK. */
#define DEMO_TICK_HZ              1000UL
#define TIMER_A0_CCR0            ((HAL_CLOCK_SMCLK_HZ / DEMO_TICK_HZ) - 1UL)

/* SPI remains at the approximately 500 kHz rate of the original bench test. */
#define DAC_SPI_HZ                500000UL
#define DAC_SPI_DIVIDER          (HAL_CLOCK_SMCLK_HZ / DAC_SPI_HZ)

#define DAC_A_MAX_MV              4000U
#define DAC_A_STEP_MV             4U
#define DAC_B_FIXED_MV            2000U
#define DAC_B_REFRESH_MS          100U

/* 200 full steps/rev; one rising edge per 10 ms = 0.5 rev/s while moving. */
#define MOTOR_STEPS_PER_REV        200U
#define MOTOR_STEP_PERIOD_MS       10U
#define MOTOR_WAKE_MS              2U
#define MOTOR_REVERSAL_PAUSE_MS  250U

#define PIN_MOTOR_MS              BIT3    /* P3.3: MS1/2/3 tied together */
#define PIN_MOTOR_STEP            BIT4    /* P3.4 */
#define PIN_MOTOR_DIR             BIT5    /* P3.5 */
#define PIN_MOTOR_ENABLE          BIT6    /* P3.6; active low */
#define PIN_DAC_CS                BIT7    /* P3.7; active low */
#define PIN_MOTOR_SLEEP           BIT3    /* P7.3; active low */
#define PIN_DAC_MOSI              BIT0    /* P5.0 / UCB1SIMO */
#define PIN_DAC_SCK               BIT2    /* P5.2 / UCB1CLK */

#if DEMO_USE_BUTTONS
#define PIN_BUTTON_START_END      BIT6    /* LaunchPad S1 / P5.6 */
#define PIN_BUTTON_COUNT          BIT5    /* LaunchPad S2 / P5.5 */
#define PIN_LED_RED               BIT0    /* P1.0 / LED1 */
#define PIN_LED_GREEN             BIT1    /* P1.1 / LED2 */
#define BUTTON_DEBOUNCE_MS        25U
#define BUTTON_TIMEOUT_MS         10000U
#define BUTTON_MAX_COMMAND        6U
#define FEEDBACK_MS               350U
#endif

#if (HAL_CLOCK_SMCLK_HZ % DEMO_TICK_HZ) != 0
#error "SMCLK must be divisible by the timer tick rate"
#endif
#if (HAL_CLOCK_SMCLK_HZ % DAC_SPI_HZ) != 0
#error "SMCLK must be divisible by the chosen SPI frequency"
#endif
#if TIMER_A0_CCR0 > 65535UL
#error "Timer_A0 period exceeds the 16-bit counter"
#endif

/* Shared with Timer_A0 ISR. 16-bit reads/writes are atomic on MSP430. */
static volatile uint16_t s_time_ms;
static volatile bool s_motor_running;
static volatile bool s_sweep_running;
static volatile bool s_step_high;
static volatile uint8_t s_motor_wake_left_ms;
static volatile uint16_t s_motor_period_ms;
static volatile uint16_t s_motor_pause_left_ms;
static volatile uint16_t s_motor_steps_in_rev;
static volatile bool s_motor_forward;
static volatile uint16_t s_dac_a_mv;
static volatile bool s_dac_a_update_pending;

static void gpio_init(void)
{
    /* Set safe inactive output latches BEFORE selecting output direction. */
    P3SEL0 &= ~(PIN_MOTOR_MS | PIN_MOTOR_STEP | PIN_MOTOR_DIR |
                PIN_MOTOR_ENABLE | PIN_DAC_CS);
    P3SEL1 &= ~(PIN_MOTOR_MS | PIN_MOTOR_STEP | PIN_MOTOR_DIR |
                PIN_MOTOR_ENABLE | PIN_DAC_CS);
    P3OUT |= PIN_MOTOR_ENABLE | PIN_DAC_CS | PIN_MOTOR_DIR;
    P3OUT &= ~(PIN_MOTOR_MS | PIN_MOTOR_STEP);
    P3DIR |= PIN_MOTOR_MS | PIN_MOTOR_STEP | PIN_MOTOR_DIR |
             PIN_MOTOR_ENABLE | PIN_DAC_CS;

    P7SEL0 &= ~PIN_MOTOR_SLEEP;
    P7SEL1 &= ~PIN_MOTOR_SLEEP;
    P7OUT &= ~PIN_MOTOR_SLEEP;
    P7DIR |= PIN_MOTOR_SLEEP;

#if DEMO_USE_BUTTONS
    /* Onboard S1 and S2 short the GPIO to GND when pressed. */
    P5SEL0 &= ~(PIN_BUTTON_START_END | PIN_BUTTON_COUNT);
    P5SEL1 &= ~(PIN_BUTTON_START_END | PIN_BUTTON_COUNT);
    P5DIR &= ~(PIN_BUTTON_START_END | PIN_BUTTON_COUNT);
    P5REN |= PIN_BUTTON_START_END | PIN_BUTTON_COUNT;
    P5OUT |= PIN_BUTTON_START_END | PIN_BUTTON_COUNT; /* pull-ups */

    P1SEL0 &= ~(PIN_LED_RED | PIN_LED_GREEN);
    P1SEL1 &= ~(PIN_LED_RED | PIN_LED_GREEN);
    P1OUT &= ~(PIN_LED_RED | PIN_LED_GREEN);
    P1DIR |= PIN_LED_RED | PIN_LED_GREEN;
#endif
}

static void spi_init(void)
{
    UCB1CTLW0 = UCSWRST;
    UCB1CTLW0 |= UCMST | UCSYNC | UCMSB | UCCKPH | UCSSEL__SMCLK;
    UCB1BRW = (uint16_t)DAC_SPI_DIVIDER;

    P5SEL1 &= ~(PIN_DAC_MOSI | PIN_DAC_SCK);
    P5SEL0 |= PIN_DAC_MOSI | PIN_DAC_SCK;

    UCB1CTLW0 &= ~UCSWRST;
}

/* MCP4822: 16-bit word, bit 15 selects channel, bit 12 enables output. */
static void dac_write(bool channel_b, uint16_t millivolts)
{
    uint16_t command;

    if (millivolts > 4095U) {
        millivolts = 4095U;
    }
    command = (uint16_t)(0x1000U | millivolts);
    if (channel_b) {
        command |= 0x8000U;
    }

    P3OUT &= ~PIN_DAC_CS;

    while ((UCB1IFG & UCTXIFG) == 0U) {
    }
    UCB1TXBUF = (uint8_t)(command >> 8);

    while ((UCB1IFG & UCTXIFG) == 0U) {
    }
    UCB1TXBUF = (uint8_t)(command & 0xFFU);

    /* Wait for the final clock edge before deasserting CS. */
    while ((UCB1STATW & UCBUSY) != 0U) {
    }
    P3OUT |= PIN_DAC_CS;
}

static void timer_init(void)
{
    TA0CTL = MC__STOP;
    TA0CCR0 = (uint16_t)TIMER_A0_CCR0;
    TA0CCTL0 = CCIE;
    TA0CTL = TASSEL__SMCLK | MC__UP | TACLR;
}

static void set_sweep_running(bool run)
{
    __disable_interrupt();
    s_sweep_running = run;
    s_dac_a_mv = 0U;
    s_dac_a_update_pending = true; /* Set A to 0 V when stopped. */
    __enable_interrupt();
}

static void set_motor_running(bool run)
{
    __disable_interrupt();

    s_motor_running = false;
    P3OUT &= ~PIN_MOTOR_STEP;
    s_step_high = false;
    s_motor_steps_in_rev = 0U;
    s_motor_period_ms = 0U;
    s_motor_pause_left_ms = 0U;
    s_motor_forward = true;
    P3OUT |= PIN_MOTOR_DIR;

    if (run) {
        P7OUT |= PIN_MOTOR_SLEEP;
        P3OUT &= ~PIN_MOTOR_ENABLE;
        s_motor_wake_left_ms = MOTOR_WAKE_MS;
        s_motor_running = true;
    } else {
        P3OUT |= PIN_MOTOR_ENABLE;
        P7OUT &= ~PIN_MOTOR_SLEEP;
        s_motor_wake_left_ms = 0U;
    }

    __enable_interrupt();
}

/* Timer tick drives step edges; SPI is deliberately never called from ISR. */
#pragma vector=TIMER0_A0_VECTOR
__interrupt void Timer_A0_ISR(void)
{
    ++s_time_ms;

    if (s_sweep_running) {
        if (s_dac_a_mv >= DAC_A_MAX_MV) {
            s_dac_a_mv = 0U;
        } else {
            s_dac_a_mv += DAC_A_STEP_MV;
        }
        s_dac_a_update_pending = true;
    }

    if (!s_motor_running) {
        return;
    }

    /* Exactly one timer tick HIGH, then LOW until the next rising edge. */
    if (s_step_high) {
        P3OUT &= ~PIN_MOTOR_STEP;
        s_step_high = false;
    }

    if (s_motor_wake_left_ms != 0U) {
        --s_motor_wake_left_ms;
        return;
    }

    if (s_motor_pause_left_ms != 0U) {
        --s_motor_pause_left_ms;
        if (s_motor_pause_left_ms == 0U) {
            s_motor_forward = !s_motor_forward;
            if (s_motor_forward) {
                P3OUT |= PIN_MOTOR_DIR;
            } else {
                P3OUT &= ~PIN_MOTOR_DIR;
            }
        }
        return;
    }

    ++s_motor_period_ms;
    if (s_motor_period_ms < MOTOR_STEP_PERIOD_MS) {
        return;
    }
    s_motor_period_ms = 0U;

    P3OUT |= PIN_MOTOR_STEP;
    s_step_high = true;
    ++s_motor_steps_in_rev;

    if (s_motor_steps_in_rev == MOTOR_STEPS_PER_REV) {
        s_motor_steps_in_rev = 0U;
        s_motor_pause_left_ms = MOTOR_REVERSAL_PAUSE_MS;
    }
}

#if DEMO_USE_BUTTONS
/* Small, main-loop-only input parser. No GPIO button interrupts required. */
typedef struct {
    bool sample_pressed;
    bool stable_pressed;
    uint8_t stable_ms;
} button_t;

static button_t s_start_button;
static button_t s_count_button;
static bool s_receiving;
static uint8_t s_command_count;
static uint16_t s_last_activity_ms;
static uint16_t s_feedback_started_ms;
static bool s_feedback_active;
static bool s_feedback_ok;

/* Debugger-readable: future telemetry publisher can consume these. */
static volatile uint8_t s_last_command_id;
static volatile bool s_last_command_accepted;

static bool button_pressed_event(button_t *button, bool raw_pressed)
{
    if (raw_pressed != button->sample_pressed) {
        button->sample_pressed = raw_pressed;
        button->stable_ms = 1U;
    } else if (button->stable_ms < BUTTON_DEBOUNCE_MS) {
        ++button->stable_ms;
    }

    if ((button->stable_ms >= BUTTON_DEBOUNCE_MS) &&
        (button->stable_pressed != raw_pressed)) {
        button->stable_pressed = raw_pressed;
        return raw_pressed; /* report only transitions into pressed */
    }
    return false;
}

static void give_feedback(uint16_t now_ms, bool accepted)
{
    s_feedback_active = true;
    s_feedback_started_ms = now_ms;
    s_feedback_ok = accepted;
}

static bool dispatch_command(uint8_t command)
{
    /* 1 start sweep, 2 start motor, 3 start both,
     * 4 stop motor, 5 stop sweep, 6 stop both. */
    switch (command) {
    case 1U: set_sweep_running(true);   break;
    case 2U: set_motor_running(true);   break;
    case 3U: set_sweep_running(true);
             set_motor_running(true);   break;
    case 4U: set_motor_running(false);  break;
    case 5U: set_sweep_running(false);  break;
    case 6U: set_sweep_running(false);
             set_motor_running(false);  break;
    default: return false;
    }
    return true;
}

static void buttons_service(uint16_t now_ms)
{
    bool start_event;
    bool count_event;
    bool accepted;

    start_event = button_pressed_event(
        &s_start_button, (P5IN & PIN_BUTTON_START_END) == 0U);
    count_event = button_pressed_event(
        &s_count_button, (P5IN & PIN_BUTTON_COUNT) == 0U);

    if (s_receiving &&
        ((uint16_t)(now_ms - s_last_activity_ms) >= BUTTON_TIMEOUT_MS)) {
        s_receiving = false;
        give_feedback(now_ms, false);
    }

    /* Do not accept simultaneous events as an ambiguous command. */
    if (start_event && count_event) {
        s_receiving = false;
        give_feedback(now_ms, false);
    } else if (start_event) {
        if (!s_receiving) {
            s_receiving = true;
            s_command_count = 0U;
            s_last_activity_ms = now_ms;
        } else {
            s_receiving = false;
            accepted = (s_command_count != 0U) &&
                       dispatch_command(s_command_count);
            s_last_command_id = s_command_count;
            s_last_command_accepted = accepted;
            give_feedback(now_ms, accepted);
        }
    } else if (count_event && s_receiving) {
        s_last_activity_ms = now_ms;
        if (s_command_count < BUTTON_MAX_COMMAND) {
            ++s_command_count;
        } else {
            s_receiving = false;
            give_feedback(now_ms, false);
        }
    }

    P1OUT &= ~(PIN_LED_RED | PIN_LED_GREEN);
    if (s_receiving) {
        if (((now_ms / 250U) & 1U) == 0U) {
            P1OUT |= PIN_LED_RED;
        }
    } else if (s_feedback_active) {
        if ((uint16_t)(now_ms - s_feedback_started_ms) < FEEDBACK_MS) {
            P1OUT |= s_feedback_ok ? PIN_LED_GREEN : PIN_LED_RED;
        } else {
            s_feedback_active = false;
        }
    }
}
#endif /* DEMO_USE_BUTTONS */

int main(void)
{
    uint16_t last_ms;
    uint16_t last_b_refresh_ms;
    uint16_t now_ms;
    uint16_t dac_value_mv;
    bool update_dac;

    WDTCTL = WDTPW | WDTHOLD;
    hal_clock_init();
    gpio_init();
    spi_init();
    PM5CTL0 &= ~LOCKLPM5;

    /* Proven DAC start-up workaround; it does not time the motor or ramp. */
    __delay_cycles(800000UL); /* 100 ms at 8 MHz */
    dac_write(false, 0U);
    dac_write(true, DAC_B_FIXED_MV);

    timer_init();
    __enable_interrupt();

#if !DEMO_USE_BUTTONS
    /* Preserve the original demo: both actuators start automatically. */
    set_sweep_running(true);
    set_motor_running(true);
#endif

    last_ms = s_time_ms;
    last_b_refresh_ms = last_ms;

    for (;;) {
        now_ms = s_time_ms;

#if DEMO_USE_BUTTONS
        if (now_ms != last_ms) {
            buttons_service(now_ms);
        }
#endif
        last_ms = now_ms;

        __disable_interrupt();
        update_dac = s_dac_a_update_pending;
        dac_value_mv = s_dac_a_mv;
        s_dac_a_update_pending = false;
        __enable_interrupt();

        if (update_dac) {
            dac_write(false, dac_value_mv);
        }

        /* B is stationary but periodically refreshed for the bench demo. */
        if ((uint16_t)(now_ms - last_b_refresh_ms) >= DAC_B_REFRESH_MS) {
            last_b_refresh_ms = now_ms;
            dac_write(true, DAC_B_FIXED_MV);
        }
    }
}
