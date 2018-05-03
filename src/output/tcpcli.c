/*
 * Copyright (c) 2018, OARC, Inc.
 * All rights reserved.
 *
 * This file is part of dnsjit.
 *
 * dnsjit is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dnsjit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dnsjit.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "output/tcpcli.h"
#include "core/object/dns.h"
#include "core/object/udp.h"
#include "core/object/tcp.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

static core_log_t      _log      = LOG_T_INIT("output.tcpcli");
static output_tcpcli_t _defaults = {
    LOG_T_INIT_OBJ("output.tcpcli"),
    0, 0, -1,
};

core_log_t* output_tcpcli_log()
{
    return &_log;
}

int output_tcpcli_init(output_tcpcli_t* self, const char* host, const char* port)
{
    struct addrinfo* addr;
    int              err;

    if (!self || !host || !port) {
        return 1;
    }

    *self = _defaults;

    ldebug("init %s %s", host, port);

    if ((err = getaddrinfo(host, port, 0, &addr))) {
        lcritical("getaddrinfo() %d", err);
        return 1;
    }
    if (!addr) {
        lcritical("getaddrinfo failed");
        return 1;
    }
    ldebug("getaddrinfo() flags: 0x%x family: 0x%x socktype: 0x%x protocol: 0x%x addrlen: %d",
        addr->ai_flags,
        addr->ai_family,
        addr->ai_socktype,
        addr->ai_protocol,
        addr->ai_addrlen);

    if ((self->fd = socket(addr->ai_addr->sa_family, SOCK_STREAM, 0)) < 0) {
        lcritical("socket failed");
        freeaddrinfo(addr);
        return 1;
    }

    if (connect(self->fd, addr->ai_addr, addr->ai_addrlen)) {
        lcritical("connect failed");
        freeaddrinfo(addr);
        close(self->fd);
        self->fd = -1;
        return 1;
    }

    freeaddrinfo(addr);

    if ((err = fcntl(self->fd, F_GETFL)) == -1
        || fcntl(self->fd, F_SETFL, err | O_NONBLOCK)) {
        lcritical("fcntl failed");
    }

    return 0;
}

int output_tcpcli_destroy(output_tcpcli_t* self)
{
    if (!self) {
        return 1;
    }

    ldebug("destroy");

    if (self->fd > -1) {
        shutdown(self->fd, SHUT_RDWR);
        close(self->fd);
    }

    return 0;
}

static int _receive(void* ctx, const core_object_t* obj)
{
    output_tcpcli_t* self = (output_tcpcli_t*)ctx;
    const uint8_t*   payload;
    size_t           len, sent;
    uint16_t         dnslen;

    if (!self) {
        return 1;
    }

    for (; obj;) {
        switch (obj->obj_type) {
        case CORE_OBJECT_DNS:
            obj = obj->obj_prev;
            continue;
        case CORE_OBJECT_UDP:
            payload = ((core_object_udp_t*)obj)->payload;
            len     = ((core_object_udp_t*)obj)->len;
            break;
        case CORE_OBJECT_TCP:
            payload = ((core_object_tcp_t*)obj)->payload;
            len     = ((core_object_tcp_t*)obj)->len;
            break;
        default:
            return 1;
        }

        if (len < 3 || payload[2] & 0x80) {
            return 0;
        }

        sent = 0;
        self->pkts++;

        dnslen = htons(len);

        for (;;) {
            ssize_t ret = sendto(self->fd, ((uint8_t*)&dnslen) + sent, sizeof(dnslen) - sent, 0, 0, 0);
            if (ret > -1) {
                sent += ret;
                if (sent < sizeof(dnslen))
                    continue;

                sent = 0;
                for (;;) {
                    ssize_t ret = sendto(self->fd, payload + sent, len - sent, 0, 0, 0);
                    if (ret > -1) {
                        sent += ret;
                        if (sent < len)
                            continue;
                        return 0;
                    }
                    switch (errno) {
                    case EAGAIN:
#if EAGAIN != EWOULDBLOCK
                    case EWOULDBLOCK:
#endif
                        continue;
                    default:
                        break;
                    }
                    self->errs++;
                    break;
                }
                break;
            }
            switch (errno) {
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                continue;
            default:
                break;
            }
            self->errs++;
            break;
        }
        break;
    }

    return 1;
}

core_receiver_t output_tcpcli_receiver()
{
    return _receive;
}