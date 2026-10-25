/* msgs.h -- public ABI for the freestanding wasm32 MIDI synth.
 * Fixed ABI shared by the wasm and native builds; pointer arguments are byte
 * offsets into the engine's memory. Do not change signatures. */
#ifndef MSGS_H
#define MSGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    uint32_t msgs_abi_version(void); /* returns 1 */
    /* bytes of linear memory required/used */
    uint32_t msgs_mem_size(void);
    /* bump allocator; returns offset into linear memory */
    uint32_t msgs_alloc(uint32_t nbytes);
    /* host places gm.dls first; 0 = OK */
    int32_t msgs_init(uint32_t dls_ptr, uint32_t dls_len);
    /* channels+voices to defaults AND rewinds the loaded song */
    void msgs_reset(void);
    int32_t msgs_load_smf(uint32_t smf_ptr, uint32_t smf_len); /* 0 = OK */
    /* -1 = infinite, 0 = play once */
    void msgs_set_loop(int32_t loops);
    /* stereo interleaved int16 @22050 Hz */
    uint32_t msgs_render(uint32_t out_ptr, uint32_t frames);
    int32_t msgs_is_finished(void);
    /* inject one short message immediately */
    void msgs_midi(uint32_t status, uint32_t d1, uint32_t d2);

#ifdef __cplusplus
}
#endif

#endif /* MSGS_H */
