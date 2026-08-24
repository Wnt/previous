/*
  Previous - sdlscreen.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains the SDL interface for video output.
*/
const char SDLscreen_fileid[] = "Previous sdlscreen.c";

#include "main.h"
#include "configuration.h"
#include "screen.h"
#include "sdlscreen.h"
#include "statusbar.h"
#include "sdlstatusbar.h"
#include "event.h"
#include "dimension.hpp"
#include "nd_sdl.hpp"
#include "video.h"
#include "keymap.h"
#include "m68000.h"


/* NeXT screen resolution */
const int NeXT_SCRN_W = 1120;
const int NeXT_SCRN_H = 832;

SDL_Window*   sdlWindow;
SDL_Surface*  sdlscrn = NULL;        /* The SDL screen surface */

/* extern for shortcuts */
volatile bool bGrabMouse    = false; /* Grab the mouse cursor in the window */
volatile bool bInFullScreen = false; /* true if in full screen */

/* extern for tablet */
int screen_w = NeXT_SCRN_W;
int screen_h = NeXT_SCRN_H;

static int width  = NeXT_SCRN_W;
static int height = NeXT_SCRN_H;

static SDL_Renderer* sdlRenderer;
static SDL_Texture*  uiTexture;
static SDL_Texture*  fbTexture;
static SDL_Texture*  groupTexture[NUM_MONITORS];
static SDL_FRect     uiRect;
static SDL_FRect     fbRect;
static SDL_FRect     groupRect[NUM_MONITORS];
static SDL_AtomicInt blitUI;
static SDL_Rect      statusBar;
static SDL_Rect      saveWindowBounds; /* Window bounds before going fullscreen. Used to restore window size & position. */
static SCREENMODE    saveScreenMode;   /* Save screen mode to restore on return from fullscreen */
static SCREENMODE    initScreenMode;   /* Save screen mode present at last init */
static int           initScreenWidth;  /* Save screen width at last init */
static int           initScreenHeight; /* Save screen height at last init */
static uint32_t      mask;             /* green screen mask for transparent UI areas */
static void*         uiBuffer;         /* uiBuffer used for user interface texture */
static SDL_SpinLock  uiBufferLock;     /* Lock for concurrent access to UI buffer between m68k thread and repainter */
#ifdef ENABLE_RENDERING_THREAD
static volatile bool doRepaint;        /* Repaint thread runs while true */
static SDL_Thread*   repaintThread;
#endif


static uint32_t BW2RGB[0x100][4];
static uint32_t COL2RGB[0x10000];

static uint32_t bw2rgb(SDL_Surface* surf, int bw) {
	int c = (~bw & 3) * 0x55;
	return SDL_MapSurfaceRGB(surf, c, c, c);
}

static uint32_t col2rgb(SDL_Surface* surf, int col) {
	int r = ((col >> 12) & 0x0F) * 0x11;
	int g = ((col >>  8) & 0x0F) * 0x11;
	int b = ((col >>  4) & 0x0F) * 0x11;
	return SDL_MapSurfaceRGB(surf, r, g, b);
}

/*
 BW format is 2 bit per pixel
 */

/* ---- Kernel Hive: IFB1 shm framebuffer export (PREVIOUS_SHM_PATH) ---------
 *
 * Publish each finished frame into a file-backed mapping so the streamhost
 * daemon can capture the emulated screen with no X server and no window (SDL
 * dummy video driver). Wire format and the reader's contract:
 * streamhost/streamhost/src/capture/shm.rs — 64-byte header, seqlock in the
 * u64 at offset 24 (ODD while pixels are being written, EVEN when stable),
 * dirty rect at 32..48, XRGB8888 pixels from 64. Inert unless the env is set.
 *
 * WHY NOT PUBLISH EVERY REPAINT. Previous repaints on its own cadence whether
 * or not the NeXT framebuffer moved, and a 1120x832 frame is 3.73 MB: at 60 Hz
 * an unconditional publish is ~224 MB/s of memcpy here, and every one of those
 * frames also costs the reader a full copy plus (SH_SHM_DAMAGE=1) a full-frame
 * diff, because the reader skips a frame only on an unchanged seqlock or an
 * empty dirty rect. Measured on an idle NeXTSTEP 3.3 Workspace: about 94% of
 * repaints carry no change at all. So:
 *
 *   pass 1  row-wise memcmp of the blitted frame against a PRIVATE shadow ->
 *           the changed row span, and within it a pixel-precise x span
 *           (forward/backward word scan, which early-exits on the first
 *           difference).
 *   pass 2  ONLY IF something changed: seq -> odd, copy the dirty region into
 *           both the mapping and the shadow, write the real dirty rect,
 *           seq -> even.
 *
 * An unchanged repaint never touches the seqlock, so the reader never wakes
 * for it. PREVIOUS_SHM_STATS=1 prints the counters; PREVIOUS_SHM_FULL=1
 * disables the diff entirely and publishes every repaint whole, which is the
 * control for "is this artefact the guest's or mine?" — the question a
 * damage-tracking producer must always be able to answer.
 *
 * THE DIFF MUST RUN ON THE PIXELS BEING PUBLISHED, NOT ON THE SOURCE. Diffing
 * the guest's own 2 bpp video memory instead is 16x less memory to touch and
 * measured 15x faster (59 us against 880 us per repaint) — and it is WRONG, in
 * a way that takes a control run to see. The blit reads NEXTVideo on the
 * rendering thread while the 68k keeps writing it, so a row that changes
 * between the blit and the diff is recorded into the shadow with its NEW
 * source bytes while the mapping receives the OLD pixels. Every later repaint
 * then finds that row "unchanged" and the mapping keeps the stale pixels
 * indefinitely. Observed as permanent bands of smeared window texture on the
 * desktop, absent under PREVIOUS_SHM_FULL=1 with the identical scene. Diffing
 * the blitted frame is self-consistent by construction: the bytes compared,
 * the bytes shadowed and the bytes published are the same bytes.
 *
 * THE FIRST PUBLISH IS ALWAYS WHOLE. The mapping is freshly ftruncate'd, so
 * every pixel in it is zero — which is not what a zero source row blits to.
 * A row that never changes would otherwise never be written and would stay
 * black for the life of the process.
 *
 * Because the rect is real, the integration should run streamhost with
 * SH_SHM_DAMAGE=0: the host-side diff exists to recover a bbox from a producer
 * that reports only whole-frame dirty, and repeating it there would be another
 * full pass over pixels this side has already classified.
 *
 * SEQLOCK EXACTNESS. The sequence counter is kept in a LOCAL variable and only
 * stored to the mapping, never read back from it: a reader cannot write it, but
 * neither should the producer trust a word another process can map. Both stores
 * are __ATOMIC_RELEASE, the odd store strictly precedes the first pixel write
 * and the even store strictly follows the last — including the dirty rect,
 * which the reader consumes as part of the frame.
 *
 * ONE PRODUCER. blitBW()/blitColor() run on the repaint thread only, so no lock
 * is needed; the shadow buffer and the counters are owned by that thread. */
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#define FBSHM_HEADER 64
#define FBSHM_MAGIC  0x31424649u /* 'IFB1' little-endian */

static uint8_t*  fbshm_map    = NULL;   /* the published mapping */
static uint8_t*  fbshm_shadow = NULL;   /* private copy of the last publish */
static uint64_t  fbshm_seq    = 0;      /* local seqlock counter, even = stable */
static int       fbshm_state  = 0;      /* 0 = untried, 1 = live, -1 = off */
static int       fbshm_w      = 0;
static int       fbshm_h      = 0;
static int       fbshm_stats  = 0;
static int       fbshm_full   = 0;      /* PREVIOUS_SHM_FULL: never diff */
static uint64_t  fbshm_pub    = 0;      /* frames published */
static uint64_t  fbshm_skip   = 0;      /* repaints found unchanged */
static uint64_t  fbshm_bytes  = 0;      /* pixel bytes copied */
static uint64_t  fbshm_ns     = 0;      /* time in publish (both passes) */

static uint64_t fbshm_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* One-time setup: size the file at the emulated geometry, map it, write the
 * header. Failure disables the export for the process rather than retrying per
 * frame — a station whose mapping cannot be created has a launcher bug, and a
 * retry loop in the repaint path would hide it behind a stutter. */
static void fbshm_open(void) {
	const char* path;
	size_t sz;
	int fd;

	fbshm_state = -1;
	path = SDL_getenv("PREVIOUS_SHM_PATH");
	if (!path || !*path) {
		return;
	}
	fbshm_w = NeXT_SCRN_W;
	fbshm_h = NeXT_SCRN_H;
	if (fbshm_w <= 0 || fbshm_h <= 0) {
		fprintf(stderr, "fbshm: refusing to publish %dx%d\n", fbshm_w, fbshm_h);
		return;
	}
	sz = FBSHM_HEADER + (size_t)fbshm_w * (size_t)fbshm_h * 4;
	fd = open(path, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		fprintf(stderr, "fbshm: open %s: %s\n", path, strerror(errno));
		return;
	}
	if (ftruncate(fd, (off_t)sz) != 0) {
		fprintf(stderr, "fbshm: ftruncate %s: %s\n", path, strerror(errno));
		close(fd);
		return;
	}
	fbshm_map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (fbshm_map == MAP_FAILED) {
		fbshm_map = NULL;
		fprintf(stderr, "fbshm: mmap %s: %s\n", path, strerror(errno));
		return;
	}
	fbshm_shadow = calloc((size_t)fbshm_w * (size_t)fbshm_h, 4);
	if (!fbshm_shadow) {
		munmap(fbshm_map, sz);
		fbshm_map = NULL;
		fprintf(stderr, "fbshm: out of memory for the %dx%d shadow\n", fbshm_w, fbshm_h);
		return;
	}
	/* The header is written BEFORE any frame and never again; the reader
	 * validates magic/version/bpp and sizes its own mapping from it. The
	 * seqlock starts at 0 (even, no frame yet) and the dirty rect empty, which
	 * the reader reads as "nothing to take". */
	((uint32_t*)(void*)fbshm_map)[0] = FBSHM_MAGIC;
	((uint32_t*)(void*)fbshm_map)[1] = 1;
	((uint32_t*)(void*)fbshm_map)[2] = (uint32_t)fbshm_w;
	((uint32_t*)(void*)fbshm_map)[3] = (uint32_t)fbshm_h;
	((uint32_t*)(void*)fbshm_map)[4] = (uint32_t)fbshm_w * 4;
	((uint32_t*)(void*)fbshm_map)[5] = 32;
	fbshm_stats = SDL_getenv("PREVIOUS_SHM_STATS") != NULL;
	fbshm_full  = SDL_getenv("PREVIOUS_SHM_FULL") != NULL;
	fbshm_state = 1;
	fprintf(stderr, "fbshm: publishing %dx%d to %s%s%s\n", fbshm_w, fbshm_h, path,
	        fbshm_stats ? " (stats on)" : "", fbshm_full ? " (full frames)" : "");
}

/* PREVIOUS_SHM_STATS: the measurement that justifies the diff. Printed from
 * BOTH the published and the skipped path — on an idle desktop almost every
 * repaint is a skip, and a counter that only printed when something changed
 * would never report the case it exists to measure. */
static void fbshm_report(void) {
	uint64_t n = fbshm_pub + fbshm_skip;

	if (n == 0 || (n % 600) != 0) {
		return;
	}
	fprintf(stderr, "fbshm: %llu repaints, %llu published, %llu unchanged, "
	        "%llu MB copied, %llu ns/repaint\n",
	        (unsigned long long)n, (unsigned long long)fbshm_pub,
	        (unsigned long long)fbshm_skip,
	        (unsigned long long)(fbshm_bytes >> 20),
	        (unsigned long long)(fbshm_ns / n));
}

static void fbshm_publish(const void* pixels, int pitch) {
	const uint8_t* src;
	uint8_t*  dst;
	uint32_t* dirty;
	size_t    row;
	uint64_t  t0 = 0;
	int y, y0, y1, x0, x1;

	if (fbshm_state == 0) {
		fbshm_open();
	}
	if (fbshm_state != 1) {
		return;
	}
	/* The geometry is baked into the mapping's size. NeXT_SCRN_W/H are const
	 * for the process, so this can only fire if that ever stops being true —
	 * in which case dropping the frame is right and a torn read is not. */
	if (NeXT_SCRN_W != fbshm_w || NeXT_SCRN_H != fbshm_h) {
		return;
	}
	if (fbshm_stats) {
		t0 = fbshm_now_ns();
	}

	src = (const uint8_t*)pixels;
	dst = fbshm_map + FBSHM_HEADER;
	row = (size_t)fbshm_w * 4;

	/* Pass 1: find the changed region. Reads only; nothing is published for a
	 * frame that turns out to be identical to the last one. The first publish
	 * of the process, and PREVIOUS_SHM_FULL, skip straight to a whole frame. */
	y0 = fbshm_h;
	y1 = -1;
	x0 = fbshm_w;
	x1 = -1;
	if (fbshm_full || fbshm_pub == 0) {
		y0 = 0; y1 = fbshm_h - 1;
		x0 = 0; x1 = fbshm_w - 1;
	} else {
		for (y = 0; y < fbshm_h; y++) {
			const uint32_t* s = (const uint32_t*)(const void*)(src + (size_t)y * pitch);
			const uint32_t* p = (const uint32_t*)(const void*)(fbshm_shadow + (size_t)y * row);
			int lo, hi;
			if (memcmp(s, p, row) == 0) {
				continue;
			}
			if (y < y0) y0 = y;
			if (y > y1) y1 = y;
			/* Narrow x only while it can still narrow: once the span is the
			 * whole width these scans cannot improve it, so skip them. */
			if (x0 == 0 && x1 == fbshm_w - 1) {
				continue;
			}
			for (lo = 0; lo < fbshm_w && s[lo] == p[lo]; lo++) { }
			for (hi = fbshm_w - 1; hi > lo && s[hi] == p[hi]; hi--) { }
			if (lo < x0) x0 = lo;
			if (hi > x1) x1 = hi;
		}
	}
	if (y1 < 0) {
		fbshm_skip++;
		if (fbshm_stats) {
			fbshm_ns += fbshm_now_ns() - t0;
			fbshm_report();
		}
		return;
	}

	/* Pass 2: publish. Odd first, even last, both released, dirty rect inside
	 * the critical section because the reader consumes it with the pixels. */
	__atomic_store_n((uint64_t*)(void*)(fbshm_map + 24), ++fbshm_seq, __ATOMIC_RELEASE);
	for (y = y0; y <= y1; y++) {
		const uint8_t* s = src + (size_t)y * pitch + (size_t)x0 * 4;
		size_t n = (size_t)(x1 - x0 + 1) * 4;
		memcpy(dst + (size_t)y * row + (size_t)x0 * 4, s, n);
		memcpy(fbshm_shadow + (size_t)y * row + (size_t)x0 * 4, s, n);
		fbshm_bytes += n;
	}
	dirty = (uint32_t*)(void*)(fbshm_map + 32);
	dirty[0] = (uint32_t)x0;
	dirty[1] = (uint32_t)y0;
	dirty[2] = (uint32_t)(x1 + 1); /* exclusive: the reader wants x1 > x0 */
	dirty[3] = (uint32_t)(y1 + 1);
	__atomic_store_n((uint64_t*)(void*)(fbshm_map + 24), ++fbshm_seq, __ATOMIC_RELEASE);

	fbshm_pub++;
	if (fbshm_stats) {
		fbshm_ns += fbshm_now_ns() - t0;
		fbshm_report();
	}
}
/* ---- end Kernel Hive fbshm ---- */

static void blitBW(SDL_Texture* tex) {
	void* pixels;
	uint8_t* src;
	uint8_t* dst;
	int pitch, src_padding, dst_padding, x, y;

	SDL_LockTexture(tex, NULL, &pixels, &pitch);
	src = NEXTVideo;
	dst = (uint8_t*)pixels;
	src_padding = ConfigureParams.System.bTurbo ? 0 : (32 / 4);
	dst_padding = pitch - NeXT_SCRN_W * 4;
	for (y = 0; y < NeXT_SCRN_H; y++) {
		for (x = 0; x < NeXT_SCRN_W / 4; x++) {
			memcpy(dst, BW2RGB[*src++], 16);
			dst += 16;
		}
		src += src_padding;
		dst += dst_padding;
	}
	fbshm_publish(pixels, pitch);
	SDL_UnlockTexture(tex);
}

/*
 Color format is 4 bit per pixel, big-endian: RGBX
 */
static void blitColor(SDL_Texture* tex) {
	void* pixels;
	uint16_t* src;
	uint32_t* dst;
	int pitch, src_padding, dst_padding, x, y;

	SDL_LockTexture(tex, NULL, &pixels, &pitch);
	src = (uint16_t*)NEXTVideo;
	dst = (uint32_t*)pixels;
	src_padding = ConfigureParams.System.bTurbo ? 0 : 32;
	dst_padding = pitch / 4 - NeXT_SCRN_W;
	for (y = 0; y < NeXT_SCRN_H; y++) {
		for (x = 0; x < NeXT_SCRN_W; x++) {
			*dst++ = COL2RGB[*src++];
		}
		src += src_padding;
		dst += dst_padding;
	}
	fbshm_publish(pixels, pitch);
	SDL_UnlockTexture(tex);
}

/*
 Dimension format is 8 bit per pixel, big-endian: BBGGRRAA
 */
void Screen_BlitDimension(uint32_t* vram, SDL_Texture* tex) {
	void* src;
	void* dst;
	int src_pitch, dst_pitch;
	SDL_PixelFormat src_format, dst_format;

#if ND_STEP
	src = &vram[0];
#else
	src = &vram[4];
#endif
	src_pitch  = (NeXT_SCRN_W + 32) * 4;
	src_format = SDL_PIXELFORMAT_BGRA32;
	dst_format = tex->format;

	SDL_LockTexture(tex, NULL, &dst, &dst_pitch);
	SDL_ConvertPixels(NeXT_SCRN_W, NeXT_SCRN_H, src_format, src, src_pitch, dst_format, dst, dst_pitch);
	SDL_UnlockTexture(tex);
}

/*
 Blank screen
 */
void Screen_Blank(SDL_Texture* tex) {
	void* pixels;
	int   pitch;
	SDL_LockTexture(tex, NULL, &pixels, &pitch);
	SDL_memset4(pixels, COL2RGB[0], pitch * NeXT_SCRN_H / 4);
	SDL_UnlockTexture(tex);
}

/*
 Blit NeXT framebuffer to texture.
 */
static bool blitScreen(int slot, SDL_Texture* tex) {
	if (slot > 0) {
		uint32_t* vram = nd_vram_for_slot(slot);
		if (vram) {
			if (nd_video_enabled(slot)) {
				Screen_BlitDimension(vram, tex);
			} else {
				Screen_Blank(tex);
			}
			return true;
		}
	} else {
		if (NEXTVideo) {
			if (Video_Enabled()) {
				if (ConfigureParams.System.bColor) {
					blitColor(tex);
				} else {
					blitBW(tex);
				}
			} else {
				Screen_Blank(tex);
			}
			return true;
		}
	}
	return false;
}

/*
 Blit user interface to texture.
 */
static void blitUserInterface(SDL_Texture* tex) {
	void* pixels;
	int   pitch;
	SDL_LockTexture(tex, NULL, &pixels, &pitch);
	SDL_LockSpinlock(&uiBufferLock);
	memcpy(pixels, uiBuffer, height * pitch);
	SDL_SetAtomicInt(&blitUI, 0);
	SDL_UnlockSpinlock(&uiBufferLock);
	SDL_UnlockTexture(tex);
}

/*
 Blits the NeXT framebuffer to the fbTexture, blends with the GUI surface and shows it.
 */
static bool Screen_SingleRepaint(void) {
	bool updateScreen = false;

	/* Blit the NeXT framebuffer to texture */
	if (bEmulationActive) {
		updateScreen = blitScreen(ConfigureParams.Screen.nSingleModeSlot, fbTexture);
	}

	/* Copy UI surface to texture */
	if (SDL_GetAtomicInt(&blitUI)) {
		blitUserInterface(uiTexture);
		updateScreen = true;
	}

	if (updateScreen) {
		SDL_RenderClear(sdlRenderer);
		/* Render NeXT framebuffer texture */
		SDL_RenderTexture(sdlRenderer, fbTexture, NULL, &fbRect);
		SDL_RenderTexture(sdlRenderer, uiTexture, NULL, &uiRect);
		/* Sleeps until next VSYNC if enabled in ScreenInit */
		SDL_RenderPresent(sdlRenderer);
	}

	return updateScreen;
}

static bool Screen_GroupRepaint(void) {
	bool updateScreen = false;
	int i;
	
	/* Blit the NeXT framebuffer to texture */
	if (bEmulationActive) {
		for (i = 0; i < NUM_MONITORS; i++) {
			if (groupTexture[i]) {
				if (blitScreen(i * 2, groupTexture[i])) {
					updateScreen = true;
				}
			}
		}
	}
	
	/* Copy UI surface to texture */
	if (SDL_GetAtomicInt(&blitUI)) {
		blitUserInterface(uiTexture);
		updateScreen = true;
	}
	
	if (updateScreen) {
		SDL_RenderClear(sdlRenderer);
		/* Render NeXT framebuffer texture */
		for (i = 0; i < NUM_MONITORS; i++) {
			if (groupTexture[i]) {
				SDL_RenderTexture(sdlRenderer, groupTexture[i], NULL, &groupRect[i]);
			}
		}
		SDL_RenderTexture(sdlRenderer, uiTexture, NULL, &uiRect);
		/* Sleeps until next VSYNC if enabled in ScreenInit */
		SDL_RenderPresent(sdlRenderer);
	}
	
	return updateScreen;
}

bool Screen_Repaint(void) {
	if (initScreenMode == SCREEN_GROUP) {
		return Screen_GroupRepaint();
	}
	return Screen_SingleRepaint();
}

#ifdef ENABLE_RENDERING_THREAD
static int repainter(void* unused) {
	SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL);

	/* Enter repaint loop */
	while (doRepaint) {
		if (!Screen_Repaint()) {
			SDL_Delay(10);
		}
	}
	return 0;
}
#endif

/*-----------------------------------------------------------------------*/
/**
 * Force repaint after window size or full screen change
 */
static void Screen_ForceRepaint(void) {
	SDL_SetAtomicInt(&blitUI, 1);
#ifndef ENABLE_RENDERING_THREAD
	if (!bEmulationActive) {
		Screen_Repaint();
	}
#endif
}

/*-----------------------------------------------------------------------*/
/**
 * Set Previous window title. Use NULL for default
 */
static void Screen_SetTitle(const char *title) {
	if (title)
		SDL_SetWindowTitle(sdlWindow, title);
	else
		SDL_SetWindowTitle(sdlWindow, PROG_NAME);
}

/*-----------------------------------------------------------------------*/
/**
 * Force things associated with changing between single and all screens.
 */
static void Screen_ModeChanged(void) {
	if (!sdlscrn) {
		/* screen not yet initialized */
		return;
	}

	/* Do not use multiple windows in full screen mode */
	if (bInFullScreen) {
		saveScreenMode = ConfigureParams.Screen.nMode;
		if (ConfigureParams.Screen.nMode == SCREEN_ALL) {
			ConfigureParams.Screen.nMode = SCREEN_SINGLE;
		}
	}
	if (ConfigureParams.Screen.nMode == SCREEN_ALL) {
		nd_sdl_show();
	} else {
		nd_sdl_hide();
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Calculate window size and position to fit into host display bounds.
 */
static void Screen_GetWindowBounds(SDL_Rect* r) {
	SDL_DisplayID d;
	SDL_Rect usable;

	float maxscale = 1.0;
	float scale    = 1.0;

	d = SDL_GetDisplayForWindow(sdlWindow);

	r->x = SDL_WINDOWPOS_CENTERED_DISPLAY(d);
	r->y = SDL_WINDOWPOS_CENTERED_DISPLAY(d);
	r->w = width;
	r->h = height;

	if (width == initScreenWidth && abs(height - initScreenHeight) < NeXT_SCRN_H) {
		int x, y, w, h;
		if (bInFullScreen) {
			r->x = saveWindowBounds.x;
			r->y = saveWindowBounds.y;
			scale = (float)saveWindowBounds.w / width;
		} else {
			if (SDL_GetWindowPosition(sdlWindow, &x, &y)) {
				r->x = x;
				r->y = y;
			}
			if (SDL_GetWindowSize(sdlWindow, &w, &h)) {
				scale = (float)w / width;
			}
		}
	}
	if (SDL_GetDisplayUsableBounds(d, &usable)) {
		int top, left, bottom, right;
		float hscale, wscale;
		if (SDL_GetWindowBordersSize(sdlWindow, &top, &left, &bottom, &right) == false) {
			/* No window manager: the window really has no decorations,
			 * so do not shrink the emulated screen to make room for
			 * imaginary ones (kernel-hive bridge kiosk). */
			top = bottom = left = right = 0;
		} else if (!ConfigureParams.Screen.bShowStatusbar) {
			bottom += 24; /* make sure there is enough space to show statusbar */
		}
		hscale = (float)(usable.h - top - bottom) / height;
		wscale = (float)(usable.w - left - right) / width;
		maxscale = wscale < hscale ? wscale : hscale;
	}
	if (scale > maxscale) {
		scale = maxscale;
		r->x = SDL_WINDOWPOS_CENTERED_DISPLAY(d);
		r->y = SDL_WINDOWPOS_CENTERED_DISPLAY(d);
	}
	if (scale > 0.0 && scale != 1.0) {
		fprintf(stderr, "SDL screen scale: %.3f\n", scale);
		r->w = (int)SDL_lroundf((float)r->w * scale);
		r->h = (int)SDL_lroundf((float)r->h * scale);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Create texture with default parameters.
 */
static SDL_Texture* Screen_CreateFramebufferTexture(SDL_PixelFormat format, int w, int h) {
	SDL_Texture* tex;
	
	tex = SDL_CreateTexture(sdlRenderer, format, SDL_TEXTUREACCESS_STREAMING, w, h);
	if (tex == NULL) {
		Main_ErrorExit("Failed to create texture:", SDL_GetError(), -1);
	}
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
	
	return tex;
}

/*-----------------------------------------------------------------------*/
/**
 * (Re-)initialise screen or handle mode change 
 */
void Screen_Reset(void) {
	int d, i;

	SDL_PixelFormat format = SDL_PIXELFORMAT_BGRA32;

#ifdef ENABLE_RENDERING_THREAD
	if (doRepaint) {
		doRepaint = false;
		SDL_WaitThread(repaintThread, &d);
	}
#endif

	/* Set initial window resolution */
	if (ConfigureParams.Screen.nMode == SCREEN_GROUP) {
		int xmax = 0;
		int ymax = 0;

		int xpos, ypos;

		for (i = 0; i < NUM_MONITORS; i++) {
			if (ConfigureParams.Screen.nGroupModePos[i] >= 0) {
				assert(ConfigureParams.Screen.nGroupModePos[i] < (NUM_MONITORS * NUM_MONITORS));

				xpos = ConfigureParams.Screen.nGroupModePos[i] % NUM_MONITORS;
				ypos = ConfigureParams.Screen.nGroupModePos[i] / NUM_MONITORS;

				xmax = xpos > xmax ? xpos : xmax;
				ymax = ypos > ymax ? ypos : ymax;

				groupRect[i].w = NeXT_SCRN_W;
				groupRect[i].h = NeXT_SCRN_H;
				groupRect[i].x = xpos * NeXT_SCRN_W;
				groupRect[i].y = ypos * NeXT_SCRN_H;
			}
		}
		screen_w = (xmax + 1) * NeXT_SCRN_W;
		screen_h = (ymax + 1) * NeXT_SCRN_H;
	} else {
		fbRect.x = 0;
		fbRect.y = 0;
		fbRect.w = NeXT_SCRN_W;
		fbRect.h = NeXT_SCRN_H;

		screen_w = NeXT_SCRN_W;
		screen_h = NeXT_SCRN_H;
	}

	width  = screen_w;
	height = screen_h;

	/* Grow to fit statusbar */
	height += Statusbar_SetHeight(screen_w, screen_h);

	/* Statusbar */
	statusBar.x = 0;
	statusBar.y = screen_h;
	statusBar.w = screen_w;
	statusBar.h = Statusbar_GetHeight();

	/* User interface including statusbar */
	uiRect.x = 0;
	uiRect.y = 0;
	uiRect.w = width;
	uiRect.h = height;

	/* Set new video mode only if necessary */
	if (width != initScreenWidth || height != initScreenHeight) {
		SDL_Rect windowBounds;
		uint32_t r, g, b, a;

		fprintf(stderr, "SDL screen request: %d x %d (%s)\n", width, height, bInFullScreen ? "fullscreen" : "windowed");

		Screen_GetWindowBounds(&windowBounds);

		SDL_SetWindowAspectRatio(sdlWindow, (float)width/height, (float)width/height);

		if (bInFullScreen) {
			/* If we are in full screen change saved window sizes */
			saveWindowBounds = windowBounds;
			SDL_SetRenderLogicalPresentation(sdlRenderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		} else {
			/* Set new window size */
			SDL_SetRenderLogicalPresentation(sdlRenderer, width, height, SDL_LOGICAL_PRESENTATION_STRETCH);
			SDL_SetWindowSize(sdlWindow, windowBounds.w, windowBounds.h);
			SDL_SetWindowPosition(sdlWindow, windowBounds.x, windowBounds.y);
		}

		/* (Re-)initialise UI texture */
		if (uiTexture) {
			SDL_DestroyTexture(uiTexture);
			uiTexture = NULL;
		}
		uiTexture = SDL_CreateTexture(sdlRenderer, format, SDL_TEXTUREACCESS_STREAMING, width, height);
		if (!uiTexture) {
			Main_ErrorExit("Failed to create texture:", SDL_GetError(), -1);
		}
		SDL_SetTextureBlendMode(uiTexture, SDL_BLENDMODE_BLEND);

		/* Get color masks */
		SDL_GetMasksForPixelFormat(format, &d, &r, &g, &b, &a);
		mask = g | a;

		/* (Re-)initialise UI surface */
		if (sdlscrn) {
			SDL_DestroySurface(sdlscrn);
			sdlscrn = NULL;
		}
		sdlscrn = SDL_CreateSurface(width, height, format);
		if (!sdlscrn) {
			Main_ErrorExit("Could not set video mode:", SDL_GetError(), -2);
		}

		/* Clear UI with mask */
		SDL_FillSurfaceRect(sdlscrn, NULL, mask);

		/* Allocate buffer for copy routines */
		if (uiBuffer) {
			free(uiBuffer);
			uiBuffer = NULL;
		}
		uiBuffer = calloc(1, sdlscrn->h * sdlscrn->pitch);
	}

	/* Handle mode change */
	if (ConfigureParams.Screen.nMode != initScreenMode) {
		Screen_ModeChanged();
	}

	/* Create framebuffer textures and start with blank screen */
	for (i = 0; i < NUM_MONITORS; i++) {
		if (ConfigureParams.Screen.nGroupModePos[i] < 0 || ConfigureParams.Screen.nMode != SCREEN_GROUP) {
			if (groupTexture[i]) {
				SDL_DestroyTexture(groupTexture[i]);
				groupTexture[i] = NULL;
			}
		} else if (groupTexture[i] == NULL) {
			groupTexture[i] = Screen_CreateFramebufferTexture(format, NeXT_SCRN_W, NeXT_SCRN_H);
			Screen_Blank(groupTexture[i]);
		}
	}
	if (ConfigureParams.Screen.nMode == SCREEN_GROUP) {
		if (fbTexture) {
			SDL_DestroyTexture(fbTexture);
			fbTexture = NULL;
		}
	} else if (fbTexture == NULL) {
		fbTexture = Screen_CreateFramebufferTexture(format, NeXT_SCRN_W, NeXT_SCRN_H);
		Screen_Blank(fbTexture);
	}

	/* Save mode and sizes */
	initScreenMode   = ConfigureParams.Screen.nMode;
	initScreenWidth  = width;
	initScreenHeight = height;

	/* Initialise statusbar and set visibility */
	if (ConfigureParams.Screen.bShowStatusbar) {
		Statusbar_Init(sdlscrn);
		Statusbar_Update(sdlscrn);
	}

#ifdef ENABLE_RENDERING_THREAD
	/* Start repaint thread */
	doRepaint = true;
	repaintThread = SDL_CreateThread(repainter, "[Previous] Screen at slot 0", NULL);
#endif

	/* Make sure screen is painted in case emulation is paused */
	Screen_ForceRepaint();
}

/*-----------------------------------------------------------------------*/
/**
 * Init Screen, create window, renderer and textures
 */
void Screen_Init(void) {
	int i;

	SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (SDL_CreateWindowAndRenderer(PROG_NAME, width, height, flags, &sdlWindow, &sdlRenderer) == false) {
		Main_ErrorExit("Failed to create window and renderer:", SDL_GetError(), -1);
	}
#ifdef ENABLE_RENDERING_THREAD
	SDL_SetRenderVSync(sdlRenderer, 1);
#endif

	/* Initialise textures and screen surface */
	Screen_Reset();

	/* Setup lookup tables */
	for (i = 0; i < 0x100; i++) {
		BW2RGB[i][0] = bw2rgb(sdlscrn, i>>6);
		BW2RGB[i][1] = bw2rgb(sdlscrn, i>>4);
		BW2RGB[i][2] = bw2rgb(sdlscrn, i>>2);
		BW2RGB[i][3] = bw2rgb(sdlscrn, i>>0);
	}
	for (i = 0; i < 0x10000; i++) {
		COL2RGB[SDL_Swap16BE(i)] = col2rgb(sdlscrn, i);
	}

	/* Set title, cursor visibility and mouse grab */
	Screen_SetTitle(NULL);
	Screen_ShowCursor(false);
	Screen_SetMouseGrab(bGrabMouse);

	/* Set titlebar visibility and change to fullscreen if requested */
	if (!ConfigureParams.Screen.bShowTitlebar) {
		Screen_TitlebarChanged();
	}
	if (ConfigureParams.Screen.bFullScreen) {
		Screen_EnterFullScreen();
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Free screen bitmap and allocated resources
 */
void Screen_UnInit(void) {
	int i;
#ifdef ENABLE_RENDERING_THREAD
	int s;
	doRepaint = false; /* stop repaint thread */
	SDL_WaitThread(repaintThread, &s);
#endif
	free(uiBuffer);
	SDL_DestroySurface(sdlscrn);
	SDL_DestroyTexture(uiTexture);
	if (fbTexture) {
		SDL_DestroyTexture(fbTexture);
	}
	for (i = 0; i < NUM_MONITORS; i++) {
		if (groupTexture[i]) {
			SDL_DestroyTexture(groupTexture[i]);
		}
	}
	SDL_DestroyRenderer(sdlRenderer);
	SDL_DestroyWindow(sdlWindow);
}

/*-----------------------------------------------------------------------*/
/**
 * Enter Full screen mode
 */
void Screen_EnterFullScreen(void) {
	bool bWasRunning;

	if (!bInFullScreen) {
		/* Hold things... */
		bWasRunning = Main_PauseEmulation(false);
		bInFullScreen = true;

		SDL_GetWindowPosition(sdlWindow, &saveWindowBounds.x, &saveWindowBounds.y);
		SDL_GetWindowSize(sdlWindow, &saveWindowBounds.w, &saveWindowBounds.h);
		SDL_SetRenderLogicalPresentation(sdlRenderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		SDL_SetWindowFullscreen(sdlWindow, true);
		SDL_SyncWindow(sdlWindow); /* To give monitor time to change to new resolution */

		/* If using multiple screen windows, save and go to single window mode */
		Screen_ModeChanged();

		if (bWasRunning) {
			/* And off we go... */
			Main_UnPauseEmulation();
		}

		/* Always grab mouse pointer in full screen mode */
		Screen_SetMouseGrab(true);

		/* Make sure screen is painted in case emulation is paused */
		Screen_ForceRepaint();
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Return from Full screen mode back to a window
 */
void Screen_ReturnFromFullScreen(void) {
	bool bWasRunning;

	if (bInFullScreen) {
		/* Hold things... */
		bWasRunning = Main_PauseEmulation(false);
		bInFullScreen = false;

		SDL_SetWindowFullscreen(sdlWindow, false);
		SDL_SyncWindow(sdlWindow); /* To give monitor time to switch resolution */
		SDL_SetWindowSize(sdlWindow, saveWindowBounds.w, saveWindowBounds.h);
		SDL_SetWindowPosition(sdlWindow, saveWindowBounds.x, saveWindowBounds.y);
		SDL_SetRenderLogicalPresentation(sdlRenderer, width, height, SDL_LOGICAL_PRESENTATION_STRETCH);

		/* Return to windowed monitor mode */
		if (saveScreenMode == SCREEN_ALL) {
			ConfigureParams.Screen.nMode = saveScreenMode;
			Screen_ModeChanged();
		}

		if (bWasRunning) {
			/* And off we go... */
			Main_UnPauseEmulation();
		}

		/* Go back to windowed mode mouse grab settings */
		Screen_SetMouseGrab(bGrabMouse);

		/* Make sure screen is painted in case emulation is paused */
		Screen_ForceRepaint();
	}
}

/* ----------------------------------------------------------------------- */
/**
 * Set mouse grab.
 */
void Screen_SetMouseGrab(bool grab) {
	/* If emulation is active, set the mouse cursor mode now: */
	if (grab) {
		if (bEmulationActive) {
			Screen_CenterCursor(); /* Cursor must be inside window */
			SDL_SetWindowRelativeMouseMode(sdlWindow, true);
			SDL_SetWindowKeyboardGrab(sdlWindow, true);
			SDL_SetWindowMouseGrab(sdlWindow, true);
			if (ConfigureParams.Mouse.bEnableAutoGrab) {
				Screen_SetTitle("Mouse is locked. Ctrl-click to release.");
			} else {
				char message[64];
				
				snprintf(message, sizeof(message), "Mouse is locked. Press ctrl-alt-%s to release.", 
						 Keymap_GetKeyName(ConfigureParams.Shortcut.withModifier[SHORTCUT_MOUSEGRAB]));
				Screen_SetTitle(message);
			}
		}
	} else {
		SDL_SetWindowRelativeMouseMode(sdlWindow, false);
		SDL_SetWindowKeyboardGrab(sdlWindow, false);
		SDL_SetWindowMouseGrab(sdlWindow, false);
		Screen_SetTitle(NULL);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Show main window
 */
void Screen_ShowMainWindow(void) {
	if (!bInFullScreen) {
		SDL_RestoreWindow(sdlWindow);
		SDL_RaiseWindow(sdlWindow);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Force things associated with changing screen size
 */
void Screen_SizeChanged(void) {
	int h;

	if (!bInFullScreen) {
		SDL_GetWindowSize(sdlWindow, NULL, &h);
		nd_sdl_resize((float)h/height);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Set visibilty of title bar.
 */
void Screen_TitlebarChanged(void) {
	if (sdlscrn && !bInFullScreen) {
		SDL_SetWindowBordered(sdlWindow, ConfigureParams.Screen.bShowTitlebar);
		nd_sdl_titlebar(ConfigureParams.Screen.bShowTitlebar);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Wrapper for Statusbar_AddMessage() and Statusbar_Update() in one go.
 */
void Screen_StatusbarMessage(const char *msg, uint32_t msecs)
{
	Statusbar_AddMessage(msg, msecs);
	Statusbar_Update(sdlscrn);
}

/*-----------------------------------------------------------------------*/
/**
 * Wrapper for Statusbar_Update().
 */
void Screen_StatusbarUpdate(void) {
	Statusbar_Update(sdlscrn);
}

/*-----------------------------------------------------------------------*/
/**
 * Check if we need to update full user interface or just the statusbar 
 * and copy user interface surface to buffer. Replace mask pixels with 
 * transparent pixels for blending with framebuffer texture.
 */
void Screen_UpdateRects(SDL_Surface *screen, int numrects, SDL_Rect *rects) {
	bool doUIblit = true;

	while (numrects--) {
		doUIblit = (rects->y < statusBar.y);
		if (doUIblit) {
			break;
		}
		rects++;
	}

	SDL_LockSurface(sdlscrn);
	SDL_LockSpinlock(&uiBufferLock);
	if (doUIblit) {
		/* Copy user interface surface and replace mask pixels. */
		int i;
		uint32_t* src = (uint32_t*)sdlscrn->pixels;
		uint32_t* dst = (uint32_t*)uiBuffer;
		/* Primitive green-screen - would be nice if SDL had more blending modes. */
		for (i = sdlscrn->w * sdlscrn->h; --i >= 0; src++) *dst++ = *src == mask ? 0 : *src;
	} else {
		/* Copy statusbar without transparent pixels. */
		void* src = (uint8_t*)sdlscrn->pixels + statusBar.y * sdlscrn->pitch;
		void* dst = (uint8_t*)uiBuffer + statusBar.y * sdlscrn->pitch;
		memcpy(dst, src, statusBar.h * sdlscrn->pitch);
	}
	SDL_SetAtomicInt(&blitUI, 1);
	SDL_UnlockSpinlock(&uiBufferLock);
	SDL_UnlockSurface(sdlscrn);

#ifndef ENABLE_RENDERING_THREAD
	if (!bEmulationActive) {
		Screen_Repaint();
	}
#endif
}

void Screen_UpdateRect(SDL_Surface *screen, int32_t x, int32_t y, int32_t w, int32_t h) {
	SDL_Rect rect = { x, y, w, h };
	Screen_UpdateRects(screen, 1, &rect);
}

/* ----------------------------------------------------------------------- */
/**
 * Set mouse cursor visibility and return if it was visible before.
 */
bool Screen_ShowCursor(bool show) {
	bool bOldVisibility;
	
	bOldVisibility = SDL_CursorVisible();
	if (bOldVisibility != show) {
		if (show) {
			SDL_ShowCursor();
		} else {
			SDL_HideCursor();
		}
	}
	return bOldVisibility;
}

/* ----------------------------------------------------------------------- */
/**
 * Set mouse cursor to the center of the screen.
 */
void Screen_CenterCursor(void) {
	SDL_WarpMouseInWindow(sdlWindow, sdlscrn->w/2, sdlscrn->h/2);
	GuiEvent_WarpMouse();
}
