#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <odp_api.h>
#include <odp/helper/linux.h>

#define UNUSE(x)               (x = x)
#define MAX_WORKERS            8

#define SHM_PKT_POOL_SIZE      (512*2048)   /**< pkt pool size */
//#define SHM_PKT_POOL_SIZE      8192
#define SHM_PKT_POOL_BUF_SIZE  1856

/** Maximum number of pktio interfaces */
#define MAX_PKTIOS             8

/** Maximum number of pktio queues per interface */
#define MAX_QUEUES             32

#define MAX_PKT_BURST          32

/** Get rid of path in filename - only for unix-type paths using '/' */
#define NO_PATH(file_name) (strrchr((file_name), '/') ? \
        strrchr((file_name), '/') + 1 : (file_name))

struct test_timer {
    odp_timer_t tim;
    odp_event_t ev;
};

/**
 * Parsed command line application arguments
 */
typedef struct {
    int if_count;       /**< Number of interfaces to be used */
    char **if_names;    /**< Array of pointers to interface names */
    uint32_t num_workers;       /**< Number of worker threads */
    int cpu_count;
    char *if_str;       /**< Storage for interface names */
} appl_args_t;

/**
 * Statistics
 */
typedef union {
    struct {
        /** Number of forwarded packets */
        uint64_t packets;
        /** Packets dropped due to receive error */
        uint64_t rx_drops;
        /** Packets dropped due to transmit error */
        uint64_t tx_drops;
    } s;

    uint8_t padding[ODP_CACHE_LINE_SIZE];
} stats_t ODP_ALIGNED_CACHE;

/**
 * Thread specific arguments
 */
typedef struct thread_args_t {
    int thr_idx;
    stats_t *stats; /**< Pointer to per thread stats */
} thread_args_t;

typedef struct {
    /* Application (parsed) arguments */
    appl_args_t appl;
    /* Barrier for test synchronisation*/
    odp_barrier_t barrier;  
    /** Per thread packet stats */
    stats_t stats[MAX_WORKERS];
    /* Thread specific arguments */
    thread_args_t thread[MAX_WORKERS];
    /** Table of dst ports */
    int dst_port[MAX_PKTIOS];
    /* Table of pktio handles */
    struct {
        odp_pktio_t pktio;
        odp_pktout_queue_t pktout[MAX_QUEUES];
        odp_queue_t rx_q[MAX_QUEUES];
    } pktios[MAX_PKTIOS];
} args_t;

/** Global pointer to args */
static args_t *gbl_args;

static void usage(char *progname)
{
    printf("\nOpenDataPlane pktio test application.\n"
            "\nUsage: %s OPTIONS\n"
            "  -c <number> CPU count. %s will create worker thread on each CPU core.\n"
            "  -i, --interface Eth interfaces (comma-separated, no spaces)\n"
            "  -h, --help           Display help and exit.\n\n",
            NO_PATH(progname), NO_PATH(progname)
          );
}

static void gbl_args_init(args_t *args)
{
    int pktio, queue;

    memset(args, 0, sizeof(args_t));

    for (pktio = 0; pktio < MAX_PKTIOS; pktio++) {
        args->pktios[pktio].pktio = ODP_PKTIO_INVALID;

        for (queue = 0; queue < MAX_QUEUES; queue++)
            args->pktios[pktio].rx_q[queue] = ODP_QUEUE_INVALID;
    }
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
    char *token;
    size_t len;
    int i;

    static const char *shortopts =  "c:i:h";

    while (1) {
        opt = getopt(argc, argv, shortopts);

        if (opt == -1)
            break;	/* No more options */

        switch (opt) {
            case 'c':
                appl_args->cpu_count = atoi(optarg);
                break;
            case 'i':
                len = strlen(optarg);
                if (len == 0) {
                    usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                len += 1;	/* add room for '\0' */

                appl_args->if_str = malloc(len);
                if (appl_args->if_str == NULL) {
                    usage(argv[0]);
                    exit(EXIT_FAILURE);
                }

                /* count the number of tokens separated by ',' */
                strcpy(appl_args->if_str, optarg);
                for (token = strtok(appl_args->if_str, ","), i = 0;
                        token != NULL;
                        token = strtok(NULL, ","), i++)
                    ;

                appl_args->if_count = i;

                if (appl_args->if_count < 1 ||
                        appl_args->if_count > MAX_PKTIOS) {
                    usage(argv[0]);
                    exit(EXIT_FAILURE);
                }

                /* allocate storage for the if names */
                appl_args->if_names =
                    calloc(appl_args->if_count, sizeof(char *));

                /* store the if names (reset names string) */
                strcpy(appl_args->if_str, optarg);
                for (token = strtok(appl_args->if_str, ","), i = 0;
                        token != NULL; token = strtok(NULL, ","), i++) {
                    appl_args->if_names[i] = token;
                }
                break;
            case 'h':
                usage(argv[0]);
                exit(EXIT_SUCCESS);
                break;
            default:
                break;
        }
    }

    if (appl_args->if_count == 0) {
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }
}

static void print_info(char *progname, appl_args_t *appl_args)
{
    int i;

    printf("\n"
            "ODP system info\n"
            "---------------\n"
            "ODP API version: %s\n"
            "ODP impl name:   %s\n"
            "CPU model:       %s\n"
            "CPU freq (hz):   %lu\n"
            "Cache line size: %i\n"
            "CPU count:       %i\n"
            "\n",
            odp_version_api_str(), odp_version_impl_name(),
            odp_cpu_model_str(), odp_cpu_hz_max(),
            odp_sys_cache_line_size(), odp_cpu_count());

    printf("Running ODP appl: \"%s\"\n"
            "-----------------\n"
            "IF-count:        %i\n"
            "Using IFs:      ",
            progname, appl_args->if_count);
    for (i = 0; i < appl_args->if_count; ++i)
        printf(" %s", appl_args->if_names[i]);
    printf("\n"
            "Mode:            ");
    printf("PKTIN_SCHED_PARALLEL, ");
    printf("PKTOUT_DIRECT");

    printf("\n\n");
    fflush(NULL);
}

/**
 * Find the destination port for a given input port
 *
 * @param port  Input port index
 */
static int find_dest_port(int port)
{
    /* Even number of ports */
    if (gbl_args->appl.if_count % 2 == 0)
        return (port % 2 == 0) ? port + 1 : port - 1;

    /* Odd number of ports */
    if (port == gbl_args->appl.if_count - 1)
        return 0;
    else
        return port + 1;
}

/**
 * Lookup the destination port for a given packet
 *
 * @param pkt  ODP packet handle
 */
static inline int lookup_dest_port(odp_packet_t pkt)
{
    int i, src_idx;
    odp_pktio_t pktio_src;

    pktio_src = odp_packet_input(pkt);

    for (src_idx = -1, i = 0; gbl_args->pktios[i].pktio
            != ODP_PKTIO_INVALID; ++i)
        if (gbl_args->pktios[i].pktio == pktio_src)
            src_idx = i;

    if (src_idx == -1) {
        printf("Failed to determine pktio input\n");
        return -1;
    }

    return gbl_args->dst_port[src_idx];
}

static int create_pktio(const char *dev, int idx, odp_pool_t pool)
{
    odp_pktio_t pktio;
    odp_pktio_param_t pktio_param;
    odp_pktin_queue_param_t pktin_param;
    odp_pktout_queue_param_t pktout_param;
    odp_pktio_capability_t capa;
    int num_rx, num_tx;

    odp_pktio_param_init(&pktio_param);
    pktio_param.in_mode = ODP_PKTIN_MODE_SCHED;
    pktio_param.out_mode = ODP_PKTOUT_MODE_DIRECT;

    /* Open a packet IO instance */
    pktio = odp_pktio_open(dev, pool, &pktio_param);

    if (pktio == ODP_PKTIO_INVALID) {
        printf("Error: pktio create failed for %s\n", dev);
        exit(EXIT_FAILURE);
    }

    if (odp_pktio_capability(pktio, &capa)) {
        printf("Error: capability query failed %s\n", dev);
        exit(EXIT_FAILURE);
    }

    num_rx = gbl_args->appl.num_workers;
    if (num_rx > (int)capa.max_input_queues) {
        printf("Sharing %i input queues between %i workers\n",
                capa.max_input_queues, num_rx);
        num_rx  = capa.max_input_queues;
    }
    num_tx = 1;

    odp_pktin_queue_param_init(&pktin_param);
    odp_pktout_queue_param_init(&pktout_param);

    pktin_param.queue_param.sched.sync  = ODP_SCHED_SYNC_ORDERED;
    pktin_param.queue_param.type        = ODP_QUEUE_TYPE_SCHED;
    pktin_param.queue_param.sched.prio  = ODP_SCHED_PRIO_DEFAULT;
    pktin_param.queue_param.sched.group = ODP_SCHED_GROUP_ALL;
    pktin_param.num_queues  = num_rx;
    pktin_param.op_mode     = ODP_PKTIO_OP_MT;

    if (odp_pktin_queue_config(pktio, &pktin_param)) {
        printf("Error: pktin queue config failed for %s\n", dev);
        exit(EXIT_FAILURE);
    }

    pktout_param.op_mode = ODP_PKTIO_OP_MT;
    pktout_param.num_queues = num_tx;

    if (odp_pktout_queue_config(pktio, &pktout_param)) {
        printf("Error: pktout queue config failed for %s\n", dev);
        exit(EXIT_FAILURE);
    }

    if (odp_pktin_event_queue(pktio,
                gbl_args->pktios[idx].rx_q,
                num_rx) != num_rx) {
        printf("Error: pktin event queue query failed %s\n", dev);
        exit(EXIT_FAILURE);
    }

    if (odp_pktout_queue(pktio,
                gbl_args->pktios[idx].pktout,
                num_tx) != num_tx) {
        printf("Error: pktout event queue query failed %s\n", dev);
        exit(EXIT_FAILURE);
    }

    printf("  created pktio:%02lu, dev:%s, queue mode (ATOMIC queues)\n"
            "          default pktio%02lu\n",
            odp_pktio_to_u64(pktio), dev,
            odp_pktio_to_u64(pktio));

    gbl_args->pktios[idx].pktio        = pktio;

    return 0;
}

static int worker_thread(void *arg)
{
    uint64_t wait;
    int pkts;
    int thr = odp_thread_id();
    thread_args_t *thr_args = arg;
    stats_t *stats = thr_args->stats;
    //int cpu = odp_cpu_id();
    int i;
    int sent = 0;
    int dst_idx;
    int tx_drops;
    odp_pktout_queue_t pktout[MAX_PKTIOS];
    odp_event_t  ev_tbl[MAX_PKT_BURST] = {ODP_EVENT_INVALID};
    odp_packet_t pkt_tbl[MAX_PKT_BURST];

    memset(pktout, 0, sizeof(pktout));

    for (i = 0; i < gbl_args->appl.if_count; i++) {
        pktout[i] = gbl_args->pktios[i].pktout[0];
    }

    printf("[%02i] PKTIN_SCHED_ORDERED, PKTOUT_DIRECT\n", thr);

    odp_barrier_wait(&gbl_args->barrier);

    wait = odp_schedule_wait_time(ODP_TIME_MSEC_IN_NS * 1000);
    while(1) {
        pkts = odp_schedule_multi(NULL, wait, ev_tbl, MAX_PKT_BURST);
        if (pkts <= 0) 
            continue;

        //printf("  [CPU %i, thread %i] %i packets received\n", cpu, thr, pkts);
        for (i = 0; i < pkts; i++) {
            pkt_tbl[i] = odp_packet_from_event(ev_tbl[i]);
        }
        dst_idx = lookup_dest_port(pkt_tbl[0]);

        sent = odp_pktout_send(pktout[dst_idx], pkt_tbl, pkts);
        sent = odp_unlikely(sent < 0) ? 0 : sent;
        tx_drops = pkts - sent;
        if (odp_unlikely(tx_drops)) {
            stats->s.tx_drops += tx_drops;

            /* Drop rejected packets */
            for (i = sent; i < pkts; i++)
                odp_packet_free(pkt_tbl[i]);
        }

        stats->s.packets += pkts;
    }

    /* Make sure that latest stat writes are visible to other threads */
    odp_mb_full();

    return 0;
}

/**
 *  Print statistics
 *
 * @param num_workers Number of worker threads
 * @param thr_stats Pointer to stats storage
 *
 */
static int print_stats(int num_workers, stats_t *thr_stats)
{
    uint64_t pkts = 0;
    uint64_t  tx_drops;
    int i;

    /* Wait for all threads to be ready*/
    odp_barrier_wait(&gbl_args->barrier);

    while(1) {
        pkts = 0;
        tx_drops = 0;

        sleep(1);
        for (i = 0; i < num_workers; i++) {
            pkts += thr_stats[i].s.packets;
            tx_drops += thr_stats[i].s.tx_drops;
        }

        printf("%lu packets received, tx drops %lu\n", pkts, tx_drops);
    }

    return pkts > 100 ? 0 : -1;
}

int main(int argc, char *argv[])
{
    int i;
    int ret;
    odp_instance_t instance;
    int num_workers;
    odp_cpumask_t cpumask;
    char cpumaskstr[ODP_CPUMASK_STR_SIZE];
    odph_odpthread_t thread_tbl[MAX_WORKERS];
    int cpu;
    odp_pool_param_t params;
    odp_pool_t pool;
    odp_shm_t shm = ODP_SHM_INVALID;
    int rx_idx;
    stats_t *stats;

    UNUSE(argc);

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
    gbl_args_init(gbl_args);

    /* Parse and store the application arguments */
    parse_args(argc, argv, &gbl_args->appl);
    /* Print both system and application information */
    print_info(NO_PATH(argv[0]), &gbl_args->appl);

    /* Default to system CPU count unless user specified */
    num_workers = MAX_WORKERS;
    if (gbl_args->appl.cpu_count) {
        num_workers = gbl_args->appl.cpu_count;
    }
    num_workers = odp_cpumask_default_worker(&cpumask, num_workers);
    (void)odp_cpumask_to_str(&cpumask, cpumaskstr, sizeof(cpumaskstr));
    printf("num worker threads: %i\n", num_workers);
    printf("first CPU:          %i\n", odp_cpumask_first(&cpumask));
    printf("cpu mask:           %s\n", cpumaskstr);

    gbl_args->appl.num_workers = num_workers;
    for (i = 0; i < num_workers; i++)
        gbl_args->thread[i].thr_idx = i;

    odp_barrier_init(&gbl_args->barrier, num_workers + 1); 

    /* Create packet pool */
    odp_pool_param_init(&params);
    params.pkt.seg_len = SHM_PKT_POOL_BUF_SIZE;
    params.pkt.len     = SHM_PKT_POOL_BUF_SIZE;
    params.pkt.num     = gbl_args->appl.if_count * 
        SHM_PKT_POOL_SIZE/SHM_PKT_POOL_BUF_SIZE;
    params.type        = ODP_POOL_PACKET;

    pool = odp_pool_create("packet_pool", &params);

    if (pool == ODP_POOL_INVALID) {
        printf("Error: packet pool create failed.\n");
        exit(EXIT_FAILURE);
    }
    odp_pool_print(pool);

    /* initialize port forwarding table */
    for (rx_idx = 0; rx_idx < gbl_args->appl.if_count; rx_idx++) {
        gbl_args->dst_port[rx_idx] = find_dest_port(rx_idx);
        printf("@@@rx_idx is %i, dest_port is %i\n", rx_idx, gbl_args->dst_port[rx_idx]);
    }

    /* Create pktio */
    for (i = 0; i < gbl_args->appl.if_count; ++i) {
        const char *dev = gbl_args->appl.if_names[i];
        if (create_pktio(dev, i, pool)) {
            exit(EXIT_FAILURE);
        }
    }

    memset(thread_tbl, 0, sizeof(thread_tbl));
    stats = gbl_args->stats;

    /* Create worker threads */
    cpu = odp_cpumask_first(&cpumask);
    for (i = 0; i < num_workers; ++i) {
        odp_cpumask_t thd_mask;
        odph_odpthread_params_t thr_params;

        memset(&thr_params, 0, sizeof(thr_params));
        thr_params.start    = worker_thread;
        thr_params.arg      = &gbl_args->thread[i];
        thr_params.thr_type = ODP_THREAD_WORKER;
        thr_params.instance = instance;

        gbl_args->thread[i].stats = &stats[i];

        odp_cpumask_zero(&thd_mask);
        odp_cpumask_set(&thd_mask, cpu);
        odph_odpthreads_create(&thread_tbl[i], &thd_mask,
                &thr_params);
        cpu = odp_cpumask_next(&cpumask, cpu);
    }

    /* Start packet receive and transmit */
    for (i = 0; i < gbl_args->appl.if_count; ++i) {
        odp_pktio_t pktio;

        pktio = gbl_args->pktios[i].pktio;
        ret   = odp_pktio_start(pktio);
        if (ret) {
            printf("Error: unable to start %s\n", gbl_args->appl.if_names[i]);
            exit(EXIT_FAILURE);
        } else {
            printf("pktio started!\n");
        }
    }

    print_stats(num_workers, stats);

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
