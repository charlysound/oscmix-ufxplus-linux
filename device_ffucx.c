#include <assert.h>
#include <stddef.h>
#include "device.h"
#include "intpack.h"

#define LEN(a) (sizeof (a) / sizeof *(a))

static const char *const reflevel_input[] = {"-10dBV", "+4dBu", "Lo Gain"};
static const char *const reflevel_output[] = {"-10dBV", "+4dBu", "Hi Gain"};

static const struct channelinfo inputs[] = {

	{"Mic 1", INPUT_HAS_GAIN | INPUT_HAS_48V | INPUT_HAS_AUTOSET,
			.gain = {0, 65},
	},
	{"Mic 2", INPUT_HAS_GAIN | INPUT_HAS_48V | INPUT_HAS_AUTOSET,
			.gain = {0, 65},
	},
	{"AN 3",  INPUT_HAS_GAIN | INPUT_HAS_REFLEVEL | INPUT_HAS_HIZ | INPUT_HAS_AUTOSET,
			.gain = {0, 12},
			.reflevel = {reflevel_input, LEN(reflevel_input)},
	},
	{"AN 4",  INPUT_HAS_GAIN | INPUT_HAS_REFLEVEL | INPUT_HAS_HIZ | INPUT_HAS_AUTOSET,
			.gain = {0, 12},
			.reflevel = {reflevel_input, LEN(reflevel_input)},
	},
	{"AN 5",     INPUT_HAS_REFLEVEL,
			.reflevel = {reflevel_input, LEN(reflevel_input)},
	},
	{"AN 6",     INPUT_HAS_REFLEVEL,
			.reflevel = {reflevel_input, LEN(reflevel_input)},
	},
	{"AN 7",     INPUT_HAS_REFLEVEL,
			.reflevel = {reflevel_input, LEN(reflevel_input)},
	},
	{"AN 8",     INPUT_HAS_REFLEVEL,
			.reflevel = {reflevel_input, LEN(reflevel_input)},
	},
	{"SPDIF L"},
	{"SPDIF R"},
	{"AS 1"},
	{"AS 2"},
	{"ADAT 3"},
	{"ADAT 4"},
	{"ADAT 5"},
	{"ADAT 6"},
	{"ADAT 7"},
	{"ADAT 8"},
};
_Static_assert(LEN(inputs) == 18, "bad inputs");

static const struct channelinfo outputs[] = {
	{"AN 1",     OUTPUT_HAS_REFLEVEL, .reflevel = {reflevel_output, LEN(reflevel_output)}},
	{"AN 2",     OUTPUT_HAS_REFLEVEL, .reflevel = {reflevel_output, LEN(reflevel_output)}},
	{"AN 3",     OUTPUT_HAS_REFLEVEL, .reflevel = {reflevel_output, LEN(reflevel_output)}},
	{"AN 4",     OUTPUT_HAS_REFLEVEL, .reflevel = {reflevel_output, LEN(reflevel_output)}},
	{"AN 5",     OUTPUT_HAS_REFLEVEL, .reflevel = {reflevel_output, LEN(reflevel_output)}},
	{"AN 6",     OUTPUT_HAS_REFLEVEL, .reflevel = {reflevel_output, LEN(reflevel_output)}},
	{"PH 7",     OUTPUT_HAS_REFLEVEL, .reflevel = {reflevel_output, LEN(reflevel_output)}},
	{"PH 8",     OUTPUT_HAS_REFLEVEL, .reflevel = {reflevel_output, LEN(reflevel_output)}},
	{"SPDIF L"},
	{"SPDIF R"},
	{"AS 1"},
	{"AS 2"},
	{"ADAT 3"},
	{"ADAT 4"},
	{"ADAT 5"},
	{"ADAT 6"},
	{"ADAT 7"},
	{"ADAT 8"},
};
_Static_assert(LEN(outputs) == 18, "bad outputs");

static enum control
regtoctl(int reg, struct param *p)
{
	int idx, flags = 0;

	if (reg < 0)
		return -1;
	/* Input channels (0x0000-0x11FF) */
	if (reg < 0x1200) {
		idx = reg >> 8;
		reg &= 0xFF;
		if (idx >= LEN(inputs))
			return -1;
		p->in = idx;
		p->out = -1;
		flags = inputs[idx].flags;
	}
	/* Output channels (0x1200-0x23FF) */
	else if (reg < 0x2400) {
		idx = (reg - 0x1200) >> 8;
		reg &= 0xFF;
		if (idx >= LEN(outputs))
			return -1;
		p->out = idx;

		// TODO: We need the return MIX somehow. This might break correct input mapping - so comment the if-expression out if it doesnt work:
		if (reg >= 0xE0) {
			p->in = reg - 0xE0;
			if ((unsigned)p->in >= LEN(inputs))
				return -1;
			return MIX;
		}


		p->in = -1;
		flags = outputs[idx].flags;
		reg += 0x1200;
	}

	else {
		p->in = -1;
		p->out = -1;
	}

	switch (reg) {

		case 0x0000: return INPUT_MUTE;
		case 0x0001: return INPUT_FXSEND;
		case 0x0002: return INPUT_STEREO;
		case 0x0005: return INPUT_MSPROC;
		case 0x0006: return INPUT_PHASE;
		case 0x0007: return INPUT_GAIN;
		case 0x0008: return (flags & INPUT_HAS_48V) ? INPUT_48V : (flags & INPUT_HAS_REFLEVEL) ? INPUT_REFLEVEL : -1;
		case 0x0009: return (flags & INPUT_HAS_HIZ) ? INPUT_HIZ : -1; 
		case 0x000A: return INPUT_AUTOSET;

		case 0x1200: return OUTPUT_VOLUME;
		case 0x1201: return OUTPUT_PAN;
		case 0x1202: return OUTPUT_MUTE;
		case 0x1203: return OUTPUT_FXRETURN;
		case 0x1204: return OUTPUT_STEREO;
		case 0x1207: return OUTPUT_PHASE;
		case 0x1208: return OUTPUT_REFLEVEL;

		case 0x0020: return LOWCUT;
		case 0x0021: return LOWCUT_FREQ;
		case 0x0022: return LOWCUT_SLOPE;
		case 0x0040: return EQ;
		case 0x0041: return EQ_BAND1TYPE;
		case 0x0042: return EQ_BAND1GAIN;
		case 0x0043: return EQ_BAND1FREQ;
		case 0x0044: return EQ_BAND1Q;
		case 0x0045: return EQ_BAND2GAIN;
		case 0x0046: return EQ_BAND2FREQ;
		case 0x0047: return EQ_BAND2Q;
		case 0x0048: return EQ_BAND3TYPE;
		case 0x0049: return EQ_BAND3GAIN;
		case 0x004A: return EQ_BAND3FREQ;
		case 0x004B: return EQ_BAND3Q;
		case 0x0060: return DYNAMICS;
		case 0x0061: return DYNAMICS_GAIN;
		case 0x0062: return DYNAMICS_ATTACK;
		case 0x0063: return DYNAMICS_RELEASE;
		case 0x0064: return DYNAMICS_COMPTHRES;
		case 0x0065: return DYNAMICS_COMPRATIO;
		case 0x0066: return DYNAMICS_EXPTHRES;
		case 0x0067: return DYNAMICS_EXPRATIO;
		case 0x0080: return AUTOLEVEL;
		case 0x0081: return AUTOLEVEL_MAXGAIN;
		case 0x0082: return AUTOLEVEL_HEADROOM;
		case 0x0083: return AUTOLEVEL_RISETIME;

		case 0x3C00: return REVERB;
		case 0x3C01: return REVERB_TYPE;
		case 0x3C02: return REVERB_PREDELAY;
		case 0x3C03: return REVERB_LOWCUT;
		case 0x3C04: return REVERB_ROOMSCALE;
		case 0x3C05: return REVERB_ATTACK;
		case 0x3C06: return REVERB_HOLD;
		case 0x3C07: return REVERB_RELEASE;
		case 0x3C08: return REVERB_HIGHCUT;
		case 0x3C09: return REVERB_TIME;
		case 0x3C0A: return REVERB_HIGHDAMP;
		case 0x3C0B: return REVERB_SMOOTH;
		case 0x3C0C: return REVERB_VOLUME;
		case 0x3C0D: return REVERB_WIDTH;

		case 0x3C20: return ECHO;
		case 0x3C21: return ECHO_TYPE;
		case 0x3C22: return ECHO_DELAY;
		case 0x3C23: return ECHO_FEEDBACK;
		case 0x3C24: return ECHO_HIGHCUT;
		case 0x3C25: return ECHO_VOLUME;
		case 0x3C26: return ECHO_WIDTH;


		case 0x3D20: return CLOCK_SOURCE;
		case 0x3D21: return CLOCK_SAMPLERATE;
		case 0x3D22: return CLOCK_WCKSINGLE;
		case 0x3D40: return CLOCK_WCKTERM;
		case 0x3D41: return HARDWARE_OPTICALOUT;
		case 0x3D42: return HARDWARE_SPDIFOUT;

		case 0x3D43: return HARDWARE_STANDALONEMIDI;
		case 0x3D44: return HARDWARE_CCMODE;

		case 0x3F00: return HARDWARE_DSPVERLOAD;
		case 0x3F01: return HARDWARE_DSPSTATUS;
		case 0x3F02: return HARDWARE_DSPAVAIL;

		default:     return UNKNOWN;
	}
	return -1;
}


static int
ctltoreg(enum control ctl, const struct param *p)
{
	int reg, idx = -1, flags = 0;

	if ((unsigned)p->in < LEN(inputs)) {
		flags = inputs[p->in].flags;
		idx = p->in;
	} else if ((unsigned)p->out < LEN(outputs)) {
		flags = outputs[p->out].flags;
		idx = LEN(inputs) + p->out;
	}

	switch (ctl) {

		case INPUT_MUTE:              reg = 0x00; goto channel;
		case INPUT_FXSEND:            reg = 0x01; goto channel;
		case INPUT_STEREO:            reg = 0x02; goto channel;
		case INPUT_MSPROC:            reg = 0x05; goto channel;
		case INPUT_PHASE:             reg = 0x06; goto channel;
		case INPUT_GAIN:              if (~flags & INPUT_HAS_GAIN) break;
			reg = 0x07; goto channel;
		case INPUT_48V:               if (~flags & INPUT_HAS_48V) break;
			reg = 0x08; goto channel;
		case INPUT_REFLEVEL:          if (~flags & INPUT_HAS_REFLEVEL) break;
			reg = 0x08; goto channel;
		case INPUT_HIZ:               if (~flags & INPUT_HAS_HIZ) break;
			reg = 0x09; goto channel;
		case INPUT_AUTOSET:           if (~flags & INPUT_HAS_AUTOSET) break;
			reg = 0x0A; goto channel;

		case OUTPUT_VOLUME:           reg = 0x00; goto channel;
		case OUTPUT_PAN:              reg = 0x01; goto channel;
		case OUTPUT_MUTE:             reg = 0x02; goto channel;
		case OUTPUT_FXRETURN:         reg = 0x03; goto channel;
		case OUTPUT_STEREO:           reg = 0x04; goto channel;
		case OUTPUT_PHASE:            reg = 0x07; goto channel;
		case OUTPUT_REFLEVEL:         reg = 0x08; goto channel;

		case LOWCUT:                  reg = 0x20; goto channel;
		case LOWCUT_FREQ:             reg = 0x21; goto channel;
		case LOWCUT_SLOPE:            reg = 0x22; goto channel;
		case EQ:                      reg = 0x40; goto channel;
		case EQ_BAND1TYPE:            reg = 0x41; goto channel;
		case EQ_BAND1GAIN:            reg = 0x42; goto channel;
		case EQ_BAND1FREQ:            reg = 0x43; goto channel;
		case EQ_BAND1Q:               reg = 0x44; goto channel;
		case EQ_BAND2GAIN:            reg = 0x45; goto channel;
		case EQ_BAND2FREQ:            reg = 0x46; goto channel;
		case EQ_BAND2Q:               reg = 0x47; goto channel;
		case EQ_BAND3TYPE:            reg = 0x48; goto channel;
		case EQ_BAND3GAIN:            reg = 0x49; goto channel;
		case EQ_BAND3FREQ:            reg = 0x4A; goto channel;
		case EQ_BAND3Q:               reg = 0x4B; goto channel;
		case DYNAMICS:                reg = 0x60; goto channel;
		case DYNAMICS_GAIN:           reg = 0x61; goto channel;
		case DYNAMICS_ATTACK:         reg = 0x62; goto channel;
		case DYNAMICS_RELEASE:        reg = 0x63; goto channel;
		case DYNAMICS_COMPTHRES:      reg = 0x64; goto channel;
		case DYNAMICS_COMPRATIO:      reg = 0x65; goto channel;
		case DYNAMICS_EXPTHRES:       reg = 0x66; goto channel;
		case DYNAMICS_EXPRATIO:       reg = 0x67; goto channel;
		case AUTOLEVEL:               reg = 0x80; goto channel;
		case AUTOLEVEL_MAXGAIN:       reg = 0x81; goto channel;
		case AUTOLEVEL_HEADROOM:      reg = 0x82; goto channel;
		case AUTOLEVEL_RISETIME:      reg = 0x83; goto channel;
			channel:
				if (idx == -1) break;
				if (idx < LEN(inputs))
					return (idx << 8) + reg;
				return 0x1200 + ((idx - LEN(inputs)) << 8) + reg;

		case MIX:
			if ((unsigned)p->out >= LEN(outputs)) break;
			if ((unsigned)p->in >= LEN(inputs)) break;
			return 0x12E0 + p->out * 0x100 + p->in;
		case MIX_LEVEL:
			if ((unsigned)p->out >= LEN(outputs)) break;
			if ((unsigned)p->in >= LEN(inputs) + LEN(outputs)) break;
			idx = p->in;
			if (idx >= LEN(inputs)) idx += 0x20 - LEN(inputs);
			return 0x4000 | p->out << 6 | idx;

		case REVERB:                  return 0x3C00;
		case REVERB_TYPE:             return 0x3C01;
		case REVERB_PREDELAY:         return 0x3C02;
		case REVERB_LOWCUT:           return 0x3C03;
		case REVERB_ROOMSCALE:        return 0x3C04;
		case REVERB_ATTACK:           return 0x3C05;
		case REVERB_HOLD:             return 0x3C06;
		case REVERB_RELEASE:          return 0x3C07;
		case REVERB_HIGHCUT:          return 0x3C08;
		case REVERB_TIME:             return 0x3C09;
		case REVERB_HIGHDAMP:         return 0x3C0A;
		case REVERB_SMOOTH:           return 0x3C0B;
		case REVERB_VOLUME:           return 0x3C0C;
		case REVERB_WIDTH:            return 0x3C0D;

		case ECHO:                    return 0x3C20;
		case ECHO_TYPE:               return 0x3C21;
		case ECHO_DELAY:              return 0x3C22;
		case ECHO_FEEDBACK:           return 0x3C23;
		case ECHO_HIGHCUT:            return 0x3C24;
		case ECHO_VOLUME:             return 0x3C25;
		case ECHO_WIDTH:              return 0x3C26;


		case CLOCK_SOURCE:            return 0x3D20;
		case CLOCK_SAMPLERATE:        return 0x3D21;
		case CLOCK_WCKSINGLE:         return 0x3D22;
		case CLOCK_WCKTERM:           return 0x3D40;
		case HARDWARE_OPTICALOUT:     return 0x3D41;
		case HARDWARE_SPDIFOUT:       return 0x3D42;

		case HARDWARE_STANDALONEMIDI: return 0x3D43;
		case HARDWARE_CCMODE:         return 0x3D44;

		case HARDWARE_DSPVERLOAD:     return 0x3F00;
		case HARDWARE_DSPSTATUS:      return 0x3F01;
		case HARDWARE_DSPAVAIL:       return 0x3F02;

		case REFRESH:                 return 0x3F99;
		default:                      break;
	}
	return -1;
}

const struct device ffucx = {
	.id = "ffucx",
	.name = "Fireface UCX",
	.version = 20,
	.flags = 0,
	.inputs = inputs,
	.inputslen = LEN(inputs),
	.outputs = outputs,
	.outputslen = LEN(outputs),
	.refresh = 0x0812,
	.regtoctl = regtoctl,
	.ctltoreg = ctltoreg,
};
