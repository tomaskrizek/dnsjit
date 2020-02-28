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

/*
 * TCP dnssim
 *
 * TODO: extract functions common to tcp/udp into separate functions
 */
static void _on_tcp_handle_closed(uv_handle_t* handle)
{
    // TODO free unneeded, fail/reassign queries (+timers)
    _output_dnssim_connection_t* conn = (_output_dnssim_connection_t*)handle->data;
    conn->state = _OUTPUT_DNSSIM_CONN_CLOSED;
    // TODO before memory can be freed, the timeout callback has to have been called
}

static void _write_tcp_query_cb(uv_write_t* wr_req, int status)
{
    _output_dnssim_query_tcp_t* qry = (_output_dnssim_query_tcp_t*)wr_req->data;
    mlassert(qry->conn, "qry must be associated with connection");

    if (status < 0) {  // TODO: handle more gracefully?
        // TODO how to ensure this query is re-tried if there's no conn?
        if (qry->qry.state == _OUTPUT_DNSSIM_QUERY_PENDING_WRITE_CB)
            qry->qry.state = _OUTPUT_DNSSIM_QUERY_PENDING_WRITE;
        mlinfo("tcp write failed: %s", uv_strerror(status));
        // TODO: check if connection is writable, then check state.
        // if state == active, close the connection
        // this is called when conn is closed with uv_close() and there are peding write reqs
        switch(status) {
        case UV_ECANCELED:  // TODO: maybe the switch is useless and _close_connection() can always be called?
            break;
        case UV_ECONNRESET:
        case UV_EPIPE:
        default:
            _close_connection(qry->conn);
            break;
        }
    } else if (qry->qry.state == _OUTPUT_DNSSIM_QUERY_PENDING_WRITE_CB) {
        /* Mark query as sent and assign it to connection. */
        qry->qry.state = _OUTPUT_DNSSIM_QUERY_SENT;
        if (qry->conn->state == _OUTPUT_DNSSIM_CONN_ACTIVE) {
            mlassert(qry->conn->queued, "conn has no queued queries");
            _ll_remove(qry->conn->queued, &qry->qry);
            _ll_append(qry->conn->sent, &qry->qry);
        }
    }

    if (qry->qry.state == _OUTPUT_DNSSIM_QUERY_PENDING_CLOSE) {
        qry->qry.state = _OUTPUT_DNSSIM_QUERY_SENT;
        _output_dnssim_request_t* req = qry->qry.req;
        _close_query_tcp(qry);
        _maybe_free_request(req);
    }

    free(((_output_dnssim_query_tcp_t*)qry)->bufs[0].base);
}

static void _write_tcp_query(_output_dnssim_query_tcp_t* qry, _output_dnssim_connection_t* conn)
{
    mlassert(qry, "qry can't be null");
    mlassert(qry->qry.state == _OUTPUT_DNSSIM_QUERY_PENDING_WRITE, "qry must be pending write");
    mlassert(qry->qry.req, "req can't be null");
    mlassert(qry->qry.req->dns_q, "dns_q can't be null");
    mlassert(qry->qry.req->dns_q->obj_prev, "payload can't be null");
    mlassert(conn, "conn can't be null");
    mlassert(conn->state == _OUTPUT_DNSSIM_CONN_ACTIVE, "connection state != ACTIVE");
    mlassert(conn->client, "conn must be associated with client");
    mlassert(conn->client->pending, "conn has no pending queries");

    mldebug("tcp write dnsmsg id: %04x", qry->qry.req->dns_q->id);

    core_object_payload_t* payload = (core_object_payload_t*)qry->qry.req->dns_q->obj_prev;
    uint16_t* len;
    mlfatal_oom(len = malloc(sizeof(uint16_t)));
    *len = htons(payload->len);
    qry->bufs[0] = uv_buf_init((char*)len, 2);
    qry->bufs[1] = uv_buf_init((char*)payload->payload, payload->len);

    qry->conn = conn;
    _ll_remove(conn->client->pending, &qry->qry);
    _ll_append(conn->queued, &qry->qry);

    qry->write_req.data = (void*)qry;
    uv_write(&qry->write_req, (uv_stream_t*)&conn->handle, qry->bufs, 2, _write_tcp_query_cb);
    qry->qry.state = _OUTPUT_DNSSIM_QUERY_PENDING_WRITE_CB;
}

static void _send_pending_queries(_output_dnssim_connection_t* conn)
{
    _output_dnssim_query_tcp_t* qry = (_output_dnssim_query_tcp_t*)conn->client->pending;

    while (qry != NULL) {
        _output_dnssim_query_tcp_t* next = (_output_dnssim_query_tcp_t*)qry->qry.next;
        mlassert(qry->qry.state == _OUTPUT_DNSSIM_QUERY_PENDING_WRITE,
            "pending query is in not in PENDING_WRITE state");
        _write_tcp_query(qry, conn);
        qry = next;
    }
}

void _process_tcp_dnsmsg(_output_dnssim_connection_t* conn)
{
    mlassert(conn, "conn can't be nil");

    core_object_payload_t payload = CORE_OBJECT_PAYLOAD_INIT(NULL);
    core_object_dns_t dns_a = CORE_OBJECT_DNS_INIT(&payload);

    payload.payload = conn->recv_data;
    payload.len = conn->recv_len;

    dns_a.obj_prev = (core_object_t*)&payload;
    int ret = core_object_dns_parse_header(&dns_a);
    if (ret != 0) {
        mlwarning("tcp response malformed");
        return;
    }
    mldebug("tcp recv dnsmsg id: %04x", dns_a.id);

    _output_dnssim_query_t* qry = conn->sent;
    while (qry != NULL) {
        if (qry->req->dns_q->id == dns_a.id) {
            _request_answered(qry->req, &dns_a);
            _ll_remove(conn->sent, qry);
            _close_request(qry->req);
            break;
        }
        qry = qry->next;
    }
}

void _parse_recv_data(_output_dnssim_connection_t* conn)
{
    mlassert(conn, "conn can't be nil");
    mlassert(conn->recv_pos == conn->recv_len, "attempt to parse incomplete recv_data");

    switch(conn->read_state) {
    case _OUTPUT_DNSSIM_READ_STATE_DNSLEN: {
        uint16_t* p_dnslen = (uint16_t*)conn->recv_data;
        conn->recv_len = ntohs(*p_dnslen);
        mldebug("tcp dnslen: %d", conn->recv_len);
        conn->read_state = _OUTPUT_DNSSIM_READ_STATE_DNSMSG;
        break;
    }
    case _OUTPUT_DNSSIM_READ_STATE_DNSMSG:
        _process_tcp_dnsmsg(conn);
        conn->recv_len = 2;
        conn->read_state = _OUTPUT_DNSSIM_READ_STATE_DNSLEN;
        break;
    default:
        mlfatal("tcp invalid connection read_state");
        break;
    }

    conn->recv_pos = 0;
    if (conn->recv_free_after_use) {
        conn->recv_free_after_use = false;
        free(conn->recv_data);
    }
    conn->recv_data = NULL;
}

unsigned int _read_tcp_stream(_output_dnssim_connection_t* conn, size_t len, const char* data)
{
    mlassert(conn, "conn can't be nil");
    mlassert(data, "data can't be nil");
    mlassert(len > 0, "no data to read");
    mlassert((conn->read_state == _OUTPUT_DNSSIM_READ_STATE_DNSLEN ||
              conn->read_state == _OUTPUT_DNSSIM_READ_STATE_DNSMSG),
             "connection has invalid read_state");

    unsigned int nread;
    size_t expected = conn->recv_len - conn->recv_pos;
    mlassert(expected > 0, "no data expected");

    if (conn->recv_free_after_use == false && expected > len) {
        /* Start of partial read. */
        mlassert(conn->recv_pos == 0, "conn->recv_pos must be 0 at start of partial read");
        mlassert(conn->recv_len > 0, "conn->recv_len must be set at start of partial read");
        mlfatal_oom(conn->recv_data = malloc(conn->recv_len * sizeof(char)));
        conn->recv_free_after_use = true;
    }

    if (conn->recv_free_after_use) {  /* Partial read is in progress. */
        char* dest = conn->recv_data + conn->recv_pos;
        if (expected < len)
            len = expected;
        memcpy(dest, data, len);
        conn->recv_pos += len;
        nread = len;
    } else {  /* Complete and clean read. */
        mlassert(expected <= len, "not enough data to perform complete read");
        conn->recv_data = (char*)data;
        conn->recv_pos = conn->recv_len;
        nread = expected;
    }

    /* If entire dnslen/dnsmsg was read, parse it. */
    if (conn->recv_len == conn->recv_pos)
        _parse_recv_data(conn);

    return nread;
}

static void _on_tcp_read(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf)
{
    _output_dnssim_connection_t* conn = (_output_dnssim_connection_t*)handle->data;
    if (nread > 0) {
        int pos = 0;
        char* data = buf->base;
        while (pos < nread)
            pos += _read_tcp_stream(conn, nread - pos, data + pos);
        mlassert(pos == nread, "tcp data read invalid, pos != nread");
    } else if (nread < 0) {
        if (nread != UV_EOF)
            mlinfo("tcp conn unexpected close: %s", uv_strerror(nread));
        _close_connection(conn);
    }

    if (buf->base != NULL)
        free(buf->base);
}

static void _on_tcp_handle_connected(uv_connect_t* conn_req, int status)
{
    _output_dnssim_connection_t* conn = (_output_dnssim_connection_t*)conn_req->handle->data;
    free(conn_req);

    if (status < 0) {
        mlwarning("tcp connect failed: %s", uv_strerror(status));
        _close_connection(conn);
        return;
    }

    mlassert(conn->state == _OUTPUT_DNSSIM_CONN_CONNECTING, "connection state != CONNECTING");
    int ret = uv_read_start((uv_stream_t*)&conn->handle, _uv_alloc_cb, _on_tcp_read);
    if (ret < 0) {
        mlwarning("tcp uv_read_start() failed: %s", uv_strerror(ret));
        _close_connection(conn);
        return;
    }

    conn->state = _OUTPUT_DNSSIM_CONN_ACTIVE;
    conn->read_state = _OUTPUT_DNSSIM_READ_STATE_DNSLEN;
    conn->recv_len = 2;
    conn->recv_pos = 0;
    conn->recv_free_after_use = false;

    _send_pending_queries(conn);
}

static void _close_connection(_output_dnssim_connection_t* conn)
{
    mlassert(conn, "conn can't be nil");
    if (conn->state == _OUTPUT_DNSSIM_CONN_CLOSING || conn->state == _OUTPUT_DNSSIM_CONN_CLOSED)
        return;

    // TODO move queries to pending
    // TODO if client has pending queries and no active/connecting connections, establish new

    // TODO: if try_remove the best approach? when should this be called?
    if (conn->client != NULL)
        _ll_try_remove(conn->client->conn, conn);

    conn->state = _OUTPUT_DNSSIM_CONN_CLOSING;
    uv_timer_stop(&conn->timeout);
    uv_close((uv_handle_t*)&conn->timeout, NULL);
    uv_read_stop((uv_stream_t*)&conn->handle);
    uv_close((uv_handle_t*)&conn->handle, _on_tcp_handle_closed);
}

static void _on_connection_timeout(uv_timer_t* handle)
{
    _output_dnssim_connection_t* conn = (_output_dnssim_connection_t*)handle->data;
    _close_connection(conn);
}

static void _refresh_tcp_connection_timeout(_output_dnssim_connection_t* conn)
{
    mlassert(conn, "conn can't be nil");
    if (conn->state == _OUTPUT_DNSSIM_CONN_CLOSING || conn->state == _OUTPUT_DNSSIM_CONN_CLOSED)
        return;

    int ret = uv_timer_start(&conn->timeout, _on_connection_timeout, 15000, 0);  // TODO: un-hardcode
    if (ret < 0)
        mlfatal("failed uv_timer_start(): %s", uv_strerror(ret));
}

static int _connect_tcp_handle(output_dnssim_t* self, _output_dnssim_connection_t* conn)
{
    mlassert_self();
    lassert(conn, "connection can't be null");
    lassert(conn->state == _OUTPUT_DNSSIM_CONN_INITIALIZED, "connection state != INITIALIZED");

    conn->handle.data = (void*)conn;
    int ret = uv_tcp_init(&_self->loop, &conn->handle);
    if (ret < 0) {
        lwarning("failed to init uv_tcp_t");
        goto failure;
    }

    ret = _bind_before_connect(self, (uv_handle_t*)&conn->handle);
    if (ret < 0)
        goto failure;

    /* Set connection parameters. */
    ret = uv_tcp_nodelay(&conn->handle, 1);
    if (ret < 0)
        lwarning("tcp: failed to set TCP_NODELAY: %s", uv_strerror(ret));

    // TODO: make this configurable
    // ret = uv_tcp_keepalive(&conn->handle, 1, 5);
    // if (ret < 0)
    //     mlwarning("tcp: failed to set TCP_KEEPALIVE: %s", uv_strerror(ret));

    /* Set connection inactivity timeout. */
    ret = uv_timer_init(&_self->loop, &conn->timeout);
    if (ret < 0)
        mlfatal("failed uv_timer_init(): %s", uv_strerror(ret));
    conn->timeout.data = (void*)conn;
    _refresh_tcp_connection_timeout(conn);

    uv_connect_t* conn_req;
    lfatal_oom(conn_req = malloc(sizeof(uv_connect_t)));
    ret = uv_tcp_connect(conn_req, &conn->handle, (struct sockaddr*)&_self->target, _on_tcp_handle_connected);
    if (ret < 0)
        goto failure;

    conn->state = _OUTPUT_DNSSIM_CONN_CONNECTING;
    return 0;
failure:
    _close_connection(conn);
    return ret;
}

static int _create_query_tcp(output_dnssim_t* self, _output_dnssim_request_t* req)
{
    mlassert_self();
    lassert(req->client, "request must have a client associated with it");

    int ret;
    _output_dnssim_query_tcp_t* qry;
    _output_dnssim_connection_t* conn;
    core_object_payload_t* payload = (core_object_payload_t*)req->dns_q->obj_prev;

    lfatal_oom(qry = calloc(1, sizeof(_output_dnssim_query_tcp_t)));

    qry->qry.transport = OUTPUT_DNSSIM_TRANSPORT_TCP;
    qry->qry.req = req;
    qry->qry.state = _OUTPUT_DNSSIM_QUERY_PENDING_WRITE;
    _ll_append(req->qry, &qry->qry);
    _ll_append(req->client->pending, &qry->qry);

    /* Get active TCP connection or find out whether new connection has to be opened. */
    bool is_connecting = false;
    conn = req->client->conn;
    while (conn != NULL) {
        if (conn->state == _OUTPUT_DNSSIM_CONN_ACTIVE)
            break;
        else if (conn->state == _OUTPUT_DNSSIM_CONN_CONNECTING)
            is_connecting = true;
        conn = conn->next;
    }

    if (conn != NULL) {  /* Send data right away over active connection. */
        _send_pending_queries(conn);
    } else if (!is_connecting) {  /* No active or connecting connection -> open a new one. */
        lfatal_oom(conn = calloc(1, sizeof(_output_dnssim_connection_t)));  // TODO free
        conn->state = _OUTPUT_DNSSIM_CONN_INITIALIZED;
        conn->client = req->client;
        ret = _connect_tcp_handle(self, conn);
        if (ret < 0)
            return ret;
        _ll_append(req->client->conn, conn);
    } /* Otherwise, pending queries wil be sent after connected callback. */

    return 0;
}

static void _close_query_tcp(_output_dnssim_query_tcp_t* qry)
{
    mlassert(qry, "qry can't be null");
    mlassert(qry->qry.req, "query must be part of a request");
    _output_dnssim_request_t* req = qry->qry.req;
    mlassert(req->client, "request must belong to a client");

    if ((qry->qry.state == _OUTPUT_DNSSIM_QUERY_PENDING_WRITE_CB ||
         qry->qry.state == _OUTPUT_DNSSIM_QUERY_PENDING_CLOSE)) {
        /* Query can't be freed until uv callback is called. */
        qry->qry.state = _OUTPUT_DNSSIM_QUERY_PENDING_CLOSE;
        return;
    }

    _ll_try_remove(req->client->pending, &qry->qry);
    if (qry->conn) {
        _ll_try_remove(qry->conn->queued, &qry->qry);
        _ll_try_remove(qry->conn->sent, &qry->qry);
    }

    _ll_remove(req->qry, &qry->qry);
    free(qry);
}