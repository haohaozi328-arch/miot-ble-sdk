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
#define MB_OBJ_TEMP_F32       0x4801  /* 4B：float32 ℃（MJWSD06MMC 新批次） */
#define MB_OBJ_HUM_U8         0x4802  /* 1B：uint8 %（MJWSD06MMC） */
#define MB_OBJ_BATT_U8_48     0x4803  /* 1B：电量 %（MJWSD06MMC） */

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
 */
int miot_beacon_frame_v5(const miot_beacon_cfg_t *cfg, uint32_t frame_cnt,
                         const uint8_t *objs, size_t objs_len,
                         uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_BEACON_H */
