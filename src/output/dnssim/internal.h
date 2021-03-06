/*
 * Copyright (c) 2019-2020, CZ.NIC, z.s.p.o.
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
#ifndef __dnsjit_output_dnssim_internal_h
#define __dnsjit_output_dnssim_internal_h

#define _self ((_output_dnssim_t*)self)
#define _ERR_MALFORMED -2
#define _ERR_MSGID -3
#define _ERR_TC -4


typedef struct _output_dnssim_request _output_dnssim_request_t;
typedef struct _output_dnssim_connection _output_dnssim_connection_t;
typedef struct _output_dnssim_client _output_dnssim_client_t;


/*
 * Query-related structures.
 */

typedef struct _output_dnssim_query _output_dnssim_query_t;
struct _output_dnssim_query {
    /*
     * Next query in the list.
     *
     * Currently, next is used for TCP clients/connection, which makes it
     * impossible to use for tracking multiple queries of a single request.
     *
     * TODO: refactor the linked lists to allow query to be part of multiple lists
     */
    _output_dnssim_query_t* next;

    output_dnssim_transport_t transport;
    _output_dnssim_request_t* req;

    /* Query state, currently used only for TCP. */
    enum {
        _OUTPUT_DNSSIM_QUERY_PENDING_WRITE,
        _OUTPUT_DNSSIM_QUERY_PENDING_WRITE_CB,
        _OUTPUT_DNSSIM_QUERY_PENDING_CLOSE,
        _OUTPUT_DNSSIM_QUERY_WRITE_FAILED,
        _OUTPUT_DNSSIM_QUERY_SENT,
        _OUTPUT_DNSSIM_QUERY_ORPHANED
    } state;
};

typedef struct _output_dnssim_query_udp _output_dnssim_query_udp_t;
struct _output_dnssim_query_udp {
    _output_dnssim_query_t qry;

    uv_udp_t* handle;
    uv_buf_t buf;
};

typedef struct _output_dnssim_query_tcp _output_dnssim_query_tcp_t;
struct _output_dnssim_query_tcp {
    _output_dnssim_query_t qry;

    /* Connection this query is assigned to. */
    _output_dnssim_connection_t* conn;

    uv_write_t write_req;

    /* Send buffers for libuv; 0 is for dnslen, 1 is for dnsmsg. */
    uv_buf_t bufs[2];
};

struct _output_dnssim_request {
    /* List of queries associated with this request. */
    _output_dnssim_query_t* qry;

    /* Client this request belongs to. */
    _output_dnssim_client_t* client;

    /* The DNS question to be resolved. */
    core_object_payload_t* payload;
    core_object_dns_t* dns_q;

    /* Timestamps for latency calculation. */
    uint64_t created_at;
    uint64_t ended_at;

    /* Timer for tracking timeout of the request. */
    uv_timer_t* timer;

    /* The output component of this request. */
    output_dnssim_t* dnssim;

    /* State of the request. */
    enum {
        _OUTPUT_DNSSIM_REQ_ONGOING,
        _OUTPUT_DNSSIM_REQ_CLOSING
    } state;

    /* Statistics interval in which this request is tracked. */
    output_dnssim_stats_t* stats;
};


/*
 * Connection-related structures.
 */

/* Read-state of connection's data stream. */
typedef enum _output_dnssim_read_state {
    _OUTPUT_DNSSIM_READ_STATE_CLEAN,
    _OUTPUT_DNSSIM_READ_STATE_DNSLEN,   /* Expecting bytes of dnslen. */
    _OUTPUT_DNSSIM_READ_STATE_DNSMSG,   /* Expecting bytes of dnsmsg. */
    _OUTPUT_DNSSIM_READ_STATE_INVALID
} _output_dnssim_read_state_t;

struct _output_dnssim_connection {
    _output_dnssim_connection_t* next;

    uv_tcp_t* handle;

    /* Timeout timer for establishing the connection. */
    uv_timer_t* handshake_timer;

    /* Idle timer for connection reuse. rfc7766#section-6.2.3 */
    uv_timer_t* idle_timer;
    bool is_idle;

    /* List of queries that have been queued (pending write callback). */
    _output_dnssim_query_t* queued;

    /* List of queries that have been sent over this connection. */
    _output_dnssim_query_t* sent;

    /* Client this connection belongs to. */
    _output_dnssim_client_t* client;

    /* State of the connection. */
    enum {
        _OUTPUT_DNSSIM_CONN_INITIALIZED,
        _OUTPUT_DNSSIM_CONN_CONNECTING,
        _OUTPUT_DNSSIM_CONN_ACTIVE,
        _OUTPUT_DNSSIM_CONN_CLOSING,
        _OUTPUT_DNSSIM_CONN_CLOSED
    } state;

    /* State of the data stream read. */
    _output_dnssim_read_state_t read_state;

    /* Total length of the expected stream data (either 2 for dnslen, or dnslen itself). */
    size_t recv_len;

    /* Current position in the receive buffer. */
    size_t recv_pos;

    /* Receive buffer used for incomplete messages or dnslen. */
    char* recv_data;
    bool recv_free_after_use;

    /* Statistics interval in which the handshake is tracked. */
    output_dnssim_stats_t* stats;
};


/*
 * Client structure.
 */

struct _output_dnssim_client {
    /* Dnssim component this client belongs to. */
    output_dnssim_t* dnssim;

    /* List of connections.
     * Multiple connections may be used (e.g. some are already closed for writing).
     */
    _output_dnssim_connection_t* conn;

    /* List of queries that are pending to be sent over any available connection. */
    _output_dnssim_query_t* pending;
};


/*
 * DnsSim-related structures.
 */

typedef struct _output_dnssim_source _output_dnssim_source_t;
struct _output_dnssim_source {
    _output_dnssim_source_t* next;
    struct sockaddr_storage addr;
};

typedef struct _output_dnssim _output_dnssim_t;
struct _output_dnssim {
    output_dnssim_t pub;

    uv_loop_t loop;
    uv_timer_t stats_timer;

    struct sockaddr_storage target;
    _output_dnssim_source_t* source;
    output_dnssim_transport_t transport;

    /* Array of clients, mapped by client ID (ranges from 0 to max_clients). */
    _output_dnssim_client_t* client_arr;
};


/*
 * Forward function declarations.
 */

static int _bind_before_connect(output_dnssim_t* self, uv_handle_t* handle);
static int _create_query_udp(output_dnssim_t* self, _output_dnssim_request_t* req);
static int _create_query_tcp(output_dnssim_t* self, _output_dnssim_request_t* req);
static void _close_query_udp(_output_dnssim_query_udp_t* qry);
static void _close_query_tcp(_output_dnssim_query_tcp_t* qry);
static void _on_request_timer_closed(uv_handle_t* handle);
static void _on_request_timeout(uv_timer_t* handle);
static void _maybe_close_connection(_output_dnssim_connection_t* conn);
static void _close_connection(_output_dnssim_connection_t* conn);
static void _request_answered(_output_dnssim_request_t* req, core_object_dns_t* msg);
static void _close_request(_output_dnssim_request_t* req);
static void _maybe_free_request(_output_dnssim_request_t* req);
static void _close_query(_output_dnssim_query_t* qry);
static void _on_uv_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static int _handle_pending_queries(_output_dnssim_client_t* client);


/*
 * Defaults.
 */

static core_log_t _log = LOG_T_INIT("output.dnssim");

#endif
