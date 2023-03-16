/*-
 * Copyright (c) 1985, 1993 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * From: @(#)measure.c	5.1 (Berkeley) 5/11/93
 */
char measure_rcsid[] =
  "$Id: measure.c,v 1.8 1997/05/19 09:41:37 dholland Exp $";

#ifdef sgi
#ident "$Revision: 1.8 $"
#endif

#include "globals.h"
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#if defined(__GLIBC__) && (__GLIBC__ >= 2)
#define icmphdr icmp
#endif

#define MSEC_DAY	(SECDAY*1000)

#define PACKET_IN	1024

#define MSGS		5		/* timestamps to average */
#define TRIALS		10		/* max # of timestamps sent */

extern int sock_raw;

int measure_delta;

extern int in_cksum(u_short*, int);

static u_short seqno = 0;

/*
 * Measures the differences between machines' clocks using
 * ICMP timestamp messages.
 */
int					/* status val defined in globals.h */
measure(u_long maxmsec,			/* wait this many msec at most */
	u_long wmsec,			/* msec to wait for an answer */
	char *hname,
	struct sockaddr_in *xaddr,
	int doprint)			/* print complaints on stderr */
{
	size_t length;
	int measure_status;
	int rcvcount, trials = 0;
	int cc, count;
	fd_set ready;
	long sendtime, recvtime, histime1, histime2;
	long idelta, odelta, total;
	long min_idelta, min_odelta;
	struct timeval tdone, tcur, ttrans, twait, tout;
	u_char packet[PACKET_IN], opacket[64];
	register struct icmphdr *icp = (struct icmphdr *) packet;
	register struct icmphdr *oicp = (struct icmphdr *) opacket;
#ifndef icmp_data
	time_t *icp_time = (time_t *)(icp + 1);
	time_t *oicp_time = (time_t *)(oicp + 1);
#else
	time_t *icp_time = (time_t *)icp->icmp_data;
	time_t *oicp_time = (time_t *)oicp->icmp_data;
#endif
	struct iphdr *ip = (struct iphdr *) packet;
	struct sockaddr_in fixed_addr = *xaddr;
	fixed_addr.sin_port = 0;

#ifdef __linux__
#define ICPSIZE ((int)(sizeof(struct icmphdr)+3*sizeof(time_t)))
#else
#define ICPSIZE ((int)(sizeof(struct icmphdr)))
#endif

	min_idelta = min_odelta = 0x7fffffff;
	measure_status = HOSTDOWN;
	measure_delta = HOSTDOWN;
	errno = 0;

	/* open raw socket used to measure time differences */
	if (sock_raw < 0) {
		sock_raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
		if (sock_raw < 0)  {
			syslog(LOG_ERR, "opening raw socket: %m");
			goto quit;
		}
	}
	    

	/*
	 * empty the icmp input queue
	 */
	FD_ZERO(&ready);
	for (;;) {
		tout.tv_sec = tout.tv_usec = 0;
		FD_SET(sock_raw, &ready);
		if (select(sock_raw+1, &ready, 0,0, &tout)) {
			length = sizeof(struct sockaddr_in);
			cc = recvfrom(sock_raw, (char *)packet, PACKET_IN, 0,
				      0,&length);
			if (cc < 0)
				goto quit;
			continue;
		}
		break;
	}

	/*
	 * Choose the smallest transmission time in each of the two
	 * directions. Use these two latter quantities to compute the delta
	 * between the two clocks.
	 */

#if !defined(__GLIBC__) || (__GLIBC__ < 2)
#define icmp_type	type
#define icmp_code	code
#define icmp_id		un.echo.id
#define icmp_seq	un.echo.sequence
#define icmp_cksum	checksum
#define ICMP_TSTAMP	ICMP_TIMESTAMP
#define ICMP_TSTAMPREPLY	ICMP_TIMESTAMPREPLY
#endif

	oicp->icmp_type = ICMP_TSTAMP;
	oicp->icmp_code = 0;
	oicp->icmp_id = getpid();
	oicp_time[1] = 0;
	oicp_time[2] = 0;
	oicp->icmp_seq = seqno;

	FD_ZERO(&ready);

#ifdef sgi
	sginap(1);			/* start at a clock tick */
#endif /* sgi */

	(void)gettimeofday(&tdone, 0);
	mstotvround(&tout, maxmsec);
	timevaladd(&tdone, &tout);		/* when we give up */

	mstotvround(&twait, wmsec);

	rcvcount = 0;
	trials = 0;
	while (rcvcount < MSGS) {
		(void)gettimeofday(&tcur, 0);

		/*
		 * keep sending until we have sent the max
		 */
		if (trials < TRIALS) {
			trials++;
			oicp_time[0] = htonl(((tcur.tv_sec % SECDAY) * 1000
					      + tcur.tv_usec / 1000));
			oicp->icmp_cksum = 0;
			oicp->icmp_cksum = in_cksum((u_short*)oicp,
						    ICPSIZE);

			count = sendto(sock_raw, opacket, ICPSIZE, 0,
				       (struct sockaddr*)&fixed_addr,
				       sizeof(struct sockaddr));
			if (count < 0) {
				if (measure_status == HOSTDOWN)
					measure_status = UNREACHABLE;
				goto quit;
			}
			++oicp->icmp_seq;

			ttrans = tcur;
			timevaladd(&ttrans, &twait);
		} else {
			ttrans = tdone;
		}

		while (rcvcount < trials) {
			timevalsub(&tout, &ttrans, &tcur);
			if (tout.tv_sec < 0)
				tout.tv_sec = 0;

			FD_SET(sock_raw, &ready);
			count = select(sock_raw+1, &ready, (fd_set *)0,
				       (fd_set *)0, &tout);
			(void)gettimeofday(&tcur, (struct timezone *)0);
			if (count <= 0)
				break;

			length = sizeof(struct sockaddr_in);
			cc = recvfrom(sock_raw, (char *)packet, PACKET_IN, 0,
				      0,&length);
			if (cc < 0)
				goto quit;

			/* 
			 * got something.  See if it is ours
			 */
			icp = (struct icmphdr *)(packet + (ip->ihl << 2));
			if (cc < ICPSIZE
			    || icp->icmp_type != ICMP_TSTAMPREPLY
			    || icp->icmp_id != oicp->icmp_id
			    || icp->icmp_seq < seqno
			    || icp->icmp_seq >= oicp->icmp_seq)
				continue;

			/* Reset this because icp just changed... */
			icp_time = (time_t *)(icp + 1);

			sendtime = ntohl(icp_time[0]);
			recvtime = ((tcur.tv_sec % SECDAY) * 1000 +
				    tcur.tv_usec / 1000);

			total = recvtime-sendtime;
			if (total < 0)	/* do not hassle midnight */
				continue;

			rcvcount++;
			histime1 = ntohl(icp_time[1]);
			histime2 = ntohl(icp_time[2]);
			/*
			 * a host using a time format different from
			 * msec. since midnight UT (as per RFC792) should
			 * set the high order bit of the 32-bit time
			 * value it transmits.
			 */
			if ((histime1 & 0x80000000) != 0) {
				measure_status = NONSTDTIME;
				goto quit;
			}
			measure_status = GOOD;

			idelta = recvtime-histime2;
			odelta = histime1-sendtime;

			/* do not be confused by midnight */
			if (idelta < -MSEC_DAY/2) idelta += MSEC_DAY;
			else if (idelta > MSEC_DAY/2) idelta -= MSEC_DAY;

			if (odelta < -MSEC_DAY/2) odelta += MSEC_DAY;
			else if (odelta > MSEC_DAY/2) odelta -= MSEC_DAY;

			/* save the quantization error so that we can get a
			 * measurement finer than our system clock.
			 */
			if (total < MIN_ROUND) {
				measure_delta = (odelta - idelta)/2;
				goto quit;
			}

			if (idelta < min_idelta)
				min_idelta = idelta;
			if (odelta < min_odelta)
				min_odelta = odelta;

			measure_delta = (min_odelta - min_idelta)/2;
		}

		if (tcur.tv_sec > tdone.tv_sec
		    || (tcur.tv_sec == tdone.tv_sec
			&& tcur.tv_usec >= tdone.tv_usec))
			break;
	}

quit:
	seqno += TRIALS;		/* allocate our sequence numbers */

	/*
	 * If no answer is received for TRIALS consecutive times,
	 * the machine is assumed to be down
	 */
	if (measure_status == GOOD) {
		if (trace) {
			fprintf(fd,
				"measured delta %4d, %d trials to %-15s %s\n",
			   	measure_delta, trials,
				inet_ntoa(fixed_addr.sin_addr), hname);
		}
	} else if (doprint) {
		if (errno != 0)
			fprintf(stderr, "measure %s: %s\n", hname,
				strerror(errno));
	} else {
		if (errno != 0) {
			syslog(LOG_ERR, "measure %s: %m", hname);
		} else {
			syslog(LOG_ERR, "measure: %s did not respond", hname);
		}
		if (trace) {
			fprintf(fd,
				"measure: %s failed after %d trials\n",
				hname, trials);
			(void)fflush(fd);
		}
	}

	return(measure_status);
}





/*
 * round a number of milliseconds into a struct timeval
 */
void
mstotvround(struct timeval *res, long x)
{
#ifndef sgi
	if (x < 0)
		x = -((-x + 3)/5);
	else
		x = (x+3)/5;
	x *= 5;
#endif /* sgi */
	res->tv_sec = x/1000;
	res->tv_usec = (x-res->tv_sec*1000)*1000;
	if (res->tv_usec < 0) {
		res->tv_usec += 1000000;
		res->tv_sec--;
	}
}

void
timevaladd(struct timeval *tv1, struct timeval *tv2)
{
	tv1->tv_sec += tv2->tv_sec;
	tv1->tv_usec += tv2->tv_usec;
	if (tv1->tv_usec >= 1000000) {
		tv1->tv_sec++;
		tv1->tv_usec -= 1000000;
	}
	if (tv1->tv_usec < 0) {
		tv1->tv_sec--;
		tv1->tv_usec += 1000000;
	}
}

void
timevalsub(struct timeval *res, struct timeval *tv1, struct timeval *tv2)
{
	res->tv_sec = tv1->tv_sec - tv2->tv_sec;
	res->tv_usec = tv1->tv_usec - tv2->tv_usec;
	if (res->tv_usec >= 1000000) {
		res->tv_sec++;
		res->tv_usec -= 1000000;
	}
	if (res->tv_usec < 0) {
		res->tv_sec--;
		res->tv_usec += 1000000;
	}
}

