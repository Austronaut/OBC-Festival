/**
 * @file main.c
 * @brief Engineering Festival demo: tether motor + DAC sweep + radio downlink.
 *
 * After boot the board waits for a start trigger: either LaunchPad button
 * S1 (P5.6), or an uplink packet "TETHER_GO" (uplink_cmd.h) received by the
 * CC1101, sent by the ground LaunchPad (ground/uplink/uplink_msp430/). Each
 * trigger runs the demo once,
 * deliberately synchronous (each call blocks until it has finished):
 *
 *   1. Motor, once: 2 revolutions in the uncoil direction, then the A4988
 *      is switched off (nothing is attached, so there is no return).
 *   2. For each of the TETHER_SIM_SWEEP_COUNT simulated sweeps:
 *        a. DAC A: one 0-4 V voltage sweep (DAC B stays at 2 V).
 *        b. Radio: that sweep's 50 currents sent as 6 AX.25 packets. The
 *           ground station decodes them into ground/iv_viewer/readings.txt.
 *   3. Back to waiting for the next trigger.
 *
 * One press takes about 4 s (motor) + 10 x (1 s sweep + 3.5 s radio),
 * roughly 50 s. With DEMO_DAC_SWEEP = 0 (below) the voltage sweeps are
 * skipped: about 4 s of motor, then the 10 downlinks (~35 s). The LED left
 * on afterwards shows how the run went.
 */

/* 1 = use the tether hardware: init the DAC/motor driver at boot and turn
 *     the motor once at the start of each run.
 * 0 = no Tether_* calls at all (radio-only bench testing). */
#define DEMO_USE_TETHER         1

/* 1 = run a DAC A 0-4 V voltage sweep before downlinking each sweep.
 * 0 = no voltage sweep: each run just sends the stored simulated data.
 * Only has an effect when DEMO_USE_TETHER is 1. */
#define DEMO_DAC_SWEEP          0

#include <stdint.h>
#include <string.h>

#include <driverlib.h>

#include "HAL_SPI.h"
#include "HAL_clock.h"
#include "cc1101.h"
#include "sweep_packet.h"
#include "sweep_tx.h"
#if DEMO_USE_TETHER
#include "tether.h"
#endif
#include "tether_sim_data.h"
#include "uplink_cmd.h"

#if TETHER_SIM_SAMPLES != SWEEP_PACKET_SAMPLES
#error "Simulated sweep length does not match the radio packet format"
#endif

#define DEMO_STEPS_PER_REV      200U   /* full steps, NEMA-8 = 1.8 deg/step */
#define DEMO_MOTOR_REVS         2U     /* revolutions per button press */

/* Radio output power in dBm (CC1101_SetPA steps: -30, -20, -15, -10, 0, 5,
 * 7, 10). CC1101_Init() sets +10. DEBUG: try -10 to test whether the
 * transmitter's own power is disturbing the bench wiring. */
#define DEMO_TX_POWER_DBM       10

#define LED_PORT   GPIO_PORT_P1
#define RED_LED    GPIO_PIN0   /* P1.0 */
#define GREEN_LED  GPIO_PIN1   /* P1.1 */

/* Status LEDs (the board runs on battery, so these are the only display):
 *
 *   waiting, all OK      green: 50 ms blink every 1 s (heartbeat)
 *   waiting, radio fault red:   fast blink (radio init / register check failed)
 *   trigger accepted     both:  brief flash
 *   motor turning        both:  on
 *   sending              green: toggles on every chunk sent
 *   chunk re-sent        red:   brief flash
 *   run finished         green (all sent) or red (errors): solid for 5 s,
 *                        then back to the heartbeat / fault blink
 */
#define LED_HEARTBEAT_PERIOD_MS  1000U
#define LED_HEARTBEAT_ON_MS        50U
#define LED_FAULT_BLINK_MS        100U   /* half period of the fault blink */
#define LED_TRIGGER_FLASH_MS      150U
#define LED_RETRY_FLASH_MS        100U
#define LED_RESULT_HOLD_MS       5000U

#define RESULT_NONE  0U
#define RESULT_OK    1U
#define RESULT_FAIL  2U

/* LaunchPad S1: shorts P5.6 to GND when pressed (needs the internal pull-up). */
#define BUTTON_PORT         GPIO_PORT_P5
#define BUTTON_PIN          GPIO_PIN6
#define BUTTON_DEBOUNCE_MS  20U

/* The start command itself is UPLINK_CMD_START in uplink_cmd.h, shared with
 * the ground transmitter. The CC1101's sync word and CRC already reject
 * noise; matching the text rejects other devices that happen to use a
 * compatible packet format. */
#define TRIGGER_POLL_MS     10U

/* 1 = listen for the uplink start command while waiting (radio receive
 * mode). 0 = S1 only; the radio never enters receive mode, like the
 * firmware that gave clean sweeps on 23 Nov morning. DEBUG A/B switch. */
#define DEMO_RADIO_TRIGGER  1

#define TRIGGER_BUTTON      1U
#define TRIGGER_RADIO       2U

/* Results are globals so they can be watched in CCS's Expressions view. */
volatile uint8_t  g_radio_init_err;  /* 0 - CC1101 reset handshake OK          */
volatile int8_t   g_ax25_init_err;   /* 0 - AX.25 header built                  */
volatile uint8_t  g_last_tx_err;     /* 0 - else SWEEP_TX_* / CC1101_SEND_*     */
volatile uint16_t g_sweep_id;        /*     id of the last sweep sent           */
volatile uint16_t g_tx_failures;     /*     sweeps whose radio send failed      */
volatile uint8_t  g_trigger_source;  /*     1 = button S1, 2 = radio command    */
volatile uint16_t g_rx_ok;           /*     good packets received while waiting */
volatile uint16_t g_rx_bad_crc;      /*     packets with a failed CRC           */
volatile uint16_t g_rx_other;        /*     wrong command / RX errors           */
volatile uint8_t  g_rx_last[16];     /*     start of the last packet received   */
volatile uint8_t  g_run_radio_err;   /* 0 - radio reset + config OK at run start */

/* DEBUG (SPI corruption hunt): after CC1101_Init's 47-byte burst write,
 * read the registers back and compare. Remove once the SPI issue is fixed. */
volatile uint8_t  g_cfg_mismatch;    /* 0    - registers that read back wrong   */
volatile uint8_t  g_cfg_first_bad;   /* 0xFF - else index of first bad register */
volatile uint8_t  g_cfg_readback[CC1101_CONFIG_LEN];

static void check_config_readback(void)
{
    uint8_t i;

    HAL_SPI_ReadBurstReg(CC1101_CONFIG_START, (uint8_t *)g_cfg_readback,
                         CC1101_CONFIG_LEN);
    g_cfg_mismatch  = 0U;
    g_cfg_first_bad = 0xFFU;
    for (i = 0U; i < CC1101_CONFIG_LEN; ++i)
    {
        if (g_cfg_readback[i] != cc1101_config[i])
        {
            if (g_cfg_mismatch == 0U)
            {
                g_cfg_first_bad = i;
            }
            ++g_cfg_mismatch;
        }
    }
}

/* Sweep id for the radio. Keeps counting across button presses: the ground
 * decoder ignores ids it has recently written (to drop repeated packets), so
 * restarting at 0 on every press would make later runs look like repeats. */
static uint16_t s_sweep_id;

static uint8_t s_last_result;   /* RESULT_*: shown for 5 s after a run */
static uint8_t s_radio_fault;   /* 1 = radio failed its last init/check */
static uint8_t s_green_on;      /* current green state while sending */

static void leds(uint8_t red_on, uint8_t green_on)
{
    if (red_on)   { GPIO_setOutputHighOnPin(LED_PORT, RED_LED); }
    else          { GPIO_setOutputLowOnPin(LED_PORT, RED_LED); }
    if (green_on) { GPIO_setOutputHighOnPin(LED_PORT, GREEN_LED); }
    else          { GPIO_setOutputLowOnPin(LED_PORT, GREEN_LED); }
    s_green_on = green_on;
}

/* LEDs while waiting. waited_ms counts from the start of this wait. */
static void update_wait_leds(uint32_t waited_ms)
{
    if (s_last_result != RESULT_NONE && waited_ms < LED_RESULT_HOLD_MS)
    {
        leds(s_last_result == RESULT_FAIL, s_last_result == RESULT_OK);
    }
    else if (s_radio_fault)
    {
        leds((uint8_t)((waited_ms / LED_FAULT_BLINK_MS) & 1U), 0U);
    }
    else
    {
        leds(0U, (uint8_t)((waited_ms % LED_HEARTBEAT_PERIOD_MS) < LED_HEARTBEAT_ON_MS));
    }
}

/* Called by SweepTx_Send() for every chunk (see sweep_tx.h). */
static void on_sweep_tx_event(uint8_t event)
{
    if (event == SWEEP_TX_EVENT_SENT)
    {
        leds(0U, (uint8_t)!s_green_on);          /* toggle green */
    }
    else if (event == SWEEP_TX_EVENT_RETRY)
    {
        uint8_t green = s_green_on;
        leds(1U, green);                         /* brief red flash */
        HAL_DELAY_MS(LED_RETRY_FLASH_MS);
        leds(0U, green);
    }
}

static uint8_t button_down(void)
{
    return GPIO_getInputPinValue(BUTTON_PORT, BUTTON_PIN) == GPIO_INPUT_PIN_LOW;
}

/* Called once S1 has been seen down. Confirms it is a real press (still
 * down after BUTTON_DEBOUNCE_MS, which ignores contact bounce), then waits
 * for the release, so holding the button cannot start a second run.
 * Returns 0 if it was only a glitch. */
static uint8_t confirm_button_press(void)
{
    HAL_DELAY_MS(BUTTON_DEBOUNCE_MS);
    if (!button_down())
    {
        return 0U;
    }

    do
    {
        while (button_down())
        {
        }
        HAL_DELAY_MS(BUTTON_DEBOUNCE_MS);
    } while (button_down());

    return 1U;
}

/* Checks the radio once. Returns 1 if the start command arrived. Any other
 * packet or error is counted and the radio goes back to listening. */
static uint8_t check_uplink(void)
{
    uint8_t buf[CC1101_MAX_PAYLOAD];
    uint8_t len;
    uint8_t result = CC1101_PollPacket(buf, sizeof(buf), &len);

    if (result == CC1101_RX_NONE)
    {
        return 0U;
    }

    if (len > 0U)
    {
        memcpy((void *)g_rx_last, buf,
               (len < sizeof(g_rx_last)) ? len : sizeof(g_rx_last));
    }

    if (result == CC1101_RX_OK)
    {
        ++g_rx_ok;
        if (len == UPLINK_CMD_START_LEN &&
            memcmp(buf, UPLINK_CMD_START, UPLINK_CMD_START_LEN) == 0)
        {
            return 1U;
        }
        ++g_rx_other;   /* good packet, but not our command */
    }
    else if (result == CC1101_RX_BAD_CRC)
    {
        ++g_rx_bad_crc;
    }
    else
    {
        ++g_rx_other;
    }

    CC1101_StartRx();   /* PollPacket left the radio in IDLE */
    return 0U;
}

/* Blocks until S1 is pressed or the uplink start command is received, and
 * returns which one (TRIGGER_BUTTON / TRIGGER_RADIO). The radio only
 * listens here, never during a run. Before returning it is put back in
 * IDLE with an empty RX FIFO, so the run's transmissions start clean. */
static uint8_t wait_for_trigger(uint8_t radio_ok)
{
    uint8_t  source = 0U;
    uint32_t waited_ms = 0U;   /* approximate: counts poll intervals */

    radio_ok = (uint8_t)(radio_ok && DEMO_RADIO_TRIGGER);   /* listen at all? */

    if (radio_ok)
    {
        CC1101_StartRx();
    }

    while (source == 0U)
    {
        if (button_down() && confirm_button_press())
        {
            source = TRIGGER_BUTTON;
        }
        else if (radio_ok && check_uplink())
        {
            source = TRIGGER_RADIO;
        }
        else
        {
            update_wait_leds(waited_ms);
            HAL_DELAY_MS(TRIGGER_POLL_MS);
            waited_ms += TRIGGER_POLL_MS;
        }
    }

    leds(1U, 1U);                        /* trigger accepted: brief flash */
    HAL_DELAY_MS(LED_TRIGGER_FLASH_MS);
    leds(0U, 0U);

    if (radio_ok)
    {
        HAL_SPI_Strobe(CC1101_SIDLE);
        HAL_SPI_Strobe(CC1101_SFRX);
    }
    return source;
}

/* One complete demo run: motor once, then every simulated sweep. */
static void run_demo(uint8_t radio_ok)
{
    uint8_t table_index;
    uint8_t run_ok = radio_ok;

    /* 0. Reset the radio and reload its registers, so every run starts from
     *    the same state as a clean boot, whatever it was doing while we
     *    waited (receive mode, a half-received packet, ...). Then check the
     *    registers read back correctly. */
    if (radio_ok)
    {
        g_run_radio_err = CC1101_Init();
        CC1101_SetPA(DEMO_TX_POWER_DBM);
        check_config_readback();
        if (g_run_radio_err != 0U || g_cfg_mismatch != 0U)
        {
            g_run_radio_err = (g_run_radio_err != 0U) ? g_run_radio_err : 0xFFU;
            run_ok = 0U;
        }
        s_radio_fault = (uint8_t)(g_run_radio_err != 0U);
    }

#if DEMO_USE_TETHER
    /* 1. Motor, once per press. Switched off afterwards: nothing needs
     *    holding, and an idle driver adds no coil noise during the radio
     *    transmissions. */
    leds(1U, 1U);                        /* motor turning */
    Tether_Uncoil(DEMO_STEPS_PER_REV * DEMO_MOTOR_REVS);
    Tether_DisableMotor();
    leds(0U, 0U);
#endif

//     /* 2. Each simulated sweep: (DAC ramp,) then downlink its readings. */
    for (table_index = 0U; table_index < TETHER_SIM_SWEEP_COUNT; ++table_index)
    {
#if DEMO_USE_TETHER && DEMO_DAC_SWEEP
        Tether_SweepVoltageA();
#endif

        if (radio_ok)
        {
            g_sweep_id    = s_sweep_id;
            g_last_tx_err = SweepTx_Send(s_sweep_id, tether_sim_sweeps[table_index]);
            if (g_last_tx_err != SWEEP_TX_OK)
            {
                ++g_tx_failures;
                run_ok = 0U;
            }
            ++s_sweep_id;
        }
    }

    /* Shown for LED_RESULT_HOLD_MS at the start of the next wait. */
    s_last_result = run_ok ? RESULT_OK : RESULT_FAIL;
}

int main(void)
{
    uint8_t radio_ok;

    WDT_A_hold(WDT_A_BASE);   /* stop the watchdog before anything slow */

    /* Init order matters:
     *  - The clock comes first: SPI and every delay are derived from it.
     *  - HAL_SPI_Init sets up eUSCI_B1 and the CC1101 CS (P4.4) and unlocks
     *    the GPIOs (LOCKLPM5).
     *  - (With the tether) Tether_Init drives the DAC CS (P3.7) high. This
     *    must happen BEFORE any CC1101 traffic: an undriven DAC CS could let
     *    the DAC treat radio bytes as DAC writes.
     *  - Then the radio. */
    HAL_Clock_Init();
    HAL_SPI_Init();
#if DEMO_USE_TETHER
    Tether_Init();                         /* DAC A = 0 V, B = 2 V, motor off */
#endif
    g_radio_init_err = CC1101_Init();
    CC1101_SetPA(DEMO_TX_POWER_DBM);
    g_ax25_init_err  = SweepTx_Init();
    check_config_readback();               /* DEBUG: see g_cfg_* */

    GPIO_setOutputLowOnPin(LED_PORT, RED_LED | GREEN_LED);
    GPIO_setAsOutputPin(LED_PORT, RED_LED | GREEN_LED);
    GPIO_setAsInputPinWithPullUpResistor(BUTTON_PORT, BUTTON_PIN);

    radio_ok = (g_radio_init_err == 0U && g_ax25_init_err == 0);
    s_radio_fault = (uint8_t)(!radio_ok || g_cfg_mismatch != 0U);
    SweepTx_SetEventHandler(on_sweep_tx_event);

    while (1)
    {
        g_trigger_source = wait_for_trigger(radio_ok);
        run_demo(radio_ok);
    }
}
