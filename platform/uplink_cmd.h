/**
 * @file   uplink_cmd.h
 * @brief  The uplink command shared by the OBC and the ground transmitter.
 *
 * The OBC (platform/main.c) starts a demo run when it receives a CC1101
 * packet whose payload is exactly UPLINK_CMD_START. The ground LaunchPad
 * (ground/uplink/uplink_msp430/main.c) sends it. Both include this header,
 * so the two sides cannot disagree about the text.
 */

#ifndef UPLINK_CMD_H
#define UPLINK_CMD_H

#define UPLINK_CMD_START      "TETHER_GO"
#define UPLINK_CMD_START_LEN  (sizeof(UPLINK_CMD_START) - 1U)   /* no NUL */

#endif /* UPLINK_CMD_H */
