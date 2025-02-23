/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <apr_atomic.h>
#include <apr_thread_mutex.h>
#include <apr_thread_cond.h>

#include <mpm_common.h>
#include <httpd.h>
#include <http_connection.h>
#include <http_core.h>
#include <http_log.h>
#include <http_protocol.h>

#include "h2.h"
#include "h2_private.h"
#include "h2_mplx.h"
#include "h2_c2.h"
#include "h2_workers.h"
#include "h2_util.h"

typedef struct h2_slot h2_slot;
struct h2_slot {
    int id;
    h2_slot *next;
    h2_workers *workers;
    conn_rec *connection;
    apr_thread_t *thread;
    apr_thread_mutex_t *lock;
    apr_thread_cond_t *not_idle;
    /* atomic */ apr_uint32_t timed_out;
};

static h2_slot *pop_slot(h2_slot *volatile *phead) 
{
    /* Atomically pop a slot from the list */
    for (;;) {
        h2_slot *first = *phead;
        if (first == NULL) {
            return NULL;
        }
        if (apr_atomic_casptr((void*)phead, first->next, first) == first) {
            first->next = NULL;
            return first;
        }
    }
}

static void push_slot(h2_slot *volatile *phead, h2_slot *slot)
{
    /* Atomically push a slot to the list */
    ap_assert(!slot->next);
    for (;;) {
        h2_slot *next = slot->next = *phead;
        if (apr_atomic_casptr((void*)phead, slot, next) == next) {
            return;
        }
    }
}

static void* APR_THREAD_FUNC slot_run(apr_thread_t *thread, void *wctx);
static void slot_done(h2_slot *slot);

static apr_status_t activate_slot(h2_workers *workers, h2_slot *slot) 
{
    apr_status_t rv;
    
    slot->workers = workers;
    slot->connection = NULL;

    apr_thread_mutex_lock(workers->lock);
    if (!slot->lock) {
        rv = apr_thread_mutex_create(&slot->lock,
                                         APR_THREAD_MUTEX_DEFAULT,
                                         workers->pool);
        if (rv != APR_SUCCESS) goto cleanup;
    }

    if (!slot->not_idle) {
        rv = apr_thread_cond_create(&slot->not_idle, workers->pool);
        if (rv != APR_SUCCESS) goto cleanup;
    }
    
    ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                 "h2_workers: new thread for slot %d", slot->id); 

    /* thread will either immediately start work or add itself
     * to the idle queue */
    apr_atomic_inc32(&workers->worker_count);
    apr_atomic_set32(&slot->timed_out, 0);
    rv = ap_thread_create(&slot->thread, workers->thread_attr,
                          slot_run, slot, workers->pool);
    if (rv != APR_SUCCESS) {
        apr_atomic_dec32(&workers->worker_count);
    }

cleanup:
    apr_thread_mutex_unlock(workers->lock);
    if (rv != APR_SUCCESS) {
        push_slot(&workers->free, slot);
    }
    return rv;
}

static apr_status_t add_worker(h2_workers *workers)
{
    h2_slot *slot = pop_slot(&workers->free);
    if (slot) {
        return activate_slot(workers, slot);
    }
    return APR_EAGAIN;
}

static void wake_idle_worker(h2_workers *workers) 
{
    h2_slot *slot;;
    for (;;) {
        slot = pop_slot(&workers->idle);
        if (!slot) {
            if (workers->dynamic && apr_atomic_read32(&workers->shutdown) == 0) {
                add_worker(workers);
            }
            return;
        }
        if (!apr_atomic_read32(&slot->timed_out)) {
            apr_thread_mutex_lock(slot->lock);
            apr_thread_cond_signal(slot->not_idle);
            apr_thread_mutex_unlock(slot->lock);
            return;
        }
        slot_done(slot);
    }
}

static void join_zombies(h2_workers *workers)
{
    h2_slot *slot;
    while ((slot = pop_slot(&workers->zombies))) {
        apr_status_t status;
        ap_assert(slot->thread != NULL);
        apr_thread_join(&status, slot->thread);
        slot->thread = NULL;

        push_slot(&workers->free, slot);
    }
}

static apr_status_t slot_pull_c2(h2_slot *slot, h2_mplx *m)
{
    apr_status_t rv;
    
    rv = h2_mplx_worker_pop_c2(m, &slot->connection);
    if (slot->connection) {
        return rv;
    }
    return APR_EOF;
}

static h2_fifo_op_t mplx_peek(void *head, void *ctx)
{
    h2_mplx *m = head;
    h2_slot *slot = ctx;
    
    if (slot_pull_c2(slot, m) == APR_EAGAIN) {
        wake_idle_worker(slot->workers);
        return H2_FIFO_OP_REPUSH;
    } 
    return H2_FIFO_OP_PULL;
}

/**
 * Get the next c2 for the given worker. Will block until a c2 arrives
 * or the max_wait timer expires and more than min workers exist.
 */
static int get_next(h2_slot *slot)
{
    h2_workers *workers = slot->workers;
    int non_essential = slot->id >= workers->min_workers;
    apr_status_t rv;

    while (apr_atomic_read32(&workers->aborted) == 0
        && apr_atomic_read32(&slot->timed_out) == 0) {
        ap_assert(slot->connection == NULL);
        if (non_essential && apr_atomic_read32(&workers->shutdown)) {
            /* Terminate non-essential worker on shutdown */
            break;
        }
        if (h2_fifo_try_peek(workers->mplxs, mplx_peek, slot) == APR_EOF) {
            /* The queue is terminated with the MPM child being cleaned up,
             * just leave. */
            break;
        }
        if (slot->connection) {
            return 1;
        }
        
        join_zombies(workers);

        apr_thread_mutex_lock(slot->lock);
        if (apr_atomic_read32(&workers->aborted) == 0) {
            apr_uint32_t idle_secs;

            push_slot(&workers->idle, slot);
            if (non_essential
                && (idle_secs = apr_atomic_read32(&workers->max_idle_secs))) {
                rv = apr_thread_cond_timedwait(slot->not_idle, slot->lock,
                                               apr_time_from_sec(idle_secs));
                if (APR_TIMEUP == rv) {
                    apr_atomic_set32(&slot->timed_out, 1);
                }
            }
            else {
                apr_thread_cond_wait(slot->not_idle, slot->lock);
            }
        }
        apr_thread_mutex_unlock(slot->lock);
    }

    return 0;
}

static void slot_done(h2_slot *slot)
{
    h2_workers *workers = slot->workers;

    push_slot(&workers->zombies, slot);

    /* If this worker is the last one exiting and the MPM child is stopping,
     * unblock workers_pool_cleanup().
     */
    if (!apr_atomic_dec32(&workers->worker_count)
        && apr_atomic_read32(&workers->aborted)) {
        apr_thread_mutex_lock(workers->lock);
        apr_thread_cond_signal(workers->all_done);
        apr_thread_mutex_unlock(workers->lock);
    }
}


static void* APR_THREAD_FUNC slot_run(apr_thread_t *thread, void *wctx)
{
    h2_slot *slot = wctx;
    
    /* Get the next c2 from mplx to process. */
    while (get_next(slot)) {
        /* See the discussion at <https://github.com/icing/mod_h2/issues/195>
         *
         * Each conn_rec->id is supposed to be unique at a point in time. Since
         * some modules (and maybe external code) uses this id as an identifier
         * for the request_rec they handle, it needs to be unique for secondary
         * connections also.
         *
         * The MPM module assigns the connection ids and mod_unique_id is using
         * that one to generate identifier for requests. While the implementation
         * works for HTTP/1.x, the parallel execution of several requests per
         * connection will generate duplicate identifiers on load.
         *
         * The original implementation for secondary connection identifiers used
         * to shift the master connection id up and assign the stream id to the
         * lower bits. This was cramped on 32 bit systems, but on 64bit there was
         * enough space.
         *
         * As issue 195 showed, mod_unique_id only uses the lower 32 bit of the
         * connection id, even on 64bit systems. Therefore collisions in request ids.
         *
         * The way master connection ids are generated, there is some space "at the
         * top" of the lower 32 bits on allmost all systems. If you have a setup
         * with 64k threads per child and 255 child processes, you live on the edge.
         *
         * The new implementation shifts 8 bits and XORs in the worker
         * id. This will experience collisions with > 256 h2 workers and heavy
         * load still. There seems to be no way to solve this in all possible
         * configurations by mod_h2 alone.
         */
        AP_DEBUG_ASSERT(slot->connection != NULL);
        slot->connection->id = (slot->connection->master->id << 8)^slot->id;
        slot->connection->current_thread = thread;

        ap_process_connection(slot->connection, ap_get_conn_socket(slot->connection));

        h2_mplx_worker_c2_done(slot->connection);
        slot->connection = NULL;
    }

    if (apr_atomic_read32(&slot->timed_out) == 0) {
        slot_done(slot);
    }

    apr_thread_exit(thread, APR_SUCCESS);
    return NULL;
}

static void wake_non_essential_workers(h2_workers *workers)
{
    h2_slot *slot;
    /* pop all idle, signal the non essentials and add the others again */
    if ((slot = pop_slot(&workers->idle))) {
        wake_non_essential_workers(workers);
        if (slot->id > workers->min_workers) {
            apr_thread_mutex_lock(slot->lock);
            apr_thread_cond_signal(slot->not_idle);
            apr_thread_mutex_unlock(slot->lock);
        }
        else {
            push_slot(&workers->idle, slot);
        }
    }
}

static void workers_abort_idle(h2_workers *workers)
{
    h2_slot *slot;

    apr_atomic_set32(&workers->shutdown, 1);
    apr_atomic_set32(&workers->aborted, 1);
    h2_fifo_term(workers->mplxs);

    /* abort all idle slots */
    while ((slot = pop_slot(&workers->idle))) {
        apr_thread_mutex_lock(slot->lock);
        apr_thread_cond_signal(slot->not_idle);
        apr_thread_mutex_unlock(slot->lock);
    }
}

static apr_status_t workers_pool_cleanup(void *data)
{
    h2_workers *workers = data;
    apr_time_t end, timeout = apr_time_from_sec(1);
    apr_status_t rv;
    int n, wait_sec = 5;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, workers->s,
                 "h2_workers: cleanup %d workers idling",
                 (int)apr_atomic_read32(&workers->worker_count));
    workers_abort_idle(workers);

    /* wait for all the workers to become zombies and join them.
     * this gets called after the mpm shuts down and all connections
     * have either been handled (graceful) or we are forced exiting
     * (ungrateful). Either way, we show limited patience. */
    apr_thread_mutex_lock(workers->lock);
    end = apr_time_now() + apr_time_from_sec(wait_sec);
    while ((n = apr_atomic_read32(&workers->worker_count)) > 0
           && apr_time_now() < end) {
        rv = apr_thread_cond_timedwait(workers->all_done, workers->lock, timeout);
        if (APR_TIMEUP == rv) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, workers->s,
                         APLOGNO(10290) "h2_workers: waiting for idle workers to close, "
                         "still seeing %d workers living",
                         apr_atomic_read32(&workers->worker_count));
            continue;
        }
    }
    if (n) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, workers->s,
                     APLOGNO(10291) "h2_workers: cleanup, %d idle workers "
                     "did not exit after %d seconds.",
                     n, wait_sec);
    }
    apr_thread_mutex_unlock(workers->lock);
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, workers->s,
                 "h2_workers: cleanup all workers terminated");
    join_zombies(workers);
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, workers->s,
                 "h2_workers: cleanup zombie workers joined");

    return APR_SUCCESS;
}

h2_workers *h2_workers_create(server_rec *s, apr_pool_t *pchild,
                              int min_workers, int max_workers,
                              int idle_secs)
{
    apr_status_t rv;
    h2_workers *workers;
    apr_pool_t *pool;
    int i, n;

    ap_assert(s);
    ap_assert(pchild);

    /* let's have our own pool that will be parent to all h2_worker
     * instances we create. This happens in various threads, but always
     * guarded by our lock. Without this pool, all subpool creations would
     * happen on the pool handed to us, which we do not guard.
     */
    apr_pool_create(&pool, pchild);
    apr_pool_tag(pool, "h2_workers");
    workers = apr_pcalloc(pool, sizeof(h2_workers));
    if (!workers) {
        return NULL;
    }
    
    workers->s = s;
    workers->pool = pool;
    workers->min_workers = min_workers;
    workers->max_workers = max_workers;
    workers->max_idle_secs = (idle_secs > 0)? idle_secs : 10;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, workers->s,
                 "h2_workers: created with min=%d max=%d idle_timeout=%d sec",
                 workers->min_workers, workers->max_workers,
                 (int)workers->max_idle_secs);
    /* FIXME: the fifo set we use here has limited capacity. Once the
     * set is full, connections with new requests do a wait.
     */
    rv = h2_fifo_set_create(&workers->mplxs, pool, 16 * 1024);
    if (rv != APR_SUCCESS) goto cleanup;

    rv = apr_threadattr_create(&workers->thread_attr, workers->pool);
    if (rv != APR_SUCCESS) goto cleanup;

    if (ap_thread_stacksize != 0) {
        apr_threadattr_stacksize_set(workers->thread_attr,
                                     ap_thread_stacksize);
        ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, s,
                     "h2_workers: using stacksize=%ld", 
                     (long)ap_thread_stacksize);
    }
    
    rv = apr_thread_mutex_create(&workers->lock,
                                 APR_THREAD_MUTEX_DEFAULT,
                                 workers->pool);
    if (rv != APR_SUCCESS) goto cleanup;
    rv = apr_thread_cond_create(&workers->all_done, workers->pool);
    if (rv != APR_SUCCESS) goto cleanup;

    n = workers->nslots = workers->max_workers;
    workers->slots = apr_pcalloc(workers->pool, n * sizeof(h2_slot));
    if (workers->slots == NULL) {
        n = workers->nslots = 0;
        rv = APR_ENOMEM;
        goto cleanup;
    }
    for (i = 0; i < n; ++i) {
        workers->slots[i].id = i;
    }
    /* we activate all for now, TODO: support min_workers again.
     * do this in reverse for vanity reasons so slot 0 will most
     * likely be at head of idle queue. */
    n = workers->min_workers;
    for (i = n-1; i >= 0; --i) {
        rv = activate_slot(workers, &workers->slots[i]);
        if (rv != APR_SUCCESS) goto cleanup;
    }
    /* the rest of the slots go on the free list */
    for(i = n; i < workers->nslots; ++i) {
        push_slot(&workers->free, &workers->slots[i]);
    }
    workers->dynamic = (workers->worker_count < workers->max_workers);

cleanup:
    if (rv == APR_SUCCESS) {
        /* Stop/join the workers threads when the MPM child exits (pchild is
         * destroyed), and as a pre_cleanup of pchild thus before the threads
         * pools (children of workers->pool) so that they are not destroyed
         * before/under us.
         */
        apr_pool_pre_cleanup_register(pchild, workers, workers_pool_cleanup);    
        return workers;
    }
    return NULL;
}

apr_status_t h2_workers_register(h2_workers *workers, struct h2_mplx *m)
{
    apr_status_t status = h2_fifo_push(workers->mplxs, m);
    wake_idle_worker(workers);
    return status;
}

apr_status_t h2_workers_unregister(h2_workers *workers, struct h2_mplx *m)
{
    return h2_fifo_remove(workers->mplxs, m);
}

void h2_workers_graceful_shutdown(h2_workers *workers)
{
    apr_atomic_set32(&workers->shutdown, 1);
    apr_atomic_set32(&workers->max_idle_secs, 1);
    wake_non_essential_workers(workers);
}
