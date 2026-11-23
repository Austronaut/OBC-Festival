#include "uart_backchannel.h"

#include <stdbool.h>
#include <stdint.h>

#include <driverlib.h>

#include "HAL_clock.h"

#define UART_BASE   EUSCI_A0_BASE
#define UART_PORT   GPIO_PORT_P2
#define UART_PINS   (GPIO_PIN0 | GPIO_PIN1)   /* P2.0 UCA0TXD, P2.1 UCA0RXD */

/* Divider settings for 9600 baud from an 8 MHz clock, from the baud rate
 * table in the MSP430FR5xx family user's guide (SLAU367, "Typical Baud Rates
 * and Errors"): oversampling on, UCBRx = 52, UCBRFx = 1, UCBRSx = 0x49. */
#if HAL_CLOCK_SMCLK_HZ != 8000000UL
#error "UART divider settings below assume an 8 MHz SMCLK"
#endif
#define UART_BR_PRESCALER   52U
#define UART_BR_FIRST_MOD   1U
#define UART_BR_SECOND_MOD  0x49U

void uart_backchannel_init(void)
{
    EUSCI_A_UART_initParam param = {0};

    /* On the FR5994 the UART is the pins' secondary function. */
    GPIO_setAsPeripheralModuleFunctionInputPin(UART_PORT, UART_PINS,
                                               GPIO_SECONDARY_MODULE_FUNCTION);

    param.selectClockSource = EUSCI_A_UART_CLOCKSOURCE_SMCLK;
    param.clockPrescalar    = UART_BR_PRESCALER;
    param.firstModReg       = UART_BR_FIRST_MOD;
    param.secondModReg      = UART_BR_SECOND_MOD;
    param.parity            = EUSCI_A_UART_NO_PARITY;
    param.msborLsbFirst     = EUSCI_A_UART_LSB_FIRST;
    param.numberofStopBits  = EUSCI_A_UART_ONE_STOP_BIT;
    param.uartMode          = EUSCI_A_UART_MODE;
    param.overSampling      = EUSCI_A_UART_OVERSAMPLING_BAUDRATE_GENERATION;

    EUSCI_A_UART_init(UART_BASE, &param);
    EUSCI_A_UART_enable(UART_BASE);
}

void uart_backchannel_put_char(char byte)
{
    /* Wait until the transmit buffer is free, then hand the byte over. */
    while (!EUSCI_A_UART_getInterruptStatus(UART_BASE, EUSCI_A_UART_TRANSMIT_INTERRUPT_FLAG))
    {
    }
    EUSCI_A_UART_transmitData(UART_BASE, (uint8_t)byte);
}

void uart_backchannel_put_string(const char *text)
{
    while (*text != '\0')
    {
        uart_backchannel_put_char(*text);
        ++text;
    }
}

void uart_backchannel_put_hex(uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    uart_backchannel_put_char(digits[value >> 4]);
    uart_backchannel_put_char(digits[value & 0x0FU]);
}

bool uart_backchannel_get_char(char *byte)
{
    if (!EUSCI_A_UART_getInterruptStatus(UART_BASE, EUSCI_A_UART_RECEIVE_INTERRUPT_FLAG))
    {
        return false;
    }
    *byte = (char)EUSCI_A_UART_receiveData(UART_BASE);   /* clears RXIFG */
    return true;
}
