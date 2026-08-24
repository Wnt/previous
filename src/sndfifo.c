/*
  Previous - sndfifo.c

  Kernel Hive host-native audio plane: publish the emulator's sound output as
  raw PCM on a named pipe, so the streamhost daemon can encode it with no host
  audio device, no PulseAudio and no D-Bus.

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.


  THE CONTRACT is streamhost's `SH_AUDIO_SOURCE=fifo` reader
  (streamhost/streamhost/src/audio.rs): a headerless stream of signed 16-bit
  LITTLE endian, stereo interleaved, 48000 Hz samples - 192,000 B/s - which the
  daemon paces itself, taking one 3840-byte Opus frame per 20 ms tick and
  letting pipe backpressure hold the producer. There is no negotiation and no
  header: the format is fixed by this contract on both sides.

  WHAT THE NeXT ACTUALLY PRODUCES is signed 16-bit BIG endian stereo at
  44100 Hz (snd.c: Audio_Output_Init(2, SND_CDDA_FREQUENCY), SDL_AUDIO_S16BE).
  The 22.05 kHz "double" modes are already expanded to 44.1 kHz by
  snd_make_double_samples() before this sees them, so 44100 is the only rate on
  this path. Two conversions are therefore unavoidable and both are done here,
  in the emulator, rather than asking the daemon for a per-station format:

    endianness  byte swap per sample.
    rate        44100 -> 48000 by linear interpolation on a 32.32 fixed-point
                phase accumulator, carried ACROSS calls (the last frame of one
                block is the left neighbour of the first output frame of the
                next), so block boundaries introduce no discontinuity. Linear
                interpolation is the right tool at a 1.088 ratio for a museum
                exhibit's chimes and key clicks; it is not, and does not claim
                to be, a mastering-grade resampler.

  THE LANDMINE THIS AVOIDS is the one the VICE conversion hit (see
  docs/lab/DEBRIDGE-HANDOVER.md): nothing drains a station's audio pipe until a
  visitor connects, 192 kB/s fills a 64 KB pipe in a third of a second, and a
  BLOCKING write then parks the emulator inside write() servicing nothing - no
  control socket, no checkpoint restore, no frames. So the pipe is opened
  O_NONBLOCK|O_RDWR (RDWR so that opening it never blocks waiting for a reader
  and a reader that goes away never raises SIGPIPE/EPIPE on us), and a write
  that does not fit is DROPPED. What is dropped is re-aligned to the 4-byte
  stereo frame boundary before the next write, so a partial write can never
  shift the stream's framing by a byte or two and swap the channels for the
  rest of the session. A full pipe costs audio, never the machine.
*/
const char SndFifo_fileid[] = "Previous sndfifo.c";

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "main.h"
#include "log.h"
#include "sndfifo.h"

#define FIFO_IN_RATE   44100
#define FIFO_OUT_RATE  48000
/* 32.32 fixed point step through the input stream, one output frame at a time. */
#define FIFO_STEP      (((uint64_t)FIFO_IN_RATE << 32) / FIFO_OUT_RATE)
/* Output staging. 4096 stereo frames = 16 KB, comfortably more than the
   largest block snd.c hands over in one go. */
#define FIFO_OUTFRAMES 4096

static int      fifo_fd = -1;
static int      fifo_partial;             /* bytes of a frame owed to alignment */
static uint64_t fifo_phase;               /* 32.32, position between prev and cur */
static int16_t  fifo_prev[2];             /* last input frame, the left neighbour */
static int      fifo_primed;
static uint64_t fifo_bytes, fifo_dropped;
static int      fifo_warned;

void SndFifo_Init(void) {
	const char* path = SDL_getenv("PREVIOUS_AUDIO_FIFO");

	if (!path || !*path) {
		return;
	}
	/* The launcher is expected to mkfifo(1) the path; create it if it is
	 * missing so a hand-run rig works too. An existing regular file is left
	 * alone and opened as-is (useful for capturing a reference recording). */
	if (mkfifo(path, 0644) != 0 && errno != EEXIST) {
		Log_Printf(LOG_WARN, "[SndFifo] mkfifo %s: %s", path, strerror(errno));
		return;
	}
	fifo_fd = open(path, O_RDWR | O_NONBLOCK);
	if (fifo_fd < 0) {
		Log_Printf(LOG_WARN, "[SndFifo] open %s: %s", path, strerror(errno));
		return;
	}
	fprintf(stderr, "sndfifo: %d Hz s16le stereo -> %s\n", FIFO_OUT_RATE, path);
}

void SndFifo_UnInit(void) {
	if (fifo_fd >= 0) {
		close(fifo_fd);
		fifo_fd = -1;
	}
}

/* Non-blocking write with frame-boundary repair. `len` is always a multiple of
 * 4 on entry; if the pipe takes only part of it, the remainder is discarded and
 * the byte count owed to get back onto a frame boundary is carried into the
 * next call. */
static void fifo_emit(const uint8_t* buf, size_t len) {
	ssize_t n;

	if (fifo_partial) {
		/* The previous write ended mid-frame. Skip forward to the next frame
		 * boundary rather than writing bytes that would swap L and R. */
		size_t skip = (size_t)(4 - fifo_partial);
		if (skip >= len) {
			fifo_partial = (fifo_partial + (int)len) & 3;
			return;
		}
		buf += skip;
		len -= skip;
		fifo_partial = 0;
	}
	n = write(fifo_fd, buf, len);
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && !fifo_warned) {
			fifo_warned = 1;
			Log_Printf(LOG_WARN, "[SndFifo] write: %s", strerror(errno));
		}
		fifo_dropped += len;
		return;
	}
	fifo_bytes += (size_t)n;
	if ((size_t)n < len) {
		fifo_dropped += len - (size_t)n;
		fifo_partial = (int)((size_t)n & 3);
	}
}

void SndFifo_Write(const uint8_t* data, int len) {
	uint8_t out[FIFO_OUTFRAMES * 4];
	int     inframes, i, o = 0;

	if (fifo_fd < 0 || len < 4) {
		return;
	}
	inframes = len >> 2;

	if (!fifo_primed) {
		/* Nothing to interpolate from on the very first block: start on its
		 * first frame with a zero phase. */
		fifo_prev[0] = (int16_t)((data[0] << 8) | data[1]);
		fifo_prev[1] = (int16_t)((data[2] << 8) | data[3]);
		fifo_phase   = 0;
		fifo_primed  = 1;
	}

	for (i = 0; i < inframes; i++) {
		int16_t cur[2];
		cur[0] = (int16_t)((data[i * 4 + 0] << 8) | data[i * 4 + 1]);
		cur[1] = (int16_t)((data[i * 4 + 2] << 8) | data[i * 4 + 3]);

		/* Emit every output frame whose position falls in [prev, cur). */
		while (fifo_phase < ((uint64_t)1 << 32)) {
			uint32_t f = (uint32_t)fifo_phase;   /* fraction, 0..2^32-1 */
			int      c;
			if (o >= FIFO_OUTFRAMES) {
				fifo_emit(out, (size_t)o * 4);
				o = 0;
			}
			for (c = 0; c < 2; c++) {
				int32_t a = fifo_prev[c];
				int32_t b = cur[c];
				int32_t v = a + (int32_t)(((int64_t)(b - a) * (int64_t)f) >> 32);
				/* Little endian is the daemon's contract, not the host's
				 * accident: emit the two bytes explicitly. */
				out[o * 4 + c * 2 + 0] = (uint8_t)(v & 0xFF);
				out[o * 4 + c * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
			}
			o++;
			fifo_phase += FIFO_STEP;
		}
		fifo_phase -= ((uint64_t)1 << 32);
		fifo_prev[0] = cur[0];
		fifo_prev[1] = cur[1];
	}
	if (o > 0) {
		fifo_emit(out, (size_t)o * 4);
	}
}
