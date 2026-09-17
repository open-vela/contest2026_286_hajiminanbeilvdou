/****************************************************************************
 * apps/netutils/pppd/ppp_conf.h
 *
 *   Copyright (C) 2015 Max Nekludov. All rights reserved.
 *   Author: Max Nekludov <macscomp@gmail.com>
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

#ifndef __APPS_NETUTILS_PPPD_PPP_CONF_H
#define __APPS_NETUTILS_PPPD_PPP_CONF_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IPCP_RETRY_COUNT        5
#define IPCP_TIMEOUT            5
#define IPV6CP_RETRY_COUNT      5
#define IPV6CP_TIMEOUT          5
#define LCP_RETRY_COUNT         5
#define LCP_TIMEOUT             5
#define PAP_RETRY_COUNT         5
#define PAP_TIMEOUT             5
#define LCP_ECHO_INTERVAL       20

#define PPP_IP_TIMEOUT          (6*3600)
#define PPP_MAX_CONNECT         15

#define xxdebug_printf          ninfo
#define debug_printf            ninfo

/* ip_buf[] and ahdlc_rx_buffer[] are both this size, and it also becomes the
 * MRU advertised to the peer. It has to hold one whole PPP frame un-escaped:
 * protocol (2) + IP packet + FCS (2).
 *
 * Left at 1024 because it has to move together with CONFIG_NET_TUN_PKTSIZE.
 * Raising both lifts the TCP MSS off 256 and recovers about a fifth of the
 * link, but it also destabilises this board's PPP link -- the interface stops
 * answering LCP echo. See the note on CONFIG_NET_TUN_PKTSIZE in the agent
 * defconfig before changing either.
 */

#define PPP_RX_BUFFER_SIZE      1024

/* Off, because this counter does not measure what it claims to.

 * ahdlc_tx() bumps it on every frame transmitted and the receive path clears
 * it on every frame received with a good CRC, and when it passes 5 the link
 * is declared dead. That is a dial-up assumption twice over: a modem's TX
 * queue really can back up while a call is up, and recovering by redialling
 * is the right response. Over a direct serial cable there is no modem, no
 * dial tone and nothing to redial -- and ppp_reconnect() marks the interface
 * down for four seconds and writes "+++" and "ATE1" into the middle of the
 * PPP byte stream, which is where those stray strings in a capture come
 * from.
 *
 * On this link the counter trips in ordinary use. A 4 KB audio frame leaves
 * as sixteen 256 byte frames back to back, so a handful of them can go out
 * between two ACKs and the "outstanding" count reaches the limit while the
 * link is working perfectly. The reconnect that follows is what actually
 * drops the connection: the host sees the interface stop answering LCP echo
 * and gives up.
 */

#define AHDLC_TX_OFFLINE        0

#define IPCP_GET_PEER_IP        1

#define PPP_STATISTICS          1
#define PPP_DEBUG               defined(CONFIG_DEBUG_NET_INFO)

#endif /* __APPS_NETUTILS_PPPD_PPP_CONF_H */
