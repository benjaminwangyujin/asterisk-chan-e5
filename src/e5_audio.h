/*
 * e5_audio.h - CP 语音 AP 分流（tap）音频桥
 *
 * E5 的 FE_ST_VOICE(hw:0,5) 是 hostless PCM，AP 不能直接读写采样。
 * 本层操作的是实机验证过的 tap 端点：
 *   capture_dev  -> 远端语音（对端说话），供 ast_read：hw:0,9 (FE_ST_VOICE_C)
 *   playback_dev -> 本地语音（Asterisk 侧来的帧），送往 CP：hw:0,18 (FE_ST_VOICE_PCM_P)
 *
 * 降级策略：
 *   - 下行 tap 打不开/通话中丢失 -> “静音模式”：按帧节奏喂零帧保持 RTP 存活，
 *     上行注入（playback）继续工作；绝不回退打开 host 麦克风（通话中开麦会被
 *     路由层从基带抢走，基带 ~20s 拆线）。
 *   - 上行 tap 打不开 -> 下行继续（SIP 能听到对端，对端听不到 SIP）。
 *   - 仅当双向都不可用才整体失败（信令态）。
 */
#ifndef E5_AUDIO_H
#define E5_AUDIO_H

#include <asterisk/frame.h>
#include <pthread.h>

struct e5_audio;

struct e5_audio_cfg {
	const char *capture_dev;   /* ALSA 名，如 hw:0,9 */
	const char *playback_dev;  /* ALSA 名，如 hw:0,18 */
	int cap_rate;              /* 下行采集采样率（8000/16000），默认 8000 */
	int pb_rate;               /* 上行注入采样率（8000/16000），默认 8000 */
	int period_ms;             /* 默认 20 */
	int buffer_ms;             /* 默认 160 */
	int txgain;                /* 0..100，软件增益 */
	int rxgain;
	int dump_capture;          /* 1=把下行采集 dump 到 /tmp/e5_cap_2down_dump.raw（默认 0） */
	int dump_playback;         /* 1=把上行注入 dump 到 /tmp/e5_cap_2up_dump.raw（默认 0） */
	const char *band_file;     /* 可选：DSP 带宽通知文件（内容 0/1）。0->8000 1->16000 */
};

/* 打开并预启动双向 PCM；不提供有效设备时返回 NULL（信令态） */
struct e5_audio *e5_audio_open(const struct e5_audio_cfg *cfg);
void e5_audio_close(struct e5_audio *a);

/* 运行中切换采样率（DSP 上报带宽变化时；内部安全重开） */
int e5_audio_set_rate(struct e5_audio *a, int rate);

/* 取出一帧远端音频（按 cap_rate：8k->AST_FORMAT_SLINEAR / 16k->AST_FORMAT_SLINEAR16）。
 * 阻塞至多 timeout_ms；返回的帧归调用方 ast_frfree。无数据返回 NULL。 */
struct ast_frame *e5_audio_read(struct e5_audio *a, int timeout_ms);

/* 写入一帧发往对端的音频；仅接受与 pb_rate 匹配的 slinear(16)。返回 0 成功。 */
int e5_audio_write(struct e5_audio *a, struct ast_frame *f);

int e5_audio_is_open(struct e5_audio *a);

/* 下行 capture 是否可用（0 = 静音模式：仅上行注入在工作） */
int e5_audio_capture_ok(struct e5_audio *a);

#endif
