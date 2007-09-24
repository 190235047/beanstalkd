/* beanstalk - fast, general-purpose work queue */

#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "conn.h"
#include "net.h"
#include "beanstalkd.h"
#include "pq.h"
#include "util.h"

/* job data cannot be greater than this */
#define JOB_DATA_SIZE_LIMIT ((1 << 16) - 1)

static pq ready_q;
static conn waiting_conn_front, waiting_conn_rear;

static void
drop_root()
{
    /* pass */
}

static void
daemonize()
{
    /* pass */
}

static void
set_sig_handlers()
{
    int r;
    struct sigaction sa;

    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    r = sigemptyset(&sa.sa_mask);
    if (r == -1) perror("sigemptyset()"), exit(111);

    r = sigaction(SIGPIPE, &sa, 0);
    if (r == -1) perror("sigaction(SIGPIPE)"), exit(111);
}

static void
check_err(conn c, const char *s)
{
    if (errno == EAGAIN) return;
    if (errno == EINTR) return;
    if (errno == EWOULDBLOCK) return;

    perror(s);
    conn_close(c);
    return;
}

static int
waiting_conn_p()
{
    return !!waiting_conn_front;
}

static void
enqueue_waiting_conn(conn c)
{
    c->next_waiting = NULL;
    if (waiting_conn_p()) {
        waiting_conn_rear->next_waiting = c;
    } else {
        waiting_conn_front = c;
    }
    waiting_conn_rear = c;
}

static conn
next_waiting_conn()
{
    conn c;

    if (!waiting_conn_p()) return NULL;

    c = waiting_conn_front;
    waiting_conn_front = c->next_waiting;

    return c;
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

/* parse the command line */
static int
which_cmd(conn c)
{
#define TEST_CMD(s,c,o) if (strncmp((s), (c), CONSTSTRLEN(c)) == 0) return (o);
    TEST_CMD(c->cmd, CMD_PUT, OP_PUT);
    TEST_CMD(c->cmd, CMD_PEEK, OP_PEEK);
    TEST_CMD(c->cmd, CMD_RESERVE, OP_RESERVE);
    TEST_CMD(c->cmd, CMD_DELETE, OP_DELETE);
    TEST_CMD(c->cmd, CMD_JOBSTATS, OP_JOBSTATS);
    TEST_CMD(c->cmd, CMD_STATS, OP_STATS);
    return OP_UNKNOWN;
}

static void
reply(conn c, char *line, int len, int state)
{
    int r;

    r = conn_update_evq(c, EV_WRITE | EV_PERSIST, NULL);
    if (r == -1) return warn("conn_update_evq() failed"), conn_close(c);

    c->reply = line;
    c->reply_len = len;
    c->reply_sent = 0;
    c->state = state;
}

/* Copy up to data_size trailing bytes into the job, then the rest into the cmd
 * buffer. If c->in_job exists, this assumes that c->in_job->data is empty. */
static void
fill_extra_data(conn c)
{
    int extra_bytes, job_data_bytes = 0, cmd_bytes;

    /* how many extra bytes did we read? */
    extra_bytes = c->cmd_read - c->cmd_len;
    fprintf(stderr, "have %d extra bytes\n", extra_bytes);

    /* how many bytes should we put into the job data? */
    fprintf(stderr, "c->in_job is %p\n", c->in_job);
    if (c->in_job) {
        job_data_bytes = min(extra_bytes, c->in_job->data_size);
        memcpy(c->in_job->data, c->cmd + c->cmd_len, job_data_bytes);
        c->in_job->data_xfer = job_data_bytes;
    }

    /* how many bytes are left to go into the future cmd? */
    cmd_bytes = extra_bytes - job_data_bytes;
    memmove(c->cmd, c->cmd + c->cmd_len + job_data_bytes, cmd_bytes);
    c->cmd_read = cmd_bytes;
}

static void
send_reply(conn c, const char *word)
{
    int r;
    job j = c->reserved_job;

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

    fprintf(stderr, "found job %p\n", c->reserved_job);

    return send_reply(c, MSG_RESERVED);
}

static void
process_queue()
{
    job j;

    warn("processing queue");
    while (waiting_conn_p() && (j = pq_take(ready_q))) {
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

static void
enqueue_incoming_job(conn c)
{
    int r;
    job j = c->in_job;

    c->in_job = NULL; /* the connection no longer owns this job */

    fprintf(stderr, "enqueueing job %lld\n", j->id);

    /* check if the trailer is present and correct */
    if (memcmp(j->data + j->data_size - 2, "\r\n", 2)) return conn_close(c);

    /* we have a complete job, so let's stick it in the pqueue */
    r = enqueue_job(j);

    if (r) return reply(c, MSG_INSERTED, MSG_INSERTED_LEN, STATE_SENDWORD);

    free(j); /* the job didn't go in the queue, so it goes bye bye */
    reply(c, MSG_NOT_INSERTED, MSG_NOT_INSERTED_LEN, STATE_SENDWORD);
}

static void
maybe_enqueue_incoming_job(conn c)
{
    job j = c->in_job;

    /* do we have a complete job? */
    if (j->data_xfer == j->data_size) return enqueue_incoming_job(c);

    /* otherwise we have incomplete data, so just keep waiting */
    c->state = STATE_WANTDATA;
}

static void
do_cmd(conn c)
{
    char type;
    char *size_buf, *end_buf;
    unsigned int pri, data_size;

    /* NUL-terminate this string so we can use strtol and friends */
    c->cmd[c->cmd_len - 2] = '\0';

    /* check for possible maliciousness */
    if (strlen(c->cmd) != c->cmd_len - 2) return conn_close(c);

    type = which_cmd(c);
    switch (type) {
    case OP_PUT:
        errno = 0;
        pri = strtoul(c->cmd + 4, &size_buf, 10);
        if (errno) return conn_close(c);

        errno = 0;
        data_size = strtoul(size_buf, &end_buf, 10);
        if (errno) return conn_close(c);

        if (data_size > JOB_DATA_SIZE_LIMIT) return conn_close(c);

        /* don't allow trailing garbage */
        if (end_buf[0] != '\0') return conn_close(c);

        c->in_job = make_job(pri, data_size + 2);

        fprintf(stderr, "put pri=%d bytes=%d\n", pri, data_size);

        fill_extra_data(c);

        /* it's possible we already have a complete job */
        maybe_enqueue_incoming_job(c);

        break;
    case OP_PEEK:
        reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);
        break;
    case OP_RESERVE:
        /* don't allow trailing garbage */
        if (c->cmd_len != CMD_RESERVE_LEN + 2) return conn_close(c);

        fprintf(stderr, "got reserve cmd: %s\n", c->cmd);

        fill_extra_data(c);

        warn("looking for a job");

        /* keep any existing job, but take one if necessary */
        c->reserved_job = c->reserved_job ? : pq_take(ready_q);

        fprintf(stderr, "found job %p\n", c->reserved_job);

        if (c->reserved_job) return send_reply(c, MSG_RESERVED);

        enqueue_waiting_conn(c);
        break;
    case OP_DELETE:
        reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);
        break;
    case OP_STATS:
        /* don't allow trailing garbage */
        if (c->cmd_len != CMD_STATS_LEN + 2) return conn_close(c);
        warn("got stats command");
        conn_close(c);
        break;
    case OP_JOBSTATS:
        warn("got job stats command");
        conn_close(c);
        break;
    default:
        /* unknown command -- protocol error */
        fprintf(stderr, "got unknown cmd: %s\n", c->cmd);
        return conn_close(c);
    }
}

static void
reset_conn(conn c)
{
    int r;

    r = conn_update_evq(c, EV_READ | EV_PERSIST, NULL);
    if (r == -1) return warn("update flags failed"), conn_close(c);

    c->reply_sent = 0; /* now that we're done, reset this */
    c->state = STATE_WANTCOMMAND;
}

static void
h_conn(const int fd, const short which, conn c)
{
    int r;
    job j;
    struct iovec iov[2];

    if (fd != c->fd) {
        warn("Argh! event fd doesn't match conn fd.");
        close(fd);
        return conn_close(c);
    }

    fprintf(stderr, "%d: got event %d\n", fd, which);

    switch (c->state) {
        case STATE_WANTCOMMAND:
            r = read(fd, c->cmd + c->cmd_read, LINE_BUF_SIZE - c->cmd_read);
            if (r == -1) return check_err(c, "read()");
            if (r == 0) return conn_close(c); /* the client hung up */

            c->cmd_read += r; /* we got some bytes */

            c->cmd_len = scan_line_end(c->cmd, c->cmd_read); /* find the EOL */

            /* yay, complete command line */
            if (c->cmd_len) return do_cmd(c);

            /* command line too long? */
            if (c->cmd_read >= LINE_BUF_SIZE) return conn_close(c);

            /* otherwise we have an incomplete line, so just keep waiting */
            break;
        case STATE_WANTDATA:
            j = c->in_job;

            r = read(fd, j->data + j->data_xfer, j->data_size - j->data_xfer);
            if (r == -1) return check_err(c, "read()");
            if (r == 0) return conn_close(c); /* the client hung up */

            fprintf(stderr, "got %d data bytes\n", r);
            j->data_xfer += r; /* we got some bytes */

            /* (j->data_xfer > j->data_size) can't happen */

            maybe_enqueue_incoming_job(c);
            break;
        case STATE_SENDWORD:
            r = write(fd, c->reply+c->reply_sent, c->reply_len - c->reply_sent);
            if (r == -1) return check_err(c, "write()");
            if (r == 0) return conn_close(c); /* the client hung up */

            c->reply_sent += r; /* we got some bytes */

            /* (c->reply_sent > c->reply_len) can't happen */

            if (c->reply_sent == c->reply_len) return reset_conn(c);

            /* otherwise we sent an incomplete reply, so just keep waiting */
            break;
        case STATE_SENDJOB:
            j = c->reserved_job;

            iov[0].iov_base = c->reply + c->reply_sent;
            iov[0].iov_len = c->reply_len - c->reply_sent; /* maybe 0 */
            iov[1].iov_base = j->data + j->data_xfer;
            iov[1].iov_len = j->data_size - j->data_xfer;

            r = writev(c->fd, iov, 2);
            if (r == -1) return check_err(c, "writev()");
            if (r == 0) return conn_close(c); /* the client hung up */

            /* update the sent values */
            c->reply_sent += r;
            if (c->reply_sent >= c->reply_len) {
                j->data_xfer += c->reply_sent - c->reply_len;
                c->reply_sent = c->reply_len;
            }

            /* (j->data_xfer > j->data_size) can't happen */

            /* are we done? */
            if (j->data_xfer == j->data_size) return reset_conn(c);

            /* otherwise we sent incomplete data, so just keep waiting */
            break;
    }
}

static void
h_accept(const int fd, const short which, struct event *ev)
{
    conn c;
    int cfd, flags, r;
    socklen_t addrlen;
    struct sockaddr addr;

    addrlen = sizeof addr;
    cfd = accept(fd, &addr, &addrlen);
    if (cfd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept()");
        return;
    }

    flags = fcntl(cfd, F_GETFL, 0);
    if (flags < 0) return perror("getting flags"), close(cfd), v();

    r = fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
    if (r < 0) return perror("setting O_NONBLOCK"), close(cfd), v();

    fprintf(stderr, "got a new connection %d\n", cfd);
    c = make_conn(cfd, STATE_WANTCOMMAND);
    if (!c) return warn("make_conn() failed"), close(cfd), v();

    r = conn_update_evq(c, EV_READ | EV_PERSIST, (evh) h_conn);
    if (r == -1) return warn("conn_update_evq() failed"), close(cfd), v();
}

int
main(int argc, char **argv)
{
    int listen_socket;
    struct event listen_evq;

    listen_socket = make_server_socket(HOST, PORT);
    if (listen_socket == -1) warn("make_server_socket()"), exit(111);

    drop_root();
    daemonize();
    event_init();
    set_sig_handlers();

    event_set(&listen_evq, listen_socket, EV_READ | EV_PERSIST, (evh) h_accept, &listen_evq);
    event_add(&listen_evq, NULL);

    ready_q = make_pq(HEAP_SIZE);

    event_dispatch();
    return 0;
}
