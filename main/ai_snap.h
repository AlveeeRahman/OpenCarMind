/* ai_snap: a single-writer seqlock for the AI service layer (EdgeOS, ESP-Mosaico). Header only, no IDF: host-tested.
 *
 * One task writes a struct, any number of tasks read it, and nobody ever waits. The writer makes the sequence odd,
 * writes the data, makes it even again. A reader copies the data between two reads of the sequence and keeps the copy
 * only when both reads are equal and even, so a copy torn by a write in progress is thrown away (Lamport 1977,
 * "Concurrent reading and writing"; the fences as in Boehm 2012, "Can seqlocks get along with programming language
 * memory models?"). A reader gives up after AI_SEQ_TRIES attempts and keeps its previous copy: on one core a reader of
 * higher priority that preempted the writer mid-write would otherwise spin forever, because the writer cannot run.
 * So a read is wait-free and bounded, and a stale value is the worst case. No lock, no priority inversion, no deadlock.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define AI_SEQ_TRIES 4

typedef struct { uint32_t seq; } ai_seq_t;

static inline void ai_seq_write_begin(ai_seq_t *s)
{
    uint32_t q = __atomic_load_n(&s->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s->seq, q + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);            /* the odd sequence is visible before any data store */
}

static inline void ai_seq_write_end(ai_seq_t *s)
{
    uint32_t q = __atomic_load_n(&s->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s->seq, q + 1, __ATOMIC_RELEASE);  /* the data is visible before the even sequence */
}

/* copies n bytes from src to dst only when the copy is consistent; false: dst unchanged (keep the last good copy) */
static inline bool ai_seq_read(const ai_seq_t *s, void *dst, const void *src, void *tmp, size_t n)
{
    for (int i = 0; i < AI_SEQ_TRIES; i++) {
        uint32_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (a & 1u) continue;
        memcpy(tmp, src, n);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);        /* the data loads complete before the second sequence load */
        uint32_t b = __atomic_load_n(&s->seq, __ATOMIC_RELAXED);
        if (a == b) { memcpy(dst, tmp, n); return true; }
    }
    return false;
}

/* typed helpers: AI_SEQ_PUT(&seq, &shared, &value), AI_SEQ_GET(&seq, &out, &shared) */
#define AI_SEQ_PUT(sq, shared, val) do { ai_seq_write_begin(sq); memcpy((shared), (val), sizeof *(shared)); ai_seq_write_end(sq); } while (0)
#define AI_SEQ_GET(sq, out, shared) __extension__({ __typeof__(*(shared)) ai_tmp_; ai_seq_read((sq), (out), (shared), &ai_tmp_, sizeof ai_tmp_); })

/* A single-producer single-consumer ring of fixed-size text lines (the battery log rows). The producer never waits: a
   full ring drops the line and counts it. head is written only by the producer, tail only by the consumer. */
#define AI_RING_SLOTS 8
#define AI_RING_LINE  128
typedef struct {
    uint32_t head, tail, dropped;
    char     line[AI_RING_SLOTS][AI_RING_LINE];
} ai_ring_t;

static inline bool ai_ring_push(ai_ring_t *r, const char *text)
{
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_RELAXED), t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if (h - t >= AI_RING_SLOTS) { __atomic_fetch_add(&r->dropped, 1, __ATOMIC_RELAXED); return false; }
    char *d = r->line[h % AI_RING_SLOTS];
    size_t n = strlen(text);
    if (n >= AI_RING_LINE) n = AI_RING_LINE - 1;
    memcpy(d, text, n);
    d[n] = 0;
    __atomic_store_n(&r->head, h + 1, __ATOMIC_RELEASE);
    return true;
}

/* the oldest line or NULL; ai_ring_pop() after the consumer is done with it */
static inline const char *ai_ring_peek(ai_ring_t *r)
{
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_RELAXED), h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    return h == t ? NULL : r->line[t % AI_RING_SLOTS];
}

static inline void ai_ring_pop(ai_ring_t *r)
{
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    __atomic_store_n(&r->tail, t + 1, __ATOMIC_RELEASE);
}

static inline uint32_t ai_ring_count(ai_ring_t *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}
