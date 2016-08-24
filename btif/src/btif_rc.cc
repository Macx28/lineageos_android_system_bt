/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*****************************************************************************
 *
 *  Filename:      btif_rc.c
 *
 *  Description:   Bluetooth AVRC implementation
 *
 *****************************************************************************/

#define LOG_TAG "bt_btif_avrc"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <hardware/bluetooth.h>
#include <hardware/bt_rc.h>

#include "avrc_defs.h"
#include "bta_api.h"
#include "bta_av_api.h"
#include "btif_av.h"
#include "btif_common.h"
#include "btif_util.h"
#include "bt_common.h"
#include "device/include/interop.h"
#include "uinput.h"
#include "bdaddr.h"
#include "osi/include/list.h"
#include "osi/include/properties.h"
#include "btu.h"
#define RC_INVALID_TRACK_ID (0xFFFFFFFFFFFFFFFFULL)

/*****************************************************************************
**  Constants & Macros
******************************************************************************/

/* cod value for Headsets */
#define COD_AV_HEADSETS        0x0404
/* for AVRC 1.4 need to change this */
#define MAX_RC_NOTIFICATIONS AVRC_EVT_VOLUME_CHANGE

#define IDX_GET_PLAY_STATUS_RSP   0
#define IDX_LIST_APP_ATTR_RSP     1
#define IDX_LIST_APP_VALUE_RSP    2
#define IDX_GET_CURR_APP_VAL_RSP  3
#define IDX_SET_APP_VAL_RSP       4
#define IDX_GET_APP_ATTR_TXT_RSP  5
#define IDX_GET_APP_VAL_TXT_RSP   6
#define IDX_GET_ELEMENT_ATTR_RSP  7
#define MAX_VOLUME 128
#define MAX_LABEL 16
#define MAX_TRANSACTIONS_PER_SESSION 16
#define MAX_CMD_QUEUE_LEN 8
#define PLAY_STATUS_PLAYING 1

#define CHECK_RC_CONNECTED                                                                  \
    BTIF_TRACE_DEBUG("## %s ##", __func__);                                            \
    if (btif_rc_cb.rc_connected == false)                                                    \
    {                                                                                       \
        BTIF_TRACE_WARNING("Function %s() called when RC is not connected", __func__); \
        return BT_STATUS_NOT_READY;                                                         \
    }

#define FILL_PDU_QUEUE(index, ctype, label, pending)        \
{                                                           \
    btif_rc_cb.rc_pdu_info[index].ctype = ctype;            \
    btif_rc_cb.rc_pdu_info[index].label = label;            \
    btif_rc_cb.rc_pdu_info[index].is_rsp_pending = pending; \
}

#define SEND_METAMSG_RSP(index, avrc_rsp)                                                      \
{                                                                                              \
    if (btif_rc_cb.rc_pdu_info[index].is_rsp_pending == false)                                  \
    {                                                                                          \
        BTIF_TRACE_WARNING("%s Not sending response as no PDU was registered", __func__); \
        return BT_STATUS_UNHANDLED;                                                            \
    }                                                                                          \
    send_metamsg_rsp(btif_rc_cb.rc_handle, btif_rc_cb.rc_pdu_info[index].label,                \
        btif_rc_cb.rc_pdu_info[index].ctype, avrc_rsp);                                        \
    btif_rc_cb.rc_pdu_info[index].ctype = 0;                                                   \
    btif_rc_cb.rc_pdu_info[index].label = 0;                                                   \
    btif_rc_cb.rc_pdu_info[index].is_rsp_pending = false;                                      \
}

/*****************************************************************************
**  Local type definitions
******************************************************************************/
typedef struct {
    uint8_t bNotify;
    uint8_t label;
} btif_rc_reg_notifications_t;

typedef struct
{
    uint8_t   label;
    uint8_t   ctype;
    bool is_rsp_pending;
} btif_rc_cmd_ctxt_t;

/* 2 second timeout to get interim response */
#define BTIF_TIMEOUT_RC_INTERIM_RSP_MS     (2 * 1000)
#define BTIF_TIMEOUT_RC_STATUS_CMD_MS      (2 * 1000)
#define BTIF_TIMEOUT_RC_CONTROL_CMD_MS     (2 * 1000)


typedef enum
{
    eNOT_REGISTERED,
    eREGISTERED,
    eINTERIM
} btif_rc_nfn_reg_status_t;

typedef struct {
    uint8_t                       event_id;
    uint8_t                       label;
    btif_rc_nfn_reg_status_t    status;
} btif_rc_supported_event_t;

#define BTIF_RC_STS_TIMEOUT     0xFE
typedef struct {
    uint8_t   label;
    uint8_t   pdu_id;
} btif_rc_status_cmd_timer_t;

typedef struct {
    uint8_t   label;
    uint8_t   pdu_id;
} btif_rc_control_cmd_timer_t;

typedef struct {
    union {
        btif_rc_status_cmd_timer_t rc_status_cmd;
        btif_rc_control_cmd_timer_t rc_control_cmd;
    };
} btif_rc_timer_context_t;

typedef struct {
    bool  query_started;
    uint8_t num_attrs;
    uint8_t num_ext_attrs;

    uint8_t attr_index;
    uint8_t ext_attr_index;
    uint8_t ext_val_index;
    btrc_player_app_attr_t attrs[AVRC_MAX_APP_ATTR_SIZE];
    btrc_player_app_ext_attr_t ext_attrs[AVRC_MAX_APP_ATTR_SIZE];
} btif_rc_player_app_settings_t;

/* TODO : Merge btif_rc_reg_notifications_t and btif_rc_cmd_ctxt_t to a single struct */
typedef struct {
    bool                     rc_connected;
    uint8_t                       rc_handle;
    tBTA_AV_FEAT                rc_features;
    BD_ADDR                     rc_addr;
    uint16_t                      rc_pending_play;
    btif_rc_cmd_ctxt_t          rc_pdu_info[MAX_CMD_QUEUE_LEN];
    btif_rc_reg_notifications_t rc_notif[MAX_RC_NOTIFICATIONS];
    unsigned int                rc_volume;
    uint8_t                     rc_vol_label;
    list_t                      *rc_supported_event_list;
    btif_rc_player_app_settings_t   rc_app_settings;
    alarm_t                     *rc_play_status_timer;
    bool                     rc_features_processed;
    uint64_t                      rc_playing_uid;
    bool                     rc_procedure_complete;
} btif_rc_cb_t;

typedef struct {
    bool in_use;
    uint8_t lbl;
    uint8_t handle;
    btif_rc_timer_context_t txn_timer_context;
    alarm_t *txn_timer;
} rc_transaction_t;

typedef struct
{
    pthread_mutex_t lbllock;
    rc_transaction_t transaction[MAX_TRANSACTIONS_PER_SESSION];
} rc_device_t;

rc_device_t device;

#define MAX_UINPUT_PATHS 3
static const char* uinput_dev_path[] =
                       {"/dev/uinput", "/dev/input/uinput", "/dev/misc/uinput" };
static int uinput_fd = -1;

static int  send_event (int fd, uint16_t type, uint16_t code, int32_t value);
static void send_key (int fd, uint16_t key, int pressed);
static int  uinput_driver_check();
static int  uinput_create(const char *name);
static int  init_uinput (void);
static void close_uinput (void);
static void sleep_ms(period_ms_t timeout_ms);

static const struct {
    const char *name;
    uint8_t avrcp;
    uint16_t mapped_id;
    uint8_t release_quirk;
} key_map[] = {
    { "PLAY",         AVRC_ID_PLAY,     KEY_PLAYCD,       1 },
    { "STOP",         AVRC_ID_STOP,     KEY_STOPCD,       0 },
    { "PAUSE",        AVRC_ID_PAUSE,    KEY_PAUSECD,      1 },
    { "FORWARD",      AVRC_ID_FORWARD,  KEY_NEXTSONG,     0 },
    { "BACKWARD",     AVRC_ID_BACKWARD, KEY_PREVIOUSSONG, 0 },
    { "REWIND",       AVRC_ID_REWIND,   KEY_REWIND,       0 },
    { "FAST FORWARD", AVRC_ID_FAST_FOR, KEY_FAST_FORWARD, 0 },
    { NULL,           0,                0,                0 }
};

static void send_reject_response (uint8_t rc_handle, uint8_t label,
    uint8_t pdu, uint8_t status);
static uint8_t opcode_from_pdu(uint8_t pdu);
static void send_metamsg_rsp (uint8_t rc_handle, uint8_t label,
    tBTA_AV_CODE code, tAVRC_RESPONSE *pmetamsg_resp);
#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
static void register_volumechange(uint8_t label);
#endif
static void lbl_init();
static void lbl_destroy();
static void init_all_transactions();
static bt_status_t  get_transaction(rc_transaction_t **ptransaction);
static void release_transaction(uint8_t label);
static rc_transaction_t* get_transaction_by_lbl(uint8_t label);
#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
static void handle_rc_metamsg_rsp(tBTA_AV_META_MSG *pmeta_msg);
#endif
#if (AVRC_CTRL_INCLUDED == TRUE)
static void handle_avk_rc_metamsg_cmd(tBTA_AV_META_MSG *pmeta_msg);
static void handle_avk_rc_metamsg_rsp(tBTA_AV_META_MSG *pmeta_msg);
static void btif_rc_ctrl_upstreams_rsp_cmd(
    uint8_t event, tAVRC_COMMAND *pavrc_cmd, uint8_t label);
static void rc_ctrl_procedure_complete();
static void rc_stop_play_status_timer();
static void register_for_event_notification (btif_rc_supported_event_t *p_event);
static void handle_get_capability_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_CAPS_RSP *p_rsp);
static void handle_app_attr_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_LIST_APP_ATTR_RSP *p_rsp);
static void handle_app_val_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_LIST_APP_VALUES_RSP *p_rsp);
static void handle_app_cur_val_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_CUR_APP_VALUE_RSP *p_rsp);
static void handle_app_attr_txt_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_APP_ATTR_TXT_RSP *p_rsp);
static void handle_app_attr_val_txt_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_APP_ATTR_TXT_RSP *p_rsp);
static void handle_get_playstatus_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_PLAY_STATUS_RSP *p_rsp);
static void handle_get_elem_attr_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_ELEM_ATTRS_RSP *p_rsp);
static void handle_set_app_attr_val_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_RSP *p_rsp);
static bt_status_t get_play_status_cmd(void);
static bt_status_t get_player_app_setting_attr_text_cmd (uint8_t *attrs, uint8_t num_attrs);
static bt_status_t get_player_app_setting_value_text_cmd (uint8_t *vals, uint8_t num_vals);
static bt_status_t register_notification_cmd (uint8_t label, uint8_t event_id, uint32_t event_value);
static bt_status_t get_element_attribute_cmd (uint8_t num_attribute, uint32_t *p_attr_ids);
static bt_status_t getcapabilities_cmd (uint8_t cap_id);
static bt_status_t list_player_app_setting_attrib_cmd(void);
static bt_status_t list_player_app_setting_value_cmd(uint8_t attrib_id);
static bt_status_t get_player_app_setting_cmd(uint8_t num_attrib, uint8_t* attrib_ids);
#endif
static void btif_rc_upstreams_evt(uint16_t event, tAVRC_COMMAND* p_param, uint8_t ctype, uint8_t label);
#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
static void btif_rc_upstreams_rsp_evt(uint16_t event, tAVRC_RESPONSE *pavrc_resp, uint8_t ctype, uint8_t label);
#endif
static void rc_start_play_status_timer(void);
static bool absolute_volume_disabled(void);
static char const* key_id_to_str(uint16_t id);

/*****************************************************************************
**  Static variables
******************************************************************************/
static btif_rc_cb_t btif_rc_cb;
static btrc_callbacks_t *bt_rc_callbacks = NULL;
static btrc_ctrl_callbacks_t *bt_rc_ctrl_callbacks = NULL;

/*****************************************************************************
**  Static functions
******************************************************************************/

/*****************************************************************************
**  Externs
******************************************************************************/
extern bool btif_hf_call_terminated_recently();
extern bool check_cod(const bt_bdaddr_t *remote_bdaddr, uint32_t cod);

extern fixed_queue_t *btu_general_alarm_queue;

/*****************************************************************************
**  Functions
******************************************************************************/

/*****************************************************************************
**   Local uinput helper functions
******************************************************************************/
int send_event (int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct uinput_event event;
    BTIF_TRACE_DEBUG("%s type:%u code:%u value:%d", __func__,
        type, code, value);
    memset(&event, 0, sizeof(event));
    event.type  = type;
    event.code  = code;
    event.value = value;

    ssize_t ret;
    OSI_NO_INTR(ret = write(fd, &event, sizeof(event)));
    return (int)ret;
}

void send_key (int fd, uint16_t key, int pressed)
{
    BTIF_TRACE_DEBUG("%s fd:%d key:%u pressed:%d", __func__,
        fd, key, pressed);

    if (fd < 0)
    {
        return;
    }

    LOG_INFO(LOG_TAG, "AVRCP: Send key %s (%d) fd=%d", key_id_to_str(key), pressed, fd);
    send_event(fd, EV_KEY, key, pressed);
    send_event(fd, EV_SYN, SYN_REPORT, 0);
}

/************** uinput related functions **************/
int uinput_driver_check()
{
    uint32_t i;
    for (i=0; i < MAX_UINPUT_PATHS; i++)
    {
        if (access(uinput_dev_path[i], O_RDWR) == 0) {
           return 0;
        }
    }
    BTIF_TRACE_ERROR("%s ERROR: uinput device is not in the system", __func__);
    return -1;
}

int uinput_create(const char *name)
{
    struct uinput_dev dev;
    int fd, x = 0;

    for(x=0; x < MAX_UINPUT_PATHS; x++)
    {
        fd = open(uinput_dev_path[x], O_RDWR);
        if (fd < 0)
            continue;
        break;
    }
    if (x == MAX_UINPUT_PATHS) {
        BTIF_TRACE_ERROR("%s ERROR: uinput device open failed", __func__);
        return -1;
    }
    memset(&dev, 0, sizeof(dev));
    if (name)
        strncpy(dev.name, name, UINPUT_MAX_NAME_SIZE-1);

    dev.id.bustype = BUS_BLUETOOTH;
    dev.id.vendor  = 0x0000;
    dev.id.product = 0x0000;
    dev.id.version = 0x0000;

    ssize_t ret;
    OSI_NO_INTR(ret = write(fd, &dev, sizeof(dev)));
    if (ret < 0) {
        BTIF_TRACE_ERROR("%s Unable to write device information", __func__);
        close(fd);
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    for (x = 0; key_map[x].name != NULL; x++)
        ioctl(fd, UI_SET_KEYBIT, key_map[x].mapped_id);

    if (ioctl(fd, UI_DEV_CREATE, NULL) < 0) {
        BTIF_TRACE_ERROR("%s Unable to create uinput device", __func__);
        close(fd);
        return -1;
    }
    return fd;
}

int init_uinput (void)
{
    const char *name = "AVRCP";

    BTIF_TRACE_DEBUG("%s", __func__);
    uinput_fd = uinput_create(name);
    if (uinput_fd < 0) {
        BTIF_TRACE_ERROR("%s AVRCP: Failed to initialize uinput for %s (%d)",
                          __func__, name, uinput_fd);
    } else {
        BTIF_TRACE_DEBUG("%s AVRCP: Initialized uinput for %s (fd=%d)",
                          __func__, name, uinput_fd);
    }
    return uinput_fd;
}

void close_uinput (void)
{
    BTIF_TRACE_DEBUG("%s", __func__);
    if (uinput_fd > 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);

        close(uinput_fd);
        uinput_fd = -1;
    }
}

#if (AVRC_CTRL_INCLUDED == TRUE)
void rc_cleanup_sent_cmd (void *p_data)
{
    BTIF_TRACE_DEBUG("%s", __func__);

}

void handle_rc_ctrl_features(BD_ADDR bd_addr)
{
    if ((btif_rc_cb.rc_features & BTA_AV_FEAT_RCTG)||
       ((btif_rc_cb.rc_features & BTA_AV_FEAT_RCCT)&&
        (btif_rc_cb.rc_features & BTA_AV_FEAT_ADV_CTRL)))
    {
        bt_bdaddr_t rc_addr;
        int rc_features = 0;
        bdcpy(rc_addr.address,bd_addr);

        if ((btif_rc_cb.rc_features & BTA_AV_FEAT_ADV_CTRL)&&
             (btif_rc_cb.rc_features & BTA_AV_FEAT_RCCT))
        {
            rc_features |= BTRC_FEAT_ABSOLUTE_VOLUME;
        }
        if ((btif_rc_cb.rc_features & BTA_AV_FEAT_METADATA)&&
            (btif_rc_cb.rc_features & BTA_AV_FEAT_VENDOR)&&
            (btif_rc_cb.rc_features_processed != true))
        {
            rc_features |= BTRC_FEAT_METADATA;
            /* Mark rc features processed to avoid repeating
             * the AVRCP procedure every time on receiving this
             * update.
             */
            btif_rc_cb.rc_features_processed = true;

            if (btif_av_is_sink_enabled())
                getcapabilities_cmd (AVRC_CAP_COMPANY_ID);
        }
        BTIF_TRACE_DEBUG("%s Update rc features to CTRL %d", __func__, rc_features);
        HAL_CBACK(bt_rc_ctrl_callbacks, getrcfeatures_cb, &rc_addr, rc_features);
    }
}
#endif

void handle_rc_features(BD_ADDR bd_addr)
{
    if (bt_rc_callbacks != NULL)
    {
    btrc_remote_features_t rc_features = BTRC_FEAT_NONE;
    bt_bdaddr_t rc_addr;

    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);
    bt_bdaddr_t avdtp_addr  = btif_av_get_addr();

    bdstr_t addr1, addr2;
    BTIF_TRACE_DEBUG("%s: AVDTP Address: %s AVCTP address: %s", __func__,
                     bdaddr_to_string(&avdtp_addr, addr1, sizeof(addr1)),
                     bdaddr_to_string(&rc_addr, addr2, sizeof(addr2)));

    if (interop_match_addr(INTEROP_DISABLE_ABSOLUTE_VOLUME, &rc_addr)
        || absolute_volume_disabled()
        || bdcmp(avdtp_addr.address, rc_addr.address))
        btif_rc_cb.rc_features &= ~BTA_AV_FEAT_ADV_CTRL;

    if (btif_rc_cb.rc_features & BTA_AV_FEAT_BROWSE)
    {
        rc_features = (btrc_remote_features_t)(rc_features | BTRC_FEAT_BROWSE);
    }

#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
    if ( (btif_rc_cb.rc_features & BTA_AV_FEAT_ADV_CTRL) &&
         (btif_rc_cb.rc_features & BTA_AV_FEAT_RCTG))
    {
        rc_features = (btrc_remote_features_t)(rc_features | BTRC_FEAT_ABSOLUTE_VOLUME);
    }
#endif

    if (btif_rc_cb.rc_features & BTA_AV_FEAT_METADATA)
    {
        rc_features = (btrc_remote_features_t)(rc_features | BTRC_FEAT_METADATA);
    }

    BTIF_TRACE_DEBUG("%s: rc_features=0x%x", __func__, rc_features);
    HAL_CBACK(bt_rc_callbacks, remote_features_cb, &rc_addr, rc_features)

#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
     BTIF_TRACE_DEBUG("%s Checking for feature flags in btif_rc_handler with label %d",
                        __func__, btif_rc_cb.rc_vol_label);
     // Register for volume change on connect
      if (btif_rc_cb.rc_features & BTA_AV_FEAT_ADV_CTRL &&
         btif_rc_cb.rc_features & BTA_AV_FEAT_RCTG)
      {
         rc_transaction_t *p_transaction=NULL;
         bt_status_t status = BT_STATUS_NOT_READY;
         if (MAX_LABEL==btif_rc_cb.rc_vol_label)
         {
            status=get_transaction(&p_transaction);
         }
         else
         {
            p_transaction=get_transaction_by_lbl(btif_rc_cb.rc_vol_label);
            if (NULL!=p_transaction)
            {
               BTIF_TRACE_DEBUG("%s register_volumechange already in progress for label %d",
                                  __func__, btif_rc_cb.rc_vol_label);
               return;
            }
            else
              status=get_transaction(&p_transaction);
         }

         if (BT_STATUS_SUCCESS == status && NULL!=p_transaction)
         {
            btif_rc_cb.rc_vol_label=p_transaction->lbl;
            register_volumechange(btif_rc_cb.rc_vol_label);
         }
       }
#endif
    }
}

/***************************************************************************
 *  Function       handle_rc_connect
 *
 *  - Argument:    tBTA_AV_RC_OPEN  RC open data structure
 *
 *  - Description: RC connection event handler
 *
 ***************************************************************************/
void handle_rc_connect (tBTA_AV_RC_OPEN *p_rc_open)
{
    BTIF_TRACE_DEBUG("%s: rc_handle: %d", __func__, p_rc_open->rc_handle);
    bt_status_t result = BT_STATUS_SUCCESS;
#if (AVRC_CTRL_INCLUDED == TRUE)
    bt_bdaddr_t rc_addr;
#endif

    if (p_rc_open->status == BTA_AV_SUCCESS)
    {
        //check if already some RC is connected
        if (btif_rc_cb.rc_connected)
        {
            BTIF_TRACE_ERROR("%s Got RC OPEN in connected state, Connected RC: %d \
                and Current RC: %d", __func__, btif_rc_cb.rc_handle,p_rc_open->rc_handle );
            if ((btif_rc_cb.rc_handle != p_rc_open->rc_handle)
                && (bdcmp(btif_rc_cb.rc_addr, p_rc_open->peer_addr)))
            {
                BTIF_TRACE_DEBUG("%s Got RC connected for some other handle", __func__);
                BTA_AvCloseRc(p_rc_open->rc_handle);
                return;
            }
        }
        memcpy(btif_rc_cb.rc_addr, p_rc_open->peer_addr, sizeof(BD_ADDR));
        btif_rc_cb.rc_features = p_rc_open->peer_features;
        btif_rc_cb.rc_vol_label=MAX_LABEL;
        btif_rc_cb.rc_volume=MAX_VOLUME;

        btif_rc_cb.rc_connected = true;
        btif_rc_cb.rc_handle = p_rc_open->rc_handle;

        /* on locally initiated connection we will get remote features as part of connect */
        if (btif_rc_cb.rc_features != 0)
            handle_rc_features(btif_rc_cb.rc_addr);
        if (bt_rc_callbacks)
        {
            result = (bt_status_t)uinput_driver_check();
            if (result == BT_STATUS_SUCCESS)
            {
                init_uinput();
            }
        }
        else
        {
            BTIF_TRACE_WARNING("%s Avrcp TG role not enabled, not initializing UInput",
                               __func__);
        }
        BTIF_TRACE_DEBUG("%s handle_rc_connect features %d ",__func__, btif_rc_cb.rc_features);
#if (AVRC_CTRL_INCLUDED == TRUE)
        btif_rc_cb.rc_playing_uid = RC_INVALID_TRACK_ID;
        bdcpy(rc_addr.address, btif_rc_cb.rc_addr);
        if (bt_rc_ctrl_callbacks != NULL)
        {
            HAL_CBACK(bt_rc_ctrl_callbacks, connection_state_cb, true, &rc_addr);
        }
        /* report connection state if remote device is AVRCP target */
        if ((btif_rc_cb.rc_features & BTA_AV_FEAT_RCTG)||
           ((btif_rc_cb.rc_features & BTA_AV_FEAT_RCCT)&&
            (btif_rc_cb.rc_features & BTA_AV_FEAT_ADV_CTRL)))
        {
            handle_rc_ctrl_features(btif_rc_cb.rc_addr);
        }
#endif
    }
    else
    {
        BTIF_TRACE_ERROR("%s Connect failed with error code: %d",
            __func__, p_rc_open->status);
        btif_rc_cb.rc_connected = false;
    }
}

/***************************************************************************
 *  Function       handle_rc_disconnect
 *
 *  - Argument:    tBTA_AV_RC_CLOSE     RC close data structure
 *
 *  - Description: RC disconnection event handler
 *
 ***************************************************************************/
void handle_rc_disconnect (tBTA_AV_RC_CLOSE *p_rc_close)
{
#if (AVRC_CTRL_INCLUDED == TRUE)
    bt_bdaddr_t rc_addr;
    tBTA_AV_FEAT features;
#endif
    BTIF_TRACE_DEBUG("%s: rc_handle: %d", __func__, p_rc_close->rc_handle);
    if ((p_rc_close->rc_handle != btif_rc_cb.rc_handle)
        && (bdcmp(btif_rc_cb.rc_addr, p_rc_close->peer_addr)))
    {
        BTIF_TRACE_ERROR("Got disconnect of unknown device");
        return;
    }
#if (AVRC_CTRL_INCLUDED == TRUE)
    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);
    features = btif_rc_cb.rc_features;
        /* Clean up AVRCP procedure flags */
    memset(&btif_rc_cb.rc_app_settings, 0,
        sizeof(btif_rc_player_app_settings_t));
    btif_rc_cb.rc_features_processed = false;
    btif_rc_cb.rc_procedure_complete = false;
    rc_stop_play_status_timer();
    /* Check and clear the notification event list */
    if (btif_rc_cb.rc_supported_event_list != NULL)
    {
        list_clear(btif_rc_cb.rc_supported_event_list);
        btif_rc_cb.rc_supported_event_list = NULL;
    }
#endif
    btif_rc_cb.rc_handle = 0;
    btif_rc_cb.rc_connected = false;
    memset(btif_rc_cb.rc_addr, 0, sizeof(BD_ADDR));
    memset(btif_rc_cb.rc_notif, 0, sizeof(btif_rc_cb.rc_notif));

    btif_rc_cb.rc_features = 0;
    btif_rc_cb.rc_vol_label=MAX_LABEL;
    btif_rc_cb.rc_volume=MAX_VOLUME;
    init_all_transactions();
    if (bt_rc_callbacks != NULL)
    {
        close_uinput();
    }
    else
    {
        BTIF_TRACE_WARNING("%s Avrcp TG role not enabled, not closing UInput", __func__);
    }

    memset(btif_rc_cb.rc_addr, 0, sizeof(BD_ADDR));
#if (AVRC_CTRL_INCLUDED == TRUE)
    /* report connection state if device is AVRCP target */
    if (bt_rc_ctrl_callbacks != NULL)
   {
        HAL_CBACK(bt_rc_ctrl_callbacks, connection_state_cb, false, &rc_addr);
   }
#endif
}

/***************************************************************************
 *  Function       handle_rc_passthrough_cmd
 *
 *  - Argument:    tBTA_AV_RC rc_id   remote control command ID
 *                 tBTA_AV_STATE key_state status of key press
 *
 *  - Description: Remote control command handler
 *
 ***************************************************************************/
void handle_rc_passthrough_cmd ( tBTA_AV_REMOTE_CMD *p_remote_cmd)
{
    const char *status;
    int pressed, i;

    BTIF_TRACE_DEBUG("%s: p_remote_cmd->rc_id=%d", __func__, p_remote_cmd->rc_id);

    /* If AVRC is open and peer sends PLAY but there is no AVDT, then we queue-up this PLAY */
    if (p_remote_cmd)
    {
        /* queue AVRC PLAY if GAVDTP Open notification to app is pending (2 second timer) */
        if ((p_remote_cmd->rc_id == BTA_AV_RC_PLAY) && (!btif_av_is_connected()))
        {
            if (p_remote_cmd->key_state == AVRC_STATE_PRESS)
            {
                APPL_TRACE_WARNING("%s: AVDT not open, queuing the PLAY command", __func__);
                btif_rc_cb.rc_pending_play = true;
            }
            return;
        }

        if ((p_remote_cmd->rc_id == BTA_AV_RC_PAUSE) && (btif_rc_cb.rc_pending_play))
        {
            APPL_TRACE_WARNING("%s: Clear the pending PLAY on PAUSE received", __func__);
            btif_rc_cb.rc_pending_play = false;
            return;
        }
        if ((p_remote_cmd->rc_id == BTA_AV_RC_VOL_UP)||(p_remote_cmd->rc_id == BTA_AV_RC_VOL_DOWN))
            return; // this command is not to be sent to UINPUT, only needed for PTS
    }

    if ((p_remote_cmd->rc_id == BTA_AV_RC_STOP) && (!btif_av_stream_started_ready()))
    {
        APPL_TRACE_WARNING("%s: Stream suspended, ignore STOP cmd",__func__);
        return;
    }

    if (p_remote_cmd->key_state == AVRC_STATE_RELEASE) {
        status = "released";
        pressed = 0;
    } else {
        status = "pressed";
        pressed = 1;
    }

    if (p_remote_cmd->rc_id == BTA_AV_RC_FAST_FOR || p_remote_cmd->rc_id == BTA_AV_RC_REWIND) {
        HAL_CBACK(bt_rc_callbacks, passthrough_cmd_cb, p_remote_cmd->rc_id, pressed);
        return;
    }

    for (i = 0; key_map[i].name != NULL; i++) {
        if (p_remote_cmd->rc_id == key_map[i].avrcp) {
            BTIF_TRACE_DEBUG("%s: %s %s", __func__, key_map[i].name, status);

           /* MusicPlayer uses a long_press_timeout of 1 second for PLAYPAUSE button
            * and maps that to autoshuffle. So if for some reason release for PLAY/PAUSE
            * comes 1 second after the press, the MediaPlayer UI goes into a bad state.
            * The reason for the delay could be sniff mode exit or some AVDTP procedure etc.
            * The fix is to generate a release right after the press and drown the 'actual'
            * release.
            */
            if ((key_map[i].release_quirk == 1) && (pressed == 0))
            {
                BTIF_TRACE_DEBUG("%s: AVRC %s Release Faked earlier, drowned now",
                                  __func__, key_map[i].name);
                return;
            }
            send_key(uinput_fd, key_map[i].mapped_id, pressed);
            if ((key_map[i].release_quirk == 1) && (pressed == 1))
            {
                sleep_ms(30);
                BTIF_TRACE_DEBUG("%s: AVRC %s Release quirk enabled, send release now",
                                  __func__, key_map[i].name);
                send_key(uinput_fd, key_map[i].mapped_id, 0);
            }
            break;
        }
    }

    if (key_map[i].name == NULL)
        BTIF_TRACE_ERROR("%s AVRCP: unknown button 0x%02X %s", __func__,
                        p_remote_cmd->rc_id, status);
}

/***************************************************************************
 *  Function       handle_rc_passthrough_rsp
 *
 *  - Argument:    tBTA_AV_REMOTE_RSP passthrough command response
 *
 *  - Description: Remote control passthrough response handler
 *
 ***************************************************************************/
void handle_rc_passthrough_rsp ( tBTA_AV_REMOTE_RSP *p_remote_rsp)
{
#if (AVRC_CTRL_INCLUDED == TRUE)
    const char *status;
    if (btif_rc_cb.rc_features & BTA_AV_FEAT_RCTG)
    {
        int key_state;
        if (p_remote_rsp->key_state == AVRC_STATE_RELEASE)
        {
            status = "released";
            key_state = 1;
        }
        else
        {
            status = "pressed";
            key_state = 0;
        }

        BTIF_TRACE_DEBUG("%s: rc_id=%d status=%s", __func__, p_remote_rsp->rc_id, status);

        release_transaction(p_remote_rsp->label);
        if (bt_rc_ctrl_callbacks != NULL) {
            HAL_CBACK(bt_rc_ctrl_callbacks, passthrough_rsp_cb, p_remote_rsp->rc_id, key_state);
        }
    }
    else
    {
        BTIF_TRACE_ERROR("%s DUT does not support AVRCP controller role", __func__);
    }
#else
    BTIF_TRACE_ERROR("%s AVRCP controller role is not enabled", __func__);
#endif
}

/***************************************************************************
 *  Function       handle_rc_vendorunique_rsp
 *
 *  - Argument:    tBTA_AV_REMOTE_RSP  command response
 *
 *  - Description: Remote control vendor unique response handler
 *
 ***************************************************************************/
void handle_rc_vendorunique_rsp ( tBTA_AV_REMOTE_RSP *p_remote_rsp)
{
#if (AVRC_CTRL_INCLUDED == TRUE)
    const char *status;
    uint8_t vendor_id = 0;
    if (btif_rc_cb.rc_features & BTA_AV_FEAT_RCTG)
    {
        int key_state;
        if (p_remote_rsp->key_state == AVRC_STATE_RELEASE)
        {
            status = "released";
            key_state = 1;
        }
        else
        {
            status = "pressed";
            key_state = 0;
        }

        if (p_remote_rsp->len > 0)
        {
            if (p_remote_rsp->len >= AVRC_PASS_THRU_GROUP_LEN)
                vendor_id = p_remote_rsp->p_data[AVRC_PASS_THRU_GROUP_LEN -1];
            osi_free_and_reset((void **)&p_remote_rsp->p_data);
        }
        BTIF_TRACE_DEBUG("%s: vendor_id=%d status=%s", __func__, vendor_id, status);

        release_transaction(p_remote_rsp->label);
        HAL_CBACK(bt_rc_ctrl_callbacks, groupnavigation_rsp_cb, vendor_id, key_state);
    }
    else
    {
        BTIF_TRACE_ERROR("%s Remote does not support AVRCP TG role", __func__);
    }
#else
    BTIF_TRACE_ERROR("%s AVRCP controller role is not enabled", __func__);
#endif
}

void handle_uid_changed_notification(tBTA_AV_META_MSG *pmeta_msg, tAVRC_COMMAND *pavrc_command)
{
    tAVRC_RESPONSE avrc_rsp = {0};
    avrc_rsp.rsp.pdu = pavrc_command->pdu;
    avrc_rsp.rsp.status = AVRC_STS_NO_ERROR;
    avrc_rsp.rsp.opcode = pavrc_command->cmd.opcode;

    avrc_rsp.reg_notif.event_id = pavrc_command->reg_notif.event_id;
    avrc_rsp.reg_notif.param.uid_counter = 0;

    send_metamsg_rsp(pmeta_msg->rc_handle, pmeta_msg->label, AVRC_RSP_INTERIM, &avrc_rsp);
    send_metamsg_rsp(pmeta_msg->rc_handle, pmeta_msg->label, AVRC_RSP_CHANGED, &avrc_rsp);

}

/***************************************************************************
 *  Function       handle_rc_metamsg_cmd
 *
 *  - Argument:    tBTA_AV_VENDOR Structure containing the received
 *                          metamsg command
 *
 *  - Description: Remote control metamsg command handler (AVRCP 1.3)
 *
 ***************************************************************************/
void handle_rc_metamsg_cmd (tBTA_AV_META_MSG *pmeta_msg)
{
    /* Parse the metamsg command and pass it on to BTL-IFS */
    uint8_t             scratch_buf[512] = {0};
    tAVRC_COMMAND    avrc_command = {0};
    tAVRC_STS status;

    BTIF_TRACE_EVENT("+ %s", __func__);

    if (pmeta_msg->p_msg->hdr.opcode != AVRC_OP_VENDOR)
    {
        BTIF_TRACE_WARNING("Invalid opcode: %x", pmeta_msg->p_msg->hdr.opcode);
        return;
    }
    if (pmeta_msg->len < 3)
    {
        BTIF_TRACE_WARNING("Invalid length.Opcode: 0x%x, len: 0x%x", pmeta_msg->p_msg->hdr.opcode,
            pmeta_msg->len);
        return;
    }

    if (pmeta_msg->code >= AVRC_RSP_NOT_IMPL)
    {
#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
{
     rc_transaction_t *transaction=NULL;
     transaction=get_transaction_by_lbl(pmeta_msg->label);
     if (NULL!=transaction)
     {
        handle_rc_metamsg_rsp(pmeta_msg);
     }
     else
     {
         BTIF_TRACE_DEBUG("%s:Discard vendor dependent rsp. code: %d label:%d.",
             __func__, pmeta_msg->code, pmeta_msg->label);
     }
     return;
}
#else
{
        BTIF_TRACE_DEBUG("%s:Received vendor dependent rsp. code: %d len: %d. Not processing it.",
            __func__, pmeta_msg->code, pmeta_msg->len);
        return;
}
#endif
      }

    status=AVRC_ParsCommand(pmeta_msg->p_msg, &avrc_command, scratch_buf, sizeof(scratch_buf));
    BTIF_TRACE_DEBUG("%s Received vendor command.code,PDU and label: %d, %d,%d",
                     __func__, pmeta_msg->code, avrc_command.cmd.pdu, pmeta_msg->label);

    if (status != AVRC_STS_NO_ERROR)
    {
        /* return error */
        BTIF_TRACE_WARNING("%s: Error in parsing received metamsg command. status: 0x%02x",
            __func__, status);
        send_reject_response(pmeta_msg->rc_handle, pmeta_msg->label, avrc_command.pdu, status);
    }
    else
    {
        /* if RegisterNotification, add it to our registered queue */

        if (avrc_command.cmd.pdu == AVRC_PDU_REGISTER_NOTIFICATION)
        {
            uint8_t event_id = avrc_command.reg_notif.event_id;
            BTIF_TRACE_EVENT("%s:New register notification received.event_id:%s,label:0x%x,code:%x",
            __func__,dump_rc_notification_event_id(event_id), pmeta_msg->label,pmeta_msg->code);
            btif_rc_cb.rc_notif[event_id-1].bNotify = true;
            btif_rc_cb.rc_notif[event_id-1].label = pmeta_msg->label;

            if (event_id == AVRC_EVT_UIDS_CHANGE)
            {
                handle_uid_changed_notification(pmeta_msg, &avrc_command);
                return;
            }

        }

        BTIF_TRACE_EVENT("%s: Passing received metamsg command to app. pdu: %s",
            __func__, dump_rc_pdu(avrc_command.cmd.pdu));

        /* Since handle_rc_metamsg_cmd() itself is called from
            *btif context, no context switching is required. Invoke
            * btif_rc_upstreams_evt directly from here. */
        btif_rc_upstreams_evt((uint16_t)avrc_command.cmd.pdu, &avrc_command, pmeta_msg->code,
                               pmeta_msg->label);
    }
}

/***************************************************************************
 **
 ** Function       btif_rc_handler
 **
 ** Description    RC event handler
 **
 ***************************************************************************/
void btif_rc_handler(tBTA_AV_EVT event, tBTA_AV *p_data)
{
    BTIF_TRACE_DEBUG ("%s event:%s", __func__, dump_rc_event(event));
    switch (event)
    {
        case BTA_AV_RC_OPEN_EVT:
        {
            BTIF_TRACE_DEBUG("%s Peer_features:%x", __func__, p_data->rc_open.peer_features);
            handle_rc_connect( &(p_data->rc_open) );
        }break;

        case BTA_AV_RC_CLOSE_EVT:
        {
            handle_rc_disconnect( &(p_data->rc_close) );
        }break;

        case BTA_AV_REMOTE_CMD_EVT:
        {
            if (bt_rc_callbacks != NULL)
            {
              BTIF_TRACE_DEBUG("%s rc_id:0x%x key_state:%d",
                               __func__, p_data->remote_cmd.rc_id,
                               p_data->remote_cmd.key_state);
                /** In race conditions just after 2nd AVRCP is connected
                 *  remote might send pass through commands, so check for
                 *  Rc handle before processing pass through commands
                 **/
                if (btif_rc_cb.rc_handle == p_data->remote_cmd.rc_handle)
                {
                    handle_rc_passthrough_cmd( (&p_data->remote_cmd) );
                }
                else
                {
                    BTIF_TRACE_DEBUG("%s Pass-through command for Invalid rc handle", __func__);
                }
            }
            else
            {
                BTIF_TRACE_ERROR("AVRCP TG role not up, drop passthrough commands");
            }
        }
        break;

#if (AVRC_CTRL_INCLUDED == TRUE)
        case BTA_AV_REMOTE_RSP_EVT:
        {
            BTIF_TRACE_DEBUG("%s RSP: rc_id:0x%x key_state:%d",
                             __func__, p_data->remote_rsp.rc_id, p_data->remote_rsp.key_state);
            if (p_data->remote_rsp.rc_id == AVRC_ID_VENDOR)
            {
                handle_rc_vendorunique_rsp(&p_data->remote_rsp);
            }
            else
            {
                handle_rc_passthrough_rsp(&p_data->remote_rsp);
            }
        }
        break;

#endif
        case BTA_AV_RC_FEAT_EVT:
        {
            BTIF_TRACE_DEBUG("%s Peer_features:%x", __func__, p_data->rc_feat.peer_features);
            btif_rc_cb.rc_features = p_data->rc_feat.peer_features;
            handle_rc_features(p_data->rc_feat.peer_addr);
#if (AVRC_CTRL_INCLUDED == TRUE)
            if ((btif_rc_cb.rc_connected) && (bt_rc_ctrl_callbacks != NULL))
            {
                handle_rc_ctrl_features(btif_rc_cb.rc_addr);
            }
#endif
        }
        break;

        case BTA_AV_META_MSG_EVT:
        {
            if (bt_rc_callbacks != NULL)
            {
                BTIF_TRACE_DEBUG("%s BTA_AV_META_MSG_EVT  code:%d label:%d",
                                 __func__,
                                 p_data->meta_msg.code,
                                 p_data->meta_msg.label);
                BTIF_TRACE_DEBUG("%s company_id:0x%x len:%d handle:%d",
                                 __func__,
                                 p_data->meta_msg.company_id,
                                 p_data->meta_msg.len,
                                 p_data->meta_msg.rc_handle);
                /* handle the metamsg command */
                handle_rc_metamsg_cmd(&(p_data->meta_msg));
                /* Free the Memory allocated for tAVRC_MSG */
            }
#if (AVRC_CTRL_INCLUDED == TRUE)
            else if ((bt_rc_callbacks == NULL)&&(bt_rc_ctrl_callbacks != NULL))
            {
                /* This is case of Sink + CT + TG(for abs vol)) */
                BTIF_TRACE_DEBUG("%s BTA_AV_META_MSG_EVT  code:%d label:%d",
                                 __func__,
                                 p_data->meta_msg.code,
                                 p_data->meta_msg.label);
                BTIF_TRACE_DEBUG("%s company_id:0x%x len:%d handle:%d",
                                 __func__,
                                 p_data->meta_msg.company_id,
                                 p_data->meta_msg.len,
                                 p_data->meta_msg.rc_handle);
                if ((p_data->meta_msg.code >= AVRC_RSP_NOT_IMPL)&&
                    (p_data->meta_msg.code <= AVRC_RSP_INTERIM))
                {
                    /* Its a response */
                    handle_avk_rc_metamsg_rsp(&(p_data->meta_msg));
                }
                else if (p_data->meta_msg.code <= AVRC_CMD_GEN_INQ)
                {
                    /* Its a command  */
                    handle_avk_rc_metamsg_cmd(&(p_data->meta_msg));
                }

            }
#endif
            else
            {
                BTIF_TRACE_ERROR("Neither CTRL, nor TG is up, drop meta commands");
            }
        }
        break;

        default:
            BTIF_TRACE_DEBUG("%s Unhandled RC event : 0x%x", __func__, event);
    }
}

/***************************************************************************
 **
 ** Function       btif_rc_get_connected_peer
 **
 ** Description    Fetches the connected headset's BD_ADDR if any
 **
 ***************************************************************************/
bool btif_rc_get_connected_peer(BD_ADDR peer_addr)
{
    if (btif_rc_cb.rc_connected == true) {
        bdcpy(peer_addr, btif_rc_cb.rc_addr);
        return true;
    }
    return false;
}

/***************************************************************************
 **
 ** Function       btif_rc_get_connected_peer_handle
 **
 ** Description    Fetches the connected headset's handle if any
 **
 ***************************************************************************/
uint8_t btif_rc_get_connected_peer_handle(void)
{
    return btif_rc_cb.rc_handle;
}

/***************************************************************************
 **
 ** Function       btif_rc_check_handle_pending_play
 **
 ** Description    Clears the queued PLAY command. if bSend is true, forwards to app
 **
 ***************************************************************************/

/* clear the queued PLAY command. if bSend is true, forward to app */
void btif_rc_check_handle_pending_play (BD_ADDR peer_addr, bool bSendToApp)
{
    UNUSED(peer_addr);

    BTIF_TRACE_DEBUG("%s: bSendToApp=%d", __func__, bSendToApp);
    if (btif_rc_cb.rc_pending_play)
    {
        if (bSendToApp)
        {
            tBTA_AV_REMOTE_CMD remote_cmd;
            APPL_TRACE_DEBUG("%s: Sending queued PLAYED event to app", __func__);

            memset (&remote_cmd, 0, sizeof(tBTA_AV_REMOTE_CMD));
            remote_cmd.rc_handle  = btif_rc_cb.rc_handle;
            remote_cmd.rc_id      = AVRC_ID_PLAY;
            remote_cmd.hdr.ctype  = AVRC_CMD_CTRL;
            remote_cmd.hdr.opcode = AVRC_OP_PASS_THRU;

            /* delay sending to app, else there is a timing issue in the framework,
             ** which causes the audio to be on th device's speaker. Delay between
             ** OPEN & RC_PLAYs
            */
            sleep_ms(200);
            /* send to app - both PRESSED & RELEASED */
            remote_cmd.key_state  = AVRC_STATE_PRESS;
            handle_rc_passthrough_cmd( &remote_cmd );

            sleep_ms(100);

            remote_cmd.key_state  = AVRC_STATE_RELEASE;
            handle_rc_passthrough_cmd( &remote_cmd );
        }
        btif_rc_cb.rc_pending_play = false;
    }
}

/* Generic reject response */
static void send_reject_response (uint8_t rc_handle, uint8_t label, uint8_t pdu, uint8_t status)
{
    uint8_t ctype = AVRC_RSP_REJ;
    tAVRC_RESPONSE avrc_rsp;
    BT_HDR *p_msg = NULL;
    memset (&avrc_rsp, 0, sizeof(tAVRC_RESPONSE));

    avrc_rsp.rsp.opcode = opcode_from_pdu(pdu);
    avrc_rsp.rsp.pdu    = pdu;
    avrc_rsp.rsp.status = status;

    if (AVRC_STS_NO_ERROR == (status = AVRC_BldResponse(rc_handle, &avrc_rsp, &p_msg)) )
    {
        BTIF_TRACE_DEBUG("%s:Sending error notification to handle:%d. pdu:%s,status:0x%02x",
            __func__, rc_handle, dump_rc_pdu(pdu), status);
        BTA_AvMetaRsp(rc_handle, label, ctype, p_msg);
    }
}

/***************************************************************************
 *  Function       send_metamsg_rsp
 *
 *  - Argument:
 *                  rc_handle     RC handle corresponding to the connected RC
 *                  label            Label of the RC response
 *                  code            Response type
 *                  pmetamsg_resp    Vendor response
 *
 *  - Description: Remote control metamsg response handler (AVRCP 1.3)
 *
 ***************************************************************************/
static void send_metamsg_rsp (uint8_t rc_handle, uint8_t label, tBTA_AV_CODE code,
    tAVRC_RESPONSE *pmetamsg_resp)
{
    uint8_t ctype;

    if (!pmetamsg_resp)
    {
        BTIF_TRACE_WARNING("%s: Invalid response received from application", __func__);
        return;
    }

    BTIF_TRACE_EVENT("+%s: rc_handle: %d, label: %d, code: 0x%02x, pdu: %s", __func__,
        rc_handle, label, code, dump_rc_pdu(pmetamsg_resp->rsp.pdu));

    if (pmetamsg_resp->rsp.status != AVRC_STS_NO_ERROR)
    {
        ctype = AVRC_RSP_REJ;
    }
    else
    {
        if ( code < AVRC_RSP_NOT_IMPL)
        {
            if (code == AVRC_CMD_NOTIF)
            {
               ctype = AVRC_RSP_INTERIM;
            }
            else if (code == AVRC_CMD_STATUS)
            {
               ctype = AVRC_RSP_IMPL_STBL;
            }
            else
            {
               ctype = AVRC_RSP_ACCEPT;
            }
        }
        else
        {
            ctype = code;
        }
    }
    /* if response is for register_notification, make sure the rc has
    actually registered for this */
    if ((pmetamsg_resp->rsp.pdu == AVRC_PDU_REGISTER_NOTIFICATION) && (code == AVRC_RSP_CHANGED))
    {
        bool bSent = false;
        uint8_t   event_id = pmetamsg_resp->reg_notif.event_id;
        bool bNotify = (btif_rc_cb.rc_connected) && (btif_rc_cb.rc_notif[event_id-1].bNotify);

        /* de-register this notification for a CHANGED response */
        btif_rc_cb.rc_notif[event_id-1].bNotify = false;
        BTIF_TRACE_DEBUG("%s rc_handle: %d. event_id: 0x%02d bNotify:%u", __func__,
             btif_rc_cb.rc_handle, event_id, bNotify);
        if (bNotify)
        {
            BT_HDR *p_msg = NULL;
            tAVRC_STS status;

            if (AVRC_STS_NO_ERROR == (status = AVRC_BldResponse(btif_rc_cb.rc_handle,
                pmetamsg_resp, &p_msg)) )
            {
                BTIF_TRACE_DEBUG("%s Sending notification to rc_handle: %d. event_id: 0x%02d",
                     __func__, btif_rc_cb.rc_handle, event_id);
                bSent = true;
                BTA_AvMetaRsp(btif_rc_cb.rc_handle, btif_rc_cb.rc_notif[event_id-1].label,
                    ctype, p_msg);
            }
            else
            {
                BTIF_TRACE_WARNING("%s failed to build metamsg response. status: 0x%02x",
                    __func__, status);
            }

        }

        if (!bSent)
        {
            BTIF_TRACE_DEBUG("%s: Notification not sent, as there are no RC connections or the \
                CT has not subscribed for event_id: %s", __func__, dump_rc_notification_event_id(event_id));
        }
    }
    else
    {
        /* All other commands go here */

        BT_HDR *p_msg = NULL;
        tAVRC_STS status;

        status = AVRC_BldResponse(rc_handle, pmetamsg_resp, &p_msg);

        if (status == AVRC_STS_NO_ERROR)
        {
            BTA_AvMetaRsp(rc_handle, label, ctype, p_msg);
        }
        else
        {
            BTIF_TRACE_ERROR("%s: failed to build metamsg response. status: 0x%02x",
                __func__, status);
        }
    }
}

static uint8_t opcode_from_pdu(uint8_t pdu)
{
    uint8_t opcode = 0;

    switch (pdu)
    {
    case AVRC_PDU_NEXT_GROUP:
    case AVRC_PDU_PREV_GROUP: /* pass thru */
        opcode  = AVRC_OP_PASS_THRU;
        break;

    default: /* vendor */
        opcode  = AVRC_OP_VENDOR;
        break;
    }

    return opcode;
}

/*******************************************************************************
**
** Function         btif_rc_upstreams_evt
**
** Description      Executes AVRC UPSTREAMS events in btif context.
**
** Returns          void
**
*******************************************************************************/
static void btif_rc_upstreams_evt(uint16_t event, tAVRC_COMMAND *pavrc_cmd, uint8_t ctype, uint8_t label)
{
    BTIF_TRACE_EVENT("%s pdu: %s handle: 0x%x ctype:%x label:%x", __func__,
        dump_rc_pdu(pavrc_cmd->pdu), btif_rc_cb.rc_handle, ctype, label);

    switch (event)
    {
        case AVRC_PDU_GET_PLAY_STATUS:
        {
            FILL_PDU_QUEUE(IDX_GET_PLAY_STATUS_RSP, ctype, label, true)
            HAL_CBACK(bt_rc_callbacks, get_play_status_cb);
        }
        break;
        case AVRC_PDU_LIST_PLAYER_APP_ATTR:
        case AVRC_PDU_LIST_PLAYER_APP_VALUES:
        case AVRC_PDU_GET_CUR_PLAYER_APP_VALUE:
        case AVRC_PDU_SET_PLAYER_APP_VALUE:
        case AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT:
        case AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT:
        {
            /* TODO: Add support for Application Settings */
            send_reject_response (btif_rc_cb.rc_handle, label, pavrc_cmd->pdu, AVRC_STS_BAD_CMD);
        }
        break;
        case AVRC_PDU_GET_ELEMENT_ATTR:
        {
            btrc_media_attr_t element_attrs[BTRC_MAX_ELEM_ATTR_SIZE];
            uint8_t num_attr;
             memset(&element_attrs, 0, sizeof(element_attrs));
            if (pavrc_cmd->get_elem_attrs.num_attr == 0)
            {
                /* CT requests for all attributes */
                int attr_cnt;
                num_attr = BTRC_MAX_ELEM_ATTR_SIZE;
                for (attr_cnt = 0; attr_cnt < BTRC_MAX_ELEM_ATTR_SIZE; attr_cnt++)
                {
                    element_attrs[attr_cnt] = (btrc_media_attr_t)(attr_cnt + 1);
                }
            }
            else if (pavrc_cmd->get_elem_attrs.num_attr == 0xFF)
            {
                /* 0xff indicates, no attributes requested - reject */
                send_reject_response (btif_rc_cb.rc_handle, label, pavrc_cmd->pdu,
                    AVRC_STS_BAD_PARAM);
                return;
            }
            else
            {
                int attr_cnt, filled_attr_count;

                num_attr = 0;
                /* Attribute IDs from 1 to AVRC_MAX_NUM_MEDIA_ATTR_ID are only valid,
                 * hence HAL definition limits the attributes to AVRC_MAX_NUM_MEDIA_ATTR_ID.
                 * Fill only valid entries.
                 */
                for (attr_cnt = 0; (attr_cnt < pavrc_cmd->get_elem_attrs.num_attr) &&
                    (num_attr < AVRC_MAX_NUM_MEDIA_ATTR_ID); attr_cnt++)
                {
                    if ((pavrc_cmd->get_elem_attrs.attrs[attr_cnt] > 0) &&
                        (pavrc_cmd->get_elem_attrs.attrs[attr_cnt] <= AVRC_MAX_NUM_MEDIA_ATTR_ID))
                    {
                        /* Skip the duplicate entries : PTS sends duplicate entries for Fragment cases
                         */
                        for (filled_attr_count = 0; filled_attr_count < num_attr; filled_attr_count++)
                        {
                            if (element_attrs[filled_attr_count] == pavrc_cmd->get_elem_attrs.attrs[attr_cnt])
                                break;
                        }
                        if (filled_attr_count == num_attr)
                        {
                            element_attrs[num_attr] = (btrc_media_attr_t)pavrc_cmd->get_elem_attrs.attrs[attr_cnt];
                            num_attr++;
                        }
                    }
                }
            }
            FILL_PDU_QUEUE(IDX_GET_ELEMENT_ATTR_RSP, ctype, label, true);
            HAL_CBACK(bt_rc_callbacks, get_element_attr_cb, num_attr, element_attrs);
        }
        break;
        case AVRC_PDU_REGISTER_NOTIFICATION:
        {
            if (pavrc_cmd->reg_notif.event_id == BTRC_EVT_PLAY_POS_CHANGED &&
                pavrc_cmd->reg_notif.param == 0)
            {
                BTIF_TRACE_WARNING("%s Device registering position changed with illegal param 0.",
                    __func__);
                send_reject_response (btif_rc_cb.rc_handle, label, pavrc_cmd->pdu, AVRC_STS_BAD_PARAM);
                /* de-register this notification for a rejected response */
                btif_rc_cb.rc_notif[BTRC_EVT_PLAY_POS_CHANGED - 1].bNotify = false;
                return;
            }
            HAL_CBACK(bt_rc_callbacks, register_notification_cb, (btrc_event_id_t)pavrc_cmd->reg_notif.event_id,
                pavrc_cmd->reg_notif.param);
        }
        break;
        case AVRC_PDU_INFORM_DISPLAY_CHARSET:
        {
            tAVRC_RESPONSE avrc_rsp;
            BTIF_TRACE_EVENT("%s() AVRC_PDU_INFORM_DISPLAY_CHARSET", __func__);
            if (btif_rc_cb.rc_connected == true)
            {
                memset(&(avrc_rsp.inform_charset), 0, sizeof(tAVRC_RSP));
                avrc_rsp.inform_charset.opcode=opcode_from_pdu(AVRC_PDU_INFORM_DISPLAY_CHARSET);
                avrc_rsp.inform_charset.pdu=AVRC_PDU_INFORM_DISPLAY_CHARSET;
                avrc_rsp.inform_charset.status=AVRC_STS_NO_ERROR;
                send_metamsg_rsp(btif_rc_cb.rc_handle, label, ctype, &avrc_rsp);
            }
        }
        break;

        case AVRC_PDU_REQUEST_CONTINUATION_RSP:
        {
            BTIF_TRACE_EVENT("%s() REQUEST CONTINUATION: target_pdu: 0x%02d",
                             __func__, pavrc_cmd->continu.target_pdu);
            tAVRC_RESPONSE avrc_rsp;
            if (btif_rc_cb.rc_connected == TRUE)
            {
                memset(&(avrc_rsp.continu), 0, sizeof(tAVRC_NEXT_RSP));
                avrc_rsp.continu.opcode = opcode_from_pdu(AVRC_PDU_REQUEST_CONTINUATION_RSP);
                avrc_rsp.continu.pdu = AVRC_PDU_REQUEST_CONTINUATION_RSP;
                avrc_rsp.continu.status = AVRC_STS_NO_ERROR;
                avrc_rsp.continu.target_pdu = pavrc_cmd->continu.target_pdu;
                send_metamsg_rsp(btif_rc_cb.rc_handle, label, ctype, &avrc_rsp);
            }
        }
        break;

        case AVRC_PDU_ABORT_CONTINUATION_RSP:
        {
            BTIF_TRACE_EVENT("%s() ABORT CONTINUATION: target_pdu: 0x%02d",
                             __func__, pavrc_cmd->abort.target_pdu);
            tAVRC_RESPONSE avrc_rsp;
            if (btif_rc_cb.rc_connected == TRUE)
            {
                memset(&(avrc_rsp.abort), 0, sizeof(tAVRC_NEXT_RSP));
                avrc_rsp.abort.opcode = opcode_from_pdu(AVRC_PDU_ABORT_CONTINUATION_RSP);
                avrc_rsp.abort.pdu = AVRC_PDU_ABORT_CONTINUATION_RSP;
                avrc_rsp.abort.status = AVRC_STS_NO_ERROR;
                avrc_rsp.abort.target_pdu = pavrc_cmd->continu.target_pdu;
                send_metamsg_rsp(btif_rc_cb.rc_handle, label, ctype, &avrc_rsp);
            }
        }
        break;

        default:
        {
        send_reject_response (btif_rc_cb.rc_handle, label, pavrc_cmd->pdu,
            (pavrc_cmd->pdu == AVRC_PDU_SEARCH)?AVRC_STS_SEARCH_NOT_SUP:AVRC_STS_BAD_CMD);
        return;
        }
        break;
    }
}

#if (AVRC_CTRL_INCLUDED == TRUE)
/*******************************************************************************
**
** Function         btif_rc_ctrl_upstreams_rsp_cmd
**
** Description      Executes AVRC UPSTREAMS response events in btif context.
**
** Returns          void
**
*******************************************************************************/
static void btif_rc_ctrl_upstreams_rsp_cmd(uint8_t event, tAVRC_COMMAND *pavrc_cmd,
        uint8_t label)
{
    BTIF_TRACE_DEBUG("%s pdu: %s handle: 0x%x", __func__,
        dump_rc_pdu(pavrc_cmd->pdu), btif_rc_cb.rc_handle);
    bt_bdaddr_t rc_addr;
    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);
    switch (event)
    {
    case AVRC_PDU_SET_ABSOLUTE_VOLUME:
         HAL_CBACK(bt_rc_ctrl_callbacks,setabsvol_cmd_cb, &rc_addr,
                 pavrc_cmd->volume.volume, label);
         break;
    case AVRC_PDU_REGISTER_NOTIFICATION:
         if (pavrc_cmd->reg_notif.event_id == AVRC_EVT_VOLUME_CHANGE)
         {
             HAL_CBACK(bt_rc_ctrl_callbacks, registernotification_absvol_cb,
                    &rc_addr, label);
         }
         break;
    }
}
#endif

#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
/*******************************************************************************
**
** Function         btif_rc_upstreams_rsp_evt
**
** Description      Executes AVRC UPSTREAMS response events in btif context.
**
** Returns          void
**
*******************************************************************************/
static void btif_rc_upstreams_rsp_evt(uint16_t event, tAVRC_RESPONSE *pavrc_resp, uint8_t ctype, uint8_t label)
{
    BTIF_TRACE_EVENT("%s pdu: %s handle: 0x%x ctype:%x label:%x", __func__,
        dump_rc_pdu(pavrc_resp->pdu), btif_rc_cb.rc_handle, ctype, label);

    switch (event)
    {
        case AVRC_PDU_REGISTER_NOTIFICATION:
        {
             if (AVRC_RSP_CHANGED==ctype)
                 btif_rc_cb.rc_volume=pavrc_resp->reg_notif.param.volume;
             HAL_CBACK(bt_rc_callbacks, volume_change_cb, pavrc_resp->reg_notif.param.volume,ctype)
        }
        break;

        case AVRC_PDU_SET_ABSOLUTE_VOLUME:
        {
            BTIF_TRACE_DEBUG("%s Set absolute volume change event received: volume %d,ctype %d",
                             __func__, pavrc_resp->volume.volume,ctype);
            if (AVRC_RSP_ACCEPT==ctype)
                btif_rc_cb.rc_volume=pavrc_resp->volume.volume;
            HAL_CBACK(bt_rc_callbacks,volume_change_cb,pavrc_resp->volume.volume,ctype)
        }
        break;

        default:
            return;
    }
}
#endif

/************************************************************************************
**  AVRCP API Functions
************************************************************************************/

/*******************************************************************************
**
** Function         init
**
** Description      Initializes the AVRC interface
**
** Returns          bt_status_t
**
*******************************************************************************/
static bt_status_t init(btrc_callbacks_t* callbacks )
{
    BTIF_TRACE_EVENT("## %s ##", __func__);
    bt_status_t result = BT_STATUS_SUCCESS;

    if (bt_rc_callbacks)
        return BT_STATUS_DONE;

    bt_rc_callbacks = callbacks;
    memset (&btif_rc_cb, 0, sizeof(btif_rc_cb));
    btif_rc_cb.rc_vol_label=MAX_LABEL;
    btif_rc_cb.rc_volume=MAX_VOLUME;
    lbl_init();

    return result;
}

/*******************************************************************************
**
** Function         init_ctrl
**
** Description      Initializes the AVRC interface
**
** Returns          bt_status_t
**
*******************************************************************************/
static bt_status_t init_ctrl(btrc_ctrl_callbacks_t* callbacks )
{
    BTIF_TRACE_EVENT("## %s ##", __func__);
    bt_status_t result = BT_STATUS_SUCCESS;

    if (bt_rc_ctrl_callbacks)
        return BT_STATUS_DONE;

    bt_rc_ctrl_callbacks = callbacks;
    memset (&btif_rc_cb, 0, sizeof(btif_rc_cb));
    btif_rc_cb.rc_vol_label=MAX_LABEL;
    btif_rc_cb.rc_volume=MAX_VOLUME;
    lbl_init();

    return result;
}

static void rc_ctrl_procedure_complete ()
{
    if (btif_rc_cb.rc_procedure_complete == true)
    {
        return;
    }
    btif_rc_cb.rc_procedure_complete = true;
    uint32_t attr_list[] = {
            AVRC_MEDIA_ATTR_ID_TITLE,
            AVRC_MEDIA_ATTR_ID_ARTIST,
            AVRC_MEDIA_ATTR_ID_ALBUM,
            AVRC_MEDIA_ATTR_ID_TRACK_NUM,
            AVRC_MEDIA_ATTR_ID_NUM_TRACKS,
            AVRC_MEDIA_ATTR_ID_GENRE,
            AVRC_MEDIA_ATTR_ID_PLAYING_TIME
            };
    get_element_attribute_cmd (AVRC_MAX_NUM_MEDIA_ATTR_ID, attr_list);
}

/***************************************************************************
**
** Function         get_play_status_rsp
**
** Description      Returns the current play status.
**                      This method is called in response to
**                      GetPlayStatus request.
**
** Returns          bt_status_t
**
***************************************************************************/
static bt_status_t get_play_status_rsp(btrc_play_status_t play_status, uint32_t song_len,
    uint32_t song_pos)
{
    tAVRC_RESPONSE avrc_rsp;
    CHECK_RC_CONNECTED
    memset(&(avrc_rsp.get_play_status), 0, sizeof(tAVRC_GET_PLAY_STATUS_RSP));
    avrc_rsp.get_play_status.song_len = song_len;
    avrc_rsp.get_play_status.song_pos = song_pos;
    avrc_rsp.get_play_status.play_status = play_status;

    avrc_rsp.get_play_status.pdu = AVRC_PDU_GET_PLAY_STATUS;
    avrc_rsp.get_play_status.opcode = opcode_from_pdu(AVRC_PDU_GET_PLAY_STATUS);
    avrc_rsp.get_play_status.status = AVRC_STS_NO_ERROR;
    /* Send the response */
    SEND_METAMSG_RSP(IDX_GET_PLAY_STATUS_RSP, &avrc_rsp);
    return BT_STATUS_SUCCESS;
}

/***************************************************************************
**
** Function         get_element_attr_rsp
**
** Description      Returns the current songs' element attributes
**                      in text.
**
** Returns          bt_status_t
**
***************************************************************************/
static bt_status_t get_element_attr_rsp(uint8_t num_attr, btrc_element_attr_val_t *p_attrs)
{
    tAVRC_RESPONSE avrc_rsp;
    uint32_t i;
    tAVRC_ATTR_ENTRY element_attrs[BTRC_MAX_ELEM_ATTR_SIZE];
    CHECK_RC_CONNECTED
    memset(element_attrs, 0, sizeof(tAVRC_ATTR_ENTRY) * num_attr);

    if (num_attr == 0)
    {
        avrc_rsp.get_play_status.status = AVRC_STS_BAD_PARAM;
    }
    else
    {
        for (i=0; i<num_attr; i++) {
            element_attrs[i].attr_id = p_attrs[i].attr_id;
            element_attrs[i].name.charset_id = AVRC_CHARSET_ID_UTF8;
            element_attrs[i].name.str_len = (uint16_t)strlen((char *)p_attrs[i].text);
            element_attrs[i].name.p_str = p_attrs[i].text;
            BTIF_TRACE_DEBUG("%s attr_id:0x%x, charset_id:0x%x, str_len:%d, str:%s",
                             __func__, (unsigned int)element_attrs[i].attr_id,
                             element_attrs[i].name.charset_id, element_attrs[i].name.str_len,
                             element_attrs[i].name.p_str);
        }
        avrc_rsp.get_play_status.status = AVRC_STS_NO_ERROR;
    }
    avrc_rsp.get_elem_attrs.num_attr = num_attr;
    avrc_rsp.get_elem_attrs.p_attrs = element_attrs;
    avrc_rsp.get_elem_attrs.pdu = AVRC_PDU_GET_ELEMENT_ATTR;
    avrc_rsp.get_elem_attrs.opcode = opcode_from_pdu(AVRC_PDU_GET_ELEMENT_ATTR);
    /* Send the response */
    SEND_METAMSG_RSP(IDX_GET_ELEMENT_ATTR_RSP, &avrc_rsp);
    return BT_STATUS_SUCCESS;
}

/***************************************************************************
**
** Function         register_notification_rsp
**
** Description      Response to the register notification request.
**                      in text.
**
** Returns          bt_status_t
**
***************************************************************************/
static bt_status_t register_notification_rsp(btrc_event_id_t event_id,
    btrc_notification_type_t type, btrc_register_notification_t *p_param)
{
    tAVRC_RESPONSE avrc_rsp;
    CHECK_RC_CONNECTED
    BTIF_TRACE_EVENT("## %s ## event_id:%s", __func__, dump_rc_notification_event_id(event_id));
    if (btif_rc_cb.rc_notif[event_id-1].bNotify == false)
    {
        BTIF_TRACE_ERROR("Avrcp Event id not registered: event_id = %x", event_id);
        return BT_STATUS_NOT_READY;
    }
    memset(&(avrc_rsp.reg_notif), 0, sizeof(tAVRC_REG_NOTIF_RSP));
    avrc_rsp.reg_notif.event_id = event_id;

    switch(event_id)
    {
        case BTRC_EVT_PLAY_STATUS_CHANGED:
            avrc_rsp.reg_notif.param.play_status = p_param->play_status;
            if (avrc_rsp.reg_notif.param.play_status == PLAY_STATUS_PLAYING)
                btif_av_clear_remote_suspend_flag();
            break;
        case BTRC_EVT_TRACK_CHANGE:
            memcpy(&(avrc_rsp.reg_notif.param.track), &(p_param->track), sizeof(btrc_uid_t));
            break;
        case BTRC_EVT_PLAY_POS_CHANGED:
            avrc_rsp.reg_notif.param.play_pos = p_param->song_pos;
            break;
        default:
            BTIF_TRACE_WARNING("%s : Unhandled event ID : 0x%x", __func__, event_id);
            return BT_STATUS_UNHANDLED;
    }

    avrc_rsp.reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
    avrc_rsp.reg_notif.opcode = opcode_from_pdu(AVRC_PDU_REGISTER_NOTIFICATION);
    avrc_rsp.get_play_status.status = AVRC_STS_NO_ERROR;

    /* Send the response. */
    send_metamsg_rsp(btif_rc_cb.rc_handle, btif_rc_cb.rc_notif[event_id-1].label,
        ((type == BTRC_NOTIFICATION_TYPE_INTERIM)?AVRC_CMD_NOTIF:AVRC_RSP_CHANGED), &avrc_rsp);
    return BT_STATUS_SUCCESS;
}

/***************************************************************************
**
** Function         set_volume
**
** Description      Send current volume setting to remote side.
**                  Support limited to SetAbsoluteVolume
**                  This can be enhanced to support Relative Volume (AVRCP 1.0).
**                  With RelateVolume, we will send VOLUME_UP/VOLUME_DOWN
**                  as opposed to absolute volume level
** volume: Should be in the range 0-127. bit7 is reseved and cannot be set
**
** Returns          bt_status_t
**
***************************************************************************/
static bt_status_t set_volume(uint8_t volume)
{
    BTIF_TRACE_DEBUG("%s", __func__);
    CHECK_RC_CONNECTED
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;

    if (btif_rc_cb.rc_volume==volume)
    {
        status=BT_STATUS_DONE;
        BTIF_TRACE_ERROR("%s: volume value already set earlier: 0x%02x",__func__, volume);
        return (bt_status_t)status;
    }

    if ((btif_rc_cb.rc_features & BTA_AV_FEAT_RCTG) &&
        (btif_rc_cb.rc_features & BTA_AV_FEAT_ADV_CTRL))
    {
        tAVRC_COMMAND avrc_cmd = {0};
        BT_HDR *p_msg = NULL;

        BTIF_TRACE_DEBUG("%s: Peer supports absolute volume. newVolume=%d", __func__, volume);
        avrc_cmd.volume.opcode = AVRC_OP_VENDOR;
        avrc_cmd.volume.pdu = AVRC_PDU_SET_ABSOLUTE_VOLUME;
        avrc_cmd.volume.status = AVRC_STS_NO_ERROR;
        avrc_cmd.volume.volume = volume;

        if (AVRC_BldCommand(&avrc_cmd, &p_msg) == AVRC_STS_NO_ERROR)
        {
            bt_status_t tran_status=get_transaction(&p_transaction);
            if (BT_STATUS_SUCCESS == tran_status && NULL!=p_transaction)
            {
                BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                                   __func__,p_transaction->lbl);
                BTA_AvMetaCmd(btif_rc_cb.rc_handle,p_transaction->lbl, AVRC_CMD_CTRL, p_msg);
                status =  BT_STATUS_SUCCESS;
            }
            else
            {
                osi_free(p_msg);
                BTIF_TRACE_ERROR("%s: failed to obtain transaction details. status: 0x%02x",
                                    __func__, tran_status);
                status = BT_STATUS_FAIL;
            }
        }
        else
        {
            BTIF_TRACE_ERROR("%s: failed to build absolute volume command. status: 0x%02x",
                                __func__, status);
            status = BT_STATUS_FAIL;
        }
    }
    else
        status=BT_STATUS_NOT_READY;
    return (bt_status_t)status;
}

#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
/***************************************************************************
**
** Function         register_volumechange
**
** Description     Register for volume change notification from remote side.
**
** Returns          void
**
***************************************************************************/

static void register_volumechange (uint8_t lbl)
{
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    tAVRC_STS BldResp=AVRC_STS_BAD_CMD;
    rc_transaction_t *p_transaction=NULL;

    BTIF_TRACE_DEBUG("%s called with label:%d",__func__,lbl);

    avrc_cmd.cmd.opcode=0x00;
    avrc_cmd.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
    avrc_cmd.reg_notif.event_id = AVRC_EVT_VOLUME_CHANGE;
    avrc_cmd.reg_notif.status = AVRC_STS_NO_ERROR;
    avrc_cmd.reg_notif.param = 0;

    BldResp=AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (AVRC_STS_NO_ERROR == BldResp && p_msg) {
        p_transaction = get_transaction_by_lbl(lbl);
        if (p_transaction != NULL) {
            BTA_AvMetaCmd(btif_rc_cb.rc_handle, p_transaction->lbl,
                          AVRC_CMD_NOTIF, p_msg);
            BTIF_TRACE_DEBUG("%s:BTA_AvMetaCmd called", __func__);
         } else {
            osi_free(p_msg);
            BTIF_TRACE_ERROR("%s transaction not obtained with label: %d",
                             __func__, lbl);
         }
    } else {
        BTIF_TRACE_ERROR("%s failed to build command:%d", __func__, BldResp);
    }
}

/***************************************************************************
**
** Function         handle_rc_metamsg_rsp
**
** Description      Handle RC metamessage response
**
** Returns          void
**
***************************************************************************/
static void handle_rc_metamsg_rsp(tBTA_AV_META_MSG *pmeta_msg)
{
    tAVRC_RESPONSE    avrc_response = {0};
    uint8_t             scratch_buf[512] = {0};
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;

    if (AVRC_OP_VENDOR==pmeta_msg->p_msg->hdr.opcode &&(AVRC_RSP_CHANGED==pmeta_msg->code
      || AVRC_RSP_INTERIM==pmeta_msg->code || AVRC_RSP_ACCEPT==pmeta_msg->code
      || AVRC_RSP_REJ==pmeta_msg->code || AVRC_RSP_NOT_IMPL==pmeta_msg->code))
    {
        status=AVRC_ParsResponse(pmeta_msg->p_msg, &avrc_response, scratch_buf, sizeof(scratch_buf));
        BTIF_TRACE_DEBUG("%s: code %d,event ID %d,PDU %x,parsing status %d, label:%d",
          __func__,pmeta_msg->code,avrc_response.reg_notif.event_id,avrc_response.reg_notif.pdu,
          status, pmeta_msg->label);

        if (status != AVRC_STS_NO_ERROR)
        {
            if (AVRC_PDU_REGISTER_NOTIFICATION==avrc_response.rsp.pdu
                && AVRC_EVT_VOLUME_CHANGE==avrc_response.reg_notif.event_id
                && btif_rc_cb.rc_vol_label==pmeta_msg->label)
            {
                btif_rc_cb.rc_vol_label=MAX_LABEL;
                release_transaction(btif_rc_cb.rc_vol_label);
            }
            else if (AVRC_PDU_SET_ABSOLUTE_VOLUME==avrc_response.rsp.pdu)
            {
                release_transaction(pmeta_msg->label);
            }
            return;
        }
        else if (AVRC_PDU_REGISTER_NOTIFICATION==avrc_response.rsp.pdu
            && AVRC_EVT_VOLUME_CHANGE==avrc_response.reg_notif.event_id
            && btif_rc_cb.rc_vol_label!=pmeta_msg->label)
            {
                // Just discard the message, if the device sends back with an incorrect label
                BTIF_TRACE_DEBUG("%s:Discarding register notfn in rsp.code: %d and label %d",
                __func__, pmeta_msg->code, pmeta_msg->label);
                return;
            }
    }
    else
    {
        BTIF_TRACE_DEBUG("%s:Received vendor dependent in adv ctrl rsp. code: %d len: %d. Not processing it.",
        __func__, pmeta_msg->code, pmeta_msg->len);
        return;
    }

    if (AVRC_PDU_REGISTER_NOTIFICATION==avrc_response.rsp.pdu
        && AVRC_EVT_VOLUME_CHANGE==avrc_response.reg_notif.event_id
        && AVRC_RSP_CHANGED==pmeta_msg->code)
     {
         /* re-register for volume change notification */
         // Do not re-register for rejected case, as it might get into endless loop
         register_volumechange(btif_rc_cb.rc_vol_label);
     }
     else if (AVRC_PDU_SET_ABSOLUTE_VOLUME==avrc_response.rsp.pdu)
     {
          /* free up the label here */
          release_transaction(pmeta_msg->label);
     }

     BTIF_TRACE_EVENT("%s: Passing received metamsg response to app. pdu: %s",
             __func__, dump_rc_pdu(avrc_response.pdu));
     btif_rc_upstreams_rsp_evt((uint16_t)avrc_response.rsp.pdu, &avrc_response, pmeta_msg->code,
                                pmeta_msg->label);
}
#endif

#if (AVRC_CTRL_INCLUDED == TRUE)
/***************************************************************************
**
** Function         iterate_supported_event_list_for_interim_rsp
**
** Description      iterator callback function to match the event and handle
**                  timer cleanup
** Returns          true to continue iterating, false to stop
**
***************************************************************************/
bool iterate_supported_event_list_for_interim_rsp(void *data, void *cb_data)
{
    uint8_t *p_event_id;
    btif_rc_supported_event_t *p_event = (btif_rc_supported_event_t *)data;

    p_event_id = (uint8_t*)cb_data;

    if (p_event->event_id == *p_event_id)
    {
        p_event->status = eINTERIM;
        return false;
    }
    return true;
}

/***************************************************************************
**
** Function         iterate_supported_event_list_for_timeout
**
** Description      Iterator callback function for timeout handling.
**                  As part of the failure handling, it releases the
**                  transaction label and removes the event from list,
**                  this event will not be requested again during
**                  the lifetime of the connection.
** Returns          false to stop iterating, true to continue
**
***************************************************************************/
bool iterate_supported_event_list_for_timeout(void *data, void *cb_data)
{
    uint8_t label;
    btif_rc_supported_event_t *p_event = (btif_rc_supported_event_t *)data;

    label = (*(uint8_t*)cb_data) & 0xFF;

    if (p_event->label == label)
    {
        list_remove(btif_rc_cb.rc_supported_event_list, p_event);
        return false;
    }
    return true;
}

/***************************************************************************
**
** Function         rc_notification_interim_timout
**
** Description      Interim response timeout handler.
**                  Runs the iterator to check and clear the timed out event.
**                  Proceeds to register for the unregistered events.
** Returns          None
**
***************************************************************************/
static void rc_notification_interim_timout (uint8_t label)
{
    list_node_t *node;

    list_foreach(btif_rc_cb.rc_supported_event_list,
                     iterate_supported_event_list_for_timeout, &label);
    /* Timeout happened for interim response for the registered event,
     * check if there are any pending for registration
     */
    node = list_begin(btif_rc_cb.rc_supported_event_list);
    while (node != NULL)
    {
        btif_rc_supported_event_t *p_event;

        p_event = (btif_rc_supported_event_t *)list_node(node);
        if ((p_event != NULL) && (p_event->status == eNOT_REGISTERED))
        {
            register_for_event_notification(p_event);
            break;
        }
        node = list_next (node);
    }
    /* Todo. Need to initiate application settings query if this
     * is the last event registration.
     */
}

/***************************************************************************
**
** Function         btif_rc_status_cmd_timeout_handler
**
** Description      RC status command timeout handler (Runs in BTIF context).
** Returns          None
**
***************************************************************************/
static void btif_rc_status_cmd_timeout_handler(UNUSED_ATTR uint16_t event,
                                               char *data)
{
    btif_rc_timer_context_t *p_context;
    tAVRC_RESPONSE      avrc_response = {0};
    tBTA_AV_META_MSG    meta_msg;

    p_context = (btif_rc_timer_context_t *)data;
    memset(&meta_msg, 0, sizeof(tBTA_AV_META_MSG));
    meta_msg.rc_handle = btif_rc_cb.rc_handle;

    switch (p_context->rc_status_cmd.pdu_id) {
    case AVRC_PDU_REGISTER_NOTIFICATION:
        rc_notification_interim_timout(p_context->rc_status_cmd.label);
        break;

    case AVRC_PDU_GET_CAPABILITIES:
        avrc_response.get_caps.status = BTIF_RC_STS_TIMEOUT;
        handle_get_capability_response(&meta_msg, &avrc_response.get_caps);
        break;

    case AVRC_PDU_LIST_PLAYER_APP_ATTR:
        avrc_response.list_app_attr.status = BTIF_RC_STS_TIMEOUT;
        handle_app_attr_response(&meta_msg, &avrc_response.list_app_attr);
        break;

    case AVRC_PDU_LIST_PLAYER_APP_VALUES:
        avrc_response.list_app_values.status = BTIF_RC_STS_TIMEOUT;
        handle_app_val_response(&meta_msg, &avrc_response.list_app_values);
        break;

    case AVRC_PDU_GET_CUR_PLAYER_APP_VALUE:
        avrc_response.get_cur_app_val.status = BTIF_RC_STS_TIMEOUT;
        handle_app_cur_val_response(&meta_msg, &avrc_response.get_cur_app_val);
        break;

    case AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT:
        avrc_response.get_app_attr_txt.status = BTIF_RC_STS_TIMEOUT;
        handle_app_attr_txt_response(&meta_msg, &avrc_response.get_app_attr_txt);
        break;

    case AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT:
        avrc_response.get_app_val_txt.status = BTIF_RC_STS_TIMEOUT;
        handle_app_attr_txt_response(&meta_msg, &avrc_response.get_app_val_txt);
        break;

    case AVRC_PDU_GET_ELEMENT_ATTR:
        avrc_response.get_elem_attrs.status = BTIF_RC_STS_TIMEOUT;
        handle_get_elem_attr_response(&meta_msg, &avrc_response.get_elem_attrs);
        break;

    case AVRC_PDU_GET_PLAY_STATUS:
        avrc_response.get_play_status.status = BTIF_RC_STS_TIMEOUT;
        handle_get_playstatus_response(&meta_msg, &avrc_response.get_play_status);
        break;
    }
    release_transaction(p_context->rc_status_cmd.label);
}

/***************************************************************************
**
** Function         btif_rc_status_cmd_timer_timeout
**
** Description      RC status command timeout callback.
**                  This is called from BTU context and switches to BTIF
**                  context to handle the timeout events
** Returns          None
**
***************************************************************************/
static void btif_rc_status_cmd_timer_timeout(void *data)
{
    btif_rc_timer_context_t *p_data = (btif_rc_timer_context_t *)data;

    btif_transfer_context(btif_rc_status_cmd_timeout_handler, 0,
                          (char *)p_data, sizeof(btif_rc_timer_context_t),
                          NULL);
}

/***************************************************************************
**
** Function         btif_rc_control_cmd_timeout_handler
**
** Description      RC control command timeout handler (Runs in BTIF context).
** Returns          None
**
***************************************************************************/
static void btif_rc_control_cmd_timeout_handler(UNUSED_ATTR uint16_t event,
                                                char *data)
{
    btif_rc_timer_context_t *p_context = (btif_rc_timer_context_t *)data;
    tAVRC_RESPONSE      avrc_response = {0};
    tBTA_AV_META_MSG    meta_msg;

    memset(&meta_msg, 0, sizeof(tBTA_AV_META_MSG));
    meta_msg.rc_handle = btif_rc_cb.rc_handle;

    switch (p_context->rc_control_cmd.pdu_id) {
    case AVRC_PDU_SET_PLAYER_APP_VALUE:
        avrc_response.set_app_val.status = BTIF_RC_STS_TIMEOUT;
        handle_set_app_attr_val_response(&meta_msg,
                                         &avrc_response.set_app_val);
        break;
    }
    release_transaction(p_context->rc_control_cmd.label);
}

/***************************************************************************
**
** Function         btif_rc_control_cmd_timer_timeout
**
** Description      RC control command timeout callback.
**                  This is called from BTU context and switches to BTIF
**                  context to handle the timeout events
** Returns          None
**
***************************************************************************/
static void btif_rc_control_cmd_timer_timeout(void *data)
{
    btif_rc_timer_context_t *p_data = (btif_rc_timer_context_t *)data;

    btif_transfer_context(btif_rc_control_cmd_timeout_handler, 0,
                          (char *)p_data, sizeof(btif_rc_timer_context_t),
                          NULL);
}

/***************************************************************************
**
** Function         btif_rc_play_status_timeout_handler
**
** Description      RC play status timeout handler (Runs in BTIF context).
** Returns          None
**
***************************************************************************/
static void btif_rc_play_status_timeout_handler(UNUSED_ATTR uint16_t event,
                                                UNUSED_ATTR char *p_data)
{
    get_play_status_cmd();
    rc_start_play_status_timer();
}

/***************************************************************************
**
** Function         btif_rc_play_status_timer_timeout
**
** Description      RC play status timeout callback.
**                  This is called from BTU context and switches to BTIF
**                  context to handle the timeout events
** Returns          None
**
***************************************************************************/
static void btif_rc_play_status_timer_timeout(UNUSED_ATTR void *data)
{
    btif_transfer_context(btif_rc_play_status_timeout_handler, 0, 0, 0, NULL);
}

/***************************************************************************
**
** Function         rc_start_play_status_timer
**
** Description      Helper function to start the timer to fetch play status.
** Returns          None
**
***************************************************************************/
static void rc_start_play_status_timer(void)
{
    /* Start the Play status timer only if it is not started */
    if (!alarm_is_scheduled(btif_rc_cb.rc_play_status_timer)) {
        if (btif_rc_cb.rc_play_status_timer == NULL) {
            btif_rc_cb.rc_play_status_timer =
                alarm_new("btif_rc.rc_play_status_timer");
        }
        alarm_set_on_queue(btif_rc_cb.rc_play_status_timer,
                           BTIF_TIMEOUT_RC_INTERIM_RSP_MS,
                           btif_rc_play_status_timer_timeout, NULL,
                           btu_general_alarm_queue);
    }
}

/***************************************************************************
**
** Function         rc_stop_play_status_timer
**
** Description      Helper function to stop the play status timer.
** Returns          None
**
***************************************************************************/
void rc_stop_play_status_timer()
{
    if (btif_rc_cb.rc_play_status_timer != NULL)
        alarm_cancel(btif_rc_cb.rc_play_status_timer);
}

/***************************************************************************
**
** Function         register_for_event_notification
**
** Description      Helper function registering notification events
**                  sets an interim response timeout to handle if the remote
**                  does not respond.
** Returns          None
**
***************************************************************************/
static void register_for_event_notification(btif_rc_supported_event_t *p_event)
{
    bt_status_t status;
    rc_transaction_t *p_transaction;

    status = get_transaction(&p_transaction);
    if (status == BT_STATUS_SUCCESS)
    {
        btif_rc_timer_context_t *p_context = &p_transaction->txn_timer_context;

        status = register_notification_cmd (p_transaction->lbl, p_event->event_id, 0);
        if (status != BT_STATUS_SUCCESS)
        {
            BTIF_TRACE_ERROR("%s Error in Notification registration %d",
                __func__, status);
            release_transaction (p_transaction->lbl);
            return;
        }
        p_event->label = p_transaction->lbl;
        p_event->status = eREGISTERED;
        p_context->rc_status_cmd.label = p_transaction->lbl;
        p_context->rc_status_cmd.pdu_id = AVRC_PDU_REGISTER_NOTIFICATION;

        alarm_free(p_transaction->txn_timer);
        p_transaction->txn_timer =
            alarm_new("btif_rc.status_command_txn_timer");
        alarm_set_on_queue(p_transaction->txn_timer,
                           BTIF_TIMEOUT_RC_INTERIM_RSP_MS,
                           btif_rc_status_cmd_timer_timeout, p_context,
                           btu_general_alarm_queue);
    }
    else
    {
        BTIF_TRACE_ERROR("%s Error No more Transaction label %d",
            __func__, status);
    }
}

static void start_status_command_timer(uint8_t pdu_id, rc_transaction_t *p_txn)
{
    btif_rc_timer_context_t *p_context = &p_txn->txn_timer_context;
    p_context->rc_status_cmd.label = p_txn->lbl;
    p_context->rc_status_cmd.pdu_id = pdu_id;

    alarm_free(p_txn->txn_timer);
    p_txn->txn_timer = alarm_new("btif_rc.status_command_txn_timer");
    alarm_set_on_queue(p_txn->txn_timer, BTIF_TIMEOUT_RC_STATUS_CMD_MS,
                       btif_rc_status_cmd_timer_timeout, p_context,
                       btu_general_alarm_queue);
}

static void start_control_command_timer(uint8_t pdu_id, rc_transaction_t *p_txn)
{
    btif_rc_timer_context_t *p_context = &p_txn->txn_timer_context;
    p_context->rc_control_cmd.label = p_txn->lbl;
    p_context->rc_control_cmd.pdu_id = pdu_id;

    alarm_free(p_txn->txn_timer);
    p_txn->txn_timer = alarm_new("btif_rc.control_command_txn_timer");
    alarm_set_on_queue(p_txn->txn_timer,
                       BTIF_TIMEOUT_RC_CONTROL_CMD_MS,
                       btif_rc_control_cmd_timer_timeout, p_context,
                       btu_general_alarm_queue);
}

/***************************************************************************
**
** Function         handle_get_capability_response
**
** Description      Handles the get_cap_response to populate company id info
**                  and query the supported events.
**                  Initiates Notification registration for events supported
** Returns          None
**
***************************************************************************/
static void handle_get_capability_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_CAPS_RSP *p_rsp)
{
    int xx = 0;

    /* Todo: Do we need to retry on command timeout */
    if (p_rsp->status != AVRC_STS_NO_ERROR)
    {
        BTIF_TRACE_ERROR("%s Error capability response 0x%02X",
                __func__, p_rsp->status);
        return;
    }

    if (p_rsp->capability_id == AVRC_CAP_EVENTS_SUPPORTED)
    {
        btif_rc_supported_event_t *p_event;

        /* Todo: Check if list can be active when we hit here */
        btif_rc_cb.rc_supported_event_list = list_new(osi_free);
        for (xx = 0; xx < p_rsp->count; xx++)
        {
            /* Skip registering for Play position change notification */
            if ((p_rsp->param.event_id[xx] == AVRC_EVT_PLAY_STATUS_CHANGE)||
                (p_rsp->param.event_id[xx] == AVRC_EVT_TRACK_CHANGE)||
                (p_rsp->param.event_id[xx] == AVRC_EVT_APP_SETTING_CHANGE))
            {
                p_event = (btif_rc_supported_event_t *)osi_malloc(sizeof(btif_rc_supported_event_t));
                p_event->event_id = p_rsp->param.event_id[xx];
                p_event->status = eNOT_REGISTERED;
                list_append(btif_rc_cb.rc_supported_event_list, p_event);
            }
        }
        p_event = (btif_rc_supported_event_t*)list_front(btif_rc_cb.rc_supported_event_list);
        if (p_event != NULL)
        {
            register_for_event_notification(p_event);
        }
    }
    else if (p_rsp->capability_id == AVRC_CAP_COMPANY_ID)
    {
        getcapabilities_cmd (AVRC_CAP_EVENTS_SUPPORTED);
        BTIF_TRACE_EVENT("%s AVRC_CAP_COMPANY_ID: ", __func__);
        for (xx = 0; xx < p_rsp->count; xx++)
        {
            BTIF_TRACE_EVENT("%s    : %d", __func__, p_rsp->param.company_id[xx]);
        }
    }
}

bool rc_is_track_id_valid (tAVRC_UID uid)
{
    tAVRC_UID invalid_uid = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    if (memcmp(uid, invalid_uid, sizeof(tAVRC_UID)) == 0)
    {
        return false;
    }
    else
    {
        return true;
    }
}

/***************************************************************************
**
** Function         handle_notification_response
**
** Description      Main handler for notification responses to registered events
**                  1. Register for unregistered event(in interim response path)
**                  2. After registering for all supported events, start
**                     retrieving application settings and values
**                  3. Reregister for events on getting changed response
**                  4. Run play status timer for getting position when the
**                     status changes to playing
**                  5. Get the Media details when the track change happens
**                     or track change interim response is received with
**                     valid track id
**                  6. HAL callback for play status change and application
**                     setting change
** Returns          None
**
***************************************************************************/
static void handle_notification_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_REG_NOTIF_RSP *p_rsp)
{
    bt_bdaddr_t rc_addr;
    uint32_t attr_list[] = {
        AVRC_MEDIA_ATTR_ID_TITLE,
        AVRC_MEDIA_ATTR_ID_ARTIST,
        AVRC_MEDIA_ATTR_ID_ALBUM,
        AVRC_MEDIA_ATTR_ID_TRACK_NUM,
        AVRC_MEDIA_ATTR_ID_NUM_TRACKS,
        AVRC_MEDIA_ATTR_ID_GENRE,
        AVRC_MEDIA_ATTR_ID_PLAYING_TIME
        };


    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);

    if (pmeta_msg->code == AVRC_RSP_INTERIM)
    {
        btif_rc_supported_event_t *p_event;
        list_node_t *node;

        BTIF_TRACE_DEBUG("%s Interim response : 0x%2X ", __func__, p_rsp->event_id);
        switch (p_rsp->event_id)
        {
            case AVRC_EVT_PLAY_STATUS_CHANGE:
                /* Start timer to get play status periodically
                 * if the play state is playing.
                 */
                if (p_rsp->param.play_status == AVRC_PLAYSTATE_PLAYING)
                {
                    rc_start_play_status_timer();
                }
                HAL_CBACK(bt_rc_ctrl_callbacks, play_status_changed_cb,
                    &rc_addr, (btrc_play_status_t)p_rsp->param.play_status);
                break;

            case AVRC_EVT_TRACK_CHANGE:
                if (rc_is_track_id_valid (p_rsp->param.track) != true)
                {
                    break;
                }
                else
                {
                    uint8_t *p_data = p_rsp->param.track;
                    /* Update the UID for current track
                     * Attributes will be fetched after the AVRCP procedure
                     */
                    BE_STREAM_TO_UINT64(btif_rc_cb.rc_playing_uid, p_data);
                }
                break;

            case AVRC_EVT_APP_SETTING_CHANGE:
                break;

            case AVRC_EVT_NOW_PLAYING_CHANGE:
                break;

            case AVRC_EVT_AVAL_PLAYERS_CHANGE:
                break;

            case AVRC_EVT_ADDR_PLAYER_CHANGE:
                break;

            case AVRC_EVT_UIDS_CHANGE:
                break;

            case AVRC_EVT_TRACK_REACHED_END:
            case AVRC_EVT_TRACK_REACHED_START:
            case AVRC_EVT_PLAY_POS_CHANGED:
            case AVRC_EVT_BATTERY_STATUS_CHANGE:
            case AVRC_EVT_SYSTEM_STATUS_CHANGE:
            default:
                BTIF_TRACE_ERROR("%s  Unhandled interim response 0x%2X", __func__,
                    p_rsp->event_id);
                return;
        }
        list_foreach(btif_rc_cb.rc_supported_event_list,
                iterate_supported_event_list_for_interim_rsp,
                &p_rsp->event_id);

        node = list_begin(btif_rc_cb.rc_supported_event_list);
        while (node != NULL)
        {
            p_event = (btif_rc_supported_event_t *)list_node(node);
            if ((p_event != NULL) && (p_event->status == eNOT_REGISTERED))
            {
                register_for_event_notification(p_event);
                break;
            }
            node = list_next (node);
            p_event = NULL;
        }
        /* Registered for all events, we can request application settings */
        if ((p_event == NULL) && (btif_rc_cb.rc_app_settings.query_started == false))
        {
            /* we need to do this only if remote TG supports
             * player application settings
             */
            btif_rc_cb.rc_app_settings.query_started = true;
            if (btif_rc_cb.rc_features & BTA_AV_FEAT_APP_SETTING)
            {
                list_player_app_setting_attrib_cmd();
            }
            else
            {
                BTIF_TRACE_DEBUG("%s App setting not supported, complete procedure", __func__);
                rc_ctrl_procedure_complete();
            }
        }
    }
    else if (pmeta_msg->code == AVRC_RSP_CHANGED)
    {
        btif_rc_supported_event_t *p_event;
        list_node_t *node;

        BTIF_TRACE_DEBUG("%s Notification completed : 0x%2X ", __func__,
            p_rsp->event_id);

        node = list_begin(btif_rc_cb.rc_supported_event_list);
        while (node != NULL)
        {
            p_event = (btif_rc_supported_event_t *)list_node(node);
            if ((p_event != NULL) && (p_event->event_id == p_rsp->event_id))
            {
                p_event->status = eNOT_REGISTERED;
                register_for_event_notification(p_event);
                break;
            }
            node = list_next (node);
        }

        switch (p_rsp->event_id)
        {
            case AVRC_EVT_PLAY_STATUS_CHANGE:
                /* Start timer to get play status periodically
                 * if the play state is playing.
                 */
                if (p_rsp->param.play_status == AVRC_PLAYSTATE_PLAYING)
                {
                    rc_start_play_status_timer();
                }
                else
                {
                    rc_stop_play_status_timer();
                }
                HAL_CBACK(bt_rc_ctrl_callbacks, play_status_changed_cb,
                    &rc_addr, (btrc_play_status_t)p_rsp->param.play_status);
                break;

            case AVRC_EVT_TRACK_CHANGE:
                if (rc_is_track_id_valid (p_rsp->param.track) != true)
                {
                    break;
                }
                get_element_attribute_cmd (AVRC_MAX_NUM_MEDIA_ATTR_ID, attr_list);
                break;

            case AVRC_EVT_APP_SETTING_CHANGE:
            {
                btrc_player_settings_t app_settings;
                uint16_t xx;

                app_settings.num_attr = p_rsp->param.player_setting.num_attr;
                for (xx = 0; xx < app_settings.num_attr; xx++)
                {
                    app_settings.attr_ids[xx] = p_rsp->param.player_setting.attr_id[xx];
                    app_settings.attr_values[xx] = p_rsp->param.player_setting.attr_value[xx];
                }
                HAL_CBACK(bt_rc_ctrl_callbacks, playerapplicationsetting_changed_cb,
                    &rc_addr, &app_settings);
            }
                break;

            case AVRC_EVT_NOW_PLAYING_CHANGE:
                break;

            case AVRC_EVT_AVAL_PLAYERS_CHANGE:
                break;

            case AVRC_EVT_ADDR_PLAYER_CHANGE:
                break;

            case AVRC_EVT_UIDS_CHANGE:
                break;

            case AVRC_EVT_TRACK_REACHED_END:
            case AVRC_EVT_TRACK_REACHED_START:
            case AVRC_EVT_PLAY_POS_CHANGED:
            case AVRC_EVT_BATTERY_STATUS_CHANGE:
            case AVRC_EVT_SYSTEM_STATUS_CHANGE:
            default:
                BTIF_TRACE_ERROR("%s  Unhandled completion response 0x%2X",
                    __func__, p_rsp->event_id);
                return;
        }
    }
}

/***************************************************************************
**
** Function         handle_app_attr_response
**
** Description      handles the the application attributes response and
**                  initiates procedure to fetch the attribute values
** Returns          None
**
***************************************************************************/
static void handle_app_attr_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_LIST_APP_ATTR_RSP *p_rsp)
{
    uint8_t xx;

    if (p_rsp->status != AVRC_STS_NO_ERROR)
    {
        BTIF_TRACE_ERROR("%s Error getting Player application settings: 0x%2X",
                __func__, p_rsp->status);
        rc_ctrl_procedure_complete();
        return;
    }

    for (xx = 0; xx < p_rsp->num_attr; xx++)
    {
        uint8_t st_index;

        if (p_rsp->attrs[xx] > AVRC_PLAYER_SETTING_LOW_MENU_EXT)
        {
            st_index = btif_rc_cb.rc_app_settings.num_ext_attrs;
            btif_rc_cb.rc_app_settings.ext_attrs[st_index].attr_id = p_rsp->attrs[xx];
            btif_rc_cb.rc_app_settings.num_ext_attrs++;
        }
        else
        {
            st_index = btif_rc_cb.rc_app_settings.num_attrs;
            btif_rc_cb.rc_app_settings.attrs[st_index].attr_id = p_rsp->attrs[xx];
            btif_rc_cb.rc_app_settings.num_attrs++;
        }
    }
    btif_rc_cb.rc_app_settings.attr_index = 0;
    btif_rc_cb.rc_app_settings.ext_attr_index = 0;
    btif_rc_cb.rc_app_settings.ext_val_index = 0;
    if (p_rsp->num_attr)
    {
        list_player_app_setting_value_cmd (btif_rc_cb.rc_app_settings.attrs[0].attr_id);
    }
    else
    {
        BTIF_TRACE_ERROR("%s No Player application settings found",
                __func__);
    }
}

/***************************************************************************
**
** Function         handle_app_val_response
**
** Description      handles the the attributes value response and if extended
**                  menu is available, it initiates query for the attribute
**                  text. If not, it initiates procedure to get the current
**                  attribute values and calls the HAL callback for provding
**                  application settings information.
** Returns          None
**
***************************************************************************/
static void handle_app_val_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_LIST_APP_VALUES_RSP *p_rsp)
{
    uint8_t xx, attr_index;
    uint8_t attrs[AVRC_MAX_APP_ATTR_SIZE];
    btif_rc_player_app_settings_t *p_app_settings;
    bt_bdaddr_t rc_addr;

    /* Todo: Do we need to retry on command timeout */
    if (p_rsp->status != AVRC_STS_NO_ERROR)
    {
        BTIF_TRACE_ERROR("%s Error fetching attribute values 0x%02X",
                __func__, p_rsp->status);
        return;
    }

    p_app_settings = &btif_rc_cb.rc_app_settings;
    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);

    if (p_app_settings->attr_index < p_app_settings->num_attrs)
    {
        attr_index = p_app_settings->attr_index;
        p_app_settings->attrs[attr_index].num_val = p_rsp->num_val;
        for (xx = 0; xx < p_rsp->num_val; xx++)
        {
            p_app_settings->attrs[attr_index].attr_val[xx] = p_rsp->vals[xx];
        }
        attr_index++;
        p_app_settings->attr_index++;
        if (attr_index < p_app_settings->num_attrs)
        {
            list_player_app_setting_value_cmd (p_app_settings->attrs[p_app_settings->attr_index].attr_id);
        }
        else if (p_app_settings->ext_attr_index < p_app_settings->num_ext_attrs)
        {
            attr_index = 0;
            p_app_settings->ext_attr_index = 0;
            list_player_app_setting_value_cmd (p_app_settings->ext_attrs[attr_index].attr_id);
        }
        else
        {
            for (xx = 0; xx < p_app_settings->num_attrs; xx++)
            {
                attrs[xx] = p_app_settings->attrs[xx].attr_id;
            }
            get_player_app_setting_cmd (p_app_settings->num_attrs, attrs);
            HAL_CBACK (bt_rc_ctrl_callbacks, playerapplicationsetting_cb, &rc_addr,
                        p_app_settings->num_attrs, p_app_settings->attrs, 0, NULL);
        }
    }
    else if (p_app_settings->ext_attr_index < p_app_settings->num_ext_attrs)
    {
        attr_index = p_app_settings->ext_attr_index;
        p_app_settings->ext_attrs[attr_index].num_val = p_rsp->num_val;
        for (xx = 0; xx < p_rsp->num_val; xx++)
        {
            p_app_settings->ext_attrs[attr_index].ext_attr_val[xx].val = p_rsp->vals[xx];
        }
        attr_index++;
        p_app_settings->ext_attr_index++;
        if (attr_index < p_app_settings->num_ext_attrs)
        {
            list_player_app_setting_value_cmd (p_app_settings->ext_attrs[p_app_settings->ext_attr_index].attr_id);
        }
        else
        {
            uint8_t attr[AVRC_MAX_APP_ATTR_SIZE];
            uint8_t xx;

            for (xx = 0; xx < p_app_settings->num_ext_attrs; xx++)
            {
                attr[xx] = p_app_settings->ext_attrs[xx].attr_id;
            }
            get_player_app_setting_attr_text_cmd(attr, xx);
        }
    }
}

/***************************************************************************
**
** Function         handle_app_cur_val_response
**
** Description      handles the the get attributes value response.
**
** Returns          None
**
***************************************************************************/
static void handle_app_cur_val_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_CUR_APP_VALUE_RSP *p_rsp)
{
    btrc_player_settings_t app_settings;
    bt_bdaddr_t rc_addr;
    uint16_t xx;

    /* Todo: Do we need to retry on command timeout */
    if (p_rsp->status != AVRC_STS_NO_ERROR)
    {
        BTIF_TRACE_ERROR("%s Error fetching current settings: 0x%02X",
                __func__, p_rsp->status);
        return;
    }

    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);

    app_settings.num_attr = p_rsp->num_val;
    for (xx = 0; xx < app_settings.num_attr; xx++)
    {
        app_settings.attr_ids[xx] = p_rsp->p_vals[xx].attr_id;
        app_settings.attr_values[xx] = p_rsp->p_vals[xx].attr_val;
    }
    HAL_CBACK(bt_rc_ctrl_callbacks, playerapplicationsetting_changed_cb,
        &rc_addr, &app_settings);
    /* Application settings are fetched only once for initial values
     * initiate anything that follows after RC procedure.
     * Defer it if browsing is supported till players query
     */
    rc_ctrl_procedure_complete ();
    osi_free_and_reset((void **)&p_rsp->p_vals);
}

/***************************************************************************
**
** Function         handle_app_attr_txt_response
**
** Description      handles the the get attributes text response, if fails
**                  calls HAL callback with just normal settings and initiates
**                  query for current settings else initiates query for value text
** Returns          None
**
***************************************************************************/
static void handle_app_attr_txt_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_APP_ATTR_TXT_RSP *p_rsp)
{
    uint8_t xx;
    uint8_t vals[AVRC_MAX_APP_ATTR_SIZE];
    btif_rc_player_app_settings_t *p_app_settings;
    bt_bdaddr_t rc_addr;

    p_app_settings = &btif_rc_cb.rc_app_settings;
    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);

    /* Todo: Do we need to retry on command timeout */
    if (p_rsp->status != AVRC_STS_NO_ERROR)
    {
        uint8_t attrs[AVRC_MAX_APP_ATTR_SIZE];

        BTIF_TRACE_ERROR("%s Error fetching attribute text: 0x%02X",
                __func__, p_rsp->status);
        /* Not able to fetch Text for extended Menu, skip the process
         * and cleanup used memory. Proceed to get the current settings
         * for standard attributes.
         */
        p_app_settings->num_ext_attrs = 0;
        for (xx = 0; xx < p_app_settings->ext_attr_index; xx++)
            osi_free_and_reset((void **)&p_app_settings->ext_attrs[xx].p_str);
        p_app_settings->ext_attr_index = 0;

        for (xx = 0; xx < p_app_settings->num_attrs; xx++)
        {
            attrs[xx] = p_app_settings->attrs[xx].attr_id;
        }
        HAL_CBACK (bt_rc_ctrl_callbacks, playerapplicationsetting_cb, &rc_addr,
                    p_app_settings->num_attrs, p_app_settings->attrs, 0, NULL);

        get_player_app_setting_cmd (xx, attrs);
        return;
    }

    for (xx = 0; xx < p_rsp->num_attr; xx++)
    {
        uint8_t x;
        for (x = 0; x < p_app_settings->num_ext_attrs; x++)
        {
            if (p_app_settings->ext_attrs[x].attr_id == p_rsp->p_attrs[xx].attr_id)
            {
                p_app_settings->ext_attrs[x].charset_id = p_rsp->p_attrs[xx].charset_id;
                p_app_settings->ext_attrs[x].str_len = p_rsp->p_attrs[xx].str_len;
                p_app_settings->ext_attrs[x].p_str = p_rsp->p_attrs[xx].p_str;
                break;
            }
        }
    }

    for (xx = 0; xx < p_app_settings->ext_attrs[0].num_val; xx++)
    {
        vals[xx] = p_app_settings->ext_attrs[0].ext_attr_val[xx].val;
    }
    get_player_app_setting_value_text_cmd(vals, xx);
}


/***************************************************************************
**
** Function         handle_app_attr_val_txt_response
**
** Description      handles the the get attributes value text response, if fails
**                  calls HAL callback with just normal settings and initiates
**                  query for current settings
** Returns          None
**
***************************************************************************/
static void handle_app_attr_val_txt_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_APP_ATTR_TXT_RSP *p_rsp)
{
    uint8_t xx, attr_index;
    uint8_t vals[AVRC_MAX_APP_ATTR_SIZE];
    uint8_t attrs[AVRC_MAX_APP_ATTR_SIZE];
    btif_rc_player_app_settings_t *p_app_settings;
    bt_bdaddr_t rc_addr;

    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);
    p_app_settings = &btif_rc_cb.rc_app_settings;

    /* Todo: Do we need to retry on command timeout */
    if (p_rsp->status != AVRC_STS_NO_ERROR)
    {
        uint8_t attrs[AVRC_MAX_APP_ATTR_SIZE];

        BTIF_TRACE_ERROR("%s Error fetching attribute value text: 0x%02X",
                __func__, p_rsp->status);

        /* Not able to fetch Text for extended Menu, skip the process
         * and cleanup used memory. Proceed to get the current settings
         * for standard attributes.
         */
        p_app_settings->num_ext_attrs = 0;
        for (xx = 0; xx < p_app_settings->ext_attr_index; xx++)
        {
            int x;
            btrc_player_app_ext_attr_t *p_ext_attr = &p_app_settings->ext_attrs[xx];

            for (x = 0; x < p_ext_attr->num_val; x++)
                osi_free_and_reset((void **)&p_ext_attr->ext_attr_val[x].p_str);
            p_ext_attr->num_val = 0;
            osi_free_and_reset((void **)&p_app_settings->ext_attrs[xx].p_str);
        }
        p_app_settings->ext_attr_index = 0;

        for (xx = 0; xx < p_app_settings->num_attrs; xx++)
        {
            attrs[xx] = p_app_settings->attrs[xx].attr_id;
        }
        HAL_CBACK (bt_rc_ctrl_callbacks, playerapplicationsetting_cb, &rc_addr,
                    p_app_settings->num_attrs, p_app_settings->attrs, 0, NULL);

        get_player_app_setting_cmd (xx, attrs);
        return;
    }

    for (xx = 0; xx < p_rsp->num_attr; xx++)
    {
        uint8_t x;
        btrc_player_app_ext_attr_t *p_ext_attr;
        p_ext_attr = &p_app_settings->ext_attrs[p_app_settings->ext_val_index];
        for (x = 0; x < p_rsp->num_attr; x++)
        {
            if (p_ext_attr->ext_attr_val[x].val == p_rsp->p_attrs[xx].attr_id)
            {
                p_ext_attr->ext_attr_val[x].charset_id = p_rsp->p_attrs[xx].charset_id;
                p_ext_attr->ext_attr_val[x].str_len = p_rsp->p_attrs[xx].str_len;
                p_ext_attr->ext_attr_val[x].p_str = p_rsp->p_attrs[xx].p_str;
                break;
            }
        }
    }
    p_app_settings->ext_val_index++;

    if (p_app_settings->ext_val_index < p_app_settings->num_ext_attrs)
    {
        attr_index = p_app_settings->ext_val_index;
        for (xx = 0; xx < p_app_settings->ext_attrs[attr_index].num_val; xx++)
        {
            vals[xx] = p_app_settings->ext_attrs[attr_index].ext_attr_val[xx].val;
        }
        get_player_app_setting_value_text_cmd(vals, xx);
    }
    else
    {
        uint8_t x;

        for (xx = 0; xx < p_app_settings->num_attrs; xx++)
        {
            attrs[xx] = p_app_settings->attrs[xx].attr_id;
        }
        for (x = 0; x < p_app_settings->num_ext_attrs; x++)
        {
            attrs[xx+x] = p_app_settings->ext_attrs[x].attr_id;
        }
        HAL_CBACK (bt_rc_ctrl_callbacks, playerapplicationsetting_cb, &rc_addr,
                    p_app_settings->num_attrs, p_app_settings->attrs,
                    p_app_settings->num_ext_attrs, p_app_settings->ext_attrs);
        get_player_app_setting_cmd (xx + x, attrs);

        /* Free the application settings information after sending to
         * application.
         */
        for (xx = 0; xx < p_app_settings->ext_attr_index; xx++)
        {
            int x;
            btrc_player_app_ext_attr_t *p_ext_attr = &p_app_settings->ext_attrs[xx];

            for (x = 0; x < p_ext_attr->num_val; x++)
                osi_free_and_reset((void **)&p_ext_attr->ext_attr_val[x].p_str);
            p_ext_attr->num_val = 0;
            osi_free_and_reset((void **)&p_app_settings->ext_attrs[xx].p_str);
        }
        p_app_settings->num_attrs = 0;
    }
}

/***************************************************************************
**
** Function         handle_set_app_attr_val_response
**
** Description      handles the the set attributes value response, if fails
**                  calls HAL callback to indicate the failure
** Returns          None
**
***************************************************************************/
static void handle_set_app_attr_val_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_RSP *p_rsp)
{
    uint8_t accepted = 0;
    bt_bdaddr_t rc_addr;

    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);

    /* For timeout pmeta_msg will be NULL, else we need to
     * check if this is accepted by TG
     */
    if (pmeta_msg && (pmeta_msg->code == AVRC_RSP_ACCEPT))
    {
        accepted = 1;
    }
    HAL_CBACK(bt_rc_ctrl_callbacks, setplayerappsetting_rsp_cb, &rc_addr, accepted);
}

/***************************************************************************
**
** Function         handle_get_elem_attr_response
**
** Description      handles the the element attributes response, calls
**                  HAL callback to update track change information.
** Returns          None
**
***************************************************************************/
static void handle_get_elem_attr_response (tBTA_AV_META_MSG *pmeta_msg,
                                           tAVRC_GET_ELEM_ATTRS_RSP *p_rsp)
{
    if (p_rsp->status == AVRC_STS_NO_ERROR) {
        bt_bdaddr_t rc_addr;
        size_t buf_size = p_rsp->num_attr * sizeof(btrc_element_attr_val_t);
        btrc_element_attr_val_t *p_attr =
            (btrc_element_attr_val_t *)osi_calloc(buf_size);

        bdcpy(rc_addr.address, btif_rc_cb.rc_addr);

        for (int i = 0; i < p_rsp->num_attr; i++) {
            p_attr[i].attr_id = p_rsp->p_attrs[i].attr_id;
            /* Todo. Legth limit check to include null */
            if (p_rsp->p_attrs[i].name.str_len &&
                p_rsp->p_attrs[i].name.p_str) {
                memcpy(p_attr[i].text, p_rsp->p_attrs[i].name.p_str,
                       p_rsp->p_attrs[i].name.str_len);
                osi_free_and_reset((void **)&p_rsp->p_attrs[i].name.p_str);
            }
        }
        HAL_CBACK(bt_rc_ctrl_callbacks, track_changed_cb,
                  &rc_addr, p_rsp->num_attr, p_attr);
        osi_free(p_attr);
    } else if (p_rsp->status == BTIF_RC_STS_TIMEOUT) {
        /* Retry for timeout case, this covers error handling
         * for continuation failure also.
         */
        uint32_t attr_list[] = {
            AVRC_MEDIA_ATTR_ID_TITLE,
            AVRC_MEDIA_ATTR_ID_ARTIST,
            AVRC_MEDIA_ATTR_ID_ALBUM,
            AVRC_MEDIA_ATTR_ID_TRACK_NUM,
            AVRC_MEDIA_ATTR_ID_NUM_TRACKS,
            AVRC_MEDIA_ATTR_ID_GENRE,
            AVRC_MEDIA_ATTR_ID_PLAYING_TIME
            };
        get_element_attribute_cmd (AVRC_MAX_NUM_MEDIA_ATTR_ID, attr_list);
    } else {
        BTIF_TRACE_ERROR("%s: Error in get element attr procedure %d",
                         __func__, p_rsp->status);
    }
}

/***************************************************************************
**
** Function         handle_get_playstatus_response
**
** Description      handles the the play status response, calls
**                  HAL callback to update play position.
** Returns          None
**
***************************************************************************/
static void handle_get_playstatus_response (tBTA_AV_META_MSG *pmeta_msg, tAVRC_GET_PLAY_STATUS_RSP *p_rsp)
{
    bt_bdaddr_t rc_addr;

    bdcpy(rc_addr.address, btif_rc_cb.rc_addr);

    if (p_rsp->status == AVRC_STS_NO_ERROR)
    {
        HAL_CBACK(bt_rc_ctrl_callbacks, play_position_changed_cb,
            &rc_addr, p_rsp->song_len, p_rsp->song_pos);
    }
    else
    {
        BTIF_TRACE_ERROR("%s: Error in get play status procedure %d",
            __func__, p_rsp->status);
    }
}

/***************************************************************************
**
** Function         clear_cmd_timeout
**
** Description      helper function to stop the command timeout timer
** Returns          None
**
***************************************************************************/
static void clear_cmd_timeout (uint8_t label)
{
    rc_transaction_t *p_txn;

    p_txn = get_transaction_by_lbl (label);
    if (p_txn == NULL)
    {
        BTIF_TRACE_ERROR("%s: Error in transaction label lookup", __func__);
        return;
    }

    if (p_txn->txn_timer != NULL)
        alarm_cancel(p_txn->txn_timer);
}

/***************************************************************************
**
** Function         handle_avk_rc_metamsg_rsp
**
** Description      Handle RC metamessage response
**
** Returns          void
**
***************************************************************************/
static void handle_avk_rc_metamsg_rsp(tBTA_AV_META_MSG *pmeta_msg)
{
    tAVRC_RESPONSE    avrc_response = {0};
    uint8_t             scratch_buf[512] = {0};// this variable is unused
    uint16_t            buf_len;
    tAVRC_STS         status;

    BTIF_TRACE_DEBUG("%s opcode = %d rsp_code = %d  ", __func__,
                        pmeta_msg->p_msg->hdr.opcode, pmeta_msg->code);

    if ((AVRC_OP_VENDOR == pmeta_msg->p_msg->hdr.opcode)&&
                (pmeta_msg->code >= AVRC_RSP_NOT_IMPL)&&
                (pmeta_msg->code <= AVRC_RSP_INTERIM))
    {
        status = AVRC_Ctrl_ParsResponse(pmeta_msg->p_msg, &avrc_response, scratch_buf, &buf_len);
        BTIF_TRACE_DEBUG("%s parse status %d pdu = %d rsp_status = %d",
                         __func__, status, avrc_response.pdu,
                         pmeta_msg->p_msg->vendor.hdr.ctype);

        switch (avrc_response.pdu)
        {
            case AVRC_PDU_REGISTER_NOTIFICATION:
                handle_notification_response(pmeta_msg, &avrc_response.reg_notif);
                if (pmeta_msg->code == AVRC_RSP_INTERIM)
                {
                    /* Don't free the transaction Id */
                    clear_cmd_timeout (pmeta_msg->label);
                    return;
                }
                break;

            case AVRC_PDU_GET_CAPABILITIES:
                handle_get_capability_response(pmeta_msg, &avrc_response.get_caps);
                break;

            case AVRC_PDU_LIST_PLAYER_APP_ATTR:
                handle_app_attr_response(pmeta_msg, &avrc_response.list_app_attr);
                break;

            case AVRC_PDU_LIST_PLAYER_APP_VALUES:
                handle_app_val_response(pmeta_msg, &avrc_response.list_app_values);
                break;

            case AVRC_PDU_GET_CUR_PLAYER_APP_VALUE:
                handle_app_cur_val_response(pmeta_msg, &avrc_response.get_cur_app_val);
                break;

            case AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT:
                handle_app_attr_txt_response(pmeta_msg, &avrc_response.get_app_attr_txt);
                break;

            case AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT:
                handle_app_attr_val_txt_response(pmeta_msg, &avrc_response.get_app_val_txt);
                break;

            case AVRC_PDU_SET_PLAYER_APP_VALUE:
                handle_set_app_attr_val_response(pmeta_msg, &avrc_response.set_app_val);
                break;

            case AVRC_PDU_GET_ELEMENT_ATTR:
                handle_get_elem_attr_response(pmeta_msg, &avrc_response.get_elem_attrs);
                break;

            case AVRC_PDU_GET_PLAY_STATUS:
                handle_get_playstatus_response(pmeta_msg, &avrc_response.get_play_status);
                break;
        }
        release_transaction(pmeta_msg->label);
    }
    else
    {
        BTIF_TRACE_DEBUG("%s:Invalid Vendor Command  code: %d len: %d. Not processing it.",
            __func__, pmeta_msg->code, pmeta_msg->len);
        return;
    }
}

/***************************************************************************
**
** Function         handle_avk_rc_metamsg_cmd
**
** Description      Handle RC metamessage response
**
** Returns          void
**
***************************************************************************/
static void handle_avk_rc_metamsg_cmd(tBTA_AV_META_MSG *pmeta_msg)
{
    tAVRC_COMMAND    avrc_cmd = {0};
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    BTIF_TRACE_DEBUG("%s opcode = %d rsp_code = %d  ",__func__,
                     pmeta_msg->p_msg->hdr.opcode,pmeta_msg->code);
    if ((AVRC_OP_VENDOR==pmeta_msg->p_msg->hdr.opcode)&&
                (pmeta_msg->code <= AVRC_CMD_GEN_INQ))
    {
        status = AVRC_Ctrl_ParsCommand(pmeta_msg->p_msg, &avrc_cmd);
        BTIF_TRACE_DEBUG("%s Received vendor command.code %d, PDU %d label %d",
                         __func__, pmeta_msg->code, avrc_cmd.pdu, pmeta_msg->label);

        if (status != AVRC_STS_NO_ERROR)
        {
            /* return error */
            BTIF_TRACE_WARNING("%s: Error in parsing received metamsg command. status: 0x%02x",
                __func__, status);
            send_reject_response(pmeta_msg->rc_handle, pmeta_msg->label, avrc_cmd.pdu, status);
        }
        else
        {
            if (avrc_cmd.pdu == AVRC_PDU_REGISTER_NOTIFICATION)
            {
                uint8_t event_id = avrc_cmd.reg_notif.event_id;
                BTIF_TRACE_EVENT("%s:Register notification event_id: %s",
                        __func__, dump_rc_notification_event_id(event_id));
            }
            else if (avrc_cmd.pdu == AVRC_PDU_SET_ABSOLUTE_VOLUME)
            {
                BTIF_TRACE_EVENT("%s: Abs Volume Cmd Recvd", __func__);
            }
            btif_rc_ctrl_upstreams_rsp_cmd(avrc_cmd.pdu, &avrc_cmd, pmeta_msg->label);
        }
    }
    else
    {
      BTIF_TRACE_DEBUG("%s:Invalid Vendor Command  code: %d len: %d. Not processing it.",
                       __func__, pmeta_msg->code, pmeta_msg->len);
        return;
    }
}
#endif

/***************************************************************************
**
** Function         cleanup
**
** Description      Closes the AVRC interface
**
** Returns          void
**
***************************************************************************/
static void cleanup()
{
    BTIF_TRACE_EVENT("## %s ##", __func__);
    close_uinput();
    if (bt_rc_callbacks)
    {
        bt_rc_callbacks = NULL;
    }
    alarm_free(btif_rc_cb.rc_play_status_timer);
    memset(&btif_rc_cb, 0, sizeof(btif_rc_cb_t));
    lbl_destroy();
    BTIF_TRACE_EVENT("## %s ## completed", __func__);
}

/***************************************************************************
**
** Function         cleanup_ctrl
**
** Description      Closes the AVRC Controller interface
**
** Returns          void
**
***************************************************************************/
static void cleanup_ctrl()
{
    BTIF_TRACE_EVENT("## %s ##", __func__);

    if (bt_rc_ctrl_callbacks)
    {
        bt_rc_ctrl_callbacks = NULL;
    }
    alarm_free(btif_rc_cb.rc_play_status_timer);
    memset(&btif_rc_cb, 0, sizeof(btif_rc_cb_t));
    lbl_destroy();
    BTIF_TRACE_EVENT("## %s ## completed", __func__);
}

/***************************************************************************
**
** Function         getcapabilities_cmd
**
** Description      GetCapabilties from Remote(Company_ID, Events_Supported)
**
** Returns          void
**
***************************************************************************/
static bt_status_t getcapabilities_cmd (uint8_t cap_id)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction = NULL;
#if (AVRC_CTRL_INCLUDED == TRUE)
    BTIF_TRACE_DEBUG("%s: cap_id %d", __func__, cap_id);
    CHECK_RC_CONNECTED
    bt_status_t tran_status=get_transaction(&p_transaction);
    if (BT_STATUS_SUCCESS != tran_status)
        return BT_STATUS_FAIL;

     tAVRC_COMMAND avrc_cmd = {0};
     BT_HDR *p_msg = NULL;
     avrc_cmd.get_caps.opcode = AVRC_OP_VENDOR;
     avrc_cmd.get_caps.capability_id = cap_id;
     avrc_cmd.get_caps.pdu = AVRC_PDU_GET_CAPABILITIES;
     avrc_cmd.get_caps.status = AVRC_STS_NO_ERROR;
     status = AVRC_BldCommand(&avrc_cmd, &p_msg);
     if ((status == AVRC_STS_NO_ERROR)&&(p_msg != NULL))
     {
         uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
         BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                            __func__,p_transaction->lbl);
         BTA_AvVendorCmd(btif_rc_cb.rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                                                          data_start, p_msg->len);
         status =  BT_STATUS_SUCCESS;
         start_status_command_timer (AVRC_PDU_GET_CAPABILITIES, p_transaction);
     }
     else
     {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                             __func__, status);
     }
     osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         list_player_app_setting_attrib_cmd
**
** Description      Get supported List Player Attributes
**
** Returns          void
**
***************************************************************************/
static bt_status_t list_player_app_setting_attrib_cmd(void)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction = NULL;
#if (AVRC_CTRL_INCLUDED == TRUE)
    BTIF_TRACE_DEBUG("%s: ", __func__);
    CHECK_RC_CONNECTED
    bt_status_t tran_status=get_transaction(&p_transaction);
    if (BT_STATUS_SUCCESS != tran_status)
        return BT_STATUS_FAIL;

     tAVRC_COMMAND avrc_cmd = {0};
     BT_HDR *p_msg = NULL;
     avrc_cmd.list_app_attr.opcode = AVRC_OP_VENDOR;
     avrc_cmd.list_app_attr.pdu = AVRC_PDU_LIST_PLAYER_APP_ATTR;
     avrc_cmd.list_app_attr.status = AVRC_STS_NO_ERROR;
     status = AVRC_BldCommand(&avrc_cmd, &p_msg);
     if ((status == AVRC_STS_NO_ERROR)&&(p_msg != NULL))
     {
         uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
         BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                            __func__,p_transaction->lbl);
         BTA_AvVendorCmd(btif_rc_cb.rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                                                          data_start, p_msg->len);
         status =  BT_STATUS_SUCCESS;
         start_status_command_timer (AVRC_PDU_LIST_PLAYER_APP_ATTR, p_transaction);
     }
     else
     {

         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __func__, status);
     }
     osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         list_player_app_setting_value_cmd
**
** Description      Get values of supported Player Attributes
**
** Returns          void
**
***************************************************************************/
static bt_status_t list_player_app_setting_value_cmd(uint8_t attrib_id)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTRL_INCLUDED == TRUE)
    BTIF_TRACE_DEBUG("%s: attrib_id %d", __func__, attrib_id);
    CHECK_RC_CONNECTED
    bt_status_t tran_status=get_transaction(&p_transaction);
    if (BT_STATUS_SUCCESS != tran_status)
        return BT_STATUS_FAIL;

     tAVRC_COMMAND avrc_cmd = {0};
     BT_HDR *p_msg = NULL;
     avrc_cmd.list_app_values.attr_id = attrib_id;
     avrc_cmd.list_app_values.opcode = AVRC_OP_VENDOR;
     avrc_cmd.list_app_values.pdu = AVRC_PDU_LIST_PLAYER_APP_VALUES;
     avrc_cmd.list_app_values.status = AVRC_STS_NO_ERROR;
     status = AVRC_BldCommand(&avrc_cmd, &p_msg);
     if ((status == AVRC_STS_NO_ERROR) && (p_msg != NULL))
     {
         uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
         BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                            __func__,p_transaction->lbl);
         BTA_AvVendorCmd(btif_rc_cb.rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                               data_start, p_msg->len);
         status =  BT_STATUS_SUCCESS;
         start_status_command_timer (AVRC_PDU_LIST_PLAYER_APP_VALUES, p_transaction);
     }
     else
     {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x", __func__, status);
     }
     osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         get_player_app_setting_cmd
**
** Description      Get current values of Player Attributes
**
** Returns          void
**
***************************************************************************/
static bt_status_t get_player_app_setting_cmd(uint8_t num_attrib, uint8_t* attrib_ids)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction = NULL;
    int count  = 0;
#if (AVRC_CTRL_INCLUDED == TRUE)
    BTIF_TRACE_DEBUG("%s: num attrib_id %d", __func__, num_attrib);
    CHECK_RC_CONNECTED
    bt_status_t tran_status=get_transaction(&p_transaction);
    if (BT_STATUS_SUCCESS != tran_status)
        return BT_STATUS_FAIL;

     tAVRC_COMMAND avrc_cmd = {0};
     BT_HDR *p_msg = NULL;
     avrc_cmd.get_cur_app_val.opcode = AVRC_OP_VENDOR;
     avrc_cmd.get_cur_app_val.status = AVRC_STS_NO_ERROR;
     avrc_cmd.get_cur_app_val.num_attr = num_attrib;
     avrc_cmd.get_cur_app_val.pdu = AVRC_PDU_GET_CUR_PLAYER_APP_VALUE;

     for (count = 0; count < num_attrib; count++)
     {
         avrc_cmd.get_cur_app_val.attrs[count] = attrib_ids[count];
     }
     status = AVRC_BldCommand(&avrc_cmd, &p_msg);
     if ((status == AVRC_STS_NO_ERROR) && (p_msg != NULL))
     {
         uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
         BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                            __func__,p_transaction->lbl);
         BTA_AvVendorCmd(btif_rc_cb.rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                          data_start, p_msg->len);
         status =  BT_STATUS_SUCCESS;
         start_status_command_timer (AVRC_PDU_GET_CUR_PLAYER_APP_VALUE, p_transaction);
     }
     else
     {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __func__, status);
     }
     osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         change_player_app_setting
**
** Description      Set current values of Player Attributes
**
** Returns          void
**
***************************************************************************/
static bt_status_t change_player_app_setting(bt_bdaddr_t *bd_addr, uint8_t num_attrib, uint8_t* attrib_ids, uint8_t* attrib_vals)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction = NULL;
    int count  = 0;
#if (AVRC_CTRL_INCLUDED == TRUE)
    BTIF_TRACE_DEBUG("%s: num attrib_id %d", __func__, num_attrib);
    CHECK_RC_CONNECTED
    bt_status_t tran_status=get_transaction(&p_transaction);
    if (BT_STATUS_SUCCESS != tran_status)
        return BT_STATUS_FAIL;

     tAVRC_COMMAND avrc_cmd = {0};
     BT_HDR *p_msg = NULL;
     avrc_cmd.set_app_val.opcode = AVRC_OP_VENDOR;
     avrc_cmd.set_app_val.status = AVRC_STS_NO_ERROR;
     avrc_cmd.set_app_val.num_val = num_attrib;
     avrc_cmd.set_app_val.pdu = AVRC_PDU_SET_PLAYER_APP_VALUE;
     avrc_cmd.set_app_val.p_vals =
           (tAVRC_APP_SETTING *)osi_malloc(sizeof(tAVRC_APP_SETTING) * num_attrib);
     for (count = 0; count < num_attrib; count++)
     {
         avrc_cmd.set_app_val.p_vals[count].attr_id = attrib_ids[count];
         avrc_cmd.set_app_val.p_vals[count].attr_val = attrib_vals[count];
     }
     status = AVRC_BldCommand(&avrc_cmd, &p_msg);
     if ((status == AVRC_STS_NO_ERROR) && (p_msg != NULL))
     {
         uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
         BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                            __func__,p_transaction->lbl);
         BTA_AvVendorCmd(btif_rc_cb.rc_handle,p_transaction->lbl,AVRC_CMD_CTRL,
                              data_start, p_msg->len);
         status =  BT_STATUS_SUCCESS;
         start_control_command_timer (AVRC_PDU_SET_PLAYER_APP_VALUE, p_transaction);
     }
     else
     {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __func__, status);
     }
     osi_free(p_msg);
     osi_free_and_reset((void **)&avrc_cmd.set_app_val.p_vals);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         get_player_app_setting_attr_text_cmd
**
** Description      Get text description for app attribute
**
** Returns          void
**
***************************************************************************/
static bt_status_t get_player_app_setting_attr_text_cmd (uint8_t *attrs, uint8_t num_attrs)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction = NULL;
    int count  = 0;
#if (AVRC_CTRL_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;
    CHECK_RC_CONNECTED

    BTIF_TRACE_DEBUG("%s: num attrs %d", __func__, num_attrs);

    tran_status = get_transaction(&p_transaction);
    if (BT_STATUS_SUCCESS != tran_status)
        return BT_STATUS_FAIL;

    avrc_cmd.pdu = AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT;
    avrc_cmd.get_app_attr_txt.opcode = AVRC_OP_VENDOR;
    avrc_cmd.get_app_attr_txt.num_attr = num_attrs;

    for (count = 0; count < num_attrs; count++)
    {
        avrc_cmd.get_app_attr_txt.attrs[count] = attrs[count];
    }
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
                BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __func__, p_transaction->lbl);
        BTA_AvVendorCmd(btif_rc_cb.rc_handle, p_transaction->lbl,
                AVRC_CMD_STATUS, data_start, p_msg->len);
        osi_free(p_msg);
        status =  BT_STATUS_SUCCESS;
        start_status_command_timer (AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT, p_transaction);
    }
    else
    {
        BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x", __func__, status);
    }
    osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         get_player_app_setting_val_text_cmd
**
** Description      Get text description for app attribute values
**
** Returns          void
**
***************************************************************************/
static bt_status_t get_player_app_setting_value_text_cmd (uint8_t *vals, uint8_t num_vals)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction = NULL;
    int count  = 0;
#if (AVRC_CTRL_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;
    CHECK_RC_CONNECTED

    BTIF_TRACE_DEBUG("%s: num_vals %d", __func__, num_vals);

    tran_status = get_transaction(&p_transaction);
    if (BT_STATUS_SUCCESS != tran_status)
        return BT_STATUS_FAIL;

    avrc_cmd.pdu = AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT;
    avrc_cmd.get_app_val_txt.opcode = AVRC_OP_VENDOR;
    avrc_cmd.get_app_val_txt.num_val = num_vals;

    for (count = 0; count < num_vals; count++)
    {
        avrc_cmd.get_app_val_txt.vals[count] = vals[count];
    }
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                         __func__, p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvVendorCmd(btif_rc_cb.rc_handle, p_transaction->lbl,
                    AVRC_CMD_STATUS, data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
            start_status_command_timer (AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT, p_transaction);
        }
    }
    else
    {
        BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                __func__, status);
    }
    osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         register_notification_cmd
**
** Description      Send Command to register for a Notification ID
**
** Returns          void
**
***************************************************************************/
static bt_status_t register_notification_cmd (uint8_t label, uint8_t event_id, uint32_t event_value)
{

    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
#if (AVRC_CTRL_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    CHECK_RC_CONNECTED


    BTIF_TRACE_DEBUG("%s: event_id %d  event_value", __func__, event_id, event_value);

    avrc_cmd.reg_notif.opcode = AVRC_OP_VENDOR;
    avrc_cmd.reg_notif.status = AVRC_STS_NO_ERROR;
    avrc_cmd.reg_notif.event_id = event_id;
    avrc_cmd.reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
    avrc_cmd.reg_notif.param = event_value;
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __func__, label);
        if (p_msg != NULL)
        {
            BTA_AvVendorCmd(btif_rc_cb.rc_handle, label, AVRC_CMD_NOTIF,
                    data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __func__, status);
    }
    osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         get_element_attribute_cmd
**
** Description      Get Element Attribute for  attributeIds
**
** Returns          void
**
***************************************************************************/
static bt_status_t get_element_attribute_cmd (uint8_t num_attribute, uint32_t *p_attr_ids)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
    int count  = 0;
#if (AVRC_CTRL_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;
    CHECK_RC_CONNECTED

    BTIF_TRACE_DEBUG("%s: num_attribute  %d attribute_id %d",
                   __func__, num_attribute, p_attr_ids[0]);

    tran_status = get_transaction(&p_transaction);
    if (BT_STATUS_SUCCESS != tran_status)
        return BT_STATUS_FAIL;

    avrc_cmd.get_elem_attrs.opcode = AVRC_OP_VENDOR;
    avrc_cmd.get_elem_attrs.status = AVRC_STS_NO_ERROR;
    avrc_cmd.get_elem_attrs.num_attr = num_attribute;
    avrc_cmd.get_elem_attrs.pdu = AVRC_PDU_GET_ELEMENT_ATTR;
    for (count = 0; count < num_attribute; count++)
    {
        avrc_cmd.get_elem_attrs.attrs[count] = p_attr_ids[count];
    }

    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __func__, p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvVendorCmd(btif_rc_cb.rc_handle, p_transaction->lbl,
                    AVRC_CMD_STATUS, data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
            start_status_command_timer (AVRC_PDU_GET_ELEMENT_ATTR,
                    p_transaction);
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __func__, status);
    }
    osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         get_play_status_cmd
**
** Description      Get Element Attribute for  attributeIds
**
** Returns          void
**
***************************************************************************/
static bt_status_t get_play_status_cmd(void)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction = NULL;
#if (AVRC_CTRL_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;
    CHECK_RC_CONNECTED

    BTIF_TRACE_DEBUG("%s: ", __func__);
    tran_status = get_transaction(&p_transaction);
    if (BT_STATUS_SUCCESS != tran_status)
        return BT_STATUS_FAIL;

    avrc_cmd.get_play_status.opcode = AVRC_OP_VENDOR;
    avrc_cmd.get_play_status.pdu = AVRC_PDU_GET_PLAY_STATUS;
    avrc_cmd.get_play_status.status = AVRC_STS_NO_ERROR;
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __func__, p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvVendorCmd(btif_rc_cb.rc_handle,p_transaction->lbl,
                    AVRC_CMD_STATUS, data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
            start_status_command_timer (AVRC_PDU_GET_PLAY_STATUS, p_transaction);
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __func__, status);
    }
    osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;

}

/***************************************************************************
**
** Function         set_volume_rsp
**
** Description      Rsp for SetAbsoluteVolume Command
**
** Returns          void
**
***************************************************************************/
static bt_status_t set_volume_rsp(bt_bdaddr_t *bd_addr, uint8_t abs_vol, uint8_t label)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
#if (AVRC_CTRL_INCLUDED == TRUE)
    tAVRC_RESPONSE avrc_rsp;
    BT_HDR *p_msg = NULL;
    CHECK_RC_CONNECTED

    BTIF_TRACE_DEBUG("%s: abs_vol %d", __func__, abs_vol);

    avrc_rsp.volume.opcode = AVRC_OP_VENDOR;
    avrc_rsp.volume.pdu = AVRC_PDU_SET_ABSOLUTE_VOLUME;
    avrc_rsp.volume.status = AVRC_STS_NO_ERROR;
    avrc_rsp.volume.volume = abs_vol;
    status = AVRC_BldResponse(btif_rc_cb.rc_handle, &avrc_rsp, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __func__, btif_rc_cb.rc_vol_label);
        if (p_msg != NULL)
        {
            BTA_AvVendorRsp(btif_rc_cb.rc_handle, label,
                    BTA_AV_RSP_ACCEPT, data_start, p_msg->len, 0);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __func__, status);
    }
    osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         send_register_abs_vol_rsp
**
** Description      Rsp for Notification of Absolute Volume
**
** Returns          void
**
***************************************************************************/
static bt_status_t volume_change_notification_rsp(bt_bdaddr_t *bd_addr, btrc_notification_type_t rsp_type,
            uint8_t abs_vol, uint8_t label)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    tAVRC_RESPONSE avrc_rsp;
    BT_HDR *p_msg = NULL;
#if (AVRC_CTRL_INCLUDED == TRUE)
    BTIF_TRACE_DEBUG("%s: rsp_type  %d abs_vol %d", __func__, rsp_type, abs_vol);
    CHECK_RC_CONNECTED

    avrc_rsp.reg_notif.opcode = AVRC_OP_VENDOR;
    avrc_rsp.reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
    avrc_rsp.reg_notif.status = AVRC_STS_NO_ERROR;
    avrc_rsp.reg_notif.param.volume = abs_vol;
    avrc_rsp.reg_notif.event_id = AVRC_EVT_VOLUME_CHANGE;

    status = AVRC_BldResponse(btif_rc_cb.rc_handle, &avrc_rsp, &p_msg);
    if (status == AVRC_STS_NO_ERROR) {
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                         __func__, label);
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTA_AvVendorRsp(btif_rc_cb.rc_handle, label,
                        (rsp_type == BTRC_NOTIFICATION_TYPE_INTERIM) ?
                            AVRC_RSP_INTERIM : AVRC_RSP_CHANGED,
                        data_start, p_msg->len, 0);
        status = BT_STATUS_SUCCESS;
    } else {
        BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                         __func__, status);
    }
    osi_free(p_msg);

#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         send_groupnavigation_cmd
**
** Description      Send Pass-Through command
**
** Returns          void
**
***************************************************************************/
static bt_status_t send_groupnavigation_cmd(bt_bdaddr_t *bd_addr, uint8_t key_code,
                                            uint8_t key_state)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
#if (AVRC_CTRL_INCLUDED == TRUE)
    rc_transaction_t *p_transaction=NULL;
    BTIF_TRACE_DEBUG("%s: key-code: %d, key-state: %d", __func__,
                                                    key_code, key_state);
    CHECK_RC_CONNECTED
    if (btif_rc_cb.rc_features & BTA_AV_FEAT_RCTG)
    {
        bt_status_t tran_status = get_transaction(&p_transaction);
        if ((BT_STATUS_SUCCESS == tran_status) && (NULL != p_transaction)) {
             uint8_t buffer[AVRC_PASS_THRU_GROUP_LEN] = {0};
             uint8_t* start = buffer;
             UINT24_TO_BE_STREAM(start, AVRC_CO_METADATA);
             *(start)++ = 0;
             UINT8_TO_BE_STREAM(start, key_code);
             BTA_AvRemoteVendorUniqueCmd(btif_rc_cb.rc_handle,
                                         p_transaction->lbl,
                                         (tBTA_AV_STATE)key_state, buffer,
                                         AVRC_PASS_THRU_GROUP_LEN);
             status =  BT_STATUS_SUCCESS;
             BTIF_TRACE_DEBUG("%s: succesfully sent group_navigation command to BTA",
                              __func__);
        }
        else
        {
            status =  BT_STATUS_FAIL;
            BTIF_TRACE_DEBUG("%s: error in fetching transaction", __func__);
        }
    }
    else
    {
        status =  BT_STATUS_FAIL;
        BTIF_TRACE_DEBUG("%s: feature not supported", __func__);
    }
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         send_passthrough_cmd
**
** Description      Send Pass-Through command
**
** Returns          void
**
***************************************************************************/
static bt_status_t send_passthrough_cmd(bt_bdaddr_t *bd_addr, uint8_t key_code, uint8_t key_state)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
#if (AVRC_CTRL_INCLUDED == TRUE)
    CHECK_RC_CONNECTED
    rc_transaction_t *p_transaction=NULL;
    BTIF_TRACE_DEBUG("%s: key-code: %d, key-state: %d", __func__,
                                                    key_code, key_state);
    if (btif_rc_cb.rc_features & BTA_AV_FEAT_RCTG)
    {
        bt_status_t tran_status = get_transaction(&p_transaction);
        if (BT_STATUS_SUCCESS == tran_status && NULL != p_transaction)
        {
            BTA_AvRemoteCmd(btif_rc_cb.rc_handle, p_transaction->lbl,
                (tBTA_AV_RC)key_code, (tBTA_AV_STATE)key_state);
            status =  BT_STATUS_SUCCESS;
            BTIF_TRACE_DEBUG("%s: succesfully sent passthrough command to BTA", __func__);
        }
        else
        {
            status =  BT_STATUS_FAIL;
            BTIF_TRACE_DEBUG("%s: error in fetching transaction", __func__);
        }
    }
    else
    {
        status =  BT_STATUS_FAIL;
        BTIF_TRACE_DEBUG("%s: feature not supported", __func__);
    }
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __func__);
#endif
    return (bt_status_t)status;
}

static const btrc_interface_t bt_rc_interface = {
    sizeof(bt_rc_interface),
    init,
    get_play_status_rsp,
    NULL, /* list_player_app_attr_rsp */
    NULL, /* list_player_app_value_rsp */
    NULL, /* get_player_app_value_rsp */
    NULL, /* get_player_app_attr_text_rsp */
    NULL, /* get_player_app_value_text_rsp */
    get_element_attr_rsp,
    NULL, /* set_player_app_value_rsp */
    register_notification_rsp,
    set_volume,
    cleanup,
};

static const btrc_ctrl_interface_t bt_rc_ctrl_interface = {
    sizeof(bt_rc_ctrl_interface),
    init_ctrl,
    send_passthrough_cmd,
    send_groupnavigation_cmd,
    change_player_app_setting,
    set_volume_rsp,
    volume_change_notification_rsp,
    cleanup_ctrl,
};

/*******************************************************************************
**
** Function         btif_rc_get_interface
**
** Description      Get the AVRCP Target callback interface
**
** Returns          btrc_interface_t
**
*******************************************************************************/
const btrc_interface_t *btif_rc_get_interface(void)
{
    BTIF_TRACE_EVENT("%s", __func__);
    return &bt_rc_interface;
}

/*******************************************************************************
**
** Function         btif_rc_ctrl_get_interface
**
** Description      Get the AVRCP Controller callback interface
**
** Returns          btrc_ctrl_interface_t
**
*******************************************************************************/
const btrc_ctrl_interface_t *btif_rc_ctrl_get_interface(void)
{
    BTIF_TRACE_EVENT("%s", __func__);
    return &bt_rc_ctrl_interface;
}

/*******************************************************************************
**      Function         initialize_transaction
**
**      Description    Initializes fields of the transaction structure
**
**      Returns          void
*******************************************************************************/
static void initialize_transaction(int lbl)
{
    pthread_mutex_lock(&device.lbllock);
    if (lbl < MAX_TRANSACTIONS_PER_SESSION) {
        if (alarm_is_scheduled(device.transaction[lbl].txn_timer)) {
            clear_cmd_timeout(lbl);
        }
        device.transaction[lbl].lbl = lbl;
        device.transaction[lbl].in_use=false;
        device.transaction[lbl].handle=0;
    }
    pthread_mutex_unlock(&device.lbllock);
}

/*******************************************************************************
**      Function         lbl_init
**
**      Description    Initializes label structures and mutexes.
**
**      Returns         void
*******************************************************************************/
void lbl_init()
{
    memset(&device,0,sizeof(rc_device_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&(device.lbllock), &attr);
    pthread_mutexattr_destroy(&attr);
    init_all_transactions();
}

/*******************************************************************************
**
** Function         init_all_transactions
**
** Description    Initializes all transactions
**
** Returns          void
*******************************************************************************/
void init_all_transactions()
{
    uint8_t txn_indx=0;
    for(txn_indx=0; txn_indx < MAX_TRANSACTIONS_PER_SESSION; txn_indx++)
    {
        initialize_transaction(txn_indx);
    }
}

/*******************************************************************************
**
** Function         get_transaction_by_lbl
**
** Description    Will return a transaction based on the label. If not inuse
**                     will return an error.
**
** Returns          bt_status_t
*******************************************************************************/
rc_transaction_t *get_transaction_by_lbl(uint8_t lbl)
{
    rc_transaction_t *transaction = NULL;
    pthread_mutex_lock(&device.lbllock);

    /* Determine if this is a valid label */
    if (lbl < MAX_TRANSACTIONS_PER_SESSION)
    {
        if (false==device.transaction[lbl].in_use)
        {
            transaction = NULL;
        }
        else
        {
            transaction = &(device.transaction[lbl]);
            BTIF_TRACE_DEBUG("%s: Got transaction.label: %d",__func__,lbl);
        }
    }

    pthread_mutex_unlock(&device.lbllock);
    return transaction;
}

/*******************************************************************************
**
** Function         get_transaction
**
** Description    Obtains the transaction details.
**
** Returns          bt_status_t
*******************************************************************************/

bt_status_t  get_transaction(rc_transaction_t **ptransaction)
{
    bt_status_t result = BT_STATUS_NOMEM;
    uint8_t i=0;
    pthread_mutex_lock(&device.lbllock);

    // Check for unused transactions
    for (i=0; i<MAX_TRANSACTIONS_PER_SESSION; i++)
    {
        if (false==device.transaction[i].in_use)
        {
            BTIF_TRACE_DEBUG("%s:Got transaction.label: %d",__func__,device.transaction[i].lbl);
            device.transaction[i].in_use = true;
            *ptransaction = &(device.transaction[i]);
            result = BT_STATUS_SUCCESS;
            break;
        }
    }

    pthread_mutex_unlock(&device.lbllock);
    return result;
}

/*******************************************************************************
**
** Function         release_transaction
**
** Description    Will release a transaction for reuse
**
** Returns          bt_status_t
*******************************************************************************/
void release_transaction(uint8_t lbl)
{
    rc_transaction_t *transaction = get_transaction_by_lbl(lbl);

    /* If the transaction is in use... */
    if (transaction != NULL)
    {
        BTIF_TRACE_DEBUG("%s: lbl: %d", __func__, lbl);
        initialize_transaction(lbl);
    }
}

/*******************************************************************************
**
** Function         lbl_destroy
**
** Description    Cleanup of the mutex
**
** Returns          void
*******************************************************************************/
void lbl_destroy()
{
    pthread_mutex_destroy(&(device.lbllock));
}

/*******************************************************************************
**      Function       sleep_ms
**
**      Description    Sleep the calling thread unconditionally for
**                     |timeout_ms| milliseconds.
**
**      Returns        void
*******************************************************************************/
static void sleep_ms(period_ms_t timeout_ms) {
    struct timespec delay;
    delay.tv_sec = timeout_ms / 1000;
    delay.tv_nsec = 1000 * 1000 * (timeout_ms % 1000);

    OSI_NO_INTR(nanosleep(&delay, &delay));
}

static bool absolute_volume_disabled() {
    char volume_disabled[PROPERTY_VALUE_MAX] = {0};
    osi_property_get("persist.bluetooth.disableabsvol", volume_disabled, "false");
    if (strncmp(volume_disabled, "true", 4) == 0) {
        BTIF_TRACE_WARNING("%s: Absolute volume disabled by property", __func__);
        return true;
    }
    return false;
}

static char const* key_id_to_str(uint16_t id) {
    for (int i = 0; key_map[i].name != NULL; i++) {
        if (id == key_map[i].mapped_id)
            return key_map[i].name;
    }
    return "UNKNOWN KEY";
}
