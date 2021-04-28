#include "slack-api.h"
#include "slack-channel.h"
#include "slack-conversation.h"
#include "slack-im.h"
#include "slack-json.h"
#include "slack-message.h"
#include "slack-thread.h"
#include "slack-user.h"

#include <debug.h>

#include <errno.h>

enum ThreadOp {
	ThreadOpPost,
	ThreadOpGetReplies,
};

struct thread_op {
	SlackObject *conv;
	enum ThreadOp op;
	char *msg;
};

static gboolean slack_is_slack_ts(const char *str) {
	gboolean found_prefix = FALSE;
	gboolean found_dot = FALSE;
	gboolean found_suffix = FALSE;
	for (int i = 0; str[i] != '\0'; i++) {
		if (str[i] >= '0' && str[i] <= '9') {
			if (found_dot)
				found_suffix = TRUE;
			else
				found_prefix = TRUE;
			continue;
		}
		if (str[i] == '.') {
			if (found_dot)
				return FALSE;
			else
				found_dot = TRUE;
		}
	}
	return found_prefix && found_dot && found_suffix;
}

static time_t slack_get_ts_from_time_str(const char *time_str) {
	struct tm tm;
	char *result;

	time_t ts = time(NULL);
	localtime_r(&ts, &tm);

	// Time only.
	result = strptime(time_str, "%X", &tm);
	if (result == time_str + strlen(time_str))
		return mktime(&tm);

	// Date and time.
	result = strptime(time_str, "%x-%X", &tm);
	if (result == time_str + strlen(time_str))
		return mktime(&tm);

	return 0;
}

static struct thread_op *thread_op_new_post(SlackObject *conv, const char *msg) {
	struct thread_op *ret = g_new(struct thread_op, 1);
	ret->conv = g_object_ref(conv);
	ret->op = ThreadOpPost;
	ret->msg = g_strdup(msg);
	return ret;
}

static struct thread_op *thread_op_new_get_replies(SlackObject *conv) {
	struct thread_op *ret = g_new(struct thread_op, 1);
	ret->conv = g_object_ref(conv);
	ret->op = ThreadOpGetReplies;
	ret->msg = NULL;
	return ret;
}

static void thread_op_free(struct thread_op *op) {
	if (!op)
		return;

	g_object_unref(op->conv);
	g_free(op->msg);
	g_free(op);
}

static int slack_thread_send_message(SlackAccount *sa, SlackObject *conv, const char *msg, PurpleMessageFlags flags) {
	if (SLACK_IS_CHANNEL(conv)) {
		SlackChannel *chan = (SlackChannel *)conv;
		slack_chat_send(sa->gc, chan->cid, msg, flags);
	} else if (SLACK_IS_USER(conv)) {
		SlackUser *user = (SlackUser *)conv;
		slack_send_im(sa->gc, user->object.name, msg, flags);
	} else
		return -ENOENT;

	return 1;
}

static void slack_thread_post(SlackAccount *sa, SlackObject *conv, const char *ts, const char *msg) {
	if (!msg)
		return;

	// Temporarily set thread_ts for this one message, and then restore it
	// afterwards.
	// XXX: this won't always work because slack_send_im may be asynchronous
	char *old_thread_ts = conv->thread_ts;
	conv->thread_ts = g_strdup(ts);

	int status = slack_thread_send_message(sa, conv, msg, 0);
	if (status < 0)
		purple_debug_error("slack", "Not able to send message \"%s\": %s\n", msg, strerror(-status));

	g_free(conv->thread_ts);
	conv->thread_ts = old_thread_ts;
}

static gboolean slack_thread_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct thread_op *op = data;
	if (!op)
		return FALSE;

	SlackObject *conv = op->conv;

	json_value *list = json_get_prop_type(json, "messages", array);

	if (!list || error) {
		purple_debug_error("slack", "Error querying threads: %s\n", error ?: "missing");
	}

	if (list->u.array.length <= 0) {
		slack_write_message(sa, conv, "Thread not found. If the thread start date is not today, make sure you specify the date in the thread timestamp.", PURPLE_MESSAGE_SYSTEM);
		goto end;
	} else if (list->u.array.length > 1) {
		GString *errmsg = g_string_new(NULL);
		g_string_assign(errmsg, "Thread timestamp is ambiguous. Please use one of the following unambiguous thread IDs:\n");
		for (unsigned i = 0; i < list->u.array.length; i++) {
			json_value *entry = list->u.array.values[i];
			const char *ts = json_get_prop_strptr(entry, "ts");
			if (ts) {
				const char *msg = json_get_prop_strptr(entry, "text");
				GString *color = slack_get_thread_color(ts);
				g_string_append(errmsg, "<font color=\"#");
				g_string_append(errmsg, color->str);
				g_string_append(errmsg, "\">");
				g_string_append(errmsg, ts);
				g_string_append(errmsg, "</font> (\"");
				g_string_append(errmsg, msg ?: "NULL");
				g_string_append(errmsg, "\")\n");

				g_string_free(color, TRUE);
			}
		}
		slack_write_message(sa, conv, errmsg->str, PURPLE_MESSAGE_SYSTEM);
		g_string_free(errmsg, TRUE);
		goto end;
	}

	json_value *entry = list->u.array.values[0];
	const char *ts = json_get_prop_strptr(entry, "ts");
	if (!ts) {
		purple_debug_misc("slack", "Missing ts value in thread callback\n");
		goto end;
	}

	switch (op->op) {
	case ThreadOpPost:
		slack_thread_post(sa, conv, ts, op->msg);
		break;
	case ThreadOpGetReplies:
		slack_get_thread_replies(sa, conv, ts);
		break;
	default:
		break;
	}

end:
	thread_op_free(op);
	return FALSE;
}

static void slack_thread_call_operation(SlackAccount *sa, SlackObject *obj, struct thread_op *op, time_t ts) {
	GString *oldest = g_string_new(NULL);
	g_string_printf(oldest, "%ld.000000", ts);
	GString *latest = g_string_new(NULL);
	g_string_printf(latest, "%ld.999999", ts);

	const char *id = slack_conversation_id(obj);
	slack_api_get(sa, slack_thread_cb, op, "conversations.history", "channel", id, "oldest", oldest->str, "latest", latest->str, NULL);

	g_string_free(oldest, TRUE);
	g_string_free(latest, TRUE);
}

void slack_thread_post_to_timestamp(SlackAccount *sa, SlackObject *obj, const char *timestr, const char *msg) {
	if (slack_is_slack_ts(timestr))
		slack_thread_post(sa, obj, timestr, msg);
	else {
		time_t ts = slack_get_ts_from_time_str(timestr);
		if (ts == 0) {
			slack_write_message(sa, obj, "Could not parse thread timestamp.", PURPLE_MESSAGE_SYSTEM);
			return;
		}

		struct thread_op *op = thread_op_new_post(obj, msg);
		slack_thread_call_operation(sa, obj, op, ts);
	}
}

void slack_thread_get_replies(SlackAccount *sa, SlackObject *obj, const char *timestr) {
	if (slack_is_slack_ts(timestr))
		slack_get_thread_replies(sa, obj, timestr);
	else {
		time_t ts = slack_get_ts_from_time_str(timestr);
		if (ts == 0) {
			slack_write_message(sa, obj, "Could not parse thread timestamp.", PURPLE_MESSAGE_SYSTEM);
			return;
		}

		struct thread_op *op = thread_op_new_get_replies(obj);
		slack_thread_call_operation(sa, obj, op, ts);
	}
}
