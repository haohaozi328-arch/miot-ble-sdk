/**
 * @file test_adv_probe.c
 * @brief 未绑定广播帧 + PSK 扫描探测形态表单测（host）
 *
 * 覆盖：
 *   1. miot_beacon_frame_unbound 的字节布局（FC/PID/cnt/MAC/capability）
 *   2. 形态表约束：fc0 恒 0x30、registered=0、authMode∈{0..3}、
 *      version∈{4,5}、capability ∈ {0x00,0x08,0x10}
 *   3. 每种形态构造出的 Service Data 总广播 ≤31B（12B 帧 + 头 4B + flags 3B）
 */
#include "miot_ble/miot_beacon.h"
#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
} while (0)

static const uint8_t MAC[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

static void test_frame_layout(void)
{
    uint8_t fr[32];
    int n = miot_beacon_frame_unbound(0x040A, MAC, 0x30, 0x58, 0x10, 0x2A,
                                      fr, sizeof(fr));
    CHECK(n == 12, "unbound frame length == 12");

    CHECK(fr[0] == 0x30 && fr[1] == 0x58, "FC bytes passthrough");
    CHECK(fr[2] == 0x0A && fr[3] == 0x04, "PID little-endian (0x040A)");
    CHECK(fr[4] == 0x2A, "frame_cnt passthrough");
    CHECK(memcmp(fr + 5, MAC, 6) == 0, "MAC air-order passthrough");
    CHECK(fr[11] == 0x10, "capability passthrough");

    /* cap 不足必须失败，而不是越界写 */
    CHECK(miot_beacon_frame_unbound(0x040A, MAC, 0x30, 0x58, 0x10, 0,
                                    fr, 11) < 0, "cap<12 rejected");

    /* 含 flags + service data 头的完整广播长度 */
    size_t adv_total = 3 /* flags AD */ + 2 /* len+type */ + 2 /* 95 FE */ + (size_t)n;
    CHECK(adv_total <= 31, "full adv payload <= 31B");
}

static void test_form_table(void)
{
    size_t cnt = 0;
    const miot_adv_form_t *forms = miot_adv_probe_forms(&cnt);
    CHECK(forms != NULL && cnt >= 5, "probe table non-empty");

    for (size_t i = 0; i < cnt; i++) {
        const miot_adv_form_t *f = &forms[i];
        CHECK(f->fc0 == 0x30, "fc0: MAC|Capability, no encrypt, no object");
        CHECK((f->fc1 & 0x01) == 0, "registered=0 (bound bit must be clear)");
        CHECK((f->fc1 >> 2 & 3) <= 3, "authMode field in range");
        CHECK((f->fc1 >> 4) == 4 || (f->fc1 >> 4) == 5,
              "beacon version 4 or 5");
        CHECK(f->capability == 0x00 || f->capability == 0x08 ||
              f->capability == 0x10, "capability is a BondAbility value");
        CHECK(f->desc != NULL && f->desc[0] != '\0', "desc non-empty");

        uint8_t fr[32];
        int n = miot_beacon_frame_unbound(0x040A, MAC, f->fc0, f->fc1,
                                          f->capability, (uint8_t)i,
                                          fr, sizeof(fr));
        CHECK(n == 12, "every form builds a 12B frame");
        printf("form %zu/%zu: FC=%02X %02X cap=%02X  %s\n",
               i + 1, cnt, f->fc0, f->fc1, f->capability, f->desc);
    }

    /* 第 0 项必须是标准认证基线（App 已实测认可的唯一形态） */
    CHECK(forms[0].fc0 == 0x30 && forms[0].fc1 == 0x58 &&
          forms[0].capability == 0x10, "form 0 = std-auth baseline");
}

int main(void)
{
    printf("== adv probe test ==\n\n");
    test_frame_layout();
    printf("\n");
    test_form_table();
    printf("\n%s\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
