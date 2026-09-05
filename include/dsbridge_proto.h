/*
 * dsbridge IPC protocol.
 *
 * Transport: one Unix domain SOCK_STREAM connection per effect instance
 * (the DsService only ever creates one instance in practice, but we don't
 * assume that -- daemon supports multiple concurrent connections/instances).
 *
 * Every message is: dsbridge_msg_hdr_t (fixed size) followed by
 * hdr.payloadSize bytes of payload (may be 0).
 *
 * Design rules (do not violate -- these exist to avoid 32/64-bit ABI bugs):
 *   1. Raw pointers are NEVER put on the wire. Not audio_buffer_t.raw,
 *      not buffer_provider_t callbacks, nothing. Only plain scalars and
 *      copied byte payloads cross the wire.
 *   2. effect_handle_t (the real interface pointer returned by the vendor
 *      .so's create_effect()) stays inside the 32-bit daemon process only.
 *      The 64-bit shim only ever sees a small integer dsbridge_instance_id.
 *   3. PCM sample data for PROCESS is copied into the message payload
 *      (a real memcpy across the socket). This is "Phase 1: correctness
 *      first" -- no shared memory / zero-copy yet. Expect this to be the
 *      first thing to optimize if you get underrun glitches once the
 *      control path (CREATE/RELEASE/COMMAND) is confirmed stable.
 */
#ifndef DSBRIDGE_PROTO_H
#define DSBRIDGE_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSBRIDGE_SOCK_PATH "/data/adb/modules/dsplus/dsbridge.sock"
#define DSBRIDGE_PROTO_VERSION 1
#define DSBRIDGE_MAX_PAYLOAD (256 * 1024) /* generous cap, sanity limit only */

typedef enum dsbridge_op_e {
    DSB_OP_HELLO = 1,        /* handshake: shim -> daemon, checks proto version */
    DSB_OP_GET_DESCRIPTOR,   /* shim -> daemon -> shim: effect_descriptor_t */
    DSB_OP_CREATE,           /* shim -> daemon: uuid + session/io ids -> instance id */
    DSB_OP_RELEASE,          /* shim -> daemon: instance id */
    DSB_OP_COMMAND_GENERIC,  /* shim -> daemon: raw cmdCode+bytes -> raw reply bytes
                                 (used for commands we don't special-case) */
    DSB_OP_COMMAND_SET_CONFIG, /* shim -> daemon: dsbridge_wire_config_t (marshaled) */
    DSB_OP_PROCESS,          /* shim -> daemon: frameCount + interleaved PCM in
                                 -> frameCount + interleaved PCM out */
    DSB_OP_PONG_ERROR = 0x7fff, /* daemon -> shim: something went wrong, see status */
} dsbridge_op_t;

typedef struct dsbridge_msg_hdr_s {
    uint32_t magic;       /* 'D''S''B''1' */
    uint16_t version;
    uint16_t op;           /* dsbridge_op_t */
    int32_t  instanceId;   /* -1 if not applicable yet (HELLO/GET_DESCRIPTOR/CREATE) */
    int32_t  status;       /* 0 == OK on replies; request messages set 0 */
    uint32_t payloadSize;  /* bytes following this header */
} dsbridge_msg_hdr_t;

#define DSBRIDGE_MAGIC 0x31425344u /* "DSB1" little-endian */

/* Payload for DSB_OP_CREATE (request) */
typedef struct dsbridge_create_req_s {
    uint8_t  uuid_bytes[16]; /* raw effect_uuid_t bytes, POD-safe */
    int32_t  sessionId;
    int32_t  ioId;
} dsbridge_create_req_t;

/* Payload for DSB_OP_CREATE (reply, status==0) : nothing extra, instanceId in header */

/* Payload for DSB_OP_COMMAND_GENERIC (request) */
typedef struct dsbridge_cmd_generic_req_s {
    uint32_t cmdCode;
    uint32_t cmdSize;     /* bytes of cmd data immediately following this struct */
    uint32_t maxReplySize;
    /* cmdSize bytes of raw command payload follow */
} dsbridge_cmd_generic_req_t;

/* Payload for DSB_OP_COMMAND_GENERIC (reply) */
typedef struct dsbridge_cmd_generic_reply_s {
    uint32_t replySize; /* bytes following this struct */
    /* replySize bytes of raw reply payload follow */
} dsbridge_cmd_generic_reply_t;

/* Payload for DSB_OP_PROCESS (request): fixed header + interleaved 16-bit PCM */
typedef struct dsbridge_process_req_s {
    uint32_t frameCount;
    uint16_t channels;   /* channel count, e.g. 2 */
    uint16_t bytesPerSample; /* e.g. 2 for PCM16 */
    /* frameCount * channels * bytesPerSample bytes of input PCM follow */
} dsbridge_process_req_t;

typedef struct dsbridge_process_reply_s {
    uint32_t frameCount;
    uint16_t channels;
    uint16_t bytesPerSample;
    /* frameCount * channels * bytesPerSample bytes of output PCM follow */
} dsbridge_process_reply_t;

#ifdef __cplusplus
}
#endif

#endif /* DSBRIDGE_PROTO_H */
