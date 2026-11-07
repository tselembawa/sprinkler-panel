/*
 * Raspberry Pi <-> Main Controller protocol v1 (docs/pi-link.md).
 * The Python side (hmi/app/link_proto.py) uses the same numbers.
 */
#ifndef PI_LINK_PROTO_H
#define PI_LINK_PROTO_H

#define PL_VERSION          1u
#define PL_HEADER_LEN       6u      /* ver, type, seq, flags, len(2) */
#define PL_MAX_PAYLOAD      512u
#define PL_MAX_FRAME        (PL_HEADER_LEN + PL_MAX_PAYLOAD + 2u)
#define PL_MAX_ENCODED      (PL_MAX_FRAME + PL_MAX_FRAME / 254u + 2u)

#define PL_PING             0x01u
#define PL_GET_STATUS       0x02u
#define PL_GET_POINTS       0x04u
#define PL_GET_MODULES      0x05u
#define PL_GET_EVENTS       0x06u
#define PL_CFG_BEGIN        0x10u
#define PL_CFG_PUT          0x11u
#define PL_CFG_END          0x12u
#define PL_CFG_ACTIVATE     0x13u
#define PL_CFG_INFO         0x14u
#define PL_CMD_ACK          0x20u
#define PL_CMD_SILENCE      0x21u
#define PL_CMD_RESET        0x22u
#define PL_CMD_DISABLE      0x23u
#define PL_CMD_SERVICE      0x25u
#define PL_SET_TIME         0x30u
#define PL_DEV_SET_KEY      0x40u
#define PL_LOG              0x60u
#define PL_NACK             0x7Fu

#define PL_NACK_UNKNOWN_TYPE     1u
#define PL_NACK_BAD_LENGTH       2u
#define PL_NACK_ACCESS_DENIED    3u
#define PL_NACK_BAD_DURATION     4u
#define PL_NACK_UNKNOWN_POINT    5u
#define PL_NACK_BAD_TIME         6u
#define PL_NACK_CFG_SEQUENCE     7u
#define PL_NACK_CFG_TOO_MANY     8u
#define PL_NACK_CFG_CRC_MISMATCH 9u
#define PL_NACK_BAD_VERSION      10u

#define PL_POINT_REC_LEN    10u
#define PL_EVENT_REC_LEN    21u
#define PL_MODULE_REC_LEN   12u
#define PL_CFG_REC_LEN      52u
#define PL_STATUS_LEN       43u

#define PL_HMI_LOST_MS      5000u   /* no frame from the Pi this long -> HMI_LOST */

#endif /* PI_LINK_PROTO_H */
