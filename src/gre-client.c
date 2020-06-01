/*
 * Copyright (C) 2020 Owen Kirby <oskirby@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip6.h>
#include <sys/uio.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_ether.h>

#include "gre.h"

/* Process the GRE/UDP socket when data is ready. */
static int
gre_process_socket(int sockfd, int tunfd)
{
    struct sockaddr_storage sas;
    socklen_t addrlen = sizeof(struct sockaddr_storage);
    struct gre_header gre;
    struct tun_header tun;
    char rxbuf[IP6_MTU + sizeof(struct gre_header)];
    int pktlen;

    struct iovec iov[] = {
        {.iov_base = &tun, .iov_len = sizeof(tun)},
        {.iov_base = rxbuf + sizeof(struct gre_header), .iov_len = 0},
    };

    /* Receive a packet from the tunnel device */
    pktlen = recvfrom(sockfd, rxbuf, sizeof(rxbuf), 0, (struct sockaddr *)&sas, &addrlen);
    if (pktlen < 0) {
        fprintf(stderr, "recv() failed on GRE socket: %s\n", strerror(errno));
        return -1;
    }
    if (pktlen <= sizeof(struct gre_header)) {
        fprintf(stderr, "Truncated tunnel header, dropping packet.");
        return -1;
    }
    memcpy(&gre, rxbuf, sizeof(struct gre_header));
    /* TODO: Parse optional GRE header data */

    /* We only care about IPv6 packets. */
    if ((gre.proto != htons(ETH_P_IPV6)) || (pktlen < sizeof(struct ip6_hdr)+sizeof(struct gre_header))) {
        fprintf(stderr, "Foreign protocol, dropping packet.");
        return -1;
    }

    /* TODO: Validate Me! */
    /* TODO: Update connection timeout */

    /* Pass the packet into the kernel */
    iov[1].iov_len = pktlen - sizeof(struct gre_header);
    tun.flags = 0;
    tun.proto = htons(ETH_P_IPV6);
    return writev(tunfd, iov, 2);
} /* gre_process_socket */

/* Process the Kernel tunnel device when data is ready. */
static int
gre_process_tunnel(int sockfd, int tunfd)
{
    struct gre_header gre;
    struct tun_header tun;
    struct iovec iov[2];
    char rxbuf[IP6_MTU + sizeof(struct tun_header)];
    int pktlen;

    /* Receive a packet from the tunnel device */
    pktlen = read(tunfd, rxbuf, sizeof(rxbuf));
    if (pktlen < 0) {
        fprintf(stderr, "read() failed on tunnel device: %s\n", strerror(errno));
        return -1;
    }
    if (pktlen <= sizeof(struct tun_header)) {
        fprintf(stderr, "Truncated tunnel header, dropping packet.");
        return -1;
    }
    memcpy(&tun, rxbuf, sizeof(struct tun_header));

    /* We only care about IPv6 packets. */
    if ((tun.proto != htons(ETH_P_IPV6)) || (pktlen < sizeof(struct ip6_hdr)+sizeof(struct tun_header))) {
        fprintf(stderr, "Foreign protocol, dropping packet.");
        return -1;
    }

    /* TODO: Update connection timeout */

    /* Send the GRE packet to the remote host. */
    memset(&gre, 0, sizeof(gre));
    gre.flags = 0;
    gre.proto = htons(ETH_P_IPV6);
    gre.cksum = 0;
    gre.reserved = 0;
    iov[0].iov_base = &gre;
    iov[0].iov_len = sizeof(gre);
    iov[1].iov_base = rxbuf + sizeof(struct tun_header);
    iov[1].iov_len = pktlen - sizeof(struct tun_header);

    return writev(sockfd, iov, 2);
}

int
gre_client_run(int sockfd)
{
    int tunfd = gre_allocate_tun();
    if (tunfd < 0) {
        return -1;
    }

    while (1) {
        fd_set rfd;
        int maxfd = 0;
        int ret;

        /* Prepare the list of file descriptors to check. */
        FD_ZERO(&rfd);
        FD_SET(sockfd, &rfd);
        FD_SET(tunfd, &rfd);
        maxfd = (sockfd > tunfd) ? sockfd : tunfd;

        /* Wait for socket activity */
        ret = select(maxfd+1, &rfd, NULL, NULL, NULL);
        if (ret == 0) continue;
        else if (ret < 0) {
            if (errno != EINTR) {
                fprintf(stderr, "select() failed: %s\n", strerror(errno));
            }
            break;
        }

        if (FD_ISSET(sockfd, &rfd)) {
            gre_process_socket(sockfd, tunfd);
        }
        if (FD_ISSET(tunfd, &rfd)) {
            gre_process_tunnel(sockfd, tunfd);
        }
    }

    return 0;
}