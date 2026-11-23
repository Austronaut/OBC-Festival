"""Send the festival demo start command from the laptop.

Talks to the ground LaunchPad (ground/uplink/uplink_msp430) over its USB
serial port; that board's CC1101 then transmits "TETHER_GO" and the OBC
starts a run. (Pressing S1 on the ground LaunchPad does the same.)

    pip install pyserial
    python3 ground/uplink/uplink.py                     # port /dev/ttyACM1
    python3 ground/uplink/uplink.py --port /dev/ttyACM1 --command info

The LaunchPad appears as two ports: /dev/ttyACM0 is the debugger, and
/dev/ttyACM1 is the UART ("backchannel") this script needs. On Linux your
user must be in the 'dialout' group to open it.
"""

import argparse

import serial  # pyserial

BAUD = 9600            # must match UART_BACKCHANNEL_BAUD in uart_backchannel.h
REPLY_TIMEOUT_S = 2.0  # a send takes about 0.15 s; "info" a little longer


def main() -> None:
    parser = argparse.ArgumentParser(description="Send a command to the ground LaunchPad")
    parser.add_argument("--port", default="/dev/ttyACM1")
    parser.add_argument("--command", default="go", choices=["go", "status", "info"])
    args = parser.parse_args()

    with serial.Serial(args.port, BAUD, timeout=REPLY_TIMEOUT_S) as port:
        port.reset_input_buffer()            # drop anything printed earlier
        port.write(f"{args.command}\n".encode())

        # "info" prints 2-3 lines ending with "config mismatches: ..."; the
        # other commands print one line.
        while True:
            reply = port.readline().decode(errors="replace").strip()
            if not reply:
                print("no reply: check --port, and that uplink_msp430 is flashed and running")
                break
            print(reply)
            if args.command != "info" or reply.startswith("config mismatches"):
                break


if __name__ == "__main__":
    main()
