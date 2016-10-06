/*
Copyright (c) 2016, Julien Desgats
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <api/ngx_http_lua_api.h>
/* FIXME: this modules goes far beyond what the lua-nginx-module public API
 * provides and uses the actual headers for now. */
#include <ddebug.h>
#include <ngx_http_lua_common.h>
#include <ngx_http_lua_util.h>

#include "serialize.h"

#ifndef NGX_THREADS
# error thread support required
#endif

typedef enum {
    LUA_THREADPOOL_TASK_CREATED,
    LUA_THREADPOOL_TASK_YIELDED,
    LUA_THREADPOOL_TASK_RUNNING,
    LUA_THREADPOOL_TASK_SUCCESS,
    LUA_THREADPOOL_TASK_FAILED,
    LUA_THREADPOOL_TASK_DESTROYED,
} ngx_http_resty_threadpool_thread_status_t;

typedef struct {
} ngx_http_resty_threadpool_conf_t;

typedef struct {
    ngx_thread_pool_t                        *tp;
    lua_State                                *L;
    ngx_http_resty_threadpool_thread_status_t status;
} ngx_http_resty_threadpool_state_t;

typedef struct {
    ngx_http_lua_co_ctx_t       *coctx;
    ngx_http_request_t *r;
    ngx_int_t nres; /* result count */
    ngx_http_resty_threadpool_state_t *thread;
} ngx_thread_lua_task_ctx_t;

static ngx_int_t
ngx_http_resty_threadpool_resume(ngx_http_request_t *r);

static ngx_int_t
ngx_http_resty_threadpool_inject_api(ngx_conf_t *cf);

static ngx_http_module_t  ngx_http_resty_threadpool_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_resty_threadpool_inject_api,  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};

ngx_module_t  ngx_http_resty_threadpool_module = {
    NGX_MODULE_V1,
    &ngx_http_resty_threadpool_module_ctx, /* module context */
    NULL,                                  /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static void
ngx_http_resty_threadpool_task_handler(void *data, ngx_log_t *log)
{
    /* called from inside the worker thread: responsible to run the actual Lua
     * code in the thread state.
     */
    const char                *code;
    ngx_thread_lua_task_ctx_t *ctx = data;
    lua_State                 *L = ctx->thread->L;
    lua_State                 *co;
    size_t                     codelen;
    ngx_int_t                  i, nres;

    if (ctx->thread->status == LUA_THREADPOOL_TASK_CREATED) {
        /* new task, setup the state (only the serialized code is on the stack) */
        luaL_openlibs(L);

        ngx_http_lua_assert(lua_type(L, 1) == LUA_TSTRING);
        code = lua_tolstring(L, 1, &codelen);
        co = lua_newthread(L);
        luaser_decode(co, code, codelen);
        lua_remove(L, 1); /* the parameters can be GCed now */
        ngx_http_lua_assert(lua_gettop(L) == 1);
    } else {
        /* already created: the running coro is still on the top of the stack */
        ngx_http_lua_assert(ctx->thread->status == LUA_THREADPOOL_TASK_YIELDED);
        ngx_http_lua_assert(lua_gettop(L) == 1);
        co = lua_tothread(L, 1);
        ngx_http_lua_assert(co != NULL && lua_gettop(co) == 0);
    }

    ctx->thread->status = LUA_THREADPOOL_TASK_RUNNING;
    switch (lua_resume(co, 0)) {
    case 0: /* finished */
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0, "lua task completed");
        ctx->thread->status = LUA_THREADPOOL_TASK_SUCCESS;
        break;
    case LUA_YIELD:
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0, "lua task suspended");
        ctx->thread->status = LUA_THREADPOOL_TASK_YIELDED;
        break;
    default: { /* error */
        const char *msg = lua_tostring(co, -1);
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "failed to run lua code in thread: %s", msg);
        goto failed;
    }
    }

    /* serialize returned values */
    /* TODO: bench that, matbe it worth making a special case when the result
     * is a single string (avoid to make too many copies).
     * TODO: the serialization could actually use the main thread instead of creating one
     */
    nres = lua_gettop(co);
    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, log, 0,
                   "lua task returned %d results: \"%V?%V\"",
                   nres, &(ctx->r->uri), &(ctx->r->args));
    for (i = 1; i <= nres; i++) {
        luaser_encode(co, i);
    }
    lua_xmove(co, L, nres);
    lua_pop(co, nres); /* TODO: lua_settop(co, 0); */
    ngx_http_lua_assert(lua_gettop(co) == 0);
    ngx_http_lua_assert(lua_gettop(L) == 1 + nres); /* (thread, res1, ..., resN) */
    ngx_http_lua_assert(lua_type(L, 1) == LUA_TTHREAD);
    ctx->nres = nres;
    return;
failed:
    ctx->nres = 0;
    ctx->thread->status = LUA_THREADPOOL_TASK_FAILED;
}

static void
ngx_http_resty_threadpool_thread_event_handler(ngx_event_t *ev)
{
    /* called in the main event loop after task completion. Responsible of
     * copying task result(s) into the calling coroutine */
    ngx_connection_t            *c;
    ngx_http_request_t          *r;
    ngx_thread_lua_task_ctx_t   *ctx;
    ngx_http_lua_ctx_t          *luactx;
    ngx_http_lua_co_ctx_t       *coctx;
    lua_State                   *L;
    ngx_int_t                    i;

    ctx = ev->data;
    L = ctx->thread->L;
    coctx = ctx->coctx;
    ngx_http_lua_assert(coctx->data == ev->data);

    r = ctx->r;
    c = r->connection;

    luactx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (luactx == NULL) {
        lua_close(ctx->thread->L);
        ctx->thread->L = NULL;
        ctx->thread->status = LUA_THREADPOOL_TASK_DESTROYED;
        return; /* not sure what it means in this case */
    }

    if (c->fd != (ngx_socket_t) -1) {  /* not a fake connection */
        ngx_http_log_ctx_t *log_ctx = c->log->data;
        log_ctx->current_request = r;
    }

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "lua task status: %d with %d results: \"%V?%V\"",
                   ctx->thread->status, ctx->nres, &r->uri, &r->args);

    /* push results into the main coroutine */
    /* prepare_retvals(r, u, ctx->cur_co_ctx->co); */
    for (i=1; i <= ctx->nres; i++) {
        const char *res;
        size_t reslen;
        ngx_http_lua_assert(lua_type(L, 1+i) == LUA_TSTRING);
        res = lua_tolstring(L, 1+i, &reslen);
        luaser_decode(coctx->co, res, reslen);
    }
    lua_pop(L, ctx->nres);
    ngx_http_lua_assert(lua_gettop(L) == 1 && lua_type(L, 1) == LUA_TTHREAD);

    if (ctx->thread->status == LUA_THREADPOOL_TASK_SUCCESS ||
        ctx->thread->status == LUA_THREADPOOL_TASK_FAILED)
    {
        lua_close(ctx->thread->L);
        ctx->thread->L = NULL;
        ctx->thread->status = LUA_THREADPOOL_TASK_DESTROYED;
        coctx->cleanup = NULL;
    }

    luactx->cur_co_ctx = coctx;
    if (luactx->entered_content_phase) {
        (void) ngx_http_resty_threadpool_resume(r);
    } else {
        luactx->resume_handler = ngx_http_resty_threadpool_resume;
        ngx_http_core_run_phases(r);
    }

    ngx_http_run_posted_requests(c);
}

/* copy of ngx_http_lua_sleep_resume */
static ngx_int_t
ngx_http_resty_threadpool_resume(ngx_http_request_t *r)
{
    lua_State                   *vm;
    ngx_connection_t            *c;
    ngx_int_t                    rc;
    ngx_http_lua_ctx_t          *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->resume_handler = ngx_http_lua_wev_handler;

    c = r->connection;
    vm = ngx_http_lua_get_lua_vm(r, ctx);

    /* FIXME: result handling is a mess, clean it up and do everything here */
    rc = ngx_http_lua_run_thread(vm, r, ctx, ((ngx_thread_lua_task_ctx_t *)(ctx->cur_co_ctx->data))->nres);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua run thread returned %d", rc);

    if (rc == NGX_AGAIN) {
        return ngx_http_lua_run_posted_threads(c, vm, r, ctx);
    }

    if (rc == NGX_DONE) {
        ngx_http_lua_finalize_request(r, NGX_DONE);
        return ngx_http_lua_run_posted_threads(c, vm, r, ctx);
    }

    if (ctx->entered_content_phase) {
        ngx_http_lua_finalize_request(r, rc);
        return NGX_DONE;
    }

    return rc;
}

static void
ngx_http_resty_threadpool_task_cleanup(void *data)
{
    ngx_log_debug0(NGX_LOG_CRIT, ngx_cycle->log, 0,
                   "ngx_http_resty_threadpool_task_cleanup: this is not really supposed to happen.");
    /* TODO: free buffer memory for SUCCESS or FAILED states */
}

/***********/
/* Lua API */
/***********/

#define LUA_THREADPOOL_MT_NAME "resty.threadpool"

static int
ngx_http_resty_threadpool_thread_create(lua_State *L) {
    ngx_http_resty_threadpool_state_t *ud;
    const char                        *code;
    ngx_str_t                          pool;
    size_t                             codelen;

    pool.data = (u_char *)luaL_checklstring(L, 1, &pool.len);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    ud = lua_newuserdata(L, sizeof(ngx_http_resty_threadpool_state_t));
    ud->status = LUA_THREADPOOL_TASK_CREATED;
    ud->L = NULL;
    /* L = (poolname, func, thread_ud) */

    luaL_getmetatable(L, LUA_THREADPOOL_MT_NAME);
    lua_setmetatable(L, -2);
    /* L = (poolname, func, thread_ud) */

    /* find the thread pool */
    ud->tp = ngx_thread_pool_get((ngx_cycle_t *) ngx_cycle, &pool);
    if (ud->tp == NULL) {
        return luaL_error(L, "no pool '%s' found", pool.data);
    }

    /* prepare the state: just push the code for now, the actual loading will
     * be done in thread */
    ud->L = luaL_newstate();
    if (ud->L == NULL) {
        return luaL_error(L, "failed to create task state");
    }

    luaser_encode(L, 2); /* L = (poolname, func, thread_ud, serialized) */
    code = lua_tolstring(L, -1, &codelen);
    lua_pushlstring(ud->L, code, codelen);
    lua_pop(L, 1); /* L = (poolname, func, thread_ud) */

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "Lua thread %p created to run on pool %V", ud, &pool);
    return 1;
}

static int
ngx_http_resty_threadpool_thread_resume(lua_State *L) {
    ngx_http_resty_threadpool_state_t *ud;
    ngx_http_request_t                *r;
    ngx_thread_task_t                 *task;
    ngx_thread_lua_task_ctx_t         *ctx;
    ngx_http_lua_ctx_t                *luactx;
    ngx_http_lua_co_ctx_t             *coctx;

    ud = luaL_checkudata(L, 1, LUA_THREADPOOL_MT_NAME);
    if (ud->status != LUA_THREADPOOL_TASK_CREATED &&
        ud->status != LUA_THREADPOOL_TASK_YIELDED) {
        return luaL_error(L, "thread not in good state");
    }

    r = ngx_http_lua_get_req(L);
    if (r == NULL) {
        return luaL_error(L, "no request found");
    }

    luactx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (luactx == NULL) {
        return luaL_error(L, "no request ctx found");
    }

    coctx = luactx->cur_co_ctx;
    if (coctx == NULL) {
        return luaL_error(L, "no co ctx found");
    }

    // create the task
    task = ngx_thread_task_alloc(r->pool, sizeof(ngx_thread_lua_task_ctx_t));
    if (task == NULL) {
        return luaL_error(L, "failed to allocate task");
    }

    task->handler = ngx_http_resty_threadpool_task_handler;
    ctx = task->ctx;
    ctx->thread = ud;
    ctx->coctx = coctx;
    ctx->r = r;

    /* return handler */
    task->event.data = ctx;
    task->event.handler = ngx_http_resty_threadpool_thread_event_handler;

    // push task in queue
    ngx_http_lua_cleanup_pending_operation(coctx);
    coctx->cleanup = ngx_http_resty_threadpool_task_cleanup;
    coctx->data = ctx;


    if (ngx_thread_task_post(ud->tp, task) != NGX_OK) {
        return luaL_error(L, "failed to post task to queue");
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "Lua thread %p scheduled for resume", ud);

    return lua_yield(L, 0);
}

static int
ngx_http_resty_threadpool_thread_close(lua_State *L) {
    ngx_http_resty_threadpool_state_t *ud;
    ud = luaL_checkudata(L, 1, LUA_THREADPOOL_MT_NAME);
    /* TODO: check the the task is not actually running or queued */
    if (ud->L != NULL) {
       lua_close(ud->L);
    }
    ud->L = NULL;
    ud->status = LUA_THREADPOOL_TASK_DESTROYED;
    return 0;
}

static const luaL_Reg LUA_THREADPOOL_MT[] = {
    { "__gc", ngx_http_resty_threadpool_thread_close },
    { NULL, NULL }
};

static const luaL_Reg LUA_THREADPOOL_FUNCTABLE[] = {
    { "create", ngx_http_resty_threadpool_thread_create },
    { "resume", ngx_http_resty_threadpool_thread_resume },
    { NULL, NULL }
};

static int
luaopen_resty_threadpool(lua_State *L) {
    luaL_newmetatable(L, LUA_THREADPOOL_MT_NAME);
    luaL_register(L, NULL, LUA_THREADPOOL_MT);

    lua_newtable(L);
    luaL_register(L, NULL, LUA_THREADPOOL_FUNCTABLE);

    /* set the function table as methods of threads too */
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "__index");
    return 1;
}

static ngx_int_t
ngx_http_resty_threadpool_inject_api(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "resty.threadpool",
                                         luaopen_resty_threadpool) != NGX_OK)
    {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                      "failed to inject resty.threadpool API");
    } else {
        ngx_conf_log_error(NGX_LOG_CRIT, cf, 0,
                      "resty.threadpool module injected");
    }

    return NGX_OK; /* do not stop the process loading for that */
}
