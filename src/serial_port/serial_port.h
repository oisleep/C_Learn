#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
typedef struct
{
    HANDLE h;
    char name[128];
    bool rtscts;
} SerialPort;
#else
typedef struct
{
    int fd;
    char name[128];
    bool rtscts;
} SerialPort;
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    // 打开串口：name 例子 Windows: "COM3"  Linux: "/dev/ttyUSB0"  macOS: "/dev/tty.usbserial-xxx"
    // 只支持常见波特率（115200 / 921600 等），8N1，默认不启用硬件流控（可后续 sp_set_rtscts）
    bool sp_open(SerialPort *sp, const char *name, int baud);

    // 关闭串口
    void sp_close(SerialPort *sp);

    // 写串口（尽力写，返回已写字节数，错误返回 <0）
    long sp_write(SerialPort *sp, const void *buf, size_t n);

    // 读串口（带短超时，通常几十毫秒就返回；返回已读字节数，超时0，错误<0）
    long sp_read(SerialPort *sp, void *buf, size_t n);

    // 切换 RTS/CTS 硬件流控
    bool sp_set_rtscts(SerialPort *sp, bool enable);

    // 是否已打开
    bool sp_is_open(const SerialPort *sp);

#ifdef __cplusplus
}
#endif

#endif // SERIAL_PORT_H
