/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "manager.h"
#include "hostlist.h"
#include "iotable.h"
#include "timer-ng.h"
#include "internal.h"

#define LOGARGS(mgr, lvl) mgr->settings, "lcbio_mgr", LCB_LOG_##lvl, __FILE__, __LINE__

typedef enum {
    CS_PENDING,
    CS_IDLE,
    CS_LEASED
} cinfo_state;

typedef enum {
    RS_PENDING,
    RS_ASSIGNED
} request_state;

typedef char mgr_KEY[NI_MAXSERV + NI_MAXHOST + 2];

typedef struct lcbio_MGRHOST {
    lcb_clist_t ll_idle; /* idle connections */
    lcb_clist_t ll_pending; /* pending cinfo */
    lcb_clist_t requests; /* pending requests */
    mgr_KEY key; /* host:port */
    struct lcbio_MGR *parent;
    lcbio_pASYNC async;
    unsigned n_total; /* number of total connections */
    unsigned refcount;
} mgr_HOST;

struct CinfoNode : lcb_list_t {};

struct mgr_CINFO : lcbio_PROTOCTX, CinfoNode {
    inline mgr_CINFO(mgr_HOST *parent, uint32_t timeout);
    inline ~mgr_CINFO();
    inline void on_idle_timeout();
    inline void on_connected(lcbio_SOCKET *sock, lcb_error_t err);

    static mgr_CINFO *from_llnode(lcb_list_t *node) {
        return static_cast<mgr_CINFO*>(static_cast<CinfoNode*>(node));
    }

    static mgr_CINFO *from_sock(lcbio_SOCKET *sock) {
        lcbio_PROTOCTX *ctx = lcbio_protoctx_get(sock, LCBIO_PROTOCTX_POOL);
        return static_cast<mgr_CINFO*>(ctx);
    }

    mgr_HOST *parent;
    lcbio_SOCKET *sock;
    lcbio_pCONNSTART cs;
    lcb::io::Timer<mgr_CINFO, &mgr_CINFO::on_idle_timeout> idle_timer;
    int state;
};

struct ReqNode : lcb_list_t {};
struct lcbio_MGRREQ : ReqNode {
    static lcbio_MGRREQ *from_llnode(lcb_list_t *node) {
        return static_cast<lcbio_MGRREQ*>(static_cast<ReqNode*>(node));
    }

    lcbio_CONNDONE_cb callback;
    void *arg;
    mgr_HOST *host;
    lcbio_pTIMER timer;
    int state;
    lcbio_SOCKET *sock;
    lcb_error_t err;
};

typedef lcbio_MGRREQ mgr_REQ;

#define HE_NPEND(he) LCB_CLIST_SIZE(&(he)->ll_pending)
#define HE_NIDLE(he) LCB_CLIST_SIZE(&(he)->ll_idle)
#define HE_NREQS(he) LCB_CLIST_SIZE(&(he)->requests)
#define HE_NLEASED(he) ((he)->n_total - (HE_NIDLE(he) + HE_NPEND(he)))

static void he_available_notify(void *cookie);
static void he_dump(mgr_HOST *he, FILE *out);
static void he_unref(mgr_HOST *he);
static void mgr_unref(lcbio_MGR *mgr);

#define he_ref(he) (he)->refcount++
#define mgr_ref(mgr) (mgr)->refcount++

static const char *get_hehost(mgr_HOST *h) {
    if (!h) { return "NOHOST:NOPORT"; }
    return h->key;
}

/** Format string arguments for %p%s:%s */
#define HE_LOGID(h) get_hehost(h), (void*)h
#define HE_LOGFMT "<%s> (HE=%p) "

mgr_CINFO::~mgr_CINFO() {
    parent->n_total--;
    if (state == CS_IDLE) {
        lcb_clist_delete(&parent->ll_idle, this);

    } else if (state == CS_PENDING && cs) {
        lcbio_connect_cancel(cs);
    }

    if (sock) {
        // Ensure destructor is not called!
        dtor = NULL;
        lcbio_protoctx_delptr(sock, this, 0);
        lcbio_unref(sock);
    }
    he_unref(parent);
}

static void
cinfo_protoctx_dtor(lcbio_PROTOCTX *ctx)
{
    mgr_CINFO *info = reinterpret_cast<mgr_CINFO*>(ctx);
    info->sock = NULL;
    delete info;
}

lcbio_MGR *
lcbio_mgr_create(lcb_settings *settings, lcbio_TABLE *io)
{
    lcbio_MGR *pool = reinterpret_cast<lcbio_MGR*>(calloc(1, sizeof(*pool)));
    if (!pool) {
        return NULL;
    }

    if ((pool->ht = lcb_hashtable_nc_new(32)) == NULL) {
        free(pool);
        return NULL;
    }

    pool->settings = settings;
    pool->io = io;
    mgr_ref(pool);
    return pool;
}

static void
iterfunc(const void *, lcb_size_t, const void *v, lcb_size_t, void *arg)
{
    lcb_clist_t *he_list = (lcb_clist_t *)arg;
    mgr_HOST *he = (mgr_HOST *)v;
    lcb_list_t *cur, *next;

    LCB_LIST_SAFE_FOR(cur, next, (lcb_list_t *)&he->ll_idle) {
        delete mgr_CINFO::from_llnode(cur);
    }

    LCB_LIST_SAFE_FOR(cur, next, (lcb_list_t *)&he->ll_pending) {
        delete mgr_CINFO::from_llnode(cur);
    }

    memset(&he->ll_idle, 0, sizeof(he->ll_idle));
    lcb_clist_append(he_list, (lcb_list_t *)&he->ll_idle);
}

static void
he_unref(mgr_HOST *host)
{
    if (--host->refcount) {
        return;
    }

    mgr_unref(host->parent);
    free(host);
}

static void
mgr_unref(lcbio_MGR *mgr)
{
    if (--mgr->refcount) {
        return;
    }
    genhash_free(mgr->ht);
    free(mgr);
}

void
lcbio_mgr_destroy(lcbio_MGR *mgr)
{
    lcb_clist_t hes;
    lcb_list_t *cur, *next;
    lcb_clist_init(&hes);

    genhash_iter(mgr->ht, iterfunc, &hes);

    LCB_LIST_SAFE_FOR(cur, next, (lcb_list_t*)&hes) {
        mgr_HOST *he = LCB_LIST_ITEM(cur, mgr_HOST, ll_idle);
        genhash_delete(mgr->ht, he->key, strlen(he->key));
        lcb_clist_delete(&hes, (lcb_list_t *)&he->ll_idle);
        lcbio_timer_destroy(he->async);
        he->async = NULL;
        he_unref(he);
    }
    mgr_unref(mgr);
}

static void
invoke_request(mgr_REQ *req)
{
    if (req->sock) {
        mgr_CINFO *info = mgr_CINFO::from_sock(req->sock);
        lcb_assert(info->state == CS_IDLE);
        info->state = CS_LEASED;
        req->state = RS_ASSIGNED;
        info->idle_timer.cancel();
        lcb_log(LOGARGS(info->parent->parent, DEBUG), HE_LOGFMT "Assigning R=%p SOCKET=%p",HE_LOGID(info->parent), (void*)req, (void*)req->sock);
    }

    if (req->timer) {
        lcbio_timer_destroy(req->timer);
        req->timer = NULL;
    }

    req->callback(req->sock, req->arg, req->err, 0);
    if (req->sock) {
        lcbio_unref(req->sock);
    }
    free(req);
}

/**
 * Called to notify that a connection has become available.
 */
static void
connection_available(mgr_HOST *he)
{
    while (LCB_CLIST_SIZE(&he->requests) && LCB_CLIST_SIZE(&he->ll_idle)) {
        lcb_list_t *reqitem = lcb_clist_shift(&he->requests);
        lcb_list_t *connitem = lcb_clist_pop(&he->ll_idle);

        mgr_REQ* req = mgr_REQ::from_llnode(reqitem);
        mgr_CINFO* info = mgr_CINFO::from_llnode(connitem);
        req->sock = info->sock;
        req->err = LCB_SUCCESS;
        invoke_request(req);
    }
}

/**
 * Connection callback invoked from lcbio_connect() when a result is received
 */
static void
on_connected(lcbio_SOCKET *sock, void *arg, lcb_error_t err, lcbio_OSERR)
{
    reinterpret_cast<mgr_CINFO*>(arg)->on_connected(sock, err);
}


void mgr_CINFO::on_connected(lcbio_SOCKET *sock_, lcb_error_t err) {
    lcb_assert(state == CS_PENDING);
    cs = NULL;

    lcb_log(LOGARGS(parent->parent, DEBUG), HE_LOGFMT "Received result for I=%p,C=%p; E=0x%x", HE_LOGID(parent), (void*)this, (void*)sock, err);
    lcb_clist_delete(&parent->ll_pending, this);

    if (err != LCB_SUCCESS) {
        /** If the connection failed, fail out all remaining requests */
        lcb_list_t *cur, *next;
        LCB_LIST_SAFE_FOR(cur, next, (lcb_list_t *)&parent->requests) {
            mgr_REQ *req = mgr_REQ::from_llnode(cur);
            lcb_clist_delete(&parent->requests, req);
            req->sock = NULL;
            req->err = err;
            invoke_request(req);
        }
        delete this;

    } else {
        state = CS_IDLE;
        sock = sock_;
        lcbio_ref(sock);
        lcbio_protoctx_add(sock, this);

        lcb_clist_append(&parent->ll_idle, this);
        idle_timer.rearm(parent->parent->tmoidle);
        connection_available(parent);
    }
}

mgr_CINFO::mgr_CINFO(mgr_HOST *he, uint32_t timeout)
    : parent(he), sock(NULL), cs(NULL), idle_timer(he->parent->io, this),
      state(CS_PENDING) {

    // protoctx fields
    id = LCBIO_PROTOCTX_POOL;
    dtor = cinfo_protoctx_dtor;

    lcb_host_t tmphost;
    lcb_error_t err = lcb_host_parsez(&tmphost, he->key, 80);
    if (err != LCB_SUCCESS) {
        lcb_log(LOGARGS(he->parent, ERROR), HE_LOGFMT "Could not parse host! Will supply dummy host (I=%p)", HE_LOGID(he), (void*)this);
        strcpy(tmphost.host, "BADHOST");
        strcpy(tmphost.port, "BADPORT");
    }
    lcb_log(LOGARGS(he->parent, TRACE), HE_LOGFMT "New pool entry: I=%p", HE_LOGID(he), (void*)this);

    cs = lcbio_connect(he->parent->io, he->parent->settings, &tmphost,
                       timeout, ::on_connected, this);
}

static void
start_new_connection(mgr_HOST *he, uint32_t tmo)
{
    mgr_CINFO *info = new mgr_CINFO(he, tmo);
    lcb_clist_append(&he->ll_pending, info);
    he->n_total++;
    he_ref(he);
}

static void
on_request_timeout(void *cookie)
{
    mgr_REQ *req = reinterpret_cast<mgr_REQ*>(cookie);
    lcb_clist_delete(&req->host->requests, req);
    req->err = LCB_ETIMEDOUT;
    invoke_request(req);
}

static void
async_invoke_request(void *cookie)
{
    mgr_REQ *req = reinterpret_cast<mgr_REQ*>(cookie);
    mgr_CINFO *cinfo = mgr_CINFO::from_sock(req->sock);
    cinfo->state = CS_IDLE;
    invoke_request(req);
}

mgr_REQ *
lcbio_mgr_get(lcbio_MGR *pool, lcb_host_t *dest, uint32_t timeout,
              lcbio_CONNDONE_cb handler, void *arg)
{
    mgr_HOST *he;
    lcb_list_t *cur;
    mgr_REQ *req = reinterpret_cast<mgr_REQ*>(calloc(1, sizeof(*req)));
    mgr_KEY key = { 0 };

    sprintf(key, "%s:%s", dest->host, dest->port);

    req->callback = handler;
    req->arg = arg;

    he = reinterpret_cast<mgr_HOST*>(genhash_find(pool->ht, key, strlen(key)));
    if (!he) {
        he = reinterpret_cast<mgr_HOST*>(calloc(1, sizeof(*he)));
        he->parent = pool;
        he->async = lcbio_timer_new(pool->io, he, he_available_notify);
        strcpy(he->key, key);

        lcb_clist_init(&he->ll_idle);
        lcb_clist_init(&he->ll_pending);
        lcb_clist_init(&he->requests);

        /** Not copied */
        genhash_store(pool->ht, he->key, strlen(he->key), he, 0);
        he_ref(he);
        mgr_ref(pool);
    }

    req->host = he;

    GT_POPAGAIN:

    cur = lcb_clist_pop(&he->ll_idle);
    if (cur) {
        int clstatus;
        mgr_CINFO *info = mgr_CINFO::from_llnode(cur);

        clstatus = lcbio_is_netclosed(info->sock, LCB_IO_SOCKCHECK_PEND_IS_ERROR);

        if (clstatus == LCB_IO_SOCKCHECK_STATUS_CLOSED) {
            lcb_log(LOGARGS(pool, WARN), HE_LOGFMT "Pooled socket is dead. Continuing to next one", HE_LOGID(he));

            /* Set to CS_LEASED, since it's not inside any of our lists */
            info->state = CS_LEASED;
            delete info;
            goto GT_POPAGAIN;
        }

        info->idle_timer.cancel();
        req->sock = info->sock;
        req->state = RS_ASSIGNED;
        req->timer = lcbio_timer_new(pool->io, req, async_invoke_request);
        info->state = CS_LEASED;
        lcbio_async_signal(req->timer);
        lcb_log(LOGARGS(pool, INFO), HE_LOGFMT "Found ready connection in pool. Reusing socket and not creating new connection", HE_LOGID(he));

    } else {
        req->state = RS_PENDING;
        req->timer = lcbio_timer_new(pool->io, req, on_request_timeout);
        lcbio_timer_rearm(req->timer, timeout);

        lcb_clist_append(&he->requests, req);
        if (HE_NPEND(he) < HE_NREQS(he)) {
            lcb_log(LOGARGS(pool, DEBUG), HE_LOGFMT "Creating new connection because none are available in the pool", HE_LOGID(he));
            start_new_connection(he, timeout);

        } else {
            lcb_log(LOGARGS(pool, DEBUG), HE_LOGFMT "Not creating a new connection. There are still pending ones", HE_LOGID(he));
        }
    }

    return req;
}

/**
 * Invoked when a new socket is available for allocation within the
 * request queue.
 */
static void
he_available_notify(void *cookie)
{
    connection_available((mgr_HOST *)cookie);
}

void
lcbio_mgr_cancel(mgr_REQ *req)
{
    mgr_HOST *he = req->host;
    lcbio_MGR *mgr = he->parent;
    if (req->timer) {
        lcbio_timer_destroy(req->timer);
        req->timer = NULL;
    }

    if (req->sock) {
        lcb_log(LOGARGS(mgr, DEBUG), HE_LOGFMT "Cancelling request=%p with existing connection", HE_LOGID(he), (void*)req);
        lcbio_mgr_put(req->sock);
        lcbio_async_signal(he->async);

    } else {
        lcb_log(LOGARGS(mgr, DEBUG), HE_LOGFMT "Request=%p has no connection.. yet", HE_LOGID(he), (void*)req);
        lcb_clist_delete(&he->requests, req);
    }
    free(req);
}

void mgr_CINFO::on_idle_timeout() {
    lcb_log(LOGARGS(parent->parent, DEBUG), HE_LOGFMT "Idle connection expired", HE_LOGID(parent));
    lcbio_unref(sock);
}

void
lcbio_mgr_put(lcbio_SOCKET *sock)
{
    mgr_HOST *he;
    lcbio_MGR *mgr;
    mgr_CINFO *info = mgr_CINFO::from_sock(sock);

    if (!info) {
        fprintf(stderr, "Requested put() for non-pooled (or detached) socket=%p\n", (void *)sock);
        lcbio_unref(sock);
        return;
    }

    he = info->parent;
    mgr = he->parent;

    if (HE_NIDLE(he) >= mgr->maxidle) {
        lcb_log(LOGARGS(mgr, INFO), HE_LOGFMT "Closing idle connection. Too many in quota", HE_LOGID(he));
        lcbio_unref(info->sock);
        return;
    }

    lcb_log(LOGARGS(mgr, INFO), HE_LOGFMT "Placing socket back into the pool. I=%p,C=%p", HE_LOGID(he), (void*)info, (void*)sock);
    info->idle_timer.rearm(mgr->tmoidle);
    lcb_clist_append(&he->ll_idle, info);
    info->state = CS_IDLE;
}

void
lcbio_mgr_discard(lcbio_SOCKET *sock)
{
    lcbio_unref(sock);
}

void
lcbio_mgr_detach(lcbio_SOCKET *sock)
{
    lcbio_protoctx_delid(sock, LCBIO_PROTOCTX_POOL, 1);
}


#define CONN_INDENT "    "

static void
write_he_list(lcb_clist_t *ll, FILE *out)
{
    lcb_list_t *llcur;
    LCB_LIST_FOR(llcur, (lcb_list_t *)ll) {
        mgr_CINFO *info = mgr_CINFO::from_llnode(llcur);
        fprintf(out, "%sCONN [I=%p,C=%p ",
                CONN_INDENT, (void *)info, (void *)&info->sock);

        if (info->sock->io->model == LCB_IOMODEL_EVENT) {
            fprintf(out, "SOCKFD=%d", (int)info->sock->u.fd);
        } else {
            fprintf(out, "SOCKDATA=%p", (void *)info->sock->u.sd);
        }
        fprintf(out, " STATE=0x%x", info->state);
        fprintf(out, "]\n");
    }

}

static void
he_dump(mgr_HOST *he, FILE *out)
{
    lcb_list_t *llcur;
    fprintf(out, "HOST=%s", he->key);
    fprintf(out, "Requests=%d, Idle=%d, Pending=%d, Leased=%d\n",
            (int)HE_NREQS(he), (int)HE_NIDLE(he), (int)HE_NPEND(he), (int)HE_NLEASED(he));

    fprintf(out, CONN_INDENT "Idle Connections:\n");
    write_he_list(&he->ll_idle, out);
    fprintf(out, CONN_INDENT "Pending Connections: \n");
    write_he_list(&he->ll_pending, out);
    fprintf(out, CONN_INDENT "Pending Requests:\n");

    LCB_LIST_FOR(llcur, (lcb_list_t *)&he->requests) {
        mgr_REQ *req = mgr_REQ::from_llnode(llcur);
        union {
            lcbio_CONNDONE_cb cb;
            void *ptr;
        } u_cb;

        u_cb.cb = req->callback;

        fprintf(out, "%sREQ [R=%p, Callback=%p, Data=%p, State=0x%x]\n",
                CONN_INDENT, (void *)req, u_cb.ptr, (void *)req->arg,
                req->state);
    }

    fprintf(out, "\n");

}

static void
dumpfunc(const void *, lcb_size_t, const void *v, lcb_size_t, void *arg)
{
    FILE *out = (FILE *)arg;
    mgr_HOST *he = reinterpret_cast<mgr_HOST*>(const_cast<void*>(v));
    he_dump(he, out);
}

/**
 * Dumps the connection manager state to stderr
 */
LCB_INTERNAL_API
void lcbio_mgr_dump(lcbio_MGR *mgr, FILE *out)
{
    if (out == NULL) {
        out = stderr;
    }

    genhash_iter(mgr->ht, dumpfunc, out);
}
