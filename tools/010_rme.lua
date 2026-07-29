--[[
RME USB Protocol Dissector for Wireshark
Version 0.1.0 | Author: Martin Augustyniak (huddx01)
Repository: https://github.com/huddx01/oscmix

INSTALLATION
------------
Place this file in your Wireshark personal plugins folder:
macOS  : ~/.local/lib/wireshark/plugins/
Windows: %APPDATA%\Wireshark\plugins\
Linux  : ~/.local/lib/wireshark/plugins/
Then restart Wireshark or use Analyze Reload Lua Plugins.

SUPPORTED DEVICES
-----------------
Fireface UFX III  (USB + CC mode)
Fireface UCX II   (USB)
Fireface UCX      (USB)
Fireface UFX II   (USB + CC mode)
Fireface 802      (USB + CC mode)
Babyface          (USB)

USAGE
-----
Unplug your device, start capture, then plug-in your device.
Capture USB traffic with Wireshark using usbmon (Linux) or USBPcap (Windows)
or the built-in XHC driver on macOS. The dissector activates automatically for
the VID/PID combinations listed above. Apply display filter "usbrme" to focus
on RME traffic.

NOTES
-----
- The dissector needs the first vID/pID USB connection packets to chime in.
- These packets are sent only once on plugging the device into usb port.
- Make sure Wireshark is "armed" (capture started) before plugging in.
- All register ranges and field descriptions are best-effort from protocol
reverse engineering and may contain inaccuracies.
- Polling registers (0x3F00 CC-mode, 0x3DFF USB-mode) cycle 0x0000-0x000F.
- Room EQ registers use separate read/write addresses on UFX III and UFX II.
--]]

local rme_info = {
	version     = "0.1.0",
	author      = "Martin Augustyniak (huddx01)",
	description = "USB Protocol Dissector for RME Devices (USB and CC Mode).",
	repository  = "https://github.com/huddx01/oscmix",
}
set_plugin_info(rme_info)

-- Forward declaration; field tables are defined after format helpers below.
local device_profiles = {}

-- Lua >= 5.3 compatibility shim for bit32
if not bit32 then
	bit32 = load[[return{
		lshift=function(a, b) return a << b end,
		rshift=function(a, b) return a >> b end,
		bor=function(a, b) return a | b end,
		band=function(a, b) return a & b end,
		bxor=function(a, b) return a ~ b end,
	}]]()
end
if not math.log10 then
	function math.log10(a) return math.log(a, 10) end
end

local endpoint_pf   = Field.new('usb.endpoint_address.number')
local f_subid




----------------------------------------------
-- ProtoField declarations
----------------------------------------------

local register_pf    = ProtoField.uint16('usbrme.register', 'Register', base.HEX)
local value_pf       = ProtoField.uint16('usbrme.value',    'Value',    base.HEX)
local levels_pf      = ProtoField.uint32('usbrme.levels',   'Levels',   base.HEX)
local peak_pf        = ProtoField.uint64('usbrme.peak',     'Peak',     base.HEX)
local rms_pf         = ProtoField.uint32('usbrme.rms',      'RMS',      base.HEX)
local register_desc_pf = ProtoField.string('usbrme.register.description', 'Register Description')
local value_desc_pf    = ProtoField.string('usbrme.value.description',    'Value Description')
local device_name_pf   = ProtoField.string('usbrme.device.name',          'DeviceName')
-- Expert info for register and value descriptions
local exp_reg_desc = ProtoExpert.new("usbrme.expert.reg_desc", "Register Info",
									 expert.group.PROTOCOL, expert.severity.NOTE)
local exp_val_desc = ProtoExpert.new("usbrme.expert.val_desc", "Value Info",
									 expert.group.PROTOCOL, expert.severity.CHAT)



----------------------------------------------
-- Proto definition and field declarations
----------------------------------------------
usb_rme_proto = Proto('usbrme', 'USB RME Protocol')

usb_rme_proto.fields.register          = register_pf
usb_rme_proto.fields.value             = value_pf
usb_rme_proto.fields.levels            = levels_pf
usb_rme_proto.fields.peak              = peak_pf
usb_rme_proto.fields.rms               = rms_pf
usb_rme_proto.fields.devicename        = device_name_pf
usb_rme_proto.fields.registerdescription = register_desc_pf
usb_rme_proto.fields.valuedescription  = value_desc_pf

usb_rme_proto.experts = { exp_reg_desc, exp_val_desc }
----------------------------------------------
-- Format helpers and shared enums
----------------------------------------------

local function format_bool(val)
	val = val:le_uint()
	if val == 0 then
		return 'off'
		elseif val == 1 then
		return 'on'
	end
end

local function format_int(val)
	return val:le_int()
end

local function format_int10(val)
	val = val:le_int()
	return val / 10
end

local function format_float(scale)
	return function(val)
	return val:le_int() / scale
end
end

local function format_int100(val)
	val = val:le_int()
	return val / 100
end

local function format_int1000(val)
	val = val:le_int()
	return val / 1000
end

local function format_time(val)
	val = val:le_uint()
	return string.format('%.2d:%.2d', bit32.rshift(val, 8), bit32.band(val, 0xff))
end

local function format_date(val)
	val = val:le_uint()
	return string.format('%.4d-%.2d-%.2d', 2000 + bit32.rshift(val, 9), bit32.band(bit32.rshift(val, 5), 0xf), bit32.band(val, 0x1f))
end

local function format_volume(val)
	val = val:le_uint()
	local ref
	if bit32.band(val, 0x8000) ~= 0 then
		ref = 0x1000
		val = bit32.band(val, 0x7fff)
		else
		ref = 0x8000
	end
	val = (bit32.bxor(val, 0x4000) - 0x4000) / ref
	local phase
	if val < 0 then
		phase = ' Phase Inverted'
		val = -val
		else
		phase = ''
	end
	return string.format('%.2f dB%s', 20 * math.log10(val), phase)
end


local roomtypes = {
	'Small Room',
	'Medium Room',
	'Large Room',
	'Walls',
	'Shorty',
	'Attack',
	'Swagger',
	'Old School',
	'Echoistic',
	'8plus9',
	'Grand Wide',
	'Thicker',
	'Envelope',
	'Gated',
	'Space',
}

local function format_enum(enum)
	return function(val)
	val = val:le_uint()
	if val < #enum then
		val = val + 1
	end
	return enum[val]
end
end

local function format_cue(val)
	if val:le_int() == 0xffff then
		return string.format('Cue not active')
	end
	local to = val(0, 1):le_int() + 1
	local from = val(1):le_int() + 1
	return string.format('Cue from %d to %d', from, to)
end

local function format_controlroom(val)
	local bits = {}
	if val:bitfield(7) == 1 then table.insert(bits, 'LED Main Mono') end
	if val:bitfield(6) == 1 then table.insert(bits, 'LED VOL PH 1') end
	if val:bitfield(5) == 1 then table.insert(bits, 'LED VOL PH 2') end
	if val:bitfield(4) == 1 then table.insert(bits, 'LED Ext. Input') end
	if val:bitfield(3) == 1 then table.insert(bits, 'LED Talkback') end
	if val:bitfield(2) == 1 then table.insert(bits, 'LED Speaker B') end
	if val:bitfield(1) == 1 then table.insert(bits, 'LED Dim Enabled') end
	if val:bitfield(0) == 1 then table.insert(bits, 'TRS Pedal') end
	return table.concat(bits, ', ')
end

local function format_dsp_available(val)
	local value = val:le_uint()
	local functions = {"1 low cut", "2 low cut", "1 eq", "2 eq", "1 dynamics", "2 dynamics", "1 autolevel", "2 autolevel", "1 record", "2 record"}

	local result = {}
	for i = 0, 9 do
		if bit32.band(value, bit32.lshift(1, i)) ~= 0 then
			table.insert(result, functions[i + 1])
		end
	end

	local unused = bit32.rshift(value, 10)
	if unused ~= 0 then
		table.insert(result, string.format("unused: 0x%x", unused))
	end
	return #result > 0 and table.concat(result, ", ") or "None"
end

local function format_dsp_overload(val)
	local value = val:le_uint()
	local functions = {"play", "low cut", "eq", "dynamics", "autolevel", "record", "delay", "room eq"}

	local result = {}
	for i = 0, 7 do
		local bit_val = bit32.band(bit32.rshift(value, i), 1)
		result[#result + 1] = string.format("%s: %s", functions[i + 1], bit_val == 1 and "ok" or "overload")
	end

	local channel = bit32.rshift(value, 8)
	if channel ~= 0 then
		result[#result + 1] = string.format("channel: %d", channel)
	end

	return table.concat(result, ", ")
end

local function format_fxload(val)
	local value = val:le_uint()
	local dsp_load = bit32.band(value, 0xFF)        -- lower 8 Bits [0:7]
	local dsp_version = bit32.rshift(value, 8)      -- upper 8 Bits [8:15]
	return string.format("Version: %d, Load: %d" , dsp_version, dsp_load)
end

local format_samplerate = format_enum{'32000 Hz', '44100 Hz', '48000 Hz', '64000 Hz', '88200 Hz', '96000 Hz', '128000 Hz', '176400 Hz', '192000 Hz'}

local function format_durecinfo(val)
	return string.format('%s, %s channels', format_samplerate(val(0, 1)), format_int(val(1)))
end

local bandtypes = {'Bell', 'Shelving', 'High Pass', 'Low Pass'}

----------------------------------------------
-- Field tables per device family
-- Each family gets its own dspfields/input/output/global tables.
-- These are referenced by device_profiles further below.
----------------------------------------------

---------
-- FF 802
---------
local ff802_dspfields = {
	[0x20] = {name='Low Cut Enable',            format=format_bool},
	[0x21] = {name='Low Cut Freq',              format=format_int},
	[0x22] = {name='Low Cut dB/oct',            format=format_enum{6, 12, 18, 24}},
	[0x40] = {name='Eq Enable',                 format=format_bool},
	[0x41] = {name='Eq Band 1 Type',            format=format_enum(bandtypes)},
	[0x42] = {name='Eq Band 1 Gain',            format=format_int10},
	[0x43] = {name='Eq Band 1 Freq',            format=format_int},
	[0x44] = {name='Eq Band 1 Q',               format=format_int10},
	[0x45] = {name='Eq Band 2 Gain',            format=format_int10},
	[0x46] = {name='Eq Band 2 Freq',            format=format_int},
	[0x47] = {name='Eq Band 2 Q',               format=format_int10},
	[0x48] = {name='Eq Band 3 Type',            format=format_enum(bandtypes)},
	[0x49] = {name='Eq Band 3 Gain',            format=format_int10},
	[0x4a] = {name='Eq Band 3 Freq',            format=format_int},
	[0x4b] = {name='Eq Band 3 Q',               format=format_int10},
	[0x60] = {name='Dynamics Enable',           format=format_bool},
	[0x61] = {name='Dynamics Gain',             format=format_int10},
	[0x62] = {name='Dynamics Attack',           format=format_int},
	[0x63] = {name='Dynamics Release',          format=format_int},
	[0x64] = {name='Dynamics Comp. Threshold',  format=format_int10},
	[0x65] = {name='Dynamics Comp. Ratio',      format=format_int10},
	[0x66] = {name='Dynamics Exp. Threshold',   format=format_int10},
	[0x67] = {name='Dynamics Exp. Ratio',       format=format_int10},
	[0x80] = {name='Autolevel Enable',          format=format_bool},
	[0x81] = {name='Autolevel Max Gain',        format=format_int10},
	[0x82] = {name='Autolevel Headroom',        format=format_int10},
	[0x83] = {name='Autolevel Rise Time',       format=format_int10},
}
local ff802_input_fields = setmetatable({
										[0x00] = {name='Mute',           format=format_bool},
										[0x01] = {name='FX Send',        format=format_int10},
										[0x02] = {name='Stereo',         format=format_bool},
										[0x03] = {name='Record',         format=format_bool},
										[0x04] = {name='Play Channel',   format=format_int},
										[0x05] = {name='M/S Proc',       format=format_bool},
										[0x06] = {name='Phase Invert',   format=format_bool},
										[0x07] = {name='Gain',           format=format_int10},
										[0x08] = {name='RefLevel/48v',   format=format_enum{'off', 'on'}},
										[0x09] = {name='Hi-Z',           format=format_bool},
										[0x0a] = {name='Autoset',        format=format_bool},
										}, {__index=ff802_dspfields})
local ff802_output_fields = setmetatable({
										 [0x00] = {name='Volume',         format=format_int10},
										 [0x01] = {name='Balance',        format=format_int},
										 [0x02] = {name='Mute',           format=format_bool},
										 [0x03] = {name='FX Return',      format=format_int10},
										 [0x04] = {name='Stereo',         format=format_bool},
										 [0x05] = {name='Record',         format=format_bool},
										 [0x06] = {name='Play Channel',   format=format_int},
										 [0x07] = {name='Phase Invert',   format=format_bool},
										 [0x08] = {name='RefLevel',       format=format_enum{'+4dBu', '+13dBu', '+19dBu'}},
										 }, {__index=ff802_dspfields})
local ff802_global_fields = {
	[0x3c00] = {name='Reverb Enable',                      format=format_bool},
	[0x3c01] = {name='Reverb Room Type',                   format=format_enum(roomtypes)},
	[0x3c02] = {name='Reverb Pre Delay',                   format=format_int},
	[0x3c03] = {name='Reverb Low Cut Freq',                format=format_int},
	[0x3c04] = {name='Reverb Room Scale',                  format=format_int100},
	[0x3c05] = {name='Reverb Attack Time',                 format=format_int},
	[0x3c06] = {name='Reverb Hold Time',                   format=format_int},
	[0x3c07] = {name='Reverb Release Time',                format=format_int},
	[0x3c08] = {name='Reverb High Cut Freq',               format=format_int},
	[0x3c09] = {name='Reverb Time',                        format=format_int10},
	[0x3c0b] = {name='Reverb Smoothness',                  format=format_int},
	[0x3c0c] = {name='Reverb Volume',                      format=format_int10},
	[0x3c0d] = {name='Reverb Stereo Width',                format=format_int100},
	[0x3c20] = {name='Echo Enable',                        format=format_bool},
	[0x3c21] = {name='Echo Type',                          format=format_enum{'Stereo Echo', 'Stereo Cross', 'Pong Echo'}},
	[0x3c23] = {name='Echo Feedback',                      format=format_int},
	[0x3c24] = {name='Echo High Cut',                      format=format_enum{'off', '16kHz', '12kHz', '8kHz', '4kHz', '2kHz'}},
	[0x3c25] = {name='Echo Volume',                        format=format_int10},
	[0x3c26] = {name='Echo Stereo Width',                  format=format_int100},
	[0x3d00] = {name='Main Out',                           format=format_enum{'1/2', '3/4', '5/6', '7/8', '9/10', '11/12', '13/14', '15/16', '17/18', '19/20', '21/22', '23/24', '25/26', '27/28', '29/30'}},
	[0x3d01] = {name='Mute Enable',                        format=format_bool},
	[0x3d02] = {name='Dim Volume',                         format=format_int10},
	[0x3d03] = {name='Dim Enable',                         format=format_bool},
	[0x3d04] = {name='Recall Volume',                      format=format_int10},
	[0x3d20] = {name='CC-Mode: Clock Source',              format=format_enum{'Internal', 'Word Clock', 'AES', 'ADAT 1', 'ADAT 2'}},
	[0x3d21] = {name='CC-Mode: Sample Rate',               format=format_samplerate},
	[0x3d22] = {name='CC-Mode: Word Clock Out Single Speed',format=format_bool},
	[0x3d23] = {name='CC-Mode: Word Clock Termination',    format=format_bool},
	[0x3d40] = {name='CC-Mode: AES Input Source',          format=format_enum{'AES', 'ADAT2'}},
	[0x3d41] = {name='CC-Mode: Optical Out',               format=format_enum{'ADAT2', 'AES/SPDIF'}},
	[0x3d42] = {name='CC-Mode: SPDIF Format',              format=format_enum{'Consumer', 'Professional'}},
	[0x3d43] = {name='Standalone MIDI',                    format=format_bool},
	[0x3d44] = {name='CC Mode Active',                     format=format_bool},
	[0x3d45] = {name='Standalone ARC',                     format=format_enum{'Volume', '1s Op', 'Normal'}},
	[0x3dff] = {name='USB-Mode Poll Cycle',             format=format_int},
	[0x3f00] = {name='CC-Mode Poll Cycle / DSP Status', format=format_fxload},
	[0x3f01] = {name='DSP Function Available',          format=format_dsp_available},
	[0x3f02] = {name='DSP Function Overload',           format=format_dsp_overload},
	[0x3f05] = {name='Dump Finished Indicator',         format=format_int},
	[0x3f10] = {name='Unknown 3f10',                    format=format_int},
	[0x3f50] = {name='Unknown 3f50',                    format=format_int},
	[0x3f64] = {name='Unknown 3f64',                    format=format_int},
	[0x3f96] = {name='Load State from Slot 1-6',        format=format_int},
	[0x3f97] = {name='Store State in Slot 1-6',         format=format_int},
	[0x3f98] = {name='Cue',                             format=format_cue},
	[0x3f99] = {name='Full Dump State Request FF802',   format=format_int},
	[0x3f9e] = {name='ARC LEDs',                        format=format_int},
}

-- Room EQ field tables (UFX III, UFX II, UCX II)
-- 0x20 (32) registers per output channel.
-- Write and read addresses are separate ranges; dispatch is direction-aware.

-- Band type enums differ between bands 1 (low) and 8/9 (high).
local roomeq_band_types_low  = {'Bell', 'Low Shelf',  'High Pass', 'Low Pass'}
local roomeq_band_types_high = {'Bell', 'High Shelf', 'Low Pass',  'High Pass'}

local roomeq_fields = {
	[0x00] = {name='Delay',           format=format_int100},
	[0x01] = {name='Room EQ Enable',  format=format_bool},
	[0x02] = {name='Band 1 Type',     format=format_enum(roomeq_band_types_low)},
	[0x03] = {name='Band 1 Gain',     format=format_int10},
	[0x04] = {name='Band 1 Freq',     format=format_int},
	[0x05] = {name='Band 1 Q',        format=format_int10},
	[0x06] = {name='Band 2 Gain',     format=format_int10},
	[0x07] = {name='Band 2 Freq',     format=format_int},
	[0x08] = {name='Band 2 Q',        format=format_int10},
	[0x09] = {name='Band 3 Gain',     format=format_int10},
	[0x0a] = {name='Band 3 Freq',     format=format_int},
	[0x0b] = {name='Band 3 Q',        format=format_int10},
	[0x0c] = {name='Band 4 Gain',     format=format_int10},
	[0x0d] = {name='Band 4 Freq',     format=format_int},
	[0x0e] = {name='Band 4 Q',        format=format_int10},
	[0x0f] = {name='Band 5 Gain',     format=format_int10},
	[0x10] = {name='Band 5 Freq',     format=format_int},
	[0x11] = {name='Band 5 Q',        format=format_int10},
	[0x12] = {name='Band 6 Gain',     format=format_int10},
	[0x13] = {name='Band 6 Freq',     format=format_int},
	[0x14] = {name='Band 6 Q',        format=format_int10},
	[0x15] = {name='Band 7 Gain',     format=format_int10},
	[0x16] = {name='Band 7 Freq',     format=format_int},
	[0x17] = {name='Band 7 Q',        format=format_int10},
	[0x18] = {name='Band 8 Type',     format=format_enum(roomeq_band_types_high)},
	[0x19] = {name='Band 8 Gain',     format=format_int10},
	[0x1a] = {name='Band 8 Freq',     format=format_int},
	[0x1b] = {name='Band 8 Q',        format=format_int10},
	[0x1c] = {name='Band 9 Type',     format=format_enum(roomeq_band_types_high)},
	[0x1d] = {name='Band 9 Gain',     format=format_int10},
	[0x1e] = {name='Band 9 Freq',     format=format_int},
	[0x1f] = {name='Band 9 Q',        format=format_int10},
}

-- Output channel names for Room EQ (94 channels, 0-indexed):
-- 0-7: AN 1-8 | 8-11: PH 9-12 | 12-13: AES L/R | 14-29: ADAT 1-16 | 30-93: MA 1-64
local roomeq_ch_names = {}
for i = 1, 8  do roomeq_ch_names[i - 1]  = 'AN '   .. i         end
for i = 9, 12 do roomeq_ch_names[i - 1]  = 'PH '   .. i         end
roomeq_ch_names[12] = 'AES L'
roomeq_ch_names[13] = 'AES R'
for i = 1, 16 do roomeq_ch_names[13 + i] = 'ADAT ' .. i         end
for i = 1, 64 do roomeq_ch_names[29 + i] = 'MA '   .. i         end

---------
-- FF UFX III DSP / channel field tables
---------
local dspfields = {
	[0x0d] = {name='Low Cut Enable', format=format_bool},
	[0x0e] = {name='Low Cut Freq', format=format_int},
	[0x0f] = {name='Low Cut dB/oct', format=format_enum{6, 12, 18, 24}},
	[0x10] = {name='Eq Enable', format=format_bool},
	[0x11] = {name='Eq Band 1 Type', format=format_enum(bandtypes)},
	[0x12] = {name='Eq Band 1 Gain', format=format_int10},
	[0x13] = {name='Eq Band 1 Freq', format=format_int},
	[0x14] = {name='Eq Band 1 Q', format=format_int10},
	[0x15] = {name='Eq Band 2 Gain', format=format_int10},
	[0x16] = {name='Eq Band 2 Freq', format=format_int},
	[0x17] = {name='Eq Band 2 Q', format=format_int10},
	[0x18] = {name='Eq Band 3 Type', format=format_enum(bandtypes)},
	[0x19] = {name='Eq Band 3 Gain', format=format_int10},
	[0x1a] = {name='Eq Band 3 Freq', format=format_int},
	[0x1b] = {name='Eq Band 3 Q', format=format_int10},
	[0x1c] = {name='Dynamics Enable', format=format_bool},
	[0x1d] = {name='Dynamics Gain', format=format_int10},
	[0x1e] = {name='Dynamics Attack', format=format_int},
	[0x1f] = {name='Dynamics Release', format=format_int},
	[0x20] = {name='Dynamics Comp. Threshold', format=format_int10},
	[0x21] = {name='Dynamics Comp. Ratio', format=format_int10},
	[0x22] = {name='Dynamics Exp. Threshold', format=format_int10},
	[0x23] = {name='Dynamics Exp. Ratio', format=format_int10},
	[0x24] = {name='Autolevel Enable', format=format_bool},
	[0x25] = {name='Autolevel Max Gain', format=format_int10},
	[0x26] = {name='Autolevel Headroom', format=format_int10},
	[0x27] = {name='Autolevel Rise Time', format=format_int10},
}
local input_fields = setmetatable({
								  [0x00] = {name='Mute', format=format_bool},
								  [0x01] = {name='FX Send', format=format_int10},
								  [0x02] = {name='Stereo', format=format_bool},
								  [0x03] = {name='Record', format=format_bool},
								  [0x04] = {name='Unknown Input Field ?', format=format_int},
								  [0x05] = {name='Play Channel', format=format_int},
								  [0x06] = {name='Width?', format=format_int},
								  [0x07] = {name='M/S Proc', format=format_bool},
								  [0x08] = {name='Phase Invert', format=format_bool},
								  [0x09] = {name='Gain', format=format_int10},
								  [0x0a] = {name='Ref. Level / 48v', format=format_enum{'+13dBu / off', '+19dBu / on'}},
								  [0x0b] = {name='Hi-Z', format=format_bool},
								  [0x0c] = {name='Autoset', format=format_bool},
								  }, {__index=dspfields})
local output_fields = setmetatable({
								   [0x00] = {name='Volume', format=format_int10},
								   [0x01] = {name='Balance', format=format_int},
								   [0x02] = {name='Mute', format=format_bool},
								   [0x03] = {name='FX Return', format=format_int10},
								   [0x04] = {name='Stereo', format=format_bool},
								   [0x05] = {name='Record', format=format_bool},
								   [0x06] = {name='Unknown Output Field ?', format=format_int},
								   [0x07] = {name='Play Channel', format=format_int},
								   [0x08] = {name='Phase Invert', format=format_bool},
								   [0x09] = {name='Ref. Level', format=format_enum{'+4dBu', '+13dBu', '+19dBu'}},
								   [0x0a] = {name='Crossfeed', format=format_enum{'off', 'Crossf. 1', 'Crossf. 2', 'Crossf. 3', 'Crossf. 4', 'Crossf. 5'}},
								   [0x0b] = {name='Volume Calibration', format=format_int100},
								   }, {__index=dspfields})
local playback_fields = {
	[0x00] = {name='Mute', format=format_bool},
	[0x01] = {name='FX Return', format=format_int10},
	[0x02] = {name='Stereo', format=format_bool},
	[0x03] = {name='Width', format=format_int},
	[0x04] = {name='M/S Proc', format=format_bool},
	[0x05] = {name='Phase Invert', format=format_bool},
}

local global_fields = {
	[0x3000] = {name='Reverb Enable',                  format=format_bool},
	[0x3001] = {name='Reverb Room Type',               format=format_enum(roomtypes)},
	[0x3002] = {name='Reverb Pre Delay',               format=format_int},
	[0x3003] = {name='Reverb Low Cut Freq',            format=format_int},
	[0x3004] = {name='Reverb Room Scale',              format=format_int100},
	[0x3005] = {name='Reverb Attack Time',             format=format_int},
	[0x3006] = {name='Reverb Hold Time',               format=format_int},
	[0x3007] = {name='Reverb Release Time',            format=format_int},
	[0x3008] = {name='Reverb High Cut Freq',           format=format_int},
	[0x3009] = {name='Reverb Time',                    format=format_int10},
	[0x300a] = {name='Reverb High Damp',               format=format_int},
	[0x300b] = {name='Reverb Smoothness',              format=format_int},
	[0x300c] = {name='Reverb Volume',                  format=format_int10},
	[0x300d] = {name='Reverb Stereo Width',            format=format_int100},
	[0x3014] = {name='Echo Enable',                    format=format_bool},
	[0x3015] = {name='Echo Type',                      format=format_enum{'Stereo Echo', 'Stereo Cross', 'Pong Echo'}},
	[0x3016] = {name='Echo Delay Time',                format=format_int1000},
	[0x3017] = {name='Echo Feedback',                  format=format_int},
	[0x3018] = {name='Echo High Cut',                  format=format_enum{'off', '16kHz', '12kHz', '8kHz', '4kHz', '2kHz'}},
	[0x3019] = {name='Echo Volume',                    format=format_int10},
	[0x301a] = {name='Echo Stereo Width',              format=format_int100},
	[0x3050] = {name='Main Out',                       format=format_enum{
		"1/2", "3/4", "5/6", "7/8", "9/10", "11/12", "13/14", "15/16",
		"17/18", "19/20", "21/22", "23/24", "25/26", "27/28", "29/30", "31/32",
		"33/34", "35/36", "37/38", "39/40", "41/42", "43/44", "45/46", "47/48",
		"49/50", "51/52", "53/54", "55/56", "57/58", "59/60", "61/62", "63/64",
		"65/66", "67/68", "69/70", "71/72", "73/74", "75/76", "77/78", "79/80",
		"81/82", "83/84", "85/86", "87/88", "89/90", "91/92", "93/94"}},
	[0x3051] = {name='Main Mono',                      format=format_bool},
	[0x3052] = {name='Mute Enable',                    format=format_bool},
	[0x3053] = {name='Dim Reduction',                  format=format_int},
	[0x3054] = {name='Dim Enable',                     format=format_bool},
	[0x3055] = {name='Recall Volume',                  format=format_int},
	[0x3064] = {name='Clock Source',                   format=format_enum{'Internal', 'Word Clock', 'SPDIF', 'AES', 'Optical'}},
	[0x3066] = {name='Word Clock Out',                 format=format_bool},
	[0x3067] = {name='Word Clock Out Single Speed',    format=format_bool},
	[0x3068] = {name='Word Clock Termination',         format=format_bool},
	[0x3078] = {name='AES IN',                         format=format_enum{'XLR', 'Optical 2'}},
	[0x3079] = {name='Optical Out 1',                  format=format_enum{'ADAT', 'SPDIF'}},
	[0x307A] = {name='Optical Out 2',                  format=format_enum{'ADAT', 'SPDIF', 'AES'}},
	[0x307B] = {name='AES Channel Status',             format=format_enum{'Consumer', 'Professional'}},
	[0x307C] = {name='Interface Mode',                 format=format_enum{'Auto', 'USB2', 'USB3', 'CC'}},
	[0x307D] = {name='CC Routing',                     format=format_enum{'All Ch.', 'Phones'}},
	[0x3200] = {name='DSP Version / DSP Load',         format=format_fxload},
	[0x3201] = {name='DSP Function Available',         format=format_dsp_available},
	[0x3202] = {name='DSP Function Overload',          format=format_dsp_overload},
	[0x3203] = {name='ARC Encoder Delta',              format=format_int},
	[0x3204] = {name='ARC LEDs Status',                format=format_int},
	[0x3580] = {name='Durec Status',                   format=format_enum{[0x20]='No Media', [0x22]='Initializing', [0x25]='Stopped', [0x2a]='Playing', [0x06]='Recording'}},
	[0x3581] = {name='Durec Time',                     format=format_int},
	[0x3582] = {name='Durec USB Errors',               format=format_int},
	[0x3583] = {name='Durec USB Load',                 format=format_int},
	[0x3584] = {name='Durec Total Space',              format=format_float(16)},
	[0x3585] = {name='Durec Free Space',               format=format_float(16)},
	[0x3586] = {name='Durec Num Tracks',               format=format_int},
	[0x3587] = {name='Durec Current Track',            format=format_int},
	[0x3589] = {name='Durec Remaining Record Time',    format=format_int},
	[0x358f] = {name='Durec Track Info',               format=format_durecinfo},
	[0x3e00] = {name='Cue',                            format=format_cue},
	[0x3e01] = {name='Unknown 3e01',                   format=format_int},
	[0x3e02] = {name='ARC LED / Control Room Status',  format=format_controlroom},
	[0x3e03] = {name='Full Dump Request v1 (USB)',     format=format_int},
	[0x3e04] = {name='Full Dump Request v2 (CC)',      format=format_int},
	[0x3e05] = {name='Unknown 3e05',                   format=format_int},
	[0x3e06] = {name='Setup Store',                    format=format_enum{[0x0910]='Slot 1', [0x0911]='Slot 2', [0x0912]='Slot 3', [0x0913]='Slot 4', [0x0914]='Slot 5', [0x0915]='Slot 6'}},
	[0x3e07] = {name='Setup Slot (set on start)',      format=format_int},
	[0x3e08] = {name='Time',                           format=format_time},
	[0x3e09] = {name='Date',                           format=format_date},
	[0x3e9a] = {name='Durec Play Control',             format=format_enum{[0x8120]='Stop Record', [0x8121]='Stop', [0x8122]='Record', [0x8123]='Play/Pause'}},
	[0x3e9b] = {name='Durec Delete',                   format=format_enum{[0x8000]='Delete'}},
	[0x3e9d] = {name='Durec Seek',                     format=format_int},
	[0x3e9e] = {name='Durec Track Select',             format=format_enum{'Previous', 'Next'}},
	[0x3ea0] = {name='Durec Play Mode',                format=format_enum{[0x8000]='Single', [0x8001]='UFX Single', [0x8002]='Continuous', [0x8003]='Single Next', [0x8004]='Repeat Single', [0x8005]='Repeat All'}},
	-- Polling: host cycles 0x0000-0x000F on these registers to keep connection alive.
	-- 0x3DFF: USB mode (~5 Hz).  0x3F00: CC mode (~24-29 Hz).
	[0x3dff] = {name='USB-Mode Poll Cycle',            format=format_int},
	[0x3f00] = {name='CC-Mode Poll Cycle',             format=format_int},
}


---------
-- FF UCX II
---------
local ucxii_dspfields = {
	[0x0c] = {name='Low Cut Enable',           format=format_bool},
	[0x0d] = {name='Low Cut Freq',             format=format_int},
	[0x0e] = {name='Low Cut dB/oct',           format=format_enum{6, 12, 18, 24}},
	[0x0f] = {name='Eq Enable',                format=format_bool},
	[0x10] = {name='Eq Band 1 Type',           format=format_enum(bandtypes)},
	[0x11] = {name='Eq Band 1 Gain',           format=format_int10},
	[0x12] = {name='Eq Band 1 Freq',           format=format_int},
	[0x13] = {name='Eq Band 1 Q',              format=format_int10},
	[0x14] = {name='Eq Band 2 Gain',           format=format_int10},
	[0x15] = {name='Eq Band 2 Freq',           format=format_int},
	[0x16] = {name='Eq Band 2 Q',              format=format_int10},
	[0x17] = {name='Eq Band 3 Type',           format=format_enum(bandtypes)},
	[0x18] = {name='Eq Band 3 Gain',           format=format_int10},
	[0x19] = {name='Eq Band 3 Freq',           format=format_int},
	[0x1a] = {name='Eq Band 3 Q',              format=format_int10},
	[0x1b] = {name='Dynamics Enable',          format=format_bool},
	[0x1c] = {name='Dynamics Gain',            format=format_int10},
	[0x1d] = {name='Dynamics Attack',          format=format_int},
	[0x1e] = {name='Dynamics Release',         format=format_int},
	[0x1f] = {name='Dynamics Comp. Threshold', format=format_int10},
	[0x20] = {name='Dynamics Comp. Ratio',     format=format_int10},
	[0x21] = {name='Dynamics Exp. Threshold',  format=format_int10},
	[0x22] = {name='Dynamics Exp. Ratio',      format=format_int10},
	[0x23] = {name='Autolevel Enable',         format=format_bool},
	[0x24] = {name='Autolevel Max Gain',       format=format_int10},
	[0x25] = {name='Autolevel Headroom',       format=format_int10},
	[0x26] = {name='Autolevel Rise Time',      format=format_int10},
}
local ucxii_input_fields = setmetatable({
										[0x00] = {name='Mute',         format=format_bool},
										[0x01] = {name='FX Send',      format=format_int10},
										[0x02] = {name='Stereo',       format=format_bool},
										[0x03] = {name='Record',       format=format_bool},
										[0x05] = {name='Play Channel', format=format_int},
										[0x06] = {name='M/S Proc',     format=format_bool},
										[0x07] = {name='Phase Invert', format=format_bool},
										[0x08] = {name='Gain',         format=format_int10},
										[0x09] = {name='48v',          format=format_bool},
										[0x0a] = {name='Autoset',      format=format_bool},
										[0x0b] = {name='Hi-Z',         format=format_bool},
										}, {__index=ucxii_dspfields})
local ucxii_output_fields = setmetatable({
										 [0x00] = {name='Volume',       format=format_int10},
										 [0x01] = {name='Balance',      format=format_int},
										 [0x02] = {name='Mute',         format=format_bool},
										 [0x03] = {name='FX Return',    format=format_int10},
										 [0x04] = {name='Stereo',       format=format_bool},
										 [0x05] = {name='Record',       format=format_bool},
										 [0x07] = {name='Play Channel', format=format_int},
										 [0x08] = {name='Phase Invert', format=format_bool},
										 [0x09] = {name='Ref. Level',   format=format_enum{'+4dBu', '+13dBu', '+19dBu'}},
										 }, {__index=ucxii_dspfields})
-- UCX II global fields: 0x3xxx register layout differs from UFX III in several places.
-- Key differences: DSP status at 0x3080, hardware regs at 0x307x, no MADI.
local ucxii_global_fields = {
	[0x3000] = {name='Reverb Enable',                  format=format_bool},
	[0x3001] = {name='Reverb Room Type',               format=format_enum(roomtypes)},
	[0x3002] = {name='Reverb Pre Delay',               format=format_int},
	[0x3003] = {name='Reverb Low Cut Freq',            format=format_int},
	[0x3004] = {name='Reverb Room Scale',              format=format_int100},
	[0x3005] = {name='Reverb Attack Time',             format=format_int},
	[0x3006] = {name='Reverb Hold Time',               format=format_int},
	[0x3007] = {name='Reverb Release Time',            format=format_int},
	[0x3008] = {name='Reverb High Cut Freq',           format=format_int},
	[0x3009] = {name='Reverb Time',                    format=format_int10},
	[0x300a] = {name='Reverb High Damp',               format=format_int},
	[0x300b] = {name='Reverb Smoothness',              format=format_int},
	[0x300c] = {name='Reverb Volume',                  format=format_int10},
	[0x300d] = {name='Reverb Stereo Width',            format=format_int100},
	[0x3014] = {name='Echo Enable',                    format=format_bool},
	[0x3015] = {name='Echo Type',                      format=format_enum{'Stereo Echo', 'Stereo Cross', 'Pong Echo'}},
	[0x3016] = {name='Echo Delay Time',                format=format_int1000},
	[0x3017] = {name='Echo Feedback',                  format=format_int},
	[0x3018] = {name='Echo High Cut',                  format=format_enum{'off', '16kHz', '12kHz', '8kHz', '4kHz', '2kHz'}},
	[0x3019] = {name='Echo Volume',                    format=format_int10},
	[0x301a] = {name='Echo Stereo Width',              format=format_int100},
	[0x3050] = {name='Main Out',                       format=format_enum{'off', '1/2', '3/4', '5/6', '7/8', '9/10', '11/12', '13/14', '15/16', '17/18', '19/20'}},
	[0x3051] = {name='Main Mono',                      format=format_bool},
	[0x3052] = {name='Phones Source',                  format=format_int},
	[0x3053] = {name='Mute Enable',                    format=format_bool},
	[0x3054] = {name='Dim Reduction',                  format=format_int10},
	[0x3055] = {name='Dim Enable',                     format=format_bool},
	[0x3056] = {name='Recall Volume',                  format=format_int10},
	[0x3064] = {name='Clock Source',                   format=format_enum{'Internal', 'Word Clock', 'SPDIF', 'AES', 'Optical'}},
	[0x3065] = {name='Sample Rate',                    format=format_samplerate},
	[0x3066] = {name='Word Clock Out',                 format=format_bool},
	[0x3067] = {name='Word Clock Single Speed',        format=format_bool},
	[0x3068] = {name='Word Clock Termination',         format=format_bool},
	[0x3078] = {name='Optical Out',                    format=format_enum{'ADAT', 'SPDIF'}},
	[0x3079] = {name='SPDIF Format',                   format=format_enum{'Consumer', 'Professional'}},
	[0x307a] = {name='CC Mode',                        format=format_bool},
	[0x307b] = {name='CC Mix',                         format=format_enum{'TotalMix', '6ch+Phones', '8ch', '20ch'}},
	[0x307c] = {name='Standalone MIDI',                format=format_bool},
	[0x307d] = {name='Standalone ARC',                 format=format_enum{'Volume', '1s Op', 'Normal'}},
	[0x307e] = {name='Lock Keys',                      format=format_enum{'off', 'Keys', 'All'}},
	[0x307f] = {name='Remap Keys',                     format=format_bool},
	[0x3080] = {name='DSP Version / DSP Load',         format=format_fxload},
	[0x3081] = {name='DSP Function Available',         format=format_dsp_available},
	[0x3082] = {name='DSP Function Overload',          format=format_dsp_overload},
	[0x3083] = {name='ARC Encoder Delta',              format=format_int},
	-- 0x3180-0x3193: compression level status (read-only), paired channels.
	-- 0x3380-0x3393: autolevel status (read-only), paired channels.
	-- Handled via ucxii profile dynlevel_base / autolevel_base ranges.
	[0x2fc0] = {name='Dump Finished',                  format=format_int},
	[0x3580] = {name='Durec Status',                   format=format_enum{[0x20]='No Media', [0x22]='Initializing', [0x25]='Stopped', [0x2a]='Playing', [0x06]='Recording'}},
	[0x3581] = {name='Durec Playback/Record Position', format=format_int},
	[0x3582] = {name='Durec Unknown 3582',             format=format_int},
	[0x3583] = {name='Durec USB Load/Errors',          format=format_int},
	[0x3584] = {name='Durec Total Space',              format=format_float(16)},
	[0x3585] = {name='Durec Free Space',               format=format_float(16)},
	[0x3586] = {name='Durec Total Tracks',             format=format_int},
	[0x3587] = {name='Durec Selected Track',           format=format_int},
	[0x3588] = {name='Durec Next Track / Play Mode',   format=format_int},
	[0x3589] = {name='Durec Remaining Record Time',    format=format_int},
	[0x358f] = {name='Durec Track Info',               format=format_durecinfo},
	[0x3590] = {name='Durec Track Length',             format=format_int},
	[0x3e00] = {name='Cue',                            format=format_cue},
	[0x3e02] = {name='ARC LEDs',                       format=format_controlroom},
	[0x3e04] = {name='Trigger Register Dump',          format=format_int},
	[0x3e06] = {name='Setup Store',                    format=format_enum{[0x0910]='Slot 1', [0x0911]='Slot 2', [0x0912]='Slot 3', [0x0913]='Slot 4', [0x0914]='Slot 5', [0x0915]='Slot 6'}},
	[0x3e07] = {name='Unknown 3e07',                   format=format_int},
	[0x3e08] = {name='Time',                           format=format_time},
	[0x3e09] = {name='Date',                           format=format_date},
	[0x3e9a] = {name='Durec Play Control',             format=format_enum{[0x8120]='Stop Record', [0x8121]='Stop', [0x8122]='Record', [0x8123]='Play/Pause'}},
	[0x3e9b] = {name='Durec Delete',                   format=format_int},
	[0x3e9c] = {name='Durec Track Select',             format=format_int},
	[0x3e9d] = {name='Durec Seek',                     format=format_int},
	[0x3e9e] = {name='Durec Track Next/Prev',          format=format_enum{'Previous', 'Next'}},
	[0x3ea0] = {name='Durec Play Mode',                format=format_enum{[0x8000]='Single', [0x8001]='UFX Single', [0x8002]='Continuous', [0x8003]='Single Next', [0x8004]='Repeat Single', [0x8005]='Repeat All'}},
	[0x3dff] = {name='USB-Mode Poll Cycle',            format=format_int},
	[0x3f00] = {name='CC-Mode Poll Cycle',             format=format_int},
}

-- Generic format functions — driven by the active device profile.
-- 'profile' is passed in from the dissector so no global state is needed.

local function format_input(reg, val, profile)
	local chan = math.floor(reg / profile.ch_stride) + 1
	local sub  = reg % profile.ch_stride
	local field = profile.fields.input[sub]
	if field then
		return string.format('Input %d %s', chan, field.name), field.format(val)
		else
		return string.format('Input %d %#.2x', chan, sub)
	end
end

local function format_output(reg, val, profile)
	local rel  = reg - profile.output_base
	local chan  = math.floor(rel / profile.ch_stride) + 1
	local sub   = rel % profile.ch_stride
	local field = profile.fields.output[sub]
	if field then
		return string.format('Output %d %s', chan, field.name), field.format(val)
		else
		return string.format('Output %d %#.2x', chan, sub)
	end
end

local function format_playback(reg, val, profile)
	local stride = profile.pb_stride or profile.ch_stride
	local rel   = reg - profile.playback_base
	local chan   = math.floor(rel / stride) + 1
	local sub    = rel % stride
	local field  = profile.fields.playback and profile.fields.playback[sub]
	if field then
		return string.format('Playback %d %s', chan, field.name), field.format(val)
		else
		return string.format('Playback %d %#.2x', chan, sub)
	end
end

----------------------------------------------
-- Generic format functions (profile-driven)
----------------------------------------------

local function format_global(reg, val, profile)
	local fields = (profile and profile.fields.global) or global_fields
	local field = fields[reg]
	if field then
		return field.name, field.format(val)
	end
end

----------------------------------------------
-- FF UFX II field tables
-- Input stride: 0x30. Outputs start at 0x05A0, stride 0x30.
-- Playbacks start at 0x0B40, stride 0x0A.
----------------------------------------------

-- UFX II DSP offsets are the same as UCX II (start at 0x0C).
local ufxii_dspfields = ucxii_dspfields

local ufxii_input_fields = setmetatable({
										[0x00] = {name='Mute',           format=format_bool},
										[0x01] = {name='FX Send',        format=format_int10},
										[0x02] = {name='Stereo',         format=format_bool},
										[0x03] = {name='Record',         format=format_bool},
										[0x04] = {name='Play Channel',   format=format_int},
										[0x05] = {name='Width',          format=format_int100},
										[0x06] = {name='M/S Proc',       format=format_bool},
										[0x07] = {name='Phase Invert',   format=format_bool},
										[0x08] = {name='Gain',           format=format_int10},
										[0x09] = {name='Ref Level / 48v',format=format_enum{'+13dBu / off', '+19dBu / on'}},
										}, {__index=ufxii_dspfields})

local ufxii_output_fields = setmetatable({
										 [0x00] = {name='Volume',          format=format_int10},
										 [0x01] = {name='Balance',         format=format_int},
										 [0x02] = {name='Mute',            format=format_bool},
										 [0x03] = {name='FX Return',       format=format_int10},
										 [0x04] = {name='Stereo',          format=format_bool},
										 [0x05] = {name='Record',          format=format_bool},
										 [0x06] = {name='Play Channel',    format=format_int},
										 [0x07] = {name='Phase Invert',    format=format_bool},
										 [0x08] = {name='Ref Level',       format=format_enum{'-10dBV', '+4dBu', 'Hi Gain', '+24dBu'}},
										 [0x09] = {name='Crossfeed',       format=format_enum{'off', 'Crossf. 1', 'Crossf. 2', 'Crossf. 3', 'Crossf. 4', 'Crossf. 5'}},
										 [0x0b] = {name='Volume Cal.',     format=format_int100},
										 }, {__index=ufxii_dspfields})

-- UFX II playback fields (same sub-register layout as UFX III).
local ufxii_playback_fields = playback_fields

-- UFX II shares the same global register layout as UCX II.
local ufxii_global_fields = ucxii_global_fields

----------------------------------------------
-- Device profile field bundles
----------------------------------------------

local ufxiii_fields = {
	input    = input_fields,
	output   = output_fields,
	playback = playback_fields,
	global   = global_fields,
}
local ucxii_fields_bundle = {
	input    = ucxii_input_fields,
	output   = ucxii_output_fields,
	playback = nil,
	global   = ucxii_global_fields,
}
local ff802_fields_bundle = {
	input    = ff802_input_fields,
	output   = ff802_output_fields,
	playback = nil,
	global   = ff802_global_fields,
}
local ufxii_fields_bundle = {
	input    = ufxii_input_fields,
	output   = ufxii_output_fields,
	playback = ufxii_playback_fields,
	global   = ufxii_global_fields,
}

----------------------------------------------
-- Device profiles — one entry per VID/PID.
-- All register ranges live here; dissector body is device-agnostic.
----------------------------------------------

device_profiles = {
	-- Fireface UCX II  USB  HS
	[0x2a393f82] = {
		name              = "Fireface UCX II (USB)",
		mode              = "USB", ep_out = 12, ep_in = 13, ep_levels = 5,
		ch_stride         = 0x40,
		input_end         = 0x0500,
		output_base       = 0x0500, output_end        = 0x0a00,
		playback_base     = nil,    playback_end       = nil,
		channame_base     = 0x3200, channame_out_offset= 20,
		dynlevel_base     = 0x3180, dynlevel_in_count  = 10,  -- compression level status [R]
		autolevel_base    = 0x3380, autolevel_in_count = 10,  -- autolevel status [R]
		roomeq_write_base = 0x35d0, roomeq_write_end   = 0x3850,  -- 20 ch × 0x20 [W]
		roomeq_read_base  = nil,    roomeq_read_end    = nil,
		mixvol_base       = 0x4000, mixvol_end         = 0x4500, mixvol_stride = 0x40,
		fields            = ucxii_fields_bundle,
	},
	-- Fireface 802  USB  HS
	[0x2a393fcd] = {
		name              = "Fireface 802 (USB)",
		mode              = "USB", ep_out = 12, ep_in = 13, ep_levels = 5,
		ch_stride         = 0x100,
		input_end         = 0x1e00,
		output_base       = 0x1e00, output_end        = 0x3c00,
		playback_base     = nil,    playback_end       = nil,
		channame_base     = nil,    channame_out_offset= nil,
		dynlevel_base     = nil,    dynlevel_in_count  = nil,
		autolevel_base    = nil,    autolevel_in_count = nil,
		roomeq_write_base = nil,    roomeq_write_end   = nil,
		roomeq_read_base  = nil,    roomeq_read_end    = nil,
		mixvol_base       = 0x4040, mixvol_end         = 0x4760, mixvol_stride = 0x40,
		fields            = ff802_fields_bundle,
	},
	-- Fireface 802  CC  HS
	[0x04243fdd] = {
		name              = "Fireface 802 (CC)",
		mode              = "CC",  ep_out = 12, ep_in = 13, ep_levels = 5,
		ch_stride         = 0x100,
		input_end         = 0x1e00,
		output_base       = 0x1e00, output_end        = 0x3c00,
		playback_base     = nil,    playback_end       = nil,
		channame_base     = nil,    channame_out_offset= nil,
		dynlevel_base     = nil,    dynlevel_in_count  = nil,
		autolevel_base    = nil,    autolevel_in_count = nil,
		roomeq_write_base = nil,    roomeq_write_end   = nil,
		roomeq_read_base  = nil,    roomeq_read_end    = nil,
		mixvol_base       = 0x4040, mixvol_end         = 0x4760, mixvol_stride = 0x40,
		fields            = ff802_fields_bundle,
	},
	-- Fireface UCX  USB  HS  (same layout as UCX II)
	[0x2a393fc9] = {
		name              = "Fireface UCX (USB)",
		mode              = "USB", ep_out = 12, ep_in = 13, ep_levels = 5,
		ch_stride         = 0x40,
		input_end         = 0x0500,
		output_base       = 0x0500, output_end        = 0x0a00,
		playback_base     = nil,    playback_end       = nil,
		channame_base     = 0x3200, channame_out_offset= 20,
		dynlevel_base     = 0x3180, dynlevel_in_count  = 10,
		autolevel_base    = 0x3380, autolevel_in_count = 10,
		roomeq_write_base = 0x35d0, roomeq_write_end   = 0x3850,
		roomeq_read_base  = nil,    roomeq_read_end    = nil,
		mixvol_base       = 0x4000, mixvol_end         = 0x4500, mixvol_stride = 0x40,
		fields            = ucxii_fields_bundle,
	},
	-- Fireface UFX II  USB  HS
	-- Inputs stride 0x30, outputs 0x05A0 stride 0x30, playbacks 0x0B40 stride 0x0A.
	-- Room EQ: 30 output channels, same read/write split as UFX III.
	[0x2a393fc4] = {
		name              = "Fireface UFX II (USB)",
		mode              = "USB", ep_out = 12, ep_in = 13, ep_levels = 5,
		ch_stride         = 0x30,
		input_end         = 0x05a0,
		output_base       = 0x05a0, output_end        = 0x0b40,
		playback_base     = 0x0b40, playback_end       = 0x0c1c,  pb_stride = 0x0a,
		channame_base     = nil,    channame_out_offset= nil,  -- complex interleaved layout, not yet decoded
		dynlevel_base     = nil,    dynlevel_in_count  = nil,
		autolevel_base    = nil,    autolevel_in_count = nil,
		roomeq_write_base = 0x30a0, roomeq_write_end   = 0x3460,  -- 30 ch × 0x20 [W]
		roomeq_read_base  = 0x3426, roomeq_read_end    = 0x37e6,  -- 30 ch × 0x20 [R]
		mixvol_base       = 0x4000, mixvol_end         = 0x4d00, mixvol_stride = 0x40,
		fields            = ufxii_fields_bundle,
	},
	-- Fireface UFX II  CC  HS
	[0x2a393fd1] = {
		name              = "Fireface UFX II (CC)",
		mode              = "CC",  ep_out = 12, ep_in = 13, ep_levels = 5,
		ch_stride         = 0x30,
		input_end         = 0x05a0,
		output_base       = 0x05a0, output_end        = 0x0b40,
		playback_base     = 0x0b40, playback_end       = 0x0c1c,  pb_stride = 0x0a,
		channame_base     = nil,    channame_out_offset= nil,
		dynlevel_base     = nil,    dynlevel_in_count  = nil,
		autolevel_base    = nil,    autolevel_in_count = nil,
		roomeq_write_base = 0x30a0, roomeq_write_end   = 0x3460,
		roomeq_read_base  = 0x3426, roomeq_read_end    = 0x37e6,
		mixvol_base       = 0x4000, mixvol_end         = 0x4d00, mixvol_stride = 0x40,
		fields            = ufxii_fields_bundle,
	},
	-- Fireface UFX III  USB  SS
	[0x2a393f83] = {
		name              = "Fireface UFX III (USB)",
		mode              = "USB", ep_out = 5, ep_in = 10, ep_levels = 4,
		ch_stride         = 0x30,
		input_end         = 0x11a0,
		output_base       = 0x11a0, output_end        = 0x2340,
		playback_base     = 0x2340, playback_end       = 0x26ec,
		channame_base     = 0x2800, channame_out_offset= 94,
		mixlabel_base     = 0x2000, mixlabel_stride    = 0x30, mixlabel_end = 0x2500,
		dynlevel_base     = nil,    dynlevel_in_count  = nil,
		autolevel_base    = nil,    autolevel_in_count = nil,
		-- Room EQ: separate read/write ranges; write overlaps global regs — dispatch is direction-aware.
		roomeq_write_base = 0x30a0, roomeq_write_end   = 0x3c60,  -- 94 ch × 0x20 [W]
		roomeq_read_base  = 0x3426, roomeq_read_end    = 0x3fe6,  -- 94 ch × 0x20 [R]
		mixvol_base       = 0x4000, mixvol_end         = 0x5780, mixvol_stride = 0x30,
		fields            = ufxiii_fields,
	},
	-- Fireface UFX III  CC  SS
	[0x2a393fde] = {
		name              = "Fireface UFX III (CC)",
		mode              = "CC",  ep_out = 5, ep_in = 10, ep_levels = 4,
		ch_stride         = 0x30,
		input_end         = 0x11a0,
		output_base       = 0x11a0, output_end        = 0x2340,
		playback_base     = 0x2340, playback_end       = 0x26ec,
		channame_base     = 0x2800, channame_out_offset= 94,
		mixlabel_base     = 0x2000, mixlabel_stride    = 0x30, mixlabel_end = 0x2500,
		dynlevel_base     = nil,    dynlevel_in_count  = nil,
		autolevel_base    = nil,    autolevel_in_count = nil,
		roomeq_write_base = 0x30a0, roomeq_write_end   = 0x3c60,
		roomeq_read_base  = 0x3426, roomeq_read_end    = 0x3fe6,
		mixvol_base       = 0x4000, mixvol_end         = 0x5780, mixvol_stride = 0x30,
		fields            = ufxiii_fields,
	},
	-- Babyface  USB  HS
	[0x2a393fc7] = {
		name              = "Babyface (USB)",
		mode              = "USB", ep_out = 5, ep_in = 10, ep_levels = 4,
		ch_stride         = 0x40,
		input_end         = 0x0500,
		output_base       = 0x0500, output_end        = 0x0a00,
		playback_base     = nil,    playback_end       = nil,
		channame_base     = nil,    channame_out_offset= nil,
		dynlevel_base     = nil,    dynlevel_in_count  = nil,
		autolevel_base    = nil,    autolevel_in_count = nil,
		roomeq_write_base = nil,    roomeq_write_end   = nil,
		roomeq_read_base  = nil,    roomeq_read_end    = nil,
		mixvol_base       = 0x4000, mixvol_end         = 0x4500, mixvol_stride = 0x40,
		fields            = ucxii_fields_bundle,
	},
}
local function format_mixlabel(reg, val, profile)
	local rel  = reg - profile.mixlabel_base
	val = val:le_uint()
	local mix  = math.floor(rel / profile.mixlabel_stride) + 1
	local chan  = rel % profile.mixlabel_stride + 1
	local regdesc = string.format('Mix %d, Input %d Label', mix, chan)
	local pan = bit32.band(val, 0x8000) == 0
	val = bit32.bxor(bit32.band(val, 0x7fff), 0x4000) - 0x4000
	local valdesc
	if pan then
		valdesc = string.format('%.2f dB', val / 10)
		else
		valdesc = string.format('Pan %d', val)
	end
	return regdesc, valdesc
end

local function format_mixvolume(reg, val, profile)
	local rel  = reg - profile.mixvol_base
	local mix  = math.floor(rel / profile.mixvol_stride) + 1
	local chan  = rel % profile.mixvol_stride + 1
	local desc  = bit32.band(chan, 0x20) == 0 and 'Input' or 'Playback'
	chan = bit32.band(chan, 0x1f)
	return string.format('Mix %d, %s %d Volume', mix, desc, chan), format_volume(val)
end



local function format_playbackfx(reg, val)
	local chan, side
	if reg < 0x47e0 then
		chan = (reg - 0x47a0)
		side = 'Left'
		else
		chan = (reg - 0x47e0)
		side = 'Right'
	end
	return string.format('Playback %d FX Send %s', chan, side), format_volume(val)
end



local function format_channame(reg, val, profile)
	local rel  = reg - profile.channame_base
	local chan  = math.floor(rel / 0x08) + 1
	local sub   = rel % 0x08
	val = val:le_uint()
	local ch_type
	if chan > profile.channame_out_offset then
		ch_type = 'Output'
		chan = chan - profile.channame_out_offset
		else
		ch_type = 'Input'
	end
	local regdesc = string.format('%s %d Name[%d:%d]', ch_type, chan, sub * 2, sub * 2 + 1)
	local valdesc = string.char(bit32.band(val, 0xff), bit32.rshift(val, 8))
	return regdesc, valdesc
end

local function format_dynlevel(reg, val, profile)
	local rel      = reg - profile.dynlevel_base
	local ch_type  = rel < profile.dynlevel_in_count and 'Input' or 'Output'
	local idx      = rel < profile.dynlevel_in_count and rel or (rel - profile.dynlevel_in_count)
	return string.format('Dynamics Level %s %d/%d', ch_type, idx * 2, idx * 2 + 1)
end

local function format_autolevel(reg, val, profile)
	local rel      = reg - profile.autolevel_base
	local ch_type  = rel < profile.autolevel_in_count and 'Input' or 'Output'
	local idx      = rel < profile.autolevel_in_count and rel or (rel - profile.autolevel_in_count)
	return string.format('Auto Level %s %d/%d', ch_type, idx * 2, idx * 2 + 1)
end

local function format_roomeq(reg, val, profile)
	-- Determine base from the register address (write and read ranges don't overlap).
	local base
	if profile.roomeq_write_base and reg >= profile.roomeq_write_base and reg < profile.roomeq_write_end then
		base = profile.roomeq_write_base
		elseif profile.roomeq_read_base and reg >= profile.roomeq_read_base and reg < profile.roomeq_read_end then
		base = profile.roomeq_read_base
		else
		return string.format('RoomEQ (unexpected addr %#.4x)', reg)
	end
	local rel     = reg - base
	local ch_idx  = math.floor(rel / 0x20)   -- 0-based channel index
	local sub     = rel % 0x20
	local ch_name = roomeq_ch_names[ch_idx] or string.format('Ch %d', ch_idx + 1)
	local field   = roomeq_fields[sub]
	if field then
		return string.format('RoomEQ %s %s', ch_name, field.name), field.format(val)
		else
		return string.format('RoomEQ %s %#.2x', ch_name, sub)
	end
end


local levels_usb_label = {
	[0x11111111] = 'Input Levels (Post FX)',
	[0x55555555] = 'Input Levels (Pre FX)',
	[0x22222222] = 'Playback Levels',
	[0x33333333] = 'Output Levels (Pre FX)',
	[0x66666666] = 'Output Levels (Post FX)',
}

local function levels_usb(buffer, pinfo, tree)
	local len = buffer:len()
	local catbuf = buffer(len - 4, 4)
	local cat = catbuf:le_uint()
	tree = tree:add(levels_pf, catbuf, cat, nil, levels_usb_label[cat])

	local num_channels = (len - 4) / 12  -- 12 Bytes per channel (8 Peak + 4 RMS)

	for channel = 0, num_channels - 1 do
		local offset = channel * 12
		local channel_tree = tree:add(usb_rme_proto, buffer(offset, 12), "Channel: " .. channel)
		channel_tree:add_le(peak_pf, buffer(offset, 8))
		channel_tree:add_le(rms_pf, buffer(offset + 8, 4))
	end
end

local function levels_cc(buffer, pinfo, tree)
	local len = buffer:len()
	local num_channels = len / 12  -- 12 Bytes per channel (8 Peak + 4 RMS)

	for channel = 0, num_channels - 1 do
		local offset = channel * 12
		local channel_tree = tree:add(usb_rme_proto, buffer(offset, 12), "Channel: " .. channel)
		channel_tree:add_le(peak_pf, buffer(offset, 8))
		channel_tree:add_le(rms_pf, buffer(offset + 8, 4))
	end
end

----------------------------------------------
-- RME USB Proto dissector
----------------------------------------------

function usb_rme_proto.dissector(buffer, pinfo, tree)
	local endpoint = endpoint_pf()
	local subid    = f_subid()

	-- pinfo.match_uint is the VID/PID key Wireshark used to route to this
	-- dissector via the usb.product table — always present, no field lookup needed.
	local vp_id  = pinfo.match_uint
	local profile = vp_id and device_profiles[vp_id]

	local device_label = (profile and profile.name) or "Unknown RME Device"
	local subtree  = tree:add(usb_rme_proto, buffer(), 'RME Protocol Data (' .. device_label .. ')')

	subtree:add(device_name_pf, device_label):set_generated()

	-- Endpoint / subid routing
	if endpoint then
		if profile and endpoint.value == profile.ep_levels then
			return levels_usb(buffer, pinfo, subtree)
			elseif profile and (endpoint.value == profile.ep_in or endpoint.value == profile.ep_out) then
			-- fall through to register dissection below
			else
			return
		end
		elseif subid then
		if subid.value == 0 then
			return
			elseif subid.value >= 1 and subid.value <= 5 then
			return levels_cc(buffer, pinfo, subtree, subid.value)
			else
			return
		end
		else
		return
	end

	-- Only reached for register packets — safe to index buffer now.
	if buffer:len() < 4 then return end

	pinfo.cols.protocol = usb_rme_proto.name

	local dir_prefix = (profile and endpoint and endpoint.value == profile.ep_in) and '[IN] ' or '[OUT] '

	local length = buffer:len()
	local i = 0
	while i + 4 <= length do
		local valbuf = buffer(i, 2)
		local regbuf = buffer(i + 2, 2)
		local val    = valbuf:le_uint()
		local reg    = bit32.band(regbuf:le_uint(), 0x7fff)

		-- Fall back to UFX III profile when device is unresolved so
		-- format functions always receive a valid profile table.
		local p = profile or device_profiles[0x2a393f83]

		-- Room EQ has separate write/read address ranges; select based on
		-- packet direction to avoid shadowing global register entries.
		local roomeq_base, roomeq_end
		if p.roomeq_write_base and endpoint and endpoint.value == p.ep_out then
			roomeq_base = p.roomeq_write_base
			roomeq_end  = p.roomeq_write_end
			elseif p.roomeq_read_base and endpoint and endpoint.value == p.ep_in then
			roomeq_base = p.roomeq_read_base
			roomeq_end  = p.roomeq_read_end
		end

		local format_fn
		if     reg < p.input_end                                                        then format_fn = format_input
			elseif reg < p.output_end                                                       then format_fn = format_output
			elseif p.playback_end  and reg < p.playback_end                                 then format_fn = format_playback
			elseif p.mixlabel_base and reg >= p.mixlabel_base  and reg < p.mixlabel_end     then format_fn = format_mixlabel
			elseif p.channame_base and reg >= p.channame_base  and reg < p.channame_base + 0x5e0 then format_fn = format_channame
			elseif p.dynlevel_base and reg >= p.dynlevel_base  and reg < p.dynlevel_base  + 0x20 then format_fn = format_dynlevel
			elseif p.autolevel_base and reg >= p.autolevel_base and reg < p.autolevel_base + 0x20 then format_fn = format_autolevel
			elseif roomeq_base     and reg >= roomeq_base      and reg < roomeq_end         then format_fn = format_roomeq
			elseif reg >= p.mixvol_base and reg < p.mixvol_end                              then format_fn = format_mixvolume
			else                                                                                  format_fn = format_global
		end

		local regdesc, valdesc
		if format_fn then
			regdesc, valdesc = format_fn(reg, valbuf, p)
		end

		local reg_label
		if regdesc then
			reg_label = string.format('%s0x%04x  [%s]', dir_prefix, reg, regdesc)
			else
			reg_label = string.format('%s0x%04x', dir_prefix, reg)
		end

		local subsubtree = subtree:add(usb_rme_proto, buffer(i, 4), reg_label)

		local reg_leaf = subsubtree:add_le(usb_rme_proto.fields.register, regbuf, reg)
		if regdesc then
			reg_leaf:add_proto_expert_info(exp_reg_desc, string.format('[%s]', regdesc))
		end

		local val_leaf = subsubtree:add_le(usb_rme_proto.fields.value, valbuf, val)
		if valdesc then
			val_leaf:set_text(string.format('Value: 0x%04x  (%s)', val, valdesc))
			val_leaf:add_proto_expert_info(exp_val_desc, string.format('(%s)', valdesc))
		end

		i = i + 4
	end
end


----------------------------------------------
-- Register all known VID/PID combos from device_profiles
----------------------------------------------

local usb_table = DissectorTable.get('usb.product')
for vp_id, _ in pairs(device_profiles) do
	usb_table:add(vp_id, usb_rme_proto)
end


----------------------------------------------
-- RME SysEx Proto (for units in CC mode)
----------------------------------------------

local sysex_rme_proto = Proto('sysex_rme', 'MIDI System Exclusive RME')
local devid_pf = ProtoField.uint8('sysex_rme.devid', 'Device ID', base.HEX)
local subid_pf = ProtoField.uint8('sysex_rme.subid', 'Sub ID', base.HEX)
sysex_rme_proto.fields = {devid_pf, subid_pf}

f_subid = Field.new('sysex_rme.subid')

local function sysex_decode(input)
	local output = ByteArray.new()
	output:set_size(math.floor((input:len() * 7) / 8))
	local byte = 0
	local j = 0
	for i = 0, input:len() - 1 do
		byte = bit32.bor(bit32.lshift(input:get_index(i), -i % 8), byte)
		if i % 8 ~= 0 then
			output:set_index(j, bit32.band(byte, 0xff))
			byte = bit32.rshift(byte, 8)
			j = j + 1
		end
	end
	return output
end


----------------------------------------------
-- RME SysEx dissector (for units in CC mode)
----------------------------------------------

function sysex_rme_proto.dissector(buffer, pinfo, tree)
	pinfo.cols.protocol = sysex_rme_proto.name
	local subtree = tree:add(sysex_rme_proto, buffer(), 'RME SysEx Protocol')
	local subid = buffer(1, 1)
	subtree:add(devid_pf, buffer(0, 1))
	subtree:add(subid_pf, subid)
	buffer = buffer(2)

	local decoded_body = ByteArray.new()
	for i = 0, buffer:len() - 5, 5 do
		decoded_body = decoded_body..sysex_decode(buffer(i, 5):bytes())
	end

	buffer = decoded_body:tvb()
	if subid:le_uint() == 0 then
		usb_rme_proto.dissector(buffer, pinfo, tree)
		else
		rme_levels_proto.dissector(buffer, pinfo, tree)
	end
end

local sysex_table = DissectorTable.get('sysex.manufacturer')
if sysex_table then
	sysex_table:add(0x00200d, sysex_rme_proto)
end
