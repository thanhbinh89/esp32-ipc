#include "g711.h"

/* Segment boundaries of the A-law companding curve, on the 12-bit magnitude that
 * remains after the >> 3. */
static const uint16_t seg_end[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};

uint8_t g711a_encode(int16_t sample) {
    uint8_t mask;
    uint8_t aval;
    uint16_t pcm;
    int seg;

    pcm = sample < 0 ? (uint16_t)(-sample - 1) : (uint16_t)sample;
    mask = sample >= 0 ? 0xD5 : 0x55;
    pcm >>= 3;

    for (seg = 0; seg < 8; seg++) {
        if (pcm <= seg_end[seg]) {
            break;
        }
    }

    if (seg >= 8) {
        return 0x7F ^ mask;
    }

    aval = (uint8_t)(seg << 4);
    if (seg < 2) {
        aval |= (pcm >> 1) & 0x0F;
    } else {
        aval |= (pcm >> seg) & 0x0F;
    }
    return aval ^ mask;
}

int16_t g711a_decode(uint8_t code) {
    int t;
    int seg;

    code ^= 0x55;

    /* Rebuild the quantised magnitude at the midpoint of its interval, then
     * shift it back up by the segment exponent. */
    t = (code & 0x0F) << 4;
    seg = (code & 0x70) >> 4;
    switch (seg) {
        case 0:
            t += 8;
            break;
        case 1:
            t += 0x108;
            break;
        default:
            t += 0x108;
            t <<= seg - 1;
            break;
    }

    return (int16_t)((code & 0x80) ? t : -t);
}
