/*
 * perfuser.c
 *
 *  Created on: Apr 13, 2015
 *      Author: francis
 */

#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <pthread.h>
#include <getopt.h>

#define DEFAULT_THREAD 1
#define DEFAULT_REPEAT 10
#define DEFAULT_ITER_MAIN 1000000
#define DEFAULT_ITER_OVERHEAD 1
#define DEFAULT_DISABLE 1
#define DEFAULT_PERIOD 10000
#define DEFAULT_ALL 0
#define progname "wk-perfuser"

static struct perf_event_attr def_attr = {
    .type = PERF_TYPE_HARDWARE,
    .size = sizeof(def_attr),
    .config = PERF_COUNT_HW_CPU_CYCLES,
    .freq = 0,
};


struct args {
    int nr_thread;
    long repeat;
    int all;
    long iter_overhead;
    long iter_main;
    int disable;
    int period;
    struct perf_event_attr attr;
};

struct counter {
    pthread_t th;
    int rank; // eq to global TLS rank
    struct args *args;
    int fd;
    int tid;
    int hits;
    gregset_t regs;
};

pthread_barrier_t barrier;
static int __thread rank;
static struct counter *cnts;

static long sys_perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
        int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd,
            flags);
}

void hog(long iter)
{
    volatile int x = iter;
    while(x != 0) {
        x--;
    }
}

static void signal_handler(int signum, siginfo_t *info, void *arg)
{
    struct counter *cnt = &cnts[rank];
    int disable = cnt->args->disable;

    if (disable)
        ioctl(cnt->fd, PERF_EVENT_IOC_DISABLE, 0);

    hog(cnt->args->iter_overhead);

    if (disable)
        ioctl(cnt->fd, PERF_EVENT_IOC_ENABLE, 0);

    int ret;
    sigset_t set;
    sigemptyset(&set);
    ret = sigpending(&set);
    assert(ret == 0);
    ret = sigismember(&set, SIGIO);
    printf("sigismember=%d\n", ret);
    if (ret)
        cnt->hits++;

    ucontext_t *ctx = arg;
    ret = memcmp(&ctx->uc_mcontext.gregs, &cnt->regs, sizeof(gregset_t));
    if (ret == 0) {
        printf("context same\n");
    } else {
        printf("context different\n");
    }

    int depth = 10;
    void *bt[depth];
    backtrace(&bt, depth);
    backtrace_symbols_fd(&bt, depth, 1);
    if (cnt->hits < 10) {
        FILE *out = fopen("dump.data", "a");
        fwrite(&ctx->uc_mcontext.gregs, sizeof(gregset_t), 1, out);
        int pad = 512 - sizeof(gregset_t);
        int cafe = 0xCAFECAFE;
        int i;
        for(i = 0; i < (pad / sizeof(cafe)); i++)
            fwrite(&cafe, sizeof(cafe), 1, out);
        fflush(out);
        fclose(out);
    }
    assert(cnt->hits < 10);
}

int install_sighand()
{
    int ret;
    struct sigaction sigact;

    sigact.sa_sigaction = signal_handler;
    sigact.sa_flags = SA_SIGINFO;
    ret = sigaction(SIGIO, &sigact, NULL);
    assert(ret == 0);
    return 0;
}

int open_counter(struct counter *counter)
{
    uint64_t val;
    int tid;
    int ret;
    int flags;
    int fd;
    struct sigaction sigact;

    tid = syscall(__NR_gettid);
    fd = sys_perf_event_open(&counter->args->attr, tid, -1, -1, 0);
    assert(fd > 0);

    // fasync setup
    struct f_owner_ex ex = {
        .type = F_OWNER_TID,
        .pid = tid,
    };
    ret = fcntl(fd, F_SETOWN_EX, &ex);
    assert(ret == 0);
    flags = fcntl(fd, F_GETFL);
    ret = fcntl(fd, F_SETFL, flags | FASYNC | O_ASYNC);
    assert(ret == 0);

    counter->fd = fd;
    counter->tid = tid;
    return 0;
}

void close_counter(struct counter *counter)
{
    close(counter->fd);
}

void *do_work(void *arg)
{
    struct counter *cnt = arg;

    rank = cnt->rank;
    open_counter(cnt);

    pthread_barrier_wait(&barrier);

    int i;
    for(i = 0; i < cnt->args->repeat; i++) {
        hog(cnt->args->iter_main);
    }

    close_counter(cnt);

    printf("%d %d\n", cnt->tid, cnt->hits);
    return NULL;
}

__attribute__((noreturn))
static void usage(void) {
    fprintf(stderr, "Usage: %s [OPTIONS] [COMMAND]\n", progname);
    fprintf(stderr, "\nOptions:\n\n");
    fprintf(stderr, "--thread N       number of threads to be spawned (default = 1)\n");
    fprintf(stderr, "--repeat         number of repetitions\n");
    fprintf(stderr, "--main           main work (iterations)\n");
    fprintf(stderr, "--overhead       overhead work (iterations)\n");
    fprintf(stderr, "--all            from 1 to n threads\n");
    fprintf(stderr, "--verbose        be verbose\n");
    fprintf(stderr, "--help           print this message and exit\n");
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

static void parse_opts(int argc, char **argv, struct args *args) {
    int opt;
    int idx;

    struct option options[] = {
            { "help",       0, 0, 'h' },
            { "repeat",     1, 0, 'r' },
            { "thread",     1, 0, 't' },
            { "main",       1, 0, 'm' },
            { "overhead",   1, 0, 'x' },
            { "disable",    1, 0, 'd' },
            { "period",     1, 0, 'p' },
            { "all",        1, 0, 'a' },
            { "verbose",    0, 0, 'v' },
            { 0, 0, 0, 0 }
    };

    args->nr_thread = DEFAULT_THREAD;
    args->repeat = DEFAULT_REPEAT;
    args->all = DEFAULT_ALL;
    args->iter_main = DEFAULT_REPEAT;
    args->iter_overhead = 1;
    args->disable = DEFAULT_DISABLE;
    args->period = DEFAULT_PERIOD;
    args->attr = def_attr;

    while ((opt = getopt_long(argc, argv, "hvr:t:a:m:x:d:p:", options, &idx)) != -1) {
        switch (opt) {
        case 'r':
            args->repeat = atoi(optarg);
            break;
        case 't':
            args->nr_thread = atoi(optarg);
            break;
        case 'a':
            args->all = 1;
            break;
        case 'm':
            args->iter_main = atol(optarg);
            break;
        case 'x':
            args->iter_overhead = atol(optarg);
            break;
        case 'd':
            args->disable = atoi(optarg);
            break;
        case 'p':
            args->period = atoi(optarg);
            break;
        case 'h':
            usage();
            break;
        default:
            usage();
            break;
        }
    }

    args->attr.disabled = args->disable;
    args->attr.sample_period = args->period;
    args->attr.freq = 0; // make sure freq is zero to select sample_period in the union
}

int main(int argc, char **argv) {
    int i;
    struct args args;
    parse_opts(argc, argv, &args);

    // disable true and false
    // explore number of threads
    // explore work inside handler

    /* init */
    pthread_barrier_init(&barrier, NULL, args.nr_thread);
    cnts = calloc(args.nr_thread, sizeof(struct counter));

    install_sighand();

    /* spawn */
    for (i = 0; i < args.nr_thread; i++) {
        cnts[i].args = &args;
        cnts[i].rank = i;
        pthread_create(&cnts[i].th, NULL, do_work, &cnts[i]);
    }

    /* join */
    for (i = 0; i < args.nr_thread; i++) {
        pthread_join(cnts[i].th, NULL);
    }

    /* terminate */
    free(cnts);
    return 0;
}