#include "sweep_packet.h"

#include <stddef.h>
#include <stdint.h>

/* Write v into p[0..3], least significant byte first. Done with shifts rather
 * than casting the buffer to int32_t*, so the result does not depend on the
 * CPU's byte order or alignment rules. (The MSP430 is little-endian anyway,
 * but this also gives the same bytes when the code is tested on a PC.) */
static void put_le32(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;   /* two's complement bit pattern */

    p[0] = (uint8_t)(u & 0xFFU);
    p[1] = (uint8_t)((u >> 8) & 0xFFU);
    p[2] = (uint8_t)((u >> 16) & 0xFFU);
    p[3] = (uint8_t)((u >> 24) & 0xFFU);
}

uint8_t SweepPacket_Build(uint16_t sweep_id, uint8_t chunk,
                          const int32_t *sweep, uint8_t *out)
{
    uint8_t first;
    uint8_t count;
    uint8_t i;

    if (sweep == NULL || out == NULL || chunk >= SWEEP_PACKET_CHUNKS)
    {
        return 0U;
    }

    first = (uint8_t)(chunk * SWEEP_PACKET_VALUES_PER_CHUNK);
    count = (uint8_t)(SWEEP_PACKET_SAMPLES - first);
    if (count > SWEEP_PACKET_VALUES_PER_CHUNK)
    {
        count = SWEEP_PACKET_VALUES_PER_CHUNK;
    }

    out[0] = SWEEP_PACKET_TYPE;
    out[1] = (uint8_t)(sweep_id & 0xFFU);
    out[2] = (uint8_t)(sweep_id >> 8);
    out[3] = chunk;
    out[4] = (uint8_t)SWEEP_PACKET_CHUNKS;

    for (i = 0U; i < count; ++i)
    {
        put_le32(&out[SWEEP_PACKET_HEADER_LEN + 4U * i], sweep[first + i]);
    }

    return (uint8_t)(SWEEP_PACKET_HEADER_LEN + 4U * count);
}
