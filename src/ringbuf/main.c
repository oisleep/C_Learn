// main.c — 交互式 + 可视化 + 彩色指针 + bench，容量固定 32 字节
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <stdbool.h>

#include "ringbuf.h"

#define LINE_MAX_LEN 4096
#define FIXED_CAP    32  // 硬限制容量

/*---------------------- 终端颜色支持（Windows 终端开启 ANSI） ----------------------*/
#if defined(_WIN32)
#include <windows.h>
static void enable_ansi(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | 0x0004); // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    }
}
#else
static void enable_ansi(void) {}
#endif

#define C_RESET "\x1b[0m"
#define C_DIM   "\x1b[2m"
#define C_HEAD  "\x1b[32;1m"  // 亮绿：head
#define C_TAIL  "\x1b[36;1m"  // 亮青：tail
#define C_HT    "\x1b[35;1m"  // 亮紫：head==tail

/*---------------------- 小工具 ----------------------*/

// 去掉行尾 \r\n
static void chomp(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

static int is_hex(char c) { return isxdigit((unsigned char)c) ? 1 : 0; }

static unsigned char hexpair_to_byte(char hi, char lo) {
    unsigned v = 0;
    if (hi >= '0' && hi <= '9') v = (hi - '0') << 4;
    else v = ((unsigned)(toupper(hi) - 'A') + 10) << 4;
    if (lo >= '0' && lo <= '9') v |= (lo - '0');
    else v |= (unsigned)(toupper(lo) - 'A') + 10;
    return (unsigned char)v;
}

// 简易不区分大小写比较：相等返回 1
static int ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

// 解析十六进制输入：支持空格与 0x 前缀；奇数个半字节自动前补 0
static size_t parse_hex_bytes(const char *line, unsigned char **out) {
    if (!line || !out) return 0; *out = NULL;
    char buf[LINE_MAX_LEN]; size_t j = 0;
    for (size_t i = 0; line[i] && j + 1 < sizeof(buf); ++i) {
        unsigned char c = (unsigned char)line[i];
        if (isspace(c)) continue;
        if (c=='0' && (line[i+1]=='x'||line[i+1]=='X') && is_hex(line[i+2])) { i+=1; continue; }
        buf[j++] = (char)c;
    }
    buf[j] = '\0';
    size_t hexcnt = 0;
    for (size_t i = 0; i < j; ++i) { if (!is_hex(buf[i])) return 0; hexcnt++; }
    if (hexcnt == 0) return 0;

    int odd = (hexcnt % 2) ? 1 : 0;
    size_t bytes = (hexcnt + odd) / 2;
    unsigned char *mem = (unsigned char*)malloc(bytes);
    if (!mem) return 0;

    size_t bi = 0, i = 0;
    if (odd) mem[bi++] = hexpair_to_byte('0', buf[i++]);
    while (i + 1 < j) mem[bi++] = hexpair_to_byte(buf[i], buf[i+1]), i += 2;
    *out = mem; return bytes;
}

static void print_bytes_line(const unsigned char *p, size_t n) {
    printf("HEX  : ");
    for (size_t i = 0; i < n; ++i) printf("%02X ", p[i]);
    if (n == 0) printf("(empty)");
    printf("\nASCII: ");
    for (size_t i = 0; i < n; ++i) putchar(isprint(p[i]) ? p[i] : '.');
    if (n == 0) printf("(empty)");
    printf("\n");
}

/*---------------------- 可视化 ----------------------*/
// 以 32 格渲染：索引行 / 值行 / 指针行（彩色）
static void visualize(const RingBuf *rb) {
    size_t cap_vis = rb_capacity(rb);
    if (cap_vis > FIXED_CAP) cap_vis = FIXED_CAP;

    printf("\n[Visualization] cap=%zu size=%zu head=%zu tail=%zu%s\n",
           rb_capacity(rb), rb_size(rb), rb->head, rb->tail,
           (rb_size(rb)==rb_capacity(rb) && rb_capacity(rb)>0) ? " (FULL)" : "");

    // 1) 索引行
    printf("Index: ");
    for (size_t i = 0; i < cap_vis; ++i) printf("%02zu ", i);
    printf("\n");

    // 2) 值行（占用打印 HEX，空闲 '..'）
    printf("Value: ");
    for (size_t i = 0; i < cap_vis; ++i) {
        size_t cap_all = rb->cap ? rb->cap : 1;
        size_t head = rb->head % cap_all;
        size_t dist = (i + cap_all - head) % cap_all;
        int occupied = (dist < rb->size) ? 1 : 0;
        if (occupied) printf("%02X ", rb->data[i]);
        else          printf(".. ");
    }
    printf("\n");

    // 3) 指针行（彩色 H/T/HT）
    printf("Ptr  : ");
    for (size_t i = 0; i < cap_vis; ++i) {
        size_t cap_all = rb->cap ? rb->cap : 1;
        int is_head = (rb->size > 0 && i == rb->head % cap_all); // size=0 时不标 H
        int is_tail = (i == rb->tail % cap_all);
        if (is_head && is_tail) printf(C_HT "HT " C_RESET);
        else if (is_head)       printf(C_HEAD "H  " C_RESET);
        else if (is_tail)       printf(C_TAIL "T  " C_RESET);
        else                    printf("   ");
    }
    printf("\n");
}

/*---------------------- bench：简单压力/环回测试 ----------------------*/
static void cmd_bench(RingBuf *rb, size_t iters, size_t chunk) {
    if (chunk == 0)      { printf("bench: chunk 需要 > 0\n"); return; }
    if (chunk > FIXED_CAP) chunk = FIXED_CAP; // 我们固定 32 字节上限
    unsigned char *tmp = (unsigned char*)malloc(chunk);
    if (!tmp) { fprintf(stderr, "内存不足。\n"); return; }
    for (size_t i = 0; i < chunk; ++i) tmp[i] = (unsigned char)i;

    size_t pushed = 0, popped = 0;
    for (size_t i = 0; i < iters; ++i) {
        pushed += rb_push(rb, tmp, chunk);      // 尽力写
        popped += rb_pop (rb, tmp, chunk / 2);  // 每轮读一半，制造环回
    }
    printf("bench: iters=%zu chunk=%zu | pushed=%zu popped=%zu size=%zu free=%zu\n",
           iters, chunk, pushed, popped, rb_size(rb), rb_free_space(rb));
    free(tmp);
}

/*---------------------- 命令帮助 ----------------------*/
static void print_help(void) {
    printf(
        "命令（容量固定 32 字节）：\n"
        "  help                      显示帮助\n"
        "  viz                       打印可视化网格（带彩色 H/T）\n"
        "  autoviz on|off            修改后是否自动可视化（默认 on）\n"
        "  cap / size / free         基本信息\n"
        "  clear                     清空缓冲区\n"
        "  dump [N]                  转储最多 N 字节（默认 64）\n"
        "  pushs <字符串>            以字符串写入\n"
        "  pushx <hex...>            以十六进制写入，如：01 02 0xFF DEADBEEF\n"
        "  pop <N>                   读出 N 字节\n"
        "  peek <offset> <N>         仅查看\n"
        "  searchs <字符串>          检索字符串\n"
        "  searchx <hex...>          检索十六进制序列\n"
        "  bench <iters> <chunk>     简易压力测试（反复 push/pop）\n"
        "  init                      重新初始化为 32 字节（忽略参数）\n"
        "  exit / quit               退出\n"
    );
}

/*---------------------- 主体 ----------------------*/
int main(void) {
    enable_ansi(); // 尝试开启 ANSI 颜色（Windows 终端）

    RingBuf rb;
    bool auto_viz = true;

    if (!rb_init(&rb, FIXED_CAP)) {
        fprintf(stderr, "初始化失败：内存不足？\n");
        return 1;
    }
    printf("环形缓冲区就绪：容量固定为 %d 字节。输入 help 查看命令。\n", FIXED_CAP);
    visualize(&rb);

    char line[LINE_MAX_LEN];

    for (;;) {
        printf("\nrb32> ");
        if (!fgets(line, sizeof(line), stdin)) { printf("\n退出。\n"); break; }
        chomp(line);

        // 跳过空行
        char *p = line; while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) continue;

        // 解析命令
        char cmd[64] = {0}; size_t ci = 0;
        while (p[ci] && !isspace((unsigned char)p[ci]) && ci + 1 < sizeof(cmd))
            cmd[ci] = (char)tolower((unsigned char)p[ci]), ++ci;
        cmd[ci] = '\0';
        char *args = p + ci; while (*args && isspace((unsigned char)*args)) ++args;

        // 分发
        if (ieq(cmd, "help") || ieq(cmd, "?")) {
            print_help();
        } else if (ieq(cmd, "exit") || ieq(cmd, "quit")) {
            printf("再见。\n");
            break;
        } else if (ieq(cmd, "viz")) {
            visualize(&rb);
        } else if (ieq(cmd, "autoviz")) {
            if (!*args) { printf("autoviz = %s\n", auto_viz ? "on" : "off"); continue; }
            if (ieq(args, "on"))  { auto_viz = true;  printf("autoviz -> on\n"); }
            else if (ieq(args, "off")) { auto_viz = false; printf("autoviz -> off\n"); }
            else printf("用法：autoviz on|off\n");
        } else if (ieq(cmd, "cap")) {
            printf("capacity = %zu\n", rb_capacity(&rb));
        } else if (ieq(cmd, "size")) {
            printf("size = %zu\n", rb_size(&rb));
        } else if (ieq(cmd, "free")) {
            printf("free = %zu\n", rb_free_space(&rb));
        } else if (ieq(cmd, "clear")) {
            rb_clear(&rb);
            printf("已清空。\n");
            if (auto_viz) visualize(&rb);
        } else if (ieq(cmd, "dump")) {
            size_t n = 64;
            if (*args) {
                char *end=NULL; unsigned long tmp=strtoul(args,&end,0);
                if (end!=args) n=(size_t)tmp;
            }
            size_t avail = rb_size(&rb); if (n > avail) n = avail;
            unsigned char *buf = (unsigned char*)malloc(n ? n : 1);
            if (!buf) { fprintf(stderr,"内存不足。\n"); continue; }
            size_t got = rb_peek(&rb, buf, n, 0);
            printf("DUMP %zu 字节（从head）：\n", got);
            print_bytes_line(buf, got);
            free(buf);
        } else if (ieq(cmd, "pushs")) {
            if (!*args) { printf("用法：pushs <字符串>\n"); continue; }
            size_t want = strlen(args);
            size_t wrote = rb_push(&rb, args, want);
            printf("pushs: 请求=%zu 实际=%zu（free=%zu）\n", want, wrote, rb_free_space(&rb));
            if (auto_viz) visualize(&rb);
        } else if (ieq(cmd, "pushx")) {
            if (!*args) { printf("用法：pushx <hex...>\n"); continue; }
            unsigned char *bytes=NULL; size_t n=parse_hex_bytes(args,&bytes);
            if (n==0) { printf("pushx: 解析失败（示例：01 02 0xFF DEADBEEF）\n"); continue; }
            size_t wrote = rb_push(&rb, bytes, n);
            printf("pushx: 请求=%zu 实际=%zu（free=%zu）\n", n, wrote, rb_free_space(&rb));
            free(bytes);
            if (auto_viz) visualize(&rb);
        } else if (ieq(cmd, "pop")) {
            if (!*args) { printf("用法：pop <N>\n"); continue; }
            char *end=NULL; unsigned long tmp=strtoul(args,&end,0);
            if (end==args) { printf("pop: 参数错误\n"); continue; }
            size_t want=(size_t)tmp; size_t avail=rb_size(&rb); if (want>avail) want=avail;
            unsigned char *buf=(unsigned char*)malloc(want?want:1);
            if (!buf) { fprintf(stderr,"内存不足。\n"); continue; }
            size_t got=rb_pop(&rb, buf, want);
            printf("pop: 实际读出=%zu 剩余=%zu\n", got, rb_size(&rb));
            print_bytes_line(buf, got);
            free(buf);
            if (auto_viz) visualize(&rb);
        } else if (ieq(cmd, "peek")) {
            // peek <offset> <N>
            if (!*args){ printf("用法：peek <offset> <N>\n"); continue; }
            char *end=NULL; unsigned long off=strtoul(args,&end,0);
            if (end==args){ printf("peek: offset 应为数字\n"); continue; }
            while(*end && isspace((unsigned char)*end)) ++end;
            if(!*end){ printf("peek: 缺少 N 参数\n"); continue; }
            unsigned long nval=strtoul(end,&end,0);
            size_t offset=(size_t)off, n=(size_t)nval;
            if (offset>=rb_size(&rb)) { printf("peek: offset 超界（size=%zu）\n", rb_size(&rb)); continue; }
            size_t maxn=rb_size(&rb)-offset; if (n>maxn) n=maxn;
            unsigned char *buf=(unsigned char*)malloc(n?n:1);
            if(!buf){ fprintf(stderr,"内存不足。\n"); continue; }
            size_t got=rb_peek(&rb, buf, n, offset);
            printf("peek: offset=%zu n=%zu 实际=%zu\n", offset, n, got);
            print_bytes_line(buf, got);
        } else if (ieq(cmd, "searchs")) {
            if (!*args){ printf("用法：searchs <字符串>\n"); continue; }
            size_t idx=0; int found=rb_search(&rb, args, strlen(args), &idx);
            printf(found ? "FOUND at %zu\n" : "NOT FOUND\n", idx);
        } else if (ieq(cmd, "searchx")) {
            if (!*args){ printf("用法：searchx <hex...>\n"); continue; }
            unsigned char *pat=NULL; size_t m=parse_hex_bytes(args,&pat);
            if (m==0){ printf("searchx: 解析失败\n"); continue; }
            size_t idx=0; int found=rb_search(&rb, pat, m, &idx);
            free(pat);
            printf(found ? "FOUND at %zu\n" : "NOT FOUND\n", idx);
        } else if (ieq(cmd, "bench")) {
            if (!*args) { printf("用法：bench <iters> <chunk>\n"); continue; }
            char *end = NULL;
            unsigned long it = strtoul(args, &end, 0);
            while (*end && isspace((unsigned char)*end)) ++end;
            if (end == args || !*end) { printf("用法：bench <iters> <chunk>\n"); continue; }
            unsigned long ch = strtoul(end, NULL, 0);
            cmd_bench(&rb, (size_t)it, (size_t)ch);
            if (auto_viz) visualize(&rb);
        } else if (ieq(cmd, "init")) {
            rb_free(&rb);
            if (!rb_init(&rb, FIXED_CAP)) { fprintf(stderr,"init失败：内存不足？\n"); return 1; }
            printf("已重新初始化为固定容量 %d 字节。\n", FIXED_CAP);
            if (auto_viz) visualize(&rb);
        } else {
            printf("未知命令：%s  （help 查看帮助）\n", cmd);
        }
    }

    rb_free(&rb);
    return 0;
}
