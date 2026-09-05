/*
 * Full AOSP audio_effect.h struct subset, WITH the pointer-containing fields
 * (audio_buffer_t, buffer_provider_t). Only ever compiled into dsbridged
 * (the 32-bit daemon), which is the ONLY process that calls the real
 * libdseffect.so functions directly. Because this file is compiled natively
 * for whatever ABI dsbridged targets, the struct layout the compiler
 * produces here is automatically correct for that ABI -- no cross-ABI
 * marshaling concerns inside this file itself.
 *
 * Do NOT reuse these struct definitions on the 64-bit shim side, and do NOT
 * put instances of these structs (as raw bytes) on the dsbridge wire.
 */
#ifndef DSBRIDGE_AUDIO_EFFECT_FULL_H
#define DSBRIDGE_AUDIO_EFFECT_FULL_H

#include <stdint.h>
#include <stddef.h>
#include "../include/audio_effect_min.h" /* effect_uuid_t, effect_descriptor_t */

#ifdef __cplusplus
extern "C" {
#endif

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

typedef int32_t (*effect_create_fn_t)(const effect_uuid_t *uuid, int32_t sessionId,
                                       int32_t ioId, effect_handle_t *pHandle);
typedef int32_t (*effect_release_fn_t)(effect_handle_t handle);
typedef int32_t (*effect_get_descriptor_fn_t)(const effect_uuid_t *uuid, effect_descriptor_t *pDescriptor);

#ifdef __cplusplus
}
#endif

#endif /* DSBRIDGE_AUDIO_EFFECT_FULL_H */
