// main.c — 交互式示例
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "ringbuf.h"

#define LINE_MAX_LEN 4096

// ============ 工具 ============

// 去掉行尾的 \r\n
static void chomp(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[--n] = '\0';
    }
}

// 判断一个字符是否是十六进制位
static int is_hex(char c) {
    return isxdigit((unsigned char)c) ? 1 : 0;
}

// 把一对十六进制字符变成一个字节（已保证两个都是hex）
static unsigned char hexpair_to_byte(char hi, char lo) {
    unsigned v = 0;
    if (hi >= '0' && hi <= '9') v = (hi - '0') << 4;
    else v = ((unsigned)(toupper(hi) - 'A') + 10) << 4;

    if (lo >= '0' && lo <= '9') v |= (lo - '0');
    else v |= (unsigned)(toupper(lo) - 'A') + 10;
    return (unsigned char)v;
}

// 解析形如 "01 02 0xFF aa de adbeef" 这类十六进制串为字节数组
// 规则：
// - 忽略空白
// - 忽略每个 token 前缀 "0x"/"0X"（可有可无）
// - 允许奇数个hex字符，最前面的那个会按高位对齐（等价前补0）
// 返回生成的字节数；*out 会 malloc（调用者负责 free）
static size_t parse_hex_bytes(const char *line, unsigned char **out) {
    if (!line || !out) return 0;
    *out = NULL;

    // 先拷一份并去掉空白+0x
    char buf[LINE_MAX_LEN];
    size_t j = 0;
    for (size_t i = 0; line[i] && j + 1 < sizeof(buf); ++i) {
        unsigned char c = (unsigned char)line[i];
        if (isspace(c)) continue;

        // 跳过每个 token 的 0x/0X
        if (c == '0' && (line[i+1] == 'x' || line[i+1] == 'X')) {
            // 仅当后面紧跟十六进制字符时才当作前缀丢弃
            if (is_hex(line[i+2])) {
                i += 1; // 跳过 'x' / 'X'
                continue;
            }
        }
        buf[j++] = (char)c;
    }
    buf[j] = '\0';

    // 过滤后如果完全不是 hex，则直接返回 0
    size_t hexcnt = 0;
    for (size_t i = 0; i < j; ++i) {
        if (!is_hex(buf[i])) return 0; // 出现非hex字符，格式不合法
        hexcnt++;
    }
    if (hexcnt == 0) return 0;

    // 偶数个hex -> 每两位一字节；奇数个 -> 头部补 1 个 '0'
    int odd = (hexcnt % 2) ? 1 : 0;
    size_t bytes = (hexcnt + odd) / 2;

    unsigned char *mem = (unsigned char*)malloc(bytes);
    if (!mem) return 0;

    size_t bi = 0; // 输出字节索引
    size_t i = 0;  // buf 游标
    if (odd) {
        // 先处理一个高半字节
        mem[bi++] = hexpair_to_byte('0', buf[i++]);
    }
    while (i + 1 < j) {
        mem[bi++] = hexpair_to_byte(buf[i], buf[i+1]);
        i += 2;
    }
    *out = mem;
    return bytes;
}

// 简单 hexdump + ASCII（可读）输出
static void print_bytes_line(const unsigned char *p, size_t n) {
    // Hex
    printf("HEX  : ");
    for (size_t i = 0; i < n; ++i) {
        printf("%02X ", p[i]);
    }
    if (n == 0) printf("(empty)");
    printf("\n");

    // ASCII（不可见字符打印'.'）
    printf("ASCII: ");
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        if (isprint(c)) putchar(c);
        else putchar('.');
    }
    if (n == 0) printf("(empty)");
    printf("\n");
}

// 打印帮助
static void print_help(void) {
    printf(
        "命令一览（大小写不敏感）：\n"
        "  help                      显示帮助\n"
        "  cap                       查看容量（字节）\n"
        "  size                      查看已用大小（字节）\n"
        "  free                      查看剩余可写空间（字节）\n"
        "  clear                     清空缓冲区\n"
        "  dump [N]                  从可读头部起，转储最多 N 字节（默认64）\n"
        "  pushs <字符串>            以原样字节写入（UTF-8/ASCII）\n"
        "  pushx <hex...>            以十六进制写入，例如：pushx 01 02 0xFF aa DEADBEEF\n"
        "  pop <N>                   读出 N 字节并显示（不会溢出，按实际可读为准）\n"
        "  peek <offset> <N>         仅查看，不移动读指针\n"
        "  searchs <字符串>          在缓冲区中检索该字节序列（字符串）\n"
        "  searchx <hex...>          在缓冲区中检索十六进制序列\n"
        "  init <capacity>           重新初始化为指定容量（会清空原数据）\n"
        "  exit / quit               退出\n"
        "\n"
        "示例：\n"
        "  pushs hello world\n"
        "  pushx 01 02 0xFF aa de adbeef\n"
        "  dump 128\n"
        "  peek 6 5\n"
        "  searchs world\n"
        "  searchx DE AD BE EF\n"
        "  pop 6\n"
    );
}

// ============ 主体 ============

int main(void) {
    // 1) 初始化一个默认容量（你也可以后续用 init 改）
    RingBuf rb;
    size_t default_cap = 64;
    if (!rb_init(&rb, default_cap)) {
        fprintf(stderr, "初始化失败：内存不足？\n");
        return 1;
    }
    printf("已初始化环形缓冲区，容量=%zu 字节。输入 help 查看命令。\n", rb_capacity(&rb));

    char line[LINE_MAX_LEN];

    for (;;) {
        printf("\nrb> ");
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n输入结束，退出。\n");
            break;
        }
        chomp(line);

        // 跳过空行
        char *p = line;
        while (*p && isspace((unsigned char)*p)) ++p;
        if (*p == '\0') continue;

        // 提取命令（第一个token）
        char cmd[64] = {0};
        size_t ci = 0;
        while (p[ci] && !isspace((unsigned char)p[ci]) && ci + 1 < sizeof(cmd)) {
            cmd[ci] = (char)tolower((unsigned char)p[ci]);
            ++ci;
        }
        cmd[ci] = '\0';
        // 跳过命令后的空白，剩下的是参数区
        char *args = p + ci;
        while (*args && isspace((unsigned char)*args)) ++args;

        // ========== 分发命令 ==========
        if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            print_help();
        }
        else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            printf("再见。\n");
            break;
        }
        else if (strcmp(cmd, "cap") == 0) {
            printf("capacity = %zu\n", rb_capacity(&rb));
        }
        else if (strcmp(cmd, "size") == 0) {
            printf("size = %zu\n", rb_size(&rb));
        }
        else if (strcmp(cmd, "free") == 0) {
            printf("free = %zu\n", rb_free_space(&rb));
        }
        else if (strcmp(cmd, "clear") == 0) {
            rb_clear(&rb);
            printf("已清空。\n");
        }
        else if (strcmp(cmd, "dump") == 0) {
            size_t n = 64;
            if (*args) {
                // 尝试读取用户指定的 N
                char *end = NULL;
                unsigned long tmp = strtoul(args, &end, 0);
                if (end != args) n = (size_t)tmp;
            }
            if (n == 0) { printf("(N=0，无输出)\n"); continue; }

            size_t avail = rb_size(&rb);
            if (n > avail) n = avail;

            unsigned char *buf = (unsigned char*)malloc(n ? n : 1);
            if (!buf) { fprintf(stderr, "内存不足。\n"); continue; }

            size_t got = rb_peek(&rb, buf, n, 0);
            printf("DUMP %zu 字节（从head起）：\n", got);
            print_bytes_line(buf, got);
            free(buf);
        }
        else if (strcmp(cmd, "pushs") == 0) {
            if (*args == '\0') {
                printf("用法：pushs <字符串>\n");
                continue;
            }
            size_t want = strlen(args);
            size_t wrote = rb_push(&rb, args, want);
            printf("pushs: 请求=%zu，实际写入=%zu（free=%zu）\n", want, wrote, rb_free_space(&rb));
        }
        else if (strcmp(cmd, "pushx") == 0) {
            if (*args == '\0') {
                printf("用法：pushx <hex...>\n");
                continue;
            }
            unsigned char *bytes = NULL;
            size_t n = parse_hex_bytes(args, &bytes);
            if (n == 0) {
                printf("pushx: 解析失败，请输入十六进制，如：01 02 0xFF aa DEADBEEF\n");
                continue;
            }
            size_t wrote = rb_push(&rb, bytes, n);
            printf("pushx: 请求=%zu，实际写入=%zu（free=%zu）\n", n, wrote, rb_free_space(&rb));
            free(bytes);
        }
        else if (strcmp(cmd, "pop") == 0) {
            if (*args == '\0') {
                printf("用法：pop <N>\n");
                continue;
            }
            char *end = NULL;
            unsigned long tmp = strtoul(args, &end, 0);
            if (end == args) {
                printf("pop: 参数错误，应为数字\n");
                continue;
            }
            size_t want = (size_t)tmp;
            if (want == 0) { printf("(N=0，无操作)\n"); continue; }

            size_t avail = rb_size(&rb);
            if (want > avail) want = avail;

            unsigned char *buf = (unsigned char*)malloc(want ? want : 1);
            if (!buf) { fprintf(stderr, "内存不足。\n"); continue; }

            size_t got = rb_pop(&rb, buf, want);
            printf("pop: 实际读出=%zu，剩余=%zu\n", got, rb_size(&rb));
            print_bytes_line(buf, got);
            free(buf);
        }
        else if (strcmp(cmd, "peek") == 0) {
            // 语法：peek <offset> <N>
            if (*args == '\0') {
                printf("用法：peek <offset> <N>\n");
                continue;
            }
            // 解析 offset
            char *end = NULL;
            unsigned long off = strtoul(args, &end, 0);
            if (end == args) {
                printf("peek: 参数错误，offset 应为数字\n");
                continue;
            }
            while (*end && isspace((unsigned char)*end)) ++end;
            if (*end == '\0') {
                printf("peek: 缺少 N 参数\n");
                continue;
            }
            unsigned long nval = strtoul(end, &end, 0);
            size_t offset = (size_t)off;
            size_t n = (size_t)nval;

            if (n == 0) { printf("(N=0，无输出)\n"); continue; }
            if (offset >= rb_size(&rb)) {
                printf("peek: offset 超出已用范围（size=%zu）\n", rb_size(&rb));
                continue;
            }
            size_t maxn = rb_size(&rb) - offset;
            if (n > maxn) n = maxn;

            unsigned char *buf = (unsigned char*)malloc(n ? n : 1);
            if (!buf) { fprintf(stderr, "内存不足。\n"); continue; }

            size_t got = rb_peek(&rb, buf, n, offset);
            printf("peek: offset=%zu，n=%zu，实际拷贝=%zu\n", offset, n, got);
            print_bytes_line(buf, got);
            free(buf);
        }
        else if (strcmp(cmd, "searchs") == 0) {
            if (*args == '\0') {
                printf("用法：searchs <字符串>\n");
                continue;
            }
            size_t m = strlen(args);
            size_t idx = 0;
            int found = rb_search(&rb, args, m, &idx);
            if (found) printf("FOUND at logical index %zu（以当前head为0）\n", idx);
            else printf("NOT FOUND\n");
        }
        else if (strcmp(cmd, "searchx") == 0) {
            if (*args == '\0') {
                printf("用法：searchx <hex...>\n");
                continue;
            }
            unsigned char *pat = NULL;
            size_t m = parse_hex_bytes(args, &pat);
            if (m == 0) {
                printf("searchx: 解析失败，请输入十六进制，如：DE AD BE EF 或 0xDE 0xAD ...\n");
                continue;
            }
            size_t idx = 0;
            int found = rb_search(&rb, pat, m, &idx);
            free(pat);
            if (found) printf("FOUND at logical index %zu（以当前head为0）\n", idx);
            else printf("NOT FOUND\n");
        }
        else if (strcmp(cmd, "init") == 0) {
            if (*args == '\0') {
                printf("用法：init <capacity>\n");
                continue;
            }
            char *end = NULL;
            unsigned long cap = strtoul(args, &end, 0);
            if (end == args || cap == 0) {
                printf("init: 容量必须是正整数\n");
                continue;
            }
            rb_free(&rb);
            if (!rb_init(&rb, (size_t)cap)) {
                fprintf(stderr, "init失败：内存不足？\n");
                return 1;
            }
            printf("已重新初始化，capacity=%zu\n", rb_capacity(&rb));
        }
        else {
            printf("未知命令：%s  （输入 help 查看可用命令）\n", cmd);
        }
    }

    rb_free(&rb);
    return 0;
}
