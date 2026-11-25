/*
 * hook_ble_mesh_gate.js —— 绕开米家 App「Mesh 设备必须由网关配网」的判定
 *
 * 目标代码（com.xiaomi.smarthome.device.bluetooth.connect.single.BleConnectActivity#access$700）：
 *
 *   PluginDeviceInfo info = ...Oooo0oO(device.model);
 *   if (info == null || info.Oooo000 != Device.PID_BLE_MESH) {   // PID_BLE_MESH = 16
 *       ... if (packet.frameControl.authMode == 2) -> "Start bleStandardAuthConnect"
 *   }
 *   "Start bleMeshConnect"        // ← 0x0926 每次都掉到这里，连上后零写入被断
 *
 * 做法：把 Device.PID_BLE_MESH 从 16 改成 -1。
 * 该字段是 public static int（非 final），字节码里是 sget 运行期读，
 * 改完所有 `!= PID_BLE_MESH` 判断都会走「非 Mesh」分支 → 标准认证直连。
 *
 * 用法（需要 root + frida-server）：
 *   frida -U -f com.xiaomi.smarthome -l hook_ble_mesh_gate.js --no-pause
 *
 * 验证：logcat 里应出现
 *   BleConnectActivity Start bleStandardAuthConnect
 * 而不是
 *   BleConnectActivity Start bleMeshConnect
 */
Java.perform(function () {
  var TAG = "[hook-mesh-gate] ";

  function trySet(className, fieldName, value) {
    try {
      var C = Java.use(className);
      C[fieldName].value = value;
      console.log(TAG + className + "." + fieldName + " = " + value + "  OK");
      return true;
    } catch (e) {
      console.log(TAG + className + "." + fieldName + " 失败: " + e);
      return false;
    }
  }

  // 1) 核心：把 Mesh 类型判定值废掉
  trySet("com.xiaomi.smarthome.device.Device", "PID_BLE_MESH", -1);

  // 2) 观察：确认 BleConnectActivity 到底选了哪条路
  try {
    var Act = Java.use("com.xiaomi.smarthome.device.bluetooth.connect.single.BleConnectActivity");
    Act.access$700.implementation = function (a) {
      var r = this.access$700(a);
      return r;
    };
    console.log(TAG + "BleConnectActivity.access$700 hooked (观察用)");
  } catch (e) {
    console.log(TAG + "access$700 未 hook（可能已内联）: " + e);
  }

  // 3) 兜底：万一仍走到 bleMeshConnect，打印调用来源便于定位
  try {
    var Mgr = Java.use("kotlinx.coroutines.flow.core.server.internal.bluetooth.OooOOO0");
    Mgr.bleMeshConnect.overload("java.lang.String", "java.lang.String", "kotlinx.coroutines.flow.bluetooth.Response$BleConnectResponse").implementation = function (a, b, c) {
      console.log(TAG + "!!! bleMeshConnect 仍被调用 mac=" + a);
      return this.bleMeshConnect(a, b, c);
    };
  } catch (e) {
    console.log(TAG + "bleMeshConnect 兜底 hook 跳过: " + e);
  }

  console.log(TAG + "ready");
});
