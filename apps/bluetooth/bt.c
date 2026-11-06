#include "bluetooth/bt_sw_volume.h"
#include "settings.h"
#include "sound.h"
#define LOGF_ENABLE
#include "action.h"
#include "asm/thread.h"
#include "bluetooth/codecs.h"
#include "bt_hci.h"
#include "btc_av.h"
#include "button.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "list.h"
#include "logf.h"
#include "panic.h"
#include "pcm-internal.h"
#include "pcm.h"
#include "pcm_sink.h"
#include "queue.h"
#include "semaphore.h"
#include "splash.h"
#include "stack/a2d_api.h"
#include "stack/a2d_sbc.h"
#include "stack/btm_api.h"
#include "system.h"
#include "thread.h"
#include "tick.h"
#include "tlsf.h"
#include "misc.h"
#include "file.h"
#include "rbpaths.h"
#include "audio.h"
#include "metadata.h"
#include "pcmbuf.h"
#include "timeout.h"
#include "osi/alarm.h"
#include "osi/bt_stats.h"
#include "oi_codec_sbc.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "bt.h"
#include "bt-internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"

static struct a2dp_codec *current_codec = NULL;

/* The codec choices from best to plainest. A sink gets the one its Codec
 * option asks for or, if it doesn't have it, the best one below it that
 * it has. SBC is twice: Standard only differs by a lower bitpool ceiling,
 * which the SBC encoder takes from the option. */
static const struct codec_choice {
    enum bt_codec option;
    struct a2dp_codec *codec;
    const char *name;
} codec_choices[] = {
    { BT_CODEC_LDAC,         &a2dp_codec_ldac,    "LDAC" },
    { BT_CODEC_APTX_HD,      &a2dp_codec_aptx_hd, "aptX HD" },
    { BT_CODEC_APTX,         &a2dp_codec_aptx,    "aptX" },
    { BT_CODEC_SBC_HIGH,     &a2dp_codec_sbc,     "SBC High" },
    { BT_CODEC_SBC_STANDARD, &a2dp_codec_sbc,     "SBC Standard" },
};

/* What the sink was asked for and what it got */
static const struct codec_choice *codec_wanted;
static const struct codec_choice *codec_used;

/* The A2DP link and the encoder for it */
static bool link_open;
static uint16_t link_mtu;
static bool codec_ready;
/* The sample rate the encoder is set up for */
static unsigned long cur_sampr;
/* The stream config as last reported */
static esp_a2d_mcc_t stream_cfg;
static bool stream_cfg_valid;

static const struct codec_choice *codec_choice(enum bt_codec option)
{
    for (size_t i = 0; i < ARRAYLEN(codec_choices); i++)
        if (codec_choices[i].option == option)
            return &codec_choices[i];
    return codec_choice(BT_CODEC_SBC_HIGH);
}

extern unsigned char btbuf[];

/* The encoder's CPU load: its work over the audio it covered */
static uint64_t work_us_sum, period_us_sum;

/* Test settings, read from a "name: value" file each time Bluetooth starts.
 * A missing name or a value out of range keeps the default. */
#define BT_TUNE_FILE    ROCKBOX_DIR "/bt_tune.txt"

int bt_pwr_pin = 1;
struct bt_tune bt_tune;

static const struct {
    const char *name;
    int *val;
    int def, min, max;
} tune_items[] = {
    { "bt_pwr",            &bt_pwr_pin,          1, 0, 1 },
    { "bitpool_floor",     &bt_tune.bp_floor,   30, 3, 64 },
    { "bitpool_backlog",   &bt_tune.bp_backlog,  5, 2, 40 },
    { "bitpool_down_step", &bt_tune.bp_down_step, 6, 1, 64 },
    { "bitpool_up_step",   &bt_tune.bp_up_step,  2, 1, 64 },
    { "bitpool_up_ms",     &bt_tune.bp_up_ms,  500, 20, 10000 },
    { "precut_db",         &bt_tune.precut_db,  24, 0, 48 },
    { "edr_2mbps",         &bt_tune.edr_2mbps,   0, 0, 1 },
};

void bt_tune_read(void)
{
    char line[64];
    char *name, *value;

    for (size_t i = 0; i < ARRAYLEN(tune_items); i++)
        *tune_items[i].val = tune_items[i].def;

    int fd = open(BT_TUNE_FILE, O_RDONLY);
    if (fd >= 0) {
        while (read_line(fd, line, sizeof(line)) > 0) {
            if (!settings_parseline(line, &name, &value))
                continue;
            for (size_t i = 0; i < ARRAYLEN(tune_items); i++) {
                int v = atoi(value);
                if (!strcmp(name, tune_items[i].name) &&
                    v >= tune_items[i].min && v <= tune_items[i].max)
                    *tune_items[i].val = v;
            }
        }
        close(fd);
    }
}

/* Event log: what happened around an audible gap. Kept in RAM, as writing to
 * storage while streaming would disturb what it measures, and saved to a file
 * with long Menu on the debug screen or once the stream has stayed stopped. */
#define BT_LOG_FILE     ROCKBOX_DIR "/bt_log.txt"
#define BT_LOG_SIZE     512
#define BT_LOG_LATE_MS  40      /* media ticks at least this far apart */
#define BT_LOG_MARK_TICKS (3 * HZ)  /* an entry at least this often */
#define BT_LOG_SAVE_DELAY (5 * HZ)  /* a stop this long isn't a track change */

enum {
    BT_LOG_TICK, BT_LOG_START, BT_LOG_STOP, BT_LOG_RECONFIG, BT_LOG_MARK,
    BT_LOG_PCM_ON, BT_LOG_PCM_OFF
};

/* A change in any counter before LOG_SLOW makes an entry; the rest add up
 * until the next one */
enum {
    LOG_UNDER, LOG_SHORT, LOG_QDROP, LOG_AVDROP, LOG_BPDOWN, LOG_FLUSH,
    LOG_WAIT50, LOG_WAIT100, LOG_LINK, LOG_TXDROP,
    LOG_SLOW, LOG_SILENCE, BT_LOG_COUNTS
};

static struct bt_log_entry {
    uint32_t ms;
    uint32_t pos_ms;
    uint16_t gap_ms;
    uint16_t src_kbps;
    uint8_t src_khz;
    uint8_t pcm_pct;
    uint8_t kind;
    uint8_t bitpool;
    uint8_t queue;
    uint8_t acl_out;
    int8_t rssi;
    uint8_t delta[BT_LOG_COUNTS];
} bt_log[BT_LOG_SIZE];
static unsigned int bt_log_total;
static unsigned int bt_log_saved;
static long bt_log_next_mark;
static uint32_t bt_log_last[BT_LOG_COUNTS];
static struct timeout bt_log_save_tmo;

static void bt_log_counts(uint32_t *c)
{
    const struct bt_stats *s = &bt_stats;

    c[LOG_UNDER]  = s->a2d_underflow;
    c[LOG_SHORT]  = s->pcm_short;
    c[LOG_QDROP]  = s->a2d_q_drops;
    c[LOG_AVDROP] = s->a2d_av_drops;
    c[LOG_BPDOWN] = s->a2d_bp_down;
    c[LOG_FLUSH]  = s->flush_occ;
    c[LOG_WAIT50] = s->nocp_gap[3] + s->nocp_gap[4];
    c[LOG_WAIT100] = s->nocp_gap[4];
    c[LOG_LINK]   = s->mode_chg + s->role_chg + s->slots_chg;
    c[LOG_TXDROP] = s->tx_drops;
    c[LOG_SLOW]   = s->tx_slow;
    c[LOG_SILENCE] = s->pcm_silence;
}

static struct bt_log_entry *bt_log_add(int kind, uint32_t gap_ms)
{
    struct bt_log_entry *e = &bt_log[bt_log_total++ % BT_LOG_SIZE];
    struct mp3entry *id3 = audio_current_track();
    uint32_t queue = esp_a2d_source_audio_queue_len();
    int descs = pcmbuf_descs();

    memset(e, 0, sizeof(*e));
    e->ms = current_tick * (1000 / HZ);
    e->pos_ms = id3 ? id3->elapsed : 0;
    e->src_khz = id3 ? MIN(id3->frequency / 1000, 0xff) : 0;
    e->src_kbps = id3 ? MIN(id3->bitrate, 0xffff) : 0;
    e->pcm_pct = descs > 0 ? MIN(pcmbuf_used_descs() * 100 / descs, 100) : 0;
    e->gap_ms = MIN(gap_ms, 0xffff);
    e->kind = kind;
    e->bitpool = bt_stats.a2d_bitpool;
    e->queue = MIN(queue, 0xff);
    e->acl_out = MIN(bt_stats.acl_out, 0xff);
    e->rssi = bt_stats.rssi;
    return e;
}

/* After each media tick */
static void bt_log_tick(uint32_t gap_ms)
{
    uint32_t now[BT_LOG_COUNTS];
    bool changed = false;

    bt_log_counts(now);
    for (int i = 0; i < LOG_SLOW; i++)
        changed |= now[i] != bt_log_last[i];

    bool late = gap_ms >= BT_LOG_LATE_MS;
    bool mark = TIME_AFTER(current_tick, bt_log_next_mark);
    if (changed || late || mark) {
        struct bt_log_entry *e = bt_log_add(changed || late ? BT_LOG_TICK :
                                            BT_LOG_MARK, gap_ms);
        for (int i = 0; i < BT_LOG_COUNTS; i++)
            e->delta[i] = MIN(now[i] - bt_log_last[i], 0xff);
        memcpy(bt_log_last, now, sizeof(now));
        bt_log_next_mark = current_tick + BT_LOG_MARK_TICKS;
    }
}

static void bt_log_save(void);

/* Tick context: only says the file is due, the main thread writes it */
static int bt_log_save_due(struct timeout *tmo)
{
    (void)tmo;
    queue_broadcast(SYS_BLUETOOTH_LOG_SAVE, 0);
    return 0;
}

static void bt_log_save_soon(void)
{
    timeout_register(&bt_log_save_tmo, bt_log_save_due, BT_LOG_SAVE_DELAY, 0);
}

int bt_heap_info_action_callback(int action, struct gui_synclist *lists)
{
    static long next_rssi;

    switch (action) {
        case ACTION_NONE:
            if (bt_core_peer() && TIME_AFTER(current_tick, next_rssi)) {
                esp_bt_gap_read_rssi_delta((uint8_t *)bt_core_peer());
                next_rssi = current_tick + HZ;
            }
            return ACTION_REDRAW;
        case ACTION_STD_OK:
            bt_stats_reset();
            work_us_sum = period_us_sum = 0;
            bt_log_counts(bt_log_last);
            return ACTION_REDRAW;
        case ACTION_STD_CONTEXT:
            bt_log_save();
            return ACTION_REDRAW;
        default:
            return action;
    }
}

#define BT_INFO_FIXED_ITEMS 2

/* Short lines, the most telling first: they have to fit a narrow screen */
static const char *bt_stats_line(int line, char *buf, size_t len)
{
    const struct bt_stats *s = &bt_stats;
    const uint32_t *g = s->nocp_gap;

    switch (line) {
    case 0:  snprintf(buf, len, "bitpool %u min %u", s->a2d_bitpool, s->a2d_bp_min); break;
    case 1:  snprintf(buf, len, "bp down %lu up %lu", s->a2d_bp_down, s->a2d_bp_up); break;
    case 2:  snprintf(buf, len, "avdrop %lu", s->a2d_av_drops); break;
    case 3:  snprintf(buf, len, "avlist max %lu", s->a2d_av_list_max); break;
    case 4:  snprintf(buf, len, "qdrop %lu", s->a2d_q_drops); break;
    case 5:  snprintf(buf, len, "underflow %lu", s->a2d_underflow); break;
    case 6:  snprintf(buf, len, "rssi %d", s->rssi); break;
    case 7:  snprintf(buf, len, "acl full %lu", s->acl_full); break;
    case 8:  snprintf(buf, len, "acl out %lu max %lu", s->acl_out, s->acl_out_max); break;
    case 9:  snprintf(buf, len, "acl %ux%u", s->acl_bufs, s->acl_size); break;
    case 10: snprintf(buf, len, "wait max %lums", s->nocp_gap_us_max / 1000); break;
    case 11: snprintf(buf, len, "w<10 %lu", g[0]); break;
    case 12: snprintf(buf, len, "w<20 %lu", g[1]); break;
    case 13: snprintf(buf, len, "w<50 %lu", g[2]); break;
    case 14: snprintf(buf, len, "w<100 %lu", g[3]); break;
    case 15: snprintf(buf, len, "w>=100 %lu", g[4]); break;
    case 16: snprintf(buf, len, "tx max %luus", s->tx_us_max); break;
    case 17: snprintf(buf, len, "tx >4ms %lu", s->tx_slow); break;
    case 18: snprintf(buf, len, "tx drop %lu", s->tx_drops); break;
    case 19: snprintf(buf, len, "uart err %lu", s->drv_rx_errors); break;
    case 20: snprintf(buf, len, "flush %lu", s->flush_occ); break;
    case 21: snprintf(buf, len, "qos %lu hwerr %lu", s->qos_viol, s->hw_err); break;
    case 22: snprintf(buf, len, "mode %u/%u chg %lu", s->mode, s->mode_intv, s->mode_chg); break;
    case 23: snprintf(buf, len, "role %u chg %lu", s->role, s->role_chg); break;
    case 24: snprintf(buf, len, "slots %u chg %lu", s->slots, s->slots_chg); break;
    case 25: snprintf(buf, len, "ptype %04x", s->pkt_type); break;
    case 26: snprintf(buf, len, "pcm short %lu", s->pcm_short); break;
    case 27: snprintf(buf, len, "pcm silence %lu", s->pcm_silence); break;
    case 28: snprintf(buf, len, "tick late %lu", s->a2d_tick_late); break;
    case 29: snprintf(buf, len, "tick max %lums", s->a2d_tick_gap_us_max / 1000); break;
    case 30: snprintf(buf, len, "work max %luus", s->a2d_work_us_max); break;
    case 31: snprintf(buf, len, "q max %lu", s->a2d_q_max); break;
    case 32: snprintf(buf, len, "rate %ukbps", s->a2d_bitrate); break;
    case 33: snprintf(buf, len, "mtu %u", s->a2d_mtu); break;
    case 34: snprintf(buf, len, "alloc max %luus", s->alloc_us_max); break;
    case 35: snprintf(buf, len, "free max %luus", s->free_us_max); break;
    case 36: snprintf(buf, len, "alloc >200us %lu", s->alloc_slow); break;
    case 37: snprintf(buf, len, "tx %lu rx %lu", s->tx_pkts, s->rx_pkts); break;
    /* what the encoder takes of the CPU, the test of a codec's cost */
    case 38: {
        unsigned long permille = period_us_sum ?
                                 work_us_sum * 1000 / period_us_sum : 0;
        snprintf(buf, len, "enc load %lu.%lu%%", permille / 10, permille % 10);
        break;
    }
    case 39: bt_receive_debug(buf, len); break;
    case 40: snprintf(buf, len, "log %u, long Menu saves", bt_log_total); break;
    case 41: snprintf(buf, len, "tune w%d f%d b%d d%d u%d/%d c%d e%d",
                      bt_pwr_pin, bt_tune.bp_floor, bt_tune.bp_backlog,
                      bt_tune.bp_down_step, bt_tune.bp_up_step,
                      bt_tune.bp_up_ms, bt_tune.precut_db, bt_tune.edr_2mbps);
             break;
    case 42: {
        /* The chip's LMP features, page 0: whether it has 2 and 3 Mbps
         * packets itself, since Bluedroid masks what either side lacks */
        const uint8_t *f = BTM_ReadLocalFeatures();
        snprintf(buf, len, "lmp %02x%02x%02x%02x%02x%02x%02x%02x 2M %d 3M %d",
                 f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                 HCI_EDR_ACL_2MPS_SUPPORTED(f) ? 1 : 0,
                 HCI_EDR_ACL_3MPS_SUPPORTED(f) ? 1 : 0);
        break;
    }
    default:
        return NULL;
    }

    return buf;
}

#define BT_INFO_STATS_ITEMS 43

static const char *codec_name(tA2D_CODEC type)
{
    for (size_t i = 0; i < ARRAYLEN(codec_choices); i++)
        if (codec_choices[i].codec->codec_id == type)
            return codec_choices[i].codec == &a2dp_codec_sbc ?
                "SBC" : codec_choices[i].name;
    return "other";
}

/* The codec in use first, what the device was asked for if it differs */
static const char *bt_codec_line(int line, char *buf, size_t len)
{
    switch (line) {
    case 0:
        if (codec_used == NULL)
            return "codec none";
        if (codec_used == codec_wanted || codec_wanted == NULL)
            snprintf(buf, len, "codec %s", codec_used->name);
        else
            snprintf(buf, len, "codec %s, want %s",
                     codec_used->name, codec_wanted->name);
        return buf;
    case 1:
        if (current_codec == NULL || current_codec->debug_info == NULL)
            return "enc -";
        strlcpy(buf, "enc ", len);
        current_codec->debug_info(buf + 4, len - 4);
        return buf;
    case 2:
        if (!stream_cfg_valid)
            return "stream -";
        if (stream_cfg.codec_type == ESP_A2D_MCT_SBC)
            snprintf(buf, len, "stream SBC bp %u-%u",
                     stream_cfg.cie.sbc_info.min_bitpool,
                     stream_cfg.cie.sbc_info.max_bitpool);
        else
            snprintf(buf, len, "stream %s", codec_name(
                     A2D_GetCodecType((const uint8_t *)&stream_cfg)));
        return buf;
    case 3:
        if (!codec_ready)
            return "sampr -";
        snprintf(buf, len, "sampr %lu", cur_sampr);
        return buf;
    case 4:
        bt_volume_debug(buf, len);
        return buf;
    default:
        return NULL;
    }
}

#define BT_INFO_CODEC_ITEMS 5

const char * bt_heap_info_getname(int selected_item, void * data, char * buffer, size_t buffer_len)
{
    if (selected_item < BT_INFO_CODEC_ITEMS)
        return bt_codec_line(selected_item, buffer, buffer_len);
    selected_item -= BT_INFO_CODEC_ITEMS;

    if (selected_item < BT_INFO_STATS_ITEMS) {
        const char *s = bt_stats_line(selected_item, buffer, buffer_len);
        return s ? s : "Unknown item!!";
    }

    switch (selected_item - BT_INFO_STATS_ITEMS) {
        case 0:
#ifndef SIMULATOR
            snprintf(buffer, buffer_len, "heap %d/%d", get_used_size(btbuf), get_max_size(btbuf));
            return buffer;
#else
            return "this is sim build you dummy";
#endif
        case 1: {
            const uint8_t *addr = esp_bt_dev_get_address();
            if (addr) {
                snprintf(buffer, buffer_len, ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(addr));
                return buffer;
            } else {
                return "MAC unknown";
            }
        }
        default:
            return "Unknown item!!";
    }
}

bool bt_heap_info(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, "Bluetooth debug info:",
                         BT_INFO_CODEC_ITEMS + BT_INFO_STATS_ITEMS +
                         BT_INFO_FIXED_ITEMS, NULL);
    info.scroll_all = false;
    info.action_callback = bt_heap_info_action_callback;
    info.get_name = bt_heap_info_getname;
    return simplelist_show_list(&info);
}

/* The debug screen's lines, then the log, oldest first */
static bool bt_log_write(void)
{
    static const char * const kinds[] = {
        "tick", "start", "stop", "reconf", "mark", "pcmon", "pcmoff"
    };
    char line[64];
    struct mp3entry *id3 = audio_current_track();
    int fd = open(BT_LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0)
        return false;

    for (int i = 0; i < BT_INFO_CODEC_ITEMS + BT_INFO_STATS_ITEMS; i++)
        fdprintf(fd, "%s\n", bt_heap_info_getname(i, NULL, line, sizeof(line)));

    if (id3)
        fdprintf(fd, "track %lu Hz %lu kbps\n", (unsigned long)id3->frequency,
                 (unsigned long)id3->bitrate);

    fdprintf(fd, "\nago_s    pos_s  event  src_khz src_kbps pcm_pct gap_ms bitpool "
                 "queue acl_out rssi under short qdrop avdrop bpdown flush "
                 "wait50 wait100 link txdrop slow silence\n");

    uint32_t now = current_tick * (1000 / HZ);
    unsigned int count = MIN(bt_log_total, BT_LOG_SIZE);
    for (unsigned int i = bt_log_total - count; i != bt_log_total; i++) {
        const struct bt_log_entry *e = &bt_log[i % BT_LOG_SIZE];
        const uint8_t *d = e->delta;
        fdprintf(fd, "%5lu %8lu  %-6s %7u %8u %7u %6u %7u %5u %7u %4d %5u %5u %5u %6u %6u %5u %6u %7u %4u %6u %4u %7u\n",
                 (unsigned long)(now - e->ms) / 1000,
                 (unsigned long)e->pos_ms / 1000, kinds[e->kind],
                 e->src_khz, e->src_kbps, e->pcm_pct,
                 e->gap_ms, e->bitpool, e->queue, e->acl_out, e->rssi,
                 d[LOG_UNDER], d[LOG_SHORT], d[LOG_QDROP], d[LOG_AVDROP],
                 d[LOG_BPDOWN], d[LOG_FLUSH], d[LOG_WAIT50], d[LOG_WAIT100],
                 d[LOG_LINK], d[LOG_TXDROP], d[LOG_SLOW], d[LOG_SILENCE]);
    }

    close(fd);
    bt_log_saved = bt_log_total;
    return true;
}

static void bt_log_save(void)
{
    splash(HZ, bt_log_write() ? "Saved " BT_LOG_FILE : "Can't write " BT_LOG_FILE);
}

static esp_a2d_audio_state_t bt_current_state = ESP_A2D_AUDIO_STATE_SUSPEND;

/* From the main thread, on SYS_BLUETOOTH_LOG_SAVE */
void bt_log_autosave(void)
{
    if (bt_current_state != ESP_A2D_AUDIO_STATE_STARTED &&
        bt_log_saved != bt_log_total)
        bt_log_write();
}

uint32_t bt_codec_stack[DEFAULT_STACK_SIZE / sizeof(uint32_t)];


/* Media tick stats: how late each tick ran and how long its work took.
 * The gap is invalid until the first tick after the stream starts. */
static bt_stats_time_t tick_last;
static bool tick_valid;

/* Once a second while streaming: the signal strength for the log, the answer
 * is picked up in bt_stats_hci_rx() */
static void bt_link_poll(void)
{
    static long next_poll;
    const uint8_t *peer = bt_core_peer();

    if (peer == NULL || TIME_BEFORE(current_tick, next_poll))
        return;
    next_poll = current_tick + HZ;

    esp_bt_gap_read_rssi_delta((uint8_t *)peer);
}

static void bt_codec_tick(void)
{
    bt_stats_time_t t = bt_stats_now();
    uint32_t gap_ms = 0;

    bt_stats.a2d_ticks++;
    if (tick_valid) {
        uint32_t gap = BT_STATS_US(t - tick_last);
        gap_ms = gap / 1000;
        bt_stats_max(&bt_stats.a2d_tick_gap_us_max, gap);
        if (gap > current_codec->get_period() * 1500) {
            bt_stats.a2d_tick_late++;
        }
    }
    tick_last = t;
    tick_valid = true;

    current_codec->tick();

    uint32_t work = BT_STATS_US(bt_stats_now() - t);
    bt_stats_max(&bt_stats.a2d_work_us_max, work);
    work_us_sum += work;
    period_us_sum += current_codec->get_period() * 1000;
    bt_stats_max(&bt_stats.a2d_q_max, esp_a2d_source_audio_queue_len());
    bt_log_tick(gap_ms);
    bt_link_poll();
}

static void bt_codec_task(void)
{
    int64_t next_ms = (((int64_t)current_tick) * 1000) / HZ;
    tick_valid = false;
    while (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED && bt_current_state == ESP_A2D_AUDIO_STATE_STARTED) {
        uint64_t ticked = 0;
        do {
            if (ticked > (1000 / HZ)) {
                ticked %= (1000 / HZ);
                yield();
            }
            if (current_codec == NULL) {
                thread_exit();
                return;
            }
            bt_codec_tick();
            /* taken down while the tick waited somewhere */
            if (current_codec == NULL) {
                thread_exit();
                return;
            }
            next_ms += current_codec->get_period();
            ticked += current_codec->get_period();
        } while ((next_ms * HZ) / 1000 <= current_tick);
        /* sleep(n) blocks for n + 1 ticks: a 15 ms period slept as 2 ticks
         * made the ticks fire in pairs every 30 ms */
        long wait = ((next_ms * HZ) / 1000) - current_tick;
        sleep(wait > 1 ? wait - 1 : 0);
    }
    if (current_codec == NULL) {
        thread_exit();
        return;
    }
    current_codec->feeding_flush();
    thread_exit();
}

static esp_a2d_conn_hdl_t conn_hdl;
static struct semaphore sink_control_sem;
static struct event_queue sink_control_queue;
static esp_a2d_mcc_t new_pref_mcc;
struct pcm_sink bt_pcm_sink;
static esp_a2d_mcc_t peer_caps;

static int sink_suspended = 1;

bool bt_sink_suspend(void) {
    if (++sink_suspended == 1) {
        logf("suspending playback: sink locked");
        if (semaphore_wait(&sink_control_sem, TIMEOUT_NOBLOCK) == OBJ_WAIT_SUCCEEDED) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        } else {
            queue_post(&sink_control_queue, 1, ESP_A2D_MEDIA_CTRL_SUSPEND);
        }
        return true;
    }
    return false;
}
bool bt_sink_resume(void) {
    if (--sink_suspended == 0) {
        logf("starting playback: sink unlocked");
        if (semaphore_wait(&sink_control_sem, TIMEOUT_NOBLOCK) == OBJ_WAIT_SUCCEEDED) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else {
            queue_post(&sink_control_queue, 1, ESP_A2D_MEDIA_CTRL_START);
        }
        return true;
    }
    return false;
}


/* Our config for the codec in use at the current sample rate. For SBC
 * the bitpool is capped by the device's codec option. */
static void make_pref_mcc(esp_a2d_mcc_t *mcc) {
    current_codec->set_freq(cur_sampr, mcc);
}

/* Changes of our preferred config (sample rate switch, bitpool ceiling,
 * the initial one on connecting) go to Bluedroid one at a time: a request
 * made while one is in flight replaces what is sent next. Requests made
 * on an open stream hold it suspended, once, until the last one is done;
 * if the stream was running the first is sent on the suspend ack. */
static bool pref_busy;          /* set_pref_mcc in flight */
static bool pref_wait_suspend;  /* waiting for the suspend ack */
static bool pref_again;         /* new_pref_mcc not sent yet */
static bool pref_held;          /* we hold one bt_sink_suspend() */
static bool src_rdy_sent;       /* the sink is checked once the first is done */
static esp_a2d_mcc_t sent_pref_mcc;  /* what the one in flight asked for */

/* Whether a request asks for the stream as it was last reported */
static bool pref_matches_stream(const esp_a2d_mcc_t *mcc) {
    if (A2D_GetCodecType((const uint8_t *)&stream_cfg) !=
        A2D_GetCodecType((const uint8_t *)mcc))
        return false;
    if (mcc->codec_type == ESP_A2D_MCT_SBC)
        return stream_cfg.cie.sbc_info.samp_freq == mcc->cie.sbc_info.samp_freq &&
               stream_cfg.cie.sbc_info.max_bitpool == mcc->cie.sbc_info.max_bitpool;
    return memcmp(&stream_cfg.cie, &mcc->cie, sizeof(mcc->cie)) == 0;
}

static void pref_next(void) {
    if (pref_busy || pref_wait_suspend)
        return;

    while (pref_again) {
        pref_again = false;
        if (esp_a2d_source_set_pref_mcc(conn_hdl, &new_pref_mcc) == ESP_OK) {
            sent_pref_mcc = new_pref_mcc;
            pref_busy = true;
            bt_log_add(BT_LOG_RECONFIG, 0);
            return;
        }
    }

    if (!src_rdy_sent) {
        src_rdy_sent = true;
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    }

    if (pref_held) {
        pref_held = false;
        logf("starting playback: preferred config done");
        bt_sink_resume();
    }
}

static void pref_request(const esp_a2d_mcc_t *mcc, bool hold) {
    new_pref_mcc = *mcc;
    pref_again = true;
    if (hold && !pref_held) {
        pref_held = true;
        logf("suspending playback: preferred config change");
        if (bt_sink_suspend())
            pref_wait_suspend = true;
    }
    pref_next();
}

static void pref_reset(void) {
    pref_busy = pref_wait_suspend = pref_again = pref_held = false;
    src_rdy_sent = false;
}

/* A sink that sets up the stream itself (it connected to us) picks the
 * codec and, for SBC, the bitpool range; ask once per connection for ours
 * instead. If the sink won't, the encoder follows what it set up. */
static enum {
    STREAM_CHECK,               /* not asked yet */
    STREAM_ASKED,               /* asked for ours */
    STREAM_SETTLED,             /* ours, or the sink's after asking */
} stream_check;

static bool stream_is(const esp_a2d_mcc_t *mcc) {
    if (A2D_GetCodecType((const uint8_t *)&stream_cfg) !=
        A2D_GetCodecType((const uint8_t *)mcc))
        return false;
    if (mcc->codec_type == ESP_A2D_MCT_SBC)
        return stream_cfg.cie.sbc_info.max_bitpool ==
               mcc->cie.sbc_info.max_bitpool;
    return true;
}

static void codec_init(void);

/* Run the encoder the sink's way: the codec of the stream, with the
 * stream config standing in for the sink's caps */
static void follow_stream(void) {
    tA2D_CODEC type = A2D_GetCodecType((const uint8_t *)&stream_cfg);
    const struct codec_choice *c = NULL;

    for (size_t i = 0; i < ARRAYLEN(codec_choices); i++) {
        if (codec_choices[i].codec->codec_id == type) {
            c = &codec_choices[i];
            break;
        }
    }
    if (c == NULL) {
        logf("stream codec %08lx not supported", (unsigned long)type);
        return;
    }

    logf("sink keeps its own config, following it");
    current_codec->deinit();
    current_codec = c->codec;
    codec_used = c;
    peer_caps = stream_cfg;
    codec_init();
}

static void check_stream_cfg(void) {
    if (stream_check == STREAM_SETTLED || !stream_cfg_valid || !codec_ready)
        return;
    /* A change in progress brings our config anyway, check what it ends
     * up with */
    if (pref_busy || pref_again || pref_wait_suspend)
        return;

    esp_a2d_mcc_t mcc;
    make_pref_mcc(&mcc);
    if (stream_is(&mcc)) {
        stream_check = STREAM_SETTLED;
        return;
    }

    if (stream_check == STREAM_CHECK) {
        stream_check = STREAM_ASKED;
        logf("stream config set by the sink, reconfiguring");
        pref_request(&mcc, true);
    } else {
        stream_check = STREAM_SETTLED;
        follow_stream();
    }
}

/* Whether audio goes to the A2DP sink */
static bool sink_active;

/* The Bluetooth output went away and audio goes back to the jack. With
 * headphones there playback carries on; without, it goes as if they had
 * just been unplugged ("Pause on Headphone Unplug"). */
static void sink_lost(void) {
    if (!sink_active)
        return;
    sink_active = false;

    pcm_switch_sink(PCM_SINK_BUILTIN);
    sound_set_balance(global_settings.balance);
#ifdef HAVE_HEADPHONE_DETECTION
    if (!headphones_inserted())
        queue_broadcast(SYS_BLUETOOTH_SINK_LOST, 0);
#endif
}

/* The A2DP link is gone or going: audio goes back to the jack and the
 * encoder is set up again on the next connection */
void bt_sink_lost(void) {
    sink_lost();
    bt_volume_disconnected();
    bt_log_save_soon();

    stream_cfg_valid = false;
    pref_reset();
    link_open = false;
    codec_ready = false;
    if (current_codec != NULL) {
        bt_current_state = ESP_A2D_AUDIO_STATE_SUSPEND;
        current_codec->deinit();
        current_codec = NULL;
    }
    codec_used = NULL;
}

/* Set the encoder up for current_codec and peer_caps, at the sink's
 * default rate. Returns our config at that rate. */
static void codec_init_mcc(esp_a2d_mcc_t *pref_mcc) {
    struct a2dp_peer_info peer_info = {
        .peer_mtu = link_mtu,
        .peer_caps = &peer_caps,
        .conn_hdl = conn_hdl,
        .is_edr = btc_av_is_peer_edr(),
    };
    current_codec->init(&bt_pcm_sink.caps, &peer_info, pref_mcc);

    /* The codecs list their rates highest first and default to the
     * first. Without a playback frequency setting everything is
     * resampled to the default, so make it 44.1 kHz, the rate of most
     * music, when the sink has it: for SBC that is also where the
     * bitrate target gets bitpool 53 (48 at 48 kHz). With Stream at
     * 44.1 kHz on, it is the only rate offered, so a 48 kHz track is
     * resampled rather than streamed at 48 kHz: the same bitrate in
     * more packets a second, which the radio link may not carry. */
    for (int i = 0; i < bt_pcm_sink.caps.num_samprs; i++) {
        if (bt_pcm_sink.caps.samprs[i] == SAMPR_44) {
            bt_pcm_sink.caps.default_freq = i;
            if (global_settings.bluetooth_fixed_rate) {
                bt_pcm_sink.caps.samprs += i;
                bt_pcm_sink.caps.num_samprs = 1;
                bt_pcm_sink.caps.default_freq = 0;
            }
            break;
        }
    }
    cur_sampr = bt_pcm_sink.caps.samprs[bt_pcm_sink.caps.default_freq];
    current_codec->set_freq(cur_sampr, pref_mcc);

    /* The rate index PCM last set is into the previous rate list, if
     * any: have it set the rate again when it switches to us */
    bt_pcm_sink.configured_freq = -1U;
}

static void codec_init(void) {
    esp_a2d_mcc_t pref_mcc;
    codec_init_mcc(&pref_mcc);
}

/* The encoder is set up once both the link is open and a codec has been
 * picked from the sink's caps. Which comes first depends on who
 * connected: when the sink connects to us its caps are fetched after the
 * stream is open. */
static void codec_setup(void) {
    if (codec_ready || !link_open || current_codec == NULL)
        return;
    codec_ready = true;

    esp_a2d_mcc_t pref_mcc;
    codec_init_mcc(&pref_mcc);
    /* nothing is streaming yet, nothing to hold */
    pref_request(&pref_mcc, false);
}

/* Pick the codec for the sink from its caps, see codec_choices: the one
 * asked for or the best below it the sink has; Best Available starts at
 * the top and is no particular wish */
static void codec_pick(const esp_a2d_sep_mcc_t *seps, int num) {
    enum bt_codec option = bt_core_codec();
    const struct codec_choice *start = option == BT_CODEC_BEST ?
        &codec_choices[0] : codec_choice(option);
    codec_wanted = option == BT_CODEC_BEST ? NULL : start;

    for (const struct codec_choice *c = start;
         c < &codec_choices[ARRAYLEN(codec_choices)]; c++) {
        for (int i = 0; i < num; i++) {
            if (c->codec->is_acceptable(&seps[i].mcc)) {
                logf("codec %s, asked for %s", c->name, start->name);
                current_codec = c->codec;
                codec_used = c;
                peer_caps = seps[i].mcc;
                return;
            }
        }
    }
    logf("no codec in common with the sink");
}

void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            logf("a2d %d %d", event, param->conn_stat.state);
            if (!bt_core_a2d_conn_state(param->conn_stat.state,
                                        param->conn_stat.remote_bda))
                return;
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                semaphore_init(&sink_control_sem, 1, 0);
                queue_init(&sink_control_queue, false);
                sink_suspended = 1;
                pref_held = pref_wait_suspend = false;
                src_rdy_sent = false;
                bt_current_state = ESP_A2D_AUDIO_STATE_SUSPEND;
                stream_check = STREAM_CHECK;
                bt_volume_connected(param->conn_stat.remote_bda);
                /* 3 Mbps packets fail sooner as the signal weakens */
                if (bt_tune.edr_2mbps)
                    esp_bt_gap_set_acl_pkt_types(param->conn_stat.remote_bda,
                        ESP_BT_ACL_PKT_TYPES_MASK_DM1 | ESP_BT_ACL_PKT_TYPES_MASK_DH1 |
                        ESP_BT_ACL_PKT_TYPES_MASK_DM3 | ESP_BT_ACL_PKT_TYPES_MASK_DH3 |
                        ESP_BT_ACL_PKT_TYPES_MASK_DM5 | ESP_BT_ACL_PKT_TYPES_MASK_DH5 |
                        ESP_BT_ACL_PKT_TYPES_MASK_NO_3_DH1 |
                        ESP_BT_ACL_PKT_TYPES_MASK_NO_3_DH3 |
                        ESP_BT_ACL_PKT_TYPES_MASK_NO_3_DH5);
                conn_hdl = param->conn_stat.conn_hdl;
                link_mtu = param->conn_stat.audio_mtu;
                /* Not set up here: Bluedroid reports the connection before
                 * it counts it as open, and setting up asks about the
                 * link. One of the caps events below follows. */
                link_open = true;
                check_stream_cfg();
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED || param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTING) {
                bt_sink_lost();
            }
            return;
        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            logf("a2d %d %d %d", event, param->media_ctrl_stat.cmd, param->media_ctrl_stat.status);
            if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
                sink_active = true;
                pcm_switch_sink(PCM_SINK_BLUETOOTH);
                sound_set_balance(global_settings.balance);
#ifdef HAVE_HEADPHONE_DETECTION
                /* resumes what losing it paused, as plugging headphones in */
                queue_broadcast(SYS_BLUETOOTH_SINK_READY, 0);
#endif
            } else if (pref_wait_suspend && param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
                pref_wait_suspend = false;
                pref_next();
            }
            if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND && param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                bt_current_state = ESP_A2D_AUDIO_STATE_SUSPEND;
                bt_log_add(BT_LOG_STOP, 0);
                bt_log_save_soon();
            } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START && param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                bt_current_state = ESP_A2D_AUDIO_STATE_STARTED;
                bt_log_add(BT_LOG_START, 0);
                timeout_cancel(&bt_log_save_tmo);
                current_codec->feeding_reset();
                create_thread(bt_codec_task, bt_codec_stack, DEFAULT_STACK_SIZE, 0, "bt_codec" IF_PRIO(, PRIORITY_BLUETOOTH));
            }
            struct queue_event ev;
            queue_wait_w_tmo(&sink_control_queue, &ev, TIMEOUT_NOBLOCK);
            if (ev.id == 1) {
                esp_a2d_media_ctrl(ev.data);
            } else if (ev.id == SYS_TIMEOUT) {
                semaphore_release(&sink_control_sem);
            }
            return;
        case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT:
            /* Sent once the connection is open if the caps were fetched
             * before, the all caps event comes later otherwise */
            codec_setup();
            break;
        case ESP_A2D_REPORT_SNK_ALL_CODEC_CAPS_EVT:
            logf("a2d %d %d", event, param->a2d_report_snk_all_codec_caps_stat.sep_num);
            /* the encoder is running on what was picked already */
            if (codec_ready)
                break;
            codec_pick(param->a2d_report_snk_all_codec_caps_stat.sep_mcc,
                       param->a2d_report_snk_all_codec_caps_stat.sep_num);
            codec_setup();
            break;
        case ESP_A2D_AUDIO_CFG_EVT:
            logf("a2d cfg %08lx", (unsigned long)
                 A2D_GetCodecType((const uint8_t *)&param->audio_cfg.mcc));
            conn_hdl = param->audio_cfg.conn_hdl;
            stream_cfg = param->audio_cfg.mcc;
            stream_cfg_valid = true;
            check_stream_cfg();
            return;
        case ESP_A2D_SRC_SET_PREF_MCC_EVT:
            logf("a2d %d %d", event, param->a2d_set_pref_mcc_stat.set_status);
            pref_busy = false;
            /* A new config gets reported, check that one. A request that
             * changes nothing succeeds with no report: the stream stands. */
            if (param->a2d_set_pref_mcc_stat.set_status == ESP_BT_STATUS_SUCCESS &&
                !(stream_cfg_valid && pref_matches_stream(&sent_pref_mcc)))
                stream_cfg_valid = false;
            pref_next();
            check_stream_cfg();
            return;
        case ESP_A2D_PROF_STATE_EVT:
            if (param->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS)
                bt_core_prof_down();
            return;
        case ESP_A2D_AUDIO_STATE_EVT:
            logf("a2d %d %d\n", event, param->audio_stat.state);
        default:
            logf("a2d %d\n", event);
        return;
    }
}

/* What plays, for the AVRCP controller on the other side (headphones, a
 * car): the track's tags and the play status when it asks, and the
 * notifications it registers for, track change, play status and play
 * position. Changes are looked for once a second while it's connected. */

#define POLL_MS     1000
#define TAG_LEN     104     /* past what is sent: btc_avrc.c cuts at a character */

static char title[TAG_LEN], artist[TAG_LEN], album[TAG_LEN], genre[TAG_LEN];

/* Registered notifications, a CHANGED response ends one */
static bool ntf_status, ntf_track, ntf_pos;
static uint32_t pos_interval_ms;
static long pos_next;

/* What was last reported */
static esp_avrc_playback_stat_t last_status;
static char last_path[MAX_PATH];

static bool tg_connected;
static osi_alarm_t *poll_alarm;

static esp_avrc_playback_stat_t play_status(void)
{
    int st = audio_status();
    if (!(st & AUDIO_STATUS_PLAY))
        return ESP_AVRC_PLAYBACK_STOPPED;
    return (st & AUDIO_STATUS_PAUSE) ? ESP_AVRC_PLAYBACK_PAUSED
                                     : ESP_AVRC_PLAYBACK_PLAYING;
}

static struct mp3entry *cur_track(void)
{
    return play_status() == ESP_AVRC_PLAYBACK_STOPPED ? NULL
                                                      : audio_current_track();
}

static void copy_tag(char *dst, const char *src)
{
    strmemccpy(dst, src ? src : "", TAG_LEN);
}

/* On the BTC task, see esp_avrc_tg_register_track_info_callback() */
static void track_info(esp_avrc_tg_track_info_t *info)
{
    struct mp3entry *id3 = cur_track();

    info->status = play_status();
    if (!id3)
        return;

    /* untagged: the file name */
    const char *name = id3->title;
    if (!name || !name[0])
    {
        name = strrchr(id3->path, '/');
        name = name ? name + 1 : id3->path;
    }
    copy_tag(title, name);
    copy_tag(artist, id3->artist);
    copy_tag(album, id3->album);
    copy_tag(genre, id3->genre_string);

    info->title = title;
    info->artist = artist;
    info->album = album;
    info->genre = genre;
    info->track_num = id3->tracknum > 0 ? id3->tracknum : 0;
    info->length_ms = id3->length ? id3->length : 0xffffffff;
    info->position_ms = id3->elapsed;
}

static void send_status(esp_avrc_rn_rsp_t rsp)
{
    esp_avrc_rn_param_t p;
    memset(&p, 0, sizeof(p));
    p.playback = last_status = play_status();
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE, rsp, &p);
}

/* AVRCP 1.3: 0 for a track selected, all ones for none */
static void send_track(esp_avrc_rn_rsp_t rsp)
{
    struct mp3entry *id3 = cur_track();
    esp_avrc_rn_param_t p;
    memset(&p, 0, sizeof(p));
    if (!id3)
        memset(p.elm_id, 0xff, sizeof(p.elm_id));
    strmemccpy(last_path, id3 ? id3->path : "", sizeof(last_path));
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_TRACK_CHANGE, rsp, &p);
}

static void send_pos(esp_avrc_rn_rsp_t rsp)
{
    struct mp3entry *id3 = cur_track();
    esp_avrc_rn_param_t p;
    memset(&p, 0, sizeof(p));
    p.play_pos = id3 ? id3->elapsed : 0xffffffff;
    pos_next = current_tick + pos_interval_ms * HZ / 1000;
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_POS_CHANGED, rsp, &p);
}

/* On Bluedroid's alarm thread */
static void poll(void *arg)
{
    (void)arg;
    if (!tg_connected)
        return;

    struct mp3entry *id3 = cur_track();
    const char *path = id3 ? id3->path : "";
    bool track_changed = strcmp(path, last_path) != 0;
    bool status_changed = play_status() != last_status;

    if (ntf_track && track_changed)
    {
        ntf_track = false;
        send_track(ESP_AVRC_RN_RSP_CHANGED);
    }
    if (ntf_status && status_changed)
    {
        ntf_status = false;
        send_status(ESP_AVRC_RN_RSP_CHANGED);
    }
    /* the position goes with any change, and as often as asked while
     * playing */
    if (ntf_pos && (track_changed || status_changed ||
        (last_status == ESP_AVRC_PLAYBACK_PLAYING &&
         TIME_AFTER(current_tick, pos_next))))
    {
        ntf_pos = false;
        send_pos(ESP_AVRC_RN_RSP_CHANGED);
    }

    osi_alarm_set(poll_alarm, POLL_MS);
}

static void bt_track_register(uint8_t event_id, uint32_t param)
{
    switch (event_id)
    {
        case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
            ntf_status = true;
            send_status(ESP_AVRC_RN_RSP_INTERIM);
            break;
        case ESP_AVRC_RN_TRACK_CHANGE:
            ntf_track = true;
            send_track(ESP_AVRC_RN_RSP_INTERIM);
            break;
        case ESP_AVRC_RN_PLAY_POS_CHANGED:
            /* the interval, in seconds */
            pos_interval_ms = (param ? param : 1) * 1000;
            ntf_pos = true;
            send_pos(ESP_AVRC_RN_RSP_INTERIM);
            break;
        default:
            break;
    }
}

static void bt_track_connected(bool on)
{
    tg_connected = on;
    ntf_status = ntf_track = ntf_pos = false;
    if (!poll_alarm)
        return;
    if (on)
        osi_alarm_set(poll_alarm, POLL_MS);
    else
        osi_alarm_cancel(poll_alarm);
}

bool bt_track_up(void)
{
    esp_avrc_rn_evt_cap_mask_t evts = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evts,
                                       ESP_AVRC_RN_PLAY_STATUS_CHANGE);
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evts,
                                       ESP_AVRC_RN_TRACK_CHANGE);
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evts,
                                       ESP_AVRC_RN_PLAY_POS_CHANGED);

    tg_connected = false;
    poll_alarm = osi_alarm_new("bt_track", poll, NULL, 0);
    esp_avrc_tg_register_track_info_callback(track_info);
    return esp_avrc_tg_set_rn_evt_cap(&evts) == ESP_OK;
}

void bt_track_down(void)
{
    esp_avrc_tg_register_track_info_callback(NULL);
    tg_connected = false;
    /* the stack frees it with its other alarms */
    poll_alarm = NULL;
}
void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_TG_PROF_STATE_EVT:
            if (param->avrc_tg_init_stat.state >= ESP_AVRC_DEINIT_SUCCESS)
                bt_core_prof_down();
            else
                bt_core_prof_up(param->avrc_tg_init_stat.state !=
                                ESP_AVRC_INIT_FAIL);
            break;
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            logf("avrc-tg %d %d", event, param->conn_stat.connected);
            bt_track_connected(param->conn_stat.connected);
            break;
        case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
            logf("avrc-tg %d %d %d", event, param->rmt_feats.ct_feat_flag, param->rmt_feats.feat_mask);
            break;
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            logf("avrc-tg %d %d %d", event, param->psth_cmd.key_code, param->psth_cmd.key_state);
            if (param->psth_cmd.key_state == 1) {
                switch (param->psth_cmd.key_code) {
                    /* Separate commands, not a toggle: a headset may send
                     * play on connecting, to playback already resumed */
                    case ESP_AVRC_PT_CMD_PLAY:
                        button_queue_post(BUTTON_MULTIMEDIA_PLAY, 0);
                        break;
                    case ESP_AVRC_PT_CMD_PAUSE:
                        button_queue_post(BUTTON_MULTIMEDIA_PAUSE, 0);
                        break;
                    case ESP_AVRC_PT_CMD_STOP:
                        button_queue_post(BUTTON_MULTIMEDIA_STOP, 0);
                        break;
                    case ESP_AVRC_PT_CMD_FORWARD:
                        button_queue_post(BUTTON_MULTIMEDIA_NEXT, 0);
                        break;
                    case ESP_AVRC_PT_CMD_BACKWARD:
                        button_queue_post(BUTTON_MULTIMEDIA_PREV, 0);
                        break;
                    case ESP_AVRC_PT_CMD_REWIND:
                        button_queue_post(BUTTON_MULTIMEDIA_REW, 0);
                        break;
                    case ESP_AVRC_PT_CMD_FAST_FORWARD:
                        button_queue_post(BUTTON_MULTIMEDIA_FFWD, 0);
                        break;
                }
            }
            break;
        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
            logf("avrc-tg %d %d", event, param->reg_ntf.event_id);
            bt_track_register(param->reg_ntf.event_id,
                              param->reg_ntf.event_parameter);
            break;
        default:
            logf("avrc-tg %d", event);
            return;
    }
}

static const void *pcm_data_start = NULL;
static size_t  pcm_data_size = 0;
static int     audio_locked = 0;
struct pcm_sink bt_pcm_sink;

void bt_sink_init(void) {
}

void bt_sink_postinit(void) {
}

void bt_sink_lock(void) {
    audio_locked++;
}

void bt_sink_unlock(void) {
    audio_locked--;
}

void bt_sink_set_freq(uint16_t freq) {
    esp_a2d_mcc_t mcc;

    logf("sampr switch to %d", freq);
    cur_sampr = bt_pcm_sink.caps.samprs[freq];
    make_pref_mcc(&mcc);
    pref_request(&mcc, true);
}

void bt_sink_play(const void* addr, size_t size) {
    pcm_data_start = addr;
    pcm_data_size = size;
    logf("starting playback: starting to play");
    bt_log_add(BT_LOG_PCM_ON, 0);
    bt_sink_resume();
    pcm_play_dma_status_callback(PCM_DMAST_STARTED);
}

void bt_sink_stop(void) {
    pcm_data_start = NULL;
    pcm_data_size = 0;
    logf("suspending playback: stopped playing");
    bt_log_add(BT_LOG_PCM_OFF, 0);
    bt_sink_suspend();
}

static bool get_new_buf_maybe(void) {
    bool ret = pcm_play_dma_complete_callback(PCM_DMAST_OK, &pcm_data_start, &pcm_data_size);
    if (ret) {
        pcm_play_dma_status_callback(PCM_DMAST_STARTED);
    }
    return ret;
}

bool bt_a2d_register_endpoint(void)
{
    esp_a2d_mcc_t mcc;
    mcc.losc = A2D_SBC_INFO_LEN;
    mcc.media_type = A2D_MEDIA_TYPE_AUDIO;
    mcc.codec_type = ESP_A2D_MCT_SBC;
    mcc.cie.sbc_info.samp_freq    = ESP_A2D_SBC_CIE_SF_44K | ESP_A2D_SBC_CIE_SF_48K;
    mcc.cie.sbc_info.ch_mode      = ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    mcc.cie.sbc_info.block_len    = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    mcc.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    mcc.cie.sbc_info.alloc_mthd   = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    mcc.cie.sbc_info.min_bitpool  = 3;
    mcc.cie.sbc_info.max_bitpool  = 53;
    return esp_a2d_source_register_stream_endpoint(0, &mcc) == ESP_OK;
}

size_t bt_read_pcm(uint8_t *buf, size_t len) {
    if (len <= 0 || buf == NULL) {
        return 0;
    }
    size_t written = 0;
    if (pcm_data_start != NULL && audio_locked == 0) {
        int32_t wanted = len;
        while (len > 0 && (pcm_data_size > 0 || get_new_buf_maybe())) {
            size_t sent = MIN(pcm_data_size, len);
            bt_sw_volume_copy_buffer(buf, pcm_data_start, sent);
            pcm_data_start += sent;
            pcm_data_size -= sent;
            buf += sent;
            len -= sent;
            written += sent;
        }
        if ((int32_t)written < wanted) {
            bt_stats.pcm_short++;
        }
    } else {
        // Transmit digital silence
        bt_stats.pcm_silence++;
        written = len;
        memset(buf, 0, len);
    }
    return written;
}

struct pcm_sink bt_pcm_sink = {
    .caps = {
        .samprs       = NULL,
        .num_samprs   = -1,
        .default_freq = -1,
        .volume_type  = -1,
    },
    .ops = {
        .init     = bt_sink_init,
        .postinit = bt_sink_postinit,
        .set_freq = bt_sink_set_freq,
        .lock     = bt_sink_lock,
        .unlock   = bt_sink_unlock,
        .play     = bt_sink_play,
        .stop     = bt_sink_stop,
    },
    .volume = &bt_pcm_sink_volume,
};
/* Receiving audio: the player as a Bluetooth speaker for a phone, with
 * the stack in the A2DP sink role (bt_core.c switches roles).
 *
 * SBC frames come in on Bluedroid's sink task and are decoded into a ring
 * of PCM blocks, which the mixer's PCM_MIXER_CHAN_BTAUDIO plays from once
 * a few are filled. The two clocks drift apart: a full ring drops what
 * comes in, an empty one stops the channel until it has filled again.
 * The phone is controlled, and its track shown, over AVRCP. */

#define RX_BLOCK_BYTES     4096    /* 16-bit stereo, 23 ms at 44.1 kHz */
#define RX_NUM_BLOCKS      24
#define RX_PREBUFFER       6       /* blocks before the channel starts */

/* AVRCP transaction labels, apart from bt_volume.c's */
#define TL_PASSTHROUGH  4
#define TL_METADATA     5
#define TL_NTF_TRACK    6
#define TL_NTF_STATUS   7

static int16_t rx_blocks[RX_NUM_BLOCKS][RX_BLOCK_BYTES / 2] MEM_ALIGN_ATTR;
static volatile int rx_rd_idx;     /* the mixer's block */
static volatile int rx_filled;     /* blocks filled, from rx_rd_idx on */
static volatile bool rx_block_out; /* the mixer has rx_blocks[rx_rd_idx] */
static volatile bool rx_mixing;    /* the channel is playing */
static int rx_wr_idx;
static size_t rx_wr_pos;

static OI_CODEC_SBC_DECODER_CONTEXT rx_dec;
static uint32_t rx_dec_data[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];
static int16_t rx_pcm[SBC_MAX_SAMPLES_PER_FRAME * SBC_MAX_CHANNELS];
static bool rx_dec_ready;
static int rx_channels;

static uint8_t rx_peer[BT_ADDR_LEN];
static bool rx_connected, rx_streaming;
static bool rx_rc_connected;
static esp_avrc_playback_stat_t rx_phone_status;
static char rx_title[96], rx_artist[96];

/* debug counters */
static unsigned long rx_underruns, rx_overruns, rx_decode_errors;

static void rx_changed(void)
{
    button_queue_post(BUTTON_REDRAW, 0);
}

/***********************************/
/*    PCM RING                     */

/* In the mixer's interrupt: the block it had is done, the next one */
static void rx_get_more(const void **start, size_t *size)
{
    if (rx_block_out)
    {
        rx_block_out = false;
        rx_rd_idx = (rx_rd_idx + 1) % RX_NUM_BLOCKS;
        rx_filled--;
    }

    if (rx_filled == 0)
    {
        rx_underruns++;
        rx_mixing = false;
        *start = NULL;
        *size = 0;
        return;
    }

    rx_block_out = true;
    *start = rx_blocks[rx_rd_idx];
    *size = RX_BLOCK_BYTES;
}

static const struct mixer_play_cbs rx_play_cbs = {
    .get_more = rx_get_more,
};

static void rx_ring_reset(void)
{
    mixer_channel_stop(PCM_MIXER_CHAN_BTAUDIO);
    rx_mixing = false;
    rx_block_out = false;
    rx_rd_idx = rx_wr_idx = 0;
    rx_wr_pos = 0;
    rx_filled = 0;
}

/* On the sink task: 16-bit stereo into the ring */
static void rx_put_pcm(const int16_t *data, size_t bytes)
{
    while (bytes > 0)
    {
        /* the block being written must not be one the mixer may get */
        if (rx_filled >= RX_NUM_BLOCKS - 1)
        {
            rx_overruns++;
            return;
        }

        size_t n = MIN(bytes, RX_BLOCK_BYTES - rx_wr_pos);
        memcpy((uint8_t *)rx_blocks[rx_wr_idx] + rx_wr_pos, data, n);
        rx_wr_pos += n;
        data += n / 2;
        bytes -= n;

        if (rx_wr_pos == RX_BLOCK_BYTES)
        {
            rx_wr_pos = 0;
            rx_wr_idx = (rx_wr_idx + 1) % RX_NUM_BLOCKS;
            int irq = disable_irq_save();
            rx_filled++;
            restore_irq(irq);
        }
    }

    if (!rx_mixing && rx_filled >= RX_PREBUFFER)
    {
        int irq = disable_irq_save();
        rx_mixing = true;
        rx_block_out = false;
        restore_irq(irq);
        mixer_channel_play_data(PCM_MIXER_CHAN_BTAUDIO, &rx_play_cbs, NULL, 0);
    }
}

/***********************************/
/*    SBC                          */

static void rx_stream_config(const esp_a2d_mcc_t *mcc)
{
    rx_ring_reset();
    rx_dec_ready = false;
    if (mcc->codec_type != ESP_A2D_MCT_SBC)
        return;

    uint8_t sf = mcc->cie.sbc_info.samp_freq;
    unsigned long sampr = (sf & ESP_A2D_SBC_CIE_SF_48K) ? SAMPR_48 :
                          (sf & ESP_A2D_SBC_CIE_SF_44K) ? SAMPR_44 :
                          (sf & ESP_A2D_SBC_CIE_SF_32K) ? SAMPR_32 : SAMPR_16;
    rx_channels = mcc->cie.sbc_info.ch_mode == ESP_A2D_SBC_CIE_CH_MODE_MONO ? 1 : 2;

    rx_dec_ready = OI_SUCCESS(OI_CODEC_SBC_DecoderReset(&rx_dec, rx_dec_data,
                                sizeof(rx_dec_data), 2, rx_channels, FALSE));
    mixer_set_frequency(sampr);
    logf("bt rx: SBC %lu Hz, %d ch", sampr, rx_channels);
}

/* On the sink task */
static void rx_audio_data(esp_a2d_conn_hdl_t conn_hdl, esp_a2d_audio_buff_t *buf)
{
    (void)conn_hdl;

    if (rx_dec_ready && rx_streaming)
    {
        const OI_BYTE *p = buf->data;
        uint32_t left = buf->data_len;

        for (int i = 0; i < buf->number_frame && left > 0; i++)
        {
            /* room to spread mono out to stereo */
            uint32_t pcm_bytes = rx_channels == 1 ? sizeof(rx_pcm) / 2 : sizeof(rx_pcm);
            if (!OI_SUCCESS(OI_CODEC_SBC_DecodeFrame(&rx_dec, &p, &left,
                                                     rx_pcm, &pcm_bytes)))
            {
                rx_decode_errors++;
                break;
            }

            if (rx_channels == 1)
            {
                /* out to stereo, from the end */
                size_t n = pcm_bytes / 2;
                for (size_t j = n; j-- > 0; )
                    rx_pcm[2*j] = rx_pcm[2*j + 1] = rx_pcm[j];
                pcm_bytes *= 2;
            }
            rx_put_pcm(rx_pcm, pcm_bytes);
        }
    }

    esp_a2d_audio_buff_free(buf);
}

/***********************************/
/*    A2DP AND AVRCP               */

void bt_receive_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event)
    {
        case ESP_A2D_CONNECTION_STATE_EVT:
            logf("bt rx: a2dp %d", param->conn_stat.state);
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
            {
                memcpy(rx_peer, param->conn_stat.remote_bda, BT_ADDR_LEN);
                rx_connected = true;
            }
            else if (param->conn_stat.state ==
                     ESP_A2D_CONNECTION_STATE_DISCONNECTED)
            {
                rx_connected = rx_streaming = false;
                rx_ring_reset();
            }
            rx_changed();
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            rx_streaming = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
            if (!rx_streaming)
                rx_ring_reset();
            rx_changed();
            break;
        case ESP_A2D_AUDIO_CFG_EVT:
            rx_stream_config(&param->audio_cfg.mcc);
            break;
        case ESP_A2D_PROF_STATE_EVT:
            if (param->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS)
                bt_core_prof_down();
            break;
        default:
            logf("bt rx: a2dp event %d", event);
            break;
    }
}

static void rx_get_metadata(void)
{
    esp_avrc_ct_send_metadata_cmd(TL_METADATA,
                                  ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
}

void bt_receive_avrc_ct_cb(esp_avrc_ct_cb_event_t event,
                           esp_avrc_ct_cb_param_t *param)
{
    switch (event)
    {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            rx_rc_connected = param->conn_stat.connected;
            rx_title[0] = rx_artist[0] = '\0';
            rx_phone_status = ESP_AVRC_PLAYBACK_STOPPED;
            if (rx_rc_connected)
            {
                rx_get_metadata();
                esp_avrc_ct_send_register_notification_cmd(TL_NTF_TRACK,
                    ESP_AVRC_RN_TRACK_CHANGE, 0);
                esp_avrc_ct_send_register_notification_cmd(TL_NTF_STATUS,
                    ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
            }
            rx_changed();
            break;
        case ESP_AVRC_CT_METADATA_RSP_EVT:
        {
            char *dst = param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_TITLE ? rx_title :
                        param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_ARTIST ? rx_artist :
                        NULL;
            if (!dst)
                break;
            size_t len = MIN((size_t)MAX(param->meta_rsp.attr_length, 0),
                             sizeof(rx_title) - 1);
            memcpy(dst, param->meta_rsp.attr_text, len);
            dst[len] = '\0';
            rx_changed();
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            /* a change ends the registration, the interim one doesn't */
            if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE)
            {
                if (param->change_ntf.is_interim)
                    break;
                rx_get_metadata();
                esp_avrc_ct_send_register_notification_cmd(TL_NTF_TRACK,
                    ESP_AVRC_RN_TRACK_CHANGE, 0);
            }
            else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE)
            {
                rx_phone_status = param->change_ntf.event_parameter.playback;
                if (!param->change_ntf.is_interim)
                    esp_avrc_ct_send_register_notification_cmd(TL_NTF_STATUS,
                        ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
                rx_changed();
            }
            break;
        default:
            logf("bt rx: avrc-ct event %d", event);
            break;
    }
}

/***********************************/
/*    FOR THE CORE AND THE UI      */

bool bt_receive_up(void)
{
    /* everything SBC can do: the phone picks */
    esp_a2d_mcc_t mcc;
    memset(&mcc, 0, sizeof(mcc));
    mcc.losc = A2D_SBC_INFO_LEN;
    mcc.media_type = A2D_MEDIA_TYPE_AUDIO;
    mcc.codec_type = ESP_A2D_MCT_SBC;
    mcc.cie.sbc_info.samp_freq    = ESP_A2D_SBC_CIE_SF_16K | ESP_A2D_SBC_CIE_SF_32K |
                                    ESP_A2D_SBC_CIE_SF_44K | ESP_A2D_SBC_CIE_SF_48K;
    mcc.cie.sbc_info.ch_mode      = ESP_A2D_SBC_CIE_CH_MODE_MONO |
                                    ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL |
                                    ESP_A2D_SBC_CIE_CH_MODE_STEREO |
                                    ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    mcc.cie.sbc_info.block_len    = ESP_A2D_SBC_CIE_BLOCK_LEN_4 | ESP_A2D_SBC_CIE_BLOCK_LEN_8 |
                                    ESP_A2D_SBC_CIE_BLOCK_LEN_12 | ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    mcc.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_4 |
                                    ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    mcc.cie.sbc_info.alloc_mthd   = ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR |
                                    ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    mcc.cie.sbc_info.min_bitpool  = 2;
    mcc.cie.sbc_info.max_bitpool  = 250;

    rx_title[0] = rx_artist[0] = '\0';
    rx_underruns = rx_overruns = rx_decode_errors = 0;
    /* channels start silent */
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BTAUDIO, MIX_AMP_UNITY);

    return esp_a2d_sink_register_stream_endpoint(0, &mcc) == ESP_OK &&
           esp_a2d_sink_register_audio_data_callback(rx_audio_data) == ESP_OK;
}

void bt_receive_down(void)
{
    rx_ring_reset();
    rx_connected = rx_streaming = false;
    rx_rc_connected = false;
    rx_dec_ready = false;
}

const uint8_t *bt_receive_peer(void)
{
    return rx_connected ? rx_peer : NULL;
}

void bt_receive_status(struct bt_receive_status *st)
{
    st->streaming = rx_streaming;
    strmemccpy(st->title, rx_title, sizeof(st->title));
    strmemccpy(st->artist, rx_artist, sizeof(st->artist));
}

void bt_receive_control(enum bt_receive_cmd cmd)
{
    uint8_t key;

    if (!rx_rc_connected)
        return;

    switch (cmd)
    {
        case BT_RECEIVE_NEXT:
            key = ESP_AVRC_PT_CMD_FORWARD;
            break;
        case BT_RECEIVE_PREV:
            key = ESP_AVRC_PT_CMD_BACKWARD;
            break;
        default:
            key = (rx_streaming || rx_phone_status == ESP_AVRC_PLAYBACK_PLAYING) ?
                  ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY;
            break;
    }

    esp_avrc_ct_send_passthrough_cmd(TL_PASSTHROUGH, key,
                                     ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(TL_PASSTHROUGH, key,
                                     ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void bt_receive_debug(char *buf, size_t len)
{
    snprintf(buf, len, "rx fill %d under %lu over %lu err %lu",
             rx_filled, rx_underruns, rx_overruns, rx_decode_errors);
}
