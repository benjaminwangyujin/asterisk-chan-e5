/*
 * e5_route.c - tap 所有权切换（调用外部脚本，复用厂商 DSP 路由资产）
 */
#include "e5_route.h"

#include <asterisk.h>
#include <asterisk/utils.h>
#include <asterisk/strings.h>
#include <asterisk/app.h>

#include <stdlib.h>
#include <string.h>

struct e5_route {
	struct e5_route_cfg cfg;
	int up;
};

struct e5_route *e5_route_open(const struct e5_route_cfg *cfg)
{
	struct e5_route *r = ast_calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->cfg = *cfg;
	if (ast_strlen_zero(r->cfg.up_cmd))
		r->cfg.up_cmd = "/usr/libexec/asterisk/e5-tap-up";
	if (ast_strlen_zero(r->cfg.down_cmd))
		r->cfg.down_cmd = "/usr/libexec/asterisk/e5-tap-down";
	if (!r->cfg.timeout_ms)
		r->cfg.timeout_ms = 5000;
	return r;
}

/* ast_safe_system 走 /bin/sh -c；脚本自身负责控件快照-恢复与幂等，
 * C 侧不写死任何 mixer 常量。
 *
 * 脚本契约：
 *   E5_SIM_SLOT  1/2；down 为 0
 *   E5_RATE      8000/16000；down 为 0
 *   E5_INJECT    up=1：启用“注入模式”——tap playback 音频进 CP 上行时
 *                DSP 必须排除 host 麦克风（SIP 音频是唯一上行源，避免本地麦
 *                混音形成回环）；down=0：恢复常规通话路由。 */
static int run_cmd_env(const char *cmd, int sim_slot, int rate, int timeout_ms)
{
	char buf[512];
	snprintf(buf, sizeof(buf),
		"E5_SIM_SLOT=%d E5_RATE=%d E5_INJECT=%d %s",
		sim_slot, rate, sim_slot ? 1 : 0, cmd);
	ast_debug(1, "e5_route: %s\n", buf);
	int rc = ast_safe_system((char *)buf);
	if (rc == -1) {
		ast_log(LOG_WARNING, "e5_route: 无法执行 %s（脚本未安装？）\n", cmd);
		return -1;
	}
	if (rc != 0) {
		ast_log(LOG_WARNING, "e5_route: %s 退出码 %d\n", cmd, rc);
		return -1;
	}
	return 0;
}

int e5_route_up(struct e5_route *r, int sim_slot, int rate)
{
	if (!r || r->up)
		return 0;
	if (run_cmd_env(r->cfg.up_cmd, sim_slot, rate, r->cfg.timeout_ms))
		return -1;
	r->up = 1;
	return 0;
}

int e5_route_down(struct e5_route *r)
{
	if (!r || !r->up)
		return 0;
	(void)run_cmd_env(r->cfg.down_cmd, 0, 0, r->cfg.timeout_ms);
	r->up = 0;
	return 0;
}

void e5_route_close(struct e5_route *r)
{
	if (!r)
		return;
	e5_route_down(r);
	ast_free(r);
}
