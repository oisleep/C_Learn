#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/// 配置：缓冲区容量（固定，不用 malloc）
#define BUF_CAP 8
typedef int buf_elem_t;   // 元素类型（先用 int，后面你可以改成 uint8_t 等）

/// 返回码
typedef enum { BUF_OK = 0, BUF_FULL = 1, BUF_EMPTY = 2 } BufStatus;

/// 数据结构：head 写入位置，tail 读取位置，count 当前元素数
typedef struct {
    buf_elem_t data[BUF_CAP];
    int head;   // 下一次写入的位置
    int tail;   // 下一次读取的位置
    int count;  // 当前已存元素个数
} RingBuf;

/// —— 工具函数 ——
/// 把索引前进一步并在容量处回绕
static inline int step(int i) { return (i + 1) % BUF_CAP; }

/// 初始化
void rb_init(RingBuf *rb) {
    rb->head = rb->tail = rb->count = 0;
    for (int i = 0; i < BUF_CAP; ++i) rb->data[i] = 0;
}

/// 判空/判满
bool rb_is_empty(const RingBuf *rb) { return rb->count == 0; }
bool rb_is_full (const RingBuf *rb) { return rb->count == BUF_CAP; }

/// 存：入队（不覆盖旧数据，满则返回 BUF_FULL）
BufStatus rb_push(RingBuf *rb, buf_elem_t v) {
    if (rb_is_full(rb)) return BUF_FULL;
    rb->data[rb->head] = v;
    rb->head = step(rb->head);
    rb->count++;
    return BUF_OK;
}

/// 取：出队（读取最早写入的一个元素）
BufStatus rb_pop(RingBuf *rb, buf_elem_t *out) {
    if (rb_is_empty(rb)) return BUF_EMPTY;
    if (out) *out = rb->data[rb->tail];
    rb->tail = step(rb->tail);
    rb->count--;
    return BUF_OK;
}

/// 查看：窥视队头（不移除）
BufStatus rb_peek(const RingBuf *rb, buf_elem_t *out) {
    if (rb_is_empty(rb)) return BUF_EMPTY;
    if (out) *out = rb->data[rb->tail];
    return BUF_OK;
}

/// 查看：按队内索引读取（0 表示队头，count-1 表示队尾），不移除
/// 成功返回 1，失败返回 0
int rb_at(const RingBuf *rb, int index, buf_elem_t *out) {
    if (index < 0 || index >= rb->count) return 0;
    int pos = (rb->tail + index) % BUF_CAP;
    if (out) *out = rb->data[pos];
    return 1;
}

/// 检索：查找第一个等于 v 的元素，返回相对队头的索引，找不到返回 -1
int rb_find_first(const RingBuf *rb, buf_elem_t v) {
    for (int i = 0; i < rb->count; ++i) {
        int pos = (rb->tail + i) % BUF_CAP;
        if (rb->data[pos] == v) return i;
    }
    return -1;
}

/// 打印当前内容（从队头到队尾）
void rb_dump(const RingBuf *rb) {
    printf("RB{count=%d, cap=%d} [ ", rb->count, BUF_CAP);
    for (int i = 0; i < rb->count; ++i) {
        int pos = (rb->tail + i) % BUF_CAP;
        printf("%d ", rb->data[pos]);
    }
    printf("]\n");
}

/// 一个简单的交互式测试：a(添加), g(取出), p(查看队头), f(查找), d(打印), q(退出)
int main(void) {
    RingBuf rb; rb_init(&rb);
    printf("Commands: a <num>=push, g=pop, p=peek, f <num>=find, d=dump, q=quit\n");

    char cmd;
    while (1) {
        printf("> ");
        int ret = scanf(" %c", &cmd);       // 前置空格跳过换行/空白
        if (ret != 1) break;

        if (cmd == 'a') {                   // push
            buf_elem_t v;
            if (scanf("%d", &v) == 1) {
                BufStatus s = rb_push(&rb, v);
                if (s == BUF_OK)   printf("push OK\n");
                else               printf("push FAIL (FULL)\n");
            }
        } else if (cmd == 'g') {            // pop
            buf_elem_t v;
            BufStatus s = rb_pop(&rb, &v);
            if (s == BUF_OK)   printf("pop -> %d\n", v);
            else               printf("pop FAIL (EMPTY)\n");
        } else if (cmd == 'p') {            // peek
            buf_elem_t v;
            BufStatus s = rb_peek(&rb, &v);
            if (s == BUF_OK)   printf("peek = %d\n", v);
            else               printf("peek FAIL (EMPTY)\n");
        } else if (cmd == 'f') {            // find
            buf_elem_t v;
            if (scanf("%d", &v) == 1) {
                int idx = rb_find_first(&rb, v);
                if (idx >= 0) printf("found at index %d (0=head)\n", idx);
                else          printf("not found\n");
            }
        } else if (cmd == 'd') {            // dump
            rb_dump(&rb);
        } else if (cmd == 'q') {
            break;
        } else {
            puts("Unknown cmd. Use: a/g/p/f/d/q");
        }
    }
    return 0;
}
