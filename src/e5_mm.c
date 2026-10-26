/*
 * e5_mm.c - ModemManager D-Bus 客户端实现
 *
 * 蓝本：openwrt_device_common 中 e5-voice-audio 的 CallBus（Python libgio）。
 * 这里是等价的 C/GIO 实现，供 chan_e5 直接链接。
 */
#include "e5_mm.h"

#include <asterisk.h>
#include <asterisk/utils.h>
#include <asterisk/lock.h>
#include <asterisk/strings.h>

#include <string.h>
#include <stdlib.h>

#define MM_BUS_NAME    "org.freedesktop.ModemManager1"
#define MM_OBJ_PATH    "/org/freedesktop/ModemManager1"
#define MM_IF_MODEM    "org.freedesktop.ModemManager1.Modem"
#define MM_IF_VOICE    "org.freedesktop.ModemManager1.Modem.Voice"
#define MM_IF_MESSAGING "org.freedesktop.ModemManager1.Modem.Messaging"
#define MM_IF_CALL     "org.freedesktop.ModemManager1.Call"
#define MM_IF_SMS      "org.freedesktop.ModemManager1.Sms"
#define MM_IF_OM       "org.freedesktop.DBus.ObjectManager"
#define MM_IF_PC       "org.freedesktop.DBus.Properties"
#define MM_CALL_PATH_PREFIX "/org/freedesktop/ModemManager1/Call/"
#define MM_SMS_PATH_PREFIX  "/org/freedesktop/ModemManager1/SMS/"
#define MM_CALL_TIMEOUT_MS 3000

AST_MUTEX_DEFINE_STATIC(init_lock);

struct e5_mm {
	GDBusConnection *bus;
	GMainContext    *ctx;
	GMainLoop       *loop;
	pthread_t        thread;
	guint            sig_calls;      /* Call 对象上的 PropertiesChanged */
	guint            sig_voice;      /* Voice.CallAdded/CallDeleted */
	guint            sig_owner;      /* MM 名主变化 */
	guint            sig_messaging;  /* Messaging.Added/Deleted */
	ast_mutex_t      lock;
	char            *modem_path;     /* 当前带 Voice 接口的 modem */
	int              online;
	struct e5_mm_events ev;

	/* 周期对账：D-Bus 信号回调在本进程不可靠（见 e5-voice-audio 参考实现，
	 * 其用 dbus-monitor + 周期轮询兜底），这里用独立线程 + 同步 ListCalls
	 * 对比快照，主动触发 ev 回调。 */
	pthread_t        poll_thread;
	volatile int     poll_stop;
	GHashTable      *snapshot;       /* char* path -> GINT_TO_POINTER(state) */
};

/* ---------- 小工具 ---------- */

static void call_clear(struct e5_mm_call *c)
{
	ast_free(c->path);
	ast_free(c->number);
	memset(c, 0, sizeof(*c));
}

static gboolean call_sync(GDBusConnection *bus, const char *path, const char *iface,
	const char *method, GVariant *params, GVariant **out, GError **err)
{
	ast_debug(1, "D-Bus => %s.%s path=%s\n", iface, method, path);
	GVariant *r = g_dbus_connection_call_sync(bus, MM_BUS_NAME, path, iface, method,
		params ? params : g_variant_new_tuple(NULL, 0), G_VARIANT_TYPE_TUPLE,
		G_DBUS_CALL_FLAGS_NONE, MM_CALL_TIMEOUT_MS, NULL, err);
	if (!r) {
		ast_debug(1, "D-Bus <= %s.%s FAILED: %s\n", iface, method,
			(err && *err) ? (*err)->message : "?");
		return FALSE;
	}
	ast_debug(1, "D-Bus <= %s.%s OK\n", iface, method);
	if (out)
		*out = r;
	else
		g_variant_unref(r);
	return TRUE;
}

static GVariant *call_getall(struct e5_mm *mm, const char *path)
{
	GError *err = NULL;
	GVariant *r = NULL;
	if (!call_sync(mm->bus, path, MM_IF_PC, "GetAll",
			g_variant_new("(s)", MM_IF_CALL), &r, &err)) {
		ast_log(LOG_WARNING, "e5_mm: GetAll %s 失败: %s\n", path, err->message);
		g_error_free(err);
		return NULL;
	}
	return r;
}

/* 把单个 GVariant 解析为 int：兼容 MM 补丁版可能使用的各种 D-Bus 整型表示
 * （u/i/t/x/y/s）。不调用 g_variant_get_uint32 等强类型接口，避免类型不匹配
 * 时触发 glib 断言崩溃。成功返回 0。 */
static int variant_to_int(GVariant *v, int *out)
{
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
		*out = (int)g_variant_get_uint32(v);
	else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32))
		*out = g_variant_get_int32(v);
	else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64))
		*out = (int)g_variant_get_uint64(v);
	else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
		*out = (int)g_variant_get_int64(v);
	else if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE))
		*out = g_variant_get_byte(v);
	else if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
		const char *sv = g_variant_get_string(v, NULL);
		*out = sv ? atoi(sv) : 0;
	} else
		return -1;
	return 0;
}

/* 从 a{sv} 字典按 key 读整型，并打印实际 D-Bus 类型，用于定位
 * State/Direction 被解析为 0 的类型不匹配问题。 */
static int prop_read_int(GVariant *props_dict, const char *key, int *out)
{
	GVariant *v = g_variant_lookup_value(props_dict, key, NULL);
	if (!v) {
		ast_debug(1, "prop_read_int: 属性 %s 不存在\n", key);
		return -1;
	}
	const char *ts = g_variant_get_type_string(v);
	int rc = variant_to_int(v, out);
	ast_debug(1, "prop_read_int: %s = %d（D-Bus 类型 %s，%s）\n", key, *out, ts,
		rc ? "解析失败" : "OK");
	g_variant_unref(v);
	return rc;
}

static int parse_call_props(struct e5_mm_call *c, GVariant *props_dict)
{
	GVariant *v;
	gchar *s;

	memset(c, 0, sizeof(*c));

	if (g_variant_lookup(props_dict, "Number", "s", &s)) {
		c->number = ast_strdup(s);
		g_free(s);
	} else
		c->number = ast_strdup("");

	prop_read_int(props_dict, "State", &c->state);
	prop_read_int(props_dict, "StateReason", &c->reason);
	prop_read_int(props_dict, "Direction", &c->direction);
	prop_read_int(props_dict, "SimSlot", &c->sim_slot);

	if ((v = g_variant_lookup_value(props_dict, "Multiparty", G_VARIANT_TYPE_BOOLEAN))) {
		c->multiparty = g_variant_get_boolean(v);
		g_variant_unref(v);
	}
	return c->number ? 0 : -1;
}

/* 读取单个 Call 对象并填充 */
static int fetch_call(struct e5_mm *mm, const char *path, struct e5_mm_call *out)
{
	ast_debug(1, "fetch_call path=%s\n", path);
	GVariant *r = call_getall(mm, path);
	if (!r) {
		ast_debug(1, "fetch_call GetAll failed path=%s\n", path);
		return -1;
	}
	GVariant *dict = g_variant_get_child_value(r, 0); /* a{sv} */
	int rc = -1;
	memset(out, 0, sizeof(*out));
	if (!parse_call_props(out, dict)) {
		out->path = ast_strdup(path);
		rc = 0;
		ast_debug(1, "fetch_call OK: number=%s state=%s dir=%d sim=%d reason=%d\n",
			out->number, e5_mm_state_name(out->state),
			out->direction, out->sim_slot, out->reason);
	}
	g_variant_unref(dict);
	g_variant_unref(r);
	if (rc)
		call_clear(out);
	return rc;
}

/* ---------- modem 枚举 ---------- */

static void refresh_modem_locked(struct e5_mm *mm)
{
	GError *err = NULL;
	GVariant *r = NULL;

	ast_debug(1, "refresh_modem_locked: 开始枚举 modem\n");
	ast_free(mm->modem_path);
	mm->modem_path = NULL;
	mm->online = 0;

	if (!call_sync(mm->bus, MM_OBJ_PATH, MM_IF_OM, "GetManagedObjects",
			NULL, &r, &err)) {
		ast_log(LOG_WARNING, "e5_mm: GetManagedObjects 失败: %s\n",
			err ? err->message : "?");
		ast_debug(1, "GetManagedObjects 失败: %s\n", err ? err->message : "?");
		g_clear_error(&err);
		return;
	}

	GVariant *objs = g_variant_get_child_value(r, 0);
	GVariantIter it;
	const char *opath;
	GVariant *ifaces;

	g_variant_iter_init(&it, objs);
	while (!mm->online && g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &opath, &ifaces)) {
		GVariant *voice = g_variant_lookup_value(ifaces, MM_IF_VOICE,
			G_VARIANT_TYPE_VARDICT);
		g_variant_unref(ifaces);
		if (voice) {
			g_variant_unref(voice);
			mm->modem_path = ast_strdup(opath);
			mm->online = 1;
		}
	}
	g_variant_unref(objs);
	g_variant_unref(r);

	if (mm->online) {
		ast_debug(1, "e5_mm: 使用 modem %s\n", mm->modem_path);
		ast_debug(1, "refresh_modem_locked: 找到 modem %s\n", mm->modem_path);
	} else {
		ast_debug(1, "refresh_modem_locked: 未找到带 Voice 接口的 modem\n");
	}
}

/* ---------- D-Bus 信号处理 ---------- */

static void on_properties_changed(GDBusConnection *bus, const gchar *sender,
	const gchar *path, const gchar *iface, const gchar *signal,
	GVariant *params, gpointer user_data)
{
	struct e5_mm *mm = user_data;
	const char *changed_iface = NULL;
	GVariant *changed_dict;   /* a{sv} */
	GVariant *invalidated;    /* as（本驱动不关心） */
	int state = -1;
	int reason = E5_REASON_UNKNOWN;

	ast_debug(1, "on_properties_changed 到达: path=%s iface=%s signal=%s\n",
		path, iface, signal);

	if (!g_str_has_prefix(path, MM_CALL_PATH_PREFIX))
		return;

	/* PropertiesChanged 签名 (sa{sv}as)。用 @ 取 GVariant，再用栈迭代器
	 * 遍历，避免堆/栈迭代器混淆导致 g_variant_iter_free 断言崩溃。 */
	g_variant_get(params, "(&s@a{sv}@as)", &changed_iface, &changed_dict, &invalidated);

	ast_debug(1, "PropertiesChanged path=%s iface=%s\n", path, changed_iface);

	if (strcmp(changed_iface, MM_IF_CALL)) {
		ast_debug(1, "PropertiesChanged 忽略非 Call 接口: %s\n", changed_iface);
		g_variant_unref(changed_dict);
		g_variant_unref(invalidated);
		return;
	}

	{
		GVariantIter iter;
		const char *prop_name;
		GVariant *prop_val;
		g_variant_iter_init(&iter, changed_dict);
		while (g_variant_iter_loop(&iter, "{&sv}", &prop_name, &prop_val)) {
			if (!strcmp(prop_name, "State"))
				variant_to_int(prop_val, &state);
			else if (!strcmp(prop_name, "StateReason"))
				variant_to_int(prop_val, &reason);
		}
	}
	g_variant_unref(changed_dict);
	g_variant_unref(invalidated);

	if (state < 0) {
		ast_debug(1, "PropertiesChanged 不含 State，忽略 path=%s\n", path);
		return;
	}

	ast_debug(1, "PropertiesChanged: path=%s state=%s(%d) reason=%d\n",
		path, e5_mm_state_name(state), state, reason);

	/* 关键：信号回调里绝不发起同步 D-Bus 调用（会嵌套迭代 mm->ctx 导致
	 * 重入并拿已持有的锁）。这里只把信号参数透传给上层。 */
	ast_mutex_lock(&mm->lock);
	if (state == E5_CALL_TERMINATED) {
		if (mm->ev.call_removed)
			mm->ev.call_removed(mm, path, mm->ev.user);
	} else if (mm->ev.call_state_changed) {
		mm->ev.call_state_changed(mm, path, state, reason, mm->ev.user);
	}
	ast_mutex_unlock(&mm->lock);
}

/* CallAdded 的异步拉取任务：信号回调里不能同步 call_sync（会嵌套迭代
 * mm->ctx），改为 g_dbus_connection_call 异步，完成回调在下一次迭代触发。 */
struct call_fetch_task {
	struct e5_mm *mm;
	char *path;
};

static void call_added_fetched(GObject *src, GAsyncResult *res, gpointer data)
{
	struct call_fetch_task *t = data;
	GVariant *r = NULL;
	GError *err = NULL;
	struct e5_mm_call c;

	ast_debug(1, "call_added_fetched: 异步 GetAll 完成 path=%s\n", t->path);

	r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
	if (!r) {
		ast_log(LOG_WARNING, "e5_mm: 异步 GetAll(%s) 失败: %s\n",
			t->path, err ? err->message : "?");
		ast_debug(1, "call_added_fetched 失败: %s\n", err ? err->message : "?");
		g_clear_error(&err);
		goto out;
	}

	{
		GVariant *dict = g_variant_get_child_value(r, 0);
		memset(&c, 0, sizeof(c));
		if (!parse_call_props(&c, dict)) {
			c.path = ast_strdup(t->path);
			ast_debug(1, "CallAdded 详情: number=%s state=%s dir=%d sim=%d\n",
				c.number, e5_mm_state_name(c.state), c.direction, c.sim_slot);
			ast_mutex_lock(&t->mm->lock);
			if (t->mm->ev.call_added)
				t->mm->ev.call_added(t->mm, &c, t->mm->ev.user);
			ast_mutex_unlock(&t->mm->lock);
			call_clear(&c);
		}
		g_variant_unref(dict);
	}
	g_variant_unref(r);
out:
	ast_free(t->path);
	ast_free(t);
}

static void on_voice_signal(GDBusConnection *bus, const gchar *sender,
	const gchar *path, const gchar *iface, const gchar *signal,
	GVariant *params, gpointer user_data)
{
	struct e5_mm *mm = user_data;
	ast_debug(1, "on_voice_signal 到达: signal=%s iface=%s path=%s sender=%s\n",
		signal, iface, path, sender);
	GVariant *arg0 = g_variant_get_child_value(params, 0);
	const char *call_path = arg0 ? g_variant_get_string(arg0, NULL) : NULL;
	if (!call_path)
		goto done;

	ast_debug(1, "Voice 信号 %s call_path=%s\n", signal, call_path);

	if (!strcmp(signal, "CallDeleted")) {
		ast_mutex_lock(&mm->lock);
		if (mm->ev.call_removed)
			mm->ev.call_removed(mm, call_path, mm->ev.user);
		ast_mutex_unlock(&mm->lock);
	} else if (!strcmp(signal, "CallAdded")) {
		struct call_fetch_task *t = ast_malloc(sizeof(*t));
		if (t) {
			t->mm = mm;
			t->path = ast_strdup(call_path);
			g_dbus_connection_call(mm->bus, MM_BUS_NAME, call_path, MM_IF_PC,
				"GetAll", g_variant_new("(s)", MM_IF_CALL), G_VARIANT_TYPE_TUPLE,
				G_DBUS_CALL_FLAGS_NONE, MM_CALL_TIMEOUT_MS, NULL,
				call_added_fetched, t);
		}
	}
done:
	if (arg0)
		g_variant_unref(arg0);
}

/* ---------- SMS（Modem.Messaging） ---------- */

static void sms_clear(struct e5_mm_sms *s)
{
	ast_free(s->path);
	ast_free(s->number);
	ast_free(s->text);
	ast_free(s->timestamp);
	memset(s, 0, sizeof(*s));
}

static int fetch_sms(struct e5_mm *mm, const char *path, struct e5_mm_sms *out)
{
	ast_debug(1, "fetch_sms path=%s\n", path);
	GError *err = NULL;
	GVariant *r = NULL;
	if (!call_sync(mm->bus, path, MM_IF_PC, "GetAll",
			g_variant_new("(s)", MM_IF_SMS), &r, &err)) {
		ast_log(LOG_WARNING, "e5_mm: GetAll %s 失败: %s\n", path, err->message);
		g_error_free(err);
		return -1;
	}
	GVariant *dict = g_variant_get_child_value(r, 0);
	memset(out, 0, sizeof(*out));
	out->path = ast_strdup(path);
	gchar *s;
	GVariant *v;
	if (g_variant_lookup(dict, "Number", "s", &s)) {
		out->number = ast_strdup(s);
		g_free(s);
	} else
		out->number = ast_strdup("");
	if (g_variant_lookup(dict, "Text", "s", &s)) {
		out->text = ast_strdup(s);
		g_free(s);
	} else
		out->text = ast_strdup("");
	if (g_variant_lookup(dict, "Timestamp", "s", &s)) {
		out->timestamp = ast_strdup(s);
		g_free(s);
	} else
		out->timestamp = ast_strdup("");
	if ((v = g_variant_lookup_value(dict, "State", G_VARIANT_TYPE_UINT32))) {
		out->state = g_variant_get_uint32(v);
		g_variant_unref(v);
	}
	if ((v = g_variant_lookup_value(dict, "PduType", G_VARIANT_TYPE_UINT32))) {
		out->pdu_type = g_variant_get_uint32(v);
		g_variant_unref(v);
	}
	g_variant_unref(dict);
	g_variant_unref(r);
	if (!out->path || !out->number || !out->text || !out->timestamp) {
		sms_clear(out);
		return -1;
	}
	return 0;
}

static void on_messaging_signal(GDBusConnection *bus, const gchar *sender,
	const gchar *path, const gchar *iface, const gchar *signal,
	GVariant *params, gpointer user_data)
{
	struct e5_mm *mm = user_data;
	if (strcmp(signal, "Added"))
		return;
	const char *sms_path;
	gboolean received;
	g_variant_get(params, "(&ob)", &sms_path, &received);
	ast_debug(1, "Messaging.Added sms_path=%s received=%d\n", sms_path, received);
	if (!received || !g_str_has_prefix(sms_path, MM_SMS_PATH_PREFIX))
		return;

	struct e5_mm_sms sms;
	if (fetch_sms(mm, sms_path, &sms))
		return;
	ast_debug(1, "收到短信: from=%s text=%s ts=%s\n", sms.number, sms.text, sms.timestamp);
	if (mm->ev.sms_received)
		mm->ev.sms_received(mm, &sms, mm->ev.user);
	sms_clear(&sms);
}

static void on_owner_changed(GDBusConnection *bus, const gchar *sender,
	const gchar *path, const gchar *iface, const gchar *signal,
	GVariant *params, gpointer user_data)
{
	struct e5_mm *mm = user_data;
	const char *name, *old_owner, *new_owner;
	g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
	ast_debug(1, "NameOwnerChanged name=%s new_owner='%s'\n", name, new_owner);
	if (strcmp(name, MM_BUS_NAME))
		return;

	ast_mutex_lock(&mm->lock);
	if (*new_owner) {
		ast_debug(1, "ModemManager 出现，重新枚举\n");
		refresh_modem_locked(mm);
		if (mm->online && mm->ev.service_changed)
			mm->ev.service_changed(mm, 1, mm->ev.user);
	} else {
		ast_debug(1, "ModemManager 消失\n");
		mm->online = 0;
		ast_free(mm->modem_path);
		mm->modem_path = NULL;
		if (mm->ev.service_changed)
			mm->ev.service_changed(mm, 0, mm->ev.user);
	}
	ast_mutex_unlock(&mm->lock);
}

static void *mm_thread_main(void *arg)
{
	struct e5_mm *mm = arg;
	ast_debug(1, "mm_thread_main: 进入，开始运行事件循环\n");
	g_main_context_push_thread_default(mm->ctx);
	g_main_loop_run(mm->loop);
	g_main_context_pop_thread_default(mm->ctx);
	ast_debug(1, "mm_thread_main: 事件循环退出\n");
	return NULL;
}

/* ---------------- 周期对账（替代不可靠的 D-Bus 信号） ----------------
 * 参考 openwrt_device_common 的 e5-voice-audio：其信号监听用 dbus-monitor
 * 外部进程 + 周期读取兜底，而非 GDBusConnection 信号回调（本进程环境回调
 * 不触发）。这里用独立线程 + 同步 ListCalls 对比快照，主动抛出 ev 回调。 */

static void snapshot_fill_cb(const struct e5_mm_call *c, void *user)
{
	GHashTable *h = user;
	g_hash_table_insert(h, g_strdup(c->path), GINT_TO_POINTER(c->state));
}

struct reconcile_ctx {
	struct e5_mm *mm;
	GHashTable *seen;   /* char* path -> GINT_TO_POINTER(state) */
};

static void reconcile_cb(const struct e5_mm_call *c, void *user)
{
	struct reconcile_ctx *ctx = user;
	struct e5_mm *mm = ctx->mm;

	g_hash_table_insert(ctx->seen, g_strdup(c->path), GINT_TO_POINTER(c->state));

	gpointer oldp = g_hash_table_lookup(mm->snapshot, c->path);
	if (!oldp) {
		ast_debug(1, "reconcile: 新增通话 %s state=%s dir=%d sim=%d\n",
			c->path, e5_mm_state_name(c->state), c->direction, c->sim_slot);
		if (mm->ev.call_added)
			mm->ev.call_added(mm, c, mm->ev.user);
	} else if (GPOINTER_TO_INT(oldp) != c->state) {
		ast_debug(1, "reconcile: 状态变化 %s %s(%d) -> %s(%d) reason=%d\n",
			c->path, e5_mm_state_name(GPOINTER_TO_INT(oldp)),
			GPOINTER_TO_INT(oldp), e5_mm_state_name(c->state), c->state, c->reason);
		if (mm->ev.call_state_changed)
			mm->ev.call_state_changed(mm, c->path, c->state, c->reason, mm->ev.user);
		/* 来电首次轮询可能仍是 unknown，进入 ringing-in 时补一次接管判断；
		 * on_call_added 内部会 find_pvt_for_call 去重，已接管的不会重复。 */
		if (c->state == E5_CALL_RINGING_IN && c->direction == E5_DIR_INCOMING) {
			if (mm->ev.call_added)
				mm->ev.call_added(mm, c, mm->ev.user);
		}
	}
}

static void poll_reconcile(struct e5_mm *mm)
{
	struct reconcile_ctx ctx;
	ctx.mm = mm;
	ctx.seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	/* 同步列当前 calls；reconcile_cb 在 mm->lock 下执行，触发新增/变化 */
	if (e5_mm_list_calls(mm, reconcile_cb, &ctx) == 0) {
		GHashTableIter it;
		gpointer k, v;
		g_hash_table_iter_init(&it, mm->snapshot);
		while (g_hash_table_iter_next(&it, &k, &v)) {
			const char *path = k;
			if (!g_hash_table_lookup(ctx.seen, path)) {
				ast_debug(1, "reconcile: 通话消失 %s\n", path);
				if (mm->ev.call_removed)
					mm->ev.call_removed(mm, path, mm->ev.user);
			}
		}
	}

	g_hash_table_destroy(mm->snapshot);
	mm->snapshot = ctx.seen;
}

static void *poll_thread_main(void *arg)
{
	struct e5_mm *mm = arg;
	ast_debug(1, "poll_thread_main: 开始周期对账（间隔 500 ms）\n");
	while (!mm->poll_stop) {
		g_usleep(500000);
		if (mm->poll_stop)
			break;
		poll_reconcile(mm);
	}
	ast_debug(1, "poll_thread_main: 退出\n");
	return NULL;
}

static void init_snapshot(struct e5_mm *mm)
{
	GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	e5_mm_list_calls(mm, snapshot_fill_cb, h);
	mm->snapshot = h;
}

/* ---------- 公共 API ---------- */

struct e5_mm *e5_mm_init(const struct e5_mm_events *ev)
{
	ast_debug(1, "e5_mm_init: 进入\n");
	ast_mutex_lock(&init_lock);

	struct e5_mm *mm = ast_calloc(1, sizeof(*mm));
	if (!mm) {
		ast_debug(1, "e5_mm_init: calloc 失败\n");
		goto out;
	}
	ast_mutex_init(&mm->lock);
	if (ev)
		mm->ev = *ev;
	mm->ctx = g_main_context_new();
	mm->loop = g_main_loop_new(mm->ctx, FALSE);

	GError *err = NULL;
	ast_debug(1, "e5_mm_init: 连接 system bus（专属连接绑定 mm->ctx）\n");
	/* 不用 g_bus_get_sync 的共享单例：它的信号分发 context 固定在首次创建时的
	 * thread-default，若与本进程其它代码冲突，信号回调永不触发。这里显式创建
	 * 专属连接，在 push 期间创建从而绑定到 mm->ctx，由 mm_thread_main 迭代。 */
	g_main_context_push_thread_default(mm->ctx);
	gchar *bus_addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (bus_addr) {
		mm->bus = g_dbus_connection_new_for_address_sync(bus_addr,
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
			G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
			NULL, NULL, &err);
		g_free(bus_addr);
	} else {
		mm->bus = NULL;
	}
	g_main_context_pop_thread_default(mm->ctx);
	if (!mm->bus) {
		ast_log(LOG_ERROR, "e5_mm: 无法连接 system bus: %s\n",
			err ? err->message : "?");
		ast_debug(1, "e5_mm_init: 连接 system bus 失败: %s\n", err ? err->message : "?");
		g_clear_error(&err);
		g_main_loop_unref(mm->loop);
		g_main_context_unref(mm->ctx);
		ast_mutex_destroy(&mm->lock);
		ast_free(mm);
		mm = NULL;
		goto out;
	}

	ast_debug(1, "e5_mm_init: 订阅信号 (PropertiesChanged/Voice/Messaging/NameOwnerChanged)\n");
	/* sender 置 NULL（任意发送者），避免 well-known name 的 match rule
	 * 在某些 dbus-daemon 上不匹配导致收不到信号；精确过滤放到回调里做。 */
	mm->sig_calls = g_dbus_connection_signal_subscribe(mm->bus, NULL,
		MM_IF_PC, "PropertiesChanged", NULL, NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, on_properties_changed, mm, NULL);
	mm->sig_voice = g_dbus_connection_signal_subscribe(mm->bus, NULL,
		MM_IF_VOICE, NULL, NULL, NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, on_voice_signal, mm, NULL);
	mm->sig_messaging = g_dbus_connection_signal_subscribe(mm->bus, NULL,
		MM_IF_MESSAGING, NULL, NULL, NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, on_messaging_signal, mm, NULL);
	mm->sig_owner = g_dbus_connection_signal_subscribe(mm->bus,
		"org.freedesktop.DBus", "org.freedesktop.DBus",
		"NameOwnerChanged", "/org/freedesktop/DBus", NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, on_owner_changed, mm, NULL);
	ast_debug(1, "e5_mm_init: 订阅ID calls=%u voice=%u messaging=%u owner=%u\n",
		mm->sig_calls, mm->sig_voice, mm->sig_messaging, mm->sig_owner);

	ast_mutex_lock(&mm->lock);
	refresh_modem_locked(mm);
	ast_mutex_unlock(&mm->lock);

	/* 初始化通话快照（避免首轮轮询把既有通话误判为新增） */
	init_snapshot(mm);

	if (ast_pthread_create_background(&mm->thread, NULL, mm_thread_main, mm)) {
		ast_log(LOG_ERROR, "e5_mm: 事件线程创建失败\n");
		ast_debug(1, "e5_mm_init: 事件线程创建失败\n");
	} else {
		ast_debug(1, "e5_mm_init: 事件线程已创建\n");
	}

	/* 启动周期对账线程（主信号源） */
	mm->poll_stop = 0;
	if (ast_pthread_create_background(&mm->poll_thread, NULL, poll_thread_main, mm)) {
		ast_log(LOG_ERROR, "e5_mm: 对账线程创建失败\n");
		ast_debug(1, "e5_mm_init: 对账线程创建失败\n");
	} else {
		ast_debug(1, "e5_mm_init: 对账线程已创建\n");
	}

out:
	ast_mutex_unlock(&init_lock);
	ast_debug(1, "e5_mm_init: 返回 mm=%p online=%d\n", mm, mm ? mm->online : -1);
	return mm;
}

void e5_mm_shutdown(struct e5_mm *mm)
{
	ast_debug(1, "e5_mm_shutdown: 进入\n");
	if (!mm)
		return;
	if (mm->loop && g_main_loop_is_running(mm->loop))
		g_main_loop_quit(mm->loop);
	if (mm->thread)
		pthread_join(mm->thread, NULL);
	if (mm->poll_thread) {
		mm->poll_stop = 1;
		pthread_join(mm->poll_thread, NULL);
	}
	if (mm->snapshot) {
		g_hash_table_destroy(mm->snapshot);
		mm->snapshot = NULL;
	}
	g_dbus_connection_signal_unsubscribe(mm->bus, mm->sig_calls);
	g_dbus_connection_signal_unsubscribe(mm->bus, mm->sig_voice);
	g_dbus_connection_signal_unsubscribe(mm->bus, mm->sig_messaging);
	g_dbus_connection_signal_unsubscribe(mm->bus, mm->sig_owner);
	if (mm->loop)
		g_main_loop_unref(mm->loop);
	if (mm->bus)
		g_object_unref(mm->bus);
	g_main_context_unref(mm->ctx);
	ast_mutex_destroy(&mm->lock);
	ast_free(mm->modem_path);
	ast_free(mm);
}

int e5_mm_online(struct e5_mm *mm)
{
	int v;
	ast_mutex_lock(&mm->lock);
	v = mm->online;
	ast_mutex_unlock(&mm->lock);
	return v;
}

int e5_mm_query(struct e5_mm *mm, char *pathbuf, size_t len)
{
	int v;
	ast_mutex_lock(&mm->lock);
	v = mm->online;
	if (pathbuf && len) {
		if (mm->online && mm->modem_path)
			snprintf(pathbuf, len, "%s", mm->modem_path);
		else
			pathbuf[0] = '\0';
	}
	ast_mutex_unlock(&mm->lock);
	return v;
}

int e5_mm_sms_send(struct e5_mm *mm, const char *number, const char *text)
{
	ast_debug(1, "e5_mm_sms_send: number=%s text='%s'\n", number, text);
	GError *err = NULL;
	GVariantBuilder b;
	GVariant *r = NULL;
	int rc = -1;
	char *modem = NULL;

	ast_mutex_lock(&mm->lock);
	if (!mm->online || !mm->modem_path) {
		ast_log(LOG_WARNING, "e5_mm: 发短信时 modem 不在线\n");
		ast_debug(1, "e5_mm_sms_send: modem 不在线，失败\n");
		goto out;
	}
	modem = ast_strdup(mm->modem_path);

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Number", g_variant_new_string(number));
	g_variant_builder_add(&b, "{sv}", "Text", g_variant_new_string(text));

	if (!call_sync(mm->bus, modem, MM_IF_MESSAGING, "Create",
			g_variant_new("(@a{sv})", g_variant_builder_end(&b)), &r, &err)) {
		ast_log(LOG_WARNING, "e5_mm: Messaging.Create(%s) 失败: %s\n",
			number, err->message);
		g_clear_error(&err);
		goto out;
	}
	const char *sms_path = NULL;
	g_variant_get(r, "(&o)", &sms_path);
	char *path_copy = ast_strdup(sms_path);
	g_variant_unref(r);

	if (!call_sync(mm->bus, path_copy, MM_IF_SMS, "Send", NULL, NULL, &err)) {
		ast_log(LOG_WARNING, "e5_mm: Sms.Send(%s) 失败: %s\n",
			path_copy, err->message);
		ast_debug(1, "e5_mm_sms_send: Sms.Send 失败 %s\n", err->message);
		g_clear_error(&err);
		ast_free(path_copy);
		goto out;
	}
	ast_free(path_copy);
	rc = 0;
	ast_debug(1, "e5_mm_sms_send: 成功\n");
out:
	ast_mutex_unlock(&mm->lock);
	ast_free(modem);
	return rc;
}

int e5_mm_sms_delete(struct e5_mm *mm, const char *sms_path)
{
	ast_debug(1, "e5_mm_sms_delete: %s\n", sms_path);
	GError *err = NULL;
	int rc = 0;
	ast_mutex_lock(&mm->lock);
	if (!mm->online || !mm->modem_path) {
		ast_mutex_unlock(&mm->lock);
		return -1;
	}
	if (!call_sync(mm->bus, mm->modem_path, MM_IF_MESSAGING, "Delete",
			g_variant_new("(o)", sms_path), NULL, &err)) {
		ast_log(LOG_WARNING, "e5_mm: Messaging.Delete(%s) 失败: %s\n",
			sms_path, err->message);
		g_clear_error(&err);
		rc = -1;
	}
	ast_mutex_unlock(&mm->lock);
	return rc;
}

int e5_mm_dial(struct e5_mm *mm, int sim_slot, const char *number, char **out_call_path)
{
	ast_debug(1, "e5_mm_dial: 进入 number=%s sim_slot=%d\n", number, sim_slot);
	GError *err = NULL;
	GVariantBuilder b;
	GVariant *r = NULL;
	int rc = -1;
	char *modem = NULL;

	ast_mutex_lock(&mm->lock);
	if (!mm->online || !mm->modem_path) {
		ast_log(LOG_WARNING, "e5_mm: 拨号时 modem 不在线\n");
		ast_debug(1, "e5_mm_dial: modem 不在线，失败\n");
		goto out;
	}
	modem = ast_strdup(mm->modem_path);
	ast_debug(1, "e5_mm_dial: modem=%s\n", modem);

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Number", g_variant_new_string(number));
	g_variant_builder_add(&b, "{sv}", "SimSlot",
		g_variant_new_uint32(sim_slot > 0 ? sim_slot : 0));

	if (!call_sync(mm->bus, modem, MM_IF_VOICE, "CreateCall",
			g_variant_new("(@a{sv})", g_variant_builder_end(&b)), &r, &err)) {
		ast_log(LOG_WARNING, "e5_mm: CreateCall(%s,slot=%d) 失败: %s\n",
			number, sim_slot, err->message);
		ast_debug(1, "e5_mm_dial: CreateCall 失败 %s\n", err->message);
		g_clear_error(&err);
		goto out;
	}
	const char *call_path = NULL;
	g_variant_get(r, "(&o)", &call_path);
	char *path_copy = ast_strdup(call_path);
	g_variant_unref(r);
	ast_debug(1, "e5_mm_dial: CreateCall OK call_path=%s\n", path_copy);

	if (!call_sync(mm->bus, path_copy, MM_IF_CALL, "Start", NULL, NULL, &err)) {
		ast_log(LOG_WARNING, "e5_mm: Start(%s) 失败: %s（结果未确认，等待状态事件对账）\n",
			path_copy, err->message);
		ast_debug(1, "e5_mm_dial: Start 失败 %s（call_path 仍返回）\n", err->message);
		g_clear_error(&err);
		if (out_call_path)
			*out_call_path = path_copy;
		else
			ast_free(path_copy);
		rc = -1;
		goto out;
	}
	if (out_call_path)
		*out_call_path = path_copy;
	else
		ast_free(path_copy);
	rc = 0;
	ast_debug(1, "e5_mm_dial: 成功 number=%s\n", number);
out:
	ast_mutex_unlock(&mm->lock);
	ast_free(modem);
	return rc;
}

int e5_mm_answer(struct e5_mm *mm, const char *call_path)
{
	ast_debug(1, "e5_mm_answer: %s\n", call_path);
	GError *err = NULL;
	int rc = 0;
	ast_mutex_lock(&mm->lock);
	if (!call_sync(mm->bus, call_path, MM_IF_CALL, "Accept", NULL, NULL, &err)) {
		ast_log(LOG_WARNING, "e5_mm: Accept(%s) 失败: %s\n", call_path, err->message);
		ast_debug(1, "e5_mm_answer: Accept 失败 %s\n", err->message);
		g_clear_error(&err);
		rc = -1;
	}
	ast_mutex_unlock(&mm->lock);
	return rc;
}

int e5_mm_hangup(struct e5_mm *mm, const char *call_path)
{
	ast_debug(1, "e5_mm_hangup: %s\n", call_path);
	GError *err = NULL;
	int rc = 0;
	ast_mutex_lock(&mm->lock);
	if (!call_sync(mm->bus, call_path, MM_IF_CALL, "Hangup", NULL, NULL, &err)) {
		ast_log(LOG_WARNING, "e5_mm: Hangup(%s) 失败: %s\n", call_path, err->message);
		ast_debug(1, "e5_mm_hangup: Hangup 失败 %s\n", err->message);
		g_clear_error(&err);
		rc = -1;
	}
	ast_mutex_unlock(&mm->lock);
	return rc;
}

int e5_mm_send_dtmf(struct e5_mm *mm, const char *call_path, char digit)
{
	ast_debug(1, "e5_mm_send_dtmf: %s digit=%c\n", call_path, digit);
	if (!strchr("0123456789*#", digit))
		return -1;
	GError *err = NULL;
	int rc = 0;
	ast_mutex_lock(&mm->lock);
	if (!call_sync(mm->bus, call_path, MM_IF_CALL, "SendDtmf",
			g_variant_new("(u)", (guint32)digit), NULL, &err)) {
		ast_log(LOG_WARNING, "e5_mm: SendDtmf(%s,%c) 失败: %s\n",
			call_path, digit, err->message);
		g_clear_error(&err);
		rc = -1;
	}
	ast_mutex_unlock(&mm->lock);
	return rc;
}

struct list_ctx {
	void (*cb)(const struct e5_mm_call *, void *);
	void *user;
	struct e5_mm *mm;
};

static void list_one(struct e5_mm *mm, const char *p, struct list_ctx *ctx)
{
	struct e5_mm_call c;
	if (!fetch_call(mm, p, &c)) {
		ctx->cb(&c, ctx->user);
		call_clear(&c);
	}
}

int e5_mm_list_calls(struct e5_mm *mm,
	void (*cb)(const struct e5_mm_call *c, void *user), void *user)
{
	ast_debug(1, "e5_mm_list_calls: 进入\n");
	GError *err = NULL;
	GVariant *r = NULL;
	int rc = -1;

	ast_mutex_lock(&mm->lock);
	if (!mm->online) {
		ast_debug(1, "e5_mm_list_calls: modem 不在线\n");
		goto out;
	}

	if (!call_sync(mm->bus, mm->modem_path, MM_IF_PC, "Get",
			g_variant_new("(ss)", MM_IF_VOICE, "Calls"), &r, &err)) {
		g_clear_error(&err);
		goto out;
	}
	GVariant *v = g_variant_get_child_value(r, 0);
	GVariant *ao = g_variant_get_variant(v);
	struct list_ctx ctx = { .cb = cb, .user = user, .mm = mm };
	/* ao 是对象路径数组，用栈迭代器遍历；^ao 会得到字符串数组指针而非
	 * 迭代器，切勿与 g_variant_iter_loop 混用。 */
	GVariantIter iter;
	const char *p;
	g_variant_iter_init(&iter, ao);
	while (g_variant_iter_loop(&iter, "&o", &p))
		list_one(mm, p, &ctx);
	g_variant_unref(ao);
	g_variant_unref(v);
	g_variant_unref(r);
	rc = 0;
out:
	ast_mutex_unlock(&mm->lock);
	return rc;
}

const char *e5_mm_state_name(int state)
{
	switch (state) {
	case E5_CALL_DIALING:     return "dialing";
	case E5_CALL_RINGING_OUT: return "ringing-out";
	case E5_CALL_RINGING_IN:  return "ringing-in";
	case E5_CALL_ACTIVE:      return "active";
	case E5_CALL_HELD:        return "held";
	case E5_CALL_WAITING:     return "waiting";
	case E5_CALL_TERMINATED:  return "terminated";
	default:                  return "unknown";
	}
}
