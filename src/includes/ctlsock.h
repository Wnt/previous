/*
  Previous - ctlsock.h

  Kernel Hive host-native input plane. See ctlsock.c.

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef PREV_CTLSOCK_H
#define PREV_CTLSOCK_H

/* Start/stop the control socket. Inert unless PREVIOUS_CTL_SOCK is set. */
extern void CtlSock_Init(void);
extern void CtlSock_UnInit(void);

/* Apply queued commands. MUST be called from the emulation thread only. */
extern void CtlSock_Drain(void);

#endif /* PREV_CTLSOCK_H */
