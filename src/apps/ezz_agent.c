// SPDX-License-Identifier: MIT
// ezz_agent: Agent 进程示例。仅通过 ubus 调用 ieee1905d，不直接链接 ieee1905 库。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libubus.h>
#include <libubox/uloop.h>

static struct ubus_context *ctx;
static uint32_t ieee1905_id;

static void evt_handler(struct ubus_context *ctx, struct ubus_event_handler *ev,
                        const char *type, struct blob_attr *msg) {
    (void)ctx; (void)ev;
    char *json = blobmsg_format_json(msg, true);
    printf("[agent] event %s: %s\n", type, json ? json : "{}");
    free(json);
}

static int send_cmd(const char *type, const char *dst_ip, uint32_t dst_port) {
    struct blob_buf bb;
    blob_buf_init(&bb, 0);
    blobmsg_add_string(&bb, "type", type);
    blobmsg_add_string(&bb, "dst_ip", dst_ip);
    blobmsg_add_u32(&bb, "dst_port", dst_port);
    int rv = ubus_invoke(ctx, ieee1905_id, "send", bb.head, NULL, NULL, 2000);
    blob_buf_free(&bb);
    return rv;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <data_port>\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    uint32_t data_port = (uint32_t)atoi(argv[1]);

    uloop_init();
    ctx = ubus_connect(NULL);
    if (!ctx) {
        fprintf(stderr, "ubus connect failed\n");
        return 1;
    }
    ubus_add_uloop(ctx);
    if (ubus_lookup_id(ctx, "ieee1905", &ieee1905_id)) {
        fprintf(stderr, "cannot find ubus object 'ieee1905'\n");
        return 1;
    }

    struct ubus_event_handler ev = { .cb = evt_handler };
    ubus_register_event_handler(ctx, &ev, "ieee1905.recv");

    // Agent 启动后上报拓扑发现
    printf("[agent] send topology_discovery\n");
    send_cmd("topology_discovery", "127.0.0.1", data_port);

    uloop_run();
    ubus_free(ctx);
    uloop_done();
    return 0;
}

