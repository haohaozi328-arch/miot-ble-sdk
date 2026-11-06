/**
 * @file miot_beacon.h
 * @brief MiBeacon v5 加密广播帧 —— 覆盖不建连接的传感器设备
 *
 * 适用设备：甲醛检测仪 JQJCY01YM、门窗传感器2 MCCGQ02HL、人体传感器2 RTCGQ02LM、
 * 水浸传感器 SJWS01LM、温湿度计2 t9 / 3 mini MJWSD06MMC、体脂秤2 XMTZC05HM 探针等。
 *
 * 帧格式（真机 btsnoop/ESPHome 互证实测）：
 *   FC(2,LE) | PID(2,LE) | frame_cnt(1) | MAC(6,空中序)
 *   | 对象密文(N) | counter_ext(3) | MIC(4)
 *   FC byte0 = 0x58: obj(0x40)|mac(0x10)|enc(0x08)
 *   FC byte1 = 0x59: version=5|registered=1（0x58=未注册被 App 无视！）
 *   加密：AES-CCM(beaconkey 16B)，nonce = MAC(6)|pid_le(2)|cnt_le(4)，
 *         aad = 0x11，tag 4B 放帧尾（SDK 解析：先取末 4B 为 MIC，再往前 3B 为 counter）
 *   帧内必须带 MAC 字段：App 按帧内 MAC 做设备归属判定。
 */
#ifndef MIOT_BEACON_H
#define MIOT_BEACON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 标准 MiBeacon 对象 ID（实测自真机广播） ---------- */
#define MB_OBJ_MOTION_LIGHT   0x000F  /* 3B：b1=移动 b2=光照>0 有光（人体/夜灯共用） */
#define MB_OBJ_BUTTON         0x1001  /* 3B：类型/步数/动作 0=单击 1=双击 2=长按 */
#define MB_OBJ_TEMP_I16       0x1004  /* 2B：int16 0.1℃（t9/cgdk3/JQJCY01YM） */
#define MB_OBJ_HUM_U16        0x1006  /* 2B：uint16 0.1% */
#define MB_OBJ_BATTERY        0x100A  /* 1B：电量 % */
#define MB_OBJ_HCHO_U16       0x1010  /* 2B：uint16 0.01 mg/m³（甲醛） */
#define MB_OBJ_SWITCH         0x1012  /* 1B：0=关 1=开（驱蚊器 WX08ZM） */
#define MB_OBJ_MARGIN         0x1013  /* 1B：驱蚊片余量 % */
#define MB_OBJ_LEAK           0x1014  /* 1B：0=正常 >0=漏水（SJWS01LM） */
#define MB_OBJ_NO_MOTION      0x1017  /* 4B：距上次移动秒数（RTCGQ02LM） */
#define MB_OBJ_LIGHT          0x1018  /* 1B：0=暗 非0=亮（MCCGQ02HL） */
#define MB_OBJ_DOOR           0x1019  /* 1B：0=开 1=关 2=超时未关 3=复位（MCCGQ02HL） */
#define MB_OBJ_TEMP_F32       0x4C01  /* 4B：float32 ℃（MJWSD06MMC，miot 2.1001，mi=19457） */
#define MB_OBJ_HUM_U8         0x4C02  /* 1B：uint8 %（MJWSD06MMC，miot 2.1002，mi=19458） */
#define MB_OBJ_BATT_U8_48     0x4803  /* 1B：电量 %（MJWSD06MMC，miot 3.1003，mi=18435） */
/* 无线开关事件对象：载荷恒 1B = 按键号（单键恒 1；双键 1=左 2=右）。
 * 三键位分开成三个对象，App 侧据此拆成「单击/双击/长按」三个触发器。
 * 实测：XMWXKG01YL（0x1949 双键）与 XMWXKG01CM（0x2387 单键）通用。 */
#define MB_OBJ_BTN_CLICK      0x4E0C  /* 1B：单击 */
#define MB_OBJ_BTN_DOUBLE     0x4E0D  /* 1B：双击 */
#define MB_OBJ_BTN_HOLD       0x4E0E  /* 1B：长按 */

/** beacon 广播配置（认证/绑定阶段获得，需持久化） */
typedef struct miot_beacon_cfg {
    uint16_t pid;                    /* 产品 ID（米家后台分配，如 S400=0x30D9） */
    uint8_t  mac_air[6];             /* 广播 MAC，空中（LSB）序 */
    uint8_t  beacon_key[16];         /* beaconkey：绑定时生成（随机或派生），掉电保存 */
} miot_beacon_cfg_t;

/* ---------- 对象编码辅助（返回写入字节数） ---------- */

size_t miot_beacon_obj_u8(uint8_t *p, uint16_t obj_id, uint8_t v);
size_t miot_beacon_obj_u16(uint8_t *p, uint16_t obj_id, uint16_t v);
size_t miot_beacon_obj_i16(uint8_t *p, uint16_t obj_id, int16_t v);
size_t miot_beacon_obj_u32(uint8_t *p, uint16_t obj_id, uint32_t v);
size_t miot_beacon_obj_f32(uint8_t *p, uint16_t obj_id, float v);
size_t miot_beacon_obj_bytes(uint8_t *p, uint16_t obj_id, const uint8_t *v, size_t len);

/**
 * 构造 MiBeacon v5 加密数据帧。
 * @param objs     已编码的对象明文（miot_beacon_obj_xxx 产出）
 * @param out      输出帧（放入广播 manufacturer data，前加公司码 0x038F 时总长 ≤31B）
 * @return 帧长，负数失败（未注册加密后端 / objs 过长 / cap 不足）
 *
 * 注意：单帧对象总明文 ≤13B 才能保证含公司码的广播 ≤31B；
 * 多对象时按「三路轮换」分帧（温度/湿度/电量各占一帧交替广播）。
 *
 * ⚠️ 31B 硬上限（v85 实测定案）：广播包 = flags(3) + service data
 *    「长度1 + UUID2 + 95FE2 + 帧」，即**帧本身可用 ≤26B**。超限时 NimBLE 返回
 *    rc=4（BLE_HS_EMSGSIZE）；若此时广播已被 stop，设备会静默到下一个心跳，
 *    表现为「App 一直正在连接 → 超时」。规避顺序：
 *    ① 拆帧：一件事一个对象，别把多对象塞进一帧；
 *    ② 仍超限 → 改「无 MAC 形态」：FC 由 58/59 改 48/49 并跳过 6B MAC 字段
 *       （真机 24B 测量帧同款，网关按广播源地址归属）。本函数只产出带 MAC 形态，
 *       无 MAC 形态需调用方按此规则自行裁剪，详见 docs/testing.md 踩坑 6。
 */
int miot_beacon_frame_v5(const miot_beacon_cfg_t *cfg, uint32_t frame_cnt,
                         const uint8_t *objs, size_t objs_len,
                         uint8_t *out, size_t cap);

/* ---------- 未绑定（可被 App 发现）广播帧与扫描探测 ---------- */

/**
 * 构造绑定前广播帧（未加密，App 靠它发现设备并列出「添加」入口）。
 *
 *   FC(2) | PID(2 LE) | frame_cnt(1) | MAC(6 空中序) | capability(1) = 12B
 *
 * 基线取自标准认证型号实测：fc0=0x30（MAC|Capability，未加密不含 Object）、
 * fc1=0x58（v5 | authMode=2 | registered=0）、capability=0x10（BondAbility=2<<3）。
 * ⚠️ PSK 型号（驱蚊器 0x040A 等）App 扫描阶段认哪组值从未实测过 —— 用
 * miot_adv_probe_forms() 的候选表轮播探测，命中形态由 GAP 连接回调打日志。
 *
 * @return 帧长（12），负数失败（cap 不足）
 */
int miot_beacon_frame_unbound(uint16_t pid, const uint8_t mac_air[6],
                              uint8_t fc0, uint8_t fc1, uint8_t capability,
                              uint8_t frame_cnt, uint8_t *out, size_t cap);

/** 一个广播形态：fc1 低 3 位按基线解码 = bit0 registered | bit2..3 authMode */
typedef struct miot_adv_form {
    uint8_t       fc0;
    uint8_t       fc1;
    uint8_t       capability;
    const char   *desc;          /* 串口打印用，ASCII */
} miot_adv_form_t;

/**
 * PSK 扫描探测候选形态表（按可能性排序，第 0 项是标准认证基线）。
 * 轮播法：每 5s 换下一项重建广播；App 一旦发起连接，立即在串口打
 * 「SCANNED-ON: form i」——那一种就是该型号被 App 认可的形态，
 * 实测后把它固化为默认，其余项从表里删掉。
 */
const miot_adv_form_t *miot_adv_probe_forms(size_t *count);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_BEACON_H */
