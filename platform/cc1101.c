#include "HAL_SPI.h"
#include "HAL_clock.h"
#include "cc1101.h"

/* ---------------------------------------------------------------------
 * Register image, addresses 0x00..0x2E (burst-written in one go).
 *
 * Values assume a 26 MHz crystal. Derived from the datasheet formulas:
 *   FREQ     = 433.92e6 * 2^16 / 26e6      = 1093745 = 0x10B071
 *   DRATE    = (256+M) * 2^E * 26e6 / 2^28 -> E=5, M=67 -> 1001 baud
 *   DEVIATN  = 26e6/2^17 * (8+M) * 2^E     -> E=4, M=0  -> 25.4 kHz
 *   RX BW    = 26e6 / (8*(4+M)*2^E)        -> E=3, M=0  -> 101.6 kHz
 * RX-side tuning (AGC, FOCCFG, BSCFG, FSCAL, TEST) is carried over from
 * the ELECHOUSE defaults; review it against SmartRF Studio when RX is
 * brought up.
 * ------------------------------------------------------------------- */
const uint8_t cc1101_config[CC1101_CONFIG_LEN] =
{
    0x29,   /* 0x00 IOCFG2   GDO2: CHIP_RDYn (reset default)            */
    0x2E,   /* 0x01 IOCFG1   GDO1: high impedance                       */
    0x06,   /* 0x02 IOCFG0   GDO0: high from sync sent to end of packet */
    0x07,   /* 0x03 FIFOTHR  reset default                              */
    0xD3,   /* 0x04 SYNC1    sync word high byte                        */
    0x91,   /* 0x05 SYNC0    sync word low byte                         */
    0x3D,   /* 0x06 PKTLEN   max packet length 61 bytes                 */
    0x04,   /* 0x07 PKTCTRL1 append RSSI/LQI status, no address check   */
    0x05,   /* 0x08 PKTCTRL0 CRC on, variable packet length             */
    0x00,   /* 0x09 ADDR                                                */
    0x00,   /* 0x0A CHANNR   channel 0                                  */
    0x06,   /* 0x0B FSCTRL1  IF frequency                               */
    0x00,   /* 0x0C FSCTRL0                                             */
    0x10,   /* 0x0D FREQ2    433.92 MHz                                 */
    0xB0,   /* 0x0E FREQ1                                               */
    0x71,   /* 0x0F FREQ0                                               */
    0xC5,   /* 0x10 MDMCFG4  RX BW 101.6 kHz, DRATE_E = 5               */
    0x43,   /* 0x11 MDMCFG3  DRATE_M = 67 -> ~1 kbaud                   */
    0x02,   /* 0x12 MDMCFG2  2-FSK, DC filter on, 16/16 sync detect     */
    0x02,   /* 0x13 MDMCFG1  FEC off, 2 preamble bytes, CHANSPC_E = 2   */
    0xF8,   /* 0x14 MDMCFG0  CHANSPC_M -> ~200 kHz channel spacing      */
    0x40,   /* 0x15 DEVIATN  E=4, M=0 -> 25.4 kHz                       */
    0x07,   /* 0x16 MCSM2    reset default                              */
    0x30,   /* 0x17 MCSM1    return to IDLE after TX and RX             */
    0x18,   /* 0x18 MCSM0    auto-calibrate on IDLE -> RX/TX            */
    0x16,   /* 0x19 FOCCFG                                              */
    0x1C,   /* 0x1A BSCFG                                               */
    0xC7,   /* 0x1B AGCCTRL2                                            */
    0x00,   /* 0x1C AGCCTRL1                                            */
    0xB2,   /* 0x1D AGCCTRL0                                            */
    0x87,   /* 0x1E WOREVT1  reset default                              */
    0x6B,   /* 0x1F WOREVT0  reset default                              */
    0xFB,   /* 0x20 WORCTRL  reset default                              */
    0x56,   /* 0x21 FREND1                                              */
    0x10,   /* 0x22 FREND0   PA_POWER = 0 -> uses PATABLE[0]            */
    0xE9,   /* 0x23 FSCAL3                                              */
    0x2A,   /* 0x24 FSCAL2                                              */
    0x00,   /* 0x25 FSCAL1                                              */
    0x1F,   /* 0x26 FSCAL0                                              */
    0x41,   /* 0x27 RCCTRL1  reset default                              */
    0x00,   /* 0x28 RCCTRL0  reset default                              */
    0x59,   /* 0x29 FSTEST                                              */
    0x7F,   /* 0x2A PTEST    reset default                              */
    0x3F,   /* 0x2B AGCTEST  reset default                              */
    0x81,   /* 0x2C TEST2                                               */
    0x35,   /* 0x2D TEST1                                               */
    0x09    /* 0x2E TEST0                                               */
};

/* PA settings for the 433 MHz band, from the datasheet table:
 * -30, -20, -15, -10, 0, +5, +7, +10 dBm */
static const uint8_t pa_table_433[8] =
    { 0x12, 0x0E, 0x1D, 0x34, 0x60, 0x84, 0xC8, 0xC0 };

uint8_t CC1101_Init(void)
{
    uint8_t err = HAL_SPI_ResetCC1101();
    uint8_t version;

    if (err != 0)
    {
        return err;
    }

    /* The reset handshake only waits for MISO to read low, so a MISO wire
     * that is disconnected-and-low, or a CC1101 without power, also
     * "passes". A real chip reports a non-zero VERSION (TI documents 0x04
     * and 0x14; some modules report others), while a dead SPI line reads
     * every register as 0x00 (stuck low) or 0xFF (stuck high/timeout). So
     * only those two values are rejected. */
    version = HAL_SPI_ReadStatus(CC1101_VERSION);
    if (version == 0x00U || version == 0xFFU)
    {
        return CC1101_INIT_NO_CHIP;
    }

    HAL_SPI_WriteBurstReg(CC1101_CONFIG_START, cc1101_config, CC1101_CONFIG_LEN);
    CC1101_SetPA(10);

    return 0;
}

void CC1101_SetChannel(uint8_t channel)
{
    HAL_SPI_WriteReg(CC1101_CHANNR, channel);
}

void CC1101_SetPA(int8_t dbm)
{
    uint8_t pa[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t idx;

    if      (dbm <= -30) idx = 0;
    else if (dbm <= -20) idx = 1;
    else if (dbm <= -15) idx = 2;
    else if (dbm <= -10) idx = 3;
    else if (dbm <=   0) idx = 4;
    else if (dbm <=   5) idx = 5;
    else if (dbm <=   7) idx = 6;
    else                 idx = 7;

    /* FSK/GFSK reads PATABLE[0] (FREND0.PA_POWER = 0), so the chosen
     * value goes in the first byte and the rest stay zero. */
    pa[0] = pa_table_433[idx];
    HAL_SPI_WriteBurstReg(CC1101_PATABLE, pa, 8);
}

void CC1101_SetTx(void)
{
    HAL_SPI_Strobe(CC1101_SIDLE);
    HAL_SPI_Strobe(CC1101_STX);
}

void CC1101_SetIdle(void)
{
    HAL_SPI_Strobe(CC1101_SIDLE);
}

/* Longest packet is ~66 bytes on air (preamble, sync, length, 61 payload,
 * CRC), about 530 ms at ~1 kbaud. 1.5 s leaves plenty of margin. */
#define CC1101_SEND_TIMEOUT_MS  1500U

/* Bytes the radio adds around the payload: 2 preamble + 2 sync +
 * 1 length + 2 CRC (cc1101_config: MDMCFG1, MDMCFG2, PKTCTRL0). */
#define CC1101_PACKET_OVERHEAD_BYTES  7U

/* The data rate is ~1001 baud, so one bit takes just under 1 ms. Counting
 * 1 ms per bit slightly overestimates the airtime, which is what we want. */
#define CC1101_MS_PER_BIT             1U

/* Calibration before TX (~0.8 ms) plus a little slack. */
#define CC1101_TX_START_MARGIN_MS     5U

/* DEBUG: MARCSTATE seen just after STX in the last CC1101_SendPacket().
 * 0x13 = TX. Watch it in CCS's Expressions view. */
volatile uint8_t g_cc1101_state_after_stx;

/* How many times to load the TX FIFO before giving up on a bad byte count. */
#define CC1101_LOAD_ATTEMPTS          3U

/* Status registers (MARCSTATE, TXBYTES, RXBYTES) can be misread if they
 * change during the SPI read (CC1101 errata, "SPI read synchronization").
 * TI's workaround is to read until two reads in a row agree. */
static uint8_t read_status_stable(uint8_t addr)
{
    uint8_t first;
    uint8_t second = HAL_SPI_ReadStatus(addr);

    do
    {
        first  = second;
        second = HAL_SPI_ReadStatus(addr);
    } while (first != second);

    return second;
}

static uint8_t read_marcstate(void)
{
    return read_status_stable(CC1101_MARCSTATE) & 0x1FU;
}

/* Loads [len, payload...] into an empty TX FIFO in ONE burst, then checks
 * the FIFO holds exactly len + 1 bytes. Returns 1 when it does. */
static uint8_t load_tx_fifo(const uint8_t *payload, uint8_t len)
{
    uint8_t buf[1U + CC1101_MAX_PAYLOAD];
    uint8_t attempt;
    uint8_t i;

    buf[0] = len;                       /* variable-length mode: length first */
    for (i = 0U; i < len; ++i)
    {
        buf[1U + i] = payload[i];
    }

    for (attempt = 0U; attempt < CC1101_LOAD_ATTEMPTS; ++attempt)
    {
        HAL_SPI_Strobe(CC1101_SIDLE);
        HAL_SPI_Strobe(CC1101_SFTX);    /* start from an empty FIFO */
        HAL_SPI_WriteBurstReg(CC1101_TXFIFO, buf, (uint8_t)(len + 1U));

        if (read_status_stable(CC1101_TXBYTES) == (uint8_t)(len + 1U))
        {
            return 1U;                  /* right count, no underflow flag */
        }
    }
    return 0U;
}

uint8_t CC1101_SendPacket(const uint8_t *payload, uint8_t len)
{
    uint16_t waited_ms;
    uint16_t airtime_ms;
    uint8_t  state;

    if (payload == 0 || len == 0U || len > CC1101_MAX_PAYLOAD)
    {
        return CC1101_SEND_BAD_LEN;
    }

    if (!load_tx_fifo(payload, len))
    {
        HAL_SPI_Strobe(CC1101_SIDLE);
        HAL_SPI_Strobe(CC1101_SFTX);
        return CC1101_SEND_LOAD_ERROR;
    }

    CC1101_SetTx();

    /* Confirm the radio really started transmitting. Calibration takes
     * ~0.8 ms, so after the margin it must be in TX. Without this check a
     * radio that ignored STX would sit in IDLE and look like "sent OK". */
    HAL_DELAY_MS(CC1101_TX_START_MARGIN_MS);
    state = read_marcstate();
    g_cc1101_state_after_stx = state;
    if (state != CC1101_MARC_TX)
    {
        HAL_SPI_Strobe(CC1101_SIDLE);
        HAL_SPI_Strobe(CC1101_SFTX);
        return CC1101_SEND_NO_TX;
    }

    /* Keep the SPI bus quiet while the packet is on air: TI advises against
     * SPI activity during TX. Wait out the expected airtime, then poll. */
    airtime_ms = (uint16_t)((len + CC1101_PACKET_OVERHEAD_BYTES) * 8U * CC1101_MS_PER_BIT);
    for (waited_ms = CC1101_TX_START_MARGIN_MS; waited_ms < airtime_ms; ++waited_ms)
    {
        HAL_DELAY_MS(1);
    }

    for (; waited_ms < CC1101_SEND_TIMEOUT_MS; ++waited_ms)
    {
        state = read_marcstate();

        if (state == CC1101_MARC_IDLE)
        {
            /* A good packet empties the FIFO. Anything left means the radio
             * read a smaller length than we wrote: the packet went out short
             * (and garbled), so the caller should send it again. */
            if ((read_status_stable(CC1101_TXBYTES) & 0x7FU) != 0U)
            {
                HAL_SPI_Strobe(CC1101_SFTX);
                return CC1101_SEND_LEFTOVER;
            }
            return CC1101_SEND_OK;
        }
        if (state == CC1101_MARC_TXFIFO_UNDERFLOW)
        {
            HAL_SPI_Strobe(CC1101_SFTX);   /* flushing also returns to IDLE */
            return CC1101_SEND_UNDERFLOW;
        }
        HAL_DELAY_MS(1);
    }

    HAL_SPI_Strobe(CC1101_SIDLE);
    HAL_SPI_Strobe(CC1101_SFTX);
    return CC1101_SEND_TIMEOUT;
}

/* ---------------------------------------------------------------------
 * Receive (used for the uplink "GO" command)
 * ------------------------------------------------------------------- */

/* RXBYTES changes while a packet arrives, so it gets the same
 * read-until-two-reads-agree treatment as MARCSTATE. */
static uint8_t read_rxbytes(void)
{
    return read_status_stable(CC1101_RXBYTES);
}

static void rx_stop_and_flush(void)
{
    HAL_SPI_Strobe(CC1101_SIDLE);
    HAL_SPI_Strobe(CC1101_SFRX);   /* only valid in IDLE or RX overflow */
}

void CC1101_StartRx(void)
{
    rx_stop_and_flush();
    HAL_SPI_Strobe(CC1101_SRX);
}

uint8_t CC1101_PollPacket(uint8_t *buf, uint8_t max, uint8_t *len)
{
    uint8_t state;
    uint8_t rxbytes;
    uint8_t pkt_len;
    uint8_t status[2];

    if (buf == 0 || len == 0)
    {
        return CC1101_RX_ERROR;
    }
    *len = 0U;

    state = read_marcstate();
    /* 0x03-0x0C: calibrating / settling into RX. 0x0D: RX (listening).
     * 0x0E-0x0F: RX_END / RX_RST, brief states while a packet finishes. */
    if (state >= 0x03U && state <= 0x0FU)
    {
        return CC1101_RX_NONE;
    }

    if (state != CC1101_MARC_IDLE)
    {
        rx_stop_and_flush();           /* e.g. RX FIFO overflow (0x11) */
        return CC1101_RX_ERROR;
    }

    /* IDLE: a complete packet should be waiting in the FIFO as
     * length byte + payload + 2 status bytes. */
    rxbytes = read_rxbytes();
    if ((rxbytes & 0x80U) != 0U || (rxbytes & 0x7FU) == 0U)
    {
        rx_stop_and_flush();
        return CC1101_RX_ERROR;
    }

    pkt_len = HAL_SPI_ReadReg(CC1101_RXFIFO);   /* single read pops 1 byte */
    if (pkt_len == 0U || pkt_len > max || (rxbytes & 0x7FU) != (uint8_t)(pkt_len + 3U))
    {
        rx_stop_and_flush();
        return CC1101_RX_ERROR;
    }

    HAL_SPI_ReadBurstReg(CC1101_RXFIFO, buf, pkt_len);
    HAL_SPI_ReadBurstReg(CC1101_RXFIFO, status, 2U);  /* RSSI, LQI|CRC_OK */
    rx_stop_and_flush();

    *len = pkt_len;
    return ((status[1] & CC1101_LQI_CRC_OK) != 0U) ? CC1101_RX_OK : CC1101_RX_BAD_CRC;
}
