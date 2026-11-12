# Roadmap

Bluetooth on the Surfans F20 (`erosqnative` hw4). Sizes are estimates. How it is
built and verified is in Background.

First test on hw4: Bluetooth works well overall. Readings before 2026-11-03
are on a Bose QuietComfort over SBC High unless stated; that day's listening
was on "T33" true-wireless earbuds (log 6, SBC) and an aptX headset (log 7).

## Faults

- **Audio cuts out briefly, worse with distance.** Open; fix size unknown. The
  sound cuts out for an instant, easiest to hear in loud passages, less often
  beside the headphones and more with distance, so the radio link is the main
  factor. Driving `BT_PWR` (Fixed) made it much better but not gone.
  - **2026-11-03 report, before the `BT_PWR` fix:** clean playback only with the
    player at upper chest height or above, and not always; an arm's length from
    the head was enough to cut out. Same on LDAC and on MP3, so not the codec or
    the file; FLACs above about 1000 kbps seemed worse (a hunch). Seemed to
    improve after some minutes on. With true-wireless buds one bud dropped alone:
    the primary bud's relay starved while it retransmits to us. FLACs sometimes
    played "tinny and echoing", like a phone falling to call audio: SBC at a low
    bitpool, since the bitpool falls to the floor whenever the chip backs up (log
    4: 30–42 at `rssi` 0). `rssi` -7 to -17 (dB below the golden range, about -65
    to -75 dBm) at a few metres was 20–30 dB short of a normal link.
  - **Log 6, 2026-11-03, T33 earbuds, after the `BT_PWR` fix and the page scan
    change** (`local/bt_log6.txt`): a 44.1 kHz FLAC beside the player at `rssi` -5 to
    -30, then at 1.5 m at -15 to -32, no audible fault; then a 48 kHz 1.7 Mbps
    FLAC that skipped beside the player at -8 to -22. The numbers don't show the
    improvement the owner hears, so `rssi` isn't a usable gauge of it: log 4 read
    0 beside the headphones before the fix. The 48 kHz track streams at bitpool
    48, about 75 packets a second against 69 at 44.1 kHz; during it `tick late`
    went 0 → 26 with `tick max` 41 ms, and `slow` (UART sends over 4 ms, the chip
    holding CTS while its buffers are full) ran up to 36 a second of about 70
    sends; `qdrop` 0 and `underflow` 0 during play, so the loss is on the air
    again. `ptype ee1c` with `edr_2mbps: 0` and `mtu 666`: no 3 Mbps packets
    on this link (`btm_acl.c` excludes 3-DH when either side lacks the feature;
    log 7's headset shows the same, so the chip is the suspect, see the `lmp`
    line below), so 2-DH5 is the ceiling and the 2 Mbps decision is moot.
    `codec SBC High, want LDAC` is the device's Codec option set to LDAC, which
    this headset can't do.
  - **Playback Frequency at 44.1 kHz** was then tried against the 48 kHz
    stream: "a big improvement" (by ear, no log yet), so the 48 kHz stream's
    packet rate was the difference, now the default (Fixed, Stream at 44.1 kHz).
  - **Log 7, 2026-11-03, an aptX headset, no `bt_tune.txt`**
    (`local/bt_log7.txt`, 55 s of a 44.1 kHz 1020 kbps FLAC): `codec aptX` (the
    option was LDAC), `enc load` 11.2%, **audio good the entire time**, `rssi`
    0 nearly throughout where the T33 earbuds read -5 to -30 at the same kind of
    distance, so those numbers were the earbuds' weak signal at the player,
    not the player's receiver. The link still
    stalled at `rssi` 0: `w>=100` 14 and `wait max` 294 ms (the chip holding
    packets over 100 ms), `tx max` 26 ms, `slow` about 5 a second, `acl out`
    max 6, one role change and two mode changes; `qdrop` 0, `underflow` 0.
    `ptype ee1c` again: a second headset without 3 Mbps packets is unlikely, so
    the exclusion may be the chip's own (`btm_devctl.c` masks 3-DH when the
    local features lack it); the debug screen and log now carry an `lmp` line
    with the chip's LMP features page 0 and its 2M/3M bits, unread so far. The
    aptX tick runs every 15 ms and fired in pairs every 30 ms (`gap_ms` 28 then
    1, `tick late` 1782): Rockbox's `sleep(n)` blocks for n + 1 ticks, which
    the codec task now allows for (Fixed); harmless to the PCM buffer (96%),
    burstier for the chip.
  - **Log 8, 2026-11-03, the aptX headset, the tick fix and the `lmp` line**
    (`local/bt_log8.txt`, a 48 kHz 1735 kbps FLAC streamed at 44.1 kHz): `lmp
    bffecffedbff7b87 2M 1 3M 1`, so the chip has 3 Mbps packets and the
    exclusion is the headsets' (both of 2026-11-03's lack them); `tick late` 30
    (log 7: 1782), gaps 10 or 20 ms as designed; and the link held packets far
    less: `w>=100` 0 (was 14), `wait max` 63 ms (was 294), `tx >4ms` 23 (was
    246), `acl out` max 5, no drops or underflow, `rssi` 0. The option was aptX
    HD, which this headset lacks, so aptX again at `enc load` 11.2%.
  - **Log 9, 2026-11-03, the aptX headset: clicks in one FLAC** are the file.
    They are there over SBC and over the headphone jack too. The log is clean
    (no drops, no underflow, `rssi` 0) apart from more UART stalls than log 8
    (`tx >4ms` 643, worst 33 ms, `acl out` max 7), which is worth a look if a
    clean file ever clicks.
  - **Still open to try:** the bitpool adaptation steps (`bitpool_backlog`,
    `bitpool_down_step`, `bitpool_up_ms`), set for a link losing 9% of packets,
    against a log on the current defaults; stock firmware range on the same unit
    and headphones, which says whether anything is left to gain. Dropped on the
    owner's call, 2026-11-03: SBC Standard per device (an option the owner can
    pick without a change), a longer flush timeout, and an AFH host
    classification.
  - **Defaults (Decisions):** 2 Mbps packets off, bitpool floor 30, backlog 5, no
    flush timeout, the chip's sleep mode off. A bitpool cut refits frames per packet and
    waits for a full packet (simulated: 48 kHz, 62.5 packets a second at bitpool
    48, 41.6 at 30, 31.2 at 20; nothing lost), and a streaming link can use 7 of
    the chip's 8 ACL buffers (was 5).
  - **Readings on the weak link, before the `BT_PWR` fix:**
    - *Drops at distance, 2 Mbps packets:* tried as Android has done for A2DP,
      with a 663-byte MTU. On the 96 kHz FLAC (48 kHz stream), when `rssi` fell to
      -7 to -17 the queue climbed to 28 within about 5 s and stayed there: `qdrop`
      108, about 6 a second for 19 s, roughly 9% of packets at about 16 ms of audio
      each, at bitpool 30 with `acl out` 5. A player-side cause of cuts. With floor
      20 the owner again saw `qdrop` rising steadily, and bitpool falling, on this
      FLAC. It ran about 70 s with `qdrop` 0 on 3 Mbps packets with an 882-byte
      MTU, though signal strength may differ.
    - *Cuts with no trace, close range:* at `rssi` 0 with floor 20 and 2 Mbps
      packets, the owner reported it worse, with a stretch of 5 or more skips. The
      only `qdrop`s (9) are at track changes, bitpool stayed 53, `q max` 2, `acl
      out` max 7, no underflow during play, `wait100` only at stream starts. These
      cuts leave no trace on the player: packets reach the chip and are confirmed,
      so the loss is on the air or in the headphones. Untested suspect: this link
      can't reliably carry 328 kbps at about 69 packets a second; an earlier build
      at bitpool 30–42 (`bp down` 37, on the 44.1 kHz FLAC at `rssi` near 0, no
      drops) was found less bad.
    - *Packet rate:* before the refit a bitpool cut didn't reduce packets per
      second: at 48 kHz, 6 frames each, 62.5 a second at bitpool 48 and at 30,
      though 30 could fit 9 frames, 42 a second. The 663-byte MTU raised the rate
      from 47 a second at 882 bytes.
    - *Chip timing:* the encoder's pacing follows elapsed time and PCM never ran
      short, so audio leaves the player on time. `acl out` reaching 5–6 packets at
      20 ms each means the chip sometimes holds packets for 100–120 ms, which could
      outlast the headphones' buffer. In one log `wait50` ran about 9 a second for
      12 s after resuming and about 3 a second later, with two bitpool cuts to 47,
      `q max` 2 and no underflow during play. `uart err` has stayed 0 and `enc
      load` about 2%.
    - *Unconfirmed:* one song didn't skip after a reboot, no log; the Codec option
      is read at the next connection, so the reboot may have put SBC Standard into
      effect, and a fresh start alone clearing it isn't ruled out. The next track,
      the 96 kHz FLAC on SBC High with floor 20, sounded terrible, also no log. The
      owner also saw `sampr` fall during a track. The encoder's rate changes only
      through a reconfiguration, which the log marks as `reconf`, so a mid-track
      change would be a separate bug.
  - **Open:** the log can't show loss after the chip confirms a packet. Transmit
    power read 12 dBm of max 12, link quality always 255 and failed contacts 0, so
    the log and debug screen no longer report them.
  - **Ruled out:** missing headroom. At 12 dB of precut (`c12` in the log, AVRCP
    volume 67, was 47) the same track cut out as before, and peaks that low can't
    overflow a decoder. SBC is constant bitrate, so loud passages send no extra
    data. The encoder run on a PC matches libsbc at bitpool 53 on full-scale
    tones, noise and music (SNR within 1.5 dB, same worst block). The improvement
    the owner heard at 6 dB is unexplained. Also ruled out: a wrong patchram, since
    `erosqnative_hw4.hcd` is AzureWave's module-specific file
    (`AW_NM372SM_Generic_BCM43438A1_UART_26MHz`), and the OF update image as a
    source on GPIO 86: the one at hand (`erosq_2024`, 2024-09-10) holds only
    u-boot, UCL-packed at 0x6800 (`tools/uclpack -d`), which reads port B for the
    hardware revision and never touches PC22; the OF kernel sets it.

- **Some ticks come 20 ms late.** Medium; needs instrumentation. Log 8 had 30
  of them (`tick max` 41 ms) with the pacing fixed, so something holds the
  Bluetooth thread, at priority 4, off the CPU for a tick: a lock held by a
  lower-priority thread, or a driver running with interrupts off. The encoder
  catches up, so the cost is a burst of two packets, not a gap. To find it, log
  what the thread was waiting on, or sample the thread that ran during the gap.

- **Warm-up, and gaps on the T33.** Open; five changes built, not yet run.
  2026-11-11 report on the current build: the link grows steadier after a
  while connected, and short gaps remain, mostly on the T33. No log; the
  changes below were picked from the code, not from a measurement.
  - *SBC sent part packets at 100 a second.* The 10 ms tick (Fixed, SBC sent
    packets in pairs) sent whatever audio was due, 3 or 4 frames, at full
    bitpool: 100 packets a second against 68.9 of 5 frames at 20 ms
    (`scratch/sbc_pacing.py`). The tick now runs once per full packet, woken
    by a hardware timer (TCU channel 6) instead of the kernel tick, and only
    full packets go, on EDR and non-EDR links alike (AOSP's non-EDR path sent
    whatever was due): 68.9 a second, gaps 12.5–16.5 ms with wake-ups up to 2
    ms late; 43 a second at 8 frames (bitpool 30). aptX's 13–15 ms periods
    run exact too. The bitpool adaptation's hold and recovery are in time
    now, not ticks.
  - *Channel map kept between connections* (`/.rockbox/bt_afh.txt`). The
    RSSI poll also reads the link's AFH map; a map read once the chip chose
    alone is saved when the stream stops. On connecting, a saved map under a
    day old with 20 to 78 channels is set as the host classification, then
    released after 60 s of streaming. Debug screen: `afh` (channels now,
    seed, `held` or `free`).
  - *Chip sleep mode turned off explicitly* (0xFC27, mode 0) after the
    patch loads; before, it was left at the patch's own default.
  - *Sleep clock measured at power-on*: rising edges on PB26 over 2 ms, on
    the debug screen as `lpo`. 32768 is right; 0 is no clock or a pin that
    doesn't read back as an output, which then needs a scope. The vendor
    kernel selects the same pin function (`rtc32k-enable`, function 0) and
    no RTC register, so the code matches the OF.
  - *No sniff on the A2DP link*: it went into sniff after 7 s open or idle,
    which a stream start had to leave first.
  - To read on the next listen: `lpo`, `afh` after a minute and after a
    reconnect, `tick late`, and whether the warm-up and the T33's gaps
    change.

## Hardware pass

- **The T33 earbuds again on the current build.** Small, a listen and a log.
  Log 6, with the cut-outs, predates the tick pacing fix and the 10 ms SBC tick,
  and the T33 are the one device where the chip's queue was seen backing up.
- **Smoke test of the current build.** Small; bring-up, pair and play. The HCI
  command path, pairing prompt and receive screen changed since the last install.
- **96 kHz FLAC again without the FIFO spin.** Small. `uart_tx_timeout()` no longer
  spins before yielding, which went in with the priority change and was never tested
  alone. If the overload under Fixed returns (`tick late`, `q max`, falling bitpool),
  put it back: spin up to 1 ms for the FIFO to drain, then yield.
- **Codecs and controls.** Small; needs two different headphones. Pair both, play
  SBC, then LDAC and aptX, noting the debug screen's `enc load`; AVRCP buttons and
  absolute volume.
- **Failure paths.** Boot without the `.hcd` file for the "failed to start"
  message; a lost packet stream is harder to trigger. Small; both are reasoned from
  Bluedroid's code, not exercised.

## Later

- **Commit and decide on sending upstream.** Medium; needs the owner's
  approval. All of it is uncommitted on `bt`. Checked against Gerrit on
  2026-11-03: every change in the chain is still open with its patchset as
  fetched, except 8207 (separated volume controls), at patchset 5 since
  2026-10-23 while the tree carries patchset 1 and the 36 changes above it in
  Gerrit still sit on patchset 1. Patchsets 2–5 add an immediate (no ramp)
  flag to the software volume, start its factors at unity, handle the AVRCP
  set-volume response and guard the volume override in `option_select.c` with
  `WANT_SINK_VOLUME_OVERRIDE`; the tree does the same things its own way in
  8229–8233 and `bt_volume.c`, so a rebase would be a merge of intent, not a
  pull. Review comments sit on the Bluedroid port: 7384 has 44, 5 unresolved;
  7386 has 3; 7393 has 2. Other open Bluetooth work in Gerrit is separate:
  7649–7651, a HiBy R1 driver with its own menu, and 3044, an older menu.
- **Theme tags as their own change.** Small; `%Bo` on, `%Bc` connected, `%Bn` device
  name, `%Bk` codec, `%Br` receiving. Built and parsed by checkwps, then taken out to
  keep this change to fewer files; needs `skin_tokens.c`, `tag_table.c`,
  `tag_table.h`, `wps_tags.tex` and `bt_output_codec()` in `bt.c`.
- **Remove the test settings file.** Small; once the values are settled, make
  them constants again. `/.rockbox/bt_tune.txt`, read each time Bluetooth starts,
  takes `name: value` lines; the debug screen's `tune` line and the log show what
  loaded. Names and defaults: `bt_pwr` (1), `bitpool_floor` (30),
  `bitpool_backlog` (5), `bitpool_down_step` (6), `bitpool_up_step` (2),
  `bitpool_up_ms` (500), `precut_db` (24; 6 then 12 before 2026-11-03), `edr_2mbps` (0).
  Settled and made constants on 2026-11-03: the thread priority (4; 14 is the
  chain's value), the chip's UART sleep mode (off, its code removed from the
  driver) and the media channel's flush timeout (Bluedroid's own, none).
- **hw1–3 support.** Large; blocked on identifying the Bluetooth chip from a
  photo of the module or a stock update image (its `.psr` or `.hcd`). A CSR8811,
  as on agptekrocker, would reuse the BCSP → H4 bring-up from Gerrit change 8163.
- **More transmit power.** Large, and not recommended. The chip reports 12 dBm, its
  stated maximum, so more means changing the firmware's limits, not a setting. It
  isn't a single value: the patchram's config section holds repeated tables that
  look like gain steps by power level and channel group, in several variants; the
  layout is undocumented, so that reading is inferred. A vendor power command
  failed on a related chip. Editing the tables can't be checked without RF test
  equipment and doesn't help what the headphones send back.

## Decisions

- **24 dB precut on all Bluetooth output** (`precut_db`, up to 48; 6 dB until
  2026-11-03, then 12, then 24 the same day, on the owner's call). The owner prefers a precut and has
  volume to spare at -60. It doesn't touch the wired jack or Receive Audio.
- **2 Mbps packets off by default, bitpool floor 30, sleep mode off, no flush
  timeout.** The owner reports `edr_2mbps: 0` with `bitpool_floor: 30` better
  than floor 20 with 2 Mbps packets on the 96 kHz FLAC, by ear, with no log, on
  one pair of headphones; logs 6 and 7 show no 3 Mbps packets on two other
  headsets while the chip has them (log 8, `3M 1`), so the setting is moot for
  those two;
  `edr_2mbps: 1` turns them back on, and the code can go
  if no other device needs it. On 2026-11-03, after the `BT_PWR` fix, floor 53
  (adaptation off), sleep mode off and a 3 ms flush timeout on the media channel
  were tried together on the owner's call: "a complete unintelligible mess of
  gaps". With the flush off the other two were still "steps backwards", so all
  three went back. The 3 ms flush gave a 5-slot packet (3-DH5, 3.125 ms) one try,
  and the timer may start at queueing, so a packet behind another can be flushed
  unsent; Bluedroid's own comment warns some headsets disconnect on a partial
  frame. The flush applies to the media channel only, the other channels are sent
  non-flushable. Later the same day, with the floor back at 30, the owner found
  sleep mode off better after all, so it stays off, and its code and setting are
  gone from the driver along with the flush setting; the floor stays a test
  setting.
- **Not on the list, 2026-11-03, on the owner's call:** the unreproduced
  "codec the device can't play" report (the logs since show SBC High and
  `stream SBC bp 2-53`; save a log if it recurs), the Receive Audio, pairing
  prompt and track info device tests, and the soak.
- **The DAC stays powered during Bluetooth.** Powering it down too was tried behind
  a switch and dropped: plug detection may depend on those rails.
- **No codec is disabled.** A per-target mask was tried and dropped until there
  were measurements; as of 2026-11-03 the owner has never seen `enc load` above
  about 10% on any codec, song, bitrate or file type (SBC about 2%, aptX 11.2%
  in log 7), so the X1000 sustains them all and the item is closed.
- **The simulator stays unbuildable.** `uisimulator/common/hci-sim.c` needs SDL
  threads and `pcm_sink.h` uses `bool` without `<stdbool.h>`. Fixes were made, then
  dropped at the owner's request.
- **As few changed files as possible.** There are no new code files: shared H4 and
  address helpers, a track file and other non-functional changes were folded back or
  dropped, and the receive role lives in `bt.c`. Auto Off was built and removed for
  this. The L2CAP quota stays in `bt_target.h`: it is compile-time, and no header
  already changed here reaches it.

## Fixed

- **Picking a codec by hand.** 2026-11-03, built, not yet run. The Codec
  option has a sixth choice, Best Available, now the default for Device
  Defaults and for a device: the device gets the highest codec it supports,
  tried from LDAC down to SBC. The five fixed choices stay as ceilings. Stored
  values are unchanged, so a device set to a codec by hand keeps it until its
  Options say Best Available. The debug screen's `codec` line shows no `want`
  on Best Available.
- **SBC sent packets in pairs.** 2026-11-03. The encoder paces by elapsed
  time and sends whole packets of about 14.5 ms of audio, so a 20 ms tick sent
  one packet on some ticks and two on others. The 10 ms tick that replaced it
  sent part packets at 100 a second; replaced on 2026-11-11 by a tick per full
  packet (Faults, Warm-up).
  Log 8 showed the chip holding packets far less once aptX's sends were evened
  out, which is the reason.
- **Codec ticks fired in pairs on a 15 ms period.** 2026-11-03. `sleep(n)`
  blocks for n + 1 ticks, so the codec task's wait overshot by one and caught up
  with two ticks at once (log 7, aptX, `tick late` 1782). The wait is now one
  tick less; a 15 ms period alternates 10 and 20 ms, each packet within 5 ms of
  its time, and SBC's 20 ms period is unchanged. Log 8: `tick late` 30, and the
  chip held packets far less (`w>=100` 14 → 0, `wait max` 294 → 63 ms).
- **48 kHz tracks streamed at 48 kHz.** 2026-11-03. With Playback Frequency on
  Auto the stream was reconfigured to 48 kHz for a 48 kHz track: the same
  bitrate in about 75 packets a second instead of 69, which the link didn't
  carry (log 6), while a fixed 44.1 kHz Playback Frequency was "a big
  improvement" by ear. Now Bluetooth Settings has **Stream at 44.1 kHz**,
  default Yes, kept in `config.cfg`: the sink offers 44.1 kHz as its only rate,
  so everything is resampled to it whatever Playback Frequency says. No turns it
  off for a later hardware revision or device that carries the higher rate.
- **The level settled a second or two into playback on a device.** 2026-11-03;
  the owner hears an improvement on the build with this and the page scan change,
  not separated. The precut was applied from the first sample; what moved
  was the hand-over to absolute volume. Playback starts as soon as A2DP is up,
  with the player scaling by its own volume curve plus the precut and the sink
  at whatever level it last had; when the sink's AVRCP link came up and answered
  the capabilities request, a second or two later, the player stepped to the
  precut alone and told the sink the saved volume (`vol 75 avrcp` in the logs),
  which some headsets ramp slowly. Now `bt_volume.c` keeps the player's own scaling
  until the sink confirms the volume it was sent (the set response or the
  interim notification); the debug screen shows `avrcp pending` until then.
- **Page scan while a link is up.** 2026-11-03; the owner hears an improvement
  on the build with this and the volume hand-over, not separated. The player
  stayed connectable whenever a paired device had auto-connect, streaming
  included, so the chip opened a receive window every 1.28 s in which it could
  not send. `update_scan_mode()` now goes non-connectable once a link is up and
  back when it drops; Receive Audio keeps its own mode. A second device can't
  call in while one is connected, which it couldn't use anyway.
- **Scanning while streaming.** 2026-11-03, built, not yet run. Inquiry takes
  most of the radio, so the Pair New Device list pauses a stream to a connected
  device while it is open and resumes it on leaving (`bluetooth_menu.c`).
- **Weak radio link: `BT_PWR` was never driven.** 2026-11-03, judged by ear as
  "a good fix", no `rssi` figure yet. PC22, the OF's "BT Power" GPIO 86, was an
  output held low at boot; the driver raised only BT_REG_ON. The chip boots
  without it, so it is the module's other rail or WL_REG_ON, on which the shared
  antenna front end depends. `chip_power()` raises it before BT_REG_ON and drops
  it at power-off, behind the test setting `bt_pwr` (1; `w1`/`w0` on the debug
  screen's `tune` line). Safe: the pin was already an output, only its level
  changed, and agptekrocker drives its BT_PWR the same way. The cut-outs under
  Faults are reduced, not gone.
- **Overload on a 96 kHz, 24-bit FLAC.** It was first reported as 48 kHz; the file
  is 2.7 Mbit/s with 8192-sample blocks, resampled to 48 kHz. On the old build it
  sounded terrible: bitpool 14, `bp down` 6, `q max` 28, `wait max` 241 ms, `pcm
  silence` 39, `tick late` 500. With Bluetooth threads above a boosted codec, no
  yield for a send's last bytes, a bitpool floor of 30 and faster recovery: bitpool
  48 with min 48, `bp down` 0, `q max` 1, `tick late` 0, `tick max` 28 ms, PCM buffer
  about 91–95% full. These changes went in together, so which one fixed it isn't
  separated; `bitpool_floor` can still test that (the priority is a constant
  now), and the no-yield
  change has since been reverted (see Hardware pass). The cut-outs above are a
  separate fault and persist.

## Background

The F20 is the `erosqnative` target (Aigo Eros Q/K, AGPTEK H3, HiFi Walker H2):
native Ingenic X1000, 32 MB RAM, no OS underneath, so the host stack runs inside
Rockbox. Milestone 1 on hw4: send audio to headphones, pair from the menu, AVRCP
buttons and volume, receive audio from a phone.

- **Base.** Gerrit chain 7393 → 8243, about 55 changes, open upstream, based on
  `0098ba6023`; maintainers favor merging the import. Since 2026-11-03 the work
  sits directly on change 8243 patchset 1 (`cbc5a723ee`), branch `bt-gerrit`,
  which sits on 8207 patchset 1 (current is 5). Before that it sat on the same
  chain rebased onto master (`295b2ef49f`, branch `bt`); the work applied to
  either with no conflicts.
  - 7393: Bluedroid from esp-idf (Apache-2.0). 7384: the Rockbox port.
  - 7497: `firmware/export/bt_hci.h`. 7499: the hw4 HCI driver. 7386, 7387:
    erosqnative enable and the integration code.
  - 8055–8061: SBC, LDAC, aptX and aptX HD encoders from AOSP. 8207: separate sink
    volume. 8163: agptekrocker CSR8811 bring-up.
  - 8200–8243: debug stats, the core/UI split, settings menus, pairing, per-device
    volume, AVRCP, reconnect, the manual and an A2DP test sink (8234).
- **Baseline** `erosqnative` on `295b2ef49f`: `rockbox.erosq` 1,655,792 bytes;
  .text 1,067,772, .rodata 499,100, .data 11,068, .bss 780,548 (includes the 256
  KiB `BT_HEAP_SIZE`). 166 warnings, all in the chain's code (146 in Bluedroid, 20
  in `apps/bluetooth/codecs`); 167 on `cbc5a723ee`, the one more from the older
  master underneath, none from the work.
- **Chip.** hw4 has an AzureWave AW-NM372 (CYW43438 / BCM4343A1) on UART0. Its
  patchram ships as `device_fw/erosqnative/erosqnative_hw4.hcd`, installed to
  `/.rockbox/fw/`. Pins are in the HW4 section of
  `firmware/target/mips/ingenic_x1000/erosqnative/gpio-target.h`.
- **HCI driver** (`hci-erosqnative.c`), rewritten from the proof of concept in 7499,
  which had no LPO clock or RTS/CTS, blocking reads, `panicf` on any bad frame and
  an ACL length that misframed packets over 255 bytes. The sequence comes from the
  public H2/Eros Q forks (GPL): LPO on PB26, BT_REG_ON, reset at 115200, 3 Mbaud,
  minidriver, `.hcd` records, 250 ms, reset, 3 Mbaud, unit address, plus BT_PWR
  before BT_REG_ON (Fixed). The OF's UART sleep mode was carried too, found worse
  for the stream and removed; HOST_WAKE_BT stays high. A host test of the H4
  framing passed before the transport moved into the driver.
- **Failure.** `bt_hci_enable()` stays `void`: on failure the driver attaches
  nothing, so `hal_open()` in `hci_hal_h4.c` can't open,
  `esp_bluedroid_init_with_cfg()` cleans up and fails, and `bt_enable()` takes its
  failure path. hw1–3 attach nothing. A lost packet stream broadcasts
  `SYS_BLUETOOTH_HCI_FAILED` and `default_event_handler()` calls `bt_restart()`;
  agptekrocker keeps the chain's panic.
- **Unit address.** If Read_BD_ADDR returns Broadcom's default, shared by
  unprogrammed chips, a per-unit one is written (0xFC01) and kept in
  `/.rockbox/bt_addr.txt`, as agptekrocker does.
- **Audio path.** `audiohw_standby()` (`HAVE_AUDIOHW_STANDBY`) mutes the output
  switch and shuts the headphone amp down while Bluetooth plays. At 48 kHz, with
  Playback Frequency on Auto, `bt_sink_set_freq()` reconfigures the stream per
  track, unless Stream at 44.1 kHz is on (the default, Fixed).
- **Pairing.** Numeric comparison asks the user; passkey display is shown; passkey
  entry is refused; a failed legacy pairing offers a PIN and retries. A confirmation
  the user gives up on is refused. Unsolicited pairing is asked only on the Receive
  Audio screen, accepted as before elsewhere.
- **Receive Audio.** Bluedroid runs one A2DP role at a time, so the core switches
  source → sink and back and routes A2DP and AVRCP CT events to the receive section
  of `bt.c`. It decodes SBC (external codec mode, the OI decoder) into a 24 × 4 KiB ring on
  `PCM_MIXER_CHAN_BTAUDIO`, playing after 6 blocks. The screen is modal and stops
  playback on entry.
- **AVRCP track info.** Bluedroid patch, listed in its README:
  `esp_avrc_tg_register_track_info_callback()`; the TG answers GetElementAttributes,
  GetPlayStatus and InformDisplayCharset and allows the play status, track and
  position notifications. The provider and a one-second poll are in `bt.c`.
- **Changes after the first device test:**
  - `PRIORITY_BLUETOOTH` 14 → 4, because `boost_codec_thread()` raises the codec to
    as high as 5 when the PCM buffer runs low;
  - the reported stream config survives a request that changes nothing;
  - an event log in `bt.c`, saved with long Menu on the debug screen and from the
    main thread 5 s after the stream stops (`SYS_BLUETOOTH_LOG_SAVE`).
- **Verification.** Builds: `erosqnative` (247, the one firmware for every
  revision), the `erosqnative_v4` bootloader (249 is bootloader-only),
  `agptekrockernative` (239; 240 is the hosted Linux build) and checkwps, with a
  MIPS toolchain from `tools/rockboxdev.sh --target=i`. `erosqnative` and
  `agptekrockernative` build with no new warnings in changed files; the bootloader
  and checkwps last built before the latest cleanups, and the manual hasn't been
  built. Bench before hardware: the A2DP test sink (8234) on a Linux machine with a
  USB Bluetooth adapter. Hardware results go here with the commit they ran on.
