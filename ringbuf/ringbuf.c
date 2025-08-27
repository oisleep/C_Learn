#include "ringbuf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --- 工具：把任意逻辑下标映射到真实data下标 --- */
static inline size_t rb_mod(size_t x, size_t cap) {
    return (cap == 0) ? 0 : (x % cap);
}

/* 从 head 出发的第 logical_index 个字节（不越界时使用） */
static inline uint8_t rb_at(const RingBuf *rb, size_t logical_index) {
    size_t idx = rb_mod(rb->head + logical_index, rb->cap);
    return rb->data[idx];
}

bool rb_init(RingBuf *rb, size_t capacity) {
    if (!rb || capacity == 0) return false;
    rb->data = (uint8_t*)malloc(capacity);
    if (!rb->data) return false;
    rb->cap  = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->size = 0;
    return true;
}

void rb_free(RingBuf *rb) {
    if (!rb) return;
    if (rb->data) {
        free(rb->data);
        rb->data = NULL;
    }
    rb->cap = rb->head = rb->tail = rb->size = 0;
}

void rb_clear(RingBuf *rb) {
    if (!rb || !rb->data) return;
    rb->head = rb->tail = rb->size = 0;
}

size_t rb_capacity(const RingBuf *rb) { return rb ? rb->cap  : 0; }
size_t rb_size    (const RingBuf *rb) { return rb ? rb->size : 0; }
size_t rb_free_space(const RingBuf *rb) {
    return rb ? (rb->cap - rb->size) : 0;
}

size_t rb_push(RingBuf *rb, const void *src, size_t n) {
    if (!rb || !rb->data || !src || n == 0) return 0;

    size_t free_bytes = rb_free_space(rb);
    if (free_bytes == 0) return 0;
    if (n > free_bytes) n = free_bytes;

    const uint8_t *p = (const uint8_t*)src;

    // 第一段：从tail到数组末尾
    size_t first = rb->cap - rb->tail;
    if (first > n) first = n;
    memcpy(&rb->data[rb->tail], p, first);

    // 第二段：从0开始
    size_t second = n - first;
    if (second > 0) {
        memcpy(&rb->data[0], p + first, second);
    }

    rb->tail = rb_mod(rb->tail + n, rb->cap);
    rb->size += n;
    return n;
}

size_t rb_pop(RingBuf *rb, void *dst, size_t n) {
    if (!rb || !rb->data || !dst || n == 0) return 0;

    size_t used = rb->size;
    if (used == 0) return 0;
    if (n > used) n = used;

    uint8_t *p = (uint8_t*)dst;

    // 第一段：从head到数组末尾
    size_t first = rb->cap - rb->head;
    if (first > n) first = n;
    memcpy(p, &rb->data[rb->head], first);

    // 第二段：从0开始
    size_t second = n - first;
    if (second > 0) {
        memcpy(p + first, &rb->data[0], second);
    }

    rb->head = rb_mod(rb->head + n, rb->cap);
    rb->size -= n;
    return n;
}

size_t rb_peek(const RingBuf *rb, void *dst, size_t n, size_t offset) {
    if (!rb || !rb->data || !dst) return 0;
    if (offset >= rb->size) return 0; // 起点越界，没得看

    size_t remain = rb->size - offset;
    if (n > remain) n = remain;

    uint8_t *p = (uint8_t*)dst;
    size_t start = rb_mod(rb->head + offset, rb->cap);

    // 第一段
    size_t first = rb->cap - start;
    if (first > n) first = n;
    memcpy(p, &rb->data[start], first);

    // 第二段
    size_t second = n - first;
    if (second > 0) {
        memcpy(p + first, &rb->data[0], second);
    }
    return n;
}

bool rb_search(const RingBuf *rb, const void *pattern, size_t m, size_t *out_index) {
    if (!rb || !rb->data || !pattern) return false;
    if (m == 0) { if (out_index) *out_index = 0; return true; } // 空模式视为命中0
    if (m > rb->size) return false;

    const uint8_t *pat = (const uint8_t*)pattern;
    size_t limit = rb->size - m;

    // 朴素匹配：稳定简单，够用
    for (size_t pos = 0; pos <= limit; ++pos) {
        size_t k = 0;
        while (k < m && rb_at(rb, pos + k) == pat[k]) {
            ++k;
        }
        if (k == m) {
            if (out_index) *out_index = pos;
            return true;
        }
    }
    return false;
}

void rb_debug_dump(const RingBuf *rb, size_t max_bytes) {
    if (!rb || !rb->data) { printf("<nil>\n"); return; }
    printf("[cap=%zu size=%zu head=%zu tail=%zu] data: ",
           rb->cap, rb->size, rb->head, rb->tail);
    size_t n = (rb->size < max_bytes) ? rb->size : max_bytes;
    for (size_t i = 0; i < n; ++i) {
        printf("%02X ", rb_at(rb, i));
    }
    if (rb->size > n) printf("...");
    printf("\n");
}
