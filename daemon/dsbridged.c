/*
 * dsbridged -- 32-bit helper daemon that hosts the real Dolby libdseffect.so
 * and speaks the dsbridge protocol to the 64-bit shim running inside
 * audioserver.
 *
 * This binary MUST be built for the 32-bit ARM ABI (armeabi-v7a), matching
 * libdseffect.so, and run as a standalone process started from the module's
 * service.sh at boot. It must NOT be fork()'d from inside audioserver.
 *
 * Usage: dsbridged /path/to/libdseffect.so
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <android/log.h>

#include "audio_effect_full.h"
#include "../include/audio_effect_min.h"
#include "../include/dsbridge_proto.h"
#include "../include/dsbridge_io.h"

#define LOG_TAG "dsbridged"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_INSTANCES 4
#define MAX_CMD_PAYLOAD 4096

static void *g_lib = NULL;
static effect_create_fn_t g_EffectCreate = NULL;
static effect_release_fn_t g_EffectRelease = NULL;
static effect_get_descriptor_fn_t g_EffectGetDescriptor = NULL;

static pthread_mutex_t g_instLock = PTHREAD_MUTEX_INITIALIZER;
static effect_handle_t g_instances[MAX_INSTANCES];

static int alloc_instance_slot(effect_handle_t h) {
    pthread_mutex_lock(&g_instLock);
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (g_instances[i] == NULL) {
            g_instances[i] = h;
            pthread_mutex_unlock(&g_instLock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_instLock);
    return -1;
}

static effect_handle_t get_instance(int32_t id) {
    if (id < 0 || id >= MAX_INSTANCES) return NULL;
    pthread_mutex_lock(&g_instLock);
    effect_handle_t h = g_instances[id];
    pthread_mutex_unlock(&g_instLock);
    return h;
}

static void free_instance_slot(int32_t id) {
    if (id < 0 || id >= MAX_INSTANCES) return;
    pthread_mutex_lock(&g_instLock);
    g_instances[id] = NULL;
    pthread_mutex_unlock(&g_instLock);
}

static int load_effect_lib(const char *path) {
    g_lib = dlopen(path, RTLD_NOW);
    if (!g_lib) {
        LOGE("dlopen(%s) failed: %s", path, dlerror());
        return -1;
    }
    g_EffectCreate = (effect_create_fn_t)dlsym(g_lib, "EffectCreate");
    g_EffectRelease = (effect_release_fn_t)dlsym(g_lib, "EffectRelease");
    g_EffectGetDescriptor = (effect_get_descriptor_fn_t)dlsym(g_lib, "EffectGetDescriptor");
    if (!g_EffectCreate || !g_EffectRelease || !g_EffectGetDescriptor) {
        LOGE("missing required symbols in %s (Create=%p Release=%p GetDescriptor=%p)",
             path, (void*)g_EffectCreate, (void*)g_EffectRelease, (void*)g_EffectGetDescriptor);
        return -1;
    }
    LOGI("loaded %s ok, EffectCreate=%p EffectRelease=%p EffectGetDescriptor=%p",
         path, (void*)g_EffectCreate, (void*)g_EffectRelease, (void*)g_EffectGetDescriptor);
    return 0;
}

static void send_error(int fd, int32_t instanceId, int32_t status) {
    dsbridge_msg_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = DSBRIDGE_MAGIC;
    hdr.version = DSBRIDGE_PROTO_VERSION;
    hdr.op = DSB_OP_PONG_ERROR;
    hdr.instanceId = instanceId;
    hdr.status = status;
    hdr.payloadSize = 0;
    dsbridge_write_all(fd, &hdr, sizeof(hdr));
}

static int send_reply(int fd, uint16_t op, int32_t instanceId, int32_t status,
                       const void *payload, uint32_t payloadSize) {
    dsbridge_msg_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = DSBRIDGE_MAGIC;
    hdr.version = DSBRIDGE_PROTO_VERSION;
    hdr.op = op;
    hdr.instanceId = instanceId;
    hdr.status = status;
    hdr.payloadSize = payloadSize;
    if (dsbridge_write_all(fd, &hdr, sizeof(hdr)) != 0) return -1;
    if (payloadSize > 0) {
        if (dsbridge_write_all(fd, payload, payloadSize) != 0) return -1;
    }
    return 0;
}

static void handle_get_descriptor(int fd, const dsbridge_msg_hdr_t *req, const unsigned char *payload) {
    if (req->payloadSize < 16) { send_error(fd, -1, -EINVAL); return; }
    effect_uuid_t uuid;
    memcpy(&uuid, payload, sizeof(uuid));
    effect_descriptor_t desc;
    memset(&desc, 0, sizeof(desc));
    int32_t rc = g_EffectGetDescriptor(&uuid, &desc);
    if (rc != 0) {
        LOGE("EffectGetDescriptor failed rc=%d", rc);
        send_error(fd, -1, rc);
        return;
    }
    send_reply(fd, DSB_OP_GET_DESCRIPTOR, -1, 0, &desc, sizeof(desc));
}

static void handle_create(int fd, const dsbridge_msg_hdr_t *req, const unsigned char *payload) {
    if (req->payloadSize < sizeof(dsbridge_create_req_t)) { send_error(fd, -1, -EINVAL); return; }
    dsbridge_create_req_t creq;
    memcpy(&creq, payload, sizeof(creq));
    effect_uuid_t uuid;
    memcpy(&uuid, creq.uuid_bytes, sizeof(uuid));

    effect_handle_t handle = NULL;
    int32_t rc = g_EffectCreate(&uuid, creq.sessionId, creq.ioId, &handle);
    if (rc != 0 || handle == NULL) {
        LOGE("EffectCreate failed rc=%d handle=%p", rc, (void*)handle);
        send_error(fd, -1, rc != 0 ? rc : -EFAULT);
        return;
    }
    int slot = alloc_instance_slot(handle);
    if (slot < 0) {
        LOGE("no free instance slots, releasing newly created effect");
        g_EffectRelease(handle);
        send_error(fd, -1, -ENOMEM);
        return;
    }
    LOGI("created instance id=%d handle=%p session=%d io=%d", slot, (void*)handle, creq.sessionId, creq.ioId);
    send_reply(fd, DSB_OP_CREATE, slot, 0, NULL, 0);
}

static void handle_release(int fd, const dsbridge_msg_hdr_t *req) {
    effect_handle_t h = get_instance(req->instanceId);
    if (!h) { send_error(fd, req->instanceId, -EINVAL); return; }
    int32_t rc = g_EffectRelease(h);
    free_instance_slot(req->instanceId);
    LOGI("released instance id=%d rc=%d", req->instanceId, rc);
    send_reply(fd, DSB_OP_RELEASE, req->instanceId, rc, NULL, 0);
}

static void handle_command_generic(int fd, const dsbridge_msg_hdr_t *req, const unsigned char *payload) {
    effect_handle_t h = get_instance(req->instanceId);
    if (!h || !*h) { send_error(fd, req->instanceId, -EINVAL); return; }
    if (req->payloadSize < sizeof(dsbridge_cmd_generic_req_t)) { send_error(fd, req->instanceId, -EINVAL); return; }

    dsbridge_cmd_generic_req_t creq;
    memcpy(&creq, payload, sizeof(creq));
    const unsigned char *cmdData = payload + sizeof(creq);

    if (creq.cmdSize > MAX_CMD_PAYLOAD || creq.maxReplySize > MAX_CMD_PAYLOAD) {
        LOGW("command %u payload too large (cmdSize=%u maxReplySize=%u), rejecting",
             creq.cmdCode, creq.cmdSize, creq.maxReplySize);
        send_error(fd, req->instanceId, -EMSGSIZE);
        return;
    }

    unsigned char cmdBuf[MAX_CMD_PAYLOAD];
    unsigned char replyBuf[MAX_CMD_PAYLOAD];
    memcpy(cmdBuf, cmdData, creq.cmdSize);
    uint32_t replySize = creq.maxReplySize;
    memset(replyBuf, 0, sizeof(replyBuf));

    int32_t rc = (*h)->command(h, creq.cmdCode, creq.cmdSize, creq.cmdSize ? cmdBuf : NULL,
                                &replySize, replySize ? replyBuf : NULL);
    if (rc != 0) {
        LOGW("command 0x%x rc=%d", creq.cmdCode, rc);
    }

    unsigned char outBuf[sizeof(dsbridge_cmd_generic_reply_t) + MAX_CMD_PAYLOAD];
    dsbridge_cmd_generic_reply_t rhdr;
    rhdr.replySize = replySize;
    memcpy(outBuf, &rhdr, sizeof(rhdr));
    if (replySize > 0) memcpy(outBuf + sizeof(rhdr), replyBuf, replySize);
    send_reply(fd, DSB_OP_COMMAND_GENERIC, req->instanceId, rc, outBuf, sizeof(rhdr) + replySize);
}

static void handle_command_set_config(int fd, const dsbridge_msg_hdr_t *req, const unsigned char *payload) {
    effect_handle_t h = get_instance(req->instanceId);
    if (!h || !*h) { send_error(fd, req->instanceId, -EINVAL); return; }
    if (req->payloadSize < sizeof(dsbridge_wire_config_t)) { send_error(fd, req->instanceId, -EINVAL); return; }

    dsbridge_wire_config_t wire;
    memcpy(&wire, payload, sizeof(wire));

    if (wire.inputCfg.hadProvider || wire.outputCfg.hadProvider) {
        LOGE("SET_CONFIG requested EFFECT_CONFIG_PROVIDER (buffer_provider callbacks) "
             "which dsbridge does NOT implement -- audio for this instance will likely "
             "be silent or wrong. This needs real support added to dsbridged/libdsbridge.");
    }

    effect_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.inputCfg.samplingRate = wire.inputCfg.samplingRate;
    cfg.inputCfg.channels     = wire.inputCfg.channels;
    cfg.inputCfg.format       = wire.inputCfg.format;
    cfg.inputCfg.accessMode   = wire.inputCfg.accessMode;
    cfg.inputCfg.mask         = wire.inputCfg.mask & (uint16_t)~EFFECT_CONFIG_PROVIDER;
    cfg.inputCfg.buffer.raw   = NULL;
    cfg.inputCfg.buffer.frameCount = 0;

    cfg.outputCfg.samplingRate = wire.outputCfg.samplingRate;
    cfg.outputCfg.channels     = wire.outputCfg.channels;
    cfg.outputCfg.format       = wire.outputCfg.format;
    cfg.outputCfg.accessMode   = wire.outputCfg.accessMode;
    cfg.outputCfg.mask         = wire.outputCfg.mask & (uint16_t)~EFFECT_CONFIG_PROVIDER;
    cfg.outputCfg.buffer.raw   = NULL;
    cfg.outputCfg.buffer.frameCount = 0;

    uint32_t replySize = sizeof(int32_t);
    int32_t replyStatus = 0;
    int32_t rc = (*h)->command(h, EFFECT_CMD_SET_CONFIG, sizeof(cfg), &cfg, &replySize, &replyStatus);
    LOGI("SET_CONFIG rate_in=%u ch_in=%u rate_out=%u ch_out=%u -> rc=%d replyStatus=%d",
         cfg.inputCfg.samplingRate, cfg.inputCfg.channels,
         cfg.outputCfg.samplingRate, cfg.outputCfg.channels, rc, replyStatus);

    send_reply(fd, DSB_OP_COMMAND_SET_CONFIG, req->instanceId, rc, NULL, 0);
}

static void handle_process(int fd, const dsbridge_msg_hdr_t *req, const unsigned char *payload) {
    effect_handle_t h = get_instance(req->instanceId);
    if (!h || !*h) { send_error(fd, req->instanceId, -EINVAL); return; }
    if (req->payloadSize < sizeof(dsbridge_process_req_t)) { send_error(fd, req->instanceId, -EINVAL); return; }

    dsbridge_process_req_t preq;
    memcpy(&preq, payload, sizeof(preq));
    const unsigned char *pcmIn = payload + sizeof(preq);
    size_t sampleBytes = (size_t)preq.frameCount * preq.channels * preq.bytesPerSample;

    if (req->payloadSize < sizeof(preq) + sampleBytes) { send_error(fd, req->instanceId, -EINVAL); return; }
    if (sampleBytes > MAX_CMD_PAYLOAD * 8) { send_error(fd, req->instanceId, -EMSGSIZE); return; }

    /* Local copies -- audio_buffer_t.raw always points into THIS process's
       own memory, never a pointer value that came from the wire. */
    int16_t *inSamples = (int16_t *)malloc(sampleBytes);
    int16_t *outSamples = (int16_t *)calloc(1, sampleBytes);
    if (!inSamples || !outSamples) {
        free(inSamples); free(outSamples);
        send_error(fd, req->instanceId, -ENOMEM);
        return;
    }
    memcpy(inSamples, pcmIn, sampleBytes);

    audio_buffer_t inBuf, outBuf;
    inBuf.frameCount = preq.frameCount;
    inBuf.s16 = inSamples;
    outBuf.frameCount = preq.frameCount;
    outBuf.s16 = outSamples;

    int32_t rc = (*h)->process(h, &inBuf, &outBuf);

    unsigned char *outMsg = (unsigned char *)malloc(sizeof(dsbridge_process_reply_t) + sampleBytes);
    if (!outMsg) {
        free(inSamples); free(outSamples);
        send_error(fd, req->instanceId, -ENOMEM);
        return;
    }
    dsbridge_process_reply_t prep;
    prep.frameCount = preq.frameCount;
    prep.channels = preq.channels;
    prep.bytesPerSample = preq.bytesPerSample;
    memcpy(outMsg, &prep, sizeof(prep));
    memcpy(outMsg + sizeof(prep), outSamples, sampleBytes);

    send_reply(fd, DSB_OP_PROCESS, req->instanceId, rc, outMsg, sizeof(prep) + sampleBytes);

    free(outMsg);
    free(inSamples);
    free(outSamples);
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    unsigned char *payload = (unsigned char *)malloc(DSBRIDGE_MAX_PAYLOAD);
    if (!payload) { close(fd); return NULL; }

    for (;;) {
        dsbridge_msg_hdr_t hdr;
        if (dsbridge_read_all(fd, &hdr, sizeof(hdr)) != 0) break;
        if (hdr.magic != DSBRIDGE_MAGIC) { LOGE("bad magic, closing connection"); break; }
        if (hdr.payloadSize > DSBRIDGE_MAX_PAYLOAD) { LOGE("payload too large, closing connection"); break; }
        if (hdr.payloadSize > 0) {
            if (dsbridge_read_all(fd, payload, hdr.payloadSize) != 0) break;
        }

        switch ((dsbridge_op_t)hdr.op) {
            case DSB_OP_HELLO:
                send_reply(fd, DSB_OP_HELLO, -1, 0, NULL, 0);
                break;
            case DSB_OP_GET_DESCRIPTOR:
                handle_get_descriptor(fd, &hdr, payload);
                break;
            case DSB_OP_CREATE:
                handle_create(fd, &hdr, payload);
                break;
            case DSB_OP_RELEASE:
                handle_release(fd, &hdr);
                break;
            case DSB_OP_COMMAND_GENERIC:
                handle_command_generic(fd, &hdr, payload);
                break;
            case DSB_OP_COMMAND_SET_CONFIG:
                handle_command_set_config(fd, &hdr, payload);
                break;
            case DSB_OP_PROCESS:
                handle_process(fd, &hdr, payload);
                break;
            default:
                LOGE("unknown op %u", hdr.op);
                send_error(fd, hdr.instanceId, -ENOSYS);
                break;
        }
    }

    free(payload);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    const char *libPath = (argc > 1) ? argv[1] : "/system/vendor/lib/soundfx/libdseffect.so";
    if (load_effect_lib(libPath) != 0) {
        LOGE("failed to load effect library, exiting");
        return 1;
    }

    unlink(DSBRIDGE_SOCK_PATH);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { LOGE("socket() failed: %s", strerror(errno)); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DSBRIDGE_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGE("bind(%s) failed: %s", DSBRIDGE_SOCK_PATH, strerror(errno));
        return 1;
    }
    chmod(DSBRIDGE_SOCK_PATH, 0666);

    /* The socket file inherits its parent directory's SELinux label by
     * default, which under /data/adb/modules/... is NOT vendor_file. Our
     * sepolicy.rule grants audioserver access to vendor_file sockets
     * specifically, so relabel explicitly (equivalent to `chcon`). This
     * requires the daemon's own domain to have relabelto permission for
     * vendor_file -- true for the su/ksu domain service.sh scripts run in,
     * which already has broad relabel permissions for adb/root tooling. */
    static const char *kVendorFileContext = "u:object_r:vendor_file:s0";
    if (setxattr(DSBRIDGE_SOCK_PATH, "security.selinux",
                 kVendorFileContext, strlen(kVendorFileContext) + 1, 0) != 0) {
        LOGW("setxattr(security.selinux) on socket failed: %s "
             "(audioserver may be denied by SELinux; check avc denials)", strerror(errno));
    }

    if (listen(lfd, 4) != 0) {
        LOGE("listen() failed: %s", strerror(errno));
        return 1;
    }

    LOGI("dsbridged ready, listening on %s", DSBRIDGE_SOCK_PATH);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            LOGE("accept() failed: %s", strerror(errno));
            continue;
        }
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, (void *)(intptr_t)cfd) != 0) {
            LOGE("pthread_create failed: %s", strerror(errno));
            close(cfd);
            continue;
        }
        pthread_detach(th);
    }

    return 0;
}
