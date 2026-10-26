# asterisk-chan-e5

Asterisk 通道驱动：把荣悦 E5（Unisoc SharkL5 一体 SoC）的**蜂窝语音与短信**桥接进 Asterisk 拨号计划，目标是在设备上跑 OpenWrt 25.12 + Asterisk 23.x，作为 GSM 网关使用。

- 信令面：**不绕过 ModemManager**，只走 `org.freedesktop.ModemManager1` D-Bus 接口（Voice / Messaging / Call / Sms）。不打开 `/dev/stty_nr*`、不自拼 AT，避免与打了 unisoc 补丁的 MM 抢 SIPC 通道。
- 媒体面：AP **tap 音频桥**（AGDSP 分流端点），不与屏幕电话的 `e5-voice-audio` 抢常规通话路由。
- 短信：接收事件注入拨号计划上下文；发送提供 `E5SendSMS` 应用 + `e5 sms send` CLI。

---

## 1. 目录结构

```
asterisk-chan-e5/
├── Makefile                       OpenWrt 25.12 包（交叉编译）
├── src/
│   ├── chan_e5.c                  通道驱动主体（tech 回调 / CLI / SMS 应用）
│   ├── chan_e5.exports            version script
│   ├── e5_mm.[ch]                 ModemManager D-Bus 客户端
│   ├── e5_audio.[ch]              ALSA tap 音频桥
│   ├── e5_route.[ch]              tap 所有权切换（调用 e5-tap-up/down）
│   └── Makefile                   本地/SDK 直编
├── configs/
│   ├── e5.conf                    端点 + 上下文配置
│   └── extensions_e5.conf         拨号计划样例
└── files/usr/libexec/asterisk/
    ├── e5-tap-up                  接管 tap（上电 SWITCH + DSP 模式）
    └── e5-tap-down                释放 tap（恢复屏幕电话路由）
```

---

## 2. 架构

```
                     ModemManager (D-Bus)
                          │  Voice.CallAdded / Call PropertiesChanged
                          │  Messaging.Added
        ┌─────────────────┼──────────────────┐
        │  e5_mm.c (GIO 事件线程)             │
        └────────┬────────┬──────────────────┘
   状态事件        │        │  SMS 事件
        ┌─────────▼──┐     └──────────► Local 通道 -> [sms_context]
        │  chan_e5   │
        │  tech 回调  │  E5/<slot>/<num>  <── Dial() 外呼
        │  e5_audio   │  e5 call CLI      <── 控制台主动外呼
        └─────┬──────┘
              │  tap 端点
   capture  hw:0,9   (对端语音 -> Asterisk, S_VOICE_CAP_C)
   playback hw:0,18  (Asterisk -> 对端,   S_VOICE_PCM_P)
```

每个 SIM 卡（`sim1`/`sim2`）同一时刻只有一个活跃通话 pvt（单呼）。来电占用该卡槽后，同一卡的新来电不再接管（留给屏幕电话）。

---

## 3. 编译

### 3.1 OpenWrt 25.12 交叉编译

```sh
# 把本目录软链/拷贝进 package/
cp -r asterisk-chan-e5 $OWRT/package/
cd $OWRT
./scripts/feeds install asterisk glib2 alsa-lib
make menuconfig        # 勾选 Network -> Telephony -> asterisk-chan-e5
make package/asterisk-chan-e5/compile
```

产物：`chan_e5.so` 安装到 `/usr/lib/asterisk/modules/`。

### 3.2 设备上本地直编

前提：rootfs 已装 asterisk 头文件（`/usr/include/asterisk`）与 `glib-2.0/gio-2.0`、`alsa-lib` 开发包：

```sh
cd src && make
make install        # 装 chan_e5.so 到 /usr/lib/asterisk/modules
```

交叉编译指定工具链：

```sh
cd src
make CC=aarch64-openwrt-linux-musl-gcc \
     AST_DEV_DIR=$OWRT/staging_dir/target-*/usr/include \
     AST_LIB_DIR=$OWRT/staging_dir/target-*/usr/lib
```

---

## 4. 配置

### 4.1 `e5.conf`

关键字段（完整见 `configs/e5.conf`）：

| 字段 | 段 | 说明 |
|---|---|---|
| `sms_context` | general | 短信接收事件注入的上下文（默认 `e5-sms-in`） |
| `call_context` | general | `e5 call` CLI 外呼的呼叫规则上下文（默认 `e5-outbound`） |
| `sms_delete_after` | general | 短信进拨号计划后是否删除 SIM 上的 SMS 对象 |
| `band_file` | general | DSP 带宽联动文件（0=8k / 1=16k） |
| `context` | simN | 该卡**来电**进入的上下文 |
| `capture_dev` / `playback_dev` | simN | 音频 tap 端点，默认 `hw:0,9` / `hw:0,18` |
| `capture_rate` / `playback_rate` | simN | 下行 / 上行采样率，**独立配置**（默认 8000；缺省回退到 `rate`）。实机上行 tap 通常 16k、下行 8k |
| `dump_capture` / `dump_playback` | simN | 音频原始 PCM dump（诊断用，默认 `no`）。`yes` 时分别写到 `/tmp/e5_cap_2down_dump.raw` / `/tmp/e5_cap_2up_dump.raw` |
| `route_up` / `route_down` | simN | tap 上下电脚本，默认 `e5-tap-up` / `e5-tap-down` |

> 若 `capture_dev` / `playback_dev` 留空，模块工作在**信令态**：可接听/挂断/状态，但无语音桥。

### 4.2 音频路径（实机验证）

- 上行注入（Asterisk → 对端）：`S_VOICE_PCM_P SWITCH on` + `aplay -D hw:0,18`。
- 下行采集（对端 → Asterisk）：`S_VOICE_CAP_C SWITCH on` + `arecord -D hw:0,9`。
- 验证方式：拨通 10000 后 `arecord -D hw:0,9 ... | aplay -D hw:0,0 ...` 能听到对端语音。

`e5-tap-up` 会在通话接通时把这两个 SWITCH 打开并设置 DSP 模式；`e5-tap-down` 在挂断后关掉并恢复屏幕电话路由。若你的设备控件名不同，只需改这两个脚本，无需改 C 代码。

---

## 5. 控制台命令

| 命令 | 作用 |
|---|---|
| `e5 show status` | ModemManager 在线状态、各卡 context/rate/tap、当前通话与媒体状态 |
| `e5 show calls` | 列出 MM 当前全部 Call 对象（含非本模块接管的） |
| `e5 call <sim1\|sim2\|1\|2> <号码>` | 从指定卡主动外呼，进入 `call_context` |
| `e5 sms send <号码> <文本...>` | 经 MM 发短信（默认卡） |

在 `asterisk -r` 里执行即可。

### `e5 call` 的呼叫规则如何交给 extensions.conf

`e5 call sim1 18751886301` 会创建出站 E5 通道并 `ast_pbx_start` 到 `call_context`（默认 `e5-outbound`）。你在 `extensions.conf` 里决定接通后做什么：

```ini
[e5-outbound]
exten => _X.,1,Dial(PJSIP/70,60)   ; 外呼建立后桥到分机 70
```

同一上下文可用 `${EXTEN}`（被叫号码）做分支路由。

---

## 6. 短信上下文挂接

### 6.1 接收事件

MM 的 `Messaging.Added(received=true)` 触发后，chan_e5 创建一个 `Local/s@<sms_context>` 通道注入拨号计划，并写入通道变量：

| 变量 | 内容 |
|---|---|
| `SMS_FROM` | 对端号码 |
| `SMS_TEXT` | 短信文本 |
| `SMS_TS` | 时间戳 |
| `SMS_PATH` | MM Sms 对象路径 |

```ini
[e5-sms-in]
exten => s,1,NoOp(短信来自 ${SMS_FROM}: ${SMS_TEXT})
 same => n,System(/usr/local/bin/e5-sms-hook "${SMS_FROM}" "${SMS_TEXT}")
 same => n,Hangup()
```

### 6.2 发送

- 拨号计划内：`E5SendSMS(号码,文本)`。
- 控制台：`e5 sms send 号码 文本`。

`extensions.conf` 里典型的短信发送上下文：

```ini
[e5-sms-out]
exten => _X.,1,E5SendSMS(${EXTEN},${MESSAGE})
```

---

## 7. 完整拨号计划示例

`configs/extensions_e5.conf` 已在主 `extensions.conf` 中 include：

```ini
#include "extensions_e5.conf"
```

涵盖：来电转分机（`from-e5-sim1/2`）、分机外呼（`9`→SIM1、`8`→SIM2）、`e5 call` 出站规则、短信收发上下文。

---

## 8. 行为红线与已知边界

- **不回退打开 host 麦克风**：通话中开麦会被厂商语音路由从基带抢走，基带约 20s 拆线。tap 下行打不开时进入“静音模式”（仅上行注入），保持 RTP 存活。
- **写操作不重试**：MM 的 CreateCall/Start/Accept/Hangup 结果以 Call 状态属性变化为唯一权威（本仓库 MM 打了 `unisoc-call-recovery` / `unisoc-hangup-settle` 补丁，网络侧异常会自动恢复，重复拨号反而会误伤）。
- **单呼**：每卡同一时刻一个活跃通话；MM 里的会议/等待（held/waiting）不接管。
- **短信选卡**：stock MM `Messaging` 接口无 `SimSlot` 属性，短信发送走默认卡。多卡短信按 SIM 分流需在 `e5.conf` 之上配合 `e5-sms-receive`/`sms-forward`（e5-linux 的短信网关服务）完成，本模块只做“事件注入拨号计划 + 发送”。
- **编译前提**：需要 Asterisk 23.x 头文件（`ast_channel_alloc` 无 `linkedid` 参数、`AST_MODULE_INFO` 自动注册、`send_digit_begin`）。

---

## 9. 验证清单

1. `asterisk -r` → `module show like chan_e5` 应显示已加载。
2. `e5 show status` 显示 ModemManager 在线、SIM 卡 context 与 tap 端点。
3. 手机呼入设备 SIM1 → `e5 show calls` 出现 ringing-in → 分机 70 振铃接听 → 双向语音。
4. 分机拨 `9<手机号>` → E5 外呼 → 对端接听 → 双向语音。
5. `e5 call sim1 <手机号>` → 分机 70 振铃接听 → 双向语音。
6. `e5 sms send <手机号> 测试` → 对端收到；对端回短信 → `[e5-sms-in]` 触发。
