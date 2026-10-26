/*
 * e5_route.h - 通话音频路由所有权与 tap 上下电
 *
 * 屏幕电话的 /usr/libexec/e5-voice-audio（内含厂商 VoiceHardware）是
 * receiver/speaker/BT 路由的“原厂实现”。chan_e5 不复制那 40+ 个控件表，
 * 而是通过外部脚本切换所有权：
 *   route_up   : 停 e5-voice-audio，挂起媒体，应用 tap 控件序列
 *   route_down : 恢复控件，重启 e5-voice-audio
 * 脚本路径来自 e5.conf，默认 /usr/libexec/asterisk/e5-tap-{up,down}。
 */
#ifndef E5_ROUTE_H
#define E5_ROUTE_H

struct e5_route;

struct e5_route_cfg {
	const char *up_cmd;
	const char *down_cmd;
	int timeout_ms;          /* 默认 5000 */
};

struct e5_route *e5_route_open(const struct e5_route_cfg *cfg);

/* 通话接通（active）前后调用；返回 0 表示已取得 tap 所有权 */
int e5_route_up(struct e5_route *r, int sim_slot, int rate);
int e5_route_down(struct e5_route *r);

void e5_route_close(struct e5_route *r);

#endif
