/*
 * e5_mm.h - ModemManager D-Bus 客户端（E5 蜂窝语音控制面）
 *
 * 设计原则：
 *  - 绝不打开 /dev/stty_nr*，绝不自拼 AT；ModemManager 的 unisoc 插件独占 SIPC 通道。
 *  - 只使用 org.freedesktop.ModemManager1 稳定接口（MM 1.20+，设备上为打过补丁的 1.24）。
 *  - 所有写操作不重试：CreateCall/Start/Accept/Hangup/SendDtmf 的结果以
 *    Call 对象 State 属性变化为权威来源。
 */
#ifndef E5_MM_H
#define E5_MM_H

#include <gio/gio.h>
#include <stdint.h>
#include <pthread.h>

/* MM_CALL_STATE_*（ModemManager-enums.h 顺序） */
enum e5_call_state {
	E5_CALL_UNKNOWN     = 0,
	E5_CALL_DIALING     = 1,
	E5_CALL_RINGING_OUT = 2,
	E5_CALL_RINGING_IN  = 3,
	E5_CALL_ACTIVE      = 4,
	E5_CALL_HELD        = 5,
	E5_CALL_WAITING     = 6,
	E5_CALL_TERMINATED  = 7,
};

enum e5_call_direction {
	E5_DIR_UNKNOWN  = 0,
	E5_DIR_INCOMING = 1,
	E5_DIR_OUTGOING = 2,
};

/* MM_CALL_STATE_REASON_*（仅映射 chan 层需要的子集） */
enum e5_state_reason {
	E5_REASON_UNKNOWN             = 0,
	E5_REASON_OUTGOING_STARTED    = 1,
	E5_REASON_INCOMING_NEW        = 2,
	E5_REASON_ACCEPTED            = 3,
	E5_REASON_TERMINATED          = 4,
	E5_REASON_REFUSED_OR_BUSY     = 5,
	E5_REASON_ERROR               = 6,
	E5_REASON_AUDIO_SETUP_FAILED  = 7,
};

struct e5_mm_call {
	char *path;                 /* /org/freedesktop/ModemManager1/Call/N */
	char *number;
	int   state;                /* enum e5_call_state */
	int   reason;               /* enum e5_state_reason */
	int   direction;            /* enum e5_call_direction */
	int   sim_slot;             /* 1 或 2；0=未知 */
	int   multiparty;
};

/* MM SMS 对象（org.freedesktop.ModemManager1.Sms 属性子集） */
struct e5_mm_sms {
	char *path;                 /* /org/freedesktop/ModemManager1/SMS/N */
	char *number;               /* 对端号码 */
	char *text;                 /* 文本（PDU 型由 MM 解码） */
	char *timestamp;            /* ISO8601，可空 */
	int   state;                /* MM_SMS_STATE_* */
	int   pdu_type;             /* 1=deliver(收) 2=submit(发) */
};

struct e5_mm;

/* 事件回调（在 GMainLoop 专用线程触发；实现方需自行加锁/投递到 asterisk 线程）。
 * 约定：这些回调内不得再发起同步 D-Bus 调用（e5_mm 会在回调内持有 mm->lock，
 * 同步调用会嵌套迭代 mm->ctx 导致重入死锁）。 */
struct e5_mm_events {
	void (*call_added)(struct e5_mm *mm, const struct e5_mm_call *c, void *user);
	void (*call_removed)(struct e5_mm *mm, const char *path, void *user);
	void (*call_state_changed)(struct e5_mm *mm, const char *path,
		int state, int reason, void *user);
	void (*service_changed)(struct e5_mm *mm, int available, void *user);
	/* 收到新短信（Messaging.Added(received=true)）；回调返回后对象即释放 */
	void (*sms_received)(struct e5_mm *mm, const struct e5_mm_sms *sms, void *user);
	void *user;
};

/* 进程级初始化：连 system bus、起 GMainLoop 线程、枚举 modem。可重复安全调用。 */
struct e5_mm *e5_mm_init(const struct e5_mm_events *ev);
void e5_mm_shutdown(struct e5_mm *mm);

/* 当前是否找到带 Voice 接口的 modem */
int e5_mm_online(struct e5_mm *mm);

/* 查询 modem 对象路径（供状态展示）；返回 online，pathbuf 可为 NULL */
int e5_mm_query(struct e5_mm *mm, char *pathbuf, size_t len);

/* 同步操作（内部串行化），成功返回 0。number 需已按 ^\+?[0-9*#]{3,32}$ 校验。
 * CreateCall+Start 合并为 dial；out_call_path 成功时返回新 Call 路径（调用方 g_free）。 */
int e5_mm_dial(struct e5_mm *mm, int sim_slot, const char *number, char **out_call_path);
int e5_mm_answer(struct e5_mm *mm, const char *call_path);
int e5_mm_hangup(struct e5_mm *mm, const char *call_path);
int e5_mm_send_dtmf(struct e5_mm *mm, const char *call_path, char digit);

/* 刷新全部活动 Call（启动/MM 重启后对账）。 */
int e5_mm_list_calls(struct e5_mm *mm,
	void (*cb)(const struct e5_mm_call *c, void *user), void *user);

/* 短信（Modem.Messaging）：Create+Send 合并；不指定 SIM 槽（stock MM
 * Messaging 无 SimSlot 属性，走默认卡）。号码需已按号码正则校验；成功返回 0。 */
int e5_mm_sms_send(struct e5_mm *mm, const char *number, const char *text);

/* 删除一条 SMS 对象（网关场景避免 SIM 卡短信存满后静默丢新短信） */
int e5_mm_sms_delete(struct e5_mm *mm, const char *sms_path);

/* 工具：MM 状态值 -> 名称（日志用） */
const char *e5_mm_state_name(int state);

#endif
