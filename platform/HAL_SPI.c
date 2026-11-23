#include "HAL_SPI.h"
#include "HAL_clock.h"
#include "cc1101_regs.h"

void HAL_SPI_Init(void)
{
    /* --- Pin muxing: CLK/MOSI/MISO to eUSCI_B1 peripheral function --- */
    GPIO_setAsPeripheralModuleFunctionInputPin(
        SPI_PORT, SPI_SIMO_PIN | SPI_SOMI_PIN | SPI_CLK_PIN,
        GPIO_PRIMARY_MODULE_FUNCTION);

    /* --- CS: plain GPIO output, idle high (deasserted) --- */
    GPIO_setAsOutputPin(CS_PORT, CS_PIN);
    GPIO_setOutputHighOnPin(CS_PORT, CS_PIN);

    /* --- GDO0 / GDO2: plain GPIO inputs --- */
    GPIO_setAsInputPin(GDO0_PORT, GDO0_PIN);
    GPIO_setAsInputPin(GDO2_PORT, GDO2_PIN);

    /* --- FRAM device: unlock GPIOs from POR high-impedance state --- */
    PMM_unlockLPM5();

    /* --- eUSCI_B1 SPI config: mode 0 (CPOL=0, CPHA=0), MSB first,
     * master, 3-wire (CS handled manually, not eUSCI STE), SMCLK
     * source, divided down to SPI_TARGET_HZ. --- */
    EUSCI_B_SPI_initMasterParam param = {0};
    param.selectClockSource   = EUSCI_B_SPI_CLOCKSOURCE_SMCLK;
    param.clockSourceFrequency = HAL_CLOCK_SMCLK_HZ;
    param.desiredSpiClock      = SPI_TARGET_HZ;
    param.msbFirst             = EUSCI_B_SPI_MSB_FIRST;
    param.clockPhase           = EUSCI_B_SPI_PHASE_DATA_CAPTURED_ONFIRST_CHANGED_ON_NEXT;
    param.clockPolarity        = EUSCI_B_SPI_CLOCKPOLARITY_INACTIVITY_LOW;
    param.spiMode               = EUSCI_B_SPI_3PIN;

    EUSCI_B_SPI_initMaster(SPI_EUSCI_BASE, &param);
    EUSCI_B_SPI_enable(SPI_EUSCI_BASE);
}

void HAL_SPI_CS_Assert(void)
{
    GPIO_setOutputLowOnPin(CS_PORT, CS_PIN);   /* active low */
}

void HAL_SPI_CS_Deassert(void)
{
    GPIO_setOutputHighOnPin(CS_PORT, CS_PIN);
}

/* ---------------------------------------------------------------------
 * Raw single-byte SPI transfer - internal only, not declared in the
 * header. Used by WriteReg/ReadReg/burst variants/Strobe (Day 3).
 * Kept here now so the pattern functions have something to build on
 * without redefining this per-call.
 * ------------------------------------------------------------------- */
uint8_t SPI_TransferByte(uint8_t data)
{
    /* Clear any leftover "byte received" flag, so the wait below can only
     * end on THIS byte. Reading RXBUF clears RXIFG. */
    if (EUSCI_B_SPI_getInterruptStatus(SPI_EUSCI_BASE, EUSCI_B_SPI_RECEIVE_INTERRUPT))
    {
        (void)EUSCI_B_SPI_receiveData(SPI_EUSCI_BASE);
    }

    while (!EUSCI_B_SPI_getInterruptStatus(SPI_EUSCI_BASE, EUSCI_B_SPI_TRANSMIT_INTERRUPT));
    EUSCI_B_SPI_transmitData(SPI_EUSCI_BASE, data);

    /* Wait until the byte has been fully clocked out. In SPI the last bit
     * is received at the same clock edge the last bit is sent, so RXIFG
     * means "this byte is completely done".
     *
     * Do NOT poll UCBUSY here instead: it is not set immediately after
     * TXBUF is written. At 8 MHz the CPU could read it as "not busy"
     * before the transfer had started, return early, and the caller would
     * raise CS while the last byte was still shifting out, so the CC1101
     * dropped it (seen as TEST0 reading back 0x0B instead of 0x09). */
    while (!EUSCI_B_SPI_getInterruptStatus(SPI_EUSCI_BASE, EUSCI_B_SPI_RECEIVE_INTERRUPT));
    return EUSCI_B_SPI_receiveData(SPI_EUSCI_BASE);
}

/* ---------------------------------------------------------------------
 * Day 3: handshake, reset, single-register access, strobes
 * ------------------------------------------------------------------- */

/* Bounded wait so a wiring fault fails visibly instead of hanging.
 * Each poll is a DriverLib call (tens of cycles), so 20000 polls is still
 * several ms at 8 MHz, far longer than the CC1101's ~150 us oscillator
 * start-up. */
#define WAIT_READY_TIMEOUT   20000UL
#define HAL_SPI_ERR_VALUE    0xFF

/* Microsecond busy-wait, scaled to the CPU clock (see HAL_clock.h). */
#define DELAY_US(us)         HAL_DELAY_US(us)

/* MISO is read through the port input register while muxed to the eUSCI.
 * If the handshake times out with the chip correctly wired, suspect this. */
uint8_t HAL_SPI_WaitReady(void)
{
    uint32_t n = WAIT_READY_TIMEOUT;

    while (GPIO_getInputPinValue(SPI_PORT, SPI_SOMI_PIN) == GPIO_INPUT_PIN_HIGH)
    {
        if (--n == 0)
        {
            return 1;   /* timeout */
        }
    }
    return 0;
}

uint8_t HAL_SPI_ResetCC1101(void)
{
    uint8_t err = 0;

    /* Power-on-reset style sequence from the CC1101 manual: CS pulse,
     * hold CS high >= 40 us, then CS low, wait for MISO low, SRES strobe,
     * wait for MISO low again (reset finished). */
    HAL_SPI_CS_Deassert();
    DELAY_US(10);
    HAL_SPI_CS_Assert();
    DELAY_US(10);
    HAL_SPI_CS_Deassert();
    DELAY_US(100);

    HAL_SPI_CS_Assert();
    err = HAL_SPI_WaitReady();
    if (err == 0)
    {
        SPI_TransferByte(CC1101_SRES);
        err = HAL_SPI_WaitReady();
    }
    HAL_SPI_CS_Deassert();

    return err;
}

void HAL_SPI_WriteReg(uint8_t addr, uint8_t value)
{
    HAL_SPI_CS_Assert();
    if (HAL_SPI_WaitReady() == 0)
    {
        SPI_TransferByte(addr);          /* R/W = 0, burst = 0 */
        SPI_TransferByte(value);
    }
    HAL_SPI_CS_Deassert();
}

uint8_t HAL_SPI_ReadReg(uint8_t addr)
{
    uint8_t value = HAL_SPI_ERR_VALUE;

    HAL_SPI_CS_Assert();
    if (HAL_SPI_WaitReady() == 0)
    {
        SPI_TransferByte(addr | CC1101_READ_SINGLE);
        value = SPI_TransferByte(0x00);  /* dummy byte clocks the data out */
    }
    HAL_SPI_CS_Deassert();
    return value;
}

uint8_t HAL_SPI_ReadStatus(uint8_t addr)
{
    uint8_t value = HAL_SPI_ERR_VALUE;

    HAL_SPI_CS_Assert();
    if (HAL_SPI_WaitReady() == 0)
    {
        SPI_TransferByte(addr | CC1101_READ_BURST);   /* burst bit selects status regs */
        value = SPI_TransferByte(0x00);
    }
    HAL_SPI_CS_Deassert();
    return value;
}

uint8_t HAL_SPI_Strobe(uint8_t strobe)
{
    uint8_t status = HAL_SPI_ERR_VALUE;

    HAL_SPI_CS_Assert();
    if (HAL_SPI_WaitReady() == 0)
    {
        status = SPI_TransferByte(strobe);   /* chip status byte comes back */
    }
    HAL_SPI_CS_Deassert();
    return status;
}

/* ---------------------------------------------------------------------
 * Day 4: burst access
 * ------------------------------------------------------------------- */

/* DEBUG (SPI corruption hunt): idle time between bytes of a burst write,
 * in microseconds. 0 = back to back (normal). Try e.g. 20 to see whether
 * long FIFO writes stop corrupting when the bytes are spaced out. */
#define HAL_SPI_BURST_GAP_US   0

void HAL_SPI_WriteBurstReg(uint8_t addr, const uint8_t *buffer, uint8_t len)
{
    uint8_t i;

    HAL_SPI_CS_Assert();
    if (HAL_SPI_WaitReady() == 0)
    {
        SPI_TransferByte(addr | CC1101_WRITE_BURST);
        for (i = 0; i < len; i++)
        {
#if HAL_SPI_BURST_GAP_US > 0
            DELAY_US(HAL_SPI_BURST_GAP_US);
#endif
            SPI_TransferByte(buffer[i]);
        }
    }
    HAL_SPI_CS_Deassert();
}

void HAL_SPI_ReadBurstReg(uint8_t addr, uint8_t *buffer, uint8_t len)
{
    uint8_t i;

    /* On a handshake timeout the caller sees 0xFF everywhere. */
    for (i = 0; i < len; i++)
    {
        buffer[i] = HAL_SPI_ERR_VALUE;
    }

    HAL_SPI_CS_Assert();
    if (HAL_SPI_WaitReady() == 0)
    {
        SPI_TransferByte(addr | CC1101_READ_BURST);
        for (i = 0; i < len; i++)
        {
            buffer[i] = SPI_TransferByte(0x00);
        }
    }
    HAL_SPI_CS_Deassert();
}