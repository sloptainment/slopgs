/* smf.h -- Standard MIDI File parser and sequencer. Not part of the original
 * driver -- ordinary SMF 0/1 parsing and a tick->sample
 * scheduler, no reverse-engineered behavior to match. */
#ifndef SMF_H
#define SMF_H

#include <stdint.h>

int smf_load(const uint8_t *data, uint32_t len); /* 0 = OK; rewinds */
/* back to tick 0, clears the finished latch */
void smf_rewind(void);
/* -1 = infinite, 0 = once, N = loop N extra times */
void smf_set_loop(int32_t loops);
uint32_t smf_render(int16_t *out, uint32_t frames);
int32_t smf_is_finished(void);

#endif /* SMF_H */
