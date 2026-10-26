/*
 * chan_e5.c - Asterisk 通道驱动：荣悦 E5（Unisoc SharkL5 一体 SoC）蜂窝语音
 *
 * 通道类型: E5
 *   出站拨号（分机侧 Dial）: E5/1/10086  （E5/<sim-slot 1|2>/<号码>）
 *   出站拨号（控制台侧）  : CLI "e5 call sim1 10086"，进入 call_context
 *   入站:     由 e5.conf 中每卡 context 进入拨号计划
 *
 * 信令面: ModemManager D-Bus（e5_mm.c），不使用 AT 串口。
 * 媒体面: AP tap 音频桥（e5_audio.c），tap 未就绪时仅信令可用。
 */
#include "e5_mm.h"
#include "e5_audio.h"
#include "e5_route.h"

/* Asterisk 23+ 的 asterisk.h 要求：每个 extern 模块源文件都必须在包含
 * asterisk.h 之前声明 AST_MODULE_SELF_SYM，且 AST_MODULE 也必须存在
 * （ast_debug/ast_log 依赖它）。两者均由构建系统以 -D 全局传入：
 *   -DAST_MODULE_SELF_SYM=__chan_e5_module -DAST_MODULE=\"chan_e5\"
 * （见顶层 Makefile 与 src/Makefile）。 */

#include <asterisk.h>
#include <asterisk/module.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/lock.h>
#include <asterisk/utils.h>
#include <asterisk/config.h>
#include <asterisk/causes.h>
#include <asterisk/app.h>
#include <asterisk/cli.h>
#include <asterisk/manager.h>
#include <asterisk/indications.h>
#include <asterisk/format_cache.h>
#include <asterisk/format_cap.h>
#include <asterisk/strings.h>
#include <asterisk/timing.h>
/* 注意：不 include <asterisk/autoservice.h>。OpenWrt 25.12 的 asterisk 包
 * 未把该头文件装进 staging；本驱动出站 .call 直接阻塞（3s 超时），无需 autoservice。 */

#include <regex.h>

#define E5_TECH        "E5"
#define E5_MAX_SIM     2
#define NUMBER_RE      "^\\+?[0-9*#]{3,32}$"

AST_MUTEX_DEFINE_STATIC(users_lock);

struct e5_endpoint_cfg {
	int configured;
	int sim_slot;                       /* 1/2 */
	char context[AST_MAX_CONTEXT];      /* 来电 context */
	char group_name[AST_MAX_EXTENSION];
	struct e5_audio_cfg audio;
	struct e5_route_cfg route;
	char capture_dev[128];
	char playback_dev[128];
	char up_cmd[256];
	char down_cmd[256];
	int cap_rate;                       /* 下行采集采样率 */
	int pb_rate;                        /* 上行注入采样率 */
	int dump_capture;                   /* 1=下行采集 dump（默认 0） */
	int dump_playback;                  /* 1=上行注入 dump（默认 0） */
	int rxgain, txgain;
};

struct e5_pvt {
	struct e5_endpoint_cfg *cfg;
	struct ast_channel *chan;           /* 绑定的 Asterisk 通道（单呼） */
	char call_path[128];                /* MM Call 对象 */
	char number[64];
	int direction;                      /* E5_DIR_* */
	int state;                          /* E5_CALL_* */
	int alive;
	ast_mutex_t lock;
	struct e5_audio *audio;
	struct e5_route *route;
	struct ast_timer *timer;            /* 20ms 定时器：驱动 read 节奏 */
	int awaiting_mm;                    /* 状态待 MM 对账 */
	int media_started;                  /* tap/音频只尝试一次 */
};

static struct e5_endpoint_cfg ep_cfg[E5_MAX_SIM];
static struct e5_pvt *pvt_slot[E5_MAX_SIM]; /* 每卡一个活跃 pvt */
static struct e5_mm *g_mm;
static regex_t number_re;

/* [general]：短信网关 + 出站 context + DSP 带宽联动 */
static char sms_context[AST_MAX_CONTEXT] = "e5-sms-in";
static char call_context[AST_MAX_CONTEXT] = "e5-outbound";
static int sms_delete_after = 1;
static char band_file[128] = "/run/e5-voice-audio-tap/band";

static struct ast_channel_tech e5_tech; /* 前向声明，供 e5_new 引用 */

/* ---------------- 配置 ---------------- */

static void load_config(int reload)
{
	struct ast_flags cfg_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	struct ast_config *cfg = ast_config_load2("e5.conf", E5_TECH, cfg_flags);
	const char *cat = NULL;

	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_NOTICE, "e5.conf 缺失，chan_e5 不注册端点\n");
		if (cfg && cfg != CONFIG_STATUS_FILEUNCHANGED)
			ast_config_destroy(cfg);
		return;
	}
	if (cfg == CONFIG_STATUS_FILEUNCHANGED)
		return;

	memset(ep_cfg, 0, sizeof(ep_cfg));

	while ((cat = ast_category_browse(cfg, cat))) {
		if (!strcasecmp(cat, "general")) {
			const char *v;
			if ((v = ast_variable_retrieve(cfg, cat, "sms_context")))
				ast_copy_string(sms_context, v, sizeof(sms_context));
			if ((v = ast_variable_retrieve(cfg, cat, "call_context")))
				ast_copy_string(call_context, v, sizeof(call_context));
			sms_delete_after = ast_true(
				ast_variable_retrieve(cfg, cat, "sms_delete_after") ?: "1");
			if ((v = ast_variable_retrieve(cfg, cat, "band_file")))
				ast_copy_string(band_file, v, sizeof(band_file));
			continue;
		}
		if (!ast_begins_with(cat, "sim"))
			continue;
		int slot = atoi(cat + 3);
		if (slot < 1 || slot > E5_MAX_SIM) {
			ast_log(LOG_WARNING, "e5.conf: 忽略非法段 %s（需 sim1/sim2）\n", cat);
			continue;
		}
		struct e5_endpoint_cfg *e = &ep_cfg[slot - 1];
		e->configured = 1;
		e->sim_slot = slot;
		ast_copy_string(e->context,
			ast_variable_retrieve(cfg, cat, "context") ?: "default",
			sizeof(e->context));
		ast_copy_string(e->group_name,
			ast_variable_retrieve(cfg, cat, "group") ?: "",
			sizeof(e->group_name));
		/* 采样率按方向独立：capture_rate/playback_rate 优先，缺省回退到 rate（再缺省 8000） */
		int rate = atoi(ast_variable_retrieve(cfg, cat, "rate") ?: "0");
		const char *v;
		v = ast_variable_retrieve(cfg, cat, "capture_rate");
		e->cap_rate = v ? atoi(v) : rate;
		v = ast_variable_retrieve(cfg, cat, "playback_rate");
		e->pb_rate = v ? atoi(v) : rate;
		if (!e->cap_rate)
			e->cap_rate = 8000;
		if (!e->pb_rate)
			e->pb_rate = 8000;
		if (e->cap_rate != 16000)
			e->cap_rate = 8000;
		if (e->pb_rate != 16000)
			e->pb_rate = 8000;
		e->dump_capture = ast_true(ast_variable_retrieve(cfg, cat, "dump_capture") ?: "no");
		e->dump_playback = ast_true(ast_variable_retrieve(cfg, cat, "dump_playback") ?: "no");
		e->rxgain = atoi(ast_variable_retrieve(cfg, cat, "rxgain") ?: "100");
		e->txgain = atoi(ast_variable_retrieve(cfg, cat, "txgain") ?: "100");
		ast_copy_string(e->capture_dev,
			ast_variable_retrieve(cfg, cat, "capture_dev") ?: "",
			sizeof(e->capture_dev));
		ast_copy_string(e->playback_dev,
			ast_variable_retrieve(cfg, cat, "playback_dev") ?: "",
			sizeof(e->playback_dev));
		ast_copy_string(e->up_cmd,
			ast_variable_retrieve(cfg, cat, "route_up") ?: "",
			sizeof(e->up_cmd));
		ast_copy_string(e->down_cmd,
			ast_variable_retrieve(cfg, cat, "route_down") ?: "",
			sizeof(e->down_cmd));
	}
	ast_config_destroy(cfg);
}

/* ---------------- pvt 生命周期 ---------------- */

static struct ast_channel *e5_new(struct e5_pvt *p, int state, const char *cid_num,
	const char *ext, const char *context, const struct ast_channel *requestor)
{
	struct ast_channel *tmp;
	char name[AST_CHANNEL_NAME];
	struct ast_format_cap *caps;
	struct ast_format *chan_fmt;

	if (!(caps = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT)))
		return NULL;
	/* 通道对 Asterisk 呈现对称格式，速率由下行 cap_rate 决定；上行 pb_rate 的
	 * 差异在 e5_audio_write 内部重采样。避免读/写格式不对称导致 bridge 协商异常。 */
	chan_fmt = p->cfg->cap_rate == 16000 ? ast_format_slin16 : ast_format_slin;
	ast_format_cap_append(caps, chan_fmt, 20);
	ast_format_cap_set_framing(caps, 20);

	snprintf(name, sizeof(name), "E5/%d-%d", p->cfg->sim_slot, (int)ast_random() % 10000);

	/* Asterisk 23.1.0 签名：needqueue, state, cid_num, cid_name, acctcode,
	 * exten, context, assignedids, requestor, amaflag, name_fmt, ...。
	 * 该版本已无 linkedid 参数（linkedid 由 requestor 继承）。 */
	if (!(tmp = ast_channel_alloc(1, state, cid_num, cid_num, "", ext,
			context, NULL, requestor, AST_AMA_NONE, "%s", name))) {
		ast_debug(1, "e5_new: ast_channel_alloc 失败 ext=%s\n", ext ? ext : "(null)");
		ao2_ref(caps, -1);
		return NULL;
	}
	ast_debug(1, "e5_new: 创建通道 %s state=%d ext=%s context=%s\n", name, state, ext, context);

	p->chan = tmp;
	ast_channel_tech_set(tmp, &e5_tech);
	ast_channel_tech_pvt_set(tmp, p); /* tech 回调全部经 tech_pvt 找回 pvt */
	ast_channel_nativeformats_set(tmp, caps);

	/* 读/写统一为 chan_fmt（由 cap_rate 决定），与 nativeformats 一致 */
	ast_channel_set_writeformat(tmp, chan_fmt);
	ast_channel_set_rawwriteformat(tmp, chan_fmt);
	ast_channel_set_readformat(tmp, chan_fmt);
	ast_channel_set_rawreadformat(tmp, chan_fmt);

	ao2_ref(caps, -1);

	/* 20ms 定时器驱动 read 节奏（对齐 chan_quectel 的 ast_channel_set_fd 用法）。
	 * 语音数据本身走 e5_audio 的独立 capture 线程 + 环形缓冲，此处 fd 仅作定时。 */
	p->timer = ast_timer_open();
	if (p->timer) {
		ast_timer_set_rate(p->timer, 50); /* 50 ticks/s = 20ms */
		ast_channel_set_fd(tmp, 0, ast_timer_fd(p->timer));
	}

	ast_channel_context_set(tmp, context);
	ast_channel_exten_set(tmp, ext ? ext : "s");
	ast_channel_priority_set(tmp, 1);

	ast_channel_unlock(tmp); /* ast_channel_alloc 返回时持锁，交还调用框架 */
	return tmp;
}

static struct e5_pvt *pvt_alloc(struct e5_endpoint_cfg *cfg)
{
	struct e5_pvt *p = ast_calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->cfg = cfg;
	p->alive = 1;
	p->state = E5_CALL_UNKNOWN;
	ast_mutex_init(&p->lock);
	return p;
}

static void pvt_destroy(struct e5_pvt *p)
{
	if (!p)
		return;
	if (p->audio)
		e5_audio_close(p->audio);
	if (p->route)
		e5_route_close(p->route);
	if (p->timer)
		ast_timer_close(p->timer);
	ast_mutex_destroy(&p->lock);
	ast_free(p);
}

static int mm_reason_to_cause(int reason)
{
	switch (reason) {
	case E5_REASON_REFUSED_OR_BUSY:
		return AST_CAUSE_BUSY;
	case E5_REASON_AUDIO_SETUP_FAILED:
	case E5_REASON_ERROR:
		return AST_CAUSE_NORMAL_TEMPORARY_FAILURE;
	case E5_REASON_TERMINATED:
	default:
		return AST_CAUSE_NORMAL_CLEARING;
	}
}

/* ---------------- MM 事件回调（MM 线程！） ---------------- */

static void attach_active_media(struct e5_pvt *p)
{
	struct e5_endpoint_cfg *c = p->cfg;
	ast_debug(1, "attach_active_media: 开始 sim=%d cap_rate=%d pb_rate=%d cap=%s pb=%s\n",
		c->sim_slot, c->cap_rate, c->pb_rate, c->capture_dev, c->playback_dev);
	struct e5_route_cfg rc = {
		.up_cmd = c->up_cmd, .down_cmd = c->down_cmd, .timeout_ms = 5000 };
	p->route = e5_route_open(&rc);
	/* E5_RATE 传上行注入速率（与脚本 E5_INJECT 语义一致） */
	if (!p->route || e5_route_up(p->route, c->sim_slot, c->pb_rate))
		ast_log(LOG_WARNING, "E5/%d: tap 路由上电失败，本次通话无媒体\n", c->sim_slot);
	else
		ast_debug(1, "attach_active_media: route_up 成功\n");

	struct e5_audio_cfg ac = {
		.capture_dev = c->capture_dev, .playback_dev = c->playback_dev,
		.cap_rate = c->cap_rate, .pb_rate = c->pb_rate,
		.dump_capture = c->dump_capture, .dump_playback = c->dump_playback,
		.rxgain = c->rxgain, .txgain = c->txgain,
		.period_ms = 20, .buffer_ms = 160,
		.band_file = band_file[0] ? band_file : NULL };
	p->audio = e5_audio_open(&ac); /* 未配置时返回 NULL：信令态 */
	ast_debug(1, "attach_active_media: audio_open 返回 %p（%s）\n", p->audio,
		p->audio ? "媒体可用" : "信令态");
}

static struct e5_pvt *find_pvt_for_call(const struct e5_mm_call *mc)
{
	int slot = mc->sim_slot >= 1 ? mc->sim_slot - 1 : 0;
	if (!mc->path)
		return NULL;
	for (int i = 0; i < E5_MAX_SIM; i++) {
		struct e5_pvt *p = pvt_slot[i];
		if (p && !strcmp(p->call_path, mc->path))
			return p;
	}
	/* 外呼刚 CreateCall、事件早于本地关联时按卡槽 + 号码补认。
	 * 仅限同向外呼：避免把号码相同的来电误绑到等待对账的外呼 pvt。 */
	struct e5_pvt *p = pvt_slot[slot];
	if (p && p->awaiting_mm && mc->direction == E5_DIR_OUTGOING &&
	    !strcmp(p->number, mc->number)) {
		ast_copy_string(p->call_path, mc->path, sizeof(p->call_path));
		p->direction = E5_DIR_OUTGOING;
		return p;
	}
	return NULL;
}

static struct e5_pvt *find_pvt_for_path(const char *path)
{
	if (!path)
		return NULL;
	for (int i = 0; i < E5_MAX_SIM; i++) {
		struct e5_pvt *p = pvt_slot[i];
		if (p && !strcmp(p->call_path, path))
			return p;
	}
	return NULL;
}

/* CallAdded（异步拉取详情完成后触发）：新来电创建 pvt + 通道 + 进拨号计划；
 * 外呼则补认 call_path。 */
static void on_call_added(struct e5_mm *mm, const struct e5_mm_call *mc, void *user)
{
	ast_debug(1, "on_call_added: number=%s state=%s dir=%d sim=%d path=%s\n",
		mc->number, e5_mm_state_name(mc->state), mc->direction, mc->sim_slot, mc->path);
	ast_mutex_lock(&users_lock);
	struct e5_pvt *p = find_pvt_for_call(mc);

	if (!p && mc->direction == E5_DIR_INCOMING &&
	    mc->state == E5_CALL_RINGING_IN) {
		/* 新来电：选卡槽（MM SimSlot 1-based，0 视为卡1） */
		int slot = mc->sim_slot >= 1 ? mc->sim_slot - 1 : 0;
		if (slot >= E5_MAX_SIM || !ep_cfg[slot].configured) {
			ast_debug(1, "on_call_added: 卡槽 %d 未配置/越界，不接管\n", slot);
			goto out;
		}
		if (pvt_slot[slot]) {
			ast_log(LOG_NOTICE, "E5/%d: 线路忙，新来电 %s 不接管\n",
				slot + 1, mc->number);
			ast_debug(1, "on_call_added: 卡槽 %d 忙，不接管来电\n", slot);
			goto out;
		}
		p = pvt_alloc(&ep_cfg[slot]);
		if (!p) {
			ast_debug(1, "on_call_added: pvt_alloc 失败\n");
			goto out;
		}
		ast_copy_string(p->call_path, mc->path, sizeof(p->call_path));
		ast_copy_string(p->number, mc->number, sizeof(p->number));
		p->direction = E5_DIR_INCOMING;
		p->state = mc->state;
		pvt_slot[slot] = p;
		ast_debug(1, "on_call_added: 新来电接管 slot=%d，创建通道进 context=%s\n",
			slot + 1, p->cfg->context);

		struct ast_channel *chan = e5_new(p, AST_STATE_RING, mc->number,
			mc->number, p->cfg->context, NULL);
		if (!chan) {
			ast_debug(1, "on_call_added: e5_new 失败，回收 pvt\n");
			pvt_slot[slot] = NULL;
			pvt_destroy(p);
			goto out;
		}
		/* pbx_start 放锁外：避免在 MM 线程长时持锁阻塞其他信号 */
		ast_mutex_unlock(&users_lock);
		if (ast_pbx_start(chan)) {
			ast_log(LOG_WARNING, "E5: 来电 PBX 启动失败 %s\n", mc->number);
			ast_debug(1, "on_call_added: ast_pbx_start 失败\n");
			ast_hangup(chan);
		} else {
			ast_debug(1, "on_call_added: ast_pbx_start 已启动\n");
		}
		return;
	}
	if (!p) {
		ast_debug(1, "on_call_added: 未找到 pvt（非本模块通话），忽略\n");
		goto out; /* 屏幕电话/其他客户端的通话，不接管 */
	}

	/* 外呼补认 / 已存在 pvt */
	if (ast_strlen_zero(p->call_path)) {
		ast_copy_string(p->call_path, mc->path, sizeof(p->call_path));
		ast_debug(1, "on_call_added: 外呼补认 call_path=%s\n", p->call_path);
	}
	p->state = mc->state;
out:
	ast_mutex_unlock(&users_lock);
}

/* PropertiesChanged 透传（无同步 D-Bus）：按 path + state 更新并投递控制帧 */
static void on_call_state(struct e5_mm *mm, const char *path, int state, int reason, void *user)
{
	ast_mutex_lock(&users_lock);
	struct e5_pvt *p = find_pvt_for_path(path);
	if (!p) {
		ast_mutex_unlock(&users_lock);
		return;
	}
	p->state = state;
	if (p->chan) {
		switch (state) {
		case E5_CALL_RINGING_OUT:
		case E5_CALL_DIALING:
			ast_queue_control(p->chan, AST_CONTROL_RINGING);
			break;
		case E5_CALL_ACTIVE:
			/* 只投递事件；媒体上电延迟到 PBX 线程首次 read/write */
			ast_queue_control(p->chan, AST_CONTROL_ANSWER);
			break;
		case E5_CALL_TERMINATED:
			ast_queue_hangup_with_cause(p->chan, mm_reason_to_cause(reason));
			break;
		default:
			break;
		}
	}
	ast_mutex_unlock(&users_lock);
}

static void on_call_removed(struct e5_mm *mm, const char *path, void *user)
{
	if (!path)
		return;
	ast_mutex_lock(&users_lock);
	for (int i = 0; i < E5_MAX_SIM; i++) {
		struct e5_pvt *p = pvt_slot[i];
		if (p && !strcmp(p->call_path, path) && p->chan)
			ast_queue_hangup(p->chan);
	}
	ast_mutex_unlock(&users_lock);
}

/* 收短信：注入拨号计划（Local 通道，context=sms_context，exten=s）。
 * 变量：SMS_FROM / SMS_TEXT / SMS_TS / SMS_PATH；另发 AMI 事件。 */
static void on_sms_received(struct e5_mm *mm, const struct e5_mm_sms *sms, void *user)
{
	struct ast_format_cap *caps;
	struct ast_channel *chan;
	char dest[AST_MAX_CONTEXT + 4];
	int cause = 0;

	ast_log(LOG_NOTICE, "E5 SMS: %s -> %s: %s\n",
		sms->number, sms->timestamp, sms->text);

	ast_manager_event_multichan(EVENT_FLAG_MESSAGE, "E5SMSReceived", 0, NULL,
		"From: %s\r\nText: %s\r\nTimestamp: %s\r\nSmsPath: %s\r\n",
		sms->number, sms->text, sms->timestamp, sms->path);

	if (!(caps = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT)))
		goto out;
	ast_format_cap_append(caps, ast_format_slin, 0);

	snprintf(dest, sizeof(dest), "s@%s", sms_context);
	chan = ast_request("Local", caps, NULL, NULL, dest, &cause);
	ao2_ref(caps, -1);
	if (!chan) {
		ast_log(LOG_WARNING,
			"E5 SMS: 无法创建 Local 通道到 %s（context 缺失？短信不投递）\n", dest);
		goto out;
	}
	pbx_builtin_setvar_helper(chan, "SMS_FROM", sms->number);
	pbx_builtin_setvar_helper(chan, "SMS_TEXT", sms->text);
	pbx_builtin_setvar_helper(chan, "SMS_TS", sms->timestamp);
	pbx_builtin_setvar_helper(chan, "SMS_PATH", sms->path);
	if (ast_pbx_start(chan)) {
		ast_log(LOG_WARNING, "E5 SMS: PBX 启动失败（%s）\n", dest);
		ast_hangup(chan);
		goto out;
	}

out:
	if (sms_delete_after)
		e5_mm_sms_delete(mm, sms->path); /* 防 SIM 卡存储满静默丢新短信 */
}

static void reconcile_one_call(const struct e5_mm_call *mc, void *user)
{
	on_call_added(g_mm, mc, user);
}

static const struct e5_mm_events mm_events = {
	.call_added = on_call_added,
	.call_removed = on_call_removed,
	.call_state_changed = on_call_state,
	.sms_received = on_sms_received,
};

/* ---------------- ast_channel_tech 回调 ---------------- */

static struct ast_channel *e5_request(const char *type, struct ast_format_cap *cap,
	const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor,
	const char *addr, int *cause)
{
	/* addr: "1/10086" 或 "10086" */
	int slot = 0;
	const char *number = addr;
	char numbuf[64];

	if (!g_mm) {
		ast_log(LOG_WARNING, "E5: ModemManager 未连接，无法外呼\n");
		*cause = AST_CAUSE_CONGESTION;
		return NULL;
	}
	if (addr[0] == '1' && addr[1] == '/') { slot = 0; number = addr + 2; }
	else if (addr[0] == '2' && addr[1] == '/') { slot = 1; number = addr + 2; }
	else { slot = 0; } /* 默认卡1 */

	if (!ep_cfg[slot].configured) {
		ast_log(LOG_NOTICE, "E5: SIM%d 未配置\n", slot + 1);
		return NULL;
	}
	if (regexec(&number_re, number, 0, NULL, 0)) {
		ast_log(LOG_WARNING, "E5: 号码非法: %s\n", number);
		return NULL;
	}
	ast_copy_string(numbuf, number, sizeof(numbuf));

	ast_mutex_lock(&users_lock);
	if (pvt_slot[slot]) {
		ast_log(LOG_WARNING, "E5/%d: 线路忙\n", slot + 1);
		ast_mutex_unlock(&users_lock);
		return NULL;
	}
	struct e5_pvt *p = pvt_alloc(&ep_cfg[slot]);
	if (!p) {
		ast_mutex_unlock(&users_lock);
		return NULL;
	}
	ast_copy_string(p->number, numbuf, sizeof(p->number));
	p->direction = E5_DIR_OUTGOING;
	p->awaiting_mm = 1;
	pvt_slot[slot] = p;

	struct ast_channel *chan = e5_new(p, AST_STATE_DOWN, NULL, numbuf,
		p->cfg->context, requestor);
	ast_mutex_unlock(&users_lock);
	if (!chan) {
		ast_mutex_lock(&users_lock);
		pvt_slot[slot] = NULL;
		pvt_destroy(p);
		ast_mutex_unlock(&users_lock);
		return NULL;
	}
	*cause = 0;
	return chan;
}

static int e5_call(struct ast_channel *ast, const char *dest, int timeout)
{
	struct e5_pvt *p = ast_channel_tech_pvt(ast);
	char *call_path = NULL;
	int rc, have_path;

	if (!p)
		return -1;

	rc = e5_mm_dial(g_mm, p->cfg->sim_slot, p->number, &call_path);
	ast_mutex_lock(&users_lock);
	if (!ast_strlen_zero(call_path))
		ast_copy_string(p->call_path, call_path, sizeof(p->call_path));
	p->awaiting_mm = 0;
	have_path = !ast_strlen_zero(p->call_path);
	ast_mutex_unlock(&users_lock);
	ast_free(call_path);

	if (rc && !have_path) {
		ast_log(LOG_WARNING, "E5/%d: 外呼发起失败 %s\n", p->cfg->sim_slot, p->number);
		ast_channel_hangupcause_set(ast, AST_CAUSE_NORMAL_TEMPORARY_FAILURE);
		return -1;
	}
	return 0;
}

static int e5_answer(struct ast_channel *ast)
{
	struct e5_pvt *p = ast_channel_tech_pvt(ast);
	if (!p)
		return -1;
	if (p->direction == E5_DIR_INCOMING) {
		if (e5_mm_answer(g_mm, p->call_path))
			return -1;
	}
	return 0;
}

static int e5_hangup(struct ast_channel *ast)
{
	struct e5_pvt *p = ast_channel_tech_pvt(ast);
	char call_path[sizeof(p->call_path)];
	int need_hangup;

	if (!p)
		return 0;

	ast_mutex_lock(&users_lock);
	need_hangup = !ast_strlen_zero(p->call_path) &&
		p->state != E5_CALL_TERMINATED;
	ast_copy_string(call_path, p->call_path, sizeof(call_path));
	for (int i = 0; i < E5_MAX_SIM; i++)
		if (pvt_slot[i] == p)
			pvt_slot[i] = NULL;
	p->chan = NULL; /* 锁内清空：MM 事件线程持锁时才会读 p->chan */
	ast_mutex_unlock(&users_lock);

	if (need_hangup)
		e5_mm_hangup(g_mm, call_path);

	ast_channel_tech_pvt_set(ast, NULL);
	pvt_destroy(p);
	return 0;
}

/* 惰性挂载 tap 音频：只尝试一次，并用 users_lock 防 read/write 并发进入 */
static void try_attach_media(struct e5_pvt *p, struct ast_channel *ast)
{
	int do_attach = 0;

	ast_mutex_lock(&users_lock);
	if (ast_channel_state(ast) == AST_STATE_UP && p->state == E5_CALL_ACTIVE &&
	    !p->media_started) {
		p->media_started = 1;
		do_attach = 1;
	}
	ast_mutex_unlock(&users_lock);

	if (do_attach)
		attach_active_media(p);
}

static struct ast_frame *e5_read(struct ast_channel *ast)
{
	struct e5_pvt *p = ast_channel_tech_pvt(ast);
	if (!p)
		return &ast_null_frame;
	if (p->timer)
		ast_timer_ack(p->timer, 1);
	try_attach_media(p, ast);
	if (p->audio && ast_channel_state(ast) == AST_STATE_UP) {
		struct ast_frame *f = e5_audio_read(p->audio, 20);
		if (f)
			return f;
	}
	return &ast_null_frame;
}

static int e5_write(struct ast_channel *ast, struct ast_frame *f)
{
	struct e5_pvt *p = ast_channel_tech_pvt(ast);
	if (!p)
		return -1;
	try_attach_media(p, ast);
	if (f->frametype == AST_FRAME_VOICE && p->audio &&
	    ast_channel_state(ast) == AST_STATE_UP)
		return e5_audio_write(p->audio, f);
	return 0;
}

static int e5_digit_begin(struct ast_channel *ast, char digit)
{
	struct e5_pvt *p = ast_channel_tech_pvt(ast);
	if (!p)
		return 0;
	if (!ast_strlen_zero(p->call_path) &&
	    ast_channel_state(ast) == AST_STATE_UP)
		e5_mm_send_dtmf(g_mm, p->call_path, digit);
	return 0;
}

static int e5_indicate(struct ast_channel *ast, int condition,
	const void *data, size_t datalen)
{
	switch (condition) {
	case AST_CONTROL_RINGING:
		return -1; /* 让 core 播放回铃音；MM 事件驱动 */
	case AST_CONTROL_BUSY:
	case AST_CONTROL_CONGESTION:
		return -1;
	default:
		return 0;
	}
}

static struct ast_channel_tech e5_tech = {
	.type = E5_TECH,
	.description = "E5 (Unisoc SharkL5) Cellular Channel via ModemManager",
	.requester = e5_request,
	.call = e5_call,
	.answer = e5_answer,
	.hangup = e5_hangup,
	.read = e5_read,
	.write = e5_write,
	.send_digit_begin = e5_digit_begin,
	.indicate = e5_indicate,
	/* 本地声卡通道，非网络通道：不要 WANTSJITTER/CREATEJITTER；对齐 chan_quectel 不设 */
	.properties = 0,
};

/* ---------------- CLI（控制台命令） ---------------- */

/* 解析 "sim1"/"sim2"/"1"/"2" -> 1-based 卡槽；无效返回 0 */
static int parse_sim_slot(const char *s)
{
	if (!strcasecmp(s, "sim1") || !strcmp(s, "1")) return 1;
	if (!strcasecmp(s, "sim2") || !strcmp(s, "2")) return 2;
	return 0;
}

static char *e5_cli_status(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char modem[128];

	switch (cmd) {
	case CLI_INIT:
		e->command = "e5 show status";
		e->usage = "Usage: e5 show status\n"
			"  ModemManager 在线状态、modem 对象、各卡通道与 tap 状态\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (!g_mm || !e5_mm_query(g_mm, modem, sizeof(modem))) {
		ast_cli(a->fd, "ModemManager: 不在线（等待服务出现）\n");
		return CLI_SUCCESS;
	}
	ast_cli(a->fd, "ModemManager: 在线  modem=%s\n", modem);
	ast_cli(a->fd, "SMS: context=%s  delete_after=%s  band_file=%s\n",
		sms_context, sms_delete_after ? "yes" : "no",
		band_file[0] ? band_file : "(disabled)");
	ast_cli(a->fd, "Call: context=%s\n", call_context);

	ast_mutex_lock(&users_lock);
	for (int i = 0; i < E5_MAX_SIM; i++) {
		struct e5_endpoint_cfg *c = &ep_cfg[i];
		struct e5_pvt *p = pvt_slot[i];
		if (!c->configured) {
			ast_cli(a->fd, "SIM%d: 未配置\n", i + 1);
			continue;
		}
		ast_cli(a->fd, "SIM%d: context=%s group=%s cap_rate=%d pb_rate=%d tap=%s\n",
			i + 1, c->context, c->group_name, c->cap_rate, c->pb_rate,
			ast_strlen_zero(c->capture_dev) ? "未配置(信令态)" : c->capture_dev);
		if (p) {
			const char *audio_st = !e5_audio_is_open(p->audio) ? "down"
				: e5_audio_capture_ok(p->audio) ? "up"
				: "up(下行静音,仅上行注入)";
			ast_cli(a->fd,
				"  通话: %s %s  state=%s  audio=%s  route=%s\n",
				p->direction == E5_DIR_INCOMING ? "来电" : "外呼",
				p->number, e5_mm_state_name(p->state),
				audio_st, p->route ? "up" : "down");
		} else {
			ast_cli(a->fd, "  空闲\n");
		}
	}
	ast_mutex_unlock(&users_lock);
	return CLI_SUCCESS;
}

static void cli_list_one(const struct e5_mm_call *mc, void *user)
{
	struct ast_cli_args *a = user;
	ast_cli(a->fd, "  %s  %s  %s  slot=%d  %s\n",
		mc->path,
		mc->direction == E5_DIR_INCOMING ? "in " :
			mc->direction == E5_DIR_OUTGOING ? "out" : "?  ",
		mc->number, mc->sim_slot, e5_mm_state_name(mc->state));
}

static char *e5_cli_calls(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "e5 show calls";
		e->usage = "Usage: e5 show calls\n"
			"  列出 ModemManager 当前全部 Call 对象（含非本模块接管的）\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (!g_mm || !e5_mm_online(g_mm)) {
		ast_cli(a->fd, "ModemManager 不在线\n");
		return CLI_SUCCESS;
	}
	if (e5_mm_list_calls(g_mm, cli_list_one, a))
		ast_cli(a->fd, "（查询失败）\n");
	return CLI_SUCCESS;
}

/* e5 call <sim1|sim2|1|2> <number>
 * 从指定 SIM 主动外呼，随后进入 call_context 拨号计划，由 extensions.conf
 * 决定呼叫规则（如 Dial(PJSIP/70) 桥到分机）。 */
static char *e5_cli_call(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "e5 call";
		e->usage = "Usage: e5 call <sim1|sim2|1|2> <number>\n"
			"  从指定 SIM 卡发起蜂窝外呼，通话进入 call_context 拨号计划\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;

	int slot = parse_sim_slot(a->argv[2]);
	if (slot < 1 || slot > E5_MAX_SIM) {
		ast_cli(a->fd, "SIM 槽位无效: %s（需 sim1/sim2/1/2）\n", a->argv[2]);
		return CLI_FAILURE;
	}
	const char *number = a->argv[3];
	if (regexec(&number_re, number, 0, NULL, 0)) {
		ast_cli(a->fd, "号码非法: %s\n", number);
		return CLI_FAILURE;
	}
	if (!g_mm || !e5_mm_online(g_mm)) {
		ast_cli(a->fd, "ModemManager 不在线，无法外呼\n");
		return CLI_FAILURE;
	}
	int idx = slot - 1;
	if (!ep_cfg[idx].configured) {
		ast_cli(a->fd, "SIM%d 未配置\n", slot);
		return CLI_FAILURE;
	}

	ast_mutex_lock(&users_lock);
	if (pvt_slot[idx]) {
		ast_mutex_unlock(&users_lock);
		ast_cli(a->fd, "SIM%d 线路忙\n", slot);
		return CLI_FAILURE;
	}
	struct e5_pvt *p = pvt_alloc(&ep_cfg[idx]);
	if (!p) {
		ast_mutex_unlock(&users_lock);
		return CLI_FAILURE;
	}
	ast_copy_string(p->number, number, sizeof(p->number));
	p->direction = E5_DIR_OUTGOING;
	p->awaiting_mm = 1;
	pvt_slot[idx] = p;

	struct ast_channel *chan = e5_new(p, AST_STATE_DOWN, NULL, number,
		call_context, NULL);
	ast_mutex_unlock(&users_lock);

	if (!chan) {
		ast_mutex_lock(&users_lock);
		pvt_slot[idx] = NULL;
		pvt_destroy(p);
		ast_mutex_unlock(&users_lock);
		ast_cli(a->fd, "创建通道失败\n");
		return CLI_FAILURE;
	}

	/* 直接发起蜂窝外呼（不经过 Dial 的 .call 回调） */
	char *cp = NULL;
	int rc = e5_mm_dial(g_mm, slot, number, &cp);

	ast_mutex_lock(&users_lock);
	if (!ast_strlen_zero(cp))
		ast_copy_string(p->call_path, cp, sizeof(p->call_path));
	p->awaiting_mm = 0;
	int have_path = !ast_strlen_zero(p->call_path);
	ast_mutex_unlock(&users_lock);
	ast_free(cp);

	if (rc && !have_path) {
		ast_log(LOG_WARNING, "E5/%d: 外呼发起失败 %s\n", slot, number);
		ast_cli(a->fd, "外呼发起失败\n");
		ast_hangup(chan);
		return CLI_FAILURE;
	}

	if (ast_pbx_start(chan)) {
		ast_log(LOG_WARNING, "E5/%d: PBX 启动失败 %s\n", slot, number);
		ast_hangup(chan);
		return CLI_FAILURE;
	}
	ast_cli(a->fd, "已从 SIM%d 呼叫 %s（进入 context=%s）\n",
		slot, number, call_context);
	return CLI_SUCCESS;
}

/* e5 sms send <number> <text...>
 * 注意：命令含 3 个词（e5 sms send），参数从 argv[3] 起。 */
static char *e5_cli_sms_send(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "e5 sms send";
		e->usage = "Usage: e5 sms send <number> <text...>\n"
			"  经 ModemManager 发短信（走默认 SIM；stock MM Messaging 无选卡属性）\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc < 4) {
		ast_cli(a->fd, "用法: e5 sms send <number> <text...>\n");
		return CLI_SHOWUSAGE;
	}
	const char *number = a->argv[3];
	if (regexec(&number_re, number, 0, NULL, 0)) {
		ast_cli(a->fd, "号码非法: %s\n", number);
		return CLI_FAILURE;
	}
	char text[1024];
	text[0] = '\0';
	for (int i = 4; i < a->argc; i++) {
		if (i > 4)
			strncat(text, " ", sizeof(text) - strlen(text) - 1);
		strncat(text, a->argv[i], sizeof(text) - strlen(text) - 1);
	}
	if (!g_mm || e5_mm_sms_send(g_mm, number, text)) {
		ast_cli(a->fd, "发送失败（modem 不在线或 MM 拒绝；不自动重试）\n");
		return CLI_FAILURE;
	}
	ast_cli(a->fd, "已提交发送: %s\n", number);
	return CLI_SUCCESS;
}

static struct ast_cli_entry e5_cli[] = {
	AST_CLI_DEFINE(e5_cli_status, "Show chan_e5 status"),
	AST_CLI_DEFINE(e5_cli_calls, "List ModemManager calls"),
	AST_CLI_DEFINE(e5_cli_call, "Place a cellular call from a SIM slot"),
	AST_CLI_DEFINE(e5_cli_sms_send, "Send SMS via ModemManager"),
};

/* ---------------- E5SendSMS 拨号计划应用 ---------------- */

#define E5_SMS_APP "E5SendSMS"
#define E5_SMS_APP_SYNOPSIS "Send an SMS via the E5 modem (E5SendSMS(number,text))"
#define E5_SMS_APP_DESC \
	"E5SendSMS(number,text)\n" \
	"  通过 ModemManager 发送短信。text 含逗号请用引号包裹。\n" \
	"  走默认 SIM（stock MM Messaging 无选卡属性）。发送结果以 MM 为准，不重试。\n"

static int e5_sms_app_exec(struct ast_channel *chan, const char *data)
{
	char *parse;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(number);
		AST_APP_ARG(text);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "E5SendSMS: 需要 number,text\n");
		return -1;
	}
	parse = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, parse);
	if (ast_strlen_zero(args.number) || ast_strlen_zero(args.text)) {
		ast_log(LOG_WARNING, "E5SendSMS: 参数不完整\n");
		return -1;
	}
	if (regexec(&number_re, args.number, 0, NULL, 0)) {
		ast_log(LOG_WARNING, "E5SendSMS: 号码非法 %s\n", args.number);
		return -1;
	}
	if (!g_mm || !e5_mm_online(g_mm))
		return -1;
	return e5_mm_sms_send(g_mm, args.number, args.text) ? -1 : 0;
}

/* ---------------- 模块生命周期 ---------------- */

static int load_module(void)
{
	if (regcomp(&number_re, NUMBER_RE, REG_EXTENDED | REG_NOSUB)) {
		ast_log(LOG_ERROR, "E5: 号码正则编译失败\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	load_config(0);

	if (!(e5_tech.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT))) {
		ast_log(LOG_ERROR, "E5: 分配 capabilities 失败\n");
		regfree(&number_re);
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_format_cap_append(e5_tech.capabilities, ast_format_slin, 20);
	ast_format_cap_append(e5_tech.capabilities, ast_format_slin16, 20);
	ast_format_cap_set_framing(e5_tech.capabilities, 20);

	g_mm = e5_mm_init(&mm_events);
	if (!g_mm)
		ast_log(LOG_WARNING, "E5: ModemManager 暂不可用，模块仍加载（等待服务出现）\n");

	/* 启动时对账：MM 里可能已有进行中的 Call */
	if (g_mm)
		e5_mm_list_calls(g_mm, reconcile_one_call, NULL);

	if (ast_cli_register_multiple(e5_cli, ARRAY_LEN(e5_cli)))
		ast_log(LOG_WARNING, "E5: CLI 注册失败\n");
	if (ast_register_application(E5_SMS_APP, e5_sms_app_exec,
			E5_SMS_APP_SYNOPSIS, E5_SMS_APP_DESC))
		ast_log(LOG_WARNING, "E5: %s 应用注册失败\n", E5_SMS_APP);

	if (ast_channel_register(&e5_tech)) {
		ast_log(LOG_ERROR, "E5: 无法注册通道类 %s\n", E5_TECH);
		return AST_MODULE_LOAD_FAILURE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_channel_unregister(&e5_tech);
	if (e5_tech.capabilities) {
		ao2_ref(e5_tech.capabilities, -1);
		e5_tech.capabilities = NULL;
	}
	ast_unregister_application(E5_SMS_APP);
	ast_cli_unregister_multiple(e5_cli, ARRAY_LEN(e5_cli));
	if (g_mm) {
		e5_mm_shutdown(g_mm);
		g_mm = NULL;
	}
	regfree(&number_re);
	return 0;
}

static int reload_module(void)
{
	load_config(1);
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"E5 Cellular Channel (ModemManager + AGDSP tap)",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
	.load_pri = AST_MODPRI_CHANNEL_DRIVER,
);
