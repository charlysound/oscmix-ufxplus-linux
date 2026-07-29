//
//  regtool_generic.c
//  oscmix
//
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../arg.h"
#include "../sysex.h"

#ifdef __linux__
#include <alsa/asoundlib.h>
#elif defined(__APPLE__)
#include "../fatal.h"
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#endif



enum {
	FIREFACE = 0,
	BABYFACE = 1,
};

#ifdef __linux__
static snd_seq_t *seq;
#elif defined(__APPLE__)
static MIDIClientRef client = 0;
static MIDIPortRef inPort = 0;
static MIDIPortRef outPort = 0;
static MIDIEndpointRef destination = 0;

struct sysex_queue {
	unsigned char *data;
	size_t len;
	struct sysex_queue *next;
};
static struct sysex_queue *sysex_queue_head = NULL;
static struct sysex_queue **sysex_queue_tail = &sysex_queue_head;
static pthread_mutex_t sysex_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static int sflag;
static int wflag;
static int tflag;
static int lflag;

static void
usage(void)
{
	fprintf(stderr,
			"usage: regtool_generic [-s] client:port (on linux), port index (on macOS) \n"
			"       regtool_generic [-s] -w client:port (on linux), port index (on macOS) [reg val]...\n"
			"       regtool_generic -l (lists ports, only on macOS)\n"
			"       regtool_generic [-t] fireface or babyface\n"
			);
	exit(1);
}

#ifdef __APPLE__
static void
list_devices(void)
{
	ItemCount numDevices, i;

	printf("Input devices:\n");
	numDevices = MIDIGetNumberOfSources();
	for (i = 0; i < numDevices; i++) {
		MIDIEndpointRef source = MIDIGetSource(i);
		CFStringRef name = NULL;
		MIDIObjectGetStringProperty(source, kMIDIPropertyName, &name);
		if (name) {
			char buf[256];
			CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
			printf("  %ld: %s\n", i, buf);
			CFRelease(name);
		} else {
			printf("  %ld: (unnamed)\n", i);
		}
	}

	printf("\nOutput devices:\n");
	numDevices = MIDIGetNumberOfDestinations();
	for (i = 0; i < numDevices; i++) {
		MIDIEndpointRef dest = MIDIGetDestination(i);
		CFStringRef name = NULL;
		MIDIObjectGetStringProperty(dest, kMIDIPropertyName, &name);
		if (name) {
			char buf[256];
			CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
			printf("  %ld: %s\n", i, buf);
			CFRelease(name);
		} else {
			printf("  %ld: (unnamed)\n", i);
		}
	}
}

static void
epname(MIDIObjectRef obj, char *buf, size_t len)
{
	CFStringRef name;
	CFIndex used;
	CFRange range;
	OSStatus err;

	err = MIDIObjectGetStringProperty(obj, kMIDIPropertyDisplayName, &name);
	if (err)
		fatal("MIDIObjectGetStringProperty: %d", err);
	range = CFRangeMake(0, CFStringGetLength(name));
	CFStringGetBytes(name, range, kCFStringEncodingUTF8, 0, false, (uint8_t *)buf, len - 1, &used);
	CFRelease(name);
	buf[used] = '\0';
}

static void
listports(void)
{
	ItemCount i, n;
	MIDIEndpointRef ep;
	char name[256];

	printf("Sources:\n");
	n = MIDIGetNumberOfSources();
	for (i = 0; i < n; ++i) {
		ep = MIDIGetSource(i);
		epname(ep, name, sizeof name);
		printf("%d\t%s\n", (int)i, name);
	}

	printf("\nDestinations:\n");
	n = MIDIGetNumberOfDestinations();
	for (i = 0; i < n; ++i) {
		ep = MIDIGetDestination(i);
		epname(ep, name, sizeof name);
		printf("%d\t%s\n", (int)i, name);
	}
}

#endif

static void
dumpsysex(const char *prefix, const unsigned char *buf, size_t len)
{
	static const unsigned char hdr[] = {0xf0, 0x00, 0x20, 0x0d, 0x10};
	const unsigned char *pos, *end;
	unsigned long regval;
	unsigned reg, val, par;

	pos = buf;
	end = pos + len;
	if (sflag) {
		fputs(prefix, stdout);
		for (; pos != end; ++pos)
			printf(" %.2X", *pos);
		fputc('\n', stdout);
	}
	pos = buf;
	--end;
	if (len < sizeof hdr || memcmp(pos, hdr, sizeof hdr) != 0 || (len - sizeof hdr - 2) % 5 != 0) {
		printf("skipping unexpected sysex\n");
		return;
	}
	if (pos[5] != 0 || tflag == BABYFACE) {
		printf("subid=%d", pos[5]);
		for (pos += sizeof hdr + 1; pos != end; pos += 5) {
			if (pos[4] & 0xf0) {
				printf("\tbad encoding\n");
				return;
			}
			regval = getle32_7bit(pos);
			printf("%c%.8lX", pos == buf + sizeof hdr + 1 ? '\t' : ' ', regval);
		}
		fputc('\n', stdout);
		return;
	}
	for (pos += sizeof hdr + 1; pos != end; pos += 5) {
		regval = getle32_7bit(pos);
		reg = regval >> 16 & 0x7fff;
		val = regval & 0xffff;
		par = regval ^ regval >> 16 ^ 1;
		par ^= par >> 8;
		par ^= par >> 4;
		par ^= par >> 2;
		par ^= par >> 1;
		printf("%.4X\t%.4X", reg, val);
		if (par & 1)
			printf("\tbad parity");
		fputc('\n', stdout);
	}
	fflush(stdout);
}

#ifdef __APPLE__
static void midiReadCallback(const MIDIPacketList *list, void *readProcRefCon, void *srcConnRefCon) {
	const MIDIPacket *packet = &list->packet[0];
	for (UInt32 i = 0; i < list->numPackets; i++) {
		if (packet->length >= 1 && packet->data[0] == 0xF0) {
			unsigned char *data = malloc(packet->length);
			if (!data) {
				fprintf(stderr, "Out of memory in MIDI callback\n");
				return;
			}
			memcpy(data, packet->data, packet->length);
			struct sysex_queue *item = malloc(sizeof(struct sysex_queue));
			if (!item) {
				free(data);
				fprintf(stderr, "Out of memory in MIDI callback\n");
				return;
			}
			item->data = data;
			item->len = packet->length;
			item->next = NULL;

			pthread_mutex_lock(&sysex_queue_mutex);
			*sysex_queue_tail = item;
			sysex_queue_tail = &item->next;
			pthread_mutex_unlock(&sysex_queue_mutex);
		}
		packet = MIDIPacketNext(packet);
	}
}
#endif

static void
midiread(void)
{
#ifdef __linux__
	int ret;
	size_t len;
	snd_seq_event_t *evt;
	const unsigned char *evtbuf;
	size_t evtlen;
	unsigned char buf[8192];

	len = 0;
	for (;;) {
		ret = snd_seq_event_input(seq, &evt);
		if (ret < 0) {
			fprintf(stderr, "snd_seq_event_input: %s\n", snd_strerror(ret));
			if (ret == -ENOSPC) {
				fprintf(stderr, "buffer overrun: some events were dropped\n");
				continue;
			}
			exit(1);
		}
		if (evt->type != SND_SEQ_EVENT_SYSEX || evt->data.ext.len == 0)
			continue;
		evtbuf = evt->data.ext.ptr;
		evtlen = evt->data.ext.len;
		if (evtbuf[0] == 0xf0) {
			if (len > 0) {
				fprintf(stderr, "dropping incomplete sysex\n");
				len = 0;
			}
		}
		if (evtlen > sizeof buf - len) {
			fprintf(stderr, "dropping sysex that is too long\n");
			len = evtbuf[evtlen - 1] == 0xf7 ? 0 : sizeof buf;
			continue;
		}
		memcpy(buf + len, evtbuf, evtlen);
		len += evtlen;
		if (buf[len - 1] == 0xf7) {
			dumpsysex("<-", buf, len);
			len = 0;
		}
	}
#elif defined(__APPLE__)
	while (1) {
		pthread_mutex_lock(&sysex_queue_mutex);
		if (sysex_queue_head) {
			struct sysex_queue *item = sysex_queue_head;
			sysex_queue_head = item->next;
			if (sysex_queue_head == NULL) {
				sysex_queue_tail = &sysex_queue_head;
			}
			pthread_mutex_unlock(&sysex_queue_mutex);

			dumpsysex("<-", item->data, item->len);
			free(item->data);
			free(item);
		} else {
			pthread_mutex_unlock(&sysex_queue_mutex);
			usleep(10000);
		}
	}
#endif
}

static void
setreg(unsigned reg, unsigned val)
{
	unsigned char buf[12] = {0xf0, 0x00, 0x20, 0x0d, 0x10, 0x00, [sizeof buf - 1]=0xf7};
	unsigned par;
	unsigned long regval;

	reg &= 0x7fff;
	val &= 0xffff;
	par = reg ^ val ^ 1;
	par ^= par >> 8;
	par ^= par >> 4;
	par ^= par >> 2;
	par ^= par >> 1;
	regval = par << 31 | reg << 16 | val;
	putle32_7bit(buf + 6, regval);

	dumpsysex("->", buf, sizeof buf);

#ifdef __linux__
	snd_seq_event_t evt;
	int err;
	snd_seq_ev_clear(&evt);
	snd_seq_ev_set_source(&evt, 0);
	snd_seq_ev_set_subs(&evt);
	snd_seq_ev_set_direct(&evt);
	snd_seq_ev_set_sysex(&evt, sizeof buf, buf);
	err = snd_seq_event_output_direct(seq, &evt);
	if (err < 0)
		fprintf(stderr, "snd_seq_event_output: %s\n", snd_strerror(err));
#elif defined(__APPLE__)
	if (outPort && destination) {
		MIDIPacketList packetList;
		MIDIPacket *packet = MIDIPacketListInit(&packetList);
		packet = MIDIPacketListAdd(&packetList, sizeof(packetList), packet, 0, sizeof(buf), buf);
		OSStatus status = MIDISend(outPort, destination, &packetList);
		if (status != noErr) {
			fprintf(stderr, "Failed to send MIDI message: %d\n", (int)status);
		}
	} else {
		fprintf(stderr, "Output port or destination not set\n");
	}
#endif
}

static void
midiwrite(void)
{
	unsigned reg, val;
	char str[256];

	while (fgets(str, sizeof str, stdin)) {
		if (sscanf(str, "%x %x", &reg, &val) != 2) {
			fprintf(stderr, "invalid input\n");
			continue;
		}
		setreg(reg, val);
	}
}

int
main(int argc, char *argv[])
{
	int err, flags;
	char *arg, *end;

#ifdef __linux__
	snd_seq_addr_t dest, self;
	snd_seq_port_subscribe_t *sub;
#elif defined(__APPLE__)
	MIDIEndpointRef source = 0;
	long index;
	ItemCount numDevices;
#endif

	ARGBEGIN {
	case 's':
		sflag = 1;
		break;
	case 'w':
		wflag = 1;
		break;
	case 't':
		arg = EARGF(usage());
		if (strcmp(arg, "fireface") == 0) {
			tflag = FIREFACE;
		} else if (strcmp(arg, "babyface") == 0) {
			tflag = BABYFACE;
		} else {
			fprintf(stderr, "unknown device '%s'\n", arg);
			return 1;
		}
		break;
	case 'l':
		lflag = 1;
		break;
	default:
		usage();
	} ARGEND
	if (lflag) {
#ifdef __APPLE__
		//list_devices();
		listports();
		return 0;
#else
		fprintf(stderr, "Device listing only supported on macOS\n");
		return 1;
#endif
	}
	if (argc < 1 || (!wflag && argc != 1) || argc % 2 != 1)
		usage();

#ifdef __linux__
	dest.client = strtol(argv[0], &end, 10);
	if (*end != ':')
		usage();
	dest.port = strtol(end + 1, &end, 10);
	if (*end)
		usage();

	err = snd_seq_open(&seq, "default", wflag ? SND_SEQ_OPEN_OUTPUT : SND_SEQ_OPEN_INPUT, 0);
	if (err) {
		fprintf(stderr, "snd_seq_open: %s\n", snd_strerror(err));
		return 1;
	}
	err = snd_seq_set_client_name(seq, "regtool");
	if (err) {
		fprintf(stderr, "snd_seq_set_client_name: %s\n", snd_strerror(err));
		return 1;
	}
	if (wflag)
		flags = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
	else
		flags = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
	err = snd_seq_create_simple_port(seq, "regtool", flags, SND_SEQ_PORT_TYPE_MIDI_GENERIC);
	if (err) {
		fprintf(stderr, "snd_seq_create_simple_port: %s\n", snd_strerror(err));
		return 1;
	}

	err = snd_seq_port_subscribe_malloc(&sub);
	if (err) {
		fprintf(stderr, "snd_seq_port_subscribe_malloc: %s\n", snd_strerror(err));
		return 1;
	}
	self.client = snd_seq_client_id(seq);
	self.port = 0;
	snd_seq_port_subscribe_set_sender(sub, wflag ? &self : &dest);
	snd_seq_port_subscribe_set_dest(sub, wflag ? &dest : &self);
	err = snd_seq_subscribe_port(seq, sub);
	if (err) {
		fprintf(stderr, "snd_seq_subscribe_port: %s\n", snd_strerror(err));
		return 1;
	}
#elif defined(__APPLE__)
	OSStatus status;
	status = MIDIClientCreate(CFSTR("regtool"), NULL, NULL, &client);
	if (status != noErr) {
		fprintf(stderr, "Failed to create MIDI client: %d\n", (int)status);
		return 1;
	}

	index = strtol(argv[0], &end, 10);
	if (*end != '\0') {
		fprintf(stderr, "Invalid device index: %s\n", argv[0]);
		return 1;
	}

	if (wflag) {
		status = MIDIOutputPortCreate(client, CFSTR("regtool out"), &outPort);
		if (status != noErr) {
			fprintf(stderr, "Failed to create output port: %d\n", (int)status);
			return 1;
		}
		numDevices = MIDIGetNumberOfDestinations();
		if (index < 0 || index >= numDevices) {
			fprintf(stderr, "Invalid destination index: %ld\n", index);
			return 1;
		}
		destination = MIDIGetDestination(index);
	} else {
		status = MIDIInputPortCreate(client, CFSTR("regtool in"), midiReadCallback, NULL, &inPort);
		if (status != noErr) {
			fprintf(stderr, "Failed to create input port: %d\n", (int)status);
			return 1;
		}
		numDevices = MIDIGetNumberOfSources();
		if (index < 0 || index >= numDevices) {
			fprintf(stderr, "Invalid source index: %ld\n", index);
			return 1;
		}
		source = MIDIGetSource(index);
		MIDIPortConnectSource(inPort, source, NULL);
	}
#endif

	if (wflag) {
		if (argc > 1) {
			int i;
			long reg, val;

			for (i = 1; i < argc; i += 2) {
				reg = strtol(argv[i], &end, 16);
				if (*end || reg < 0 || reg > 0x7fff)
					usage();
				val = strtol(argv[i + 1], &end, 16);
				if (*end || val < -0x8000 || val > 0xffff)
					usage();
				setreg(reg, val);
			}
		} else {
			midiwrite();
		}
	} else {
		midiread();
	}

	return 0;
}
