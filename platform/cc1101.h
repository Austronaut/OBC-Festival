#ifndef CC1101_H
#define CC1101_H

#include <stdint.h>
#include "cc1101_regs.h"

/* Register image burst-written by CC1101_Init(): 433.92 MHz, 1 kbaud,
 * 2-FSK, 25 kHz deviation, channel 0, 16/16 sync word, CRC on.
 * Exposed so a test can read the registers back and compare. */
extern const uint8_t cc1101_config[CC1101_CONFIG_LEN];

/* CC1101_Init() result when the chip does not answer: VERSION reads 0x00
 * or 0xFF, i.e. MISO stuck, no power or wrong wiring. */
#define CC1101_INIT_NO_CHIP     2U

/* Reset the chip, check it answers (VERSION), burst-load cc1101_config, load
 * the PA table for +10 dBm. Returns 0 on success, 1 if the MISO handshake
 * timed out, CC1101_INIT_NO_CHIP if VERSION reads 0x00/0xFF (MISO
 * disconnected, or the CC1101 unpowered). Leaves the chip in IDLE. */
uint8_t CC1101_Init(void);

/* Same as ELECHOUSE setChannel(): writes CHANNR. Channel 0 = 433.92 MHz
 * with this config; each step adds the channel spacing (about 200 kHz). */
void CC1101_SetChannel(uint8_t channel);

/* Output power in dBm, 433 MHz table (same selection as ELECHOUSE setPA):
 * <=-30, -20, -15, -10, 0, 5, 7, 10. Values between steps round up. */
void CC1101_SetPA(int8_t dbm);

/* IDLE then STX, like ELECHOUSE SetTx(). Load the TX FIFO first. */
void CC1101_SetTx(void);

/* Back to IDLE (ELECHOUSE setSidle()). */
void CC1101_SetIdle(void);

/* Largest payload CC1101_SendPacket() accepts. Matches PKTLEN in
 * cc1101_config (61 bytes) and the ground decoder's limit. */
#define CC1101_MAX_PAYLOAD      61U

/* Return codes for CC1101_SendPacket(). */
#define CC1101_SEND_OK          0U
#define CC1101_SEND_BAD_LEN     1U   /* len was 0 or above CC1101_MAX_PAYLOAD */
#define CC1101_SEND_TIMEOUT     2U   /* still not IDLE after 1.5 s            */
#define CC1101_SEND_UNDERFLOW   3U   /* TX FIFO ran dry mid-packet; flushed   */
#define CC1101_SEND_NO_TX       4U   /* radio not in TX 5 ms after STX        */
#define CC1101_SEND_LOAD_ERROR  5U   /* TX FIFO byte count wrong after loading */
#define CC1101_SEND_LEFTOVER    6U   /* bytes left in TX FIFO after sending:
                                      * the length byte reached it corrupted */

/**
 * @brief  Transmits one variable-length packet and waits until it has gone.
 *
 * Loads the length byte and payload into the TX FIFO in one SPI burst and
 * checks the FIFO's byte count (TXBYTES), reloading up to 3 times if it is
 * wrong. Then it strobes STX, leaves the SPI bus idle for the packet's
 * expected airtime, and polls MARCSTATE every 1 ms until the radio is back
 * in IDLE (MCSM1 makes it return to IDLE after TX). Finally it checks the
 * FIFO is empty: leftover bytes mean the length byte was corrupted on its
 * way in and the packet went out short. The CC1101 adds the preamble, sync
 * word and CRC.
 *
 * Only byte-count problems are detectable here (the TX FIFO cannot be read
 * back). A corrupted data byte still goes out; the ground station's AX.25
 * checksum rejects it.
 *
 * @param[in] payload  Bytes to send. Must not be NULL.
 * @param[in] len      Payload length, 1 to CC1101_MAX_PAYLOAD.
 *
 * @pre    CC1101_Init() returned 0. No other SPI device is selected.
 * @post   The radio is in IDLE.
 *
 * @par Blocking
 *      Yes. (len + 7) * 8 ms of airtime plus ~5 ms (0.1-0.55 s), at most 1.5 s.
 * @par ISR-safe
 *      No.
 *
 * @retval CC1101_SEND_OK         The packet was transmitted.
 * @retval CC1101_SEND_BAD_LEN    Nothing was sent.
 * @retval CC1101_SEND_TIMEOUT    The radio never returned to IDLE; it has
 *                                been forced to IDLE and the FIFO flushed.
 * @retval CC1101_SEND_UNDERFLOW  The TX FIFO underflowed; it was flushed.
 * @retval CC1101_SEND_NO_TX      The radio was not transmitting 5 ms after
 *                                STX; it has been forced to IDLE and flushed.
 * @retval CC1101_SEND_LOAD_ERROR The FIFO byte count was still wrong after 3
 *                                loads; nothing was sent, FIFO flushed.
 * @retval CC1101_SEND_LEFTOVER   The packet went out shorter than len (bytes
 *                                were left in the FIFO); FIFO flushed.
 */
uint8_t CC1101_SendPacket(const uint8_t *payload, uint8_t len);

/* Results of CC1101_PollPacket(). */
#define CC1101_RX_NONE          0U   /* nothing yet, still listening          */
#define CC1101_RX_OK            1U   /* packet in buf, CRC good               */
#define CC1101_RX_BAD_CRC       2U   /* packet in buf, but CRC failed         */
#define CC1101_RX_ERROR         3U   /* overflow / bad length; nothing usable */

/**
 * @brief  Puts the radio into receive mode with an empty RX FIFO.
 *
 * @pre    CC1101_Init() returned 0. No other SPI device is selected.
 * @post   The radio is listening (MARCSTATE RX). After one packet arrives it
 *         drops back to IDLE by itself (MCSM1), so call this again to keep
 *         listening.
 *
 * @par Blocking
 *      No. Three command strobes.
 * @par ISR-safe
 *      No.
 */
void CC1101_StartRx(void);

/**
 * @brief  Checks once, without waiting, whether a packet has been received.
 *
 * Call it repeatedly after CC1101_StartRx(). Once the radio has finished a
 * packet (back in IDLE with data in the RX FIFO) it reads the length byte,
 * the payload and the two appended status bytes, and checks the CRC flag.
 *
 * @param[out] buf  Receives the payload. Must not be NULL.
 * @param[in]  max  Size of buf in bytes.
 * @param[out] len  Receives the payload length. Must not be NULL. Set to 0
 *                  unless the result is CC1101_RX_OK or CC1101_RX_BAD_CRC.
 *
 * @pre    CC1101_StartRx() was called. No other SPI device is selected.
 * @post   After any result other than CC1101_RX_NONE the radio is in IDLE
 *         with an empty RX FIFO. Call CC1101_StartRx() to listen again.
 *
 * @par Blocking
 *      No. A few SPI transfers (at most ~66 bytes when a packet is read).
 * @par ISR-safe
 *      No.
 *
 * @retval CC1101_RX_NONE     Still listening, nothing complete yet.
 * @retval CC1101_RX_OK       A packet with a good CRC is in buf.
 * @retval CC1101_RX_BAD_CRC  A packet is in buf but its CRC failed: ignore it.
 * @retval CC1101_RX_ERROR    FIFO overflow, a length that does not fit buf or
 *                            does not match the FIFO, or an unexpected state.
 */
uint8_t CC1101_PollPacket(uint8_t *buf, uint8_t max, uint8_t *len);

#endif /* CC1101_H */