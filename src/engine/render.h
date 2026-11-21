/* render.h -- the mixer: interpolation, gain, saturating accumulate. */
#ifndef RENDER_H
#define RENDER_H

#include "msgs.h"
#include <stdint.h>

void render_frames(Msgs *e, int16_t *out, uint32_t frames);

#endif /* RENDER_H */
