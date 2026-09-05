/*
 * libdsbridge -- 64-bit shim loaded by audioserver in place of the original
 * (32-bit only) libdseffect.so. Implements the standard AOSP
 * audio_effect_library_t ABI and forwards every call to dsbridged (the
 * 32-bit daemon that actually hosts the real Dolby binary) over a Unix
 * domain socket.
 *
 * IMPORTANT: this file must be built for the arm64 (64-bit) ABI and placed
 * where audio_effects.xml's "ds" library path points, e.g.
 * /vendor/lib64/soundfx/libdsbridge.so, with audio_effects.xml's
 * <library name="ds" path="libdsbridge.so"/> left as-is (relative name is
 * resolved by audioserver against its own lib64/soundfx dir).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <android/log.h>

#include "../include/audio_effect_min.h"
#include "../include/dsbridge_proto.h"
#include "../include/dsbridge_io.h"

#define LOG_TAG "libdsbridge"
#define MAX_CMD_PAYLOAD_LOCAL 4096
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ---- local (64-bit) mirrors of the pointer-containing AOSP structs ----
 * These exist ONLY so this file can be compiled against the real
 * effect_interface_s calling convention that audioserver expects.
 * Their layout is whatever the 64-bit compiler produces -- correct for
 * THIS process. Never sent across the wire as raw bytes. */
typedef struct audio_buffer_s {
    size_t frameCount;
    union {
        void    *raw;
        int32_t *i32;
        int16_t *s16;
        uint8_t *u8;
    };
} audio_buffer_t;

typedef int32_t (*buffer_function_t)(void *cookie, audio_buffer_t *buffer);
typedef struct buffer_provider_s {
    buffer_function_t getBuffer;
    buffer_function_t releaseBuffer;
    void *cookie;
} buffer_provider_t;

typedef struct buffer_config_s {
    audio_buffer_t buffer;
    uint32_t samplingRate;
    uint32_t channels;
    buffer_provider_t bufferProvider;
    uint8_t  format;
    uint8_t  accessMode;
    uint16_t mask;
} buffer_config_t;

typedef struct effect_config_s {
    buffer_config_t inputCfg;
    buffer_config_t outputCfg;
} effect_config_t;

typedef struct effect_interface_s **effect_handle_t;

struct effect_interface_s {
    int32_t (*process)(effect_handle_t self, audio_buffer_t *inBuffer, audio_buffer_t *outBuffer);
    int32_t (*command)(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize,
                        void *pCmdData, uint32_t *replySize, void *pReplyData);
    int32_t (*get_descriptor)(effect_handle_t self, effect_descriptor_t *pDescriptor);
    int32_t (*process_reverse)(effect_handle_t self, audio_buffer_t *inBuffer, audio_buffer_t *outBuffer);
};

typedef struct audio_effect_library_s {
    uint32_t tag;
    uint32_t version;
    const char *name;
    const char *implementor;
    int32_t (*create_effect)(const effect_uuid_t *uuid, int32_t sessionId, int32_t ioId, effect_handle_t *pHandle);
    int32_t (*release_effect)(effect_handle_t handle);
    int32_t (*get_descriptor)(const effect_uuid_t *uuid, effect_descriptor_t *pDescriptor);
} audio_effect_library_t;

#define AUDIO_EFFECT_LIBRARY_TAG (('A' << 24) | ('E' << 16) | ('L' << 8) | 'T')
#define EFFECT_LIBRARY_API_VERSION_CURRENT 0xFFFF0002u /* major=0xFFFF(any) style not used; see note below */
/* NOTE: real AOSP uses EFFECT_LIBRARY_API_VERSION_CURRENT = EFFECT_MAKE_API_VERSION(2,0) = 0x00020000.
 * Kept as a named constant here so it's easy to correct in one place if the
 * target device's EffectsFactory rejects this version. */
#undef EFFECT_LIBRARY_API_VERSION_CURRENT
#define EFFECT_LIBRARY_API_VERSION_CURRENT 0x00020000u

/* ---- per-instance context ---- */
typedef struct dsbridge_ctx_s {
    const struct effect_interface_s *itfe; /* MUST be first member */
    int fd;                 /* persistent connection to dsbridged for this instance */
    int32_t remoteInstanceId;
    uint32_t channels;
    uint32_t bytesPerSample;
    pthread_mutex_t lock;   /* one process()/command() at a time per instance */
} dsbridge_ctx_t;

static effect_descriptor_t g_cachedDescriptor;
static int g_haveCachedDescriptor = 0;
static pthread_mutex_t g_descLock = PTHREAD_MUTEX_INITIALIZER;

/* ---- socket helpers ---- */
static int dsbridge_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { LOGE("socket() failed: %s", strerror(errno)); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DSBRIDGE_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGE("connect(%s) failed: %s", DSBRIDGE_SOCK_PATH, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Send request, read header + payload of reply. *replyPayload must be freed by caller if non-NULL. */
static int dsbridge_txn(int fd, uint16_t op, int32_t instanceId,
                         const void *reqPayload, uint32_t reqPayloadSize,
                         dsbridge_msg_hdr_t *outHdr, unsigned char **outPayload) {
    dsbridge_msg_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = DSBRIDGE_MAGIC;
    hdr.version = DSBRIDGE_PROTO_VERSION;
    hdr.op = op;
    hdr.instanceId = instanceId;
    hdr.status = 0;
    hdr.payloadSize = reqPayloadSize;

    if (dsbridge_write_all(fd, &hdr, sizeof(hdr)) != 0) return -1;
    if (reqPayloadSize > 0 && dsbridge_write_all(fd, reqPayload, reqPayloadSize) != 0) return -1;

    if (dsbridge_read_all(fd, outHdr, sizeof(*outHdr)) != 0) return -1;
    if (outHdr->magic != DSBRIDGE_MAGIC) { LOGE("bad reply magic"); return -1; }

    *outPayload = NULL;
    if (outHdr->payloadSize > 0) {
        *outPayload = (unsigned char *)malloc(outHdr->payloadSize);
        if (!*outPayload) return -1;
        if (dsbridge_read_all(fd, *outPayload, outHdr->payloadSize) != 0) {
            free(*outPayload);
            *outPayload = NULL;
            return -1;
        }
    }
    return 0;
}

/* ---- effect_interface_s implementation ---- */

static int32_t ds_process(effect_handle_t self, audio_buffer_t *inBuffer, audio_buffer_t *outBuffer) {
    dsbridge_ctx_t *ctx = (dsbridge_ctx_t *)self;
    if (!ctx || !inBuffer || !outBuffer) return -EINVAL;

    pthread_mutex_lock(&ctx->lock);

    uint32_t channels = ctx->channels ? ctx->channels : 2;
    uint32_t bps = ctx->bytesPerSample ? ctx->bytesPerSample : 2;
    size_t sampleBytes = (size_t)inBuffer->frameCount * channels * bps;

    size_t reqSize = sizeof(dsbridge_process_req_t) + sampleBytes;
    unsigned char *req = (unsigned char *)malloc(reqSize);
    if (!req) { pthread_mutex_unlock(&ctx->lock); return -ENOMEM; }

    dsbridge_process_req_t preq;
    preq.frameCount = (uint32_t)inBuffer->frameCount;
    preq.channels = (uint16_t)channels;
    preq.bytesPerSample = (uint16_t)bps;
    memcpy(req, &preq, sizeof(preq));
    memcpy(req + sizeof(preq), inBuffer->raw, sampleBytes);

    dsbridge_msg_hdr_t replyHdr;
    unsigned char *replyPayload = NULL;
    int rc = dsbridge_txn(ctx->fd, DSB_OP_PROCESS, ctx->remoteInstanceId, req, (uint32_t)reqSize, &replyHdr, &replyPayload);
    free(req);

    if (rc != 0) {
        LOGE("process(): IPC failure, returning silence to avoid corrupt audio");
        memset(outBuffer->raw, 0, sampleBytes);
        pthread_mutex_unlock(&ctx->lock);
        return -EIO;
    }

    if (replyHdr.status != 0 || !replyPayload || replyHdr.payloadSize < sizeof(dsbridge_process_reply_t)) {
        LOGE("process(): daemon returned status=%d, returning silence", replyHdr.status);
        memset(outBuffer->raw, 0, sampleBytes);
        free(replyPayload);
        pthread_mutex_unlock(&ctx->lock);
        return replyHdr.status != 0 ? replyHdr.status : -EIO;
    }

    dsbridge_process_reply_t prep;
    memcpy(&prep, replyPayload, sizeof(prep));
    size_t outBytes = (size_t)prep.frameCount * prep.channels * prep.bytesPerSample;
    size_t copyBytes = outBytes < sampleBytes ? outBytes : sampleBytes;
    memcpy(outBuffer->raw, replyPayload + sizeof(prep), copyBytes);

    free(replyPayload);
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static int32_t ds_process_reverse(effect_handle_t self, audio_buffer_t *inBuffer, audio_buffer_t *outBuffer) {
    (void)self; (void)inBuffer; (void)outBuffer;
    return -ENOSYS; /* dsplus is a postprocess insert effect, reverse path unused */
}

static int32_t ds_command(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize,
                           void *pCmdData, uint32_t *replySize, void *pReplyData) {
    dsbridge_ctx_t *ctx = (dsbridge_ctx_t *)self;
    if (!ctx) return -EINVAL;

    pthread_mutex_lock(&ctx->lock);

    if (cmdCode == EFFECT_CMD_SET_CONFIG && cmdSize >= sizeof(effect_config_t) && pCmdData) {
        effect_config_t *cfg = (effect_config_t *)pCmdData;

        /* Remember channel/format for future process() calls. */
        if (cfg->outputCfg.channels) {
            /* channels field is a bitmask in real AOSP audio_channel_mask_t;
             * DS1/dsplus is stereo-only in practice. We only need a sample
             * COUNT here for framing, so approximate popcount for common
             * masks (mono=1, stereo=2). Extend if you see other masks in
             * logcat. */
            uint32_t ch = cfg->outputCfg.channels;
            uint32_t popcount = 0;
            while (ch) { popcount += (ch & 1); ch >>= 1; }
            ctx->channels = popcount ? popcount : 2;
        }
        ctx->bytesPerSample = 2; /* PCM16 assumed; adjust if format says otherwise */

        dsbridge_wire_config_t wire;
        memset(&wire, 0, sizeof(wire));
        wire.inputCfg.samplingRate = cfg->inputCfg.samplingRate;
        wire.inputCfg.channels     = cfg->inputCfg.channels;
        wire.inputCfg.format       = cfg->inputCfg.format;
        wire.inputCfg.accessMode   = cfg->inputCfg.accessMode;
        wire.inputCfg.mask         = cfg->inputCfg.mask;
        wire.inputCfg.hadProvider  = (cfg->inputCfg.mask & EFFECT_CONFIG_PROVIDER) ? 1 : 0;

        wire.outputCfg.samplingRate = cfg->outputCfg.samplingRate;
        wire.outputCfg.channels     = cfg->outputCfg.channels;
        wire.outputCfg.format       = cfg->outputCfg.format;
        wire.outputCfg.accessMode   = cfg->outputCfg.accessMode;
        wire.outputCfg.mask         = cfg->outputCfg.mask;
        wire.outputCfg.hadProvider  = (cfg->outputCfg.mask & EFFECT_CONFIG_PROVIDER) ? 1 : 0;

        dsbridge_msg_hdr_t replyHdr;
        unsigned char *replyPayload = NULL;
        int rc = dsbridge_txn(ctx->fd, DSB_OP_COMMAND_SET_CONFIG, ctx->remoteInstanceId,
                               &wire, sizeof(wire), &replyHdr, &replyPayload);
        free(replyPayload);
        pthread_mutex_unlock(&ctx->lock);

        if (replySize && pReplyData && *replySize >= sizeof(int32_t)) {
            *(int32_t *)pReplyData = (rc == 0) ? replyHdr.status : rc;
            *replySize = sizeof(int32_t);
        }
        return (rc == 0) ? replyHdr.status : rc;
    }

    /* Generic passthrough for everything else (INIT, ENABLE, DISABLE,
     * RESET, SET_PARAM, GET_PARAM, proprietary Dolby commands, ...).
     * Assumed POD (no embedded pointers). If a specific cmdCode turns out
     * to carry a pointer-containing struct, it needs the same special
     * treatment as SET_CONFIG above. */
    if (cmdSize > MAX_CMD_PAYLOAD_LOCAL || (replySize && *replySize > MAX_CMD_PAYLOAD_LOCAL)) {
        pthread_mutex_unlock(&ctx->lock);
        return -EMSGSIZE;
    }

    size_t reqSize = sizeof(dsbridge_cmd_generic_req_t) + cmdSize;
    unsigned char *req = (unsigned char *)malloc(reqSize);
    if (!req) { pthread_mutex_unlock(&ctx->lock); return -ENOMEM; }

    dsbridge_cmd_generic_req_t creq;
    creq.cmdCode = cmdCode;
    creq.cmdSize = cmdSize;
    creq.maxReplySize = replySize ? *replySize : 0;
    memcpy(req, &creq, sizeof(creq));
    if (cmdSize > 0) memcpy(req + sizeof(creq), pCmdData, cmdSize);

    dsbridge_msg_hdr_t replyHdr;
    unsigned char *replyPayload = NULL;
    int rc = dsbridge_txn(ctx->fd, DSB_OP_COMMAND_GENERIC, ctx->remoteInstanceId, req, (uint32_t)reqSize, &replyHdr, &replyPayload);
    free(req);

    if (rc == 0 && replyPayload && replyHdr.payloadSize >= sizeof(dsbridge_cmd_generic_reply_t)) {
        dsbridge_cmd_generic_reply_t rrep;
        memcpy(&rrep, replyPayload, sizeof(rrep));
        if (replySize && pReplyData) {
            uint32_t n = rrep.replySize < *replySize ? rrep.replySize : *replySize;
            if (n > 0) memcpy(pReplyData, replyPayload + sizeof(rrep), n);
            *replySize = n;
        }
    }
    free(replyPayload);
    pthread_mutex_unlock(&ctx->lock);
    return (rc == 0) ? replyHdr.status : rc;
}

static int32_t ds_get_descriptor(effect_handle_t self, effect_descriptor_t *pDescriptor) {
    (void)self;
    if (!pDescriptor) return -EINVAL;
    pthread_mutex_lock(&g_descLock);
    if (g_haveCachedDescriptor) {
        memcpy(pDescriptor, &g_cachedDescriptor, sizeof(*pDescriptor));
        pthread_mutex_unlock(&g_descLock);
        return 0;
    }
    pthread_mutex_unlock(&g_descLock);
    return -ENOSYS; /* library get_descriptor should have been called first */
}

static const struct effect_interface_s g_effect_itfe = {
    .process = ds_process,
    .command = ds_command,
    .get_descriptor = ds_get_descriptor,
    .process_reverse = ds_process_reverse,
};

/* ---- audio_effect_library_t (library-level) implementation ---- */

static int32_t lib_create_effect(const effect_uuid_t *uuid, int32_t sessionId, int32_t ioId, effect_handle_t *pHandle) {
    if (!uuid || !pHandle) return -EINVAL;

    int fd = dsbridge_connect();
    if (fd < 0) return -EIO;

    dsbridge_create_req_t creq;
    memcpy(creq.uuid_bytes, uuid, sizeof(creq.uuid_bytes));
    creq.sessionId = sessionId;
    creq.ioId = ioId;

    dsbridge_msg_hdr_t replyHdr;
    unsigned char *replyPayload = NULL;
    int rc = dsbridge_txn(fd, DSB_OP_CREATE, -1, &creq, sizeof(creq), &replyHdr, &replyPayload);
    free(replyPayload);

    if (rc != 0 || replyHdr.status != 0) {
        LOGE("CREATE failed rc=%d status=%d", rc, replyHdr.status);
        close(fd);
        return rc != 0 ? -EIO : replyHdr.status;
    }

    dsbridge_ctx_t *ctx = (dsbridge_ctx_t *)calloc(1, sizeof(dsbridge_ctx_t));
    if (!ctx) { close(fd); return -ENOMEM; }
    ctx->itfe = &g_effect_itfe;
    ctx->fd = fd;
    ctx->remoteInstanceId = replyHdr.instanceId;
    ctx->channels = 2;
    ctx->bytesPerSample = 2;
    pthread_mutex_init(&ctx->lock, NULL);

    LOGI("create_effect() -> local ctx=%p remoteInstance=%d", (void *)ctx, ctx->remoteInstanceId);
    *pHandle = (effect_handle_t)&ctx->itfe;
    return 0;
}

static int32_t lib_release_effect(effect_handle_t handle) {
    if (!handle) return -EINVAL;
    dsbridge_ctx_t *ctx = (dsbridge_ctx_t *)handle;

    dsbridge_msg_hdr_t replyHdr;
    unsigned char *replyPayload = NULL;
    dsbridge_txn(ctx->fd, DSB_OP_RELEASE, ctx->remoteInstanceId, NULL, 0, &replyHdr, &replyPayload);
    free(replyPayload);

    close(ctx->fd);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
    return 0;
}

static int32_t lib_get_descriptor(const effect_uuid_t *uuid, effect_descriptor_t *pDescriptor) {
    if (!uuid || !pDescriptor) return -EINVAL;

    int fd = dsbridge_connect();
    if (fd < 0) return -EIO;

    dsbridge_msg_hdr_t replyHdr;
    unsigned char *replyPayload = NULL;
    int rc = dsbridge_txn(fd, DSB_OP_GET_DESCRIPTOR, -1, uuid, sizeof(*uuid), &replyHdr, &replyPayload);
    close(fd);

    if (rc != 0 || replyHdr.status != 0 || !replyPayload || replyHdr.payloadSize < sizeof(effect_descriptor_t)) {
        LOGE("get_descriptor failed rc=%d status=%d", rc, replyHdr.status);
        free(replyPayload);
        return rc != 0 ? -EIO : (replyHdr.status != 0 ? replyHdr.status : -EIO);
    }

    memcpy(pDescriptor, replyPayload, sizeof(*pDescriptor));
    free(replyPayload);

    pthread_mutex_lock(&g_descLock);
    memcpy(&g_cachedDescriptor, pDescriptor, sizeof(g_cachedDescriptor));
    g_haveCachedDescriptor = 1;
    pthread_mutex_unlock(&g_descLock);

    return 0;
}

audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM_0_2 = {
    .tag = AUDIO_EFFECT_LIBRARY_TAG,
    .version = EFFECT_LIBRARY_API_VERSION_CURRENT,
    .name = "DS Bridge",
    .implementor = "dsbridge (community bridge, not Dolby)",
    .create_effect = lib_create_effect,
    .release_effect = lib_release_effect,
    .get_descriptor = lib_get_descriptor,
};
