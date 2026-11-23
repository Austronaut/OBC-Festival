/**
 * @file   uart_backchannel.h
 * @brief  Polled UART on eUSCI_A0 (P2.0 TX, P2.1 RX): the LaunchPad's USB
 *         "backchannel" serial port to the laptop.
 *
 * On Linux the LaunchPad shows up as two serial ports; this is the second
 * one, usually /dev/ttyACM1 (the first is the debugger). 9600 baud, 8N1.
 *
 * Requires HAL_Clock_Init() first: the baud rate settings assume an
 * 8 MHz SMCLK. Everything is polled (no interrupts), to match the rest of
 * the demo firmware.
 */

#ifndef UART_BACKCHANNEL_H
#define UART_BACKCHANNEL_H

#include <stdbool.h>
#include <stdint.h>

#define UART_BACKCHANNEL_BAUD  9600UL

/**
 * @brief  Routes P2.0/P2.1 to eUSCI_A0 and starts it at 9600 baud, 8N1.
 *
 * @pre    HAL_Clock_Init() has been called (SMCLK = 8 MHz) and the GPIOs are
 *         unlocked (HAL_SPI_Init() does PMM_unlockLPM5()).
 *
 * @par Blocking
 *      No.
 * @par ISR-safe
 *      No.
 */
void uart_backchannel_init(void);

/**
 * @brief  Sends one byte.
 *
 * @param[in] byte  Byte to send.
 *
 * @pre    uart_backchannel_init() has been called.
 *
 * @par Blocking
 *      Yes. Waits for the transmit buffer, at most ~1 ms per byte at 9600 baud.
 * @par ISR-safe
 *      No.
 */
void uart_backchannel_put_char(char byte);

/**
 * @brief  Sends a NUL-terminated string.
 *
 * @param[in] text  String to send. Must not be NULL.
 *
 * @pre    uart_backchannel_init() has been called.
 *
 * @par Blocking
 *      Yes. About 1 ms per character at 9600 baud.
 * @par ISR-safe
 *      No.
 */
void uart_backchannel_put_string(const char *text);

/**
 * @brief  Sends a byte as two hex digits, e.g. 0x2E -> "2E".
 *
 * @param[in] value  Byte to print.
 *
 * @pre    uart_backchannel_init() has been called.
 *
 * @par Blocking
 *      Yes. About 2 ms.
 * @par ISR-safe
 *      No.
 */
void uart_backchannel_put_hex(uint8_t value);

/**
 * @brief  Takes one received byte, if there is one, without waiting.
 *
 * @param[out] byte  Receives the byte. Must not be NULL.
 *
 * @pre    uart_backchannel_init() has been called.
 *
 * @par Blocking
 *      No.
 * @par ISR-safe
 *      No.
 *
 * @retval true   A byte was waiting and has been stored in *byte.
 * @retval false  Nothing received; *byte is unchanged.
 */
bool uart_backchannel_get_char(char *byte);

#endif /* UART_BACKCHANNEL_H */
