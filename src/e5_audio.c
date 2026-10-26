/*
 * e5_audio.c - ALSA tap 音频桥实现
 *
 * 结构与 chan_quectel/pcm.c 一致（S16_LE mono、near-period、poll fd、
 * xrun recover、jitter ringbuffer），差别仅在端点来源（tap）。
 *
 * 端点（实机双向验证）：
 *   下行 capture  = hw:0,9  (FE_ST_VOICE_C)   + S_VOICE_CAP_C SWITCH on
 *   上行 playback = hw:0,18 (FE_ST_VOICE_PCM_P)+ S_VOICE_PCM_P SWITCH on
 * 上行 playback 直接在 e5_audio_write 中写（Asterisk 以 20ms 节奏喂帧）；
 * 下行 capture 由后台 poll 线程读入环形缓冲，ast_read 侧按帧取出。
 */
#include <asterisk.h>
#include <asterisk/format.h>
#include <asterisk/format_cache.h>

#include "e5_audio.h"

#include <asterisk/utils.h>
#include <asterisk/lock.h>
#include <asterisk/frame.h>
#include <asterisk/strings.h>

#include <alsa/asoundlib.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#include <math.h>

#define RING_SECONDS 2
#define DRAIN_MS      200

struct pcm_part {
	snd_pcm_t *h;
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	struct pollfd *pfds;
	nfds_t npfds;
};

struct e5_audio {
	struct e5_audio_cfg cfg;
	struct pcm_part cap;          /* 远端 -> Asterisk */
	struct pcm_part pb;           /* Asterisk -> 对端 */
	pthread_t cap_thread;
	int stopfds[2];               /* pipe 保持可移植 */
	int running;

	/* capture 环形缓冲（采样为单位，int16） */
	int16_t *ring;
	size_t   ring_samps;
	size_t   head, tail;          /* head=写 tail=读 */
	ast_mutex_t rlock;
	ast_cond_t  rcond;

	int cap_rate;                 /* 下行采集采样率 */
	int pb_rate;                  /* 上行注入采样率 */
	int frame_samps;              /* 下行 20ms 采样数（按 cap_rate） */

	int pending_rate;             /* 非 0：capture 线程需热切到该采样率 */
	ast_mutex_t plock;            /* playback 与 reopen 互斥 */

	int cap_lost;                 /* 1=下行 tap 不可用 -> 静音模式 */
	int pb_lost;                  /* 1=上行 tap 不可用（下行继续） */
	int band_tick;                /* band 文件检查节流计数 */

	int cap_dump_fd;              /* 诊断：下行 dump（对方 -> 本机） */
	int pb_dump_fd;               /* 诊断：上行 dump（本机 -> 对方） */

	unsigned long pb_diag_frames;    /* 诊断：上行帧计数 */
	unsigned long pb_diag_samples;   /* 诊断：上行累计样本数 */
	unsigned long long pb_diag_sumsq; /* 诊断：上行累计平方和 */
	int pb_diag_peak;                /* 诊断：上行峰值幅度 */

	unsigned long rd_diag_calls;     /* 诊断：read 调用次数 */
	unsigned long rd_diag_ok;        /* 诊断：返回帧次数 */
	unsigned long rd_diag_null;      /* 诊断：返回 null 次数 */
	unsigned long rd_diag_partial;   /* 诊断：短帧（got<want）次数 */
};

/* ---------------- PCM 打开/设置 ---------------- */

static int pcm_open_part(struct pcm_part *p, const char *dev,
	snd_pcm_stream_t dir, int rate, int period_ms, int buffer_ms)
{
	int err;
	snd_pcm_uframes_t pf = rate * period_ms / 1000;
	snd_pcm_uframes_t bf = rate * buffer_ms / 1000;
	unsigned int r = rate;

	if ((err = snd_pcm_open(&p->h, dev, dir, 0)) < 0) {
		ast_log(LOG_ERROR, "e5_audio: open %s (%s) 失败: %s\n",
			dev, dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
			snd_strerror(err));
		return -1;
	}
	if ((err = snd_pcm_hw_params_malloc(&p->hw)) < 0) {
		ast_log(LOG_ERROR, "e5_audio: hw_params 分配失败: %s\n", snd_strerror(err));
		snd_pcm_close(p->h);
		p->h = NULL;
		return -1;
	}
	snd_pcm_hw_params_any(p->h, p->hw);
	snd_pcm_hw_params_set_access(p->h, p->hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(p->h, p->hw, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_channels(p->h, p->hw, 1);
	snd_pcm_hw_params_set_rate_near(p->h, p->hw, &r, 0);
	snd_pcm_hw_params_set_period_size_near(p->h, p->hw, &pf, 0);
	snd_pcm_hw_params_set_buffer_size_near(p->h, p->hw, &bf);
	if ((err = snd_pcm_hw_params(p->h, p->hw)) < 0) {
		ast_log(LOG_ERROR, "e5_audio: hw_params %s 失败: %s\n",
			dev, snd_strerror(err));
		goto fail;
	}
	if (r != (unsigned)rate) {
		ast_log(LOG_WARNING, "e5_audio: %s 不支持 %dHz，实际 %uHz（须修正路由）\n",
			dev, rate, r);
		goto fail;
	}
	snd_pcm_prepare(p->h);

	p->npfds = snd_pcm_poll_descriptors_count(p->h);
	p->pfds = ast_calloc(p->npfds, sizeof(*p->pfds));
	snd_pcm_poll_descriptors(p->h, p->pfds, p->npfds);
	ast_debug(1, "e5_audio: %s ready %uHz period=%lu buffer=%lu pfds=%lu\n",
		dev, r, (unsigned long)pf, (unsigned long)bf,
		(unsigned long)p->npfds);
	return 0;
fail:
	if (p->hw) {
		snd_pcm_hw_params_free(p->hw);
		p->hw = NULL;
	}
	snd_pcm_close(p->h);
	p->h = NULL;
	return -1;
}

static void pcm_close(struct pcm_part *p)
{
	if (p->h)
		snd_pcm_close(p->h);
	if (p->hw)
		snd_pcm_hw_params_free(p->hw);
	ast_free(p->pfds);
	memset(p, 0, sizeof(*p));
}

/* 读取 DSP 带宽文件并映射为采样率；读不到/内容非法返回 0 */
static int band_file_rate(const char *path)
{
	char b[8] = { 0 };
	int fd;
	if (!path || !*path)
		return 0;
	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	if (read(fd, b, sizeof(b) - 1) < 0)
		b[0] = '\0';
	close(fd);
	switch (b[0]) {
	case '0': return 8000;  /* NB */
	case '1': return 16000; /* WB */
	default:  return 0;
	}
}

static void audio_reopen(struct e5_audio *a, int new_rate)
{
	ast_mutex_lock(&a->plock);
	if (a->pb.h)
		snd_pcm_drop(a->pb.h);
	pcm_close(&a->cap);
	pcm_close(&a->pb);

	ast_mutex_lock(&a->rlock);
	a->head = a->tail = 0;
	ast_mutex_unlock(&a->rlock);

	a->cap_rate = new_rate;
	a->pb_rate = new_rate;
	a->frame_samps = new_rate *
		(a->cfg.period_ms ? a->cfg.period_ms : 20) / 1000;

	if (pcm_open_part(&a->cap, a->cfg.capture_dev, SND_PCM_STREAM_CAPTURE,
			new_rate, a->cfg.period_ms, a->cfg.buffer_ms) ||
	    pcm_open_part(&a->pb, a->cfg.playback_dev, SND_PCM_STREAM_PLAYBACK,
			new_rate, a->cfg.period_ms, a->cfg.buffer_ms)) {
		a->cap_lost = !a->cap.h;
		a->pb_lost = !a->pb.h;
		ast_log(LOG_WARNING,
			"e5_audio: 切换 %dHz 重开失败（cap=%s pb=%s），缺失侧进入静音/丢帧\n",
			new_rate, a->cap_lost ? "lost" : "ok", a->pb_lost ? "lost" : "ok");
		ast_mutex_unlock(&a->plock);
		return;
	}
	a->cap_lost = a->pb_lost = 0;
	if (snd_pcm_start(a->cap.h) < 0 || snd_pcm_start(a->pb.h) < 0)
		ast_debug(1, "e5_audio: 重开后 start 由 poll/首帧触发\n");
	ast_log(LOG_NOTICE, "e5_audio: DSP 带宽切换，PCM 已按 %dHz 重开\n", new_rate);
	ast_mutex_unlock(&a->plock);
}

/* 把零帧灌入环形缓冲（静音模式）：保持 Asterisk 侧 RTP 节奏存活 */
static void feed_silence(struct e5_audio *a)
{
	ast_mutex_lock(&a->rlock);
	for (size_t i = 0; i < a->frame_samps; i++) {
		size_t nh = (a->head + 1) % a->ring_samps;
		if (nh == a->tail)
			a->tail = (a->tail + 1) % a->ring_samps;
		a->ring[a->head] = 0;
		a->head = nh;
	}
	ast_cond_signal(&a->rcond);
	ast_mutex_unlock(&a->rlock);
}

/* capture 后台线程：poll -> readi -> 入环；下行失效时喂静音 */
static void *capture_thread(void *arg)
{
	struct e5_audio *a = arg;
	int16_t tmp[1600]; /* 一次最多 100ms@16k */
	/* ---- temp diag ---- */
	unsigned long diag_timeout = 0, diag_event = 0, diag_read = 0, diag_readfail = 0;
	unsigned long diag_bytes = 0, diag_samples = 0;
	unsigned long long diag_sumsq = 0;
	int diag_peak = 0;
	/* ------------------- */

	for (;;) {
		int want = a->pending_rate;
		if (!want && ++a->band_tick >= 25) {
			a->band_tick = 0;
			int br = band_file_rate(a->cfg.band_file);
			if (br && br != a->cap_rate)
				want = br;
		}
		if (want && (want == 8000 || want == 16000) && want != a->cap_rate) {
			audio_reopen(a, want);
			a->pending_rate = 0;
		} else if (want) {
			a->pending_rate = 0;
		}
		if (!a->cap.h) {
			/* 下行不可用：静音模式。红线：绝不回退打开 host 麦克风。 */
			feed_silence(a);
			struct timespec d = {
				.tv_nsec = (long)a->frame_samps * 1000000000L / a->cap_rate };
			nanosleep(&d, NULL);
			if (!a->running)
				break;
			continue;
		}
		struct pollfd fds[32];
		nfds_t n = a->cap.npfds + 1;
		if (n > 31)
			n = 31;
		for (nfds_t i = 0; i < a->cap.npfds && i < 30; i++) {
			fds[i].fd = a->cap.pfds[i].fd;
			fds[i].events = a->cap.pfds[i].events;
			fds[i].revents = 0;
		}
		fds[n - 1].fd = a->stopfds[0];
		fds[n - 1].events = POLLIN;

		int pr = poll(fds, n, 1000);
		if (pr <= 0) {
			if (pr == 0) {
				diag_timeout++;
				if (diag_timeout % 50 == 1)
					ast_log(LOG_NOTICE,
						"e5_audio DIAG: poll timeout x%lu (no data)\n",
						diag_timeout);
			}
			if (pr < 0 && errno != EINTR)
				ast_log(LOG_WARNING, "e5_audio: poll: %s\n", strerror(errno));
			if (!a->running)
				break;
			continue;
		}
		if (fds[n - 1].revents)
			break;
		diag_event++;

		snd_pcm_sframes_t avail = snd_pcm_avail_update(a->cap.h);
		if (avail <= 0) {
			if (avail == -EPIPE)
				snd_pcm_recover(a->cap.h, avail, 1);
			continue;
		}
		if (avail > (snd_pcm_sframes_t)ARRAY_LEN(tmp))
			avail = ARRAY_LEN(tmp);
		snd_pcm_sframes_t got = snd_pcm_readi(a->cap.h, tmp, avail);
		if (got < 0) {
			diag_readfail++;
			if (got == -ESTRPIPE || got == -ENODEV) {
				ast_log(LOG_WARNING,
					"e5_audio: capture 端点丢失(%ld)，转静音模式；"
					"写 band 文件或 set_rate 触发重开\n", (long)got);
				ast_mutex_lock(&a->plock);
				pcm_close(&a->cap);
				a->cap_lost = 1;
				ast_mutex_unlock(&a->plock);
				continue;
			}
			snd_pcm_recover(a->cap.h, got, 1);
			continue;
		}
		if (a->cfg.rxgain != 100) {
			int g = a->cfg.rxgain;
			for (long i = 0; i < got; i++)
				tmp[i] = (int16_t)(tmp[i] * g / 100);
		}
		diag_read++;
		diag_bytes += (unsigned long)got * sizeof(int16_t);
		diag_samples += (unsigned long)got;
		{
			int peak = 0;
			unsigned long long sq = 0;
			for (long i = 0; i < got; i++) {
				int v = tmp[i];
				if (v < 0)
					v = -v;
				if (v > peak)
					peak = v;
				sq += (long long)tmp[i] * tmp[i];
			}
			diag_sumsq += sq;
			if (peak > diag_peak)
				diag_peak = peak;
		}
		if (a->cfg.dump_capture) {
			if (a->cap_dump_fd < 0)
				a->cap_dump_fd = open("/tmp/e5_cap_2down_dump.raw",
					O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (a->cap_dump_fd >= 0)
				(void)write(a->cap_dump_fd, tmp, (size_t)got * sizeof(int16_t));
		}

		ast_mutex_lock(&a->rlock);
		for (long i = 0; i < got; i++) {
			size_t nh = (a->head + 1) % a->ring_samps;
			if (nh == a->tail)
				a->tail = (a->tail + 1) % a->ring_samps; /* 溢出丢最旧 */
			a->ring[a->head] = tmp[i];
			a->head = nh;
		}
		ast_cond_signal(&a->rcond);
		ast_mutex_unlock(&a->rlock);

		if (diag_read % 50 == 1) {
			unsigned long rms = diag_samples ?
				(unsigned long)sqrt((double)diag_sumsq / diag_samples) : 0;
			ast_log(LOG_NOTICE,
				"e5_audio DIAG-DOWN: event=%lu read_ok=%lu read_fail=%lu bytes=%lu rms=%lu peak=%d\n",
				diag_event, diag_read, diag_readfail, diag_bytes, rms, diag_peak);
		}
	}
	return NULL;
}

/* ---------------- 公共 API ---------------- */

struct e5_audio *e5_audio_open(const struct e5_audio_cfg *cfg)
{
	if (!cfg || ast_strlen_zero(cfg->capture_dev) ||
	    ast_strlen_zero(cfg->playback_dev)) {
		ast_log(LOG_NOTICE,
			"e5_audio: 未配置 tap 端点，chan_e5 工作在信令态（无语音桥）\n");
		return NULL;
	}

	struct e5_audio *a = ast_calloc(1, sizeof(*a));
	if (!a)
		return NULL;
	a->cfg = *cfg;
	a->cap_rate = cfg->cap_rate ? cfg->cap_rate : 8000;
	a->pb_rate  = cfg->pb_rate  ? cfg->pb_rate  : 8000;
	a->frame_samps = a->cap_rate * (cfg->period_ms ? cfg->period_ms : 20) / 1000;
	a->cap_dump_fd = -1;
	a->pb_dump_fd = -1;
	ast_mutex_init(&a->rlock);
	ast_cond_init(&a->rcond, NULL);
	ast_mutex_init(&a->plock);
	a->ring_samps = (size_t)a->cap_rate * RING_SECONDS;
	a->ring = ast_calloc(1, a->ring_samps * sizeof(int16_t));
	if (pipe(a->stopfds))
		goto fail;

	/* 单侧降级打开：下行不可用 -> 静音模式；上行不可用 -> 下行继续；
	 * 双侧都不可用才整体失败。 */
	if (pcm_open_part(&a->cap, cfg->capture_dev, SND_PCM_STREAM_CAPTURE,
			a->cap_rate, cfg->period_ms, cfg->buffer_ms)) {
		a->cap_lost = 1;
		ast_log(LOG_WARNING,
			"e5_audio: 下行 tap(%s) 打不开 -> 静音模式；"
			"不回退 host 麦克风，上行注入继续\n", cfg->capture_dev);
	}
	if (pcm_open_part(&a->pb, cfg->playback_dev, SND_PCM_STREAM_PLAYBACK,
			a->pb_rate, cfg->period_ms, cfg->buffer_ms)) {
		a->pb_lost = 1;
		if (a->cap_lost) {
			ast_log(LOG_ERROR, "e5_audio: 双向 tap 均不可用，回退信令态\n");
			goto fail;
		}
		ast_log(LOG_WARNING,
			"e5_audio: 上行 tap(%s) 打不开 -> 对端听不到 SIP（下行继续）\n",
			cfg->playback_dev);
	}

	a->running = 1;
	if (a->pb.h && snd_pcm_start(a->pb.h) < 0)
		ast_debug(1, "e5_audio: playback 预 start 由首帧触发\n");
	if (a->cap.h && snd_pcm_start(a->cap.h) < 0)
		ast_debug(1, "e5_audio: capture start 由 poll 触发\n");

	if (ast_pthread_create_background(&a->cap_thread, NULL, capture_thread, a))
		goto fail;
	return a;

fail:
	if (a->stopfds[0]) {
		close(a->stopfds[0]);
		close(a->stopfds[1]);
	}
	pcm_close(&a->cap);
	pcm_close(&a->pb);
	ast_free(a->ring);
	ast_mutex_destroy(&a->rlock);
	ast_cond_destroy(&a->rcond);
	ast_mutex_destroy(&a->plock);
	ast_free(a);
	return NULL;
}

void e5_audio_close(struct e5_audio *a)
{
	if (!a)
		return;
	a->running = 0;
	if (a->stopfds[1] >= 0) {
		char x = 1;
		write(a->stopfds[1], &x, 1);
	}
	if (a->cap_thread)
		pthread_join(a->cap_thread, NULL);
	if (a->cap_dump_fd >= 0) {
		close(a->cap_dump_fd);
		a->cap_dump_fd = -1;
	}
	if (a->pb_dump_fd >= 0) {
		close(a->pb_dump_fd);
		a->pb_dump_fd = -1;
	}
	if (a->pb.h)
		snd_pcm_drain(a->pb.h);
	close(a->stopfds[0]);
	close(a->stopfds[1]);
	pcm_close(&a->cap);
	pcm_close(&a->pb);
	ast_free(a->ring);
	ast_mutex_destroy(&a->rlock);
	ast_cond_destroy(&a->rcond);
	ast_mutex_destroy(&a->plock);
	ast_free(a);
}

struct ast_frame *e5_audio_read(struct e5_audio *a, int timeout_ms)
{
	int16_t buf[320]; /* 20ms@16k */
	size_t want = a->frame_samps;
	struct timespec ts;

	if (want > ARRAY_LEN(buf))
		want = ARRAY_LEN(buf);

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
	ts.tv_sec += timeout_ms / 1000 + ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;

	ast_mutex_lock(&a->rlock);
	size_t avail = (a->head + a->ring_samps - a->tail) % a->ring_samps;
	if (avail < want)
		ast_cond_timedwait(&a->rcond, &a->rlock, &ts);

	size_t got = 0;
	while (got < want) {
		size_t cur = (a->head + a->ring_samps - a->tail) % a->ring_samps;
		if (cur == 0)
			break;
		buf[got++] = a->ring[a->tail];
		a->tail = (a->tail + 1) % a->ring_samps;
	}
	ast_mutex_unlock(&a->rlock);

	if (got == 0) {
		a->rd_diag_calls++;
		a->rd_diag_null++;
		if (a->rd_diag_calls % 50 == 1)
			ast_log(LOG_NOTICE,
				"e5_audio DIAG-READ: calls=%lu ok=%lu null=%lu partial=%lu (本次无数据)\n",
				a->rd_diag_calls, a->rd_diag_ok, a->rd_diag_null, a->rd_diag_partial);
		return NULL;
	}

	a->rd_diag_calls++;
	a->rd_diag_ok++;
	if (got < want)
		a->rd_diag_partial++;
	if (a->rd_diag_calls % 50 == 1)
		ast_log(LOG_NOTICE,
			"e5_audio DIAG-READ: calls=%lu ok=%lu null=%lu partial=%lu samples=%zu\n",
			a->rd_diag_calls, a->rd_diag_ok, a->rd_diag_null, a->rd_diag_partial, got);

	/* 栈上组帧 -> ast_frisolate() 复制出带 AST_FRIENDLY_OFFSET 的堆帧。 */
	struct ast_frame f = {
		.frametype = AST_FRAME_VOICE,
		.datalen   = got * sizeof(int16_t),
		.samples   = got,
		.offset    = 0,
		.data.ptr  = buf,
		.src       = AST_MODULE,
	};
	f.subclass.format = a->cap_rate >= 16000 ? ast_format_slin16 : ast_format_slin;
	return ast_frisolate(&f);
}

int e5_audio_write(struct e5_audio *a, struct ast_frame *f)
{
	if (!a || !f || f->frametype != AST_FRAME_VOICE || f->samples == 0)
		return 0; /* 绝不在 write 回调返回 -1，否则 Asterisk 会当成写失败直接挂断 */

	size_t n = f->samples;
	int16_t *s = f->data.ptr;

	/* 输入采样率：仅接受 slinear(8k)/slinear16(16k)，其余丢帧不报错 */
	int in_rate;
	if (ast_format_cmp(f->subclass.format, ast_format_slin16) == AST_FORMAT_CMP_EQUAL)
		in_rate = 16000;
	else if (ast_format_cmp(f->subclass.format, ast_format_slin) == AST_FORMAT_CMP_EQUAL)
		in_rate = 8000;
	else
		return 0;

	/* 上行物理 tap 固定速率可能不同于 Asterisk 侧，8k<->16k 2x 重采样到 pb_rate */
	int16_t rsbuf[640];
	if (in_rate != a->pb_rate) {
		if (a->pb_rate == 16000 && in_rate == 8000) {
			size_t in = n;
			size_t out = in * 2;
			if (out > ARRAY_LEN(rsbuf))
				out = ARRAY_LEN(rsbuf);
			for (size_t i = 0; i < out / 2 && i < in; i++) {
				int16_t a0 = s[i];
				int16_t a1 = (i + 1 < in) ? s[i + 1] : a0;
				rsbuf[2 * i] = a0;
				rsbuf[2 * i + 1] = (int16_t)(((int)a0 + (int)a1) / 2);
			}
			s = rsbuf;
			n = out;
		} else if (a->pb_rate == 8000 && in_rate == 16000) {
			size_t out = n / 2;
			for (size_t i = 0; i < out; i++)
				rsbuf[i] = s[2 * i];
			s = rsbuf;
			n = out;
		}
	}

	int16_t gainbuf[640];
	if (a->cfg.txgain != 100) {
		if (n > ARRAY_LEN(gainbuf))
			n = ARRAY_LEN(gainbuf);
		int g = a->cfg.txgain;
		for (size_t i = 0; i < n; i++)
			gainbuf[i] = (int16_t)(s[i] * g / 100);
		s = gainbuf;
	}
	ast_mutex_lock(&a->plock);
	if (!a->pb.h) {
		ast_mutex_unlock(&a->plock);
		return 0; /* 带宽切换重开窗口期，丢帧不阻塞 */
	}
	if (a->cfg.dump_playback) {
		if (a->pb_dump_fd < 0)
			a->pb_dump_fd = open("/tmp/e5_cap_2up_dump.raw",
				O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (a->pb_dump_fd >= 0)
			(void)write(a->pb_dump_fd, s, n * sizeof(int16_t));
	}
	/* 诊断：上行帧能量 */
	{
		int peak = 0;
		unsigned long long sq = 0;
		for (size_t i = 0; i < n; i++) {
			int v = s[i];
			if (v < 0)
				v = -v;
			if (v > peak)
				peak = v;
			sq += (long long)s[i] * s[i];
		}
		a->pb_diag_frames++;
		a->pb_diag_samples += n;
		a->pb_diag_sumsq += sq;
		if (peak > a->pb_diag_peak)
			a->pb_diag_peak = peak;
		if (a->pb_diag_frames % 50 == 1) {
			unsigned long rms = a->pb_diag_samples ?
				(unsigned long)sqrt((double)a->pb_diag_sumsq / a->pb_diag_samples) : 0;
			ast_log(LOG_NOTICE,
				"e5_audio DIAG-UP: frames=%lu rms=%lu peak=%d fmt=%s samples=%zu (70->对方方向)\n",
				a->pb_diag_frames, rms, a->pb_diag_peak,
				ast_format_get_name(f->subclass.format), n);
		}
	}
	snd_pcm_sframes_t w = snd_pcm_writei(a->pb.h, s, n);
	ast_mutex_unlock(&a->plock);
	if (w < 0) {
		if (w == -ESTRPIPE)
			return 0;
		ast_mutex_lock(&a->plock);
		if (a->pb.h)
			snd_pcm_recover(a->pb.h, w, 1);
		ast_mutex_unlock(&a->plock);
		return 0; /* 丢一帧好过阻塞 PBX 线程 */
	}
	return 0;
}

int e5_audio_set_rate(struct e5_audio *a, int rate)
{
	if (!a || (rate != 8000 && rate != 16000) || rate == a->cap_rate)
		return -1;
	a->pending_rate = rate;
	return 0;
}

int e5_audio_is_open(struct e5_audio *a)
{
	return a != NULL && a->running;
}

int e5_audio_capture_ok(struct e5_audio *a)
{
	return a != NULL && !a->cap_lost && a->cap.h != NULL;
}
