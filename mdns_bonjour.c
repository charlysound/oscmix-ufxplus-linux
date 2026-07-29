#include "mdns.h"
#include <dns_sd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DNSServiceRef  s_service = NULL;
static pthread_t      s_thread;
static volatile int   s_running = 0;

/* Runs in a background thread, pumping Bonjour events. */
static void *
service_thread(void *arg)
{
	(void)arg;
	while (s_running) {
		DNSServiceErrorType err = DNSServiceProcessResult(s_service);
		if (err != kDNSServiceErr_NoError) {
			fprintf(stderr, "mdns: DNSServiceProcessResult error: %d\n", err);
			break;
		}
	}
	return NULL;
}

static void
register_callback(DNSServiceRef service, DNSServiceFlags flags,
                  DNSServiceErrorType err, const char *name,
                  const char *type, const char *domain, void *ctx)
{
	(void)service; (void)flags; (void)ctx;
	if (err != kDNSServiceErr_NoError)
		fprintf(stderr, "mdns: registration error: %d\n", err);
	else
		fprintf(stderr, "mdns: registered as '%s.%s%s'\n", name, type, domain);
}

int
mdns_register(const char *name, uint16_t port, const char *txt[])
{
	TXTRecordRef txtrec;
	DNSServiceErrorType err;
	int ret = 0;

	TXTRecordCreate(&txtrec, 0, NULL);

	/* Populate TXT record from NULL-terminated "key=value" array. */
	for (int i = 0; txt[i]; i++) {
		const char *kv  = txt[i];
		const char *eq  = strchr(kv, '=');
		char        key[64];
		size_t      keylen;

		if (!eq)
			continue;
		keylen = (size_t)(eq - kv);
		if (keylen == 0 || keylen >= sizeof key)
			continue;
		memcpy(key, kv, keylen);
		key[keylen] = '\0';

		const char *val = eq + 1;
		TXTRecordSetValue(&txtrec, key, (uint8_t)strlen(val), val);
	}

	err = DNSServiceRegister(
		&s_service,
		0, 0,
		name, "_osc._udp",
		NULL, NULL,
		htons(port),
		TXTRecordGetLength(&txtrec),
		TXTRecordGetBytesPtr(&txtrec),
		register_callback, NULL);

	TXTRecordDeallocate(&txtrec);

	if (err != kDNSServiceErr_NoError) {
		fprintf(stderr, "mdns: DNSServiceRegister failed: %d\n", err);
		return -1;
	}

	s_running = 1;
	if (pthread_create(&s_thread, NULL, service_thread, NULL) != 0) {
		perror("mdns: pthread_create");
		DNSServiceRefDeallocate(s_service);
		s_service = NULL;
		s_running = 0;
		ret = -1;
	} else {
		fprintf(stderr, "mdns: registering '%s' on port %u\n", name, port);
	}
	return ret;
}

void
mdns_unregister(void)
{
	if (!s_service)
		return;

	s_running = 0;
	DNSServiceRefDeallocate(s_service);
	s_service = NULL;
	pthread_join(s_thread, NULL);
}
