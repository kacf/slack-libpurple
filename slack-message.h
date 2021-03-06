#ifndef _PURPLE_SLACK_MESSAGE_H
#define _PURPLE_SLACK_MESSAGE_H

#include "json.h"
#include "slack.h"
#include "slack-object.h"

gchar *slack_html_to_message(SlackAccount *sa, const char *s, PurpleMessageFlags flags);
void slack_message_to_html(GString *html, SlackAccount *sa, gchar *s, PurpleMessageFlags *flags, gchar *prepend_newline_str);
void slack_json_to_html(GString *html, SlackAccount *sa, json_value *json, PurpleMessageFlags *flags);
void slack_get_history(SlackAccount *sa, SlackObject *obj, const char *since, unsigned count);

/* RTM event handlers */
void slack_message(SlackAccount *sa, json_value *json);
void slack_user_typing(SlackAccount *sa, json_value *json);

/* Purple protocol handlers */
unsigned int slack_send_typing(PurpleConnection *gc, const char *who, PurpleTypingState state);

#endif // _PURPLE_SLACK_MESSAGE_H
