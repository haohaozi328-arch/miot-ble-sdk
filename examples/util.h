/**
 * @file util.h
 * @brief 示例共用：host 移植层 + 十六进制打印（仅示例使用，不属于 SDK 接口）
 */
#ifndef EXAMPLE_UTIL_H
#define EXAMPLE_UTIL_H

#include <stdio.h>
#include <stdarg.h>
#include "miot_ble/miot_port.h"

static int host_notify(void *user, const uint8_t *d, size_t n)
{
    const char *ch = (const char *)user;
    printf("  >> notify[%s] %uB:", ch, (unsigned)n);
    for (size_t i = 0; i < n; i++) printf(" %02x", d[i]);
    printf("\n");
    return 0;
}

static void demo_log(void *user, int lv, const char *tag, const char *fmt, ...)
{
    (void)user; (void)lv;
    va_list ap;
    va_start(ap, fmt);
    printf("[%s] ", tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static void hex_line(const char *prefix, const uint8_t *d, size_t n)
{
    printf("%s %uB:", prefix, (unsigned)n);
    for (size_t i = 0; i < n; i++) printf(" %02x", d[i]);
    printf("\n");
}

/** beacon 帧安全打印：host 未注册加密后端时给出提示而非崩溃 */
static void show_frame(const char *prefix, const uint8_t *d, int flen)
{
    if (flen < 0)
        printf("%s: build rc=%d（host 未注册加密后端 miot_crypto_set_ops，见 port/esp32）\n",
               prefix, flen);
    else
        hex_line(prefix, d, (size_t)flen);
}

#endif /* EXAMPLE_UTIL_H */
