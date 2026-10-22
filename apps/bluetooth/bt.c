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
#include "system.h"
#include "thread.h"
#include "tick.h"
#include "tlsf.h"
#include "misc.h"
#include "osi/bt_stats.h"
#include "bt.h"
#include "bt-internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct a2dp_codec *current_codec = NULL;

#define NUM_CODECS 4

static struct a2dp_codec *codec_list[NUM_CODECS] = {
    &a2dp_codec_ldac,
    &a2dp_codec_aptx_hd,
    &a2dp_codec_aptx,
    &a2dp_codec_sbc,
};

extern unsigned char btbuf[];

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
            return ACTION_REDRAW;
        default:
            return action;
    }
}

#define BT_INFO_FIXED_ITEMS 5

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
    default:
        return NULL;
    }

    return buf;
}

#define BT_INFO_STATS_ITEMS 38

const char * bt_heap_info_getname(int selected_item, void * data, char * buffer, size_t buffer_len)
{
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
        case 2:
            if (current_codec == &a2dp_codec_ldac) {
                return "current codec: LDAC";
            } else if (current_codec == &a2dp_codec_aptx_hd) {
                return "current codec: aptX HD";
            } else if (current_codec == &a2dp_codec_aptx) {
                return "current codec: aptX";
            } else if (current_codec == &a2dp_codec_sbc) {
                return "current codec: SBC";
            } else {
                return "current codec: unknown";
            }
        case 3:
            if (current_codec != NULL && current_codec->debug_info != NULL) {
                strcpy(buffer, "codec info: ");
                current_codec->debug_info(buffer + 12, buffer_len - 12);
                return buffer;
            } else {
                return "codec info: N/A";
            }
        case 4:
            snprintf(buffer, buffer_len, "real sink sampr: %lu", pcm_current_sink_caps()->samprs[pcm_get_current_sink()->configured_freq]);
            return buffer;
        default:
            return "Unknown item!!";
    }
}

bool bt_heap_info(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, "Bluetooth debug info:",
                         BT_INFO_FIXED_ITEMS + BT_INFO_STATS_ITEMS, NULL);
    info.scroll_all = false;
    info.action_callback = bt_heap_info_action_callback;
    info.get_name = bt_heap_info_getname;
    return simplelist_show_list(&info);
}

static esp_a2d_audio_state_t bt_current_state = ESP_A2D_AUDIO_STATE_SUSPEND;

uint32_t bt_codec_stack[DEFAULT_STACK_SIZE / sizeof(uint32_t)];


/* Media tick stats: how late each tick ran and how long its work took.
 * The gap is invalid until the first tick after the stream starts. */
static bt_stats_time_t tick_last;
static bool tick_valid;

static void bt_codec_tick(void)
{
    bt_stats_time_t t = bt_stats_now();

    bt_stats.a2d_ticks++;
    if (tick_valid) {
        uint32_t gap = BT_STATS_US(t - tick_last);
        bt_stats_max(&bt_stats.a2d_tick_gap_us_max, gap);
        if (gap > current_codec->get_period() * 1500) {
            bt_stats.a2d_tick_late++;
        }
    }
    tick_last = t;
    tick_valid = true;

    current_codec->tick();

    bt_stats_max(&bt_stats.a2d_work_us_max, BT_STATS_US(bt_stats_now() - t));
    bt_stats_max(&bt_stats.a2d_q_max, esp_a2d_source_audio_queue_len());
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
            next_ms += current_codec->get_period();
            ticked += current_codec->get_period();
        } while ((next_ms * HZ) / 1000 <= current_tick);
        sleep(((next_ms * HZ) / 1000) - current_tick);
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

static int bt_volume = 127;
static int bt_balance = 0;
static bool bt_has_avrcp_abs_vol = false;

static void bt_update_volume(void);

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

/* Our SBC config at the stream's sampling frequency, bitpool capped by
 * the device's codec option */
static bool make_pref_mcc(esp_a2d_mcc_t *mcc, uint8_t samp_freq) {
    unsigned long sampr;

    if (current_codec != &a2dp_codec_sbc)
        return false;

    switch (samp_freq) {
        case ESP_A2D_SBC_CIE_SF_48K: sampr = SAMPR_48; break;
        case ESP_A2D_SBC_CIE_SF_44K: sampr = SAMPR_44; break;
        case ESP_A2D_SBC_CIE_SF_32K: sampr = SAMPR_32; break;
        case ESP_A2D_SBC_CIE_SF_16K: sampr = SAMPR_16; break;
        default: return false;
    }

    current_codec->set_freq(sampr, mcc);
    return true;
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

static void pref_next(void) {
    if (pref_busy || pref_wait_suspend)
        return;

    while (pref_again) {
        pref_again = false;
        if (esp_a2d_source_set_pref_mcc(conn_hdl, &new_pref_mcc) == ESP_OK) {
            pref_busy = true;
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

/* The stream config as last reported. A sink that sets up the stream
 * itself (it connected to us) picks the bitpool range; ask once per
 * connection for ours instead. */
static esp_a2d_cie_sbc_t stream_cfg;
static bool stream_cfg_valid;
static bool bitpool_checked;

static void check_stream_bitpool(void) {
    if (bitpool_checked || !stream_cfg_valid || !bt_core_peer())
        return;
    /* A change in progress brings our ceiling anyway, check what it ends
     * up with */
    if (pref_busy || pref_again || pref_wait_suspend)
        return;
    bitpool_checked = true;

    if (stream_cfg.max_bitpool >= bt_core_max_bitpool())
        return;

    esp_a2d_mcc_t mcc;
    if (!make_pref_mcc(&mcc, stream_cfg.samp_freq))
        return;

    logf("bitpool capped at %d by the sink, reconfiguring", stream_cfg.max_bitpool);
    pref_request(&mcc, true);
}

void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            logf("a2d %d %d", event, param->conn_stat.state);
            bt_core_a2d_conn_state(param->conn_stat.state,
                                   param->conn_stat.remote_bda);
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                semaphore_init(&sink_control_sem, 1, 0);
                queue_init(&sink_control_queue, false);
                sink_suspended = 1;
                pref_held = pref_wait_suspend = false;
                src_rdy_sent = false;
                bt_current_state = ESP_A2D_AUDIO_STATE_SUSPEND;
                bitpool_checked = false;
                check_stream_bitpool();
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED || param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTING) {
                stream_cfg_valid = false;
                pref_reset();
                if (current_codec != NULL) {
                    bt_current_state = ESP_A2D_AUDIO_STATE_SUSPEND;
                    current_codec->deinit();
                    current_codec = NULL;
                    pcm_switch_sink(PCM_SINK_BUILTIN);
                    sound_set_balance(global_settings.balance);
                    bt_has_avrcp_abs_vol = false;
                    bt_volume = 127;
                }
            }
            return;
        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            logf("a2d %d %d %d", event, param->media_ctrl_stat.cmd, param->media_ctrl_stat.status);
            if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
                pcm_switch_sink(PCM_SINK_BLUETOOTH);
                sound_set_balance(global_settings.balance);
            } else if (pref_wait_suspend && param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
                pref_wait_suspend = false;
                pref_next();
            }
            if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND && param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                bt_current_state = ESP_A2D_AUDIO_STATE_SUSPEND;
            } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START && param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                bt_current_state = ESP_A2D_AUDIO_STATE_STARTED;
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
        case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT: {
            esp_a2d_mcc_t pref_mcc;
            struct a2dp_peer_info peer_info = {
                .peer_mtu = param->a2d_report_snk_codec_caps_stat.mtu,
                .peer_caps = &peer_caps,
                .conn_hdl = param->a2d_report_snk_codec_caps_stat.conn_hdl,
                .is_edr = btc_av_is_peer_edr(),
            };
            conn_hdl = param->a2d_report_snk_codec_caps_stat.conn_hdl;
            current_codec->init(&bt_pcm_sink.caps, &peer_info, &pref_mcc);
            /* the stream isn't open yet, nothing to hold */
            pref_request(&pref_mcc, false);
            break;
        }
        case ESP_A2D_REPORT_SNK_ALL_CODEC_CAPS_EVT: {
            logf("a2d %d %d", event, param->a2d_report_snk_all_codec_caps_stat.sep_num);
            bool done = false;
            for (int codec_idx = 0; codec_idx < NUM_CODECS; codec_idx++) {
                current_codec = codec_list[codec_idx];
                for (uint32_t i = 0; i < param->a2d_report_snk_all_codec_caps_stat.sep_num; i++) {
                    esp_a2d_mcc_t *sink_mcc = &param->a2d_report_snk_all_codec_caps_stat.sep_mcc[i].mcc;
                    if (current_codec->is_acceptable(sink_mcc)) {
                        memcpy(&peer_caps, sink_mcc, sizeof(esp_a2d_mcc_t));
                        done = true;
                        break;
                    }
                }
                if (done) {
                    break;
                }
            }
            if (!done) { 
                panicf("Unable to initialize suitable codec");
            }
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT:
            if (param->audio_cfg.mcc.codec_type == ESP_A2D_MCT_SBC) {
                logf("a2d cfg bitpool %d-%d",
                     param->audio_cfg.mcc.cie.sbc_info.min_bitpool,
                     param->audio_cfg.mcc.cie.sbc_info.max_bitpool);
                conn_hdl = param->audio_cfg.conn_hdl;
                stream_cfg = param->audio_cfg.mcc.cie.sbc_info;
                stream_cfg_valid = true;
                check_stream_bitpool();
            }
            return;
        case ESP_A2D_SRC_SET_PREF_MCC_EVT:
            logf("a2d %d %d", event, param->a2d_set_pref_mcc_stat.set_status);
            pref_busy = false;
            /* a new config gets reported, check the bitpool on that one */
            if (param->a2d_set_pref_mcc_stat.set_status == ESP_BT_STATUS_SUCCESS)
                stream_cfg_valid = false;
            pref_next();
            check_stream_bitpool();
            return;
        case ESP_A2D_AUDIO_STATE_EVT:
            logf("a2d %d %d\n", event, param->audio_stat.state);
        default:
            logf("a2d %d\n", event);
        return;
    }
}

void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            logf("avrc-ct %d %d\n", event, param->conn_stat.connected);
            if (param->conn_stat.connected) {
                esp_err_t err = esp_avrc_ct_send_register_notification_cmd(4, ESP_AVRC_RN_VOLUME_CHANGE, 0);
                if (err != ESP_OK) {
                    panicf("error registering for volume change: %d", err);
                }
            }
            // Don't worry about disconnect events; if there's a serious problem
            // then the entire bluetooth connection will drop out, which is handled
            // elsewhere.
            break;
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            logf("avrc-ct %d %d\n", event, param->change_ntf.event_id);
            if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                logf("avrc-ct new vol %d\n", param->change_ntf.event_parameter.volume);
                if (bt_volume != param->change_ntf.event_parameter.volume) {
                    global_status.last_volume_change = current_tick;
                }
                bt_volume = param->change_ntf.event_parameter.volume;
                if (!bt_has_avrcp_abs_vol) {
                    bt_has_avrcp_abs_vol = true;
                    bt_update_volume();
                }
                if (!param->change_ntf.is_interim) {
                    // Resubscribe to volume facts
                    esp_err_t err = esp_avrc_ct_send_register_notification_cmd(4, ESP_AVRC_RN_VOLUME_CHANGE, 0);
                    if (err != ESP_OK) {
                        panicf("error registering for volume change: %d", err);
                    }
                }
            }
            break;
        case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
            logf("avrc-ct %d %d\n", event, param->psth_rsp.key_code);
            if (param->psth_rsp.key_state == 0) { 
                esp_err_t err = esp_avrc_ct_send_passthrough_cmd(0, param->psth_rsp.key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
                if (err != ESP_OK) {
                    panicf("error sending volume change: %d", err);
                }
            }
            break;
        default:
            logf("avrc-ct %d\n", event);
            return;
    }
}

void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            logf("avrc-tg %d %d", event, param->conn_stat.connected);
            break;
        case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
            logf("avrc-tg %d %d %d", event, param->rmt_feats.ct_feat_flag, param->rmt_feats.feat_mask);
            break;
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            logf("avrc-tg %d %d %d", event, param->psth_cmd.key_code, param->psth_cmd.key_state);
            if (param->psth_cmd.key_state == 1) {
                switch (param->psth_cmd.key_code) {
                    case ESP_AVRC_PT_CMD_PLAY:
                    case ESP_AVRC_PT_CMD_PAUSE:
                        button_queue_post(BUTTON_MULTIMEDIA_PLAYPAUSE, 0);
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
    current_codec->set_freq(bt_pcm_sink.caps.samprs[freq], &mcc);
    pref_request(&mcc, true);
}

void bt_sink_play(const void* addr, size_t size) {
    pcm_data_start = addr;
    pcm_data_size = size;
    logf("starting playback: starting to play");
    bt_sink_resume();
    pcm_play_dma_status_callback(PCM_DMAST_STARTED);
}

void bt_sink_stop(void) {
    pcm_data_start = NULL;
    pcm_data_size = 0;
    logf("suspending playback: stopped playing");
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

static void bt_update_volume(void) {
    if (bt_has_avrcp_abs_vol) {
        esp_err_t err = esp_avrc_ct_send_set_absolute_volume_cmd(2, bt_volume);
        if (err != ESP_OK) {
            panicf("error sending volume change: %d", err);
        }
        int l = 0;
        int r = 0;
        if (bt_balance > 0) {
            l = (-730 * bt_balance) / 100;
        } else if (bt_balance < 0) {
            r = (730 * bt_balance) / 100;
        }
        bt_sw_volume_set_volume(l, r);
    } else {
        if (bt_volume == 0) {
            bt_sw_volume_set_volume(-730, -730);
            return;
        }
        int volume = (bt_volume - 127) * 5;
        int l = volume, r = volume;

        if (bt_balance > 0) {
            l -= (l + 730) * bt_balance / 100;
        } else if (bt_balance < 0) {
            r += (r + 730) * bt_balance / 100;
        }
        bt_sw_volume_set_volume(l, r);
    }
}

static int bt_get_volume(void) {
    return bt_volume - 127;
}

static void bt_set_volume(int volume) {
    bt_volume = volume + 127;
    bt_update_volume();
}

static void bt_set_balance(int balance) {
    bt_balance = balance;
    bt_update_volume();
}

static void bt_adjust_volume(int steps) {
    if (bt_has_avrcp_abs_vol) {
        uint8_t cmd = steps > 0 ? ESP_AVRC_PT_CMD_VOL_UP : ESP_AVRC_PT_CMD_VOL_DOWN;
        esp_err_t err = esp_avrc_ct_send_passthrough_cmd(0, cmd, ESP_AVRC_PT_CMD_STATE_PRESSED);
        if (err != ESP_OK) {
            panicf("error sending volume change: %d", err);
        }
    } else {
        int new_volume = bt_volume + steps;
        if (new_volume < 0) {
            new_volume = 0;
        } else if (new_volume > 127) {
            new_volume = 127;
        }
        bt_volume = new_volume;
        bt_update_volume();
    }
}

struct pcm_sink_volume bt_pcm_sink_volume = {
    .volume_min = -127,
    .volume_max = 0,
    .volume_default = 0,
    .volume_step = 1,
    .volume_numdecimals = 0,
    .volume_unit = "",
    .get_volume = bt_get_volume,
    .set_volume = bt_set_volume,
    .set_balance = bt_set_balance,
    .adjust_volume = bt_adjust_volume,
};

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