#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <odp_api.h>
#include <odp/helper/linux.h>

#define UNUSE(x) (x = x)
#define MAX_WORKERS           8
#define MAX_TIMEOUT_WORKERS   4
#define NUM_TMOS              10000
#define WAIT_NUM              10    /**< Max tries to rx last tmo per worker */

/** Get rid of path in filename - only for unix-type paths using '/' */
#define NO_PATH(file_name) (strrchr((file_name), '/') ? \
        strrchr((file_name), '/') + 1 : (file_name))

struct test_timer {
    odp_timer_t tim;
    odp_event_t ev;
};

typedef struct {
    int cpu_count;
    int timeout_worker_count;
} appl_args_t;

typedef struct {
    appl_args_t appl;
    odp_barrier_t barrier;      /*< Barrier for test synchronisation*/
    odp_timer_pool_t tp;        /*< Timer pool handle*/
    odp_pool_t tmop;            /*< Timeout pool handle*/
    odp_atomic_u32_t remain;    /*< Number of timeouts to receive*/
    struct test_timer tt[256];  /*< Array of all timer helper structs*/
    uint32_t num_workers;       /**< Number of threads */
    odp_schedule_group_t schedule_group_timer;
} args_t;

/** Global pointer to args */
static args_t *gbl_args;

/**
 * Prinf usage information
 */
static void usage(char *progname)
{
    printf("\nOpenDataPlane L2 forwarding application.\n"
            "\nUsage: %s OPTIONS\n"
            "  -c <number> CPU count. %s will create worker thread on each CPU core.\n"
            "  -g <number> Worker thread number in timer schedule group. this should less than CPU count.\n"
            "  -h, --help           Display help and exit.\n\n",
            NO_PATH(progname), NO_PATH(progname)
          );
}

static void gbl_args_init(args_t *args)
{
    memset(args, 0, sizeof(args_t));
    args->tmop = ODP_POOL_INVALID;
    args->tp = ODP_TIMER_POOL_INVALID;
}

/**
 * Parse and store the command line arguments
 *
 * @param argc       argument count
 * @param argv[]     argument vector
 * @param appl_args  Store application arguments here
 */
static void parse_args(int argc, char *argv[], appl_args_t *appl_args)
{
    int opt;

    static const char *shortopts = "c:g:h";

    while (1) {
        opt = getopt(argc, argv, shortopts);

        if (opt == -1)
            break;  /* No more options */

        switch (opt) {
            case 'c':
                appl_args->cpu_count = atoi(optarg);
                break;
            case 'g':
                appl_args->timeout_worker_count = atoi(optarg);
                break;
            case 'h':
                usage(argv[0]);
                exit(EXIT_SUCCESS);
                break;
            default:
                break;
        }
    }
}

/** @private test timeout */
static void remove_prescheduled_events(void)
{
    odp_event_t ev;
    odp_queue_t queue;
    odp_schedule_pause();
    while ((ev = odp_schedule(&queue, ODP_SCHED_NO_WAIT)) !=
            ODP_EVENT_INVALID) {
        odp_event_free(ev);
    }
}

static int worker_thread(void *arg)
{
    odp_queue_t queue;
    struct test_timer *ttp;
    odp_timeout_t tmo;
    uint64_t tick;
    uint64_t period;
    uint64_t period_ns;
    args_t * gbls = arg;
    odp_thrmask_t mask;

    uint32_t num_workers = gbls->num_workers;
    int thr = odp_thread_id();
    int cpu = odp_cpu_id();

    if (thr <= gbls->appl.timeout_worker_count ) {
        odp_thrmask_zero(&mask);
        odp_thrmask_set(&mask, thr);

        if (odp_schedule_group_join(gbls->schedule_group_timer, &mask)) {
            printf("Join failed\n");
            return -1;
        }
    }

    queue = odp_queue_lookup("timer_queue");

    period_ns = 1000000 * ODP_TIME_USEC_IN_NS;
    period    = odp_timer_ns_to_tick(gbls->tp, period_ns);

    ttp = &gbls->tt[thr]; 
    ttp->tim = odp_timer_alloc(gbls->tp, queue, ttp);
    if (ttp->tim == ODP_TIMER_INVALID) {
        printf("Failed to allocate timer\n");
        return -1;
    }
    tmo = odp_timeout_alloc(gbls->tmop);
    if (tmo == ODP_TIMEOUT_INVALID) {
        printf("Failed to allocate timeout\n");
        return -1;
    }
    ttp->ev = odp_timeout_to_event(tmo);
    tick = odp_timer_current_tick(gbls->tp);

    while(1)
    {
        int wait = 0;
        odp_event_t ev;
        odp_timer_set_t rc;

        if (ttp) {
            tick += period;
            rc = odp_timer_set_abs(ttp->tim, tick, &ttp->ev);
            if (odp_unlikely(rc != ODP_TIMER_SUCCESS)) {
                printf("odp_timer_set_abs failed.\n");
                return -1;
            }
        }

        /* Get the next expired timeout.
         * We invoke the scheduler in a loop with a timeout because
         * we are not guaranteed to receive any more timeouts. The
         * scheduler isn't guaranteeing fairness when scheduling
         * buffers to threads.
         * Use 1.5 second timeout for scheduler */
        uint64_t sched_tmo =
            odp_schedule_wait_time(1500000000ULL);
        do {
            ev = odp_schedule(&queue, sched_tmo);
            /* Check if odp_schedule() timed out, possibly there
             *              * are no remaining timeouts to receive */
            if (++wait > WAIT_NUM &&
                    odp_atomic_load_u32(&gbls->remain) < num_workers)
                printf("At least one TMO was lost\n");
        } while (ev == ODP_EVENT_INVALID &&
                (int)odp_atomic_load_u32(&gbls->remain) > 0);

        if (ev == ODP_EVENT_INVALID)
            break; /* No more timeouts */
        if (odp_event_type(ev) != ODP_EVENT_TIMEOUT) {
            /* Not a default timeout event */
            printf("Unexpected event type (%u) received\n",
                    odp_event_type(ev));
        }
        odp_timeout_t tmo = odp_timeout_from_event(ev);
        tick = odp_timeout_tick(tmo);
        ttp = odp_timeout_user_ptr(tmo);
        ttp->ev = ev;
        if (!odp_timeout_fresh(tmo)) {
            /* Not the expected expiration tick, timer has
             *              * been reset or cancelled or freed */
            printf("Unexpected timeout received (timer %lu, tick %lu)\n", odp_timer_to_u64(ttp->tim), tick);
        }
        printf("  [CPU %i, thread %i] timeout, tick %lu\n", cpu, thr, tick);

        uint32_t rx_num = odp_atomic_fetch_dec_u32(&gbls->remain);

        if (!rx_num)
            printf("Unexpected timeout received (timer %lu, tick %lu)\n", odp_timer_to_u64(ttp->tim), tick);
        else if (rx_num > num_workers)
            continue;

        odp_event_free(ttp->ev);
        odp_timer_free(ttp->tim);
        ttp = NULL;
    }

    /* Remove any prescheduled events */
    remove_prescheduled_events();

    return 0;
}

int main(int argc, char *argv[])
{
    int i;
    odp_instance_t instance;
    int num_workers;
    odp_cpumask_t cpumask;
    char cpumaskstr[ODP_CPUMASK_STR_SIZE];
    odph_odpthread_t thread_tbl[MAX_WORKERS];
    int cpu;
    odp_timer_pool_param_t tparams;
    odp_pool_param_t params;
    odp_queue_param_t param;
    odp_queue_t queue;
    odp_shm_t shm = ODP_SHM_INVALID;
    odp_thrmask_t zero;

    UNUSE(argc);
    UNUSE(argv);

    /* Init ODP before calling anything else */
    if (odp_init_global(&instance, NULL, NULL)) {
        printf("Error: ODP global init failed.\n");
        exit(EXIT_FAILURE);
    }

    if (odp_init_local(instance, ODP_THREAD_CONTROL)) {
        printf("Error: ODP local init failed.\n");
        exit(EXIT_FAILURE);
    }

    /* Reserve memory for args from shared mem */
    shm = odp_shm_reserve("shm_args", sizeof(args_t),
            ODP_CACHE_LINE_SIZE, 0);
    gbl_args = odp_shm_addr(shm);

    if (gbl_args == NULL) {
        printf("Error: shared mem alloc failed.\n");
        exit(EXIT_FAILURE);
    }
    /* Init global arguments */
    gbl_args_init(gbl_args);

    /* Parse and store the application arguments */
    parse_args(argc, argv, &gbl_args->appl);

    /* Default to system CPU count unless user specified */
    num_workers = MAX_WORKERS;
    if (gbl_args->appl.cpu_count) {
        num_workers = gbl_args->appl.cpu_count;
    }
    if (gbl_args->appl.timeout_worker_count) {
        if (gbl_args->appl.timeout_worker_count > gbl_args->appl.cpu_count) {
            gbl_args->appl.timeout_worker_count = gbl_args->appl.cpu_count;
        }
    } else {
        gbl_args->appl.timeout_worker_count = MAX_TIMEOUT_WORKERS;
    }

    /* Get default worker cpumask */
    num_workers = odp_cpumask_default_worker(&cpumask, num_workers);
    (void)odp_cpumask_to_str(&cpumask, cpumaskstr, sizeof(cpumaskstr));
    printf("num worker threads: %i\n", num_workers);
    printf("num timeout worker threads: %i\n", gbl_args->appl.timeout_worker_count);
    printf("first CPU:          %i\n", odp_cpumask_first(&cpumask));
    printf("cpu mask:           %s\n", cpumaskstr);

    gbl_args->num_workers = num_workers;

    /* Initialize number of timeouts to receive */
    odp_atomic_init_u32(&gbl_args->remain, NUM_TMOS * num_workers);

    odp_barrier_init(&gbl_args->barrier, num_workers); 

    /* Create timeout pool */
    memset(&params, 0, sizeof(params));
    params.tmo.num = NUM_TMOS;
    params.type    = ODP_POOL_TIMEOUT;

    gbl_args->tmop = odp_pool_create("timeout_pool", &params);
    if (gbl_args->tmop == ODP_POOL_INVALID) {
        printf("Error: timeout pool create failed.\n");
        exit(EXIT_FAILURE);
    }

    /* Create timer pool */
    tparams.res_ns = 1 * ODP_TIME_MSEC_IN_NS;
    tparams.min_tmo = 1 * ODP_TIME_MSEC_IN_NS;
    tparams.max_tmo = 1000 * ODP_TIME_SEC_IN_NS;
    tparams.num_timers = num_workers; /* One timer per worker */
    tparams.priv = 0; /* Shared */
    tparams.clk_src = ODP_CLOCK_CPU;
    gbl_args->tp = odp_timer_pool_create("timer_pool", &tparams);
    if (gbl_args->tp == ODP_TIMER_POOL_INVALID) {
        printf("Timer pool create failed.\n");
        exit(EXIT_FAILURE);
    }
    odp_timer_pool_start();

    odp_thrmask_zero(&zero);
    gbl_args->schedule_group_timer = odp_schedule_group_create(
            "sche_group_timer", &zero);
    if (gbl_args->schedule_group_timer == ODP_SCHED_GROUP_INVALID) {
        printf("Group create failed\n");
        exit(EXIT_FAILURE);
    }

    /* Create a queue for timer test */
    odp_queue_param_init(&param);
    param.type        = ODP_QUEUE_TYPE_SCHED;
    param.sched.prio  = ODP_SCHED_PRIO_DEFAULT;
    param.sched.sync  = ODP_SCHED_SYNC_PARALLEL;
    //param.sched.group = ODP_SCHED_GROUP_ALL;
    param.sched.group = gbl_args->schedule_group_timer;

    queue = odp_queue_create("timer_queue", &param);
    if (queue == ODP_QUEUE_INVALID) {
        printf("Error: Timer queue create failed.\n");
        exit(EXIT_FAILURE);
    }

    /* Create worker threads */
    cpu = odp_cpumask_first(&cpumask);
    for (i = 0; i < num_workers; ++i) {
        odp_cpumask_t thd_mask;
        odph_odpthread_params_t thr_params;

        memset(&thr_params, 0, sizeof(thr_params));
        thr_params.start    = worker_thread;
        thr_params.arg      = gbl_args;
        thr_params.thr_type = ODP_THREAD_WORKER;
        thr_params.instance = instance;

        odp_cpumask_zero(&thd_mask);
        odp_cpumask_set(&thd_mask, cpu);
        odph_odpthreads_create(&thread_tbl[i], &thd_mask,
                &thr_params);
        cpu = odp_cpumask_next(&cpumask, cpu);
    }

    /* Master thread waits for other threads to exit */
    for (i = 0; i < num_workers; ++i)
        odph_odpthreads_join(&thread_tbl[i]);

    if (odp_term_local()) {
        printf("Error: term local\n");
        exit(EXIT_FAILURE);
    }

    if (odp_term_global(instance)) {
        printf("Error: term global\n");
        exit(EXIT_FAILURE);
    }

    printf("Exit!\n\n");
    return 0;
}
