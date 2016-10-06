Name
====

lua-resty-treadpool - access Ngnix thread pools from Lua

Table of Contents
=================

**TODO**

Status
======

This module is a *proof of concept*, the API is not fixed and implementation is
licely to contain bugs.

**Do not use in production!**


Synopsis
========

```lua

```

Description
===========

Since version 1.7.11, Nginx provides a [`thread_pool`][tp_directive] directive
that makes possible to offload blocking operations out of the regular
non-blocking I/O loop. In some situations this can lead to [massive speedups][blog].

This modules expose a Lua API to leverages these thread pools from the
[`lua-nginx-module`][lnm]. Using thread offloading from Lua makes possible to:

* Use blocking I/O libraries (database access, message queue, ...)
* Use blocking file I/O
* Offload CPU intensive tasks

Keep in mind however that it is not a silver bullet for every problem, and
threads have their own limitation and also some overhead.

Dev notes
=========

As stated previously, this module is currently a PoC, and a lot of issues still
need to be addressed. This section list some of open questions about design and
implementation.

Integration with `lua-nginx-module`
-----------------------------------

This module is deeply integrated into the regular Lua module internals, much
deeply than the current public API allows.

Also, the lua states are currently raw states, and do *not* have access to any
`ngx.*` APIs (lot even logging). Ideally, at least some APIs should be
available from worker threads (`log`, `re`, `shared` caches and other utility
functions), but most of other APIs don't make sense in this context (`socket`
for instance, as threads don't have event loops).

I'm not sure yet if such integration is possible without embedding this module
directly into `lua-nginx-module`.

Coroutine handling
------------------

Currently each task creates its own `lua_State` so when the code yields and is
resumed after, it can be scheduled on any thread of the pool. This is terribly
inefficient.

There is several ways to solve that:

* Make one state per thread and lock the thread while the task is paused, but
  this limits the thoughput of the thread pools
* Make a state pool and allocate tasks using it before scheduling them on any
  thread. This may cause a memory explosion as it can create `len(queue) +
  thread count` states in worst case

[tp_directive]: http://nginx.org/en/docs/ngx_core_module.html#thread_pool
[blog]: https://www.nginx.com/blog/thread-pools-boost-performance-9x/
[lnm]: https://github.com/openresty/lua-nginx-module
