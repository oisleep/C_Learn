#include "serial_port.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
/* ---------------- Windows 实现 ---------------- */
static int map_baud(int baud)
{
    // Windows 允许任意整数波特率，直接返回
    return baud;
}

static void build_port_path(const char *name, char path[128])
{
    // COM10 以上需要 \\.\ 前缀
    if (_strnicmp(name, "\\\\.\\", 4) == 0)
    {
        strncpy(path, name, 127);
        path[127] = 0;
    }
    else if ((_strnicmp(name, "COM", 3) == 0) && atoi(name + 3) >= 10)
    {
        snprintf(path, 128, "\\\\.\\%s", name);
    }
    else
    {
        strncpy(path, name, 127);
        path[127] = 0;
    }
}

bool sp_open(SerialPort *sp, const char *name, int baud)
{
    if (!sp || !name)
        return false;
    memset(sp, 0, sizeof(*sp));
    char path[128];
    build_port_path(name, path);

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "CreateFile '%s' failed (err=%lu)\n", path, GetLastError());
        return false;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb))
    {
        fprintf(stderr, "GetCommState failed\n");
        CloseHandle(h);
        return false;
    }
    dcb.BaudRate = map_baud(baud);
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = dcb.fInX = FALSE;

    if (!SetCommState(h, &dcb))
    {
        fprintf(stderr, "SetCommState failed\n");
        CloseHandle(h);
        return false;
    }

    // 设置短超时（让 ReadFile 周期性返回）
    COMMTIMEOUTS to = {0};
    to.ReadIntervalTimeout = 50;      // ms
    to.ReadTotalTimeoutConstant = 50; // ms
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 100;
    to.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(h, &to);

    // 建议放大驱动缓冲
    SetupComm(h, 1 << 15, 1 << 15); // 32 KB

    sp->h = h;
    strncpy(sp->name, name, sizeof(sp->name) - 1);
    sp->rtscts = false;
    return true;
}

void sp_close(SerialPort *sp)
{
    if (!sp)
        return;
    if (sp->h)
    {
        CloseHandle(sp->h);
        sp->h = NULL;
    }
}

long sp_write(SerialPort *sp, const void *buf, size_t n)
{
    if (!sp || !sp->h || !buf || n == 0)
        return 0;
    DWORD wr = 0;
    if (!WriteFile(sp->h, buf, (DWORD)n, &wr, NULL))
        return -1;
    return (long)wr;
}

long sp_read(SerialPort *sp, void *buf, size_t n)
{
    if (!sp || !sp->h || !buf || n == 0)
        return 0;
    DWORD rd = 0;
    if (!ReadFile(sp->h, buf, (DWORD)n, &rd, NULL))
        return -1;
    return (long)rd; // 可能为0（超时）
}

bool sp_set_rtscts(SerialPort *sp, bool enable)
{
    if (!sp || !sp->h)
        return false;
    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(sp->h, &dcb))
        return false;
    dcb.fOutxCtsFlow = enable ? TRUE : FALSE;
    dcb.fRtsControl = enable ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_DISABLE;
    bool ok = SetCommState(sp->h, &dcb) ? true : false;
    if (ok)
        sp->rtscts = enable;
    return ok;
}

bool sp_is_open(const SerialPort *sp)
{
    return sp && sp->h;
}

#else
/* ---------------- POSIX 实现（macOS / Linux） ---------------- */
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

static speed_t map_baud(int baud)
{
    switch (baud)
    {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
#ifdef B230400
    case 230400:
        return B230400;
#endif
#ifdef B460800
    case 460800:
        return B460800;
#endif
#ifdef B921600
    case 921600:
        return B921600;
#endif
    default:
        return 0;
    }
}

bool sp_open(SerialPort *sp, const char *name, int baud)
{
    if (!sp || !name)
        return false;
    memset(sp, 0, sizeof(*sp));

    int fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        perror("open");
        return false;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0)
    {
        perror("tcgetattr");
        close(fd);
        return false;
    }

    cfmakeraw(&tio); // 原始模式
    speed_t s = map_baud(baud);
    if (s == 0)
    {
        fprintf(stderr, "不支持的波特率: %d\n", baud);
        close(fd);
        return false;
    }
    cfsetispeed(&tio, s);
    cfsetospeed(&tio, s);

    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~PARENB; // 无校验
    tio.c_cflag &= ~CSTOPB; // 1 停止位
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8; // 8 数据位
    // 默认禁用硬件流控
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif

    // 非阻塞 + 短等待：VTIME 单位 0.1s；VMIN=0 则 read 最多等待 VTIME
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1; // 100ms

    if (tcsetattr(fd, TCSANOW, &tio) != 0)
    {
        perror("tcsetattr");
        close(fd);
        return false;
    }
    // 置回阻塞模式（可选）
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    sp->fd = fd;
    strncpy(sp->name, name, sizeof(sp->name) - 1);
    sp->rtscts = false;
    return true;
}

void sp_close(SerialPort *sp)
{
    if (!sp)
        return;
    if (sp->fd > 0)
    {
        close(sp->fd);
        sp->fd = -1;
    }
}

long sp_write(SerialPort *sp, const void *buf, size_t n)
{
    if (!sp || sp->fd < 0 || !buf || n == 0)
        return 0;
    ssize_t w = write(sp->fd, buf, n);
    if (w < 0)
        return -1;
    return (long)w;
}

long sp_read(SerialPort *sp, void *buf, size_t n)
{
    if (!sp || sp->fd < 0 || !buf || n == 0)
        return 0;
    ssize_t r = read(sp->fd, buf, n);
    if (r < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return (long)r;
}

bool sp_set_rtscts(SerialPort *sp, bool enable)
{
    if (!sp || sp->fd < 0)
        return false;
    struct termios tio;
    if (tcgetattr(sp->fd, &tio) != 0)
        return false;
#ifdef CRTSCTS
    if (enable)
        tio.c_cflag |= CRTSCTS;
    else
        tio.c_cflag &= ~CRTSCTS;
#else
    (void)enable;
    return false;
#endif
    bool ok = (tcsetattr(sp->fd, TCSANOW, &tio) == 0);
    if (ok)
        sp->rtscts = enable;
    return ok;
}

bool sp_is_open(const SerialPort *sp)
{
    return sp && sp->fd > 0;
}
#endif
