/* beanstalk - fast, general-purpose work queue */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include "net.h"
#include "beanstalkd.h"
#include "util.h"
#include "prot.h"

static char *me;
static int detach = 0;

static void
nullfd(int fd, int flags)
{
    int r;

    close(fd);
    r = open("/dev/null", flags);
    if (r != fd) twarn("open(\"/dev/null\")"), exit(1);
}

static void
dfork()
{
    pid_t p;

    p = fork();
    if (p == -1) exit(1);
    if (p) exit(0);
}

static void
daemonize()
{
    chdir("/");
    nullfd(0, O_RDONLY);
    nullfd(1, O_WRONLY);
    nullfd(2, O_WRONLY);
    umask(0);
    dfork();
    setsid();
    dfork();
}

static void
set_sig_handlers()
{
    int r;
    struct sigaction sa;

    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    r = sigemptyset(&sa.sa_mask);
    if (r == -1) twarn("sigemptyset()"), exit(111);

    r = sigaction(SIGPIPE, &sa, 0);
    if (r == -1) twarn("sigaction(SIGPIPE)"), exit(111);

    sa.sa_handler = enter_drain_mode;
    r = sigaction(SIGUSR1, &sa, 0);
    if (r == -1) twarn("sigaction(SIGUSR1)"), exit(111);
}

/* This is a workaround for a mystifying workaround in libevent's epoll
 * implementation. The epoll_init() function creates an epoll fd with space to
 * handle RLIMIT_NOFILE - 1 fds, accompanied by the following puzzling comment:
 * "Solaris is somewhat retarded - it's important to drop backwards
 * compatibility when making changes. So, don't dare to put rl.rlim_cur here."
 * This is presumably to work around a bug in Solaris, but it has the
 * unfortunate side-effect of causing epoll_ctl() (and, therefore, event_add())
 * to fail for a valid fd if we have hit the limit of open fds. That makes it
 * hard to provide reasonable behavior in that situation. So, let's reduce the
 * real value of RLIMIT_NOFILE by one, after epoll_init() has run. */
static void
nudge_fd_limit()
{
    int r;
    struct rlimit rl;

    r = getrlimit(RLIMIT_NOFILE, &rl);
    if (r != 0) twarn("getrlimit(RLIMIT_NOFILE)"), exit(2);

    rl.rlim_cur--;

    r = setrlimit(RLIMIT_NOFILE, &rl);
    if (r != 0) twarn("setrlimit(RLIMIT_NOFILE)"), exit(2);
}

static void
usage(int err)
{
    fprintf(stderr, "Use: %s [-d] [-h]\n"
            "\n"
            "Options:\n"
            " -d  detach\n"
            " -h  show this help\n",
            me);
    exit(err);
}

static void
opts(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') usage(5);
        switch (argv[i][1]) {
            case 'd':
                detach = 1;
                break;
            case 'h':
                usage(0);
            default:
                warnx("unknown option: %s", argv[i]);
                usage(5);
        }
    }
}

int
main(int argc, char **argv)
{
    int r;

    me = argv[0];
    opts(argc, argv);

    prot_init();

    r = make_server_socket(HOST, PORT);
    if (r == -1) twarnx("make_server_socket()"), exit(111);

    if (detach) daemonize();
    event_init();
    set_sig_handlers();
    nudge_fd_limit();

    unbrake((evh) h_accept);

    event_dispatch();
    twarnx("got here for some reason");
    return 0;
}
