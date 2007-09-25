/* conn.c - network connection state */

#include <stdlib.h>
#include <stdio.h>

#include "conn.h"
#include "net.h"
#include "util.h"
#include "prot.h"

/* linked list of free connections. See struct conn's next field in conn.h */
static conn pool_front = NULL, pool_rear = NULL;

static int
pool_conn_p()
{
    return !!pool_front;
}

static conn
conn_alloc()
{
    conn c;

    if (!pool_conn_p()) return malloc(sizeof(struct conn));

    /* remove it from the list */
    c = pool_front;
    pool_front = c->next;
    c->next = NULL;

    return c;
}

static void
conn_free(conn c)
{
    c->fd = 0;
    c->next = NULL;
    if (pool_conn_p()) {
        pool_rear->next = c;
    } else {
        pool_front = c;
    }
    pool_rear = c;
}

conn
make_conn(int fd, char start_state)
{
    conn c;

    c = conn_alloc();
    if (!c) return warn("OOM"), NULL;

    c->fd = fd;
    c->state = start_state;
    c->cmd_read = 0;
    c->in_job = NULL;
    c->reserved_job = NULL;

    return c;
}

int
conn_update_evq(conn c, const int events, evh handler)
{
    int r;

    if (!c) return -1;

    if (c->evq.ev_events == events) return 0;

    /* If it's been added, try to delete it first */
    if (c->evq.ev_base) {
        r = event_del(&c->evq);
        if (r == -1) return -1;
    }

    /* set the events and handler, but don't change any existing handler */
    event_set(&c->evq, c->fd, events, c->evq.ev_callback ? : handler, c);

    r = event_add(&c->evq, NULL);
    if (r == -1) return -1;

    return 0;
}

void
conn_close(conn c)
{
    fprintf(stderr, "closing conn %d\n", c->fd);
    event_del(&c->evq);

    close(c->fd);
    if (c->reserved_job) enqueue_job(c->reserved_job);
    if (c->in_job) free(c->in_job);

    conn_free(c);
}
