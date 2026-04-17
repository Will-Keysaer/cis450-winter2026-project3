/*
 * tc.c
 * P3: traffic controller
 * Last modified 4/17/2026
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <assert.h>

/* ------------------------------------------------------------------
 * clock
 * ------------------------------------------------------------------ */

 // Global start time for elapsed time calculations
static struct timeval t0;

static double elapsed(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - t0.tv_sec) + (now.tv_usec - t0.tv_usec) / 1e6;
}

/*
 * Sleep for `seconds` using nanosleep.
 * Retries automatically if interrupted by a signal.
 */
static void timed_sleep(double seconds) {
    struct timespec req;
    req.tv_sec  = (time_t)seconds;
    req.tv_nsec = (long)((seconds - req.tv_sec) * 1e9);
    while (nanosleep(&req, &req) == -1) { /* retry on EINTR */ }
}

/* ------------------------------------------------------------------
 * Direction encoding
 * ------------------------------------------------------------------ */

typedef struct {
    char from;
    char to;
} route_t;

enum { DIR_N = 0, DIR_S = 1, DIR_E = 2, DIR_W = 3, DIR_COUNT = 4 };

//Maps direction symbol (^, v, >, <) to lane index
static int lane_of(char sym) {
    if (sym == '^') return DIR_N;
    if (sym == 'v') return DIR_S;
    if (sym == '>') return DIR_E;
    if (sym == '<') return DIR_W;
    return -1;
}

//Determines turn type: 'L' (left), 'R' (right), 'S' (straight)
static char maneuver(char from, char to) {
    static const char cw[] = { '^', '>', 'v', '<' };
    int a = -1, b = -1;
    for (int k = 0; k < 4; k++) {
        if (cw[k] == from) a = k;
        if (cw[k] == to)   b = k;
    }
    int steps = (b - a + 4) % 4;
    if (steps == 1) return 'R';
    if (steps == 3) return 'L';
    return 'S';
}

static double travel_time(char from, char to) {
    switch (maneuver(from, to)) {
        case 'L': return 5.0;
        case 'R': return 3.0;
        default:  return 4.0;
    }
}

/* ------------------------------------------------------------------
 * Intersection quadrant zones
 *
 * Zones use a readers-writer scheme: same-direction cars share zones
 * freely; different-direction cars require exclusive access.
 * ------------------------------------------------------------------ */

 // Quadrant constants: NW=0, NE=1, SW=2, SE=3
enum { Q_NW = 0, Q_NE = 1, Q_SW = 2, Q_SE = 3, Q_COUNT = 4 };

// Zone structure with readers-writer lock for concurrent access control
typedef struct {
    pthread_mutex_t  guard;
    sem_t            exclusive;
    int              reader_count;
    int              owner;        
} zone_t;

static zone_t zones[Q_COUNT];

static void zones_init(void) {
    for (int q = 0; q < Q_COUNT; q++) {
        pthread_mutex_init(&zones[q].guard, NULL);
        sem_init(&zones[q].exclusive, 0, 1);
        zones[q].reader_count = 0;
        zones[q].owner        = -1;
    }
}

static int route_zones(char from, char to) {
    char m = maneuver(from, to);
    switch (from) {
        case '^':
            if (m == 'R') return (1<<Q_SE);
            if (m == 'S') return (1<<Q_SE)|(1<<Q_NE);
            /* L */        return (1<<Q_SE)|(1<<Q_NE)|(1<<Q_NW);
        case 'v':
            if (m == 'R') return (1<<Q_NW);
            if (m == 'S') return (1<<Q_NW)|(1<<Q_SW);
            /* L */        return (1<<Q_NW)|(1<<Q_SW)|(1<<Q_SE);
        case '>':
            if (m == 'R') return (1<<Q_SW);
            if (m == 'S') return (1<<Q_SW)|(1<<Q_SE);
            /* L */        return (1<<Q_SW)|(1<<Q_NW)|(1<<Q_NE);
        case '<':
            if (m == 'R') return (1<<Q_NE);
            if (m == 'S') return (1<<Q_NE)|(1<<Q_NW);
            /* L */        return (1<<Q_NE)|(1<<Q_SE)|(1<<Q_SW);
    }
    return 0;
}

// Acquires lock on a single zone using readers-writer pattern
static void lock_zone(int q, int lane) {
    pthread_mutex_lock(&zones[q].guard);
    if (zones[q].owner == lane) {
        zones[q].reader_count++;
        pthread_mutex_unlock(&zones[q].guard);
    } else {
        pthread_mutex_unlock(&zones[q].guard);
        sem_wait(&zones[q].exclusive);
        pthread_mutex_lock(&zones[q].guard);
        zones[q].owner        = lane;
        zones[q].reader_count = 1;
        pthread_mutex_unlock(&zones[q].guard);
    }
}

// Releases lock
static void unlock_zone(int q) {
    pthread_mutex_lock(&zones[q].guard);
    zones[q].reader_count--;
    if (zones[q].reader_count == 0) {
        zones[q].owner = -1;
        pthread_mutex_unlock(&zones[q].guard);
        sem_post(&zones[q].exclusive);
    } else {
        pthread_mutex_unlock(&zones[q].guard);
    }
}

// lock for all zones
static void lock_all_zones(int zmask, int lane) {
    for (int q = 0; q < Q_COUNT; q++)
        if (zmask & (1 << q)) lock_zone(q, lane);
}

// release for all zones
static void unlock_all_zones(int zmask) {
    for (int q = 0; q < Q_COUNT; q++)
        if (zmask & (1 << q)) unlock_zone(q);
}

/* ------------------------------------------------------------------
 * Per-car head-of-line queue
 *
 *
 * The first car to enqueue in an empty lane immediately posts itself
 * (no one ahead to wait for).
 * ------------------------------------------------------------------ */

#define MAX_QUEUE 100  

static sem_t           *lane_queue[DIR_COUNT][MAX_QUEUE];
static int              lane_head[DIR_COUNT];
static int              lane_tail[DIR_COUNT];
static pthread_mutex_t  lane_mutex[DIR_COUNT];

static void lane_queues_init(void) {
    for (int d = 0; d < DIR_COUNT; d++) {
        pthread_mutex_init(&lane_mutex[d], NULL);
        lane_head[d] = 0;
        lane_tail[d] = 0;
    }
}

/*
 * Enqueue this car's semaphore in its lane and block until
 * the car ahead posts it.
 */
static void lane_enqueue(int d, sem_t *car_sem) {
    pthread_mutex_lock(&lane_mutex[d]);

    int pos = lane_tail[d];
    lane_queue[d][pos] = car_sem;
    lane_tail[d] = (pos + 1) % MAX_QUEUE;

    int is_first = (lane_head[d] == pos);
    pthread_mutex_unlock(&lane_mutex[d]);

    if (is_first) {
        // no one ahead -- post ourselves to proceed immediately
        sem_post(car_sem);
    }

    // block until the car ahead of us explicitly wakes us
    sem_wait(car_sem);
}

/*
 * Called when a car leaves the stop line.
 * Advances the head pointer and posts the next car's semaphore if
 * one is waiting.
 */
static void lane_advance(int d) {
    pthread_mutex_lock(&lane_mutex[d]);

    lane_head[d] = (lane_head[d] + 1) % MAX_QUEUE;

    int next_waiting = (lane_head[d] != lane_tail[d]);
    sem_t *next_sem  = next_waiting ? lane_queue[d][lane_head[d]] : NULL;

    pthread_mutex_unlock(&lane_mutex[d]);

    if (next_sem) sem_post(next_sem);
}

/* ------------------------------------------------------------------
 * Arrival-order priority
 * ------------------------------------------------------------------ */

static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static int             next_seq = 0;
static int             lane_seq[DIR_COUNT];   

/* ------------------------------------------------------------------
 * Console output
 * ------------------------------------------------------------------ */

 // Serialized logging
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

static void print_event(int id, route_t r, const char *phase) {
    pthread_mutex_lock(&print_lock);
    double t = (int)(elapsed() * 10 + 0.5) / 10.0;
    printf("Time %.1f: Car %d (%c %c) %s\n",
           t, id, r.from, r.to, phase);
    fflush(stdout);
    pthread_mutex_unlock(&print_lock);
}

/* ------------------------------------------------------------------
 * The three intersection phases
 * ------------------------------------------------------------------ */

static void ArriveIntersection(int id, route_t r, sem_t *car_sem) {
    int lane = lane_of(r.from);
    assert(lane >= 0);

    print_event(id, r, "arriving");

    //Mandatory stop
    timed_sleep(2.0);

    // Join the lane queue
    lane_enqueue(lane, car_sem);

    //Record arrival order
    pthread_mutex_lock(&seq_lock);
    int seq = next_seq++;
    lane_seq[lane] = seq;
    pthread_mutex_unlock(&seq_lock);

    // Yield to earlier arrivals in other lanes
    int waiting;
    do {
        waiting = 0;
        pthread_mutex_lock(&seq_lock);
        for (int d = 0; d < DIR_COUNT; d++) {
            if (d == lane) continue;
            if (lane_seq[d] != -1 && lane_seq[d] < seq) {
                waiting = 1;
                break;
            }
        }
        pthread_mutex_unlock(&seq_lock);
        if (waiting) usleep(10000);
    } while (waiting);
}

// Lock the needed quadrants and let the next car in lane proceed
static void CrossIntersection(int id, route_t r) {
    int    lane  = lane_of(r.from);
    int    zmask = route_zones(r.from, r.to);
    double dur   = travel_time(r.from, r.to);

    lock_all_zones(zmask, lane);

    pthread_mutex_lock(&seq_lock);
    lane_seq[lane] = -1;
    pthread_mutex_unlock(&seq_lock);

    lane_advance(lane);

    print_event(id, r, "crossing");

    timed_sleep(dur);
}

// Release quadrants and log exit
static void ExitIntersection(int id, route_t r) {
    unlock_all_zones(route_zones(r.from, r.to));
    print_event(id, r, "exiting");
}


typedef struct {
    int     id;
    double  arrives_at;
    route_t route;
} vehicle_t;

// Each car uses a private semaphore for lane queueing
static void *run_vehicle(void *arg) {
    vehicle_t *v = (vehicle_t *)arg;

    sem_t car_sem;
    sem_init(&car_sem, 0, 0);

    // Wait until the scheduled arrival time
    double wait = v->arrives_at - elapsed();
    if (wait > 0.0) timed_sleep(wait);

    ArriveIntersection(v->id, v->route, &car_sem);
    CrossIntersection (v->id, v->route);
    ExitIntersection  (v->id, v->route);

    sem_destroy(&car_sem);
    return NULL;
}


/* ------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------ */

int main(void) {
    gettimeofday(&t0, NULL);

    zones_init();
    lane_queues_init();

    for (int d = 0; d < DIR_COUNT; d++)
        lane_seq[d] = -1;

    vehicle_t car_pool[] = {
        { .id = 1, .arrives_at = 1.1, .route = { '^', '^' } },
        { .id = 2, .arrives_at = 2.2, .route = { '^', '^' } },
        { .id = 3, .arrives_at = 3.3, .route = { '^', '<' } },
        { .id = 4, .arrives_at = 4.4, .route = { 'v', 'v' } },
        { .id = 5, .arrives_at = 5.5, .route = { 'v', '>' } },
        { .id = 6, .arrives_at = 6.6, .route = { '^', '^' } },
        { .id = 7, .arrives_at = 7.7, .route = { '>', '^' } },
        { .id = 8, .arrives_at = 8.8, .route = { '<', '^' } },
    };

    int n = (int)(sizeof(car_pool) / sizeof(car_pool[0]));
    pthread_t *threads = (pthread_t *)malloc(n * sizeof(pthread_t));

    for (int i = 0; i < n; i++)
        pthread_create(&threads[i], NULL, (void *)run_vehicle, &car_pool[i]);

    for (int i = 0; i < n; i++)
        pthread_join(threads[i], NULL);

    free(threads);
    return 0;
}