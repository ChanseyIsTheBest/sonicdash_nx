/* sd_gc.c -- Boehm stop-the-world, on a platform that never delivers signals.
 *
 * THE WALL THIS EXISTS TO CLEAR
 * Boehm suspends every mutator thread by sending it a signal with
 * pthread_kill(). Each target's handler posts an acknowledgement semaphore and
 * parks in sigsuspend(); GC_stop_world waits on those acks. On Switch,
 * pthread_kill is a no-op and the handler never runs, so the ack never arrives
 * and the FIRST COLLECTION BLOCKS FOREVER. No error, no crash -- the game just
 * stops. It is the single most common way a Unity port dies at boot.
 *
 * WHY POSTING THE ACK IS NOT ENOUGH
 * The obvious fix -- have pthread_kill itself post the ack -- unblocks the
 * collector and is what the inherited substrate did. It is wrong, and the
 * lineage has the receipt: daggerfall_nx's audit found the "stopped" threads
 * kept running while the collector marked and swept, and the audio thread read
 * an object header with the mark bit set and branched through it.
 *
 * A thread that is not stopped is not stopped. So this bridge:
 *
 *   1. ACTUALLY suspends the thread (svcSetThreadActivity), and
 *   2. does the handler's OTHER job -- publishes the thread's stack pointer and
 *      registers into GC_threads[] so GC_push_all_stacks can scan them.
 *
 * Step 2 is the one it is tempting to skip, because skipping it still boots.
 * What it costs is a root set with holes: objects still referenced only from an
 * unpublished thread are invisible to the marker and get freed while live. That
 * surfaces much later, somewhere unrelated, as a container with a non-zero
 * count and a NULL element. It is not debuggable from where it lands.
 *
 * ORDERING RULES THAT ARE NOT NEGOTIABLE
 *   - LOG BEFORE PAUSING, NEVER AFTER. debugPrintf takes the stdio/heap lock,
 *     and the thread you just froze may be holding it. Logging after the pause
 *     deadlocks the collector against its own diagnostics.
 *   - POST THE ACK WHATEVER HAPPENS. If suspension fails, the collector must
 *     still be released. One unstopped thread is a correctness risk; a
 *     collector waiting forever is a dead game.
 *   - ON RESTART, RESUME FIRST AND UNCONDITIONALLY. The retry gate decides only
 *     whether the collector wants an ack, never whether a thread may run again.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <switch.h>

#include "sd_gc.h"
#include "diag.h"
#include "libc_shim.h"
#include "util.h"

static uintptr_t g_base;
static size_t    g_size;
static int       g_armed;          /* 1 usable, -1 disabled, 0 not armed,
                                     * 2 base recorded, verification deferred */

static int g_pause_try, g_pause_ok, g_pause_live;
static int g_published, g_unpublished;
static int g_layout_ok;

/* debugPrintf and sem_post_fake come from their real headers. An earlier
 * revision re-declared both here with the wrong signatures (void returns,
 * const char*), which conflicts the moment util.h is in scope. */

/* ---------------------------------------------------------------- guards */

static int check_guard_table(const SdGcGuard *tg, unsigned n);

static int check_guards(void)
{
    static const SdGcGuard sig[] = SD_GC_SIG_GUARDS_INIT;      /* the globals     */
    static const SdGcGuard thr[] = SD_GC_THREAD_GUARDS_INIT;   /* the thread table */
    return check_guard_table(sig, (unsigned)(sizeof sig / sizeof sig[0])) &&
           check_guard_table(thr, (unsigned)(sizeof thr / sizeof thr[0]));
}

static int check_guard_table(const SdGcGuard *tg, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint32_t got = *(volatile uint32_t *)(g_base + tg[i].off);
        if (got != tg[i].word) {
            debugPrintf("[gc] BRIDGE DISABLED: guard %u/%u at il2cpp+0x%x -- "
                        "expected %08x (%s), found %08x.\n"
                        "     These offsets are for a DIFFERENT build of "
                        "libil2cpp.so. Re-derive:\n"
                        "       python3 tools/derive_gc_bridge.py libil2cpp.so\n"
                        "       python3 tools/derive_gc_threads.py libil2cpp.so <suspend_sig_off>\n",
                        i + 1u, n, (unsigned)tg[i].off, (unsigned)tg[i].word,
                        tg[i].what, (unsigned)got);
            return 0;
        }
    }
    debugPrintf("[gc] bridge guards OK (%u words)\n", n);
    return 1;
}

/* Sanity-check the thread table before trusting it: walk the buckets and
 * confirm the chains are plausible pointers into the module. A table that
 * looks like garbage means the offset is wrong, and writing stack_ptr through
 * a wrong offset corrupts the heap silently. */
static int layout_check(void)
{
    uintptr_t *tab = (uintptr_t *)(g_base + SD_GC_THREADS_OFF);
    int chains = 0, sane = 0;

    for (int i = 0; i < SD_GC_THREADS_BUCKETS; i++) {
        uintptr_t p = tab[i];
        if (!p) continue;
        chains++;
        /* A GC_thread record lives in GC-managed memory, not necessarily in
         * the module -- so check alignment and readability rather than range. */
        if ((p & 7) == 0) sane++;
        int hops = 0;
        while (p && hops < 64) {
            p = *(volatile uintptr_t *)(p + SD_GC_THR_NEXT_OFF);
            hops++;
        }
        if (hops >= 64) {
            debugPrintf("[gc] layout check FAILED: bucket %d chain does not "
                        "terminate -- GC_THREADS_OFF is probably wrong\n", i);
            return 0;
        }
    }
    debugPrintf("[gc] layout check: %d non-empty buckets, %d aligned\n",
                chains, sane);
    return chains == 0 || sane == chains;
}

/* Is this address readable in the process RIGHT NOW? Asks the kernel instead of
 * finding out by faulting.
 *
 * WHY THIS EXISTS. The first hardware boot died here: sd_gc_arm() was called
 * straight after load_module(), before so_finalize(). At that point the module
 * lives only in its heap staging copy (load_base); load_virtbase is a bare
 * reservation, and svcMapProcessCodeMemory -- inside so_finalize() -- is what
 * puts the bytes there (and takes load_base away). The first guard read hit
 * unmapped memory: data abort, translation fault, far = base + 0x0ec240c.
 *
 * Every donor port avoids this the same way: record the base early if you
 * like, but only read through it after so_finalize(). main.c now arms after
 * finalisation; this check makes a future mis-ordering degrade to the donors'
 * lazy verification instead of a crash. */
static int readable(uintptr_t addr)
{
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return 0;
    return mi.type != MemType_Unmapped && (mi.perm & Perm_R);
}

static void verify_now(void)
{
    if (!check_guards()) { g_armed = -1; return; }
    g_layout_ok = layout_check();
    g_armed = 1;
    /* The shipped signal numbers are -1; Boehm fills them in during GC_init.
     * Read them here only to report, never to cache. */
    int s = *(volatile int *)(g_base + SD_GC_SUSPEND_SIG_OFF);
    int r = *(volatile int *)(g_base + SD_GC_RESTART_SIG_OFF);
    debugPrintf("[gc] armed at il2cpp 0x%lx (suspend=%d restart=%d%s)\n",
                (unsigned long)g_base, s, r,
                (s < 0 || r < 0) ? ", not yet assigned -- GC_init pending" : "");
}

void sd_gc_arm(uintptr_t il2cpp_base, size_t il2cpp_size)
{
    g_base = il2cpp_base;
    g_size = il2cpp_size;

    if (!g_base) {
        debugPrintf("[gc] NOT ARMED: il2cpp base is 0\n");
        g_armed = -1;
        return;
    }
    if (g_size && SD_GC_MAX_OFF > g_size) {
        debugPrintf("[gc] DISABLED: offsets exceed module size "
                    "(max 0x%x > 0x%zx)\n", (unsigned)SD_GC_MAX_OFF, g_size);
        g_armed = -1;
        return;
    }
    /* Probe the module's first page and the GC_threads table (.bss): if
     * either is unreadable the image is not mapped yet. */
    if (!readable(g_base) || !readable(g_base + SD_GC_THREADS_OFF)) {
        debugPrintf("[gc] base 0x%lx recorded but NOT MAPPED yet (sd_gc_arm ran before "
                    "so_finalize?) -- guards deferred to the first pthread_kill, "
                    "as battd_nx/clayjam_nx do\n", (unsigned long)g_base);
        g_armed = 2;
        return;
    }
    verify_now();
}

/* ------------------------------------------------- publish a stopped thread */

static int publish_stopped(pthread_t t, const ThreadContext *ctx)
{
    const uint64_t want = (uint64_t)(uintptr_t)t;
    uintptr_t *tab = (uintptr_t *)(g_base + SD_GC_THREADS_OFF);

    for (int i = 0; i < SD_GC_THREADS_BUCKETS; i++) {
        for (uintptr_t p = tab[i]; p;
             p = *(volatile uintptr_t *)(p + SD_GC_THR_NEXT_OFF)) {

            if (*(volatile uint64_t *)(p + SD_GC_THR_ID_OFF) != want)
                continue;

            /* Park the register file just below the thread's own sp and point
             * stop_info.stack_ptr at it. GC_push_all_stacks then scans from
             * there upward, so the registers are treated as roots exactly as
             * the real handler's setjmp buffer would have been. */
            const uint64_t sp = ctx->sp;
            uint64_t *save = (uint64_t *)(uintptr_t)
                             ((sp - SD_GC_REGSAVE_BYTES) & ~(uint64_t)0xF);

            for (int k = 0; k < 29; k++) save[k] = ctx->cpu_gprs[k].x;
            save[29] = ctx->fp;
            save[30] = ctx->lr;
            save[31] = sp;
            save[32] = ctx->pc.x;

            *(volatile uintptr_t *)(p + SD_GC_THR_STACKPTR_OFF) = (uintptr_t)save;
            *(volatile uint64_t *)(p + SD_GC_THR_LASTSTOP_OFF) =
                *(volatile uint64_t *)(g_base + SD_GC_STOP_COUNT_OFF);
            return 1;
        }
    }
    return 0;   /* thread not in the collector's table -- nothing to publish */
}

/* ------------------------------------------------------------- the bridge */

/* ---- stall evidence (lock-free) ---------------------------------------------
 * A freeze during stop-the-world showed four threads "held by GC" and every
 * other thread parked, but not WHICH thread was the collector -- so not where
 * it was stuck. This records the collector and every suspend/resume without
 * taking a lock or logging (frozen threads may own any lock); the watchdog
 * prints it during a stall. */
static volatile uint64_t g_coll_tid;          /* collector of the round in progress; 0 = none */
static volatile uint64_t g_round_tick;
static volatile unsigned g_round_no;
#define GC_EV_N 64
static struct { uint64_t tick; uintptr_t pth; int op, pr; } g_ev[GC_EV_N];
static volatile unsigned g_ev_n;
static void gc_ev(int op, uintptr_t pth, int pr) {
  unsigned i = __atomic_fetch_add(&g_ev_n, 1, __ATOMIC_RELAXED) % GC_EV_N;
  g_ev[i].tick = armGetSystemTick(); g_ev[i].pth = pth; g_ev[i].op = op; g_ev[i].pr = pr;
}
int sd_gc_stall_info(uint64_t *collector_tid, double *secs, unsigned *round, int *paused) {
  const uint64_t tid = g_coll_tid;
  if (!tid) return 0;
  *collector_tid = tid; *round = g_round_no; *paused = g_pause_live;
  *secs = (double)(armGetSystemTick() - g_round_tick) / (double)armGetSystemTickFreq();
  return 1;
}
int sd_gc_event(unsigned back, int *op, uintptr_t *pth, int *pr, double *ago) {
  const unsigned n = g_ev_n;
  if (back >= n || back >= GC_EV_N) return 0;
  const unsigned i = (n - 1 - back) % GC_EV_N;
  *op = g_ev[i].op; *pth = g_ev[i].pth; *pr = g_ev[i].pr;
  *ago = (double)(armGetSystemTick() - g_ev[i].tick) / (double)armGetSystemTickFreq();
  return 1;
}

int sd_gc_pthread_kill(void *pthread_target, int sig)
{
    if (g_armed == 0) {
        static int warned;
        if (!warned) {
            warned = 1;
            debugPrintf("[gc] pthread_kill(sig=%d) before sd_gc_arm() -- "
                        "main.c must arm the bridge right after loading "
                        "libil2cpp, or the first collection hangs\n", sig);
        }
        return 0;
    }
    if (g_armed == 2) {
        /* Verification was deferred at arm time. By the first pthread_kill
         * the collector is running, so the module is certainly mapped; the
         * check stays anyway -- it costs one syscall, once. Logging here is
         * safe: nothing has been paused yet. */
        if (readable(g_base) && readable(g_base + SD_GC_THREADS_OFF)) verify_now();
        else { debugPrintf("[gc] DISABLED: module still unmapped at first pthread_kill\n"); g_armed = -1; }
    }
    if (g_armed < 0 || !sig) return 0;

    pthread_t t = (pthread_t)pthread_target;
    const int suspend_sig = *(volatile int *)(g_base + SD_GC_SUSPEND_SIG_OFF);
    const int restart_sig = *(volatile int *)(g_base + SD_GC_RESTART_SIG_OFF);
    void **ack_sem = (void **)(g_base + SD_GC_ACK_SEM_OFF);

    if (sig == suspend_sig) {
        /* Log FIRST -- see the ordering rules at the top of this file. */
        static int logged;
        if (!logged) {
            logged = 1;
            debugPrintf("[gc] stop-world: suspending for real, ack via "
                        "il2cpp+0x%x\n", (unsigned)SD_GC_ACK_SEM_OFF);
        }

        if (g_pause_live == 0) {                 /* first suspend of a round */
            uint64_t me = 0; svcGetThreadId(&me, CUR_THREAD_HANDLE);
            g_round_tick = armGetSystemTick(); g_round_no++; g_coll_tid = me;
        }
        ThreadContext ctx;
        int pr = diag_pause_pthread_ctx((void *)t, &ctx);   /* 1=ctx 2=no ctx 0=no */
        gc_ev(1, (uintptr_t)t, pr);
        g_pause_try++;
        if (pr) { g_pause_ok++; g_pause_live++; }

        if (pr == 1 && g_layout_ok && publish_stopped(t, &ctx)) g_published++;
        else                                                    g_unpublished++;

        sem_post_fake(ack_sem);      /* unconditional: never strand the collector */
        return 0;
    }

    if (sig == restart_sig) {
        if (diag_resume_pthread((void *)t) && g_pause_live) g_pause_live--;
        gc_ev(2, (uintptr_t)t, 0);
        if (g_pause_live == 0) g_coll_tid = 0;   /* round over */

        /* Report every round that leaves a stack unpublished, not just the
         * first. The first round is two threads and always clean; by the time
         * Addressables are streaming there are thirty, and a miss there is the
         * one that frees a live object. */
        static int logged_first;
        static unsigned round, miss_rounds;
        static int last_pub, last_unpub;
        if (g_pause_live == 0) {
            const int pub = g_published - last_pub;
            const int unpub = g_unpublished - last_unpub;
            last_pub = g_published; last_unpub = g_unpublished;
            round++;
            if (!logged_first) {
                logged_first = 1;
                debugPrintf("[gc] start-world: first round paused %d of %d, "
                            "published %d stacks (%d not)\n",
                            g_pause_ok, g_pause_try, pub, unpub);
            } else if (unpub > 0 && miss_rounds < 12) {
                miss_rounds++;
                debugPrintf("[gc] ROUND %u MISSED %d of %d stacks -- those "
                            "threads' roots were invisible to the collector, "
                            "so anything only they referenced could be freed "
                            "while still live\n", round, unpub, pub + unpub);
            }
        }

        /* The retry gate decides whether an ack is wanted. It never decides
         * whether the thread resumes -- that already happened above. */
        if (*(volatile int *)(g_base + SD_GC_START_ACK_OFF))
            sem_post_fake(ack_sem);
        return 0;
    }

    return 0;   /* any other signal: genuinely a no-op */
}

void sd_gc_report(void)
{
    if (g_armed <= 0) {
        debugPrintf("[gc] bridge was %s\n",
                    g_armed < 0 ? "DISABLED" : "never armed");
        return;
    }
    debugPrintf("[gc] totals: paused %d/%d, published %d, unpublished %d\n",
                g_pause_ok, g_pause_try, g_published, g_unpublished);
    if (g_unpublished)
        debugPrintf("[gc] %d unpublished stacks over the session -- if you saw "
                    "container corruption, this is where to look first\n",
                    g_unpublished);
}
