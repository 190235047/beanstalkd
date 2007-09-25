/* prot.c - protocol implementation */

#include <stdio.h>
#include <string.h>

#include "prot.h"
#include "pq.h"
#include "job.h"
#include "conn.h"
#include "util.h"

static pq ready_q;

/* Doubly-linked list of waiting connections. */
static struct conn wait_queue = { &wait_queue, &wait_queue, 0 };

static int
waiting_conn_p()
{
    return conn_list_any_p(&wait_queue);
}

void
reply(conn c, char *line, int len, int state)
{
    int r;

    r = conn_update_evq(c, EV_WRITE | EV_PERSIST);
    if (r == -1) return warn("conn_update_evq() failed"), conn_close(c);

    c->reply = line;
    c->reply_len = len;
    c->reply_sent = 0;
    c->state = state;
}

void
reply_job(conn c, job j, const char *word)
{
    int r;

    /* tell this connection which job to send */
    c->out_job = j;
    c->out_job_sent = 0;

    r = snprintf(c->reply_buf, LINE_BUF_SIZE, "%s %lld %d\r\n",
                 word, j->id, j->pri);

    /* can't happen */
    if (r >= LINE_BUF_SIZE) return warn("truncated reply"), conn_close(c);

    return reply(c, c->reply_buf, strlen(c->reply_buf), STATE_SENDJOB);
}

static void
reserve_job(conn c, job j)
{
    c->reserved_job = j;
    return reply_job(c, j, MSG_RESERVED);
}

static conn
next_waiting_conn()
{
    conn c;

    if (!waiting_conn_p()) return NULL;

    c = wait_queue.next;
    conn_remove(c);

    return c;
}

void
process_queue()
{
    job j;

    while (waiting_conn_p()) {
        j = pq_take(ready_q);
        if (!j) return;
        reserve_job(next_waiting_conn(), j);
    }
}

int
enqueue_job(job j)
{
    int r;

    r = pq_give(ready_q, j);
    if (r) return process_queue(), 1;
    return 0;
}

void
enqueue_waiting_conn(conn c)
{
    conn_insert(&wait_queue, c);
}

job
peek_job(unsigned long long int id)
{
    return pq_find(ready_q, id);
}

void
prot_init()
{
    ready_q = make_pq(HEAP_SIZE);
}
