/* prot.c - protocol implementation */

/* Copyright (C) 2007 Keith Rarick and Philotic Inc.

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/uio.h>

#include "prot.h"
#include "pq.h"
#include "job.h"
#include "conn.h"
#include "util.h"
#include "reserve.h"
#include "net.h"
#include "version.h"

/* job body cannot be greater than this many bytes long */
#define JOB_DATA_SIZE_LIMIT ((1 << 16) - 1)

static pq ready_q;
static pq delay_q;

/* Doubly-linked list of waiting connections. */
static struct conn wait_queue = { &wait_queue, &wait_queue, 0 };
static struct job graveyard = { &graveyard, &graveyard, 0 };
static unsigned int buried_ct = 0, urgent_ct = 0, waiting_ct = 0;

static int drain_mode = 0;
static time_t start_time;
static unsigned long long int put_ct = 0, peek_ct = 0, reserve_ct = 0,
                     delete_ct = 0, release_ct = 0, bury_ct = 0, kick_ct = 0,
                     stats_ct = 0, timeout_ct = 0;

#define SERR_OOM 0
#define SERR_INTERNAL_ERROR 1
#define SERR_DRAIN_MODE 2

/* Complete responses for the error conditions defined in SERR_*.
 * Feel free to change the contents of these strings (for example, a better
 * description) but not their order */
static const char * serrs[] = {
    "SERVER_ERROR 0 out of memory\r\n",
    "SERVER_ERROR 1 internal error\r\n",
    "SERVER_ERROR 2 draining\r\n",
};

#define CERR_BAD_FORMAT 0
#define CERR_UNKNOWN_COMMAND 1
#define CERR_BAD_TRAILER 2
#define CERR_JOB_TOO_BIG 3

/* Complete responses for the error conditions defined in CERR_*.
 * Feel free to change the contents of these strings (for example, a better
 * description) but not their order */
static const char * cerrs[] = {
    "CLIENT_ERROR 0 bad command line format\r\n",
    "CLIENT_ERROR 1 unknown command\r\n",
    "CLIENT_ERROR 2 expected CR-LF after job body\r\n",
    "CLIENT_ERROR 3 job too big\r\n",
};

#ifdef DEBUG
static const char * op_names[] = {
    "<unknown>",
    CMD_PUT,
    CMD_PEEKJOB,
    CMD_RESERVE,
    CMD_DELETE,
    CMD_RELEASE,
    CMD_BURY,
    CMD_KICK,
    CMD_STATS,
    CMD_JOBSTATS,
    CMD_PEEK,
};
#endif

static int
waiting_conn_p()
{
    return conn_list_any_p(&wait_queue);
}

static int
buried_job_p()
{
    return job_list_any_p(&graveyard);
}

static void
reply(conn c, const char *line, int len, int state)
{
    int r;

    r = conn_update_evq(c, EV_WRITE | EV_PERSIST);
    if (r == -1) return twarnx("conn_update_evq() failed"), conn_close(c);

    c->reply = line;
    c->reply_len = len;
    c->reply_sent = 0;
    c->state = state;
}

/* will clobber c->reply_buf */
static void
reply_err(conn c, const char *msg)
{
    dprintf("sending error reply: %s", msg);
    return reply(c, msg, strlen(msg), STATE_SENDWORD);
}

#define reply_serr(c,e) (twarnx("server error: %d %s",(e),serrs[e]),\
                         reply_err((c),serrs[e]))
#define reply_cerr(c,e) (reply_err((c),cerrs[e]))

void
reply_job(conn c, job j, const char *word)
{
    int r;

    /* tell this connection which job to send */
    c->out_job = j;
    c->out_job_sent = 0;

    r = snprintf(c->reply_buf, LINE_BUF_SIZE, "%s %llu %u %u\r\n",
                 word, j->id, j->pri, j->body_size - 2);

    /* can't happen */
    if (r >= LINE_BUF_SIZE) return reply_serr(c, SERR_INTERNAL_ERROR);

    return reply(c, c->reply_buf, strlen(c->reply_buf), STATE_SENDJOB);
}

conn
remove_waiting_conn(conn c)
{
    if (!(c->type & CONN_TYPE_WAITING)) return NULL;
    c->type &= ~CONN_TYPE_WAITING;
    waiting_ct--;
    return conn_remove(c);
}

static void
process_queue()
{
    job j;

    while (waiting_conn_p()) {
        j = pq_take(ready_q);
        if (!j) return;
        if (j->pri < URGENT_THRESHOLD) urgent_ct--;
        reserve_job(remove_waiting_conn(wait_queue.next), j);
    }
}

int
enqueue_job(job j, unsigned int delay)
{
    int r;

    if (delay) {
        j->deadline = time(NULL) + delay;
        r = pq_give(delay_q, j);
        if (!r) return 0;
        j->state = JOB_STATE_DELAYED;
        set_main_timeout(pq_peek(delay_q)->deadline);
    } else {
        r = pq_give(ready_q, j);
        if (!r) return 0;
        j->state = JOB_STATE_READY;
        if (j->pri < URGENT_THRESHOLD) urgent_ct++;
    }
    process_queue();
    return 1;
}

static job
delay_q_peek()
{
    return pq_peek(delay_q);
}

static job
delay_q_take()
{
    return pq_take(delay_q);
}

void
bury_job(job j)
{
    job_insert(&graveyard, j);
    buried_ct++;
    j->state = JOB_STATE_BURIED;
    j->bury_ct++;
}

static job
remove_this_buried_job(job j)
{
    j = job_remove(j);
    if (j) buried_ct--;
    return j;
}

static int
kick_buried_job()
{
    int r;
    job j;

    if (!buried_job_p()) return 0;
    j = remove_this_buried_job(graveyard.next);
    j->kick_ct++;
    r = enqueue_job(j, 0);
    if (r) return 1;

    /* ready_q is full, so bury it */
    bury_job(j);
    return 0;
}

static unsigned int
get_delayed_job_ct()
{
    return pq_used(delay_q);
}

static int
kick_delayed_job()
{
    int r;
    job j;

    if (get_delayed_job_ct() < 1) return 0;
    j = delay_q_take();
    j->kick_ct++;
    r = enqueue_job(j, 0);
    if (r) return 1;

    /* ready_q is full, so delay it again */
    r = enqueue_job(j, j->delay);
    if (r) return 0;

    /* last resort */
    bury_job(j);
    return 0;
}

/* return the number of jobs successfully kicked */
static unsigned int
kick_buried_jobs(unsigned int n)
{
    unsigned int i;
    for (i = 0; (i < n) && kick_buried_job(); ++i);
    return i;
}

/* return the number of jobs successfully kicked */
static unsigned int
kick_delayed_jobs(unsigned int n)
{
    unsigned int i;
    for (i = 0; (i < n) && kick_delayed_job(); ++i);
    return i;
}

static unsigned int
kick_jobs(unsigned int n)
{
    if (buried_job_p()) return kick_buried_jobs(n);
    return kick_delayed_jobs(n);
}

static job
peek_buried_job()
{
    return buried_job_p() ? graveyard.next : NULL;
}

static job
find_buried_job(unsigned long long int id)
{
    job j;

    for (j = graveyard.next; j != &graveyard; j = j->next) {
        if (j->id == id) return j;
    }
    return NULL;
}

static job
remove_buried_job(unsigned long long int id)
{
    return remove_this_buried_job(find_buried_job(id));
}

static void
enqueue_waiting_conn(conn c)
{
    waiting_ct++;
    c->type |= CONN_TYPE_WAITING;
    conn_insert(&wait_queue, conn_remove(c) ? : c);
}

static job
peek_job(unsigned long long int id)
{
    return pq_find(ready_q, id) ? :
           pq_find(delay_q, id) ? :
           find_reserved_job(id) ? :
           find_reserved_job_in_list(&wait_queue, id) ? :
           find_buried_job(id);
}

static unsigned int
get_ready_job_ct()
{
    return pq_used(ready_q);
}

static unsigned int
get_buried_job_ct()
{
    return buried_ct;
}

static unsigned int
get_urgent_job_ct()
{
    return urgent_ct;
}

static int
count_cur_waiting()
{
    return waiting_ct;
}

static void
check_err(conn c, const char *s)
{
    if (errno == EAGAIN) return;
    if (errno == EINTR) return;
    if (errno == EWOULDBLOCK) return;

    twarn("%s", s);
    conn_close(c);
    return;
}

/* Scan the given string for the sequence "\r\n" and return the line length.
 * Always returns at least 2 if a match is found. Returns 0 if no match. */
static int
scan_line_end(const char *s, int size)
{
    char *match;

    match = memchr(s, '\r', size - 1);
    if (!match) return 0;

    /* this is safe because we only scan size - 1 chars above */
    if (match[1] == '\n') return match - s + 2;

    return 0;
}

static int
cmd_len(conn c)
{
    return scan_line_end(c->cmd, c->cmd_read);
}

/* parse the command line */
static int
which_cmd(conn c)
{
#define TEST_CMD(s,c,o) if (strncmp((s), (c), CONSTSTRLEN(c)) == 0) return (o);
    TEST_CMD(c->cmd, CMD_PUT, OP_PUT);
    TEST_CMD(c->cmd, CMD_PEEKJOB, OP_PEEKJOB);
    TEST_CMD(c->cmd, CMD_PEEK, OP_PEEK);
    TEST_CMD(c->cmd, CMD_RESERVE, OP_RESERVE);
    TEST_CMD(c->cmd, CMD_DELETE, OP_DELETE);
    TEST_CMD(c->cmd, CMD_RELEASE, OP_RELEASE);
    TEST_CMD(c->cmd, CMD_BURY, OP_BURY);
    TEST_CMD(c->cmd, CMD_KICK, OP_KICK);
    TEST_CMD(c->cmd, CMD_JOBSTATS, OP_JOBSTATS);
    TEST_CMD(c->cmd, CMD_STATS, OP_STATS);
    return OP_UNKNOWN;
}

/* Copy up to body_size trailing bytes into the job, then the rest into the cmd
 * buffer. If c->in_job exists, this assumes that c->in_job->body is empty.
 * This function is idempotent(). */
static void
fill_extra_data(conn c)
{
    int extra_bytes, job_data_bytes = 0, cmd_bytes;

    if (!c->fd) return; /* the connection was closed */
    if (!c->cmd_len) return; /* we don't have a complete command */

    /* how many extra bytes did we read? */
    extra_bytes = c->cmd_read - c->cmd_len;

    /* how many bytes should we put into the job body? */
    if (c->in_job) {
        job_data_bytes = min(extra_bytes, c->in_job->body_size);
        memcpy(c->in_job->body, c->cmd + c->cmd_len, job_data_bytes);
        c->in_job_read = job_data_bytes;
    }

    /* how many bytes are left to go into the future cmd? */
    cmd_bytes = extra_bytes - job_data_bytes;
    memmove(c->cmd, c->cmd + c->cmd_len + job_data_bytes, cmd_bytes);
    c->cmd_read = cmd_bytes;
    c->cmd_len = 0; /* we no longer know the length of the new command */
}

static void
enqueue_incoming_job(conn c)
{
    int r;
    job j = c->in_job;

    c->in_job = NULL; /* the connection no longer owns this job */
    c->in_job_read = 0;

    /* check if the trailer is present and correct */
    if (memcmp(j->body + j->body_size - 2, "\r\n", 2)) {
        free(j);
        return reply_cerr(c, CERR_BAD_TRAILER);
    }

    /* we have a complete job, so let's stick it in the pqueue */
    r = enqueue_job(j, j->delay);
    put_ct++; /* stats */

    if (r) {
        r = snprintf(c->reply_buf, LINE_BUF_SIZE, MSG_INSERTED_FMT, j->id);
        if (r >= LINE_BUF_SIZE) return reply_serr(c, SERR_INTERNAL_ERROR);
        return reply(c, c->reply_buf, r, STATE_SENDWORD);
    }

    bury_job(j); /* there was no room in the queue, so it gets buried */
    reply(c, MSG_BURIED, MSG_BURIED_LEN, STATE_SENDWORD);
}

static unsigned int
uptime()
{
    return time(NULL) - start_time;
}

static int
fmt_stats(char *buf, size_t size, void *x)
{
    struct rusage ru = {{0, 0}, {0, 0}};
    getrusage(RUSAGE_SELF, &ru); /* don't care if it fails */
    return snprintf(buf, size, STATS_FMT,
            get_urgent_job_ct(),
            get_ready_job_ct(),
            get_reserved_job_ct(),
            get_delayed_job_ct(),
            get_buried_job_ct(),
            HEAP_SIZE,
            put_ct,
            peek_ct,
            reserve_ct,
            delete_ct,
            release_ct,
            bury_ct,
            kick_ct,
            stats_ct,
            timeout_ct,
            total_jobs(),
            count_cur_conns(),
            count_cur_producers(),
            count_cur_workers(),
            count_cur_waiting(),
            count_tot_conns(),
            getpid(),
            VERSION,
            (int) ru.ru_utime.tv_sec, (int) ru.ru_utime.tv_usec,
            (int) ru.ru_stime.tv_sec, (int) ru.ru_stime.tv_usec,
            uptime());

}

/* Read a priority value from the given buffer and place it in pri.
 * Update end to point to the address after the last character consumed.
 * Pri and end can be NULL. If they are both NULL, read_pri() will do the
 * conversion and return the status code but not update any values. This is an
 * easy way to check for errors.
 * Return 0 on success, or nonzero on failure.
 * If a failure occurs, pri and end are not modified. */
static int
read_pri(unsigned int *pri, const char *buf, char **end)
{
    char *tend;
    unsigned int tpri;

    errno = 0;
    tpri = strtoul(buf, &tend, 10);
    if (tend == buf) return -1;
    if (errno && errno != ERANGE) return -1;

    if (pri) *pri = tpri;
    if (end) *end = tend;
    return 0;
}

/* Read a delay value from the given buffer and place it in delay.
 * The interface and behavior are the same as in read_pri(). */
static int
read_delay(unsigned int *delay, const char *buf, char **end)
{
    return read_pri(delay, buf, end);
}

/* Read a timeout value from the given buffer and place it in ttr.
 * The interface and behavior are the same as in read_pri(). */
static int
read_ttr(unsigned int *ttr, const char *buf, char **end)
{
    return read_pri(ttr, buf, end);
}

static void
wait_for_job(conn c)
{
    int r;

    /* this conn is waiting, but we want to know if they hang up */
    r = conn_update_evq(c, EV_READ | EV_PERSIST);
    if (r == -1) return twarnx("update events failed"), conn_close(c);

    c->state = STATE_WAIT;
    enqueue_waiting_conn(c);
}

static void
do_stats(conn c, int(*fmt)(char *, size_t, void *), void *data)
{
    int r, stats_len;

    /* first, measure how big a buffer we will need */
    stats_len = fmt(NULL, 0, data);

    c->out_job = allocate_job(stats_len); /* fake job to hold stats data */
    if (!c->out_job) return reply_serr(c, SERR_OOM);

    /* now actually format the stats data */
    r = fmt(c->out_job->body, stats_len, data);
    if (r != stats_len) return reply_serr(c, SERR_INTERNAL_ERROR);
    c->out_job->body[stats_len - 1] = '\n'; /* patch up sprintf's output */

    c->out_job_sent = 0;

    r = snprintf(c->reply_buf, LINE_BUF_SIZE, "OK %d\r\n", stats_len - 2);
    if (r >= LINE_BUF_SIZE) return reply_serr(c, SERR_INTERNAL_ERROR);

    reply(c, c->reply_buf, strlen(c->reply_buf), STATE_SENDJOB);
}

static int
fmt_job_stats(char *buf, size_t size, void *jp)
{
    time_t t;
    job j = (job) jp;

    t = time(NULL);
    return snprintf(buf, size, JOB_STATS_FMT,
            j->id,
            job_state(j),
            (unsigned int) (t - j->creation),
            j->delay,
            j->ttr,
            (unsigned int) (j->deadline - t),
            j->timeout_ct,
            j->release_ct,
            j->bury_ct,
            j->kick_ct);
}

static void
maybe_enqueue_incoming_job(conn c)
{
    job j = c->in_job;

    /* do we have a complete job? */
    if (c->in_job_read == j->body_size) return enqueue_incoming_job(c);

    /* otherwise we have incomplete data, so just keep waiting */
    c->state = STATE_WANTDATA;
}

static void
dispatch_cmd(conn c)
{
    int r, i;
    unsigned int count;
    job j;
    char type;
    char *size_buf, *delay_buf, *ttr_buf, *pri_buf, *end_buf;
    unsigned int pri, delay, ttr, body_size;
    unsigned long long int id;

    /* NUL-terminate this string so we can use strtol and friends */
    c->cmd[c->cmd_len - 2] = '\0';

    /* check for possible maliciousness */
    if (strlen(c->cmd) != c->cmd_len - 2) return reply_cerr(c, CERR_BAD_FORMAT);

    type = which_cmd(c);
    dprintf("got %s command: \"%s\"\n", op_names[(int) type], c->cmd);

    switch (type) {
    case OP_PUT:
        if (drain_mode) return reply_serr(c, SERR_DRAIN_MODE);

        r = read_pri(&pri, c->cmd + 4, &delay_buf);
        if (r) return conn_close(c);

        r = read_delay(&delay, delay_buf, &ttr_buf);
        if (r) return conn_close(c);

        r = read_ttr(&ttr, ttr_buf, &size_buf);
        if (r) return conn_close(c);

        errno = 0;
        body_size = strtoul(size_buf, &end_buf, 10);
        if (errno) return conn_close(c);

        if (body_size > JOB_DATA_SIZE_LIMIT) {
            return reply_cerr(c, CERR_JOB_TOO_BIG);
        }

        /* don't allow trailing garbage */
        if (end_buf[0] != '\0') return conn_close(c);

        conn_set_producer(c);

        c->in_job = make_job(pri, delay, ttr, body_size + 2);

        fill_extra_data(c);

        /* it's possible we already have a complete job */
        maybe_enqueue_incoming_job(c);

        break;
    case OP_PEEK:
        /* don't allow trailing garbage */
        if (c->cmd_len != CMD_PEEK_LEN + 2) return conn_close(c);

        j = job_copy(peek_buried_job() ? : delay_q_peek());

        if (!j) return reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);

        peek_ct++; /* stats */
        reply_job(c, j, MSG_FOUND);
        break;
    case OP_PEEKJOB:
        errno = 0;
        id = strtoull(c->cmd + CMD_PEEKJOB_LEN, &end_buf, 10);
        if (errno) return conn_close(c);

        /* So, peek is annoying, because some other connection might free the
         * job while we are still trying to write it out. So we copy it and
         * then free the copy when it's done sending. */
        j = job_copy(peek_job(id));

        if (!j) return reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);

        peek_ct++; /* stats */
        reply_job(c, j, MSG_FOUND);
        break;
    case OP_RESERVE:
        /* don't allow trailing garbage */
        if (c->cmd_len != CMD_RESERVE_LEN + 2) return conn_close(c);

        reserve_ct++; /* stats */
        conn_set_worker(c);

        /* try to get a new job for this guy */
        wait_for_job(c);
        process_queue();
        break;
    case OP_DELETE:
        errno = 0;
        id = strtoull(c->cmd + CMD_DELETE_LEN, &end_buf, 10);
        if (errno) return conn_close(c);

        j = remove_reserved_job(c, id) ? : remove_buried_job(id);

        if (!j) return reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);

        delete_ct++; /* stats */
        free(j);

        reply(c, MSG_DELETED, MSG_DELETED_LEN, STATE_SENDWORD);
        break;
    case OP_RELEASE:
        errno = 0;
        id = strtoull(c->cmd + CMD_RELEASE_LEN, &pri_buf, 10);
        if (errno) return conn_close(c);

        r = read_pri(&pri, pri_buf, &delay_buf);
        if (r) return conn_close(c);

        r = read_delay(&delay, delay_buf, NULL);
        if (r) return conn_close(c);

        j = remove_reserved_job(c, id);

        if (!j) return reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);

        j->pri = pri;
        j->delay = delay;
        j->release_ct++;
        release_ct++; /* stats */
        r = enqueue_job(j, delay);
        if (r) return reply(c, MSG_RELEASED, MSG_RELEASED_LEN, STATE_SENDWORD);

        bury_job(j); /* there was no room in the queue, so it gets buried */
        reply(c, MSG_BURIED, MSG_BURIED_LEN, STATE_SENDWORD);
        break;
    case OP_BURY:
        errno = 0;
        id = strtoull(c->cmd + CMD_BURY_LEN, &pri_buf, 10);
        if (errno) return conn_close(c);

        r = read_pri(&pri, pri_buf, NULL);
        if (r) return conn_close(c);

        j = remove_reserved_job(c, id);

        if (!j) return reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);

        j->pri = pri;
        bury_ct++; /* stats */
        bury_job(j);
        reply(c, MSG_BURIED, MSG_BURIED_LEN, STATE_SENDWORD);
        break;
    case OP_KICK:
        errno = 0;
        count = strtoul(c->cmd + CMD_KICK_LEN, &end_buf, 10);
        if (end_buf == c->cmd + CMD_KICK_LEN) return conn_close(c);
        if (errno) return conn_close(c);

        kick_ct++; /* stats */

        i = kick_jobs(count);

        r = snprintf(c->reply_buf, LINE_BUF_SIZE, "KICKED %u\r\n", i);
        /* can't happen */
        if (r >= LINE_BUF_SIZE) return reply_serr(c, SERR_INTERNAL_ERROR);

        reply(c, c->reply_buf, strlen(c->reply_buf), STATE_SENDWORD);
        break;
    case OP_STATS:
        /* don't allow trailing garbage */
        if (c->cmd_len != CMD_STATS_LEN + 2) return conn_close(c);

        stats_ct++; /* stats */

        do_stats(c, fmt_stats, NULL);
        break;
    case OP_JOBSTATS:
        errno = 0;
        id = strtoull(c->cmd + CMD_JOBSTATS_LEN, &end_buf, 10);
        if (errno) return conn_close(c);

        j = peek_job(id);
        if (!j) return reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);

        stats_ct++; /* stats */
        do_stats(c, fmt_job_stats, j);
        break;
    default:
        return reply_cerr(c, CERR_UNKNOWN_COMMAND);
    }
}

/* if we get a timeout, it means that a job has been reserved for too long, so
 * we should put it back in the queue */
static void
h_conn_timeout(conn c)
{
    int r;
    job j;

    while ((j = soonest_job(c))) {
        if (j->deadline > time(NULL)) return;
        timeout_ct++; /* stats */
        j->timeout_ct++;
        r = enqueue_job(remove_this_reserved_job(c, j), 0);
        if (!r) bury_job(j); /* there was no room in the queue, so bury it */
        r = conn_update_evq(c, c->evq.ev_events);
        if (r == -1) return twarnx("conn_update_evq() failed"), conn_close(c);
    }
}

void
enter_drain_mode(int sig)
{
    drain_mode = 1;
}

static void
do_cmd(conn c)
{
    dispatch_cmd(c);
    fill_extra_data(c);
}

static void
reset_conn(conn c)
{
    int r;

    r = conn_update_evq(c, EV_READ | EV_PERSIST);
    if (r == -1) return twarnx("update events failed"), conn_close(c);

    /* was this a peek or stats command? */
    if (!has_reserved_this_job(c, c->out_job)) free(c->out_job);
    c->out_job = NULL;

    c->reply_sent = 0; /* now that we're done, reset this */
    c->state = STATE_WANTCOMMAND;
}

static void
h_conn_data(conn c)
{
    int r;
    job j;
    struct iovec iov[2];

    switch (c->state) {
    case STATE_WANTCOMMAND:
        r = read(c->fd, c->cmd + c->cmd_read, LINE_BUF_SIZE - c->cmd_read);
        if (r == -1) return check_err(c, "read()");
        if (r == 0) return conn_close(c); /* the client hung up */

        c->cmd_read += r; /* we got some bytes */

        c->cmd_len = cmd_len(c); /* find the EOL */

        /* yay, complete command line */
        if (c->cmd_len) return do_cmd(c);

        /* c->cmd_read > LINE_BUF_SIZE can't happen */

        /* command line too long? */
        if (c->cmd_read == LINE_BUF_SIZE) return conn_close(c);

        /* otherwise we have an incomplete line, so just keep waiting */
        break;
    case STATE_WANTDATA:
        j = c->in_job;

        r = read(c->fd, j->body + c->in_job_read, j->body_size - c->in_job_read);
        if (r == -1) return check_err(c, "read()");
        if (r == 0) return conn_close(c); /* the client hung up */

        c->in_job_read += r; /* we got some bytes */

        /* (j->in_job_read > j->body_size) can't happen */

        maybe_enqueue_incoming_job(c);
        break;
    case STATE_SENDWORD:
        r= write(c->fd, c->reply + c->reply_sent, c->reply_len - c->reply_sent);
        if (r == -1) return check_err(c, "write()");
        if (r == 0) return conn_close(c); /* the client hung up */

        c->reply_sent += r; /* we got some bytes */

        /* (c->reply_sent > c->reply_len) can't happen */

        if (c->reply_sent == c->reply_len) return reset_conn(c);

        /* otherwise we sent an incomplete reply, so just keep waiting */
        break;
    case STATE_SENDJOB:
        j = c->out_job;

        iov[0].iov_base = (void *)(c->reply + c->reply_sent);
        iov[0].iov_len = c->reply_len - c->reply_sent; /* maybe 0 */
        iov[1].iov_base = j->body + c->out_job_sent;
        iov[1].iov_len = j->body_size - c->out_job_sent;

        r = writev(c->fd, iov, 2);
        if (r == -1) return check_err(c, "writev()");
        if (r == 0) return conn_close(c); /* the client hung up */

        /* update the sent values */
        c->reply_sent += r;
        if (c->reply_sent >= c->reply_len) {
            c->out_job_sent += c->reply_sent - c->reply_len;
            c->reply_sent = c->reply_len;
        }

        /* (c->out_job_sent > j->body_size) can't happen */

        /* are we done? */
        if (c->out_job_sent == j->body_size) return reset_conn(c);

        /* otherwise we sent incomplete data, so just keep waiting */
        break;
    case STATE_WAIT: /* keep an eye out in case they hang up */
        /* but don't hang up just because our buffer is full */
        if (LINE_BUF_SIZE - c->cmd_read < 1) break;

        r = read(c->fd, c->cmd + c->cmd_read, LINE_BUF_SIZE - c->cmd_read);
        if (r == -1) return check_err(c, "read()");
        if (r == 0) return conn_close(c); /* the client hung up */
        c->cmd_read += r; /* we got some bytes */
    }
}

#define want_command(c) ((c)->fd && ((c)->state == STATE_WANTCOMMAND))
#define cmd_data_ready(c) (want_command(c) && (c)->cmd_read)

static void
h_conn(const int fd, const short which, conn c)
{
    if (fd != c->fd) {
        twarnx("Argh! event fd doesn't match conn fd.");
        close(fd);
        return conn_close(c);
    }

    switch (which) {
    case EV_TIMEOUT:
        h_conn_timeout(c);
        event_add(&c->evq, NULL); /* seems to be necessary */
        break;
    case EV_READ:
        /* fall through... */
    case EV_WRITE:
        /* fall through... */
    default:
        h_conn_data(c);
    }

    while (cmd_data_ready(c) && (c->cmd_len = cmd_len(c))) do_cmd(c);
}

static void
h_delay()
{
    int r;
    job j;
    time_t t;

    t = time(NULL);
    while ((j = delay_q_peek())) {
        if (j->deadline > t) break;
        j = delay_q_take();
        r = enqueue_job(j, 0);
        if (!r) bury_job(j); /* there was no room in the queue, so bury it */
    }

    set_main_timeout((j = delay_q_peek()) ? j->deadline : 0);
}

void
h_accept(const int fd, const short which, struct event *ev)
{
    conn c;
    int cfd, flags, r;
    socklen_t addrlen;
    struct sockaddr addr;

    if (which == EV_TIMEOUT) return h_delay();

    addrlen = sizeof addr;
    cfd = accept(fd, &addr, &addrlen);
    if (cfd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) twarn("accept()");
        if (errno == EMFILE) brake();
        return;
    }

    flags = fcntl(cfd, F_GETFL, 0);
    if (flags < 0) return twarn("getting flags"), close(cfd), v();

    r = fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
    if (r < 0) return twarn("setting O_NONBLOCK"), close(cfd), v();

    c = make_conn(cfd, STATE_WANTCOMMAND);
    if (!c) return twarnx("make_conn() failed"), close(cfd), brake();

    dprintf("accepted conn, fd=%d\n", cfd);
    r = conn_set_evq(c, EV_READ | EV_PERSIST, (evh) h_conn);
    if (r == -1) return twarnx("conn_set_evq() failed"), close(cfd), brake();
}

void
prot_init()
{
    start_time = time(NULL);
    ready_q = make_pq(HEAP_SIZE, job_pri_cmp);
    delay_q = make_pq(HEAP_SIZE, job_delay_cmp);
}
