/* msgs.h -- public ABI, shared by the wasm and native builds. Every instance
 * lives in one caller-supplied block and shares nothing with another, so any
 * number may run at once, one thread each. In the wasm build a pointer is an
 * offset into the module's linear memory. */
#ifndef MSGS_H
#define MSGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef __wasm__
#define MSGS_API __attribute__((visibility("default")))
#else
#define MSGS_API
#endif

/* the instance's memory block is full: give msgs_create more */
#define MSGS_ENOMEM (-3)

    typedef struct Msgs Msgs;

    MSGS_API uint32_t msgs_abi_version(void); /* returns 2 */
    /* bytes msgs_create needs to load gm.dls and any SMF up to max_smf_len */
    MSGS_API uint32_t msgs_size(uint32_t max_smf_len);
    /* mem: 8-byte aligned, outlives the instance; 0 if len is too small */
    MSGS_API Msgs *msgs_create(void *mem, uint32_t len);
    /* dls stays referenced, never copied; forgets any loaded song. 0 = OK */
    MSGS_API int32_t msgs_init(Msgs *e, const uint8_t *dls, uint32_t dls_len);
    /* channels+voices to defaults AND rewinds the loaded song */
    MSGS_API void msgs_reset(Msgs *e);
    /* smf stays referenced; replaces the previous song. 0 = OK */
    MSGS_API int32_t msgs_load_smf(Msgs *e, const uint8_t *smf,
                                   uint32_t smf_len);
    /* -1 = infinite, 0 = play once */
    MSGS_API void msgs_set_loop(Msgs *e, int32_t loops);
    /* stereo interleaved int16 @ msgs_sample_rate() */
    MSGS_API uint32_t msgs_render(Msgs *e, int16_t *out, uint32_t frames);
    MSGS_API int32_t msgs_is_finished(Msgs *e);
    /* inject one short message immediately */
    MSGS_API void msgs_midi(Msgs *e, uint32_t status, uint32_t d1, uint32_t d2);
    MSGS_API uint32_t msgs_sample_rate(void);

#ifdef __cplusplus
}
#endif

#endif /* MSGS_H */
