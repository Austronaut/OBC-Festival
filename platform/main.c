/**
 * @file main.c
 * @brief Synchronous tether demo integrated with the existing OBC HAL.
 *
 * Add tether.c/tether.h to platform/ and build both .c files in CCS.
 * Existing HAL_Clock_Init() and HAL_SPI_Init() still own clock and SPI.
 */

#include <driverlib.h>

#include "HAL_clock.h"
#include "HAL_SPI.h"
#include "tether.h"

#define TEST_BYTE 0xAA

int main(void)
{
    WDT_A_hold(WDT_A_BASE); // disables the watchdog timer

    // Initialisation functions. Should happen in this order:
    /* 1 MHz clocks -> shared eUSCI_B1 SPI -> tether. */
    HAL_Clock_Init();           // inits 1 MHz clocks.
    HAL_SPI_Init();             // sets up the SPI and also unlocks GPIO LOCKLPM5.
    Tether_Init();              // DAC A = 0 V; B = 2 V; motor off. */

    while (1)
    {
        /*
         * Tether_DemoCycle is a blocking combined test. It:
         *   - DAC A: repeated 0-4 V sweeps -- that's the voltage sweep
         *   - DAC B: constant ~2 V -- needed for another component on the payloads side
         *   - motor: 200 full steps uncoil, pause, 200 coil, pause
         *
         * Returns only after a complete forward/backward demo cycle.
         */
        // Tether_DemoCycle();

        Tether_Coil(200U);
        Tether_Uncoil(200U);

        /* Alternative API calls:
         *
         * Tether_SweepVoltageA();  // One blocking DAC A sweep
         * Tether_Uncoil(200U);     // One nominal motor revolution (200 steps)
         * Tether_Coil(200U);       // One nominal motor revolution back (200 steps)
         * Tether_SetVoltageA_mV(1500U); // to manually control the voltages
         * Tether_SetVoltageB_mV(2000U);
         * Tether_DisableMotor();   // Releases holding torque
         */
    }
}
