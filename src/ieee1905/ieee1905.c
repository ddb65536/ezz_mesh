// SPDX-License-Identifier: MIT
#include "ieee1905.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>

struct i1905_ctx {
    int sock;
    uint16_t port;
    i1905_role role;
    uint8_t al_mac[6];
    i1905_event_cb cb;
    void *user_ctx;
    uint16_t next_message_id;
};

static uint16_t next_id(struct i1905_ctx *ctx) {
    ctx->next_message_id++;
    if (ctx->next_message_id == 0) {
        ctx->next_message_id = 1;
    }
    return ctx->next_message_id;
}

static int udp_open(uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    // non-blocking for event-driven loops
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags != -1) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    return sock;
}

static int tlv_append(struct i1905_cmdu *cmdu, const struct i1905_tlv *src) {
    if (cmdu->tlv_count >= I1905_MAX_TLVS) return -1;
    cmdu->tlvs[cmdu->tlv_count] = *src;
    cmdu->tlv_count++;
    return 0;
}

static int cmdu_pack(const struct i1905_cmdu *cmdu, uint8_t *buf, size_t buf_len) {
    size_t pos = 0;
    if (buf_len < 6) return -1;
    buf[pos++] = 0x00; // message version/reserved
    buf[pos++] = (cmdu->message_type >> 8) & 0xFF;
    buf[pos++] = cmdu->message_type & 0xFF;
    buf[pos++] = (cmdu->message_id >> 8) & 0xFF;
    buf[pos++] = cmdu->message_id & 0xFF;
    buf[pos++] = cmdu->fragment_id;
    buf[pos++] = cmdu->last_fragment ? 0x80 : 0x00;

    for (size_t i = 0; i < cmdu->tlv_count; i++) {
        const struct i1905_tlv *t = &cmdu->tlvs[i];
        if (pos + 3 + t->len > buf_len) return -1;
        buf[pos++] = t->type;
        buf[pos++] = (t->len >> 8) & 0xFF;
        buf[pos++] = t->len & 0xFF;
        memcpy(&buf[pos], t->value, t->len);
        pos += t->len;
    }
    if (pos + 3 > buf_len) return -1;
    buf[pos++] = I1905_TLV_END_OF_MESSAGE;
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    return (int)pos;
}

static int cmdu_unpack(const uint8_t *buf, size_t len, struct i1905_cmdu *out) {
    if (len < 7) return -1;
    size_t pos = 0;
    pos++; // skip version/reserved
    out->message_type = (buf[pos] << 8) | buf[pos + 1]; pos += 2;
    out->message_id   = (buf[pos] << 8) | buf[pos + 1]; pos += 2;
    out->fragment_id  = buf[pos++];
    out->last_fragment = (buf[pos++] & 0x80) != 0;
    out->tlv_count = 0;

    while (pos + 3 <= len) {
        uint8_t type = buf[pos++];
        uint16_t tlen = (buf[pos] << 8) | buf[pos + 1];
        pos += 2;
        if (type == I1905_TLV_END_OF_MESSAGE) break;
        if (out->tlv_count >= I1905_MAX_TLVS) return -1;
        if (tlen > I1905_MAX_TLV_VALUE) return -1;
        if (pos + tlen > len) return -1;
        struct i1905_tlv *t = &out->tlvs[out->tlv_count++];
        t->type = type;
        t->len = tlen;
        memcpy(t->value, &buf[pos], tlen);
        pos += tlen;
    }
    return 0;
}

static int send_cmdu(struct i1905_ctx *ctx,
                     const char *dst_ip,
                     uint16_t dst_port,
                     struct i1905_cmdu *cmdu) {
    uint8_t frame[I1905_MAX_FRAME_SIZE];
    cmdu->message_id = next_id(ctx);
    int len = cmdu_pack(cmdu, frame, sizeof(frame));
    if (len < 0) return -1;

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(dst_port),
    };
    if (inet_aton(dst_ip, &dst.sin_addr) == 0) {
        fprintf(stderr, "invalid dst_ip %s\n", dst_ip);
        return -1;
    }
    ssize_t sent = sendto(ctx->sock, frame, len, 0,
                          (struct sockaddr *)&dst, sizeof(dst));
    return (sent == len) ? 0 : -1;
}

static void random_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)rand();
    }
    mac[0] &= 0xFE; // unicast
    mac[0] |= 0x02; // locally administered
}

static void mac_to_str(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int i1905_init(struct i1905_ctx **out,
               i1905_role role,
               uint16_t listen_port,
               const uint8_t al_mac[6],
               i1905_event_cb cb,
               void *user_ctx) {
    if (!out) return -1;
    struct i1905_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->sock = udp_open(listen_port);
    if (ctx->sock < 0) {
        free(ctx);
        return -1;
    }
    ctx->role = role;
    if (al_mac) memcpy(ctx->al_mac, al_mac, 6);
    else random_mac(ctx->al_mac);
    ctx->cb = cb;
    ctx->user_ctx = user_ctx;
    ctx->next_message_id = (uint16_t)(rand() & 0xFFFF);
    *out = ctx;
    return 0;
}

void i1905_close(struct i1905_ctx *ctx) {
    if (!ctx) return;
    close(ctx->sock);
    free(ctx);
}

int i1905_poll(struct i1905_ctx *ctx, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx->sock, &rfds);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int rv = select(ctx->sock + 1, &rfds, NULL, NULL, &tv);
    if (rv <= 0) return rv; // timeout or error

    uint8_t frame[I1905_MAX_FRAME_SIZE];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t got = recvfrom(ctx->sock, frame, sizeof(frame), 0,
                           (struct sockaddr *)&from, &from_len);
    if (got <= 0) return -1;

    struct i1905_cmdu cmdu;
    if (cmdu_unpack(frame, (size_t)got, &cmdu) < 0) {
        fprintf(stderr, "drop invalid CMDU\n");
        return -1;
    }

    uint8_t src_mac[6];
    random_mac(src_mac); // placeholder until real L2 integration
    if (ctx->cb) ctx->cb(&cmdu, src_mac, ctx->user_ctx);
    return 1;
}

int i1905_get_fd(const struct i1905_ctx *ctx) {
    return ctx ? ctx->sock : -1;
}

int i1905_handle_readable(struct i1905_ctx *ctx) {
    if (!ctx) return -1;
    while (1) {
        uint8_t frame[I1905_MAX_FRAME_SIZE];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t got = recvfrom(ctx->sock, frame, sizeof(frame), 0,
                               (struct sockaddr *)&from, &from_len);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (got == 0) return 0;

        struct i1905_cmdu cmdu;
        if (cmdu_unpack(frame, (size_t)got, &cmdu) < 0) {
            fprintf(stderr, "drop invalid CMDU\n");
            continue;
        }
        uint8_t src_mac[6];
        random_mac(src_mac); // placeholder until real L2 integration
        if (ctx->cb) ctx->cb(&cmdu, src_mac, ctx->user_ctx);
    }
}

int i1905_tlv_set_mac(struct i1905_tlv *tlv, uint8_t type, const uint8_t mac[6]) {
    if (!tlv || !mac) return -1;
    tlv->type = type;
    tlv->len = 6;
    memcpy(tlv->value, mac, 6);
    return 0;
}

int i1905_tlv_set_wsc(struct i1905_tlv *tlv, const uint8_t *payload, size_t len) {
    if (!tlv || !payload || len > I1905_MAX_TLV_VALUE) return -1;
    tlv->type = I1905_TLV_WSC;
    tlv->len = (uint16_t)len;
    memcpy(tlv->value, payload, len);
    return 0;
}

int i1905_tlv_set_device_info(struct i1905_tlv *tlv,
                              const uint8_t al_mac[6],
                              const uint8_t iface_mac[6]) {
    if (!tlv || !al_mac || !iface_mac) return -1;
    tlv->type = I1905_TLV_DEVICE_INFO;
    tlv->len = 1 + 6 + 6 + 2; // iface count + AL + iface mac + media type
    uint8_t *p = tlv->value;
    memcpy(p, al_mac, 6); p += 6;
    *p++ = 1; // one interface
    memcpy(p, iface_mac, 6); p += 6;
    *p++ = 0x00; *p++ = 0x00; // generic media type
    return 0;
}

static void build_cmdu_common(struct i1905_cmdu *cmdu, uint16_t type) {
    memset(cmdu, 0, sizeof(*cmdu));
    cmdu->message_type = type;
    cmdu->fragment_id = 0;
    cmdu->last_fragment = true;
}

int i1905_send_topology_discovery(struct i1905_ctx *ctx,
                                  const char *dst_ip,
                                  uint16_t dst_port,
                                  const uint8_t iface_mac[6]) {
    struct i1905_cmdu cmdu;
    build_cmdu_common(&cmdu, I1905_MSG_TOPOLOGY_DISCOVERY);
    struct i1905_tlv al, mac;
    i1905_tlv_set_mac(&al, I1905_TLV_AL_MAC, ctx->al_mac);
    i1905_tlv_set_mac(&mac, I1905_TLV_MAC_ADDR, iface_mac);
    tlv_append(&cmdu, &al);
    tlv_append(&cmdu, &mac);
    return send_cmdu(ctx, dst_ip, dst_port, &cmdu);
}

int i1905_send_topology_query(struct i1905_ctx *ctx,
                              const char *dst_ip,
                              uint16_t dst_port) {
    struct i1905_cmdu cmdu;
    build_cmdu_common(&cmdu, I1905_MSG_TOPOLOGY_QUERY);
    struct i1905_tlv al;
    i1905_tlv_set_mac(&al, I1905_TLV_AL_MAC, ctx->al_mac);
    tlv_append(&cmdu, &al);
    return send_cmdu(ctx, dst_ip, dst_port, &cmdu);
}

int i1905_send_topology_response(struct i1905_ctx *ctx,
                                 const char *dst_ip,
                                 uint16_t dst_port,
                                 const uint8_t iface_mac[6]) {
    struct i1905_cmdu cmdu;
    build_cmdu_common(&cmdu, I1905_MSG_TOPOLOGY_RESPONSE);
    struct i1905_tlv dev;
    i1905_tlv_set_device_info(&dev, ctx->al_mac, iface_mac);
    tlv_append(&cmdu, &dev);
    return send_cmdu(ctx, dst_ip, dst_port, &cmdu);
}

int i1905_send_topology_notification(struct i1905_ctx *ctx,
                                     const char *dst_ip,
                                     uint16_t dst_port,
                                     const uint8_t iface_mac[6]) {
    struct i1905_cmdu cmdu;
    build_cmdu_common(&cmdu, I1905_MSG_TOPOLOGY_NOTIFICATION);
    struct i1905_tlv al, mac;
    i1905_tlv_set_mac(&al, I1905_TLV_AL_MAC, ctx->al_mac);
    i1905_tlv_set_mac(&mac, I1905_TLV_MAC_ADDR, iface_mac);
    tlv_append(&cmdu, &al);
    tlv_append(&cmdu, &mac);
    return send_cmdu(ctx, dst_ip, dst_port, &cmdu);
}

int i1905_send_ap_autoconfig_search(struct i1905_ctx *ctx,
                                    const char *dst_ip,
                                    uint16_t dst_port,
                                    const uint8_t radio_id[6]) {
    struct i1905_cmdu cmdu;
    build_cmdu_common(&cmdu, I1905_MSG_AP_AUTOCONFIG_SEARCH);
    struct i1905_tlv mac, wsc;
    i1905_tlv_set_mac(&mac, I1905_TLV_MAC_ADDR, radio_id);
    const uint8_t placeholder[] = {0x10, 0x47, 0x00, 0x06, '1', '9', '0', '5', 'W', 'S'};
    i1905_tlv_set_wsc(&wsc, placeholder, sizeof(placeholder));
    tlv_append(&cmdu, &mac);
    tlv_append(&cmdu, &wsc);
    return send_cmdu(ctx, dst_ip, dst_port, &cmdu);
}

int i1905_send_ap_autoconfig_response(struct i1905_ctx *ctx,
                                      const char *dst_ip,
                                      uint16_t dst_port,
                                      const uint8_t radio_id[6]) {
    struct i1905_cmdu cmdu;
    build_cmdu_common(&cmdu, I1905_MSG_AP_AUTOCONFIG_RESPONSE);
    struct i1905_tlv mac;
    i1905_tlv_set_mac(&mac, I1905_TLV_MAC_ADDR, radio_id);
    tlv_append(&cmdu, &mac);
    return send_cmdu(ctx, dst_ip, dst_port, &cmdu);
}

int i1905_send_ap_autoconfig_wsc(struct i1905_ctx *ctx,
                                 const char *dst_ip,
                                 uint16_t dst_port,
                                 const uint8_t *wsc, size_t wsc_len) {
    struct i1905_cmdu cmdu;
    build_cmdu_common(&cmdu, I1905_MSG_AP_AUTOCONFIG_WSC);
    struct i1905_tlv t;
    i1905_tlv_set_wsc(&t, wsc, wsc_len);
    tlv_append(&cmdu, &t);
    return send_cmdu(ctx, dst_ip, dst_port, &cmdu);
}

#ifdef I1905_STANDALONE_TEST
int main(void) {
    srand((unsigned)time(NULL));
    struct i1905_ctx *ctx;
    if (i1905_init(&ctx, I1905_ROLE_CONTROLLER, 19050, NULL, NULL, NULL) < 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    uint8_t mac[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    i1905_send_topology_discovery(ctx, "127.0.0.1", 19050, mac);
    i1905_close(ctx);
    return 0;
}
#endif


