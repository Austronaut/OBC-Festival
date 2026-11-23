/**
 * @file   sweep_tx.h
 * @brief  Sends one tether sweep over the CC1101 as AX.25 UI frames.
 *
 * Each sweep becomes SWEEP_PACKET_CHUNKS packets (see sweep_packet.h for the
 * layout), each wrapped in an AX.25 UI frame (lib/ax25) and sent with
 * CC1101_SendPacket(). Everything is synchronous.
 */

#ifndef SWEEP_TX_H
#define SWEEP_TX_H

#include <stdint.h>

/* SweepTx_Send() returns 0 on success, else one of these or a
 * CC1101_SEND_* code (cc1101.h) from the packet that failed. */
#define SWEEP_TX_OK            0U
#define SWEEP_TX_NOT_INIT      0x10U  /* SweepTx_Init() failed or not called */
#define SWEEP_TX_ENCODE_ERROR  0x11U  /* AX.25 encoding failed (a bug)      */

/* Events reported to the optional handler (SweepTx_SetEventHandler). */
#define SWEEP_TX_EVENT_SENT    1U  /* a chunk went out (driver reported OK) */
#define SWEEP_TX_EVENT_RETRY   2U  /* a chunk failed; it is about to be re-sent */

/* A pointer to a function taking one event code and returning nothing. The
 * caller (main.c) uses it to blink LEDs, so this module needs no knowledge
 * of the LEDs. */
typedef void (*SweepTx_EventHandler)(uint8_t event);

/**
 * @brief  Registers a function to be called on every sent or retried chunk.
 *
 * @param[in] handler  Function to call, or NULL for none. It runs inside
 *                     SweepTx_Send(), so it must be quick (a few ms at most).
 *
 * @pre    None.
 *
 * @par Blocking
 *      No.
 * @par ISR-safe
 *      No.
 */
void SweepTx_SetEventHandler(SweepTx_EventHandler handler);

/**
 * @brief  Builds the AX.25 address header used for every sweep packet.
 *
 * @pre    None.
 *
 * @par Blocking
 *      No.
 * @par ISR-safe
 *      No.
 *
 * @retval 0      Header ready.
 * @retval other  AX25_ERR_* from ax25_create_hdr() (bad callsign).
 */
int8_t SweepTx_Init(void);

/**
 * @brief  Transmits all chunks of one sweep, in order.
 *
 * @param[in] sweep_id  Sequence number the ground station uses to put the
 *                      chunks back together. Use a new one for every sweep.
 * @param[in] sweep     SWEEP_PACKET_SAMPLES currents (1e-10 A). Not NULL.
 *
 * @pre    SweepTx_Init() returned 0 and CC1101_Init() returned 0.
 * @post   The radio is in IDLE.
 *
 * @par Blocking
 *      Yes. About 3.5 s (6 packets of ~0.5 s airtime plus short gaps). A
 *      chunk whose send fails in a way the radio driver can detect is sent
 *      up to 3 times; up to ~1 s extra per retried chunk. Stops early if a
 *      chunk still fails after its last attempt.
 * @par ISR-safe
 *      No.
 *
 * @return SWEEP_TX_OK, SWEEP_TX_NOT_INIT, SWEEP_TX_ENCODE_ERROR, or the
 *         CC1101_SEND_* code of the packet that failed.
 */
uint8_t SweepTx_Send(uint16_t sweep_id, const int32_t *sweep);

#endif /* SWEEP_TX_H */
