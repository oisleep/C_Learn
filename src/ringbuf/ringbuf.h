#ifndef RINGBUF_H
#define RINGBUF_H

/*
 * 一个简单、稳定的环形缓冲区（ring buffer）实现。
 * 适用场景：串口/Socket接收、流水数据的暂存与检索、日志环缓冲等。
 *
 * 设计理念（通俗版）：
 * - data：真实存储的字节数组（绕成圈的“跑道”）
 * - cap ：容量（跑道总长）
 * - head：下一个“读出”位置（从这里开始读）
 * - tail：下一个“写入”位置（从这里写）
 * - size：当前已用字节数（跑道上“正在占用”的长度）
 *
 * 特点：
 * - 固定容量，不自动扩容（更稳，适合嵌入式；要扩容可以后续自己加）
 * - push/pop 支持任意长度，自动分段拷贝（跨越“圈”时会分两段）
 * - peek 可以不移动读指针查看任意位置（从head起算的offset）
 * - search 能在缓冲区中检索目标字节序列（支持跨边界）
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;   // 实际内存
    size_t   cap;    // 容量（字节）
    size_t   head;   // 读指针：下一个读出位置
    size_t   tail;   // 写指针：下一个写入位置
    size_t   size;   // 当前已用字节数
} RingBuf;

/* ===== 基础管理 ===== */

/**
 * @brief 初始化环形缓冲区（分配内存）
 * @param rb       指向RingBuf对象
 * @param capacity 容量（字节数，>=1）
 * @return true成功；false失败（比如内存分配失败或capacity=0）
 */
bool   rb_init(RingBuf *rb, size_t capacity);

/**
 * @brief 释放缓冲区（释放内存并清零结构体）
 */
void   rb_free(RingBuf *rb);

/**
 * @brief 清空缓冲区（不释放内存，head/tail/size归零）
 */
void   rb_clear(RingBuf *rb);

/**
 * @brief 获取容量/已用/剩余空间
 */
size_t rb_capacity(const RingBuf *rb);
size_t rb_size    (const RingBuf *rb);
size_t rb_free_space(const RingBuf *rb);

/* ===== 存 / 取 ===== */

/**
 * @brief 写入n字节
 * @param src 源地址
 * @param n   请求写入的字节数
 * @return 实际写入字节数（可能小于n，取决于剩余空间）
 */
size_t rb_push(RingBuf *rb, const void *src, size_t n);

/**
 * @brief 读出n字节
 * @param dst 目标地址
 * @param n   请求读出的字节数
 * @return 实际读出字节数（可能小于n，取决于已用大小）
 */
size_t rb_pop (RingBuf *rb, void *dst, size_t n);

/* ===== 查看（不移动读指针） ===== */

/**
 * @brief 从逻辑偏移offset处开始，查看最多n字节到dst（不改变head）
 *        逻辑偏移以head为0起点：offset=0表示从当前可读区的第一个字节开始
 * @return 实际拷贝到dst的字节数（offset越界->0）
 */
size_t rb_peek(const RingBuf *rb, void *dst, size_t n, size_t offset);

/* ===== 检索 ===== */

/**
 * @brief 在缓冲区当前数据中检索pattern（大小为m）
 * @param out_index 若找到，返回逻辑起点（以head为0）
 * @return true找到；false未找到
 *
 * 说明：采用朴素匹配（简单稳定，够用）。数据量大/模式串长时可改KMP/Boyer-Moore。
 */
bool   rb_search(const RingBuf *rb, const void *pattern, size_t m, size_t *out_index);

/* ===== 调试辅助（可选） ===== */

/**
 * @brief 打印最多max_bytes个当前数据字节（从head开始）
 *        只做简单stdout打印，生产环境可自行替换为日志系统。
 */
void   rb_debug_dump(const RingBuf *rb, size_t max_bytes);

#ifdef __cplusplus
}
#endif
#endif /* RINGBUF_H */
