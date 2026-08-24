/*
  Previous - ctlsock.c

  Kernel Hive host-native input plane: a unix-socket control server that
  injects keyboard edges and ABSOLUTE pointer positions into the emulator's own
  KMS / SummaGraphics paths, so the museum's streamhost daemon can drive the
  guest with no X server, no window and no SDL input device.

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.


  WIRE PROTOCOL: mamectl/1, unchanged.
  ------------------------------------
  The museum already has a non-QEMU input sink that speaks a line protocol over
  a unix socket to an emulator-resident module: streamhost's `mamesock` backend
  (streamhost/streamhost/src/mame_sock.rs), built for MAME's ctlsock OSD module.
  Its sibling `vicesock` speaks a DIFFERENT dialect for VICE. Adding a third
  dialect would mean a third streamhost backend to write, health-model, trace
  and test. So this server speaks mamectl/1 verbatim; integration is env only:

      SH_INPUT_BACKEND=mamesock
      SH_MAMECTL_SOCK=<the same path as PREVIOUS_CTL_SOCK>
      SH_MAMESOCK_KEYMAP=<the nextstep keymap file, see KEYBOARD below>
      SH_SHM_DAMAGE=0            (the fbshm producer reports a real rect)
      SH_MAMESOCK_PTR_GRID       UNSET — targets are stated in screen pixels,
                                 which is exactly what this server wants.

  ZERO streamhost code changes. What the daemon sends, and what we answer:

      <-  HELLO mamectl/1 previous <machine> caps=... screen=WxH
      ->  <seq> MOVEA <x> <y>          absolute target, screen pixels
      ->  <seq> MOVEP <dx> <dy>        relative counts (resync slams)
      ->  <seq> DOWN1|UP1              left button
      ->  <seq> DOWN2|UP2              right button
      ->  <seq> DOWN3|UP3              middle - no such button on a NeXT, no-op
      ->  <seq> KEY <0|1> <port> <field>
      <-  <seq> OK   |   <seq> ERR <reason>

  ACKS PROVE THE EMULATOR, NOT THE SOCKET. A verb is acknowledged by the
  EMULATION thread, after it has been applied, not by the reader thread when it
  is parsed. A wedged 68k therefore stops acking and streamhost's ack-deadline
  declares the backend down, which is the entire reason that daemon moved from
  an append-only command file to a socket. The ack itself is written
  non-blocking: a peer that stops reading must never be able to stall
  emulation. (10-byte lines into a ~200 KB socket buffer; a drop is counted and
  logged, and costs at most one reconnect.)


  POINTER: absolute, through the tablet when the guest has it.
  -----------------------------------------------------------
  A NeXT mouse is a RELATIVE device (kms_mouse_move), so absolute targeting has
  two possible routes and this server picks per frame, at apply time:

  * TABLET (exact, preferred). When the guest driver has put the emulated
    SummaGraphics tablet into a streaming mode (`bTabletEnabled`), the tablet
    reports an ABSOLUTE position and summa_pen_move() maps screen pixels onto
    the tablet's own coordinate space. A MOVEA is then one call and the cursor
    lands on the commanded pixel. Nothing paces, nothing accumulates, nothing
    can drift.

  * KMS MOUSE (fallback, open loop). With no tablet the target is converged by
    dead reckoning: one homing slam into the top-left corner establishes a known
    origin (the guest clamps), then the residual is bled out at a bounded number
    of counts per drain tick. The pacing is not decoration: kms_mouse_move()
    clamps a report to +-63 counts, and NeXTSTEP applies pointer acceleration
    above a small per-report threshold, so a large jump delivered as one report
    is both truncated AND scaled. Small paced steps stay under the threshold and
    move 1:1. PREVIOUS_CTL_PTR_STEP tunes the step (default 16 counts/tick at
    the 200 Hz drain = 3200 px/s).

    On this route a button edge WAITS for convergence — the queue head is held,
    not applied — up to PREVIOUS_CTL_PTR_SETTLE ms, so a click cannot land at a
    stale position. That is also why the edge ack is late, which streamhost's
    ack model already allows for (`paced` verbs get an extra allowance each).

  PREVIOUS_CTL_PTR=tablet|kms|auto (default auto) forces a route for testing.

  BUTTON HOLD FLOOR. A ~12 ms press/release on the tablet path is sampled away
  entirely - a menu item highlights and never fires, a title-bar drag does not
  move the window - while an explicit 400 ms hold works every time (measured on
  the live station, docs/guests/nextstep.md §4). A browser forwards a visitor's
  real edges and a quick click is a quick click, so the floor belongs here at
  the injector, the same way MAME_CTL_KEY_EXCL serialises keys for the matrix
  guests. A RELEASE arriving early waits in the queue, in order, until
  PREVIOUS_CTL_BTN_HOLD ms have passed since its press (default 400, 0 = off).


  KEYBOARD: the NeXT KMS scancode space, plus a modifier MASK.
  -----------------------------------------------------------
  The NeXT keyboard is a serial KMS device, not a matrix, and Previous models it
  exactly as the hardware does: kms_keydown(modmask, keycode). Modifier keys are
  NOT keycodes - they are bits in the mask that accompanies every edge
  (NEXTKEY_MOD_*, kms.h), which is why sdlkeymap.c resolves an SDL scancode to
  NEXTKEY_NONE for Shift and lets Keymap_GetModifiers() carry it.

  mamectl/1's KEY verb takes a (port, field) pair, which is MAME's port/field
  naming - but streamhost never interprets it: `SH_MAMESOCK_KEYMAP` is a
  per-station file of `scancode<TAB>port<TAB>field` rows and the two strings are
  passed through opaquely. So this server defines the pair as:

      port `kms`  field = NeXT scancode, hex (`0x39` or `39`, kms.h NEXTKEY_*)
      port `mod`  field = one modifier bit by name:
                          meta lshift rshift lctrl rctrl lalt ralt
                          aliases, by the legend printed on a NeXT keyboard:
                          control=meta  lcommand=lctrl  rcommand=rctrl
                          lalternate=lalt  ralternate=ralt

  and the station's keymap file maps the browser's XT set 1 scancodes onto them,
  e.g.

      1e	kms	0x39	# A
      2a	mod	lshift
      1d	mod	meta	# NeXT Control

  Nothing here is guessed at run time: a `port`/`field` this server does not
  recognise is answered ERR and the key is dropped, never folded onto a
  neighbour. Hold timing is the browser's, exactly as on the QEMU stations - the
  guest generates its own auto-repeat from the held state.


  THREADING. One reader thread owns the socket and does nothing but parse and
  enqueue. Every emulator call (kms_*, tablet_*) happens in CtlSock_Drain(),
  which main.c calls from Main_EventHandler() - the emulation thread, at 200 Hz,
  the same place and thread that drains SDL's own events. The queue is a
  single-producer/single-consumer ring behind an SDL spinlock, the same shape
  sdlevent.c already uses for its cross-thread event queue.
*/
const char CtlSock_fileid[] = "Previous ctlsock.c";

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <SDL3/SDL.h>

#include "main.h"
#include "configuration.h"
#include "log.h"
#include "screen.h"
#include "kms.h"
#include "tablet.h"
#include "ctlsock.h"

/* ------------------------------------------------------------------ queue */

enum {
	CTL_NOP = 0,
	CTL_MOVEA,   /* a = x, b = y (screen pixels) */
	CTL_MOVEP,   /* a = dx, b = dy (counts) */
	CTL_BTN,     /* a = 1|2|3, b = down */
	CTL_KEY,     /* a = NeXT keycode, b = down */
	CTL_MOD,     /* a = NEXTKEY_MOD_* bit, b = down */
	CTL_RELEASE  /* peer went away: drop everything the guest is holding */
};

struct ctl_cmd {
	uint64_t seq;      /* 0 = do not acknowledge (internal command) */
	int      verb;
	int      a, b;
};

#define CTL_QUEUE 256

static struct ctl_cmd ctl_q[CTL_QUEUE];
static int            ctl_head;          /* consumer */
static int            ctl_tail;          /* producer */
static SDL_SpinLock   ctl_lock;

/* ----------------------------------------------------------------- config */

enum { PTR_AUTO = 0, PTR_TABLET, PTR_KMS };

static int ctl_ptr_mode   = PTR_AUTO;
static int ctl_ptr_step   = 16;    /* counts per drain tick on the kms route */
static int ctl_ptr_settle = 1200;  /* ms a button edge waits for convergence */
static int ctl_ptr_rate   = 0;     /* min ms between kms reports, 0 = every tick */
static int ctl_btn_hold   = 400;   /* min ms a button stays down before release */

/* ------------------------------------------------------------------ state */

static SDL_Thread*  ctl_thread;
static int          ctl_listen_fd = -1;
static SDL_AtomicInt ctl_conn_fd;        /* accepted connection, -1 = none */
static SDL_AtomicInt ctl_quit;
static char         ctl_path[108];   /* sized like sockaddr_un.sun_path */

/* Guest-facing state, owned by the emulation thread. */
static uint8_t ctl_mods;                 /* NEXTKEY_MOD_* mask */
static int     ctl_btn_left, ctl_btn_right;
static int     ctl_tgt_x, ctl_tgt_y;     /* commanded target */
static int     ctl_cur_x, ctl_cur_y;     /* believed position (kms route) */
static int     ctl_homed;                /* kms route: origin established */
static int     ctl_have_target;          /* a MOVEA has been commanded at all */
static int     ctl_home_left;            /* homing slams still to send */
static uint64_t ctl_wait_since;          /* ms an edge has been waiting */
static uint64_t ctl_last_report;         /* ms of the last kms mouse report */
static uint64_t ctl_btn_down_at[4];      /* ms a button went down, by number */
static int      ctl_route = -1;          /* last route reported, for the log */
static uint64_t ctl_acks_dropped;

/* ------------------------------------------------------------- ack writer */

/* Non-blocking, because this runs on the emulation thread. A peer that has
 * stopped reading costs an ack (and therefore, eventually, one reconnect);
 * it must never cost a stalled 68k. */
static void ctl_reply(uint64_t seq, const char* kind, const char* detail) {
	char line[160];
	int  n, fd;

	if (seq == 0) {
		return;
	}
	fd = SDL_GetAtomicInt(&ctl_conn_fd);
	if (fd < 0) {
		return;
	}
	if (detail && *detail) {
		n = snprintf(line, sizeof(line), "%llu %s %s\n",
		             (unsigned long long)seq, kind, detail);
	} else {
		n = snprintf(line, sizeof(line), "%llu %s\n", (unsigned long long)seq, kind);
	}
	if (n <= 0) {
		return;
	}
	if (send(fd, line, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL) != n) {
		if (++ctl_acks_dropped <= 8) {
			Log_Printf(LOG_WARN, "[CtlSock] dropped ack %llu (%s)",
			           (unsigned long long)seq, strerror(errno));
		}
	}
}

/* ------------------------------------------------------------ enqueue side */

/* Returns false when the ring is full; the caller answers ERR rather than
 * silently dropping the verb, so streamhost re-derives the state it lost. */
static bool ctl_push(uint64_t seq, int verb, int a, int b) {
	int next;
	bool ok = false;

	SDL_LockSpinlock(&ctl_lock);
	next = ctl_tail + 1;
	if (next >= CTL_QUEUE) {
		next = 0;
	}
	if (next != ctl_head) {
		ctl_q[ctl_tail].seq  = seq;
		ctl_q[ctl_tail].verb = verb;
		ctl_q[ctl_tail].a    = a;
		ctl_q[ctl_tail].b    = b;
		ctl_tail = next;
		ok = true;
	}
	SDL_UnlockSpinlock(&ctl_lock);
	return ok;
}

/* ------------------------------------------------------------------ parse */

struct mod_name { const char* name; uint8_t bit; };

static const struct mod_name ctl_mod_names[] = {
	{ "meta",       NEXTKEY_MOD_META   },
	{ "control",    NEXTKEY_MOD_META   },  /* the key labelled Control */
	{ "lshift",     NEXTKEY_MOD_LSHIFT },
	{ "rshift",     NEXTKEY_MOD_RSHIFT },
	{ "lctrl",      NEXTKEY_MOD_LCTRL  },
	{ "rctrl",      NEXTKEY_MOD_RCTRL  },
	{ "lcommand",   NEXTKEY_MOD_LCTRL  },  /* the key labelled Command */
	{ "rcommand",   NEXTKEY_MOD_RCTRL  },
	{ "lalt",       NEXTKEY_MOD_LALT   },
	{ "ralt",       NEXTKEY_MOD_RALT   },
	{ "lalternate", NEXTKEY_MOD_LALT   },  /* the key labelled Alternate */
	{ "ralternate", NEXTKEY_MOD_RALT   },
	{ NULL, 0 }
};

static int ctl_parse_hex(const char* s, int* out) {
	char* end;
	long  v;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	errno = 0;
	v = strtol(s, &end, 16);
	if (errno || end == s || *end || v < 0 || v > 0xFF) {
		return 0;
	}
	*out = (int)v;
	return 1;
}

static int ctl_parse_int(const char* s, int* out) {
	char* end;
	long  v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end) {
		return 0;
	}
	*out = (int)v;
	return 1;
}

/* One received line. Returns an ERR reason, or NULL when the verb was queued
 * (its OK is written later, by the emulation thread that applies it). */
static const char* ctl_dispatch(uint64_t seq, char* rest) {
	char* tok[4] = { NULL, NULL, NULL, NULL };
	int   n = 0;
	char* p = rest;

	while (n < 3 && p && *p) {
		while (*p == ' ') p++;
		if (!*p) break;
		tok[n++] = p;
		p = strchr(p, ' ');
		if (p) *p++ = '\0';
	}
	/* the KEY verb's field is the rest of the line (MAME field names contain
	 * spaces); ours do not, but keep the contract. */
	if (n == 3 && p && *p) {
		while (*p == ' ') p++;
		tok[3] = p;
	}
	if (!tok[0]) {
		return "empty";
	}

	if (!strcmp(tok[0], "MOVEA") || !strcmp(tok[0], "MOVEP")) {
		int x, y;
		if (!tok[1] || !tok[2] || !ctl_parse_int(tok[1], &x) || !ctl_parse_int(tok[2], &y)) {
			return "bad coords";
		}
		if (!ctl_push(seq, tok[0][4] == 'A' ? CTL_MOVEA : CTL_MOVEP, x, y)) {
			return "queue full";
		}
		return NULL;
	}
	if ((!strncmp(tok[0], "DOWN", 4) || !strncmp(tok[0], "UP", 2)) && !tok[1]) {
		int down = tok[0][0] == 'D';
		const char* d = tok[0] + (down ? 4 : 2);
		if (d[0] < '1' || d[0] > '3' || d[1]) {
			return "bad button";
		}
		if (!ctl_push(seq, CTL_BTN, d[0] - '0', down)) {
			return "queue full";
		}
		return NULL;
	}
	if (!strcmp(tok[0], "KEY")) {
		int down, code;
		const struct mod_name* m;
		if (!tok[1] || !tok[2] || !tok[3]) {
			return "want KEY <0|1> <port> <field>";
		}
		if (!ctl_parse_int(tok[1], &down) || (down != 0 && down != 1)) {
			return "bad edge";
		}
		if (!strcmp(tok[2], "kms")) {
			if (!ctl_parse_hex(tok[3], &code)) {
				return "bad keycode";
			}
			if (!ctl_push(seq, CTL_KEY, code, down)) {
				return "queue full";
			}
			return NULL;
		}
		if (!strcmp(tok[2], "mod")) {
			for (m = ctl_mod_names; m->name; m++) {
				if (!strcmp(tok[3], m->name)) {
					if (!ctl_push(seq, CTL_MOD, m->bit, down)) {
						return "queue full";
					}
					return NULL;
				}
			}
			return "unknown modifier";
		}
		return "unknown port (want kms|mod)";
	}
	if (!strcmp(tok[0], "PING")) {
		if (!ctl_push(seq, CTL_NOP, 0, 0)) {
			return "queue full";
		}
		return NULL;
	}
	return "unknown verb";
}

/* ---------------------------------------------------------- reader thread */

static void ctl_write_all(int fd, const char* s) {
	size_t left = strlen(s);
	while (left) {
		ssize_t n = send(fd, s, left, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			return;
		}
		s += n;
		left -= (size_t)n;
	}
}

static void ctl_serve(int fd) {
	char buf[1024];
	size_t len = 0;

	for (;;) {
		ssize_t got;
		char*   nl;

		got = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
		if (got <= 0) {
			if (got < 0 && errno == EINTR) continue;
			return;
		}
		len += (size_t)got;
		buf[len] = '\0';

		while ((nl = memchr(buf, '\n', len)) != NULL) {
			size_t used = (size_t)(nl - buf) + 1;
			char*  sp;
			uint64_t seq = 0;
			const char* err;

			*nl = '\0';
			if (nl > buf && nl[-1] == '\r') {
				nl[-1] = '\0';
			}
			sp = strchr(buf, ' ');
			if (sp) {
				*sp = '\0';
				seq = strtoull(buf, NULL, 10);
				err = ctl_dispatch(seq, sp + 1);
			} else {
				err = "want <seq> <verb>";
			}
			if (err) {
				char line[192];
				snprintf(line, sizeof(line), "%llu ERR %s\n",
				         (unsigned long long)seq, err);
				ctl_write_all(fd, line);
			}
			memmove(buf, buf + used, len - used);
			len -= used;
		}
		if (len >= sizeof(buf) - 1) {
			Log_Printf(LOG_WARN, "[CtlSock] over-long line; dropping peer");
			return;
		}
	}
}

static int SDLCALL ctl_thread_main(void* unused) {
	char hello[256];

	(void)unused;
	while (!SDL_GetAtomicInt(&ctl_quit)) {
		int fd = accept(ctl_listen_fd, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR) continue;
			return 0;   /* listener closed by CtlSock_UnInit */
		}
		SDL_SetAtomicInt(&ctl_conn_fd, fd);
		snprintf(hello, sizeof(hello),
		         "HELLO mamectl/1 previous next68k caps=abs,kbd,tablet screen=%dx%d\n",
		         screen_w, screen_h);
		ctl_write_all(fd, hello);
		Log_Printf(LOG_WARN, "[CtlSock] client connected");

		ctl_serve(fd);

		SDL_SetAtomicInt(&ctl_conn_fd, -1);
		close(fd);
		Log_Printf(LOG_WARN, "[CtlSock] client gone; releasing held input");
		ctl_push(0, CTL_RELEASE, 0, 0);
	}
	return 0;
}

/* ----------------------------------------------------------- apply (emul) */

static int ctl_use_tablet(void) {
	if (ctl_ptr_mode == PTR_KMS) {
		return 0;
	}
	if (ctl_ptr_mode == PTR_TABLET) {
		return 1;
	}
	return ConfigureParams.Tablet.nTabletType && bTabletEnabled;
}

/* Which route absolute motion is taking, announced on stderr whenever it
 * changes: the guest can enable or disable its tablet driver at any moment and
 * the answer decides whether the pointer is exact or dead reckoned. */
static int ctl_route_now(void) {
	int t = ctl_use_tablet();
	if (t != ctl_route) {
		ctl_route = t;
		fprintf(stderr, "ctlsock: pointer route = %s\n", t ? "tablet (absolute)" : "kms mouse (dead reckoned)");
	}
	return t;
}

static int ctl_clampi(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static void ctl_apply_buttons(void) {
	if (ctl_use_tablet()) {
		tablet_pen_button(1, ctl_btn_left);
		tablet_pen_button(0, ctl_btn_right);
	} else {
		kms_mouse_button(true,  ctl_btn_left  ? true : false);
		kms_mouse_button(false, ctl_btn_right ? true : false);
	}
}

/* True when the guest pointer is where it was told to be. Always true on the
 * tablet route: the position IS the command. */
static int ctl_ptr_settled(void) {
	if (ctl_use_tablet()) {
		return 1;
	}
	/* Nothing has been commanded yet, so there is nothing to converge on and
	 * nothing to wait for. Without this the reconnect preamble's UP1/UP2/UP3 -
	 * which streamhost sends BEFORE its first MOVEA - would each burn the full
	 * settle window on the dead-reckoned route. */
	if (!ctl_have_target) {
		return 1;
	}
	return ctl_homed && ctl_home_left == 0 &&
	       ctl_cur_x == ctl_tgt_x && ctl_cur_y == ctl_tgt_y;
}

/* One tick of the kms dead-reckoning engine. Nothing to do on the tablet
 * route, and nothing to do once converged. */
static void ctl_ptr_tick(void) {
	int dx, dy, step;

	if (ctl_use_tablet() || ctl_ptr_settled()) {
		return;
	}
	if (ctl_ptr_rate > 0) {
		uint64_t now = SDL_GetTicks();
		if (now - ctl_last_report < (uint64_t)ctl_ptr_rate) {
			return;
		}
		ctl_last_report = now;
	}
	step = ctl_ptr_step;
	if (ctl_home_left > 0) {
		/* Slam into the corner; the guest clamps, so an overshoot is free and
		 * a single report cannot carry more than 63 counts. */
		kms_mouse_move(-63, -63);
		if (--ctl_home_left == 0) {
			ctl_cur_x = 0;
			ctl_cur_y = 0;
			ctl_homed = 1;
		}
		return;
	}
	dx = ctl_clampi(ctl_tgt_x - ctl_cur_x, -step, step);
	dy = ctl_clampi(ctl_tgt_y - ctl_cur_y, -step, step);
	if (dx == 0 && dy == 0) {
		return;
	}
	kms_mouse_move(dx, dy);
	ctl_cur_x += dx;
	ctl_cur_y += dy;
}

static void ctl_apply(const struct ctl_cmd* c) {
	switch (c->verb) {
		case CTL_MOVEA:
			ctl_tgt_x = ctl_clampi(c->a, 0, screen_w - 1);
			ctl_tgt_y = ctl_clampi(c->b, 0, screen_h - 1);
			ctl_have_target = 1;
			if (ctl_route_now()) {
				/* summa_pen_move() takes the position in screen pixels and maps
				 * it onto the tablet's coordinate space itself, so this is the
				 * whole of an absolute move. */
				tablet_pen_move(0, 0, ctl_tgt_x, ctl_tgt_y);
				ctl_cur_x = ctl_tgt_x;
				ctl_cur_y = ctl_tgt_y;
			} else if (!ctl_homed && ctl_home_left == 0) {
				/* First target of the session: establish the origin. Enough
				 * 63-count slams to cross the whole surface in each axis. */
				ctl_home_left = (screen_w > screen_h ? screen_w : screen_h) / 63 + 2;
			}
			break;

		case CTL_MOVEP:
			if (ctl_use_tablet()) {
				/* The tablet route is closed by construction; a resync slam has
				 * nothing to resynchronise. Acked, applied as nothing. */
				break;
			}
			ctl_cur_x = ctl_clampi(ctl_cur_x + c->a, 0, screen_w - 1);
			ctl_cur_y = ctl_clampi(ctl_cur_y + c->b, 0, screen_h - 1);
			ctl_tgt_x = ctl_cur_x;
			ctl_tgt_y = ctl_cur_y;
			kms_mouse_move(ctl_clampi(c->a, -63, 63), ctl_clampi(c->b, -63, 63));
			break;

		case CTL_BTN:
			if (c->a >= 1 && c->a <= 3 && c->b) {
				ctl_btn_down_at[c->a] = SDL_GetTicks();
			}
			/* A NeXT mouse has two buttons. Middle is accepted and ignored so
			 * that streamhost's reconnect preamble (UP1 UP2 UP3) does not log a
			 * loud ERR on every reconnect. */
			if (c->a == 1) ctl_btn_left  = c->b;
			if (c->a == 2) ctl_btn_right = c->b;
			if (c->a != 3) ctl_apply_buttons();
			break;

		case CTL_KEY:
			if (c->b) {
				kms_keydown(ctl_mods, (uint8_t)c->a);
			} else {
				kms_keyup(ctl_mods, (uint8_t)c->a);
			}
			break;

		case CTL_MOD:
			/* The mask accompanies the edge and must already reflect it: a
			 * press sets the bit before the down, a release clears it before
			 * the up, exactly as SDL's own modifier state does in
			 * sdlkeymap.c. The keycode is NONE - a modifier is not a key. */
			if (c->b) {
				ctl_mods |= (uint8_t)c->a;
				kms_keydown(ctl_mods, NEXTKEY_NONE);
			} else {
				ctl_mods &= (uint8_t)~c->a;
				kms_keyup(ctl_mods, NEXTKEY_NONE);
			}
			break;

		case CTL_RELEASE:
			if (ctl_mods) {
				ctl_mods = 0;
				kms_keyup(0, NEXTKEY_NONE);
			}
			if (ctl_btn_left || ctl_btn_right) {
				ctl_btn_left = ctl_btn_right = 0;
				ctl_apply_buttons();
			}
			break;

		case CTL_NOP:
		default:
			break;
	}
}

void CtlSock_Drain(void) {
	if (!ctl_thread) {
		return;
	}
	for (;;) {
		struct ctl_cmd c;
		int have = 0;

		SDL_LockSpinlock(&ctl_lock);
		if (ctl_head != ctl_tail) {
			c = ctl_q[ctl_head];
			have = 1;
		}
		SDL_UnlockSpinlock(&ctl_lock);
		if (!have) {
			break;
		}
		/* Hold a RELEASE until the press has been down long enough to be
		 * sampled. Measured on the live station: a ~12 ms down/up on the
		 * tablet path is swallowed whole - a menu item highlights and never
		 * fires, a title-bar drag does not move the window - while an
		 * explicit 400 ms hold works every time (docs/guests/nextstep.md §4).
		 * The browser sends a visitor's real edges, and a quick click is a
		 * quick click, so the floor belongs here, at the injector, exactly
		 * like MAME's MAME_CTL_KEY_EXCL. PREVIOUS_CTL_BTN_HOLD=0 disables it. */
		if (c.verb == CTL_BTN && !c.b && c.a >= 1 && c.a <= 3 && ctl_btn_hold > 0) {
			uint64_t down = ctl_btn_down_at[c.a];
			uint64_t now  = SDL_GetTicks();
			if (down != 0 && now - down < (uint64_t)ctl_btn_hold) {
				break;   /* come back next tick; the queue keeps its order */
			}
		}
		/* Hold a button edge behind an unconverged pointer, so a click cannot
		 * land at a stale position. Bounded: after the settle window the edge
		 * goes through anyway rather than wedging the queue. */
		if (c.verb == CTL_BTN && !ctl_ptr_settled()) {
			uint64_t now = SDL_GetTicks();
			if (ctl_wait_since == 0) {
				ctl_wait_since = now;
			}
			if (now - ctl_wait_since < (uint64_t)ctl_ptr_settle) {
				break;
			}
			Log_Printf(LOG_WARN, "[CtlSock] button edge gave up waiting for the pointer");
		}
		ctl_wait_since = 0;

		SDL_LockSpinlock(&ctl_lock);
		ctl_head = (ctl_head + 1 >= CTL_QUEUE) ? 0 : ctl_head + 1;
		SDL_UnlockSpinlock(&ctl_lock);

		ctl_apply(&c);
		ctl_reply(c.seq, "OK", NULL);
	}
	ctl_ptr_tick();
}

/* ------------------------------------------------------------- init / fini */

static int ctl_env_int(const char* name, int fallback, int lo, int hi) {
	const char* v = SDL_getenv(name);
	int          n;

	if (!v || !*v || !ctl_parse_int(v, &n)) {
		return fallback;
	}
	return ctl_clampi(n, lo, hi);
}

void CtlSock_Init(void) {
	struct sockaddr_un addr;
	const char* path;
	const char* mode;

	SDL_SetAtomicInt(&ctl_conn_fd, -1);
	SDL_SetAtomicInt(&ctl_quit, 0);

	path = SDL_getenv("PREVIOUS_CTL_SOCK");
	if (!path || !*path) {
		return;
	}
	if (strlen(path) >= sizeof(ctl_path) || strlen(path) >= sizeof(addr.sun_path)) {
		Log_Printf(LOG_WARN, "[CtlSock] socket path too long");
		return;
	}
	mode = SDL_getenv("PREVIOUS_CTL_PTR");
	if (mode && !strcmp(mode, "tablet")) ctl_ptr_mode = PTR_TABLET;
	else if (mode && !strcmp(mode, "kms")) ctl_ptr_mode = PTR_KMS;
	ctl_ptr_step   = ctl_env_int("PREVIOUS_CTL_PTR_STEP", 16, 1, 63);
	ctl_ptr_settle = ctl_env_int("PREVIOUS_CTL_PTR_SETTLE", 1200, 0, 60000);
	ctl_ptr_rate   = ctl_env_int("PREVIOUS_CTL_PTR_RATE", 0, 0, 1000);
	ctl_btn_hold   = ctl_env_int("PREVIOUS_CTL_BTN_HOLD", 400, 0, 5000);

	snprintf(ctl_path, sizeof(ctl_path), "%s", path);
	/* A stale socket file from a previous run of THIS station is ours to
	 * remove; anything else fails the bind below and is reported. */
	unlink(ctl_path);

	ctl_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ctl_listen_fd < 0) {
		Log_Printf(LOG_WARN, "[CtlSock] socket: %s", strerror(errno));
		return;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, ctl_path, strlen(ctl_path));
	if (bind(ctl_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
	    listen(ctl_listen_fd, 1) != 0) {
		Log_Printf(LOG_WARN, "[CtlSock] bind/listen %s: %s", ctl_path, strerror(errno));
		close(ctl_listen_fd);
		ctl_listen_fd = -1;
		ctl_path[0] = '\0';
		return;
	}
	ctl_thread = SDL_CreateThread(ctl_thread_main, "[Previous] ctlsock", NULL);
	if (!ctl_thread) {
		Log_Printf(LOG_WARN, "[CtlSock] cannot start the reader thread");
		close(ctl_listen_fd);
		ctl_listen_fd = -1;
		unlink(ctl_path);
		ctl_path[0] = '\0';
		return;
	}
	fprintf(stderr, "ctlsock: mamectl/1 on %s (pointer %s, step %d, rate %d ms, "
	        "settle %d ms, button hold %d ms)\n",
	        ctl_path,
	        ctl_ptr_mode == PTR_TABLET ? "tablet" : (ctl_ptr_mode == PTR_KMS ? "kms" : "auto"),
	        ctl_ptr_step, ctl_ptr_rate, ctl_ptr_settle, ctl_btn_hold);
}

void CtlSock_UnInit(void) {
	int fd;

	if (!ctl_thread) {
		return;
	}
	SDL_SetAtomicInt(&ctl_quit, 1);
	fd = SDL_GetAtomicInt(&ctl_conn_fd);
	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR);
	}
	if (ctl_listen_fd >= 0) {
		shutdown(ctl_listen_fd, SHUT_RDWR);
		close(ctl_listen_fd);
		ctl_listen_fd = -1;
	}
	SDL_WaitThread(ctl_thread, NULL);
	ctl_thread = NULL;
	if (ctl_path[0]) {
		unlink(ctl_path);
		ctl_path[0] = '\0';
	}
}
