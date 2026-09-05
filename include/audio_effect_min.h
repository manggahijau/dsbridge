/*
 * Minimal, hand-extracted subset of AOSP hardware/libhardware/include/hardware/audio_effect.h
 * Only what dsbridge needs. Layouts below MUST match the real header used to build
 * libdseffect.so (32-bit) and the one used by audioserver on the target device (64-bit).
 *
 * CRITICAL ABI NOTE:
 *   - effect_uuid_t and effect_descriptor_t are pure POD (no pointers, no size_t),
 *     so their in-memory layout is IDENTICAL on 32-bit and 64-bit builds.
 *     -> Safe to copy these directly across the IPC wire.
 *   - audio_buffer_t and buffer_config_t/effect_config_t contain a raw pointer
 *     and/or size_t and/or function pointers (buffer_provider_t). Their layout
 *     DIFFERS between 32-bit and 64-bit. NEVER memcpy these across the wire.
 *     dsbridge marshals only the scalar fields it actually needs and always
 *     reconstructs buffer pointers locally in whichever process uses them.
 */
#ifndef DSBRIDGE_AUDIO_EFFECT_MIN_H
#define DSBRIDGE_AUDIO_EFFECT_MIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- effect_uuid_t : 16 bytes, identical on 32/64-bit ---- */
typedef struct effect_uuid_s {
    uint32_t timeLow;
    uint16_t timeMid;
    uint16_t timeHiAndVersion;
    uint16_t clockSeq;
    uint8_t  node[6];
} effect_uuid_t;

/* ---- effect_descriptor_t : POD, identical on 32/64-bit ---- */
typedef struct effect_descriptor_s {
    effect_uuid_t type;
    effect_uuid_t uuid;
    uint32_t apiVersion;
    uint32_t flags;
    uint16_t cpuLoad;
    uint16_t memoryUsage;
    char     name[64];
    char     implementor[64];
} effect_descriptor_t;

/* DS1 dsplus effect identity (from device's own audio_effects.xml) */
#define DSPLUS_UUID_STR "9d4921da-8225-4f29-aefa-39537a04bcaa"

/* Effect command codes we explicitly understand (subset of audio_effect.h) */
#define EFFECT_CMD_INIT           0
#define EFFECT_CMD_SET_CONFIG     1
#define EFFECT_CMD_GET_CONFIG     2
#define EFFECT_CMD_RESET          3
#define EFFECT_CMD_ENABLE         4
#define EFFECT_CMD_DISABLE        5
#define EFFECT_CMD_SET_PARAM      6
#define EFFECT_CMD_SET_PARAM_COMMIT 7
#define EFFECT_CMD_SET_PARAM_DEFERRED 8
#define EFFECT_CMD_GET_PARAM      9
#define EFFECT_CMD_SET_DEVICE     10
#define EFFECT_CMD_SET_VOLUME     11
#define EFFECT_CMD_SET_AUDIO_MODE 12
#define EFFECT_CMD_FIRST_PROPRIETARY 0x10000

/* accessMode values */
#define EFFECT_BUFFER_ACCESS_WRITE 0
#define EFFECT_BUFFER_ACCESS_READ  1
#define EFFECT_BUFFER_ACCESS_ACCUMULATE 2

/* buffer_config_t.mask bits we care about */
#define EFFECT_CONFIG_BUFFER  0x0001
#define EFFECT_CONFIG_SMP_RATE 0x0002
#define EFFECT_CONFIG_CHANNELS 0x0004
#define EFFECT_CONFIG_FORMAT   0x0008
#define EFFECT_CONFIG_ACC_MODE 0x0010
#define EFFECT_CONFIG_PROVIDER 0x0020
#define EFFECT_CONFIG_ALL (EFFECT_CONFIG_BUFFER|EFFECT_CONFIG_SMP_RATE|EFFECT_CONFIG_CHANNELS|EFFECT_CONFIG_FORMAT|EFFECT_CONFIG_ACC_MODE|EFFECT_CONFIG_PROVIDER)

/*
 * Wire-safe scalar representation of ONE buffer_config_t.
 * Deliberately drops `buffer` (audio_buffer_t, pointer+size_t) and
 * `bufferProvider` (function pointers) -- see dsbridge_wire_config_t below.
 * If EFFECT_CONFIG_PROVIDER is set in the real mask, dsbridge logs a
 * loud warning: our assumption that dsplus never uses buffer_provider
 * callbacks would be violated and audio WILL be wrong (not necessarily
 * crash, but silently broken) until that path is implemented.
 */
typedef struct dsbridge_wire_buffer_config_s {
    uint32_t samplingRate;
    uint32_t channels;
    uint8_t  format;
    uint8_t  accessMode;
    uint16_t mask;
    uint8_t  hadProvider; /* 1 if original had EFFECT_CONFIG_PROVIDER set */
} dsbridge_wire_buffer_config_t;

typedef struct dsbridge_wire_config_s {
    dsbridge_wire_buffer_config_t inputCfg;
    dsbridge_wire_buffer_config_t outputCfg;
} dsbridge_wire_config_t;

#ifdef __cplusplus
}
#endif

#endif /* DSBRIDGE_AUDIO_EFFECT_MIN_H */
