/****************************************************************************
 * apps/examples/pppd/pppd_main.c
 *
 *   Copyright (C) 2015 Brennan Ashton. All rights reserved.
 *   Author: Brennan Ashton <brennan@ombitron.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>

#include "netutils/pppd.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Null-modem link (direct wire to a host pppd server): no AT dialing,
 * the PPP peer is already on the wire.  NULL scripts skip chat entirely.
 */

static FAR const char *connect_script = NULL;
static FAR const char *disconnect_script = NULL;

/* Local address to request during IPCP (192.168.223.2).  The host side is
 * configured with the matching 192.168.223.1:192.168.223.2 pair.
 */
#define PPP_LOCAL_IP 0xC0A8DF02

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pppd_main
 ****************************************************************************/

int main(int argc, char *argv[])
{
  /* The board registers the console UART only as /dev/console (the ttySx
   * loop in sifli_uart.c explicitly skips it), so that is the line that
   * actually carries the CH340 link the peer is on.
   */

  const struct pppd_settings_s pppd_settings =
  {
    .disconnect_script = disconnect_script,
    .connect_script = connect_script,
    .ttyname = "/dev/console",
    .local_ip = { .s_addr = htonl(PPP_LOCAL_IP) },
#ifdef CONFIG_NETUTILS_PPPD_PAP
    .pap_username = "user",
    .pap_password = "pass",
#endif
  };

  return pppd(&pppd_settings);
}
