// SPDX-License-Identifier: MIT
// ieee1905d: 独立通信进程示例
// - 暴露 ubus 对象 ieee1905: method send，event recv
// - 调用 ieee1905 库组帧/收帧；收到帧后通过 ubus_notify 广播解析结果
// 说明：底层仍用 UDP 占位收发，便于后续替换为 L2/raw；ubus 接口保持稳定

#include "ieee1905.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <libubus.h>
#include <libubox/uloop.h>
#include <libubox/blobmsg.h>

#define DATA_PORT 19050

struct daemon_ctx {
    struct i1905_ctx *i1905;
    struct ubus_context *ubus;
    struct ubus_object obj;
    struct blob_buf bb;
    struct uloop_fd fd;
};

enum {
    SEND_TYPE,
    SEND_DST_IP,
    SEND_DST_PORT,
    __SEND_MAX,
};

static const struct blobmsg_policy send_policy[__SEND_MAX] = {
    [SEND_TYPE]    = { .name = "type",     .type = BLOBMSG_TYPE_STRING },
    [SEND_DST_IP]  = { .name = "dst_ip",   .type = BLOBMSG_TYPE_STRING },
    [SEND_DST_PORT]= { .name = "dst_port", .type = BLOBMSG_TYPE_INT32  },
};

static void notify_frame(struct daemon_ctx *d,
                         const struct i1905_cmdu *cmdu,
                         const uint8_t src_mac[6]) {
    blob_buf_init(&d->bb, 0);
    blobmsg_add_u32(&d->bb, "type", cmdu->message_type);
    blobmsg_add_u32(&d->bb, "mid", cmdu->message_id);
    blobmsg_add_u32(&d->bb, "tlv_count", cmdu->tlv_count);

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);
    blobmsg_add_string(&d->bb, "src", mac_str);

    ubus_notify(d->ubus, &d->obj, "recv", d->bb.head, -1);
}

static void on_frame(const struct i1905_cmdu *cmdu,
                     const uint8_t src_mac[6],
                     void *user_ctx) {
    struct daemon_ctx *d = user_ctx;
    notify_frame(d, cmdu, src_mac);
}

static int ubus_send(struct ubus_context *ctx, struct ubus_object *obj,
                     struct ubus_request_data *req, const char *method,
                     struct blob_attr *msg) {
    (void)method;
    struct daemon_ctx *d = container_of(obj, struct daemon_ctx, obj);
    struct blob_attr *tb[__SEND_MAX];
    blobmsg_parse(send_policy, __SEND_MAX, tb, blob_data(msg), blob_len(msg));

    if (!tb[SEND_TYPE] || !tb[SEND_DST_IP] || !tb[SEND_DST_PORT]) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    const char *type = blobmsg_get_string(tb[SEND_TYPE]);
    const char *dst_ip = blobmsg_get_string(tb[SEND_DST_IP]);
    uint16_t dst_port = (uint16_t)blobmsg_get_u32(tb[SEND_DST_PORT]);

    uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x10}; // placeholder iface/radio id
    int rv = 0;
    if (strcmp(type, "topology_query") == 0) {
        rv = i1905_send_topology_query(d->i1905, dst_ip, dst_port);
    } else if (strcmp(type, "topology_discovery") == 0) {
        rv = i1905_send_topology_discovery(d->i1905, dst_ip, dst_port, mac);
    } else if (strcmp(type, "topology_notification") == 0) {
        rv = i1905_send_topology_notification(d->i1905, dst_ip, dst_port, mac);
    } else if (strcmp(type, "ap_search") == 0) {
        rv = i1905_send_ap_autoconfig_search(d->i1905, dst_ip, dst_port, mac);
    } else if (strcmp(type, "ap_response") == 0) {
        rv = i1905_send_ap_autoconfig_response(d->i1905, dst_ip, dst_port, mac);
    } else {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    if (rv < 0) return UBUS_STATUS_UNKNOWN_ERROR;
    return 0;
}

static const struct ubus_method ieee1905_methods[] = {
    UBUS_METHOD("send", ubus_send, send_policy),
};

static struct ubus_object_type ieee1905_obj_type =
    UBUS_OBJECT_TYPE("ieee1905", ieee1905_methods);

static void fd_cb(struct uloop_fd *u, unsigned int events) {
    struct daemon_ctx *d = container_of(u, struct daemon_ctx, fd);
    if (events & ULOOP_READ) {
        i1905_handle_readable(d->i1905);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    srand((unsigned)time(NULL));
    uloop_init();

    struct daemon_ctx d = {0};
    if (i1905_init(&d.i1905, I1905_ROLE_CONTROLLER, DATA_PORT, NULL, on_frame, &d) < 0) {
        fprintf(stderr, "[ieee1905d] init failed\n");
        return 1;
    }

    d.ubus = ubus_connect(NULL);
    if (!d.ubus) {
        fprintf(stderr, "[ieee1905d] ubus connect failed\n");
        return 1;
    }
    ubus_add_uloop(d.ubus);
    blob_buf_init(&d.bb, 0);

    d.obj.type = &ieee1905_obj_type;
    d.obj.methods = ieee1905_methods;
    d.obj.n_methods = ARRAY_SIZE(ieee1905_methods);
    d.obj.name = "ieee1905";

    if (ubus_add_object(d.ubus, &d.obj)) {
        fprintf(stderr, "[ieee1905d] add ubus object failed\n");
        return 1;
    }

    d.fd.fd = i1905_get_fd(d.i1905);
    d.fd.cb = fd_cb;
    d.fd.events = ULOOP_READ;
    uloop_fd_add(&d.fd, ULOOP_READ);

    printf("[ieee1905d] running: ubus object 'ieee1905', data_port=%d (event-driven)\n", DATA_PORT);
    uloop_run();

    uloop_done();
    i1905_close(d.i1905);
    ubus_free(d.ubus);
    return 0;
}


