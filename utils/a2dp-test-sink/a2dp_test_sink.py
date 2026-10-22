#!/usr/bin/env python3
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Copyright (C) 2026 Marcin Bukat
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.

"""A2DP test sink: a Linux PC playing a Bluetooth headset with the
capabilities you give it, to see what a source (Rockbox) makes of them.

Registers A2DP sink endpoints with BlueZ for the codecs asked for, with
the sample rates and SBC bitpool range asked for, and reports what the
source configures and what it sends: per second the bitrate, packets,
frames per packet, the SBC bitpool of every frame (the dynamic bitpool),
gaps between packets and lost packets, and the AVRCP volume. The stream
can be recorded and turned into a WAV file with ffmpeg (SBC, aptX,
aptX HD; LDAC has no free decoder).

Needs dbus-python and PyGObject. PipeWire/WirePlumber must not run their
own A2DP sink at the same time, see README.
"""

import argparse
import os
import socket
import struct
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BLUEZ = "org.bluez"
A2DP_SINK_UUID = "0000110b-0000-1000-8000-00805f9b34fb"
ENDPOINT_IFACE = "org.bluez.MediaEndpoint1"
TRANSPORT_IFACE = "org.bluez.MediaTransport1"
PROPS_IFACE = "org.freedesktop.DBus.Properties"
AGENT_IFACE = "org.bluez.Agent1"

CODEC_SBC = 0x00
CODEC_VENDOR = 0xFF

# Vendor codecs: (company ID, vendor codec ID)
APTX = (0x004F, 0x0001)
APTX_HD = (0x00D7, 0x0024)
LDAC = (0x012D, 0x00AA)

# Sampling frequency bits per codec
SBC_RATES = {16000: 0x80, 32000: 0x40, 44100: 0x20, 48000: 0x10}
APTX_RATES = {16000: 0x80, 32000: 0x40, 44100: 0x20, 48000: 0x10}
LDAC_RATES = {44100: 0x20, 48000: 0x10, 88200: 0x08, 96000: 0x04,
              176400: 0x02, 192000: 0x01}

SBC_MODES = {"mono": 0x08, "dual": 0x04, "stereo": 0x02, "joint": 0x01}
SBC_BLOCKS = {4: 0x80, 8: 0x40, 12: 0x20, 16: 0x10}
SBC_SUBBANDS = {4: 0x08, 8: 0x04}
SBC_ALLOC = {"snr": 0x02, "loudness": 0x01}


def rate_name(rate):
    return "%g kHz" % (rate / 1000)


def bits_to_names(value, table):
    return [name for name, bit in table.items() if value & bit]


def vendor_header(vendor):
    company, codec = vendor
    return struct.pack("<IH", company, codec)


def vendor_of(caps):
    if len(caps) < 6:
        return None
    return struct.unpack("<IH", bytes(caps[:6]))


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


# --------------------------------------------------------------------------
# Codecs: the capabilities we advertise, and how to read a configuration

class Codec:
    name = "?"
    codec_id = CODEC_SBC
    vendor = None
    rtp = True              # media packets start with an RTP header
    ext = "raw"

    def __init__(self, args):
        self.args = args

    def capabilities(self):
        raise NotImplementedError

    def describe(self, config):
        return bytes(config).hex()

    def rate(self, config):
        return None

    def select(self, caps):
        """Pick a configuration when we connect to the source ourselves"""
        return caps


class Sbc(Codec):
    name = "SBC"
    ext = "sbc"

    def capabilities(self):
        rates = sum(SBC_RATES[r] for r in self.args.rates if r in SBC_RATES)
        modes = sum(SBC_MODES[m] for m in self.args.sbc_modes)
        return [rates | modes,
                sum(SBC_BLOCKS.values()) | sum(SBC_SUBBANDS.values()) |
                sum(SBC_ALLOC.values()),
                self.args.sbc_bitpool[0], self.args.sbc_bitpool[1]]

    def rate(self, config):
        for rate, bit in SBC_RATES.items():
            if config[0] & bit:
                return rate
        return None

    def describe(self, config):
        blocks = bits_to_names(config[1], SBC_BLOCKS)
        subbands = bits_to_names(config[1], SBC_SUBBANDS)
        return "%s %s, %s blocks, %s subbands, %s, bitpool %d-%d" % (
            "/".join(rate_name(r) for r in [self.rate(config)] if r),
            "/".join(bits_to_names(config[0] & 0x0F, SBC_MODES)),
            "/".join(map(str, blocks)), "/".join(map(str, subbands)),
            "/".join(bits_to_names(config[1], SBC_ALLOC)),
            config[2], config[3])

    def select(self, caps):
        def best(value, table, order):
            for key in order:
                if value & table[key]:
                    return table[key]
            raise dbus.exceptions.DBusException(
                "no common configuration",
                name="org.bluez.Error.InvalidArguments")

        ours = self.capabilities()
        freq = best(caps[0] & ours[0], SBC_RATES, [44100, 48000, 32000, 16000])
        mode = best(caps[0] & ours[0], SBC_MODES,
                    ["joint", "stereo", "dual", "mono"])
        blocks = best(caps[1], SBC_BLOCKS, [16, 12, 8, 4])
        subbands = best(caps[1], SBC_SUBBANDS, [8, 4])
        alloc = best(caps[1], SBC_ALLOC, ["loudness", "snr"])
        lo = max(caps[2], ours[2])
        hi = min(caps[3], ours[3])
        return [freq | mode, blocks | subbands | alloc, lo, hi]


class Aptx(Codec):
    name = "aptX"
    codec_id = CODEC_VENDOR
    vendor = APTX
    rtp = False             # aptX packets are the bare stream
    ext = "aptx"

    def capabilities(self):
        rates = sum(APTX_RATES[r] for r in self.args.rates
                    if r in APTX_RATES)
        return list(vendor_header(self.vendor)) + [rates | 0x02]

    def rate(self, config):
        for rate, bit in APTX_RATES.items():
            if config[6] & bit:
                return rate
        return None

    def describe(self, config):
        stereo = "stereo" if config[6] & 0x02 else "mono"
        return "%s %s" % (rate_name(self.rate(config) or 0), stereo)

    def select(self, caps):
        for rate in (44100, 48000):
            if caps[6] & APTX_RATES[rate]:
                return list(caps[:6]) + [APTX_RATES[rate] | 0x02] + \
                    list(caps[7:])
        return caps


class AptxHd(Aptx):
    name = "aptX HD"
    vendor = APTX_HD
    rtp = True
    ext = "aptxhd"

    def capabilities(self):
        return super().capabilities() + [0, 0, 0, 0]


class Ldac(Codec):
    name = "LDAC"
    codec_id = CODEC_VENDOR
    vendor = LDAC
    ext = "ldac"

    def capabilities(self):
        rates = sum(LDAC_RATES[r] for r in self.args.rates
                    if r in LDAC_RATES)
        # stereo only, as the encoders here are set up for
        return list(vendor_header(self.vendor)) + [rates, 0x01]

    def rate(self, config):
        for rate, bit in LDAC_RATES.items():
            if config[6] & bit:
                return rate
        return None

    def describe(self, config):
        modes = {0x04: "mono", 0x02: "dual", 0x01: "stereo"}
        return "%s %s" % (rate_name(self.rate(config) or 0),
                          modes.get(config[7], hex(config[7])))

    def select(self, caps):
        for rate in (44100, 48000, 88200, 96000):
            if caps[6] & LDAC_RATES[rate]:
                return list(caps[:6]) + [LDAC_RATES[rate], 0x01]
        return caps


CODECS = {"sbc": Sbc, "aptx": Aptx, "aptxhd": AptxHd, "ldac": Ldac}


# --------------------------------------------------------------------------
# Stream statistics

def sbc_frame_length(hdr):
    """Length of the SBC frame starting with hdr (4 bytes), None if it
    isn't one"""
    if len(hdr) < 4 or hdr[0] != 0x9C:
        return None
    blocks = (4, 8, 12, 16)[(hdr[1] >> 4) & 3]
    mode = (hdr[1] >> 2) & 3            # mono, dual, stereo, joint
    subbands = 8 if hdr[1] & 1 else 4
    channels = 1 if mode == 0 else 2
    bitpool = hdr[2]
    length = 4 + (4 * subbands * channels) // 8
    if mode in (0, 1):
        length += (blocks * channels * bitpool + 7) // 8
    elif mode == 2:
        length += (blocks * bitpool + 7) // 8
    else:
        length += (subbands + blocks * bitpool + 7) // 8
    return length


class Stats:
    def __init__(self):
        self.reset()
        self.total_lost = 0
        self.last_seq = None

    def reset(self):
        self.start = time.monotonic()
        self.packets = 0
        self.bytes = 0
        self.frames = 0
        self.bitpools = []
        self.max_gap = 0.0
        self.lost = 0
        self.last_rx = None

    def packet(self, size, frames, bitpools, seq):
        now = time.monotonic()
        if self.last_rx is not None:
            self.max_gap = max(self.max_gap, now - self.last_rx)
        self.last_rx = now
        self.packets += 1
        self.bytes += size
        self.frames += frames
        self.bitpools.extend(bitpools)
        if seq is not None:
            if self.last_seq is not None:
                missed = (seq - self.last_seq - 1) & 0xFFFF
                if missed < 0x8000:
                    self.lost += missed
                    self.total_lost += missed
            self.last_seq = seq

    def line(self):
        secs = max(time.monotonic() - self.start, 1e-3)
        text = "%4.0f kbit/s %3d pkt/s" % (self.bytes * 8 / secs / 1000,
                                           self.packets / secs)
        if self.packets:
            text += " %4.1f fr/pkt" % (self.frames / self.packets)
        if self.bitpools:
            text += " bitpool %d (%d-%d)" % (
                self.bitpools[-1], min(self.bitpools), max(self.bitpools))
        text += " gap %3.0f ms lost %d/%d" % (self.max_gap * 1000, self.lost,
                                              self.total_lost)
        return text


# --------------------------------------------------------------------------
# Transport: acquire the stream when the source starts it and read it

class Transport:
    def __init__(self, sink, path, codec, config):
        self.sink = sink
        self.path = path
        self.codec = codec
        self.config = config
        self.obj = sink.bus.get_object(BLUEZ, path)
        self.sock = None
        self.watch = None
        self.mtu = 0
        self.stats = Stats()
        self.record = None
        self.volume = None
        self.signal = sink.bus.add_signal_receiver(
            self.properties_changed, "PropertiesChanged", PROPS_IFACE,
            BLUEZ, path)
        props = self.obj.GetAll(TRANSPORT_IFACE, dbus_interface=PROPS_IFACE)
        self.volume = props.get("Volume")
        if self.volume is not None:
            log("volume %d" % self.volume)
        if props.get("State") == "pending":
            self.acquire()

    def properties_changed(self, iface, changed, invalidated):
        if iface != TRANSPORT_IFACE:
            return
        if "Volume" in changed:
            self.volume = int(changed["Volume"])
            log("volume %d (%d%%)" % (self.volume, self.volume * 100 // 127))
        if "State" in changed:
            state = str(changed["State"])
            log("transport %s" % state)
            if state == "pending":
                self.acquire()
            elif state == "idle":
                self.close()

    def acquire(self):
        if self.sock:
            return
        try:
            fd, self.mtu, _ = self.obj.TryAcquire(
                dbus_interface=TRANSPORT_IFACE)
        except dbus.exceptions.DBusException as e:
            log("acquire failed: %s" % e.get_dbus_message())
            return
        self.sock = socket.socket(fileno=fd.take())
        self.sock.setblocking(False)
        log("streaming, read MTU %d" % self.mtu)
        self.stats = Stats()
        if self.sink.args.record:
            name = "%s-%s.%s" % (self.sink.args.record,
                                 time.strftime("%Y%m%d-%H%M%S"),
                                 self.codec.ext)
            self.record = open(name, "wb")
            log("recording to %s" % name)
        self.watch = GLib.io_add_watch(
            self.sock.fileno(), GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR,
            self.readable)

    def readable(self, fd, cond):
        if cond & (GLib.IO_HUP | GLib.IO_ERR):
            self.close()
            return False
        while True:
            try:
                data = self.sock.recv(self.mtu or 4096)
            except BlockingIOError:
                return True
            except OSError as e:
                log("read error: %s" % e)
                self.close()
                return False
            if not data:
                self.close()
                return False
            self.packet(data)

    def packet(self, data):
        seq = None
        payload = data
        if self.codec.rtp:
            if len(data) < 12:
                return
            cc = data[0] & 0x0F
            seq = struct.unpack(">H", data[2:4])[0]
            hdr_len = 12 + 4 * cc
            if data[0] & 0x10 and len(data) >= hdr_len + 4:
                hdr_len += 4 + 4 * struct.unpack(
                    ">H", data[hdr_len + 2:hdr_len + 4])[0]
            payload = data[hdr_len:]

        frames = 0
        bitpools = []
        if isinstance(self.codec, Sbc) and payload:
            frames = payload[0] & 0x0F
            body = payload[1:]
            pos = 0
            while pos < len(body):
                length = sbc_frame_length(body[pos:pos + 4])
                if not length:
                    break
                bitpools.append(body[pos + 2])
                pos += length
            if self.record:
                self.record.write(body)
        elif isinstance(self.codec, Ldac) and payload:
            frames = payload[0] & 0x0F
            if self.record:
                self.record.write(payload[1:])
        else:
            frames = 1
            if self.record:
                self.record.write(payload)

        self.stats.packet(len(data), frames, bitpools, seq)

    def report(self):
        if self.sock:
            log(self.stats.line())
            self.stats.reset()

    def set_volume(self, volume):
        try:
            self.obj.Set(TRANSPORT_IFACE, "Volume", dbus.UInt16(volume),
                         dbus_interface=PROPS_IFACE)
        except dbus.exceptions.DBusException as e:
            log("setting the volume failed: %s" % e.get_dbus_message())

    def close(self):
        if self.watch:
            GLib.source_remove(self.watch)
            self.watch = None
        if self.sock:
            log("stream stopped, lost %d packets in all" %
                self.stats.total_lost)
            self.sock.close()
            self.sock = None
            try:
                self.obj.Release(dbus_interface=TRANSPORT_IFACE)
            except dbus.exceptions.DBusException:
                pass
        if self.record:
            self.record.close()
            convert = {
                "sbc": "ffmpeg -f sbc -i %s %s.wav",
                "aptx": "ffmpeg -f aptx -sample_rate %d -i %%s %%s.wav",
                "aptxhd": "ffmpeg -f aptx_hd -sample_rate %d -i %%s %%s.wav",
            }.get(self.codec.ext)
            if convert:
                if "%d" in convert:
                    convert = convert % (self.codec.rate(self.config) or 44100)
                log("to listen: " + convert % (self.record.name,
                                                self.record.name))
            self.record = None

    def remove(self):
        self.close()
        self.signal.remove()


# --------------------------------------------------------------------------
# Endpoint, one per codec

class Endpoint(dbus.service.Object):
    def __init__(self, sink, codec, index):
        self.sink = sink
        self.codec = codec
        self.path = "/org/rockbox/a2dp_test_sink/ep%d" % index
        super().__init__(sink.bus, self.path)

    def properties(self):
        props = {
            "UUID": A2DP_SINK_UUID,
            "Codec": dbus.Byte(self.codec.codec_id),
            "Capabilities": dbus.Array(self.codec.capabilities(),
                                       signature="y"),
        }
        if self.codec.vendor:
            company, codec = self.codec.vendor
            props["Vendor"] = dbus.UInt32(company << 16 | codec)
        return props

    @dbus.service.method(ENDPOINT_IFACE, in_signature="oa{sv}",
                         out_signature="")
    def SetConfiguration(self, transport, props):
        config = [int(b) for b in props.get("Configuration", [])]
        device = props.get("Device", "")
        log("%s configured by %s: %s" % (self.codec.name,
                                         str(device).split("/")[-1],
                                         self.codec.describe(config)))
        self.sink.transport_added(transport, self.codec, config)

    @dbus.service.method(ENDPOINT_IFACE, in_signature="ay",
                         out_signature="ay")
    def SelectConfiguration(self, caps):
        config = self.codec.select([int(b) for b in caps])
        log("%s selected: %s" % (self.codec.name, self.codec.describe(config)))
        return dbus.Array(config, signature="y")

    @dbus.service.method(ENDPOINT_IFACE, in_signature="o", out_signature="")
    def ClearConfiguration(self, transport):
        log("%s configuration cleared" % self.codec.name)
        self.sink.transport_removed(transport)

    @dbus.service.method(ENDPOINT_IFACE, in_signature="", out_signature="")
    def Release(self):
        log("%s endpoint released by BlueZ" % self.codec.name)


# --------------------------------------------------------------------------
# Pairing agent that says yes to everything

class Agent(dbus.service.Object):
    PATH = "/org/rockbox/a2dp_test_sink/agent"

    def __init__(self, bus):
        super().__init__(bus, self.PATH)

    @dbus.service.method(AGENT_IFACE, in_signature="", out_signature="")
    def Release(self):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        log("pairing with %s, passkey %06d, accepted" %
            (str(device).split("/")[-1], passkey))

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="")
    def RequestAuthorization(self, device):
        log("pairing with %s accepted" % str(device).split("/")[-1])

    @dbus.service.method(AGENT_IFACE, in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        return "0000"

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="u")
    def RequestPasskey(self, device):
        return dbus.UInt32(0)

    @dbus.service.method(AGENT_IFACE, in_signature="ouq", out_signature="")
    def DisplayPasskey(self, device, passkey, entered):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="os", out_signature="")
    def DisplayPinCode(self, device, pincode):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="", out_signature="")
    def Cancel(self):
        log("pairing cancelled")


# --------------------------------------------------------------------------

class TestSink:
    def __init__(self, args):
        self.args = args
        self.bus = dbus.SystemBus()
        self.adapter_path = "/org/bluez/" + args.adapter
        self.media = dbus.Interface(
            self.bus.get_object(BLUEZ, self.adapter_path), "org.bluez.Media1")
        self.endpoints = []
        self.transports = {}
        self.agent = None
        self.loop = GLib.MainLoop()

    def start(self):
        for i, name in enumerate(self.args.codecs):
            ep = Endpoint(self, CODECS[name](self.args), i)
            self.media.RegisterEndpoint(ep.path, ep.properties())
            log("%s sink endpoint: %s" % (
                ep.codec.name, bytes(ep.codec.capabilities()).hex()))
            self.endpoints.append(ep)

        if self.args.agent:
            self.agent = Agent(self.bus)
            manager = dbus.Interface(self.bus.get_object(BLUEZ, "/org/bluez"),
                                     "org.bluez.AgentManager1")
            manager.RegisterAgent(Agent.PATH, "NoInputNoOutput")
            manager.RequestDefaultAgent(Agent.PATH)
            log("pairing agent registered")

        if self.args.discoverable:
            adapter = self.bus.get_object(BLUEZ, self.adapter_path)
            for prop in ("Discoverable", "Pairable"):
                adapter.Set("org.bluez.Adapter1", prop, dbus.Boolean(True),
                            dbus_interface=PROPS_IFACE)
            log("discoverable and pairable")

        GLib.timeout_add_seconds(self.args.interval, self.report)
        GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN, self.command)
        print("commands: v <0-127> set volume as the sink's buttons would, "
              "q quit", flush=True)

    def transport_added(self, path, codec, config):
        self.transports[str(path)] = Transport(self, str(path), codec, config)

    def transport_removed(self, path):
        t = self.transports.pop(str(path), None)
        if t:
            t.remove()

    def report(self):
        for t in self.transports.values():
            t.report()
        return True

    def command(self, fd, cond):
        line = sys.stdin.readline()
        if not line:
            return False
        words = line.split()
        if not words:
            return True
        if words[0] == "q":
            self.loop.quit()
        elif words[0] == "v" and len(words) == 2 and words[1].isdigit():
            for t in self.transports.values():
                t.set_volume(min(int(words[1]), 127))
        else:
            print("commands: v <0-127>, q", flush=True)
        return True

    def stop(self):
        for t in list(self.transports.values()):
            t.remove()
        for ep in self.endpoints:
            try:
                self.media.UnregisterEndpoint(ep.path)
            except dbus.exceptions.DBusException:
                pass
        if self.agent:
            try:
                dbus.Interface(self.bus.get_object(BLUEZ, "/org/bluez"),
                               "org.bluez.AgentManager1").UnregisterAgent(
                                   Agent.PATH)
            except dbus.exceptions.DBusException:
                pass

    def run(self):
        self.start()
        if self.args.duration:
            GLib.timeout_add_seconds(self.args.duration, self.loop.quit)
        try:
            self.loop.run()
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()


def parse_rates(text):
    rates = []
    for item in text.split(","):
        rate = int(round(float(item) * 1000)) if float(item) < 1000 \
            else int(item)
        if rate == 88000:
            rate = 88200
        elif rate == 176000:
            rate = 176400
        elif rate == 44000:
            rate = 44100
        rates.append(rate)
    return rates


def parse_bitpool(text):
    lo, _, hi = text.partition("-")
    lo, hi = int(lo), int(hi or lo)
    if not 2 <= lo <= hi <= 250:
        raise argparse.ArgumentTypeError("bitpool range must be within 2-250")
    return lo, hi


def wireplumber_running():
    """Whether our own WirePlumber runs, not another user's (the login
    screen's)"""
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            if os.stat("/proc/" + pid).st_uid != os.getuid():
                continue
            with open("/proc/%s/comm" % pid) as f:
                if f.read().strip() == "wireplumber":
                    return True
        except OSError:
            pass
    return False


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0].replace("\n", " "),
        epilog="Example: %(prog)s --codecs sbc,aptx --rates 44.1 "
               "--sbc-bitpool 2-35 --agent --discoverable --record /tmp/rb")
    parser.add_argument("--codecs", default="sbc",
                        help="endpoints to offer, in order: sbc, aptx, "
                             "aptxhd, ldac (default sbc)")
    parser.add_argument("--rates", type=parse_rates, default=[44100, 48000],
                        help="sample rates in kHz for all codecs, "
                             "e.g. 44.1,48 (default)")
    parser.add_argument("--sbc-bitpool", type=parse_bitpool, default=(2, 53),
                        metavar="MIN-MAX",
                        help="SBC bitpool range (default 2-53)")
    parser.add_argument("--sbc-modes", default="mono,dual,stereo,joint",
                        help="SBC channel modes (default all)")
    parser.add_argument("--adapter", default="hci0")
    parser.add_argument("--agent", action="store_true",
                        help="register a pairing agent accepting everything")
    parser.add_argument("--discoverable", action="store_true",
                        help="make the adapter discoverable and pairable")
    parser.add_argument("--record", metavar="PREFIX",
                        help="save each stream to PREFIX-<time>.<codec>")
    parser.add_argument("--interval", type=int, default=1,
                        help="seconds between stream reports (default 1)")
    parser.add_argument("--duration", type=int, default=0,
                        help="quit after this many seconds")
    parser.add_argument("--force", action="store_true",
                        help="run even if WirePlumber is running")
    args = parser.parse_args()

    args.codecs = [c.strip() for c in args.codecs.split(",")]
    for c in args.codecs:
        if c not in CODECS:
            parser.error("unknown codec %s" % c)
    args.sbc_modes = [m.strip() for m in args.sbc_modes.split(",")]
    for m in args.sbc_modes:
        if m not in SBC_MODES:
            parser.error("unknown SBC mode %s" % m)
    for r in args.rates:
        if r not in SBC_RATES and r not in LDAC_RATES:
            parser.error("unsupported rate %d" % r)
    for c in args.codecs:
        table = LDAC_RATES if c == "ldac" else SBC_RATES
        if not any(r in table for r in args.rates):
            parser.error("none of the rates is possible for %s" % c)

    if wireplumber_running() and not args.force:
        print("WirePlumber is running and registers its own A2DP sink, the "
              "source may pick that one.\nUse run_test_sink.sh, which stops "
              "it while testing, or pass --force.", file=sys.stderr)
        sys.exit(1)

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    TestSink(args).run()


if __name__ == "__main__":
    main()
