/**
 * @file test_spec.c
 * @brief host 单元测试 —— 用空中抓包实证过的帧做黄金样本
 *
 * 样本来源：v62c 固件 + 米家 App（v34 hook）实测解析成功的帧
 * （App 端 ud5 解析器输出 EventOccuredPacket 与本编码逐字节一致）。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "miot_ble/miot_spec.h"
#include "miot_ble/miot_ag2.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

static void hexdump(const char *tag, const uint8_t *p, size_t n)
{
    printf("%s (%uB):", tag, (unsigned)n);
    for (size_t i = 0; i < n; i++) printf(" %02x", p[i]);
    printf("\n");
}

/* 8 字节 memcmp 比对（前 n 字节） */
static void expect_bytes(const uint8_t *got, const uint8_t *want, size_t n,
                         const char *msg)
{
    if (memcmp(got, want, n) != 0) {
        printf("FAIL: %s\n  got :", msg);
        for (size_t i = 0; i < n; i++) printf(" %02x", got[i]);
        printf("\n  want:");
        for (size_t i = 0; i < n; i++) printf(" %02x", want[i]);
        printf("\n");
        g_fail++;
    }
}

/* ---------- 黄金样本 1：事件 8.1 "0,0"（16B，App 实测解析成功） ---------- */
static const uint8_t GOLD_EVT81[] = {
    0x10, 0x20, 0xb6, 0x00, 0x07, 0x08, 0x01, 0x00,
    0x01, 0x01, 0x00, 0x03, 0xa0, 0x30, 0x2c, 0x30,
};

/* ---------- 黄金样本 2：事件 8.2 九参 CSV（35B，App 实测解析成功） ---------- */
static const uint8_t GOLD_EVT82[] = {
    0x23, 0x20, 0xb9, 0x00, 0x07, 0x08, 0x02, 0x00, 0x01,
    0x01, 0x00, 0x16, 0xa0,
    0x30, 0x2c, 0x30, 0x2c, 0x30, 0x2c, 0x36, 0x34, 0x37,
    0x2c, 0x38, 0x30, 0x2c, 0x30, 0x2c, 0x30, 0x2c, 0x31,
    0x38, 0x37, 0x2c, 0x30,
};

static void test_event_81(void)
{
    uint8_t out[64];
    miot_spec_value_t a;
    memset(&a, 0, sizeof(a));
    a.piid = 1;
    a.type = MIOT_TYPE_STRING;
    strcpy(a.val.str, "0,0");

    int n = miot_spec_build_event(0x00b6, 8, 1, &a, 1, out, sizeof(out));
    CHECK(n == 16, "evt 8.1 length");
    expect_bytes(out, GOLD_EVT81, sizeof(GOLD_EVT81), "evt 8.1 bytes");
    hexdump("evt81", out, n);
}

static void test_event_82(void)
{
    uint8_t out[64];
    miot_spec_value_t a;
    memset(&a, 0, sizeof(a));
    a.piid = 1;
    a.type = MIOT_TYPE_STRING;
    strcpy(a.val.str, "0,0,0,647,80,0,0,187,0");

    int n = miot_spec_build_event(0x00b9, 8, 2, &a, 1, out, sizeof(out));
    CHECK(n == 35, "evt 8.2 length");
    expect_bytes(out, GOLD_EVT82, sizeof(GOLD_EVT82), "evt 8.2 bytes");
    hexdump("evt82", out, n);
}

static void test_action_reply(void)
{
    /* op=6 应答只含 respCode16 —— 带 siid/aiid 会被 App 读成 respCode=269（失败） */
    uint8_t out[8];
    int n = miot_spec_build_action_reply(0x010a, 0, out, sizeof(out));
    const uint8_t want[] = {0x07, 0x20, 0x0a, 0x01, 0x06, 0x00, 0x00};
    CHECK(n == 7, "action reply length");
    expect_bytes(out, want, sizeof(want), "action reply bytes");
}

static void test_props_changed(void)
{
    /* op=4 帧体首字节必须是 count（gyb 解析器要求） */
    uint8_t out[48];
    miot_spec_value_t pc[2];
    memset(pc, 0, sizeof(pc));
    pc[0].siid = 11; pc[0].piid = 2; pc[0].type = MIOT_TYPE_U32; pc[0].val.u32 = 0x00123456;
    pc[1].siid = 11; pc[1].piid = 3; pc[1].type = MIOT_TYPE_U32; pc[1].val.u32 = 0x5f000000;

    int n = miot_spec_build_props_changed(0x0001, pc, 2, out, sizeof(out));
    CHECK(n == 24, "props_changed length");
    CHECK(out[4] == MIOT_OP_PROP_CHANGED, "opcode=4");
    CHECK(out[5] == 2, "leading count byte");
    /* 第一个对象：siid=11, piid=2 (16LE), type16=(4<<12)|4=0x4004, value 4B */
    CHECK(out[6] == 11 && out[7] == 0x02 && out[8] == 0x00, "obj1 siid/piid");
    CHECK(out[9] == 0x04 && out[10] == 0x40, "obj1 type16");
    CHECK(out[11] == 0x56 && out[12] == 0x34 && out[13] == 0x12, "obj1 value LE");
}

static void test_parse_get(void)
{
    /* App 发的 get 请求（实测样本）：[len16][tid][02][count][siid][piid16] */
    const uint8_t req[] = {0x0c, 0x20, 0x34, 0x12, 0x02, 0x02, 0x02, 0xe9, 0x03, 0x03, 0xeb, 0x03};
    miot_spec_packet_t pkt;
    int rc = miot_spec_parse(req, sizeof(req), &pkt);
    CHECK(rc == 0, "parse get rc");
    CHECK(pkt.op == MIOT_OP_GET && pkt.tid == 0x1234, "parse get hdr");
    CHECK(pkt.argc == 2, "parse get argc");
    CHECK(pkt.args[0].siid == 2 && pkt.args[0].piid == 0x03e9, "parse get obj1");
    CHECK(pkt.args[1].siid == 3 && pkt.args[1].piid == 0x03eb, "parse get obj2");
}

static void test_parse_action(void)
{
    /* action：[len16][tid][05][siid][aiid16][argc]... */
    const uint8_t req[] = {0x0a, 0x20, 0x0a, 0x05, 0x05, 0x0d, 0x01, 0x05, 0x00};
    miot_spec_packet_t pkt;
    int rc = miot_spec_parse(req, sizeof(req), &pkt);
    CHECK(rc == 0, "parse action rc");
    CHECK(pkt.op == MIOT_OP_ACTION && pkt.siid == 0x0d, "parse action siid");
    CHECK(pkt.piid == 0x0501, "parse action aiid");
    CHECK(pkt.argc == 0, "parse action argc");
}

static void test_version_exchange(void)
{
    const uint8_t req[] = {0x05, 0x20, 0x01, 0x00, 0xF0};
    miot_spec_packet_t pkt;
    CHECK(miot_spec_parse(req, sizeof(req), &pkt) == 0, "parse F0");
    CHECK(pkt.op == MIOT_OP_VERSION_XCH, "parse F0 op");

    uint8_t out[8];
    int n = miot_spec_build_version_reply(0x0001, out, sizeof(out));
    const uint8_t want[] = {0x05, 0x20, 0x01, 0x00, 0xF0};
    CHECK(n == 5, "F0 reply length");
    expect_bytes(out, want, sizeof(want), "F0 reply bytes");
}

/* ---------- ag2 重组：App START + 2 DATA 帧 → 一个完整 k61 帧 ---------- */

static uint8_t s_rx_done[256];
static size_t s_rx_done_len;

static void fake_send(void *u, const uint8_t *f, size_t n) { (void)u; (void)f; (void)n; }
static void ag2_on_frame(void *u, const uint8_t *k, size_t n)
{
    (void)u;
    memcpy(s_rx_done, k, n);
    s_rx_done_len = n;
}

static void test_ag2_reassembly(void)
{
    miot_ag2_t a;
    miot_ag2_init(&a);
    a.send = fake_send;
    a.on_frame = ag2_on_frame;

    const uint8_t payload[30] = {0};   /* 待重组的 k61 帧（30B → 2 个数据帧） */
    uint8_t f1[20], f2[20];
    f1[0] = 0; f1[1] = 0; f1[2] = 0; f1[3] = 0; f1[4] = 2; f1[5] = 0;  /* START count=2 */
    miot_ag2_rx(&a, f1, 6);
    CHECK(s_rx_done_len == 0, "ag2 no frame before data");

    f1[0] = 1; f1[1] = 0;                       /* DATA seq=1，18B 载荷 */
    memcpy(f1 + 2, payload, 18);
    miot_ag2_rx(&a, f1, 20);
    CHECK(s_rx_done_len == 0, "ag2 not complete after 1st data");

    f2[0] = 2; f2[1] = 0;                       /* DATA seq=2，12B 载荷 */
    memcpy(f2 + 2, payload + 18, 12);
    miot_ag2_rx(&a, f2, 14);
    CHECK(s_rx_done_len == 30, "ag2 complete after 2nd data");
    CHECK(memcmp(s_rx_done, payload, 30) == 0, "ag2 payload intact");
}

/* ---------- ag2 发送：k61 帧 → START + 分帧（每帧 ≤18B） ---------- */

static int s_tx_frames;
static size_t s_tx_start_count;

static void tx_counter(void *u, const uint8_t *f, size_t n)
{
    (void)u; (void)n;
    if (f[0] == 0 && f[1] == 0 && f[2] == 0 && n == 6) s_tx_start_count = f[4] | (f[5] << 8);
    s_tx_frames++;
}

static void test_ag2_send_segmentation(void)
{
    miot_ag2_t a;
    miot_ag2_init(&a);
    a.send = tx_counter;
    s_tx_frames = 0; s_tx_start_count = 0;

    uint8_t k61[40] = {0};       /* 40B → START + ceil(40/18)=3 数据帧 */
    miot_ag2_send(&a, k61, 40);
    CHECK(s_tx_frames == 4, "ag2 send frame count");
    CHECK(s_tx_start_count == 3, "ag2 start count");
}

int main(void)
{
    printf("== miot-ble-sdk host tests ==\n");
    test_event_81();
    test_event_82();
    test_action_reply();
    test_props_changed();
    test_parse_get();
    test_parse_action();
    test_version_exchange();
    test_ag2_reassembly();
    test_ag2_send_segmentation();
    if (g_fail == 0) printf("ALL TESTS PASSED\n");
    else             printf("%d FAILURES\n", g_fail);
    return g_fail ? 1 : 0;
}
