/**
 * @file   sweep_packet.h
 * @brief  Splits one 50-point tether sweep into radio-sized chunks.
 *
 * Pure C with no hardware access, so it also compiles on a PC for testing.
 *
 * A sweep is too big for one CC1101 packet (61 bytes, minus 18 for AX.25),
 * so it goes out as SWEEP_PACKET_CHUNKS AX.25 info fields. Each one is:
 *
 *   offset  size  field
 *   0       1     type, SWEEP_PACKET_TYPE ('T')
 *   1       2     sweep_id     (uint16, little-endian)
 *   3       1     chunk_index  (0 .. chunk_count-1)
 *   4       1     chunk_count  (SWEEP_PACKET_CHUNKS)
 *   5       4*n   n currents   (int32, little-endian, units 1e-10 A)
 *
 * Chunks 0-4 carry 9 currents, chunk 5 carries the last 5 (9*5 + 5 = 50).
 * The ground decoder (ground/lil_decoder.py) must agree with this layout.
 */

#ifndef SWEEP_PACKET_H
#define SWEEP_PACKET_H

#include <stdint.h>

#define SWEEP_PACKET_TYPE             0x54U  /* 'T' for tether */
#define SWEEP_PACKET_HEADER_LEN       5U
#define SWEEP_PACKET_SAMPLES          50U    /* currents per sweep */
#define SWEEP_PACKET_VALUES_PER_CHUNK 9U
#define SWEEP_PACKET_CHUNKS \
    ((SWEEP_PACKET_SAMPLES + SWEEP_PACKET_VALUES_PER_CHUNK - 1U) / SWEEP_PACKET_VALUES_PER_CHUNK)
#define SWEEP_PACKET_MAX_LEN \
    (SWEEP_PACKET_HEADER_LEN + 4U * SWEEP_PACKET_VALUES_PER_CHUNK)   /* 41 bytes */

/**
 * @brief  Builds the AX.25 info field for one chunk of a sweep.
 *
 * @param[in]  sweep_id  Sweep sequence number, shown on the ground. Wraps.
 * @param[in]  chunk     Chunk index, 0 to SWEEP_PACKET_CHUNKS - 1.
 * @param[in]  sweep     All SWEEP_PACKET_SAMPLES currents of the sweep.
 *                       Must not be NULL.
 * @param[out] out       Receives the info bytes. Must hold at least
 *                       SWEEP_PACKET_MAX_LEN bytes. Must not be NULL.
 *
 * @pre    None.
 *
 * @par Blocking
 *      No.
 * @par ISR-safe
 *      Yes.
 *
 * @return Number of bytes written to out, or 0 if chunk is out of range or
 *         a pointer is NULL.
 */
uint8_t SweepPacket_Build(uint16_t sweep_id, uint8_t chunk,
                          const int32_t *sweep, uint8_t *out);

#endif /* SWEEP_PACKET_H */
