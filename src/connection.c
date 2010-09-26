/**
 *
 * Copyright (c) 2010, Zed A. Shaw and Mongrel2 Project Contributors.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 * 
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 * 
 *     * Neither the name of the Mongrel2 Project, Zed A. Shaw, nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <sys/socket.h>

#include "connection.h"
#include "host.h"
#include "http11/http11_parser.h"
#include "http11/httpclient_parser.h"
#include "bstring.h"
#include "dbg.h"
#include "task/task.h"
#include "events.h"
#include "register.h"
#include "handler.h"
#include "pattern.h"
#include "dir.h"
#include "proxy.h"
#include "response.h"
#include "mem/halloc.h"
#include "setting.h"
#include "log.h"

struct tagbstring PING_PATTERN = bsStatic("@[a-z/]- {\"type\":\\s*\"ping\"}");

#define TRACE(C) debug("--> %s(%s:%d) %s:%d ", "" #C, State_event_name(event), event, __FUNCTION__, __LINE__)

#define error_response(F, C, M, ...)  {Response_send_status(F, &HTTP_##C); sentinel(M, ##__VA_ARGS__);}
#define error_unless(T, F, C, M, ...) if(!(T)) error_response(F, C, M, ##__VA_ARGS__)


int MAX_CONTENT_LENGTH = 20 * 1024;
int BUFFER_SIZE = 4 * 1024;
int CONNECTION_STACK = 32 * 1024;

static inline int Connection_backend_event(Backend *found, Connection *conn)
{
    switch(found->type) {
        case BACKEND_HANDLER:
            return HANDLER;
        case BACKEND_DIR:
            return DIRECTORY;
        case BACKEND_PROXY:
            return PROXY;
        default:
            error_response(conn, 501, "Invalid backend type: %d", found->type);
    }

error:
    return CLOSE;
}


int connection_open(int event, void *data)
{
    TRACE(open);
    Connection *conn = (Connection *)data;

    if(!conn->registered) {
        conn->registered = 1;
    }

    return ACCEPT;
}



int connection_finish(int event, void *data)
{
    TRACE(finish);

    Connection_destroy((Connection *)data);

    return CLOSE;
}



int connection_send_socket_response(int event, void *data)
{
    TRACE(socket_req);
    Connection *conn = (Connection *)data;

    int rc = Response_send_socket_policy(conn);
    check_debug(rc > 0, "Failed to write Flash socket response.");

    return RESP_SENT;

error:
    return CLOSE;
}


int connection_route_request(int event, void *data)
{
    TRACE(route);
    Connection *conn = (Connection *)data;
    Host *host = NULL;
    bstring pattern = NULL;

    bstring path = Request_path(conn->req);

    if(conn->req->host_name) {
        host = Server_match_backend(conn->server, conn->req->host_name);
    } else {
        host = conn->server->default_host;
    }
    error_unless(host, conn, 404, "Request for a host we don't have registered: %s", bdata(conn->req->host_name));

    Backend *found = Host_match_backend(host, path, &pattern);
    error_unless(found, conn, 404, "Handler not found: %s", bdata(path));

    Request_set_action(conn->req, found);
    conn->req->target_host = host;
    conn->req->pattern = pattern;

    return Connection_backend_event(found, conn);

error:
    return CLOSE;
}



int connection_msg_to_handler(int event, void *data)
{
    TRACE(msg_to_handler);
    Connection *conn = (Connection *)data;
    Handler *handler = Request_get_action(conn->req, handler);
    int rc = 0;

    check(handler, "JSON request doesn't match any handler: %s", 
            bdata(Request_path(conn->req)));


    if(pattern_match(conn->buf, conn->nparsed, bdata(&PING_PATTERN))) {
        Log_request(conn, 200, 0);
        Register_ping(conn->fd);
    } else {
        // TODO: Get ragel to do this, not us.
        int header_len = blength(Request_path(conn->req)) + 1;
        check(conn->nread - header_len - 1 >= 0, "Header length calculation is wrong.");

        Log_request(conn, 200, conn->nread - header_len - 1);

        bstring payload = Request_to_payload(conn->req, handler->send_ident,
                conn->fd, conn->buf + header_len, conn->nread - header_len - 1);

        debug("MSG TO HANDLER: %s", bdata(payload));

        rc = Handler_deliver(handler->send_socket, bdata(payload), blength(payload));
        bdestroy(payload);
    
        check(rc == 0, "Failed to deliver to handler: %s", bdata(Request_path(conn->req)));
    }

    return REQ_SENT;

error:
    return CLOSE;
}


static inline bstring connection_upload_file(Connection *conn, Handler *handler,
        int header_len, int content_len)
{
    int rc = 0;
    int tmpfd = 0;
    bstring result = NULL;

    // need a setting for the moby store where large uploads should go
    bstring upload_store = Setting_get_str("upload.temp_store", NULL);
    error_unless(upload_store, conn, 413, "Request entity is too large: %d, and no upload.temp_store setting for where to put the big files.", content_len);

    upload_store = bstrcpy(upload_store); // Setting owns the original

    // TODO: handler should be set to allow large uploads, otherwise error

    tmpfd = mkstemp((char *)upload_store->data);
    check(tmpfd != -1, "Failed to create secure tempfile, did you end it with XXXXXX?");
    log_info("Writing tempfile %s for large upload.", bdata(upload_store));

    // send the initial headers we have so they can kill it if they want
    dict_alloc_insert(conn->req->headers, bfromcstr("X-Mongrel2-Upload-Start"), upload_store);

    result = Request_to_payload(conn->req, handler->send_ident, conn->fd, "", 0);
    check(result, "Failed to create initial payload for upload attempt.");

    rc = Handler_deliver(handler->send_socket, bdata(result), blength(result));
    check(rc != -1, "Failed to deliver upload attempt to handler.");

    // all good so start streaming chunks into the temp file in the moby dir
    if(conn->nread - header_len > 0) {
        rc = fdwrite(tmpfd, conn->buf + header_len, conn->nread - header_len);
        check(rc == conn->nread - header_len, "Failed to write initial chunk to upload file: %s", bdata(upload_store));
    }

    conn->nread = 0;

    for(content_len -= rc; content_len > 0; content_len -= rc)
    {
        int nread = content_len < BUFFER_SIZE ? content_len : BUFFER_SIZE;

        nread = conn->recv(conn, conn->buf, nread);
        check(nread > 0, "Failed to read from socket on upload.");

        rc = fdwrite(tmpfd, conn->buf, nread);
        check(rc > 0, "Failed to write to %s upload tempfile.", bdata(upload_store));
    }

    check(content_len == 0, "Bad math on writing out the upload tmpfile: %s, it's %d", bdata(upload_store), content_len);

    // moby dir write is done, add a header to the request that indicates where to get it
    dict_alloc_insert(conn->req->headers, bfromcstr("X-Mongrel2-Upload-Done"), upload_store);

    bdestroy(result);
    fdclose(tmpfd);
    return upload_store;

error:
    bdestroy(result);
    fdclose(tmpfd);

    if(upload_store) {
        unlink((char *)upload_store->data);
        bdestroy(upload_store);
    }

    return NULL;
}

int connection_http_to_handler(int event, void *data)
{
    TRACE(http_to_handler);
    Connection *conn = (Connection *)data;
    int content_len = Request_content_length(conn->req);
    int header_len = Request_header_length(conn->req);
    int total = header_len + content_len;
    int rc = 0;
    char *body = NULL;
    bstring result = NULL;

    Handler *handler = Request_get_action(conn->req, handler);
    error_unless(handler, conn, 404, "No action for request: %s", bdata(Request_path(conn->req)));


    if(content_len == 0) {
        body = "";
    } else if(content_len > MAX_CONTENT_LENGTH) {
        bstring upload_store = connection_upload_file(conn, handler, header_len, content_len);

        body = "";
        content_len = 0;
        check(upload_store != NULL, "Failed to upload file.");
    } else {
        if(total > BUFFER_SIZE) conn->buf = h_realloc(conn->buf, total);

        if(conn->nread < total) {
            // start at the tail of what we've got so far
            body = conn->buf + conn->nread; 

            int remaining = 0;
            for(remaining = content_len; remaining > 0; remaining -= rc, body += rc) {
                rc = conn->recv(conn, body, remaining);
                check_debug(rc > 0, "Read error from MSG listener %d", conn->fd);
            }
            check(remaining == 0, "Bad math on reading request < MAX_CONTENT_LENGTH: %d", remaining);
        }

        // no matter what, the body will need to go here
        body = conn->buf + header_len;
    }

    Log_request(conn, 200, content_len);

    result = Request_to_payload(conn->req, handler->send_ident, conn->fd, body, content_len);
    check(result, "Failed to create payload for request.");

    debug("HTTP TO HANDLER: %.*s", blength(result) - content_len, bdata(result));

    rc = Handler_deliver(handler->send_socket, bdata(result), blength(result));
    error_unless(rc != -1, conn, 502, "Failed to deliver to handler: %s", 
            bdata(Request_path(conn->req)));

    bdestroy(result);
    return REQ_SENT;

error:
    bdestroy(result);
    return CLOSE;
}



int connection_http_to_directory(int event, void *data)
{
    TRACE(http_to_directory);
    Connection *conn = (Connection *)data;

    Dir *dir = Request_get_action(conn->req, dir);

    int rc = Dir_serve_file(dir, conn->req, conn);
    check_debug(rc == 0, "Failed to serve file: %s", bdata(Request_path(conn->req)));

    Log_request(conn, conn->req->status_code, conn->req->response_size);

    if(conn->close) {
        debug("Closing connection after sending file.");
        return CLOSE;
    } else {
        return RESP_SENT;
    }

error:
    return CLOSE;
}




int connection_http_to_proxy(int event, void *data)
{
    TRACE(http_to_proxy);
    Connection *conn = (Connection *)data;
    Proxy *proxy = Request_get_action(conn->req, proxy);
    check(proxy != NULL, "Should have a proxy backend.");

    conn->proxy_fd = netdial(1, bdata(proxy->server), proxy->port);
    check(conn->proxy_fd != -1, "Failed to connect to proxy backend %s:%d",
            bdata(proxy->server), proxy->port);

    if(!conn->proxy_buf) {
        conn->proxy_buf = h_calloc(sizeof(char), BUFFER_SIZE+1);
        check_mem(conn->proxy_buf);
        hattach(conn->proxy_buf, conn);
    }

    if(!conn->client) {
        conn->client = h_calloc(sizeof(httpclient_parser), 1);
        check_mem(conn->client);
        hattach(conn->client, conn);
    }

    return CONNECT;

error:
    return FAILED;
}



int connection_proxy_deliver(int event, void *data)
{
    TRACE(proxy_deliver);
    Connection *conn = (Connection *)data;
    int rc = 0;

    int total_len = Request_header_length(conn->req) + Request_content_length(conn->req);

    if(total_len < conn->nread) {
        rc = fdsend(conn->proxy_fd, conn->buf, total_len);
        check_debug(rc > 0, "Failed to write request to proxy.");

        // setting up for the next request to be read
        conn->nread -= total_len;
        memmove(conn->buf, conn->buf + total_len, conn->nread);
    } else if (total_len > conn->nread) {
        // we haven't read everything, need to do some streaming
        do {
            // TODO: look at sendfile or splice to do this instead
            rc = fdsend(conn->proxy_fd, conn->buf, conn->nread);
            check_debug(rc == conn->nread, "Failed to write full request to proxy after %d read.", conn->nread);

            total_len -= rc;

            if(total_len > 0) {
                conn->nread = conn->recv(conn, conn->buf, BUFFER_SIZE);
                check_debug(conn->nread > 0, "Failed to read from client more data with %d left.", total_len);
            } else {
                conn->nread = 0;
            }
        } while(total_len > 0);
    } else {
        // not > and not < means ==, so we just write this and try again
        rc = fdsend(conn->proxy_fd, conn->buf, total_len);
        check_debug(rc == total_len, "Failed to write complete request to proxy, wrote only: %d", rc);
        conn->nread = 0;
    }

    return REQ_SENT;

error:
    return REMOTE_CLOSE;
}



int connection_proxy_reply_parse(int event, void *data)
{
    TRACE(proxy_reply_parse);
    int nread = 0;
    int rc = 0;
    Connection *conn = (Connection *)data;
    Proxy *proxy = Request_get_action(conn->req, proxy);
    httpclient_parser *client = conn->client;

    nread = Proxy_read_and_parse(conn, 0);
    check(nread != -1, "Failed to read from proxy server: %s:%d", 
            bdata(proxy->server), proxy->port);

    if(client->chunked) {
        rc = Proxy_stream_chunks(conn, nread);
        check(rc != -1, "Failed to stream chunked encoding to client.");

    } else if(client->content_len >= 0) {
        rc = Proxy_stream_response(conn, client->body_start + client->content_len, nread);
        check(rc != -1, "Failed streaming non-chunked response.");

    } else if(client->content_len == -1) {
        debug("No chunked encoding and no content-length header, we'll read until close: %s",
               conn->proxy_buf);

        do {
            rc = conn->send(conn, conn->proxy_buf, nread);
            check(rc == nread, "Failed to send all of the request: %d length.", nread);
        } while((nread = fdrecv(conn->proxy_fd, conn->proxy_buf, BUFFER_SIZE)) > 0);
    } else {
        sentinel("Should not reach this code, Tell Zed.");
    }

    Log_request(conn, client->status, client->content_len);
    return REQ_RECV;

error:
    return FAILED;
}


int connection_proxy_req_parse(int event, void *data)
{
    TRACE(proxy_req_parse);

    int rc = 0;
    Connection *conn = (Connection *)data;
    bstring host = NULL;
    Host *target_host = conn->req->target_host;
    Backend *req_action = conn->req->action;

    // unlike other places, we keep the nread rather than reset
    rc = Connection_read_header(conn, conn->req);
    check_debug(rc > 0, "Failed to read another header.");
    error_unless(Request_is_http(conn->req), conn, 400,
            "Someone tried to change the protocol on us from HTTP.");

    host = bstrcpy(conn->req->host);
    // do a light find of this request compared to the last one
    if(!biseq(host, conn->req->host)) {
        bdestroy(host);
        return PROXY;
    } else {
        bdestroy(host);

        // query up the path to see if it gets the current request action
        Backend *found = Host_match_backend(target_host, Request_path(conn->req), NULL);
        error_unless(found, conn, 404, 
                "Handler not found: %s", bdata(Request_path(conn->req)));

        if(found != req_action) {
            Request_set_action(conn->req, found);
            return Connection_backend_event(found, conn);
        } else {
            // TODO: since we found it already, keep it set and reuse
            return HTTP_REQ;
        }
    }

    error_response(conn, 500, "Invalid code branch, tell Zed.");
error:

    bdestroy(host);
    return REMOTE_CLOSE;
}



int connection_proxy_failed(int event, void *data)
{
    TRACE(proxy_failed);
    Connection *conn = (Connection *)data;

    Response_send_status(conn, &HTTP_502);

    return CLOSE;
}


int connection_proxy_close(int event, void *data)
{
    TRACE(proxy_close);

    Connection *conn = (Connection *)data;
    fdclose(conn->proxy_fd);
    conn->proxy_fd = 0;

    return CLOSE;
}

static inline int close_or_error(int event, void *data, int next)
{
    Connection *conn = (Connection *)data;

    if(conn->proxy_fd) {
        connection_proxy_close(event, data);
    }

    check(Register_disconnect(conn->fd) != -1, "Register disconnect didn't work for %d", conn->fd);

error:
    // fallthrough on purpose
    return next;
}


int connection_close(int event, void *data)
{
    TRACE(close);
    return close_or_error(event, data, 0);
}



int connection_error(int event, void *data)
{
    TRACE(error);
    return close_or_error(event, data, CLOSE);
}


static inline int ident_and_register(int event, void *data, int reg_too)
{
    Connection *conn = (Connection *)data;
    int conn_type = 0;
    int next = CLOSE;

#ifndef NDEBUG
    bstring user_agent = Request_get(conn->req, &HTTP_USER_AGENT);

    if(user_agent) {
        debug("Got a user-agent: %s", bdata(user_agent));
        if(pattern_match(bdata(user_agent), blength(user_agent), "httperf.*")) {
            log_err("\n\n\n\n\n------\nWHAT!? WHY ARE YOU PERFORMANCE TESTING WITH DEBUGGING ON?!\nAt least you're using httperf.\n\n\n\n");
        } else if(pattern_match(bdata(user_agent), blength(user_agent), "ApacheBench.*")) {
            log_err("\n\n\n\n\n------\nWHAT!? WHY ARE YOU PERFORMANCE TESTING WITH DEBUGGING ON?!\nAB is a giant piece of crap that uses HTTP/1.0.\n\n\n\n");
        } else if(pattern_match(bdata(user_agent), blength(user_agent), ".*Siege.*")) {
            log_err("\n\n\n\n\n------\nWHAT!? WHY ARE YOU PERFORMANCE TESTING WITH DEBUGGING ON?!\nSiege is junk, there's no such measurable concept as 'user'. Snakeoil.\n\n\n\n");
        }
    }
#endif

    if(Request_is_socket(conn->req)) {
        conn_type = CONN_TYPE_SOCKET;
        taskname("SOCKET");
        next = SOCKET_REQ;
    } else if(Request_is_json(conn->req)) {
        conn_type = CONN_TYPE_MSG;
        taskname("MSG");
        next = MSG_REQ;
    } else if(Request_is_http(conn->req)) {
        conn_type = CONN_TYPE_HTTP;
        taskname("HTTP");
        next = HTTP_REQ;
    } else {
        error_response(conn, 500, "Invalid code branch, tell Zed.");
    }

    if(reg_too) Register_connect(conn->fd, conn_type);
    return next;

error:
    return CLOSE;

}

int connection_register_request(int event, void *data)
{
    TRACE(register_request);
    return ident_and_register(event, data, 1);
}


int connection_identify_request(int event, void *data)
{
    TRACE(identify_request);
    return ident_and_register(event, data, 0);
}



int connection_parse(int event, void *data)
{
    Connection *conn = (Connection *)data;
    conn->nread = 0;

    if(Connection_read_header(conn, conn->req) > 0) {
        return REQ_RECV;
    } else {
        return CLOSE;
    }
}


StateActions CONN_ACTIONS = {
    .open = connection_open,
    .error = connection_error,
    .finish = connection_finish,
    .close = connection_close,
    .parse = connection_parse,
    .register_request = connection_register_request,
    .identify_request = connection_identify_request,
    .route_request = connection_route_request,
    .send_socket_response = connection_send_socket_response,
    .msg_to_handler = connection_msg_to_handler,
    .http_to_handler = connection_http_to_handler,
    .http_to_proxy = connection_http_to_proxy,
    .http_to_directory = connection_http_to_directory,
    .proxy_deliver = connection_proxy_deliver,
    .proxy_failed = connection_proxy_failed,
    .proxy_reply_parse = connection_proxy_reply_parse,
    .proxy_req_parse = connection_proxy_req_parse,
    .proxy_close = connection_proxy_close
};



void Connection_destroy(Connection *conn)
{
    if(conn) {
        Request_destroy(conn->req);
        conn->req = NULL;
        if(conn->ssl) 
            ssl_free(conn->ssl);
        h_free(conn);
    }
}

static ssize_t plaintext_send(Connection *conn, char *buffer, int len)
{
    return fdsend(conn->fd, buffer, len);
}

static ssize_t plaintext_recv(Connection *conn, char *buffer, int len)
{
    return fdrecv(conn->fd, buffer, len);
}

static ssize_t ssl_send(Connection *conn, char *buffer, int len)
{
    check(conn->ssl != NULL, "Cannot ssl_send on a connection without ssl");
    
    return ssl_write(conn->ssl, (const unsigned char*) buffer, len);

error:
    return -1;
}

static ssize_t ssl_recv(Connection *conn, char *buffer, int len)
{
    check(conn->ssl != NULL, "Cannot ssl_recv on a connection without ssl");
    char *pread;
    int nread;

    // If we didn't read all of what we recieved last time
    if(conn->ssl_buff != NULL) {
        if(conn->ssl_buff_len < len) {
            nread = conn->ssl_buff_len;
            pread = conn->ssl_buff;
            conn->ssl_buff_len = 0;
            conn->ssl_buff = NULL;
        }
        else {
            nread = len;
            pread = conn->ssl_buff;
            conn->ssl_buff += nread;
            conn->ssl_buff_len -= nread;
        }
    }
    else {
        do {
            nread = ssl_read(conn->ssl, (unsigned char **) &pread);
        } while(nread == SSL_OK);

        // If we got more than they asked for, we should stash it for
        // successive calls.
        if(nread > len) {
            conn->ssl_buff = buffer + len;
            conn->ssl_buff_len = nread - len;
            nread = len;
        }
    }
    if(nread < 0) 
        return nread;

    check(pread != NULL, "Got a NULL from ssl_read despite no error code");
    
    memcpy(buffer, pread, nread);
    return nread;

error:
    return -1;
}

Connection *Connection_create(Server *srv, int fd, int rport, 
                              const char *remote, SSL_CTX *ssl_ctx)
{
    Connection *conn = h_calloc(sizeof(Connection), 1);
    check_mem(conn);

    conn->server = srv;
    conn->fd = fd;

    conn->rport = rport;
    memcpy(conn->remote, remote, IPADDR_SIZE);
    conn->remote[IPADDR_SIZE] = '\0';

    conn->req = Request_create();
    check_mem(conn->req);

    conn->buf = h_calloc(sizeof(char), BUFFER_SIZE + 1);
    check_mem(conn->buf);
    hattach(conn->buf, conn);

    conn->ssl_buff = 0;
    conn->ssl_buff_len = 0;
    conn->ssl = NULL;
    if(ssl_ctx != NULL)
    {
        conn->ssl = ssl_server_new(ssl_ctx, conn->fd);
        check(conn->ssl != NULL, "Failed to create new ssl for connection");
        conn->send = ssl_send;
        conn->recv = ssl_recv;
    }
    else
    {
        conn->ssl = NULL;
        conn->send = plaintext_send;
        conn->recv = plaintext_recv;
    }
    return conn;

error:
    Connection_destroy(conn);
    return NULL;
}


void Connection_accept(Connection *conn)
{
    taskcreate(Connection_task, conn, CONNECTION_STACK);
}



void Connection_task(void *v)
{
    Connection *conn = (Connection *)v;
    int i = 0;
    int next = 0;

    State_init(&conn->state, &CONN_ACTIONS);

    for(i = 0, next = OPEN; next != CLOSE; i++) {
        next = State_exec(&conn->state, next, (void *)conn);
        error_unless(next >= FINISHED && next < EVENT_END, conn, 500, 
                "!!! Invalid next event[%d]: %d, Tell ZED!", i, next);
    }

    State_exec(&conn->state, CLOSE, (void *)conn);
    return;

error:
    State_exec(&conn->state, CLOSE, (void *)conn);
    return;
}

int Connection_deliver_raw(int to_fd, bstring buf)
{
    return fdsend(to_fd, bdata(buf), blength(buf));
}

int Connection_deliver(int to_fd, bstring buf)
{
    int rc = 0;

    bstring b64_buf = bBase64Encode(buf);
    rc = fdsend(to_fd, bdata(b64_buf), blength(b64_buf)+1);
    check_debug(rc == blength(b64_buf)+1, "Failed to write entire message to conn %d", to_fd);

    bdestroy(b64_buf);
    return 0;

error:
    bdestroy(b64_buf);
    return -1;
}


static inline void check_should_close(Connection *conn, Request *req)
{
    // TODO: this should be right but double check
    if(req->version && biseqcstr(req->version, "HTTP/1.0")) {
        debug("HTTP 1.0 request coming in from %s", conn->remote);
        conn->close = 1;
    } else {
        bstring conn_close = Request_get(req, &HTTP_CONNECTION);

        if(conn_close && biseqcstrcaseless(conn_close, "close")) {
            conn->close = 1;
        } else {
            conn->close = 0;
        }
    }
}


int Connection_read_header(Connection *conn, Request *req)
{
    int finished = 0;
    int n = 0;
    conn->nparsed = 0;
    int remaining = BUFFER_SIZE - 1;
    char *cur_buf = conn->buf;

    Request_start(req);

    conn->buf[BUFFER_SIZE] = '\0';  // always cap it off

    while(finished != 1 && remaining >= 0) {
        n = conn->recv(conn, cur_buf, remaining);
        check_debug(n > 0, "Failed to read from socket after %d read: %d parsed.",
                    conn->nread, (int)conn->nparsed);
        conn->nread += n;

        check(conn->buf[BUFFER_SIZE] == '\0', "Trailing \\0 was clobbered, buffer overflow potentially.");

        finished = Request_parse(req, conn->buf, conn->nread, &conn->nparsed);

        remaining = BUFFER_SIZE - 1 - conn->nread;
        cur_buf = conn->buf + conn->nread; 
    }

    error_unless(finished == 1, conn, 400, 
            "Error in parsing: %d, bytes: %d, value: %.*s, parsed: %d", 
            finished, conn->nread, conn->nread, conn->buf, (int)conn->nparsed);

    // add the x-forwarded-for header
    dict_alloc_insert(conn->req->headers, bfromcstr("X-Forwarded-For"),
            blk2bstr(conn->remote, IPADDR_SIZE));

    check_should_close(conn, conn->req);
    return conn->nread; 

error:
    return -1;

}


void Connection_init()
{
    MAX_CONTENT_LENGTH = Setting_get_int("limits.content_length", 20 * 1024);
    BUFFER_SIZE = Setting_get_int("limits.buffer_size", 4 * 1024);
    CONNECTION_STACK = Setting_get_int("limits.connection_stack_size", 32 * 1024);

    log_info("MAX limits.content_length=%d, limits.buffer_size=%d, limits.connection_stack_size=%d",
            MAX_CONTENT_LENGTH, BUFFER_SIZE, CONNECTION_STACK);
}

