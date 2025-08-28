// main.c — 串口小终端：环形缓冲输入、实时展示、发送字符串/十六进制、日志落盘
// 架构：read_thread -> rb_push()；print_thread 周期性从 rb_pop() 打印/记录

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdatomic.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static void ms_sleep(unsigned ms) { Sleep(ms); }
#else
#include <pthread.h>
#include <unistd.h>
static void ms_sleep(unsigned ms) { usleep(ms * 1000); }
#endif

#include "D:\C_Learn\src\ringbuf\ringbuf.h"
#include "serial_port.h"

#define RB_CAP (64 * 1024) // 64 KiB 环形缓冲，更适合串口
#define LINE_MAX 4096

typedef enum
{
    VIEW_ASCII = 0,
    VIEW_HEX = 1
} ViewMode;

static RingBuf g_rb;
static SerialPort g_sp;
static atomic_bool g_run_reader = false;
static atomic_bool g_run_printer = false;
static atomic_bool g_live = true;
static ViewMode g_view = VIEW_ASCII;

static FILE *g_logf = NULL;
static atomic_ulong g_total_rx = 0;
static atomic_ulong g_total_tx = 0;
static atomic_ulong g_drop_bytes = 0;

#ifdef _WIN32
static HANDLE hReader = NULL, hPrinter = NULL;
#else
static pthread_t thReader, thPrinter;
#endif

/* ---- 工具 ---- */
static void chomp(char *s)
{
    if (!s)
        return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
}

static int is_hex(char c) { return isxdigit((unsigned char)c); }
static unsigned char hexpair_to_byte(char hi, char lo)
{
    unsigned v = 0;
    if (hi >= '0' && hi <= '9')
        v = (hi - '0') << 4;
    else
        v = ((unsigned)(toupper(hi) - 'A') + 10) << 4;
    if (lo >= '0' && lo <= '9')
        v |= (lo - '0');
    else
        v |= ((unsigned)(toupper(lo) - 'A') + 10);
    return (unsigned char)v;
}
static size_t parse_hex_bytes(const char *line, unsigned char **out)
{
    if (!line || !out)
        return 0;
    *out = NULL;
    char buf[LINE_MAX];
    size_t j = 0;
    for (size_t i = 0; line[i] && j + 1 < sizeof(buf); ++i)
    {
        unsigned char c = (unsigned char)line[i];
        if (isspace(c))
            continue;
        if (c == '0' && (line[i + 1] == 'x' || line[i + 1] == 'X') && is_hex(line[i + 2]))
        {
            i += 1;
            continue;
        }
        if (!is_hex(c))
            return 0;
        buf[j++] = (char)c;
    }
    buf[j] = 0;
    if (j == 0)
        return 0;
    int odd = (j % 2) ? 1 : 0;
    size_t bytes = (j + odd) / 2;
    unsigned char *mem = (unsigned char *)malloc(bytes);
    if (!mem)
        return 0;
    size_t bi = 0, i = 0;
    if (odd)
        mem[bi++] = hexpair_to_byte('0', buf[i++]);
    while (i + 1 < j)
    {
        mem[bi++] = hexpair_to_byte(buf[i], buf[i + 1]);
        i += 2;
    }
    *out = mem;
    return bytes;
}

static void print_hexdump_line(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        printf("%02X ", p[i]);
    }
}
static void print_ascii(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        unsigned char c = p[i];
        putchar(isprint(c) ? c : '.');
    }
}

/* 空间不够时，丢弃最旧数据以“永不阻塞” */
static void rb_push_overwrite(RingBuf *rb, const void *src, size_t n)
{
    size_t freeb = rb_free_space(rb);
    if (n >= rb->cap)
    {
        // 只保留末尾 cap 字节
        const unsigned char *p = (const unsigned char *)src + (n - rb->cap);
        rb_clear(rb);
        rb_push(rb, p, rb->cap);
        g_drop_bytes += (unsigned long)(n - rb->cap);
        return;
    }
    if (n > freeb)
    {
        size_t need = n - freeb;
        unsigned char tmp[1024];
        while (need)
        {
            size_t step = need > sizeof(tmp) ? sizeof(tmp) : need;
            size_t got = rb_pop(rb, tmp, step);
            if (got == 0)
                break;
            need -= got;
            g_drop_bytes += (unsigned long)got;
        }
    }
    rb_push(rb, src, n);
}

/* ---- 线程：串口读取 ---- */
#ifdef _WIN32
static DWORD WINAPI reader_thread(LPVOID arg)
{
#else
static void *reader_thread(void *arg)
{
#endif
    (void)arg;
    unsigned char buf[4096];
    while (atomic_load(&g_run_reader))
    {
        if (!sp_is_open(&g_sp))
        {
            ms_sleep(100);
            continue;
        }
        long r = sp_read(&g_sp, buf, sizeof(buf));
        if (r < 0)
        {
            ms_sleep(10);
            continue;
        }
        if (r == 0)
        { /* 超时 */
            continue;
        }
        rb_push_overwrite(&g_rb, buf, (size_t)r);
        g_total_rx += (unsigned long)r;
        if (g_logf)
        {
            fwrite(buf, 1, (size_t)r, g_logf);
            fflush(g_logf);
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- 线程：展示（实时从环里弹出并打印） ---- */
#ifdef _WIN32
static DWORD WINAPI printer_thread(LPVOID arg)
{
#else
static void *printer_thread(void *arg)
{
#endif
    (void)arg;
    unsigned char buf[4096];
    while (atomic_load(&g_run_printer))
    {
        if (!atomic_load(&g_live))
        {
            ms_sleep(50);
            continue;
        }
        size_t avail = rb_size(&g_rb);
        if (avail == 0)
        {
            ms_sleep(20);
            continue;
        }
        size_t want = (avail > sizeof(buf)) ? sizeof(buf) : avail;
        size_t got = rb_pop(&g_rb, buf, want);
        if (got)
        {
            if (g_view == VIEW_ASCII)
            {
                print_ascii(buf, got);
            }
            else
            {
                print_hexdump_line(buf, got);
            }
            fflush(stdout);
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- 命令行 ---- */
static void print_help(void)
{
    printf(
        "命令：\n"
        "  open <port> <baud>    打开串口（Win: COM3  Linux/mac: /dev/ttyUSB0）\n"
        "  close                 关闭串口\n"
        "  txs <字符串>          发送字符串（原样字节）\n"
        "  txx <hex...>          发送十六进制，如：txx 55 AA 01 02 0x0D 0A\n"
        "  live on|off           实时打印开关（默认 on）\n"
        "  mode ascii|hex        打印模式（ASCII/HEX）\n"
        "  log on [file]         开启日志到文件（默认 serial.log）\n"
        "  log off               关闭日志\n"
        "  dump [N]              从缓冲 peek 最多 N 字节（不消费，默认 256）\n"
        "  size/free             查看环形缓冲使用情况\n"
        "  stat                  统计：累计收/发、丢弃字节\n"
        "  rtscts on|off         硬件流控\n"
        "  exit/quit             退出\n");
}

int main(void)
{
    // init rb
    if (!rb_init(&g_rb, RB_CAP))
    {
        fprintf(stderr, "ring buffer init failed\n");
        return 1;
    }
    memset(&g_sp, 0, sizeof(g_sp));
    atomic_store(&g_run_reader, true);
    atomic_store(&g_run_printer, true);
    atomic_store(&g_live, true);

    // 启动线程
#ifdef _WIN32
    hReader = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
    hPrinter = CreateThread(NULL, 0, printer_thread, NULL, 0, NULL);
#else
    pthread_create(&thReader, NULL, reader_thread, NULL);
    pthread_create(&thPrinter, NULL, printer_thread, NULL);
#endif

    printf("串口小终端就绪。输入 help 查看命令。\n");
    char line[LINE_MAX];

    for (;;)
    {
        printf("\nser> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;
        chomp(line);
        char *p = line;
        while (*p && isspace((unsigned char)*p))
            ++p;
        if (!*p)
            continue;

        char cmd[64] = {0};
        size_t ci = 0;
        while (p[ci] && !isspace((unsigned char)p[ci]) && ci + 1 < sizeof(cmd))
        {
            cmd[ci] = (char)tolower((unsigned char)p[ci]);
            ++ci;
        }
        char *args = p + ci;
        while (*args && isspace((unsigned char)*args))
            ++args;

        if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        {
            print_help();
        }
        else if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit"))
        {
            break;
        }
        else if (!strcmp(cmd, "open"))
        {
            if (!*args)
            {
                printf("用法：open <port> <baud>\n");
                continue;
            }
            char port[128] = {0};
            int baud = 115200;
            // 解析两个参数
            int got = sscanf(args, "%127s %d", port, &baud);
            if (got < 1)
            {
                printf("用法：open <port> <baud>\n");
                continue;
            }
            if (sp_is_open(&g_sp))
            {
                sp_close(&g_sp);
            }
            if (sp_open(&g_sp, port, baud))
            {
                printf("打开成功：%s @ %d 8N1\n", port, baud);
            }
            else
            {
                printf("打开失败。\n");
            }
        }
        else if (!strcmp(cmd, "close"))
        {
            if (sp_is_open(&g_sp))
            {
                sp_close(&g_sp);
                puts("已关闭。");
            }
            else
                puts("未打开。");
        }
        else if (!strcmp(cmd, "txs"))
        {
            if (!*args)
            {
                printf("用法：txs <字符串>\n");
                continue;
            }
            if (!sp_is_open(&g_sp))
            {
                printf("未打开串口。\n");
                continue;
            }
            long w = sp_write(&g_sp, args, strlen(args));
            if (w > 0)
                g_total_tx += (unsigned long)w;
            printf("已发 %ld 字节\n", w);
        }
        else if (!strcmp(cmd, "txx"))
        {
            if (!*args)
            {
                printf("用法：txx <hex...>\n");
                continue;
            }
            if (!sp_is_open(&g_sp))
            {
                printf("未打开串口。\n");
                continue;
            }
            unsigned char *bytes = NULL;
            size_t n = parse_hex_bytes(args, &bytes);
            if (n == 0)
            {
                printf("解析失败。\n");
                continue;
            }
            long w = sp_write(&g_sp, bytes, n);
            if (w > 0)
                g_total_tx += (unsigned long)w;
            printf("已发 %ld/%zu 字节\n", w, n);
            free(bytes);
        }
        else if (!strcmp(cmd, "live"))
        {
            if (!*args)
            {
                printf("live = %s\n", atomic_load(&g_live) ? "on" : "off");
                continue;
            }
            if (!strcmp(args, "on"))
                atomic_store(&g_live, true);
            else if (!strcmp(args, "off"))
                atomic_store(&g_live, false);
            else
                printf("用法：live on|off\n");
        }
        else if (!strcmp(cmd, "mode"))
        {
            if (!*args)
            {
                printf("mode = %s\n", g_view == VIEW_ASCII ? "ascii" : "hex");
                continue;
            }
            if (!strcmp(args, "ascii"))
                g_view = VIEW_ASCII;
            else if (!strcmp(args, "hex"))
                g_view = VIEW_HEX;
            else
                printf("用法：mode ascii|hex\n");
        }
        else if (!strcmp(cmd, "log"))
        {
            if (!*args)
            {
                printf("用法：log on [file] | log off\n");
                continue;
            }
            if (!strncmp(args, "on", 2))
            {
                char path[256] = "serial.log";
                sscanf(args + 2, "%255s", path);
                if (g_logf)
                    fclose(g_logf);
                g_logf = fopen(path, "ab");
                if (g_logf)
                    printf("日志开启 -> %s\n", path);
                else
                    printf("无法打开日志文件。\n");
            }
            else if (!strcmp(args, "off"))
            {
                if (g_logf)
                {
                    fclose(g_logf);
                    g_logf = NULL;
                    puts("日志关闭。");
                }
                else
                    puts("日志本就未开。");
            }
            else
            {
                printf("用法：log on [file] | log off\n");
            }
        }
        else if (!strcmp(cmd, "dump"))
        {
            size_t n = 256;
            if (*args)
            {
                char *end = NULL;
                unsigned long tmp = strtoul(args, &end, 0);
                if (end != args)
                    n = (size_t)tmp;
            }
            if (n == 0)
            {
                puts("(N=0)");
                continue;
            }
            size_t avail = rb_size(&g_rb);
            if (n > avail)
                n = avail;
            unsigned char *buf = (unsigned char *)malloc(n ? n : 1);
            if (!buf)
            {
                fprintf(stderr, "内存不足\n");
                continue;
            }
            size_t got = rb_peek(&g_rb, buf, n, 0);
            if (g_view == VIEW_ASCII)
                print_ascii(buf, got);
            else
                print_hexdump_line(buf, got);
            putchar('\n');
            free(buf);
        }
        else if (!strcmp(cmd, "size"))
        {
            printf("size = %zu\n", rb_size(&g_rb));
        }
        else if (!strcmp(cmd, "free"))
        {
            printf("free = %zu\n", rb_free_space(&g_rb));
        }
        else if (!strcmp(cmd, "stat"))
        {
            printf("RX=%lu  TX=%lu  dropped(oldest)=%lu  rb(size=%zu free=%zu cap=%zu)\n",
                   (unsigned long)g_total_rx, (unsigned long)g_total_tx,
                   (unsigned long)g_drop_bytes, rb_size(&g_rb), rb_free_space(&g_rb), rb_capacity(&g_rb));
        }
        else if (!strcmp(cmd, "rtscts"))
        {
            if (!*args)
            {
                printf("当前：%s\n", g_sp.rtscts ? "on" : "off");
                continue;
            }
            if (!sp_is_open(&g_sp))
            {
                printf("未打开串口。\n");
                continue;
            }
            bool en = (!strcmp(args, "on"));
            if (sp_set_rtscts(&g_sp, en))
                printf("RTS/CTS -> %s\n", en ? "on" : "off");
            else
                printf("设置失败（平台/驱动可能不支持）。\n");
        }
        else
        {
            printf("未知命令：%s  （help 查看帮助）\n", cmd);
        }
    }

    // 退出收尾
    atomic_store(&g_run_reader, false);
    atomic_store(&g_run_printer, false);
    ms_sleep(100);
#ifdef _WIN32
    if (hReader)
    {
        WaitForSingleObject(hReader, 500);
        CloseHandle(hReader);
    }
    if (hPrinter)
    {
        WaitForSingleObject(hPrinter, 500);
        CloseHandle(hPrinter);
    }
#else
    pthread_join(thReader, NULL);
    pthread_join(thPrinter, NULL);
#endif
    if (g_logf)
        fclose(g_logf);
    if (sp_is_open(&g_sp))
        sp_close(&g_sp);
    rb_free(&g_rb);
    puts("bye.");
    return 0;
}
