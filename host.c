/*
 * Copyright (c) 2013-2015 Erik Ekman <yarrick@kryo.se>
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright notice
 * and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#include "host.h"
#include "icmp.h"

#include <sys/param.h>

struct linked_gaicb {
	struct gaicb gaicb;
	struct linked_gaicb *next;
};

static const struct addrinfo addr_request = {
	.ai_family = AF_UNSPEC,
	.ai_socktype = SOCK_RAW,
};

int host_make_resolvlist(FILE *file, struct gaicb **list[])
{
	int hosts = 0;
	int i;

	struct gaicb **l;
	struct linked_gaicb *head = NULL;
	struct linked_gaicb *tail = NULL;
	struct linked_gaicb *lg;

	for (;;) {
		char hostname[300];
		int res;

		memset(hostname, 0, sizeof(hostname));
		res = fscanf(file, "%256s", hostname);
		if (res == EOF) break;

		lg = calloc(1, sizeof(*lg));
		lg->gaicb.ar_name = strndup(hostname, strlen(hostname));
		lg->gaicb.ar_request = &addr_request;
		if (!head) head = lg;
		if (tail) tail->next = lg;
		tail = lg;
		hosts++;
	}

	l = calloc(hosts, sizeof(struct gaicb*));
	lg = head;
	for (i = 0; i < hosts; i++) {
		l[i] = &lg->gaicb;
		lg = lg->next;
	}

	*list = l;

	return hosts;
}

void host_free_resolvlist(struct gaicb *list[], int length)
{
	int i;

	for (i = 0; i < length; i++) {
		free((char *) list[i]->ar_name);
		if (list[i]->ar_result) freeaddrinfo(list[i]->ar_result);
		free(list[i]);
	}
	free(list);
}

struct host *host_create(struct gaicb *list[], int listlength, int hostlength)
{
	struct host *hosts = calloc(hostlength, sizeof(struct host));
	int i;
	int addr;

	addr = 0;
	for (i = 0; i < listlength; i++) {
		if (gai_error(list[i]) == 0) {
			struct addrinfo *result = list[i]->ar_result;
			while (result) {
				memcpy(&hosts[addr].sockaddr, result->ai_addr, result->ai_addrlen);
				hosts[addr].sockaddr_len = result->ai_addrlen;
				addr++;

				result = result->ai_next;
			}
		}
	}
	return hosts;
}

static void read_eval_reply(int sock, struct eval_host *evalhosts, int hosts)
{
	struct icmp_packet mypkt;
	mypkt.peer_len = sizeof(struct sockaddr_storage);
	uint8_t buf[BUFSIZ];
	int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *) &mypkt.peer, &mypkt.peer_len);
	if (len > 0) {
		if (icmp_parse(&mypkt, buf, len) == 0) {
			int i;
			for (i = 0; i < hosts; i++) {
				struct eval_host *eh = &evalhosts[i];
				if (memcmp(&mypkt.peer, &eh->host->sockaddr, mypkt.peer_len) == 0 &&
					eh->payload_len == mypkt.payload_len &&
					memcmp(mypkt.payload, eh->payload, eh->payload_len) == 0 &&
					eh->id == mypkt.id &&
					eh->cur_seqno == mypkt.seqno) {

					/* Store accepted reply */
					eh->host->rx_icmp++;
					break;
				}
			}
			free(mypkt.payload);
		}
	}
}

int host_evaluate(struct host *hostlist, int length, int sockv4, int sockv6)
{
	int i;
	int addr;
	struct eval_host *eval_hosts = calloc(length, sizeof(struct eval_host));
	uint8_t eval_payload[1024];

	for (i = 0; i < sizeof(eval_payload); i++) {
		eval_payload[i] = i & 0xff;
	}

	addr = 0;
	for (i = 0; i < length; i++) {
		eval_hosts[addr].host = &hostlist[i];
		eval_hosts[addr].id = addr;
		eval_hosts[addr].cur_seqno = addr * 2;
		eval_hosts[addr].payload = eval_payload;
		eval_hosts[addr].payload_len = sizeof(eval_payload);
		addr++;
	}

	printf("Evaluating %d hosts.", length);
	for (i = 0; i < 5; i++) {
		int maxfd;
		int h;

		printf(".");
		fflush(stdout);
		for (h = 0; h < length; h++) {
			int sock;
			struct icmp_packet pkt;

			memcpy(&pkt.peer, &eval_hosts[h].host->sockaddr, eval_hosts[h].host->sockaddr_len);
			pkt.peer_len = eval_hosts[h].host->sockaddr_len;
			pkt.type = ICMP_REQUEST;
			pkt.id = eval_hosts[h].id;
			pkt.seqno = eval_hosts[h].cur_seqno;
			pkt.payload = eval_hosts[h].payload;
			pkt.payload_len = eval_hosts[h].payload_len;

			if (ICMP_ADDRFAMILY(&pkt) == AF_INET) {
				sock = sockv4;
			} else {
				sock = sockv6;
			}

			if (sock >= 0) {
				eval_hosts[h].host->tx_icmp++;
				icmp_send(sock, &pkt);
			}
		}

		maxfd = MAX(sockv4, sockv6);
		for (;;) {
			struct timeval tv;
			fd_set fds;
			int i;

			FD_ZERO(&fds);
			if (sockv4 >= 0) FD_SET(sockv4, &fds);
			if (sockv6 >= 0) FD_SET(sockv6, &fds);

			tv.tv_sec = 1;
			tv.tv_usec = 0;
			i = select(maxfd+1, &fds, NULL, NULL, &tv);
			if (!i) break; /* No action for 1 second, break.. */
			if ((sockv4 >= 0) && FD_ISSET(sockv4, &fds)) read_eval_reply(sockv4, eval_hosts, length);
			if ((sockv6 >= 0) && FD_ISSET(sockv6, &fds)) read_eval_reply(sockv6, eval_hosts, length);
		}
	}
	printf(" done.\n");
	
	free(eval_hosts);
	return length;
}
