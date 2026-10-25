/* render.h -- the mixer: interpolation, gain, saturating accumulate. */
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

void render_frames(int16_t *out, uint32_t frames);

#endif /* RENDER_H */
