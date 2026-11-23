/*
 * Host-side helper for test_sweep_packets.py (not built for the MSP430).
 *
 * Builds the exact AX.25 frames the firmware sends for one sweep, using the
 * real firmware sources: platform/sweep_packet.c, lib/ax25/ax25.c and the
 * generated platform/tether_sim_data.c. Prints one frame per line as hex.
 *
 * Usage: host_sweep_frames <sweep_id> <table_index>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ax25.h"
#include "sweep_packet.h"
#include "tether_sim_data.h"

int main(int argc, char **argv)
{
    ax25_hdr hdr;
    uint8_t info[SWEEP_PACKET_MAX_LEN];
    uint8_t frame[AX25_OVERHEAD + SWEEP_PACKET_MAX_LEN];
    unsigned sweep_id;
    unsigned index;
    uint8_t chunk;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <sweep_id> <table_index>\n", argv[0]);
        return 2;
    }
    sweep_id = (unsigned)strtoul(argv[1], NULL, 0);
    index = (unsigned)strtoul(argv[2], NULL, 0);
    if (index >= TETHER_SIM_SWEEP_COUNT) {
        fprintf(stderr, "table_index out of range\n");
        return 2;
    }

    /* Same callsigns as platform/sweep_tx.c. */
    if (ax25_create_hdr(&hdr, (const uint8_t *)"CQ", 0,
                        (const uint8_t *)"NOCALL", 0) != AX25_OK) {
        return 1;
    }

    for (chunk = 0; chunk < SWEEP_PACKET_CHUNKS; ++chunk) {
        uint8_t info_len = SweepPacket_Build((uint16_t)sweep_id, chunk,
                                             tether_sim_sweeps[index], info);
        int8_t len = ax25_encode(&hdr, info, info_len, frame, sizeof(frame));
        if (info_len == 0 || len <= 0) {
            return 1;
        }
        for (int8_t i = 0; i < len; ++i) {
            printf("%02x", frame[i]);
        }
        printf("\n");
    }
    return 0;
}
