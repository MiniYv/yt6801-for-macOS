/*
 * yt6801_ring_sim.c — offline verification of descriptor ring index logic.
 * Replays the index arithmetic from yt6801_datapath.cpp against a simulated
 * device and checks four invariants:
 *
 *   I1 never write a TX descriptor owned by hardware (OWN=1)
 *   I2 never write an RX descriptor owned by hardware (OWN=1)
 *   I3 every submitted TX packet is reclaimed exactly once
 *   I4 RX packet conservation: ring + staging always equals pool total
 *
 * Build: clang -O2 -Wall -o build/yt6801_ring_sim source/host/yt6801_ring_sim.c
 * Run:   build/yt6801_ring_sim
 * Exit code: 0 = all invariants hold
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DESC_WORDS   4
#define RING_DESC    8          /* 8 chosen deliberately: frequent wraparound exposes edge cases */
#define ROUNDS       200000

#define OWN          0x80000000u
#define BUF1V        (1u << 24)
#define FD_LD        0x30000000u

/* ============ Index arithmetic identical to the driver ============ */
static inline uint32_t ring_next(uint32_t i, uint32_t count)
{
    return (i + 1) & (count - 1);
}

static inline uint32_t tx_avail(uint32_t clean, uint32_t head)
{
    return (clean - head - 1) & (RING_DESC - 1);
}

/* ============ Fake hardware: independent TX / RX rings ============ */
typedef struct {
    uint32_t tx_desc[RING_DESC][DESC_WORDS];
    uint32_t rx_desc[RING_DESC][DESC_WORDS];
    uint32_t tx_cursor;      /* hardware next TX to send */
    uint32_t rx_cursor;      /* hardware next RX to fill */
} hw_t;

static int hw_tx_send_one(hw_t * hw)
{
    if (!(hw->tx_desc[hw->tx_cursor][3] & OWN)) {
        return 0;
    }
    hw->tx_desc[hw->tx_cursor][3] &= ~OWN;
    hw->tx_cursor = ring_next(hw->tx_cursor, RING_DESC);
    return 1;
}

static int hw_rx_fill_one(hw_t * hw)
{
    uint32_t i = hw->rx_cursor;
    if (!(hw->rx_desc[i][3] & OWN)) {
        return 0;
    }
    hw->rx_desc[i][3] &= ~OWN;               /* done writing, return ownership */
    hw->rx_desc[i][3] |= 64;                 /* write back length */
    hw->rx_cursor = ring_next(i, RING_DESC);
    return 1;
}

/* ============ Driver-side logic under test ============ */
typedef struct {
    hw_t     hw;
    uint32_t tx_head, tx_clean;
    uint32_t tx_pkt[RING_DESC];
    uint32_t rx_arm, rx_clean;
    uint32_t rx_pkt[RING_DESC];
    uint32_t rx_free[RING_DESC + 1];
    uint32_t rx_free_n;
    uint32_t rx_pool_total;

    uint32_t submitted, completed, delivered;
    uint32_t hist_tx[4096];
    uint32_t hist_rx[4096];
    int      violations;
} sim_t;

static void violation(sim_t * s, const char * what)
{
    s->violations++;
    if (s->violations <= 10) {
        printf("  [VIOLATION] %s (tx_head=%u tx_clean=%u rx_arm=%u rx_clean=%u)\n",
               what, s->tx_head, s->tx_clean, s->rx_arm, s->rx_clean);
    }
}

/* corresponds to tx_submit_packet */
static int sim_tx_submit(sim_t * s, uint32_t pkt)
{
    if (tx_avail(s->tx_clean, s->tx_head) == 0) {
        return 0;
    }
    uint32_t i = s->tx_head;

    /* I1 */
    if (s->hw.tx_desc[i][3] & OWN) {
        violation(s, "I1: overwrote TX descriptor with OWN=1");
        return 0;
    }
    if (s->tx_pkt[i] != 0) {
        violation(s, "I1: overwrote TX slot still holding a packet");
        return 0;
    }

    s->hw.tx_desc[i][0] = 0x1000 + pkt;
    s->hw.tx_desc[i][1] = 0;
    s->hw.tx_desc[i][2] = 64;
    s->hw.tx_desc[i][3] = FD_LD;
    s->hw.tx_desc[i][3] |= OWN;

    s->tx_pkt[i] = pkt;
    s->tx_head = ring_next(i, RING_DESC);
    s->submitted++;
    return 1;
}

/* corresponds to reclaimTx */
static void sim_reclaim_tx(sim_t * s)
{
    while (s->tx_clean != s->tx_head) {
        if (s->hw.tx_desc[s->tx_clean][3] & OWN) {
            break;
        }
        uint32_t pkt = s->tx_pkt[s->tx_clean];
        if (pkt == 0) {
            violation(s, "reclaimed a TX descriptor with no packet");
        } else {
            s->hist_tx[pkt & 4095]++;
            s->tx_pkt[s->tx_clean] = 0;
            s->completed++;
        }
        s->tx_clean = ring_next(s->tx_clean, RING_DESC);
    }
}

/* corresponds to rx_arm_descriptor + refillRx */
static void sim_refill_rx(sim_t * s)
{
    while (s->rx_free_n > 0) {
        uint32_t next = ring_next(s->rx_arm, RING_DESC);
        if (next == s->rx_clean) {
            break;
        }
        /* I2 */
        if (s->hw.rx_desc[s->rx_arm][3] & OWN) {
            violation(s, "I2: overwrote RX descriptor with OWN=1");
            break;
        }
        if (s->rx_pkt[s->rx_arm] != 0) {
            violation(s, "I2: overwrote RX slot still holding a packet");
            break;
        }
        uint32_t pkt = s->rx_free[--s->rx_free_n];
        s->hw.rx_desc[s->rx_arm][0] = 0x2000 + pkt;
        s->hw.rx_desc[s->rx_arm][1] = 0;
        s->hw.rx_desc[s->rx_arm][2] = 0;
        s->hw.rx_desc[s->rx_arm][3] = OWN | BUF1V;
        s->rx_pkt[s->rx_arm] = pkt;
        s->rx_arm = next;
    }
}

/* corresponds to serviceRx */
static void sim_service_rx(sim_t * s)
{
    uint32_t harvested = 0;

    while (s->rx_clean != s->rx_arm) {
        if (s->hw.rx_desc[s->rx_clean][3] & OWN) {
            break;
        }
        uint32_t pkt = s->rx_pkt[s->rx_clean];
        s->rx_pkt[s->rx_clean] = 0;

        if (pkt == 0) {
            violation(s, "harvested an RX descriptor with no packet");
        } else {
            s->hist_rx[pkt & 4095]++;
            s->delivered++;
            if (s->rx_free_n >= RING_DESC + 1) {
                violation(s, "RX staging overflow");
            } else {
                s->rx_free[s->rx_free_n++] = pkt;
            }
        }
        s->hw.rx_desc[s->rx_clean][3] = 0;
        s->rx_clean = ring_next(s->rx_clean, RING_DESC);
        harvested++;
    }

    if (harvested) {
        sim_refill_rx(s);
    }
}

/* I4: ring packets + staging packets == RX pool total at all times */
static void check_rx_conservation(sim_t * s, int when)
{
    uint32_t armed = 0;
    for (uint32_t i = 0; i < RING_DESC; i++) {
        if (s->rx_pkt[i] != 0) {
            armed++;
        }
    }
    if (armed + s->rx_free_n + s->rx_pool_total != s->rx_pool_total + armed + s->rx_free_n) {
        /* always true; prevents optimizing the loop above away */
    }
    if (armed + s->rx_free_n != s->rx_pool_total) {
        s->violations++;
        if (s->violations <= 10) {
            printf("  [VIOLATION] I4 RX packet conservation broken (check %d): ring %u + staging %u != total %u\n",
                   when, armed, s->rx_free_n, s->rx_pool_total);
        }
    }
}

int main(void)
{
    printf("YT6801 descriptor ring state machine verification\n");
    printf("  ring length %d, %d random operation rounds\n\n", RING_DESC, ROUNDS);

    sim_t * s = calloc(1, sizeof(sim_t));
    if (!s) {
        return 2;
    }

    /* Initial: pool holds RX packets 1..RING_DESC-1, all armed on the ring */
    s->rx_pool_total = RING_DESC - 1;
    for (uint32_t i = 1; i <= RING_DESC - 1; i++) {
        s->rx_free[s->rx_free_n++] = i;
    }
    sim_refill_rx(s);
    check_rx_conservation(s, 0);

    unsigned seed = 12345;
    uint32_t next_pkt = 100;

    for (int r = 0; r < ROUNDS; r++) {
        seed = seed * 1103515245u + 12345u;
        int op = (int)((seed >> 16) % 5);

        switch (op) {
        case 0:
            if (next_pkt < 4000) {
                sim_tx_submit(s, next_pkt++);
            }
            break;
        case 1: hw_tx_send_one(&s->hw);  break;
        case 2: hw_rx_fill_one(&s->hw);  break;
        case 3: sim_service_rx(s);       break;
        case 4: sim_reclaim_tx(s);       break;
        }
        if ((r & 0x3FF) == 0) {
            check_rx_conservation(s, r);
        }
    }

    /* Drain the hardware, then let the driver finish */
    for (int i = 0; i < RING_DESC * 8; i++) {
        hw_tx_send_one(&s->hw);
        sim_reclaim_tx(s);
        hw_rx_fill_one(&s->hw);
        sim_service_rx(s);
    }
    check_rx_conservation(s, -1);

    printf("  submitted %u, reclaimed %u, delivered %u frames\n",
           s->submitted, s->completed, s->delivered);

    /* I3: each packet reclaimed at most once */
    for (uint32_t p = 0; p < 4096; p++) {
        if (s->hist_tx[p] > 1) {
            printf("  [VIOLATION] packet %u reclaimed %u times\n", p, s->hist_tx[p]);
            s->violations++;
        }
    }
    if (s->completed > s->submitted) {
        printf("  [VIOLATION] reclaimed %u exceeds submitted %u\n", s->completed, s->submitted);
        s->violations++;
    }

    /* In-flight TX accounting must balance */
    uint32_t inflight = 0;
    for (uint32_t i = 0; i < RING_DESC; i++) {
        if (s->tx_pkt[i] != 0) {
            inflight++;
        }
    }
    if (s->completed + inflight != s->submitted) {
        printf("  [VIOLATION] accounting mismatch: completed %u + in-flight %u != submitted %u\n",
               s->completed, inflight, s->submitted);
        s->violations++;
    }

    /* TX ring should be fully drained at the end */
    if (s->tx_clean != s->tx_head) {
        printf("  [VIOLATION] TX ring not drained: clean=%u head=%u\n", s->tx_clean, s->tx_head);
        s->violations++;
    }

    printf("\n");
    if (s->violations == 0) {
        printf("Result: PASS  -- all of I1/I2/I3/I4 hold\n");
        free(s);
        return 0;
    }
    printf("Result: FAIL  -- %d violations\n", s->violations);
    free(s);
    return 1;
}
