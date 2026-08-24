/*
  Previous - sndfifo.h

  Kernel Hive host-native audio plane. See sndfifo.c.

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef PREV_SNDFIFO_H
#define PREV_SNDFIFO_H

#include <stdint.h>

extern void SndFifo_Init(void);
extern void SndFifo_UnInit(void);

/* One block of NeXT sound output, exactly as it goes to the host audio queue:
   signed 16-bit BIG endian, stereo interleaved, 44100 Hz. */
extern void SndFifo_Write(const uint8_t* data, int len);

#endif /* PREV_SNDFIFO_H */
