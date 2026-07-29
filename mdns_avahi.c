#define _POSIX_C_SOURCE 200809L
#include "mdns.h"
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/thread-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AvahiThreadedPoll  *s_poll  = NULL;
static AvahiClient        *s_client = NULL;
static AvahiEntryGroup    *s_group  = NULL;
static char               *s_name  = NULL;
static uint16_t            s_port  = 0;
static const char        **s_txt   = NULL;

/* Forward declaration. */
static void create_service(AvahiClient *c);

static void
entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata)
{
	(void)userdata;
	switch (state) {
		case AVAHI_ENTRY_GROUP_COLLISION: {
			/* Rename on conflict - Avahi appends " #2", " #3" etc. */
			char *alt = avahi_alternative_service_name(s_name);
			free(s_name);
			s_name = alt;
			create_service(avahi_entry_group_get_client(g));
			break;
		}
		case AVAHI_ENTRY_GROUP_FAILURE:
			fprintf(stderr, "mdns: entry group failure: %s\n",
					avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
			break;
		default:
			break;
	}
}

static void
create_service(AvahiClient *c)
{
	AvahiStringList *list = NULL;
	int ret;

	if (!s_group) {
		s_group = avahi_entry_group_new(c, entry_group_callback, NULL);
		if (!s_group) {
			fprintf(stderr, "mdns: avahi_entry_group_new failed: %s\n",
				avahi_strerror(avahi_client_errno(c)));
			return;
		}
	}

	if (avahi_entry_group_is_empty(s_group)) {
		/* Build TXT record list from NULL-terminated array. */
		for (int i = 0; s_txt[i]; i++)
			list = avahi_string_list_add(list, s_txt[i]);

		ret = avahi_entry_group_add_service_strlst(
			s_group,
			AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
			0,
			s_name, "_osc._udp",
			NULL, NULL,
			s_port,
			list);
		avahi_string_list_free(list);

		if (ret < 0) {
			if (ret == AVAHI_ERR_COLLISION) {
				char *alt = avahi_alternative_service_name(s_name);
				free(s_name);
				s_name = alt;
				create_service(c);
			} else {
				fprintf(stderr, "mdns: add service failed: %s\n",
					avahi_strerror(ret));
			}
			return;
		}

		if ((ret = avahi_entry_group_commit(s_group)) < 0) {
			fprintf(stderr, "mdns: entry group commit failed: %s\n",
				avahi_strerror(ret));
		}
	}
}

static void
client_callback(AvahiClient *c, AvahiClientState state, void *userdata)
{
	(void)userdata;
	switch (state) {
		case AVAHI_CLIENT_S_RUNNING:
			create_service(c);
			break;
		case AVAHI_CLIENT_FAILURE:
			fprintf(stderr, "mdns: avahi client failure: %s\n",
					avahi_strerror(avahi_client_errno(c)));
			break;
		case AVAHI_CLIENT_S_COLLISION:
		case AVAHI_CLIENT_S_REGISTERING:
			if (s_group)
				avahi_entry_group_reset(s_group);
			break;
		default:
			break;
	}
}

int
mdns_register(const char *name, uint16_t port, const char *txt[])
{
	int err;

	s_name = strdup(name);
	if (!s_name) {
		perror("mdns: strdup");
		return -1;
	}
	s_port = port;
	s_txt  = txt;

	s_poll = avahi_threaded_poll_new();
	if (!s_poll) {
		fprintf(stderr, "mdns: avahi_threaded_poll_new failed\n");
		free(s_name);
		s_name = NULL;
		return -1;
	}

	s_client = avahi_client_new(
		avahi_threaded_poll_get(s_poll),
		0, client_callback, NULL, &err);
	if (!s_client) {
		fprintf(stderr, "mdns: avahi_client_new failed: %s\n",
			avahi_strerror(err));
		avahi_threaded_poll_free(s_poll);
		s_poll = NULL;
		free(s_name);
		s_name = NULL;
		return -1;
	}

	if (avahi_threaded_poll_start(s_poll) < 0) {
		fprintf(stderr, "mdns: avahi_threaded_poll_start failed\n");
		avahi_client_free(s_client);
		s_client = NULL;
		avahi_threaded_poll_free(s_poll);
		s_poll = NULL;
		free(s_name);
		s_name = NULL;
		return -1;
	}

	fprintf(stderr, "mdns: registering '%s' on port %u\n", name, port);
	return 0;
}

void
mdns_unregister(void)
{
	if (s_poll)
		avahi_threaded_poll_stop(s_poll);

	if (s_group) {
		avahi_entry_group_reset(s_group);
		s_group = NULL;
	}
	if (s_client) {
		avahi_client_free(s_client);
		s_client = NULL;
	}
	if (s_poll) {
		avahi_threaded_poll_free(s_poll);
		s_poll = NULL;
	}
	free(s_name);
	s_name = NULL;
}
