// SPDX-License-Identifier: MIT
//
// Minimal IEEE 1905.1 CMDU helper inspired by prplMesh layering.
// Provides lightweight TLV helpers, CMDU packing/unpacking, and a UDP
// transport loop suitable for early prototyping of controller/agent roles.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define I1905_MAX_TLVS          16
#define I1905_MAX_TLV_VALUE     1024
#define I1905_MAX_FRAME_SIZE    1600

// Message types (subset)
typedef enum {
    I1905_MSG_TOPOLOGY_DISCOVERY     = 0x0000,
    I1905_MSG_TOPOLOGY_NOTIFICATION  = 0x0001,
    I1905_MSG_TOPOLOGY_QUERY         = 0x0002,
    I1905_MSG_TOPOLOGY_RESPONSE      = 0x0003,
    I1905_MSG_AP_AUTOCONFIG_SEARCH   = 0x0006,
    I1905_MSG_AP_AUTOCONFIG_RESPONSE = 0x0007,
    I1905_MSG_AP_AUTOCONFIG_WSC      = 0x0008,
} i1905_message_type;

// TLV types (minimal subset)
typedef enum {
    I1905_TLV_END_OF_MESSAGE   = 0x00,
    I1905_TLV_AL_MAC           = 0x01,
    I1905_TLV_MAC_ADDR         = 0x02,
    I1905_TLV_DEVICE_INFO      = 0x09,
    I1905_TLV_WSC              = 0x0A,  // carries raw WSC/WPS payload
    I1905_TLV_VENDOR           = 0x0B,  // generic vendor blob for placeholders
} i1905_tlv_type;

typedef enum {
    I1905_ROLE_CONTROLLER,
    I1905_ROLE_AGENT,
} i1905_role;

struct i1905_tlv {
    uint8_t  type;
    uint16_t len;
    uint8_t  value[I1905_MAX_TLV_VALUE];
};

struct i1905_cmdu {
    uint16_t message_type;
    uint16_t message_id;
    uint8_t  fragment_id;
    bool     last_fragment;
    size_t   tlv_count;
    struct i1905_tlv tlvs[I1905_MAX_TLVS];
};

struct i1905_ctx;

typedef void (*i1905_event_cb)(const struct i1905_cmdu *cmdu,
                               const uint8_t src_mac[6],
                               void *user_ctx);

// Context lifecycle
int i1905_init(struct i1905_ctx **out,
               i1905_role role,
               uint16_t listen_port,
               const uint8_t al_mac[6],
               i1905_event_cb cb,
               void *user_ctx);
void i1905_close(struct i1905_ctx *ctx);

// Event loop
int i1905_poll(struct i1905_ctx *ctx, int timeout_ms);
// Event-driven helpers
int i1905_get_fd(const struct i1905_ctx *ctx);
int i1905_handle_readable(struct i1905_ctx *ctx);

// Convenience send helpers
int i1905_send_topology_discovery(struct i1905_ctx *ctx,
                                  const char *dst_ip,
                                  uint16_t dst_port,
                                  const uint8_t iface_mac[6]);
int i1905_send_topology_query(struct i1905_ctx *ctx,
                              const char *dst_ip,
                              uint16_t dst_port);
int i1905_send_topology_response(struct i1905_ctx *ctx,
                                 const char *dst_ip,
                                 uint16_t dst_port,
                                 const uint8_t iface_mac[6]);
int i1905_send_topology_notification(struct i1905_ctx *ctx,
                                     const char *dst_ip,
                                     uint16_t dst_port,
                                     const uint8_t iface_mac[6]);
int i1905_send_ap_autoconfig_search(struct i1905_ctx *ctx,
                                    const char *dst_ip,
                                    uint16_t dst_port,
                                    const uint8_t radio_id[6]);
int i1905_send_ap_autoconfig_response(struct i1905_ctx *ctx,
                                      const char *dst_ip,
                                      uint16_t dst_port,
                                      const uint8_t radio_id[6]);
int i1905_send_ap_autoconfig_wsc(struct i1905_ctx *ctx,
                                 const char *dst_ip,
                                 uint16_t dst_port,
                                 const uint8_t *wsc, size_t wsc_len);

// Low-level utilities
int i1905_tlv_set_mac(struct i1905_tlv *tlv, uint8_t type, const uint8_t mac[6]);
int i1905_tlv_set_wsc(struct i1905_tlv *tlv, const uint8_t *payload, size_t len);
int i1905_tlv_set_device_info(struct i1905_tlv *tlv,
                              const uint8_t al_mac[6],
                              const uint8_t iface_mac[6]);


