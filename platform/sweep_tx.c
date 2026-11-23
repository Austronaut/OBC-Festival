#include "sweep_tx.h"

#include <stdint.h>

#include "HAL_clock.h"
#include "ax25.h"
#include "cc1101.h"
#include "sweep_packet.h"

/* AX.25 addressing. TODO: set SRC_CALL to a licensed callsign before
 * transmitting for real. */
#define SWEEP_TX_DST_CALL  "CQ"
#define SWEEP_TX_DST_SSID  0U
#define SWEEP_TX_SRC_CALL  "NOCALL"
#define SWEEP_TX_SRC_SSID  0U

/* Quiet time between chunks, so the ground decoder sees separate bursts. */
#define SWEEP_TX_GAP_MS    50U

/* Tries per chunk. CC1101_SendPacket() detects some failures itself (wrong
 * FIFO byte count, underflow, short packet, radio not in TX); those chunks
 * are sent again instead of abandoning the rest of the sweep. */
#define SWEEP_TX_ATTEMPTS  3U

/* DEBUG: chunks that had to be re-sent, since boot. */
volatile uint16_t g_sweep_tx_retries;

/* A full frame must fit in one CC1101 packet. */
#if (AX25_OVERHEAD + SWEEP_PACKET_MAX_LEN) > CC1101_MAX_PAYLOAD
#error "Sweep chunk plus AX.25 overhead does not fit in one CC1101 packet"
#endif

static ax25_hdr s_hdr;
static uint8_t  s_ready;
static SweepTx_EventHandler s_on_event;   /* NULL = nobody listening */

static void notify(uint8_t event)
{
    if (s_on_event != 0)
    {
        s_on_event(event);   /* call through the function pointer */
    }
}

void SweepTx_SetEventHandler(SweepTx_EventHandler handler)
{
    s_on_event = handler;
}

int8_t SweepTx_Init(void)
{
    int8_t err = ax25_create_hdr(&s_hdr,
                                 (const uint8_t *)SWEEP_TX_DST_CALL, SWEEP_TX_DST_SSID,
                                 (const uint8_t *)SWEEP_TX_SRC_CALL, SWEEP_TX_SRC_SSID);

    s_ready = (err == AX25_OK) ? 1U : 0U;
    return err;
}

/* Sends one frame, retrying failures the radio driver can detect. A bad
 * length is a bug, not a transient fault, so it is not retried. */
static uint8_t send_with_retries(const uint8_t *frame, uint8_t len)
{
    uint8_t attempt;
    uint8_t err = CC1101_SEND_OK;

    for (attempt = 0U; attempt < SWEEP_TX_ATTEMPTS; ++attempt)
    {
        if (attempt != 0U)
        {
            ++g_sweep_tx_retries;
            notify(SWEEP_TX_EVENT_RETRY);
            HAL_DELAY_MS(SWEEP_TX_GAP_MS);
        }
        err = CC1101_SendPacket(frame, len);
        if (err == CC1101_SEND_OK)
        {
            notify(SWEEP_TX_EVENT_SENT);
            break;
        }
        if (err == CC1101_SEND_BAD_LEN)
        {
            break;
        }
    }
    return err;
}

uint8_t SweepTx_Send(uint16_t sweep_id, const int32_t *sweep)
{
    uint8_t info[SWEEP_PACKET_MAX_LEN];
    uint8_t frame[AX25_OVERHEAD + SWEEP_PACKET_MAX_LEN];
    uint8_t chunk;
    uint8_t info_len;
    int8_t  frame_len;
    uint8_t err;

    if (s_ready == 0U || sweep == 0)
    {
        return SWEEP_TX_NOT_INIT;
    }

    for (chunk = 0U; chunk < SWEEP_PACKET_CHUNKS; ++chunk)
    {
        info_len = SweepPacket_Build(sweep_id, chunk, sweep, info);

        /* Frames are at most 59 bytes, so ax25_encode's int8_t length
         * return cannot overflow here. Negative means an AX25_ERR_*. */
        frame_len = ax25_encode(&s_hdr, info, info_len, frame, sizeof(frame));
        if (info_len == 0U || frame_len <= 0)
        {
            return SWEEP_TX_ENCODE_ERROR;
        }

        err = send_with_retries(frame, (uint8_t)frame_len);
        if (err != CC1101_SEND_OK)
        {
            return err;          /* still failing after all attempts */
        }

        HAL_DELAY_MS(SWEEP_TX_GAP_MS);
    }

    return SWEEP_TX_OK;
}
