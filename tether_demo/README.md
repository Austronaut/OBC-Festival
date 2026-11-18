# Tether Subsystem — MSP430 Bench Demo

**Status:** Integrated bench prototype; not flight-ready  
**Last updated:** 17 November 2026  
**Target:** MSP-EXP430FR5994 LaunchPad, MCP4822-E/P dual DAC, A4988 driver, NEMA-8 bipolar stepper motor  
**Purpose:** Demonstrate independent, simultaneous DAC output and stepper operation for Phase 2 of tether integration.

This folder contains a cleaned-up version of the successful standalone tether demo. It preserves the same hardware pin assignments and DAC behaviour, uses the OBC team's 8 MHz clock configuration, and moves motion scheduling onto a 1 ms Timer_A interrupt. It also contains an **optional** two-button command interface for demonstrations without a radio uplink.

> **Verification status:** This is a refactor of the earlier bench-working code. The earlier combined setup was observed working, but **this particular version has not yet been flashed and validated on the physical board**. Treat it as a test candidate, not a certified working release.

## 1. Files

```text
tether_demo/
├── README.md
└── platform/
    ├── main.c              # Bench application, motor/DAC scheduler, optional buttons
    └── hal/
        ├── hal_clock.h     # OBC team's nominal 8 MHz clock API
        └── hal_clock.c     # Clock init adapted from the shared OBC version
```

`hal_clock.c/.h` are included to make this demo self-contained. **In the main OBC repository, prefer the team's maintained copies** rather than adding duplicates. Other register-level code remains in `main.c` for the prototype and is **not** presented as a completed HAL.

## 2. Intended behaviour

| Output / subsystem | Behaviour |
| --- | --- |
| MCP4822 channel A | Repeating 0–4.000 V sawtooth ramp, nominally ~1 second per sweep |
| MCP4822 channel B | Fixed ~2.000 V, refreshed every 100 ms |
| A4988 / stepper | Full-step mode, 200 pulses/revolution |
| Motor speed | 10 ms per STEP rising edge = 100 commanded steps/sec = 0.5 rev/s while moving |
| Direction | 200 steps forward, 250 ms holding pause, 200 steps backward, repeat |
| Timebase | Timer_A0 CCR0 interrupt every 1 ms, driven by nominal 8 MHz SMCLK |
| Boot mode (default) | Start sweep and motor automatically |
| Button mode (optional) | Wait for commands entered with LaunchPad S1/S2 |

The 250 ms reversal pause means the **whole forward/back cycle is longer than four seconds**. The stated 0.5 rev/s is the stepping speed while the motor is moving, not a promise of the average cycle speed. The motor is open-loop, so 200 commanded steps **do not prove** one physical revolution occurred.

## 3. Wiring

All grounds must be common. Keep the motor supply separate from the LaunchPad's low-voltage power.

### MSP430 ↔ MCP4822

| MSP430 | MCP4822 PDIP pin | Signal |
| --- | --- | --- |
| P5.0 | 4 | SDI (SPI MOSI) |
| P5.2 | 3 | SCK |
| P3.7 | 2 | CS, active LOW |
| +5 V | 1 | VDD |
| GND | 7 | VSS |
| GND | 5 | LDAC, tied LOW |
| External probe | 8 | VOUTA, 0–4 V ramp |
| External probe | 6 | VOUTB, ~2 V |

Place a 0.1 µF ceramic decoupling capacitor close to the DAC supply pins; adding a 10 µF local capacitor is useful. Measure VOUTA and VOUTB **relative to DAC VSS (pin 7)**, not relative to each other.

**Logic-level concern:** MCP4822 VDD = 5 V with 3.3 V MSP430 SPI signals was observed working in the prototype, but the DAC's guaranteed digital HIGH input threshold is 0.7 × VDD (3.5 V at VDD = 5 V). A suitable 3.3-to-5 V SPI/CS buffer (for example, an AHCT-family buffer powered at 5 V) should be assessed before relying on this interface.

### MSP430 ↔ A4988

| MSP430 / supply | A4988 signal | Notes |
| --- | --- | --- |
| P3.3 | MS1 + MS2 + MS3 | Connected together; LOW = full-step |
| P3.4 | STEP | Driven as a 1 ms HIGH pulse by Timer_A software ISR |
| P3.5 | DIR | Direction |
| P3.6 | ENABLE | Active LOW |
| P7.3 | SLEEP | Active LOW |
| +3.3 V | VDD | Logic supply |
| +3.3 V | RESET | Held HIGH (do not leave floating) |
| GND | Logic GND | Common ground |
| External motor supply + (e.g. 12 V) | VMOT | Never connect to LaunchPad +5 V |
| External supply − | Motor GND | Common ground |

Place a **47–100 µF electrolytic capacitor** across VMOT and motor ground **near the A4988** (observe polarity, use an appropriate voltage rating). Connect the correct winding pairs to the driver's 1A/1B and 2A/2B terminals. For the previously used Pololu NEMA-8, black/green are one coil and red/blue are the other; verify the exact motor before connecting.

Set and verify the A4988 current limit for the specific board's sense resistors **before operating the motor**. Do not connect/disconnect the stepper while the motor supply is on. Secure/solder the A4988 headers; the earlier prototype required physical pressure on the driver, which makes repeatability tests unreliable.

### Optional LaunchPad controls

No extra breadboard buttons are necessary at first:

| On-board control | MSP430 pin | Function |
| --- | --- | --- |
| S1 | P5.6 | Start/finish entry |
| S2 | P5.5 | Add one to command count |
| Red LED1 | P1.0 | Blinks while receiving; solid briefly on rejection |
| Green LED2 | P1.1 | Lights briefly on accepted command |

Both buttons are active LOW, using input pull-ups. Check that the LaunchPad's LED1 jumper (J7) is fitted if the red LED does not light. External normally-open buttons can later be wired between the corresponding GPIO pins and GND.

## 4. Software design

```text
  8 MHz DCO
      |
      +---- MCLK --> CPU (main loop / command processing)
      |
      +---- SMCLK --> Timer_A0 (1 ms ticks)
      |                    |
      |                    +--> STEP edge and DIR scheduling
      |                    +--> DAC A next value + pending flag
      |                    +--> uptime counter
      |
      +---- SMCLK --> eUSCI_B1 SPI (~500 kHz)
                                |
                              MCP4822

  main loop:
      - snapshots pending DAC A value and writes via SPI
      - refreshes DAC B every ~100 ms
      - optionally polls/debounces buttons and dispatches commands
```

**Why Timer_A?** The timer counts peripheral clock pulses independently of CPU instruction timing. The old approach delayed inside the main loop, making motor speed depend on SPI traffic and other work. Here the Timer_A0 ISR issues STEP edges. DAC SPI transfers happen **outside the ISR**, keeping interrupt work short.

**Clock calculation:**

```text
SMCLK = 8,000,000 Hz
Timer_A CCR0 = (8,000,000 / 1,000) - 1 = 7,999
=> timer tick every ~1 ms

200 full steps/rev * 10 ms/step = 2 seconds/rev
=> nominal 0.5 rev/s while moving

DAC A: +4 mV/tick, from 0 to 4000 mV, then reset
=> approximately one 0–4 V sweep per second
```

The actual timing retains the oscillator's accuracy limits and interrupt latency. DAC A writes can be skipped if the CPU cannot service every millisecond, but the motor timing is not gated by SPI transactions. A more deterministic future motor implementation could use a timer output-compare channel to generate STEP edges in hardware.

### Initialisation sequence

1. Stop watchdog; call `hal_clock_init()` (MCLK/SMCLK = nominal 8 MHz).
2. Configure GPIO output latches and SPI peripheral; unlock GPIO with `LOCKLPM5` clear.
3. Allow ~100 ms after startup before the **first** MCP4822 write. This preserves a workaround established during bench testing.
4. Initialise DAC A = 0 V and B = 2 V.
5. Start Timer_A0 and enable interrupts.
6. In the default mode, start DAC sweep and wake/enable the stepper. In button mode, wait for a command.

The motor is not stepped during DAC startup. Once started, the ISR waits an additional 2 ms before counting towards the first STEP edge. **This avoids the earlier bug where the software counted steps while the driver was asleep.**

### Motor stepping

At each 1 ms ISR:

- If STEP was HIGH in the previous tick, set it LOW (1 ms HIGH pulse, then at least 9 ms LOW at cruise).
- Wait for any A4988 wake delay.
- Wait through any reversal holding pause (driver stays enabled).
- Otherwise count 10 ticks, set STEP HIGH, and count one commanded step.
- After the 200th step, pause 250 ms and change DIR before the next rising edge.

There is **no acceleration ramp yet**. Sudden starts and speed changes remain plausible causes of missed physical steps.

### DAC updates

- The ISR advances the desired DAC A millivolt value and sets a pending flag.
- The main loop atomically copies/clears that pending flag, then writes the 16-bit MCP4822 word using eUSCI_B1.
- Channel B is commanded to 2.000 V on startup and periodically refreshed; it is **not** written with DAC A's sweep value.
- SPI is configured to remain at ~500 kHz despite changing the CPU/SMCLK configuration from 1 MHz to 8 MHz.

The MCP4822 outputs are unipolar with the present 0–5 V supply; this code **cannot** create −4 V on a DAC pin. A bipolar output requires an appropriate external analogue stage and supply rails.

## 5. Build and run in Code Composer Studio

1. Create/open an MSP430 project targeting **MSP430FR5994**, using the correct TI toolchain/device support.
2. Add `platform/main.c`, `platform/hal/hal_clock.c` and `platform/hal/hal_clock.h`. Ensure **exactly one** `main()` is built.
3. Add `platform/` to the compiler include search path, so `#include "hal/hal_clock.h"` resolves correctly. For a project rooted at `tether_demo/`, a typical search path is `${PROJECT_ROOT}/platform`.
4. If the central OBC repository already has a `hal_clock` module, keep that one rather than compiling both copies. Confirm its clock configuration matches the frequency assumptions.
5. Leave `#define DEMO_USE_BUTTONS 0` to reproduce the autonomous combined demo.
6. Power the LaunchPad and DAC, confirm outputs with an oscilloscope/multimeter, then enable the separately powered A4988 motor supply when wiring and current limit are checked.
7. If desired, change the macro to `1`, rebuild, and use the two onboard buttons as below.

**Do not simply copy the old `TA0CCR0 = 999` or `UCB1BRW = 2` settings into this build:** at 8 MHz they would produce an eight-times-faster timer/SPI clock than the earlier 1 MHz setup.

## 6. Optional manual command entry

Edit the top of `platform/main.c`:

```c
#define DEMO_USE_BUTTONS 1
```

In this mode the application boots with **motor stopped, DAC A at 0 V and DAC B at 2 V**.

Press **S1 to begin**, press **S2 N times**, then press **S1 to submit**. The parser recognises only a *debounced press*, so holding a button does not generate repeated commands. It times out after 10 seconds of inactivity and rejects more than six presses.

| S2 presses | Command | Action |
| ---: | --- | --- |
| 1 | Start DAC sweep | Channel A ramps; leaves motor as it was |
| 2 | Start motor | Begins a fresh forward/back step sequence |
| 3 | Start both | Starts DAC sweep and motor |
| 4 | Stop motor | Disables/sleeps A4988; **holding torque is lost** |
| 5 | Stop DAC sweep | Sets channel A to 0 V; channel B stays 2 V |
| 6 | Stop both | Combines the preceding two stop actions |

The red LED blinks during input. Green indicates a recognised command; red indicates an invalid/empty/timed-out entry. **Green means the command was accepted, not that the motor physically reached a requested position.** A dedicated immediate stop switch should be added for public/loaded demos; a multi-press command is **not** an emergency stop.

**Telemetry is not yet connected.** The current file has debugger-readable `s_last_command_id` and `s_last_command_accepted` values and a local dispatcher. Once the outbound communications API is available, publish receive/accept/start/complete events from an appropriate non-ISR integration point. No receive capability or ground-station transmission is claimed by this code.

## 7. HAL integration direction (suggested)

The OBC HAL conventions require `platform/hal/hal_*.c/.h` modules, public API contracts in headers, module-prefixed names, `stdint`/`stdbool`, and MSP430 register access only inside `.c` implementations.

A sensible eventual division is:

```text
platform/hal/hal_clock.*     # DCO, MCLK, SMCLK (already provided)
platform/hal/hal_timer.*     # monotonic tick and/or output compare
platform/hal/hal_spi.*       # eUSCI_B transfers, CS handling as appropriate
platform/hal/hal_gpio.*      # general-purpose input/output

platform/drivers/mcp4822.*   # command framing and DAC channel configuration
platform/drivers/a4988.*     # driver enable/sleep, direction, step service

subsystems/tether/*          # sweep and motion policy, command handling
```

These paths below the HAL are **proposals**, not a claim about the unfinished repository. The HAL should expose generic hardware operations, while MCP4822/A4988 framing and tether-specific state machines stay at higher layers. The radio-command source should eventually feed the same dispatcher as the demo buttons.

## 8. Verification checklist

- [ ] Build successfully under the chosen CCS compiler.
- [ ] Check 8 MHz clock configuration; confirm Timer_A0 produces a ~1 ms period.
- [ ] Measure MCP4822 channel B ≈ 2.000 V with respect to DAC ground.
- [ ] Verify channel A ramps between 0 and ~4 V in ~1 second.
- [ ] Check P3.4: one rising STEP edge per 10 ms while moving, each HIGH for ~1 ms.
- [ ] Count **exactly 200** STEP rising edges per revolution command.
- [ ] Verify 250 ms holding pause and DIR change before the next step.
- [ ] Verify the motor does **not** issue steps before it is enabled/awake.
- [ ] Run 20+ forward/back cycles unloaded and check shaft position repeatability.
- [ ] Run DAC and motor simultaneously; check for power/ground noise coupling.
- [ ] If using buttons, verify debounce, empty command, timeout and all six commands.
- [ ] Verify motor driver current limit, thermal behaviour and reliable connections.
- [ ] Observe an immediate externally accessible stop method before public use.

## 9. Open concerns and follow-ups

| Priority | Concern | Next action |
| --- | --- | --- |
| High | Motor sometimes fails to complete a full revolution / start position drifts | Check wiring, coil current, step count on scope, loading and reversal; add acceleration ramp and eventually feedback |
| High | Driver previously needed pressure to stay seated in breadboard | Solder headers and eliminate intermittent power/coil contacts |
| High | MCP4822 5 V digital-input HIGH threshold versus MSP430 3.3 V outputs | Add/validate appropriate SPI logic-level buffering |
| High | Open-loop step count is not a measured deployed tether length | Assess home/index switch, encoder and/or other feedback |
| High | No immediate manual stop in the two-button protocol | Add independent stop/inhibit hardware and define safe motor-state behaviour |
| Medium | DAC's first SPI commands previously failed without a power-up delay | Keep startup delay; investigate power/reset/level-shift behaviour |
| Medium | ISR-based GPIO step edges have software latency | Consider hardware output compare for deterministic pulses |
| Medium | No acceleration or deceleration at reversals | Introduce calibrated motion profiles after base electrical tests |
| Medium | Command telemetry not implemented | Integrate outbound telemetry/events once the OBC interface exists |
| Later | Actual tether instrument testing | Planned for 2027; this is still a bench demo |

## 10. Engineering Festival context

The intended demonstration uses simulated tether data held by Areeb. Subject to the final team setup, an oscilloscope can show DAC A's voltage sweep and DAC B's fixed reference, while the motor/spool prototype demonstrates motion. The tether hardware may receive structural changes; software integration and repeatability should be rechecked afterward. No physical tether deployment accuracy is claimed yet.

## 11. References

- [TI MSP-EXP430FR5994 LaunchPad User's Guide](https://www.ti.com/lit/ug/slau678c/slau678c.pdf) — onboard buttons/LEDs and power connectors.
- [TI MSP430FR5994 datasheet](https://www.ti.com/lit/ds/symlink/msp430fr5994.pdf) — GPIO functions and peripheral clocks.
- [Microchip MCP4822 product and datasheet](https://www.microchip.com/en-us/product/MCP4822) — SPI command word, voltage range and input thresholds.
- [Pololu A4988 carrier documentation](https://www.pololu.com/product/1182) — step/microstep timing, VMOT supply and current limit.
- [Pololu NEMA-8 200-step motor](https://www.pololu.com/product/1204) — winding pairs and current rating.

---

### Change log

| Date | Change |
| --- | --- |
| 2026-11-17 | Cleaned up standalone combined demo; moved to team's 8 MHz Clock HAL, corrected Timer_A/SPI divisors, guarded startup stepping, added 1 ms STEP pulse, 250 ms reversal pause and optional two-button interface. **Hardware validation pending.** |
