
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>


static nxt_int_t nxt_event_engine_post_init(nxt_event_engine_t *engine);
static nxt_int_t nxt_event_engine_signal_pipe_create(
    nxt_event_engine_t *engine);
static void nxt_event_engine_signal_pipe_close(nxt_task_t *task, void *obj,
    void *data);
static void nxt_event_engine_signal_pipe(nxt_task_t *task, void *obj,
    void *data);
static void nxt_event_engine_post_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_event_engine_signal_pipe_error(nxt_task_t *task, void *obj,
    void *data);
static void nxt_event_engine_signal_handler(nxt_task_t *task, void *obj,
    void *data);
static nxt_work_handler_t nxt_event_engine_queue_pop(nxt_event_engine_t *engine,
    nxt_task_t **task, void **obj, void **data);


nxt_event_engine_t *
nxt_event_engine_create(nxt_task_t *task,
    const nxt_event_interface_t *interface, const nxt_sig_event_t *signals,
    nxt_uint_t flags, nxt_uint_t batch)
{
    nxt_uint_t          events;
    nxt_thread_t        *thread;
    nxt_event_engine_t  *engine;

    engine = nxt_zalloc(sizeof(nxt_event_engine_t));
    if (engine == NULL) {
        return NULL;
    }

    nxt_debug(task, "create engine %p", engine);

    thread = task->thread;

    engine->task.thread = thread;
    engine->task.log = thread->log;
    engine->task.ident = nxt_task_next_ident();

    engine->batch = batch;

    if (flags & NXT_ENGINE_FIBERS) {
        engine->fibers = nxt_fiber_main_create(engine);
        if (engine->fibers == NULL) {
            goto fibers_fail;
        }
    }

    engine->current_work_queue = &engine->fast_work_queue;

    nxt_work_queue_cache_create(&engine->work_queue_cache, 0);

    engine->fast_work_queue.cache = &engine->work_queue_cache;
    engine->accept_work_queue.cache = &engine->work_queue_cache;
    engine->read_work_queue.cache = &engine->work_queue_cache;
    engine->socket_work_queue.cache = &engine->work_queue_cache;
    engine->connect_work_queue.cache = &engine->work_queue_cache;
    engine->write_work_queue.cache = &engine->work_queue_cache;
    engine->shutdown_work_queue.cache = &engine->work_queue_cache;
    engine->close_work_queue.cache = &engine->work_queue_cache;

    nxt_work_queue_name(&engine->fast_work_queue, "fast");
    nxt_work_queue_name(&engine->accept_work_queue, "accept");
    nxt_work_queue_name(&engine->read_work_queue, "read");
    nxt_work_queue_name(&engine->socket_work_queue, "socket");
    nxt_work_queue_name(&engine->connect_work_queue, "connect");
    nxt_work_queue_name(&engine->write_work_queue, "write");
    nxt_work_queue_name(&engine->shutdown_work_queue, "shutdown");
    nxt_work_queue_name(&engine->close_work_queue, "close");

    if (signals != NULL) {
        engine->signals = nxt_event_engine_signals(signals);
        if (engine->signals == NULL) {
            goto signals_fail;
        }

        engine->signals->handler = nxt_event_engine_signal_handler;

        if (!interface->signal_support) {
            if (nxt_event_engine_signals_start(engine) != NXT_OK) {
                goto signals_fail;
            }
        }
    }

    /*
     * Number of event set and timers changes should be at least twice
     * more than number of events to avoid premature flushes of the changes.
     * Fourfold is for sure.
     */
    events = (batch != 0) ? batch : 32;

    if (interface->create(engine, 4 * events, events) != NXT_OK) {
        goto event_set_fail;
    }

    engine->event = *interface;

    if (nxt_event_engine_post_init(engine) != NXT_OK) {
        goto post_fail;
    }

    if (nxt_timers_init(&engine->timers, 4 * events) != NXT_OK) {
        goto timers_fail;
    }

    thread = task->thread;

    nxt_thread_time_update(thread);
    engine->timers.now = nxt_thread_monotonic_time(thread) / 1000000;

    engine->max_connections = 0xffffffff;

    nxt_queue_init(&engine->joints);
    nxt_queue_init(&engine->listen_connections);
    nxt_queue_init(&engine->idle_connections);

    return engine;

timers_fail:
post_fail:

    interface->free(engine);

event_set_fail:
signals_fail:

    nxt_free(engine->signals);
    nxt_work_queue_cache_destroy(&engine->work_queue_cache);
    nxt_free(engine->fibers);

fibers_fail:

    nxt_free(engine);
    return NULL;
}


static nxt_int_t
nxt_event_engine_post_init(nxt_event_engine_t *engine)
{
    if (engine->event.enable_post != NULL) {
        return engine->event.enable_post(engine, nxt_event_engine_post_handler);
    }

    if (nxt_event_engine_signal_pipe_create(engine) != NXT_OK) {
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_event_engine_signal_pipe_create(nxt_event_engine_t *engine)
{
    nxt_event_engine_pipe_t  *pipe;

    pipe = nxt_zalloc(sizeof(nxt_event_engine_pipe_t));
    if (pipe == NULL) {
        return NXT_ERROR;
    }

    engine->pipe = pipe;

    /*
     * An event engine pipe is in blocking mode for writer
     * and in non-blocking node for reader.
     */

    if (nxt_pipe_create(&engine->task, pipe->fds, 1, 0) != NXT_OK) {
        nxt_free(pipe);
        return NXT_ERROR;
    }

    pipe->event.fd = pipe->fds[0];
    pipe->event.task = &engine->task;
    pipe->event.read_work_queue = &engine->fast_work_queue;
    pipe->event.read_handler = nxt_event_engine_signal_pipe;
    pipe->event.write_work_queue = &engine->fast_work_queue;
    pipe->event.error_handler = nxt_event_engine_signal_pipe_error;
    pipe->event.log = engine->task.log;

    nxt_fd_event_enable_read(engine, &pipe->event);

    return NXT_OK;
}


static void
nxt_event_engine_signal_pipe_free(nxt_event_engine_t *engine)
{
    nxt_event_engine_pipe_t  *pipe;

    pipe = engine->pipe;

    if (pipe != NULL) {

        if (pipe->event.read_work_queue != NULL) {
            nxt_fd_event_close(engine, &pipe->event);
            nxt_pipe_close(pipe->event.task, pipe->fds);
        }

        nxt_free(pipe);
    }
}


static void
nxt_event_engine_signal_pipe_close(nxt_task_t *task, void *obj, void *data)
{
    nxt_event_engine_pipe_t  *pipe;

    pipe = obj;

    nxt_pipe_close(pipe->event.task, pipe->fds);
    nxt_free(pipe);
}


void
nxt_event_engine_post(nxt_event_engine_t *engine, nxt_work_t *work)
{
    nxt_debug(&engine->task, "event engine post");

    nxt_locked_work_queue_add(&engine->locked_work_queue, work);

    nxt_event_engine_signal(engine, 0);
}


void
nxt_event_engine_signal(nxt_event_engine_t *engine, nxt_uint_t signo)
{
    u_char  buf;

    nxt_debug(&engine->task, "event engine signal:%ui", signo);

    /*
     * A signal number may be sent in a signal context, so the signal
     * information cannot be passed via a locked work queue.
     */

    if (engine->event.signal != NULL) {
        engine->event.signal(engine, signo);
        return;
    }

    buf = (u_char) signo;
    (void) nxt_fd_write(engine->pipe->fds[1], &buf, 1);
}


static void
nxt_event_engine_signal_pipe(nxt_task_t *task, void *obj, void *data)
{
    int             i, n;
    u_char          signo;
    nxt_bool_t      post;
    nxt_fd_event_t  *ev;
    u_char          buf[128];

    ev = obj;

    nxt_debug(task, "engine signal pipe");

    post = 0;

    do {
        n = nxt_fd_read(ev->fd, buf, sizeof(buf));

        for (i = 0; i < n; i++) {
            signo = buf[i];

            nxt_debug(task, "engine pipe signo:%d", signo);

            if (signo == 0) {
                /* A post should be processed only once. */
                post = 1;

            } else {
                nxt_event_engine_signal_handler(task,
                                             (void *) (uintptr_t) signo, NULL);
            }
        }

    } while (n == sizeof(buf));

    if (post) {
        nxt_event_engine_post_handler(task, NULL, NULL);
    }
}


static void
nxt_event_engine_post_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_thread_t        *thread;
    nxt_event_engine_t  *engine;

    thread = task->thread;
    engine = thread->engine;

    nxt_locked_work_queue_move(thread, &engine->locked_work_queue,
                               &engine->fast_work_queue);
}


static void
nxt_event_engine_signal_pipe_error(nxt_task_t *task, void *obj, void *data)
{
    nxt_event_engine_t       *engine;
    nxt_event_engine_pipe_t  *pipe;

    engine = task->thread->engine;
    pipe = engine->pipe;

    nxt_log(task, NXT_LOG_CRIT, "engine pipe(%FD:%FD) event error",
            pipe->fds[0], pipe->fds[1]);

    nxt_fd_event_close(engine, &pipe->event);
    nxt_pipe_close(pipe->event.task, pipe->fds);
}


static void
nxt_event_engine_signal_handler(nxt_task_t *task, void *obj, void *data)
{
    uintptr_t              signo;
    const nxt_sig_event_t  *sigev;

    signo = (uintptr_t) obj;

    for (sigev = task->thread->engine->signals->sigev;
         sigev->signo != 0;
         sigev++)
    {
        if (signo == (nxt_uint_t) sigev->signo) {
            sigev->handler(task, (void *) signo, (void *) sigev->name);
            return;
        }
    }

    nxt_log(task, NXT_LOG_CRIT, "signal %ui handler not found", signo);
}


nxt_int_t
nxt_event_engine_change(nxt_event_engine_t *engine,
    const nxt_event_interface_t *interface, nxt_uint_t batch)
{
    nxt_uint_t  events;

    engine->batch = batch;

    if (!engine->event.signal_support && interface->signal_support) {
        /*
         * Block signal processing if the current event
         * facility does not support signal processing.
         */
        nxt_event_engine_signals_stop(engine);

        /*
         * Add to engine fast work queue the signal events possibly
         * received before the blocking signal processing.
         */
        nxt_event_engine_signal_pipe(&engine->task, &engine->pipe->event, NULL);
    }

    if (engine->pipe != NULL && interface->enable_post != NULL) {
        /*
         * An engine pipe must be closed after all signal events
         * added above to engine fast work queue will be processed.
         */
        nxt_work_queue_add(&engine->fast_work_queue,
                           nxt_event_engine_signal_pipe_close,
                           &engine->task, engine->pipe, NULL);

        engine->pipe = NULL;
    }

    engine->event.free(engine);

    events = (batch != 0) ? batch : 32;

    if (interface->create(engine, 4 * events, events) != NXT_OK) {
        return NXT_ERROR;
    }

    engine->event = *interface;

    if (nxt_event_engine_post_init(engine) != NXT_OK) {
        return NXT_ERROR;
    }

    if (engine->signals != NULL) {

        if (!engine->event.signal_support) {
            return nxt_event_engine_signals_start(engine);
        }

        /*
         * Reset the PID flag to start the signal thread if
         * some future event facility will not support signals.
         */
        engine->signals->process = 0;
    }

    return NXT_OK;
}


void
nxt_event_engine_free(nxt_event_engine_t *engine)
{
    nxt_thread_log_debug("free engine %p", engine);

    nxt_event_engine_signal_pipe_free(engine);
    nxt_free(engine->signals);

    nxt_work_queue_cache_destroy(&engine->work_queue_cache);

    engine->event.free(engine);

    /* TODO: free timers */

    nxt_free(engine);
}


static nxt_work_handler_t
nxt_event_engine_queue_pop(nxt_event_engine_t *engine, nxt_task_t **task,
    void **obj, void **data)
{
    nxt_work_queue_t  *wq, *last;

    wq = engine->current_work_queue;
    last = wq;

    if (wq->head == NULL) {
        wq = &engine->fast_work_queue;

        if (wq->head == NULL) {

            do {
                engine->current_work_queue++;
                wq = engine->current_work_queue;

                if (wq > &engine->close_work_queue) {
                    wq = &engine->fast_work_queue;
                    engine->current_work_queue = wq;
                }

                if (wq->head != NULL) {
                    goto found;
                }

            } while (wq != last);

            engine->current_work_queue = &engine->fast_work_queue;

            return NULL;
        }
    }

found:

    nxt_debug(&engine->task, "work queue: %s", wq->name);

    return nxt_work_queue_pop(wq, task, obj, data);
}


void
nxt_event_engine_start(nxt_event_engine_t *engine)
{
    void                *obj, *data;
    nxt_task_t          *task;
    nxt_msec_t          timeout, now;
    nxt_thread_t        *thr;
    nxt_work_handler_t  handler;

    thr = nxt_thread();

    if (engine->fibers) {
        /*
         * _setjmp() cannot be wrapped in a function since return from
         * the function clobbers stack used by future _setjmp() returns.
         */
        _setjmp(engine->fibers->fiber.jmp);

        /* A return point from fibers. */
    }

    thr->log = engine->task.log;

    for ( ;; ) {

        for ( ;; ) {
            handler = nxt_event_engine_queue_pop(engine, &task, &obj, &data);

            if (handler == NULL) {
                break;
            }

            thr->task = task;

            handler(task, obj, data);
        }

        /* Attach some event engine work queues in preferred order. */

        timeout = nxt_timer_find(engine);

        engine->event.poll(engine, timeout);

        now = nxt_thread_monotonic_time(thr) / 1000000;

        nxt_timer_expire(engine, now);
    }
}


static nxt_int_t
nxt_req_conn_test(nxt_lvlhsh_query_t *lhq, void *data)
{
    return NXT_OK;
}

static const nxt_lvlhsh_proto_t  lvlhsh_req_conn_proto  nxt_aligned(64) = {
    NXT_LVLHSH_DEFAULT,
    nxt_req_conn_test,
    nxt_lvlhsh_alloc,
    nxt_lvlhsh_free,
};


void
nxt_event_engine_request_add(nxt_event_engine_t *engine,
    nxt_req_conn_link_t *rc)
{
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = nxt_murmur_hash2(&rc->req_id, sizeof(rc->req_id));
    lhq.key.length = sizeof(rc->req_id);
    lhq.key.start = (u_char *) &rc->req_id;
    lhq.proto = &lvlhsh_req_conn_proto;
    lhq.replace = 0;
    lhq.value = rc;
    lhq.pool = engine->mem_pool;

    switch (nxt_lvlhsh_insert(&engine->requests, &lhq)) {

    case NXT_OK:
        break;

    default:
        nxt_thread_log_error(NXT_LOG_WARN, "req %08uxD to conn add failed",
                             rc->req_id);
        break;
    }
}


nxt_req_conn_link_t *
nxt_event_engine_request_find(nxt_event_engine_t *engine, nxt_req_id_t req_id)
{
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = nxt_murmur_hash2(&req_id, sizeof(req_id));
    lhq.key.length = sizeof(req_id);
    lhq.key.start = (u_char *) &req_id;
    lhq.proto = &lvlhsh_req_conn_proto;

    if (nxt_lvlhsh_find(&engine->requests, &lhq) == NXT_OK) {
        return lhq.value;
    }

    return NULL;
}


void
nxt_event_engine_request_remove(nxt_event_engine_t *engine,
    nxt_req_conn_link_t *rc)
{
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = nxt_murmur_hash2(&rc->req_id, sizeof(rc->req_id));
    lhq.key.length = sizeof(rc->req_id);
    lhq.key.start = (u_char *) &rc->req_id;
    lhq.proto = &lvlhsh_req_conn_proto;
    lhq.pool = engine->mem_pool;

    switch (nxt_lvlhsh_delete(&engine->requests, &lhq)) {

    case NXT_OK:
        break;

    default:
        nxt_thread_log_error(NXT_LOG_WARN, "req %08uxD to conn remove failed",
                             rc->req_id);
        break;
    }
}


nxt_req_conn_link_t *
nxt_event_engine_request_find_remove(nxt_event_engine_t *engine,
    nxt_req_id_t req_id)
{
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = nxt_murmur_hash2(&req_id, sizeof(req_id));
    lhq.key.length = sizeof(req_id);
    lhq.key.start = (u_char *) &req_id;
    lhq.proto = &lvlhsh_req_conn_proto;
    lhq.pool = engine->mem_pool;

    switch (nxt_lvlhsh_delete(&engine->requests, &lhq)) {

    case NXT_OK:
        return lhq.value;

    default:
        nxt_thread_log_error(NXT_LOG_WARN, "req %08uxD to conn remove failed",
                             req_id);
        break;
    }

    return NULL;
}


#if (NXT_DEBUG)

void nxt_event_engine_thread_adopt(nxt_event_engine_t *engine)
{
    nxt_work_queue_thread_adopt(&engine->fast_work_queue);
    nxt_work_queue_thread_adopt(&engine->accept_work_queue);
    nxt_work_queue_thread_adopt(&engine->read_work_queue);
    nxt_work_queue_thread_adopt(&engine->socket_work_queue);
    nxt_work_queue_thread_adopt(&engine->connect_work_queue);
    nxt_work_queue_thread_adopt(&engine->write_work_queue);
    nxt_work_queue_thread_adopt(&engine->shutdown_work_queue);
    nxt_work_queue_thread_adopt(&engine->close_work_queue);
}

#endif
