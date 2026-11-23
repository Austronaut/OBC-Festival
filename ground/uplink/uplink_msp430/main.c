/**
 * @file main.c
 * @brief Ground transmitter: sends the "start demo" command to the OBC.
 *
 * Runs on a second MSP-EXP430FR5994 LaunchPad with its own CC1101, wired
 * exactly like the OBC's (see platform/HAL_SPI.h):
 *
 *   CC1101 SI   - P5.0    CC1101 SO (GDO1) - P5.1    CC1101 SCLK - P5.2
 *   CC1101 CSn  - P4.4    CC1101 VCC - 3V3            CC1101 GND  - GND
 *
 * It uses the OBC's own clock, SPI and CC1101 drivers (linked from
 * platform/, not copied), so both radios load the same register table.
 *
 * A send is triggered by either:
 *   - pressing S1 (P5.6) on this LaunchPad, or
 *   - typing "go" on the USB serial port (9600 baud), e.g. with
 *     ground/uplink/uplink.py.
 *
 * Other serial commands: "status" (radio state), "info" (chip ID and
 * register readback, to check the wiring).
 *
 * LEDs: green = last send OK, red = last send failed or radio not found.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <driverlib.h>

#include "HAL_SPI.h"
#include "HAL_clock.h"
#include "cc1101.h"
#include "uart_backchannel.h"
#include "uplink_cmd.h"

#define LED_PORT            GPIO_PORT_P1
#define RED_LED             GPIO_PIN0
#define GREEN_LED           GPIO_PIN1

#define BUTTON_PORT         GPIO_PORT_P5
#define BUTTON_PIN          GPIO_PIN6     /* S1, pressed = low */
#define BUTTON_DEBOUNCE_MS  20U

#define LINE_MAX            16U           /* longest serial command + 1 */

/* Debug values for CCS's Expressions view. */
volatile uint8_t  g_radio_init_err;
volatile uint8_t  g_last_send_err;
volatile uint16_t g_sends;

static void show_status(bool ok)
{
    GPIO_setOutputLowOnPin(LED_PORT, RED_LED | GREEN_LED);
    GPIO_setOutputHighOnPin(LED_PORT, ok ? GREEN_LED : RED_LED);
}

static bool button_down(void)
{
    return GPIO_getInputPinValue(BUTTON_PORT, BUTTON_PIN) == GPIO_INPUT_PIN_LOW;
}

/* Same logic as the OBC: still down after the debounce time = real press,
 * then wait for the release so one press sends once. */
static bool button_pressed(void)
{
    if (!button_down())
    {
        return false;
    }
    HAL_DELAY_MS(BUTTON_DEBOUNCE_MS);
    if (!button_down())
    {
        return false;
    }
    do
    {
        while (button_down())
        {
        }
        HAL_DELAY_MS(BUTTON_DEBOUNCE_MS);
    } while (button_down());
    return true;
}

/* Reads MARCSTATE until two reads agree (CC1101 status-read errata). */
static uint8_t marcstate(void)
{
    uint8_t first;
    uint8_t second = HAL_SPI_ReadStatus(CC1101_MARCSTATE) & 0x1FU;

    do
    {
        first  = second;
        second = HAL_SPI_ReadStatus(CC1101_MARCSTATE) & 0x1FU;
    } while (first != second);
    return second;
}

/* Chip ID and config readback: the quickest check that the wiring works. */
static void print_info(void)
{
    uint8_t readback[CC1101_CONFIG_LEN];
    uint8_t i;
    uint8_t mismatches = 0U;

    uart_backchannel_put_string("PARTNUM 0x");
    uart_backchannel_put_hex(HAL_SPI_ReadStatus(CC1101_PARTNUM));
    uart_backchannel_put_string(" (expect 00), VERSION 0x");
    uart_backchannel_put_hex(HAL_SPI_ReadStatus(CC1101_VERSION));
    uart_backchannel_put_string(" (expect 14 or 04)\r\n");

    HAL_SPI_ReadBurstReg(CC1101_CONFIG_START, readback, CC1101_CONFIG_LEN);
    for (i = 0U; i < CC1101_CONFIG_LEN; ++i)
    {
        if (readback[i] != cc1101_config[i])
        {
            if (mismatches == 0U)
            {
                uart_backchannel_put_string("config mismatch at register:");
            }
            uart_backchannel_put_string(" 0x");
            uart_backchannel_put_hex(i);
            ++mismatches;
        }
    }
    if (mismatches != 0U)
    {
        uart_backchannel_put_string("\r\n");
    }
    uart_backchannel_put_string("config mismatches: 0x");
    uart_backchannel_put_hex(mismatches);
    uart_backchannel_put_string("\r\n");
}

static void send_start_command(const char *source)
{
    g_last_send_err = CC1101_SendPacket((const uint8_t *)UPLINK_CMD_START,
                                        (uint8_t)UPLINK_CMD_START_LEN);
    ++g_sends;

    uart_backchannel_put_string(source);
    if (g_last_send_err == CC1101_SEND_OK)
    {
        uart_backchannel_put_string(": sent\r\n");
    }
    else
    {
        /* Codes in platform/cc1101.h: 2 timeout, 3 underflow, 4 no TX. */
        uart_backchannel_put_string(": error 0x");
        uart_backchannel_put_hex(g_last_send_err);
        uart_backchannel_put_string("\r\n");
    }
    show_status(g_last_send_err == CC1101_SEND_OK);
}

static void handle_line(const char *line)
{
    if (line[0] == '\0')
    {
        return;                                  /* blank line */
    }
    if (strcmp(line, "go") == 0)
    {
        if (g_radio_init_err != 0U)
        {
            uart_backchannel_put_string("error: radio not initialised\r\n");
            return;
        }
        send_start_command("serial");
    }
    else if (strcmp(line, "status") == 0)
    {
        uart_backchannel_put_string("MARCSTATE 0x");
        uart_backchannel_put_hex(marcstate());
        uart_backchannel_put_string("\r\n");
    }
    else if (strcmp(line, "info") == 0)
    {
        print_info();
    }
    else
    {
        uart_backchannel_put_string("unknown command (go | status | info)\r\n");
    }
}

int main(void)
{
    char    line[LINE_MAX];
    uint8_t line_len = 0U;
    char    c;

    WDT_A_hold(WDT_A_BASE);

    HAL_Clock_Init();
    HAL_SPI_Init();                  /* also unlocks the GPIOs (LOCKLPM5) */
    g_radio_init_err = CC1101_Init();
    uart_backchannel_init();

    GPIO_setOutputLowOnPin(LED_PORT, RED_LED | GREEN_LED);
    GPIO_setAsOutputPin(LED_PORT, RED_LED | GREEN_LED);
    GPIO_setAsInputPinWithPullUpResistor(BUTTON_PORT, BUTTON_PIN);

    uart_backchannel_put_string("\r\nuplink_msp430: CC1101 ground transmitter\r\n");
    if (g_radio_init_err != 0U)
    {
        uart_backchannel_put_string("error: CC1101 not responding (check wiring and 3V3)\r\n");
        show_status(false);
    }
    print_info();
    uart_backchannel_put_string("commands: go | status | info, or press S1\r\n");

    while (1)
    {
        if (g_radio_init_err == 0U && button_pressed())
        {
            send_start_command("button");
        }

        /* Collect a line from the serial port (lower-cased, CR or LF ends it). */
        if (uart_backchannel_get_char(&c))
        {
            if (c == '\r' || c == '\n')
            {
                line[line_len] = '\0';
                handle_line(line);
                line_len = 0U;
            }
            else if (line_len < LINE_MAX - 1U)
            {
                line[line_len++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            /* else: too long; ignore extra characters until the line ends */
        }
    }
}
