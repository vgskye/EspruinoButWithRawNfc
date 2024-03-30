
/**
 * This file is part of Espruino, a JavaScript interpreter for Microcontrollers
 *
 * Copyright (C) 2013 Gordon Williams <gw@pur3.co.uk>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * ----------------------------------------------------------------------------
 * Platform Specific Bluetooth Functionality
 * ----------------------------------------------------------------------------
 */

#ifdef BLUETOOTH

#include "jswrap_bluetooth.h"
#ifdef USE_TERMINAL
#include "jswrap_terminal.h"
#endif
#include "jsinteractive.h"
#include "jsdevices.h"
#include "jshardware.h"
#include "jstimer.h"
#include "nrf5x_utils.h"
#include "bluetooth.h"
#include "bluetooth_utils.h"

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "nordic_common.h"
#include "nrf.h"

#if NRF_SD_BLE_API_VERSION<5
#include "softdevice_handler.h"
#else
#include "nrf_sdm.h" // for softdevice_disable
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "nrf_drv_clock.h" // for nrf_drv_clock_lfclk_request workaround
#endif

#include "nrf_log.h"
#include "ble_hci.h"
#include "ble_advdata.h"
#include "ble_conn_params.h"
#include "app_timer.h"
#include "ble_nus.h"
#include "app_util_platform.h"
#include "nrf_delay.h"
#ifdef USE_NFC
#include "hal_t2t/hal_nfc_t2t.h"
#endif
#if BLE_HIDS_ENABLED
#include "ble_hids.h"
#endif
#if ESPR_BLUETOOTH_ANCS
#include "bluetooth_ancs.h"
#endif


#if PEER_MANAGER_ENABLED
#include "peer_manager.h"
#include "fds.h"
#include "id_manager.h"
#if NRF_SD_BLE_API_VERSION<5
#include "fstorage.h"
#include "fstorage_internal_defs.h"
#endif
#include "ble_conn_state.h"
static pm_peer_id_t   m_peer_id;                              /**< Device reference handle to the current bonded central. */
static pm_peer_id_t   m_whitelist_peers[BLE_GAP_WHITELIST_ADDR_MAX_COUNT];  /**< List of peers currently in the whitelist. */
static uint32_t       m_whitelist_peer_cnt;                                 /**< Number of peers currently in the whitelist. */
static bool           m_is_wl_changed;                                      /**< Indicates if the whitelist has been changed since last time it has been updated in the Peer Manager. */
static volatile ble_gap_sec_params_t  m_sec_params;                         /**< Current security parameters. */
// needed for peer_manager_init so we can smoothly upgrade from pre 1v92 firmwares
#include "fds_internal_defs.h"
// If we have peer manager we have central mode and NRF52
// So just enable link security
#define LINK_SECURITY
#endif

#ifdef LINK_SECURITY
// Code to handle secure Bluetooth links
#include "ecc.h"

#define BLE_GAP_LESC_P256_SK_LEN 32
/**@brief GAP LE Secure Connections P-256 Private Key. */
typedef struct
{
  uint8_t   sk[BLE_GAP_LESC_P256_SK_LEN];        /**< LE Secure Connections Elliptic Curve Diffie-Hellman P-256 Private Key in little-endian. */
} ble_gap_lesc_p256_sk_t;

__ALIGN(4) static ble_gap_lesc_p256_sk_t m_lesc_sk;    /**< LESC ECC Private Key */
__ALIGN(4) static ble_gap_lesc_p256_pk_t m_lesc_pk;    /**< LESC ECC Public Key */
__ALIGN(4) static ble_gap_lesc_dhkey_t m_lesc_dhkey;   /**< LESC ECC DH Key*/
#endif

// -----------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------

#if NRF_SD_BLE_API_VERSION < 5
#ifndef NRF_BLE_MAX_MTU_SIZE
#define NRF_BLE_MAX_MTU_SIZE            GATT_MTU_SIZE_DEFAULT                        /**< MTU size used in the softdevice enabling and to reply to a BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST event. */
#endif
#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(100, APP_TIMER_PRESCALER)   /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(10000, APP_TIMER_PRESCALER) /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                           /**< Number of attempts before giving up the connection parameter negotiation. */
#else
#ifndef GATT_MTU_SIZE_DEFAULT
#define GATT_MTU_SIZE_DEFAULT  BLE_GATT_ATT_MTU_DEFAULT
#endif
#define NRF_BLE_MAX_MTU_SIZE            NRF_SDH_BLE_GATT_MAX_MTU_SIZE               /**< MTU size used in the softdevice enabling and to reply to a BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST event. */
#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(100)  /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(10000) /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                           /**< Number of attempts before giving up the connection parameter negotiation. */
#endif
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS) /**< Connection Supervision Timeout in 10 ms units, see @ref BLE_GAP_CP_LIMITS.*/
// Slave latency - the number of missed responses to BLE requests we're happy to put up with - see BLE_GAP_CP_LIMITS
#define SLAVE_LATENCY                   0        // latency for *us* - we want to respond on every event
#define SLAVE_LATENCY_CENTRAL           2        // when connecting to something else, be willing to put up with some lack of response

#if NRF_BLE_MAX_MTU_SIZE != GATT_MTU_SIZE_DEFAULT
#define EXTENSIBLE_MTU // The MTU can be extended past the default of 23
#endif

#define APP_BLE_OBSERVER_PRIO               2                                       /**< Application's BLE observer priority. You shouldn't need to modify this value. */
#define APP_SOC_OBSERVER_PRIO               1                                       /**< Applications' SoC observer priority. You shoulnd't need to modify this value. */

/* We want to listen as much of the time as possible. Not sure if 100/100 is feasible (50/100 is what's used in examples), but it seems to work fine like this. */
#define SCAN_INTERVAL                   MSEC_TO_UNITS(100, UNIT_0_625_MS)            /**< Scan interval in units of 0.625 millisecond - 100 msec */
#define SCAN_WINDOW                     MSEC_TO_UNITS(100, UNIT_0_625_MS)            /**< Scan window in units of 0.625 millisecond - 100 msec */

#define APP_ADV_TIMEOUT_IN_SECONDS      180                                         /**< The advertising timeout (in units of seconds). */

// BLE HID stuff
#define BASE_USB_HID_SPEC_VERSION        0x0101                                      /**< Version number of base USB HID Specification implemented by this application. */
#define HID_OUTPUT_REPORT_INDEX              0                                           /**< Index of Output Report. */
#define HID_OUTPUT_REPORT_MAX_LEN            1                                           /**< Maximum length of Output Report. */
#define HID_INPUT_REPORT_KEYS_INDEX          0                                           /**< Index of Input Report. */
#define HID_INPUT_REP_REF_ID                 0                                           /**< Id of reference to Keyboard Input Report. */
#define HID_OUTPUT_REP_REF_ID                0                                           /**< Id of reference to Keyboard Output Report. */
#define HID_INPUT_REPORT_KEYS_MAX_LEN        8

#define APP_FEATURE_NOT_SUPPORTED       BLE_GATT_STATUS_ATTERR_APP_BEGIN + 2        /**< Reply when unsupported features are requested. */


// -----------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------

#define ADVERTISE_MAX_UUIDS             4 ///< maximum custom UUIDs to advertise

#if NRF_SD_BLE_API_VERSION < 5
static ble_nus_t                        m_nus;                                      /**< Structure to identify the Nordic UART Service. */
#elif NRF_SD_BLE_API_VERSION < 6
BLE_NUS_DEF(m_nus);                                                                 /**< Structure to identify the Nordic UART Service. */
#else
BLE_NUS_DEF(m_nus, NRF_SDH_BLE_TOTAL_LINK_COUNT);                                    /**< Structure to identify the Nordic UART Service. */
#endif

#if BLE_HIDS_ENABLED
#if NRF_SD_BLE_API_VERSION < 5
static ble_hids_t                       m_hids;                                   /**< Structure used to identify the HID service. */
#elif NRF_SD_BLE_API_VERSION < 6
BLE_HIDS_DEF(m_hids);
#else
BLE_HIDS_DEF(m_hids,
             NRF_SDH_BLE_TOTAL_LINK_COUNT,
             HID_INPUT_REPORT_KEYS_MAX_LEN,
             HID_OUTPUT_REPORT_MAX_LEN);
#endif
static bool                             m_in_boot_mode = false;
#endif


#if NRF_SD_BLE_API_VERSION > 5
uint8_t m_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;                   /**< Advertising handle used to identify an advertising set. */
int8_t m_tx_power = 0;
#endif

volatile uint16_t                       m_peripheral_conn_handle;    /**< Handle of the current connection. */
#ifndef SAVE_ON_FLASH
ble_gap_addr_t m_peripheral_addr;
#endif
#ifdef EXTENSIBLE_MTU
volatile uint16_t m_peripheral_effective_mtu;
#else
const uint16_t m_peripheral_effective_mtu = GATT_MTU_SIZE_DEFAULT;
#endif
#if CENTRAL_LINK_COUNT>0
volatile uint16_t                       m_central_conn_handles[CENTRAL_LINK_COUNT]; /**< Handle for central mode connection */
#ifdef EXTENSIBLE_MTU
volatile uint16_t m_central_effective_mtu;
#else
const uint16_t m_central_effective_mtu = GATT_MTU_SIZE_DEFAULT;
#endif
#endif

#ifdef USE_NFC
volatile bool nfcEnabled = false;
#endif

uint16_t bleAdvertisingInterval = MSEC_TO_UNITS(BLUETOOTH_ADVERTISING_INTERVAL, UNIT_0_625_MS);           /**< The advertising interval (in units of 0.625 ms). */

volatile BLEStatus bleStatus = 0;
ble_uuid_t bleUUIDFilter;
/// When doing service discovery, this is the last handle we'll need to discover with
uint16_t bleFinalHandle;

/// Array of data waiting to be sent over Bluetooth NUS
uint8_t nusTxBuf[BLE_NUS_MAX_DATA_LEN];
/// Number of bytes ready to send inside nusTxBuf
uint16_t nuxTxBufLength = 0;

#ifdef NRF52_SERIES
#define DYNAMIC_INTERVAL_ADJUSTMENT
#endif
/* Dynamic interval adjustment kicks Espruino into a low power mode after
 * a certain amount of time of being connected with nothing happening. The next time
 * stuff happens it renegotiates back to the high rate, but this could take a second
 * or two.
 */
#ifdef DYNAMIC_INTERVAL_ADJUSTMENT
#define BLE_DYNAMIC_INTERVAL_LOW_RATE 200 // connection interval when idle (milliseconds)
#define BLE_DYNAMIC_INTERVAL_HIGH_RATE 7.5 // connection interval when not idle (milliseconds) - 7.5ms is fastest possible
#define BLE_DEFAULT_HIGH_INTERVAL true
#define DEFAULT_PERIPH_MAX_CONN_INTERVAL BLE_DYNAMIC_INTERVAL_HIGH_RATE // highest possible on connect
#define BLE_DYNAMIC_INTERVAL_IDLE_TIME (int)(120000 / BLE_DYNAMIC_INTERVAL_HIGH_RATE) // time in milliseconds at which we enter idle
/// How many connection intervals has BLE been idle for? Use for dynamic interval adjustment
uint16_t bleIdleCounter = 0;
/// Are we using a high speed or low speed interval at the moment?
bool bleHighInterval;
#else
// No interval adjustment - allow us to enter a slightly lower power connection state
#define DEFAULT_PERIPH_MAX_CONN_INTERVAL 20
#endif

static ble_gap_sec_params_t get_gap_sec_params();
#if PEER_MANAGER_ENABLED
static bool jsble_can_pair_with_peer(const ble_gap_sec_params_t *own_params, const ble_gap_sec_params_t *peer_params);
#endif

#if NRF_SD_BLE_API_VERSION>5
// if m_scan_param.extended=0, use BLE_GAP_SCAN_BUFFER_MIN
// if m_scan_param.extended=1, use BLE_GAP_SCAN_BUFFER_EXTENDED_MIN
uint8_t m_scan_buffer_data[BLE_GAP_SCAN_BUFFER_EXTENDED_MIN]; /**< buffer where advertising reports will be stored by the SoftDevice. */
ble_data_t m_scan_buffer = {
   m_scan_buffer_data,
   sizeof(m_scan_buffer_data)
};
// TODO: this is 255 bytes to allow extended advertising. Maybe we don't need that all the time?
#endif

// -----------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------

/// Checks for error and reports an exception string if there was one, else 0 if no error
JsVar *jsble_get_error_string(uint32_t err_code) {
  if (!err_code) return 0;
#ifndef ESPR_NO_BLUETOOTH_MESSAGES
  const char *name = 0;
  switch (err_code) {
   case NRF_ERROR_NO_MEM        : name="NO_MEM"; break;
   case NRF_ERROR_INVALID_PARAM : name="INVALID_PARAM"; break;
   case NRF_ERROR_INVALID_STATE : name="INVALID_STATE"; break;
   case NRF_ERROR_INVALID_LENGTH: name="INVALID_LENGTH"; break;
   case NRF_ERROR_INVALID_FLAGS : name="INVALID_FLAGS"; break;
   case NRF_ERROR_DATA_SIZE     : name="DATA_SIZE"; break;
   case NRF_ERROR_FORBIDDEN     : name="FORBIDDEN"; break;
   case NRF_ERROR_BUSY          : name="BUSY"; break;
   case NRF_ERROR_INVALID_ADDR  : name="INVALID ADDR"; break;
   case BLE_ERROR_INVALID_CONN_HANDLE
                                : name="INVALID_CONN_HANDLE"; break;
   case BLE_ERROR_GAP_INVALID_BLE_ADDR
                                : name="INVALID_BLE_ADDR"; break;
   case NRF_ERROR_CONN_COUNT    : name="CONN_COUNT"; break;
   case BLE_ERROR_NOT_ENABLED   : name="NOT_ENABLED"; break;

#if NRF_SD_BLE_API_VERSION<5
   case BLE_ERROR_NO_TX_PACKETS : name="NO_TX_PACKETS"; break;
#endif
  }
  if (name)
    return jsvVarPrintf("ERR 0x%x (%s)", err_code, name);
  else
#endif // ESPR_NO_BLUETOOTH_MESSAGES
    return jsvVarPrintf("ERR 0x%x", err_code);
}

// -----------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------

/// Executes a pending BLE event - returns the number of events Handled
int jsble_exec_pending(IOEvent *event) {
  int eventsHandled = 1;
  // if we got event data, unpack it first into a buffer
#if NRF_BLE_MAX_MTU_SIZE>64
  unsigned char buffer[NRF_BLE_MAX_MTU_SIZE];
#else
  unsigned char buffer[64];
#endif
  assert(sizeof(buffer) >= sizeof(BLEAdvReportData));
  assert(sizeof(buffer) >= NRF_BLE_MAX_MTU_SIZE);
  size_t bufferLen = 0;
  while (IOEVENTFLAGS_GETTYPE(event->flags) == EV_BLUETOOTH_PENDING_DATA) {
    int i, chars = IOEVENTFLAGS_GETCHARS(event->flags);
    for (i=0;i<chars;i++) {
      assert(bufferLen < sizeof(buffer));
      if (bufferLen < sizeof(buffer))
        buffer[bufferLen++] = event->data.chars[i];
    }

    jshPopIOEvent(event);
    eventsHandled++;
  }
  assert(IOEVENTFLAGS_GETTYPE(event->flags) == EV_BLUETOOTH_PENDING);

  // Now handle the actual event
  BLEPending blep = (BLEPending)(event->data.time&255);
  uint16_t data = (uint16_t)(event->data.time>>8);
  /* jsble_exec_pending_common handles 'common' events between nRF52/ESP32, then
   * we handle nRF52-specific events below */
  if (!jsble_exec_pending_common(blep, data, buffer, bufferLen)) switch (blep) {
   case BLEP_CONNECTED: {
     assert(bufferLen == sizeof(ble_gap_addr_t));
     ble_gap_addr_t *peer_addr = (ble_gap_addr_t*)buffer;
     bleQueueEventAndUnLock(JS_EVENT_PREFIX"connect", bleAddrToStr(*peer_addr));
     jshHadEvent();
     break;
   }
   case BLEP_DISCONNECTED: {
     JsVar *reason = jsvNewFromInteger(data);
     bleQueueEventAndUnLock(JS_EVENT_PREFIX"disconnect", reason);
     break;
   }
   case BLEP_ADVERTISING_START: {
     if (bleStatus & BLE_IS_ADVERTISING) jsble_advertising_stop(); // if we were advertising, stop
     jsble_advertising_start(); // start advertising - we ignore the return code here
     break;
   }
   case BLEP_ADVERTISING_STOP: {
     jsble_advertising_stop();
     break;
   }
   case BLEP_RESTART_SOFTDEVICE: {
     jsble_restart_softdevice(NULL);
     break;
   }
   case BLEP_RSSI_PERIPH: {
     JsVar *evt = jsvNewFromInteger((signed char)data);
     if (evt) jsiQueueObjectCallbacks(execInfo.root, BLE_RSSI_EVENT, &evt, 1);
     jsvUnLock(evt);
     break;
   }
   case BLEP_WRITE: {
     JsVar *evt = jsvNewObject();
     if (evt) {
       JsVar *str = jsvNewStringOfLength(bufferLen, (char*)buffer);
       if (str) {
         JsVar *ab = jsvNewArrayBufferFromString(str, bufferLen);
         jsvUnLock(str);
         jsvObjectSetChildAndUnLock(evt, "data", ab);
       }
       char eventName[12];
       bleGetWriteEventName(eventName, data);
       jsiQueueObjectCallbacks(execInfo.root, eventName, &evt, 1);
       jsvUnLock(evt);
     }
     break;
   }
#if CENTRAL_LINK_COUNT>0
   case BLEP_RSSI_CENTRAL: { //  rssi as data low byte, index in m_central_conn_handles as high byte
     int centralIdx = data>>8; // index in m_central_conn_handles
     JsVar *gattServer = bleGetActiveBluetoothGattServer(centralIdx);
     if (gattServer) {
       JsVar *rssi = jsvNewFromInteger((signed char)(data & 255));
       JsVar *bluetoothDevice = jsvObjectGetChildIfExists(gattServer, "device");
       if (bluetoothDevice) {
         jsvObjectSetChild(bluetoothDevice, "rssi", rssi);
       }
       jsiQueueObjectCallbacks(gattServer, BLE_RSSI_EVENT, &rssi, 1);
       jsvUnLock3(rssi, gattServer, bluetoothDevice);
     }
     break;
   }

   case BLEP_TASK_DISCOVER_CHARACTERISTIC: { /* bleTaskInfo = BluetoothRemoteGATTService, bleTaskInfo2 = an array of BluetoothRemoteGATTCharacteristic, or 0 */
        if (!bleInTask(BLETASK_CHARACTERISTIC)) {
          jsExceptionHere(JSET_INTERNALERROR,"Wrong task: %d vs %d", bleGetCurrentTask(), BLETASK_PRIMARYSERVICE);
          break;
        }
        ble_gattc_char_t *p_chr = (ble_gattc_char_t*)buffer;
        if (!bleTaskInfo2) bleTaskInfo2 = jsvNewEmptyArray();
        if (!bleTaskInfo2) break;
        JsVar *o = jspNewObject(0, "BluetoothRemoteGATTCharacteristic");
        if (o) {
          jsvObjectSetChild(o,"service", bleTaskInfo);
          jsvObjectSetChildAndUnLock(o,"uuid", bleUUIDToStr(p_chr->uuid));
          jsvObjectSetChildAndUnLock(o,"handle_value", jsvNewFromInteger(p_chr->handle_value));
          jsvObjectSetChildAndUnLock(o,"handle_decl", jsvNewFromInteger(p_chr->handle_decl));
          JsVar *p = jsvNewObject();
          if (p) {
            jsvObjectSetChildAndUnLock(p,"broadcast",jsvNewFromBool(p_chr->char_props.broadcast));
            jsvObjectSetChildAndUnLock(p,"read",jsvNewFromBool(p_chr->char_props.read));
            jsvObjectSetChildAndUnLock(p,"writeWithoutResponse",jsvNewFromBool(p_chr->char_props.write_wo_resp));
            jsvObjectSetChildAndUnLock(p,"write",jsvNewFromBool(p_chr->char_props.write));
            jsvObjectSetChildAndUnLock(p,"notify",jsvNewFromBool(p_chr->char_props.notify));
            jsvObjectSetChildAndUnLock(p,"indicate",jsvNewFromBool(p_chr->char_props.indicate));
            jsvObjectSetChildAndUnLock(p,"authenticatedSignedWrites",jsvNewFromBool(p_chr->char_props.auth_signed_wr));
            jsvObjectSetChildAndUnLock(o,"properties", p);
          }
          // char_props?
          jsvArrayPushAndUnLock(bleTaskInfo2, o);
        }
        break;
      }
   case BLEP_TASK_DISCOVER_CHARACTERISTIC_COMPLETE: { /* bleTaskInfo = BluetoothRemoteGATTService, bleTaskInfo2 = an array of BluetoothRemoteGATTCharacteristic, or 0 */
     // When done, send the result to the handler
     if (bleTaskInfo2 && bleUUIDFilter.type != BLE_UUID_TYPE_UNKNOWN) {
       // single item because filtering
       JsVar *t = jsvSkipNameAndUnLock(jsvArrayPopFirst(bleTaskInfo2));
       jsvUnLock(bleTaskInfo2);
       bleTaskInfo2 = t;
     }
     if (bleTaskInfo) bleCompleteTaskSuccess(BLETASK_CHARACTERISTIC, bleTaskInfo2);
     else bleCompleteTaskFailAndUnLock(BLETASK_CHARACTERISTIC, jsvNewFromString("No Characteristics found"));
     break;
   }
   case BLEP_TASK_DISCOVER_CCCD: { /* bleTaskInfo = BluetoothRemoteGATTCharacteristic */
     uint16_t cccd_handle = data;
     if (cccd_handle) {
       if(bleTaskInfo)
         jsvObjectSetChildAndUnLock(bleTaskInfo, "handle_cccd", jsvNewFromInteger(cccd_handle));
       // Switch task here rather than completing...
       bleSwitchTask(BLETASK_CHARACTERISTIC_NOTIFY);
       jsble_central_characteristicNotify(jswrap_ble_BluetoothRemoteGATTCharacteristic_getHandle(bleTaskInfo), bleTaskInfo, true);
     } else {
       // Couldn't find anything - just report error
       bleCompleteTaskFailAndUnLock(BLETASK_CHARACTERISTIC_DESC_AND_STARTNOTIFY, jsvNewFromString("CCCD Handle not found"));
     }
     break;
   }
#endif // CENTRAL_LINK_COUNT>0
#if PEER_MANAGER_ENABLED
   case BLEP_BONDING_STATUS: {
     BLEBondingStatus bondStatus = (BLEBondingStatus)data;
     const char *bondString = 0;
     switch (bondStatus) {
       case BLE_BOND_REQUEST:
         bondString="request";
         break;
       case BLE_BOND_START:
         bondString="start";
         break;
       case BLE_BOND_SUCCESS:
         bondString="success";
         if (bleInTask(BLETASK_BONDING))
           bleCompleteTaskSuccess(BLETASK_BONDING, 0);
         break;
       case BLE_BOND_FAIL:
         bondString="fail";
         if (bleInTask(BLETASK_BONDING))
           bleCompleteTaskFailAndUnLock(BLETASK_BONDING, jsvNewFromString("Bonding failed"));
         break;
     }
     if (bondString)
       bleQueueEventAndUnLock(JS_EVENT_PREFIX"bond", jsvNewFromString(bondString));
     break;
   }
#endif
#ifdef USE_NFC
   case BLEP_NFC_STATUS:
     bleQueueEventAndUnLock(data ? JS_EVENT_PREFIX"NFCon" : JS_EVENT_PREFIX"NFCoff", 0);
     break;
   case BLEP_NFC_TX:
     bleQueueEventAndUnLock(JS_EVENT_PREFIX"NFCtx", 0);
     break;
   case BLEP_NFC_RX: {
     /* try to fetch NfcData data */
     JsVar *nfcData = jsvObjectGetChildIfExists(execInfo.hiddenRoot, "NfcData");
     if(nfcData) {
       /* success - handle request internally */
       JSV_GET_AS_CHAR_ARRAY(nfcDataPtr, nfcDataLen, nfcData);
       jsvUnLock(nfcData);

       /* check data, on error let request go into timeout - reader will retry. */
       if (!nfcDataPtr || !nfcDataLen) {
         break;
       }

       /* check rx data length and read block command code (0x30) */
       if(bufferLen < 2 || buffer[0] != 0x30) {
         jsble_nfc_send_rsp(0, 0); /* switch to rx */
         break;
       }

       /* fetch block index (addressing is done in multiples of 4 byte */
       size_t idx = buffer[1] * 4;

       /* assemble 16 byte block */
       uint8_t buf[16]; memset(buf, '\0', 16);
       if(idx + 16 < nfcDataLen) {
         memcpy(buf, nfcDataPtr + idx, 16);
       } else
       if(idx < nfcDataLen) {
         memcpy(buf, nfcDataPtr + idx, nfcDataLen - idx);
       }
       /* send response */
       jsble_nfc_send(buf, 16);
     } else {
       /* no NfcData available, fire js-event */
       bleQueueEventAndUnLock(JS_EVENT_PREFIX"NFCrx",
           jsvNewArrayBufferWithData(bufferLen, buffer));
     }
     break;
   }
#endif
#if BLE_HIDS_ENABLED
   case BLEP_HID_SENT:
     jsiQueueObjectCallbacks(execInfo.root, BLE_HID_SENT_EVENT, 0, 0);
     jsvObjectSetChild(execInfo.root, BLE_HID_SENT_EVENT, 0); // fire only once
     jshHadEvent();
     break;
   case BLEP_HID_VALUE:
     bleQueueEventAndUnLock(JS_EVENT_PREFIX"HID", jsvNewFromInteger(data));
     break;
#endif
#ifdef LINK_SECURITY
      case BLEP_TASK_PASSKEY_DISPLAY: { // data = connection handle
        uint16_t conn_handle = data;
#if CENTRAL_LINK_COUNT>0
        /* TODO: yes/no passkey
uint8_t match_request : 1;               If 1 requires the application to report the match using @ref sd_ble_gap_auth_key_reply
                                         with either @ref BLE_GAP_AUTH_KEY_TYPE_NONE if there is no match or
                                         @ref BLE_GAP_AUTH_KEY_TYPE_PASSKEY if there is a match. */
        int centralIdx = jsble_get_central_connection_idx(conn_handle);
#endif
        if (bufferLen==BLE_GAP_PASSKEY_LEN) {
          buffer[BLE_GAP_PASSKEY_LEN] = 0;
          JsVar *passkey = jsvNewFromString((char*)buffer);
#if CENTRAL_LINK_COUNT>0
          if (centralIdx<0) { // it's on the peripheral connection
#endif
            bleQueueEventAndUnLock(JS_EVENT_PREFIX"passkey", passkey);
#if CENTRAL_LINK_COUNT>0
          } else { // it's on a central connection
            JsVar *gattServer = bleGetActiveBluetoothGattServer(centralIdx);
            if (gattServer) {
              JsVar *bluetoothDevice = jsvObjectGetChildIfExists(gattServer, "device");
              if (bluetoothDevice) {
                jsiQueueObjectCallbacks(bluetoothDevice, JS_EVENT_PREFIX"passkey", &passkey, 1);
                jshHadEvent();
              }
              jsvUnLock2(bluetoothDevice, gattServer);
            }
          }
#endif
          jsvUnLock(passkey);
        }
        break;
      }
      case BLEP_TASK_AUTH_KEY_REQUEST: { // data = connection handle
        //jsiConsolePrintf("BLEP_TASK_AUTH_KEY_REQUEST\n");
        uint16_t conn_handle = data;
#if CENTRAL_LINK_COUNT>0
        int centralIdx = jsble_get_central_connection_idx(conn_handle);
        JsVar *gattServer = bleGetActiveBluetoothGattServer(centralIdx);
        if (gattServer) {
          jsvObjectSetChildAndUnLock(gattServer, "connected", jsvNewFromBool(false));
          JsVar *bluetoothDevice = jsvObjectGetChildIfExists(gattServer, "device");
          if (bluetoothDevice) {
            // HCI error code, see BLE_HCI_STATUS_CODES in ble_hci.h
            jsiQueueObjectCallbacks(bluetoothDevice, JS_EVENT_PREFIX"passkeyRequest", 0, 0);
            jshHadEvent();
          }
          jsvUnLock2(gattServer, bluetoothDevice);
        }
#endif
        if (conn_handle == m_peripheral_conn_handle) {
          bool ok = false;
          JsVar *options = jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_SECURITY);
          if (jsvIsObject(options)) {
            JsVar *oobKey = jsvObjectGetChildIfExists(options, "oob");
            JSV_GET_AS_CHAR_ARRAY(keyPtr, keyLen, oobKey);
            if (keyPtr && keyLen==BLE_GAP_SEC_KEY_LEN) {
              ok = true;
              //jsiConsolePrintf("Replying with Auth key %d,%d,%d,%d...\n",keyPtr[0],keyPtr[1],keyPtr[2],keyPtr[3]);
              jsble_check_error(sd_ble_gap_auth_key_reply(conn_handle,
                                                   BLE_GAP_AUTH_KEY_TYPE_OOB,
                                                   (uint8_t *)keyPtr));
            }
            jsvUnLock(oobKey);
          }
          jsvUnLock(options);
          if (!ok) jsExceptionHere(JSET_INTERNALERROR, "Auth key requested, but NRF.setSecurity({oob}) not valid");
          // TODO: this could be because we have keyboard:1 and the connecting device is displaying a passkey and wants US to send it back (we only implement that for central at the moment)
        }
        break;
       }
      case BLEP_TASK_AUTH_STATUS: {
        //uint16_t conn_handle = data;
        ble_gap_evt_auth_status_t *auth_status = (ble_gap_evt_auth_status_t*)buffer;
        JsVar *o = jsvNewObject();
        if (o) {
          const char *str=NULL;
#ifndef ESPR_NO_BLUETOOTH_MESSAGES
          switch(auth_status->auth_status) {
            case BLE_GAP_SEC_STATUS_SUCCESS                : str="SUCCESS";break;
            case BLE_GAP_SEC_STATUS_TIMEOUT                : str="TIMEOUT";break;
            case BLE_GAP_SEC_STATUS_PDU_INVALID            : str="PDU_INVALID";break;
            case BLE_GAP_SEC_STATUS_RFU_RANGE1_BEGIN       : str="RFU_RANGE1_BEGIN";break;
            case BLE_GAP_SEC_STATUS_RFU_RANGE1_END         : str="RFU_RANGE1_END";break;
            case BLE_GAP_SEC_STATUS_PASSKEY_ENTRY_FAILED   : str="PASSKEY_ENTRY_FAILED";break;
            case BLE_GAP_SEC_STATUS_OOB_NOT_AVAILABLE      : str="OOB_NOT_AVAILABLE";break;
            case BLE_GAP_SEC_STATUS_AUTH_REQ               : str="AUTH_REQ";break;
            case BLE_GAP_SEC_STATUS_CONFIRM_VALUE          : str="CONFIRM_VALUE";break;
            case BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP       : str="PAIRING_NOT_SUPP";break;
            case BLE_GAP_SEC_STATUS_ENC_KEY_SIZE           : str="ENC_KEY_SIZE";break;
            case BLE_GAP_SEC_STATUS_SMP_CMD_UNSUPPORTED    : str="SMP_CMD_UNSUPPORTED";break;
            case BLE_GAP_SEC_STATUS_UNSPECIFIED            : str="UNSPECIFIED";break;
            case BLE_GAP_SEC_STATUS_REPEATED_ATTEMPTS      : str="REPEATED_ATTEMPTS";break;
            case BLE_GAP_SEC_STATUS_INVALID_PARAMS         : str="INVALID_PARAMS";break;
            case BLE_GAP_SEC_STATUS_DHKEY_FAILURE          : str="DHKEY_FAILURE";break;
            case BLE_GAP_SEC_STATUS_NUM_COMP_FAILURE       : str="NUM_COMP_FAILURE";break;
            case BLE_GAP_SEC_STATUS_BR_EDR_IN_PROG         : str="BR_EDR_IN_PROG";break;
            case BLE_GAP_SEC_STATUS_X_TRANS_KEY_DISALLOWED : str="X_TRANS_KEY_DISALLOWED";break;
          }
#endif // ESPR_NO_BLUETOOTH_MESSAGES
          jsvObjectSetChildAndUnLock(o,"auth_status",str?jsvNewFromString(str):jsvNewFromInteger(auth_status->auth_status));
          jsvObjectSetChildAndUnLock(o, "bonded", jsvNewFromBool(auth_status->bonded));
          jsvObjectSetChildAndUnLock(o, "lv4", jsvNewFromInteger(auth_status->sm1_levels.lv4));
          jsvObjectSetChildAndUnLock(o, "kdist_own", jsvNewFromInteger(*((uint8_t *)&auth_status->kdist_own)));
          jsvObjectSetChildAndUnLock(o, "kdist_peer", jsvNewFromInteger(*((uint8_t *)&auth_status->kdist_peer)));
          bleQueueEventAndUnLock(JS_EVENT_PREFIX"security",o);
        }
        break;
      }
#endif
#ifdef ESPR_BLUETOOTH_ANCS
      case BLEP_ANCS_DISCOVERED:
        ble_ancs_handle_discovered();
        break;
      case BLEP_ANCS_NOTIF:
        ble_ancs_handle_notif(blep, (ble_ancs_c_evt_notif_t*)buffer);
        break;
      case BLEP_ANCS_NOTIF_ATTR:
        ble_ancs_handle_notif_attr(blep, (ble_ancs_c_evt_notif_t*)buffer);
        break;
      case BLEP_ANCS_APP_ATTR:
        ble_ancs_handle_app_attr(blep, (char *)buffer, bufferLen);
        break;
      case BLEP_ANCS_ERROR:
        if (BLETASK_IS_ANCS(bleGetCurrentTask()))
          bleCompleteTaskFailAndUnLock(bleGetCurrentTask(), jsvNewFromString("ANCS Error"));
        break;
      case BLEP_AMS_DISCOVERED:
        ble_ams_handle_discovered();
        break;
      case BLEP_AMS_TRACK_UPDATE:
        ble_ams_handle_track_update(blep, data, (char *)buffer, bufferLen);
        break;
      case BLEP_AMS_PLAYER_UPDATE:
        ble_ams_handle_player_update(blep, data, (char *)buffer, bufferLen);
        break;
      case BLEP_AMS_ATTRIBUTE:
        ble_ams_handle_attribute(blep, (char *)buffer, bufferLen);
        break;
      case BLEP_CTS_DISCOVERED:
        ble_cts_handle_discovered();
        break;
      case BLEP_CTS_TIME:
        ble_cts_handle_time(blep, (char *)buffer, bufferLen);
        break;
#endif
#ifndef SAVE_ON_FLASH
   default:
     jsWarn("jsble_exec_pending: Unknown enum type %d",(int)blep);
#endif
 }
 if (jspIsInterrupted())
   jsWarn("Interrupted processing event %d",(int)blep);
 return eventsHandled;
}


// -----------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------

/** Set the connection interval of the peripheral connection. Returns an error code. */
uint32_t jsble_set_periph_connection_interval(JsVarFloat min, JsVarFloat max) {
  ble_gap_conn_params_t   gap_conn_params;
  memset(&gap_conn_params, 0, sizeof(gap_conn_params));
  gap_conn_params.min_conn_interval = (uint16_t)(0.5+MSEC_TO_UNITS(min, UNIT_1_25_MS));   // Minimum acceptable connection interval
  gap_conn_params.max_conn_interval = (uint16_t)(0.5+MSEC_TO_UNITS(max, UNIT_1_25_MS));    // Maximum acceptable connection interval
  gap_conn_params.slave_latency     = SLAVE_LATENCY;
  gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;
  if (jsble_has_peripheral_connection()) {
    // Connected - initiate dynamic change
    return ble_conn_params_change_conn_params(
#if NRF_SD_BLE_API_VERSION>=5
        m_peripheral_conn_handle,
#endif
        &gap_conn_params);
  } else {
    // Not connected, just tell the stack
    return sd_ble_gap_ppcp_set(&gap_conn_params);
  }
}

/** Is BLE connected to any device at all? */
bool jsble_has_connection() {
  if (jsble_has_central_connection())
    return true;
  return m_peripheral_conn_handle != BLE_CONN_HANDLE_INVALID;
}

/** Is BLE connected to a central device at all? */
bool jsble_has_central_connection() {
#if CENTRAL_LINK_COUNT>0
  for (int i=0;i<CENTRAL_LINK_COUNT;i++)
    if (m_central_conn_handles[i] != BLE_CONN_HANDLE_INVALID)
      return true;
#endif
  return false;
}

/** Return the index of the central connection in m_central_conn_handles, or -1 */
int jsble_get_central_connection_idx(uint16_t handle) {
#if CENTRAL_LINK_COUNT>0
  for (int i=0;i<CENTRAL_LINK_COUNT;i++)
    if (m_central_conn_handles[i] == handle)
      return i;
#endif
  return -1;
}

/** Is BLE connected to a server device at all (eg, the simple, 'slave' mode)? */
bool jsble_has_peripheral_connection() {
  return (m_peripheral_conn_handle != BLE_CONN_HANDLE_INVALID);
}

/** Call this when something happens on BLE with this as
 * a peripheral - used with Dynamic Interval Adjustment  */
void jsble_peripheral_activity() {
#ifdef DYNAMIC_INTERVAL_ADJUSTMENT
  if (jsble_has_peripheral_connection() &&
      !(bleStatus & BLE_DISABLE_DYNAMIC_INTERVAL) &&
      bleIdleCounter < 10) {
    // so we must have been called once before
    if (!bleHighInterval) {
      bleHighInterval = true;
      jsble_set_periph_connection_interval(BLE_DYNAMIC_INTERVAL_HIGH_RATE, BLE_DYNAMIC_INTERVAL_HIGH_RATE);
    }
  }
  bleIdleCounter = 0;
#endif
}

/// Checks for error and reports an exception if there was one. Return true on error
#ifndef SAVE_ON_FLASH_EXTREME
bool jsble_check_error_line(uint32_t err_code, int lineNumber) {
  JsVar *v = jsble_get_error_string(err_code);
  if (!v) return 0;
  jsExceptionHere(JSET_ERROR, "%v (:%d)", v, lineNumber);
  bleQueueEventAndUnLock(JS_EVENT_PREFIX"error", v);
  return true;
}
#else
bool jsble_check_error(uint32_t err_code) {
  JsVar *v = jsble_get_error_string(err_code);
  if (!v) return 0;
  jsExceptionHere(JSET_ERROR, "%v", v);
  jsvUnLock(v);
  return true;
}
#endif

// -----------------------------------------------------------------------------------
// --------------------------------------------------------------------------- ERRORS

void ble_app_error_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name) {
#ifdef LED1_PININDEX
  jshPinOutput(LED1_PININDEX, LED1_ONSTATE);
#endif
#ifdef LED2_PININDEX
  jshPinOutput(LED2_PININDEX, LED2_ONSTATE);
#endif
#ifdef LED3_PININDEX
  jshPinOutput(LED3_PININDEX, LED3_ONSTATE);
#endif
  jsiConsolePrintf("NRF ERROR 0x%x\n at %s:%d\nREBOOTING.\n", error_code, p_file_name?(const char *)p_file_name:"?", line_num);

#ifdef USE_TERMINAL
  // If we have a terminal, try and write to that!
  jsiStatus  |= JSIS_ECHO_OFF;
  jsiSetConsoleDevice(EV_TERMINAL, 1);
  jsiConsolePrintf("NRF ERROR 0x%x\n at %s:%d\nREBOOTING.\n", error_code, p_file_name?(const char *)p_file_name:"?", line_num);
  jswrap_terminal_idle();
#endif

  /* don't flush - just delay. If this happened in an IRQ, waiting to flush
   * will result in the device locking up. */
#ifdef USE_TERMINAL
  nrf_delay_ms(10000);
#else
  nrf_delay_ms(1000);
#endif
  NVIC_SystemReset();
}

void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name) {
  ble_app_error_handler(0xDEADBEEF, line_num, p_file_name);
}

void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info) {
  if (id == NRF_FAULT_ID_SDK_ERROR) {
    error_info_t *error_info = (error_info_t *)info;
    ble_app_error_handler(error_info->err_code, error_info->line_num, error_info->p_file_name);
  } else
    ble_app_error_handler(id, pc, 0);
}

/// Function for handling errors from the Connection Parameters module.
static void conn_params_error_handler(uint32_t nrf_error) {
  /* connection parameters module can produce this if the connection
   * is disconnected at just the right point while it is trying to
   * negotiate connection parameters. Ignore it, since we don't
   * want it to be able to reboot the device!
   */
  if (nrf_error == NRF_ERROR_INVALID_STATE)
    return;
  APP_ERROR_CHECK_NOT_URGENT(nrf_error);
}

#if BLE_HIDS_ENABLED
static void service_error_handler(uint32_t nrf_error) {
  APP_ERROR_CHECK_NOT_URGENT(nrf_error);
}
#endif

#if NRF_LOG_ENABLED
void nrf_log_frontend_std_0(uint32_t severity_mid, char const * const p_str) {
  nrf_log_frontend_std_6(severity_mid, p_str,0,0,0,0,0,0);
}


void nrf_log_frontend_std_1(uint32_t            severity_mid,
                            char const * const p_str,
                            uint32_t           val0) {
  nrf_log_frontend_std_6(severity_mid, p_str,val0,0,0,0,0,0);
}


void nrf_log_frontend_std_2(uint32_t           severity_mid,
                            char const * const p_str,
                            uint32_t           val0,
                            uint32_t           val1) {
  nrf_log_frontend_std_6(severity_mid, p_str,val0,val1,0,0,0,0);
}


void nrf_log_frontend_std_3(uint32_t           severity_mid,
                            char const * const p_str,
                            uint32_t           val0,
                            uint32_t           val1,
                            uint32_t           val2) {
  nrf_log_frontend_std_6(severity_mid, p_str,val0,val1,val2,0,0,0);
}


void nrf_log_frontend_std_4(uint32_t           severity_mid,
                            char const * const p_str,
                            uint32_t           val0,
                            uint32_t           val1,
                            uint32_t           val2,
                            uint32_t           val3) {
  nrf_log_frontend_std_6(severity_mid, p_str,val0,val1,val2,val3,0,0);
}


void nrf_log_frontend_std_5(uint32_t           severity_mid,
                            char const * const p_str,
                            uint32_t           val0,
                            uint32_t           val1,
                            uint32_t           val2,
                            uint32_t           val3,
                            uint32_t           val4) {
  nrf_log_frontend_std_6(severity_mid, p_str,val0,val1,val2,val3,val4,0);
}


void nrf_log_frontend_std_6(uint32_t           severity_mid,
                            char const * const p_str,
                            uint32_t           val0,
                            uint32_t           val1,
                            uint32_t           val2,
                            uint32_t           val3,
                            uint32_t           val4,
                            uint32_t           val5) {
#ifdef DEFAULT_CONSOLE_DEVICE
  jshTransmitPrintf(DEFAULT_CONSOLE_DEVICE, p_str, val0, val1, val2, val3, val4, val5);
  jshTransmit(DEFAULT_CONSOLE_DEVICE, '\r');
  jshTransmit(DEFAULT_CONSOLE_DEVICE, '\n');
#endif
}

void nrf_log_frontend_hexdump(uint32_t           severity_mid,
                              const void * const p_data,
                              uint16_t           length) {
  uint8_t *u8_data = (uint8_t *)p_data;
  unsigned int i;
  for (i=0;i<length;i++) {
    jshTransmitPrintf(DEFAULT_CONSOLE_DEVICE, "%02x ", u8_data[i]);
    if ((i&7) == 7) {
      jshTransmit(DEFAULT_CONSOLE_DEVICE, '\r');
      jshTransmit(DEFAULT_CONSOLE_DEVICE, '\n');
    }
  }
  jshTransmit(DEFAULT_CONSOLE_DEVICE, '\r');
  jshTransmit(DEFAULT_CONSOLE_DEVICE, '\n');
}

#ifdef NRF5X_SDK_15_3
const nrf_log_module_const_data_t m_nrf_log_app_logs_data_const = {
    .p_module_name = ""
};
#else
nrf_log_module_dynamic_data_t NRF_LOG_MODULE_DATA_DYNAMIC = {
    .module_id = 0
};
#endif
#endif

/// Function for handling an event from the Connection Parameters Module.
static void on_conn_params_evt(ble_conn_params_evt_t * p_evt) {
  // Either BLE_CONN_PARAMS_EVT_FAILED or BLE_CONN_PARAMS_EVT_SUCCEEDED - that's it
}

/// Sigh - NFC has lots of these, so we need to define it to build
void log_uart_printf(const char * format_msg, ...) {
 // jsiConsolePrintf("NFC: %s\n", format_msg);
}

// -----------------------------------------------------------------------------------
// -------------------------------------------------------------------------- HANDLERS

#if NRF_SD_BLE_API_VERSION<5
static void nus_data_handler(ble_nus_t * p_nus, uint8_t * p_data, uint16_t length) {
  jsble_peripheral_activity(); // flag that we've been busy
  jshPushIOCharEvents(EV_BLUETOOTH, (char*)p_data, length);
  jshHadEvent();
}
#else
static void nus_data_handler(ble_nus_evt_t * p_evt) {
  if (p_evt->type == BLE_NUS_EVT_RX_DATA) {
    jsble_peripheral_activity(); // flag that we've been busy
    jshPushIOCharEvents(EV_BLUETOOTH, (char*)p_evt->params.rx_data.p_data, p_evt->params.rx_data.length);
    jshHadEvent();
  }
}
#endif

void nus_transmit_string() {
  if (!jsble_has_peripheral_connection() ||
      !(bleStatus & BLE_NUS_INITED) ||
      (bleStatus & BLE_IS_SLEEPING)) {
    // If no connection, drain the output buffer
    nuxTxBufLength = 0;
    jshTransmitClearDevice(EV_BLUETOOTH);
    return;
  }
  /* 6 is the max number of packets we can send
   * in one connection interval on nRF52. We could
   * do 5, but it seems some things have issues with
   * this (eg nRF cloud gateways) so only send 1 packet
   * for now. */
  int max_data_len = MIN((m_peripheral_effective_mtu-3),BLE_NUS_MAX_DATA_LEN);
  for (int packet=0;packet<1;packet++) {
    // No data? try and get some from our queue
    if (!nuxTxBufLength) {
      nuxTxBufLength = 0;
      int ch = jshGetCharToTransmit(EV_BLUETOOTH);
      while (ch>=0) {
        nusTxBuf[nuxTxBufLength++] = ch;
        if (nuxTxBufLength>=max_data_len) break;
        ch = jshGetCharToTransmit(EV_BLUETOOTH);
      }
    }
    // If there's no data in the queue, nothing to do - leave
    if (!nuxTxBufLength) return;
    jsble_peripheral_activity(); // flag that we've been busy
    // We have data - try and send it

#if NRF_SD_BLE_API_VERSION>5
    uint32_t err_code = ble_nus_data_send(&m_nus, nusTxBuf, &nuxTxBufLength, m_peripheral_conn_handle);
#elif NRF_SD_BLE_API_VERSION<5
    uint32_t err_code = ble_nus_string_send(&m_nus, nusTxBuf, nuxTxBufLength);
#else // NRF_SD_BLE_API_VERSION==5
    uint32_t err_code = ble_nus_string_send(&m_nus, nusTxBuf, &nuxTxBufLength);
#endif
    if (err_code == NRF_SUCCESS) {
      nuxTxBufLength=0; // everything sent Ok
      bleStatus |= BLE_IS_SENDING;
    } else if (err_code==NRF_ERROR_INVALID_STATE) {
      // If no notifications we are connected but the central isn't reading, so sends will fail.
      // Ideally we check m_nus.is_notification_enabled but SDK15 changed this, so lets just see if
      // the send creates a NRF_ERROR_INVALID_STATE error
      nuxTxBufLength = 0; // clear tx buffer
      jshTransmitClearDevice(EV_BLUETOOTH); // clear all tx data in queue
    }
    /* if it failed to send all or any data we keep it around in
     * nusTxBuf (with count in nuxTxBufLength) so next time around
     * we can try again. */
  }
}

/// Radio Notification handler
void SWI1_IRQHandler(bool radio_evt) {
  if (bleStatus & BLE_NUS_INITED)
    nus_transmit_string();
  // If we're doing multiple advertising, iterate through advertising options
  if ((bleStatus & BLE_IS_ADVERTISING)  && (bleStatus & BLE_IS_ADVERTISING_MULTIPLE)) {
    int idx = (bleStatus&BLE_ADVERTISING_MULTIPLE_MASK)>>BLE_ADVERTISING_MULTIPLE_SHIFT;
    JsVar *advData = jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_ADVERTISE_DATA);
    bool ok = true;
    if (jsvIsArray(advData)) {
      JsVar *data = jsvGetArrayItem(advData, idx);
      idx = (idx+1) % jsvGetArrayLength(advData);
      bleStatus = (bleStatus&~BLE_ADVERTISING_MULTIPLE_MASK) | (idx<<BLE_ADVERTISING_MULTIPLE_SHIFT);
      JSV_GET_AS_CHAR_ARRAY(dPtr, dLen, data);
      if (dPtr && dLen) {
        uint32_t err_code = jsble_advertising_update_advdata(dPtr, dLen);
        if (err_code)
          ok = false; // error setting BLE - disable
      } else {
        // Invalid adv data - disable
        ok = false;
      }
      jsvUnLock(data);
    } else {
      // no advdata - disable multiple advertising
      ok = false;
    }
    if (!ok) {
      bleStatus &= ~(BLE_IS_ADVERTISING_MULTIPLE|BLE_ADVERTISING_MULTIPLE_MASK);
    }
    jsvUnLock(advData);
  }
#ifdef DYNAMIC_INTERVAL_ADJUSTMENT
 // Handle Dynamic Interval Adjustment
 if (bleIdleCounter<BLE_DYNAMIC_INTERVAL_IDLE_TIME) {
   bleIdleCounter++;
 } else {
   if (jsble_has_peripheral_connection() &&
       !(bleStatus & BLE_DISABLE_DYNAMIC_INTERVAL) &&
       bleHighInterval) {
     bleHighInterval = false;
     jsble_set_periph_connection_interval(BLE_DYNAMIC_INTERVAL_LOW_RATE, BLE_DYNAMIC_INTERVAL_LOW_RATE);
   }
 }
#endif

#ifndef NRF52_SERIES
  /* NRF52 has a systick. On nRF51 we just hook on
  to this, since it happens quite often */
  void SysTick_Handler(void);
  SysTick_Handler();
#endif
}

#if PEER_MANAGER_ENABLED
static void ble_update_whitelist() {
  uint32_t err_code;
  if (m_is_wl_changed) {
    // The whitelist has been modified, update it in the Peer Manager.
    err_code = pm_whitelist_set(m_whitelist_peers, m_whitelist_peer_cnt);
    APP_ERROR_CHECK_NOT_URGENT(err_code);

    err_code = pm_device_identities_list_set(m_whitelist_peers, m_whitelist_peer_cnt);
    if (err_code != NRF_ERROR_NOT_SUPPORTED)
      APP_ERROR_CHECK_NOT_URGENT(err_code);

    m_is_wl_changed = false;
  }
}
#endif

/// Function for the application's SoftDevice event handler.
#if NRF_SD_BLE_API_VERSION<5
static void ble_evt_handler(ble_evt_t * p_ble_evt) {
#else
static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context) {
#endif

  /*if (p_ble_evt->header.evt_id != 87 && // ignore write complete (SDK14+?)
      p_ble_evt->header.evt_id != BLE_GATTS_EVT_WRITE && // Write operation performed (eg after we transmit on UART)
      p_ble_evt->header.evt_id != BLE_EVT_TX_COMPLETE)
    jsiConsolePrintf("[%d %d]\n", p_ble_evt->header.evt_id, p_ble_evt->evt.gattc_evt.params.hvx.handle );*/
#if ESPR_BLUETOOTH_ANCS
  if (bleStatus & BLE_ANCS_AMS_OR_CTS_INITED)
    ble_ancs_on_ble_evt(p_ble_evt);
#endif
    uint32_t err_code;


    switch (p_ble_evt->header.evt_id) {
      case BLE_GAP_EVT_TIMEOUT:
#if CENTRAL_LINK_COUNT>0
        if (bleInTask(BLETASK_BONDING)) { // BLE_GAP_TIMEOUT_SRC_SECURITY_REQUEST ?
          jsble_queue_pending(BLEP_TASK_FAIL_CONN_TIMEOUT, 0);
        } else if (bleInTask(BLETASK_CONNECT)) {
          jsble_queue_pending(BLEP_TASK_FAIL_CONN_TIMEOUT, 0);
        } else
#endif
        {
          // the timeout for sd_ble_gap_adv_start expired - kick it off again
          bleStatus &= ~BLE_IS_ADVERTISING; // we still think we're advertising, but we stopped
          jsble_queue_pending(BLEP_ADVERTISING_START, 0); // start advertising again
        }
        break;

#if CENTRAL_LINK_COUNT>0
      case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST:
      {
          const ble_gap_evt_t * const p_gap_evt = &p_ble_evt->evt.gap_evt;
          // Accept parameters requested by peer.
          err_code = sd_ble_gap_conn_param_update(p_gap_evt->conn_handle,
                                      &p_gap_evt->params.conn_param_update_request.conn_params);
          //if (err_code!=NRF_ERROR_INVALID_STATE) APP_ERROR_CHECK_NOT_URGENT(err_code);
          // This sometimes fails with NRF_ERROR_INVALID_STATE if this request
          // comes in between sd_ble_gap_disconnect being called and the DISCONNECT
          // event being received. The SD obviously does the checks for us, so lets
          // avoid crashing because of it!
      } break; // BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST
#endif

      case BLE_GAP_EVT_CONNECTED:
        // set connection transmit power
#if NRF_SD_BLE_API_VERSION > 5
        sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_CONN, p_ble_evt->evt.gap_evt.conn_handle, m_tx_power);
#endif
        if (p_ble_evt->evt.gap_evt.params.connected.role == BLE_GAP_ROLE_PERIPH) {
          m_peripheral_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
#ifdef EXTENSIBLE_MTU
          m_peripheral_effective_mtu = GATT_MTU_SIZE_DEFAULT;
#endif
#ifdef DYNAMIC_INTERVAL_ADJUSTMENT
          bleIdleCounter = 0;
#endif
          if (bleStatus & BLE_IS_RSSI_SCANNING) // attempt to restart RSSI scan
            sd_ble_gap_rssi_start(m_peripheral_conn_handle, 0, 0);
          bleStatus &= ~BLE_IS_SENDING; // reset state - just in case
#if BLE_HIDS_ENABLED
          bleStatus &= ~BLE_IS_SENDING_HID;
#endif
          jsble_queue_pending(BLEP_ADVERTISING_STOP, 0); //  we're not advertising now we're connected
#ifndef SAVE_ON_FLASH
          if (!(bleStatus & BLE_IS_SLEEPING) && (bleStatus & BLE_ADVERTISE_WHEN_CONNECTED))
            jsble_queue_pending(BLEP_ADVERTISING_START, 0); // start advertising again if ADVERTISE_WHEN_CONNECTED
#endif

          if (!jsiIsConsoleDeviceForced() && (bleStatus & BLE_NUS_INITED)) {
            jsiClearInputLine(false); // clear the input line on connect
            jsiSetConsoleDevice(EV_BLUETOOTH, false);
          }
          jsble_queue_pending_buf(BLEP_CONNECTED, 0, (char*)&p_ble_evt->evt.gap_evt.params.connected.peer_addr, sizeof(ble_gap_addr_t));
#ifndef SAVE_ON_FLASH
          m_peripheral_addr = p_ble_evt->evt.gap_evt.params.connected.peer_addr;
#endif
        }
#if CENTRAL_LINK_COUNT>0
        if (p_ble_evt->evt.gap_evt.params.connected.role == BLE_GAP_ROLE_CENTRAL) {
          int centralIdx;
          for (centralIdx=0;centralIdx<CENTRAL_LINK_COUNT;centralIdx++) {
            if (m_central_conn_handles[centralIdx]==BLE_CONN_HANDLE_INVALID) {
              m_central_conn_handles[centralIdx] = p_ble_evt->evt.gap_evt.conn_handle;
              break;
            }
          }
          if (centralIdx==CENTRAL_LINK_COUNT) {
            jsWarn("BLE_GAP_EVT_CONNECTED, but no m_central_conn_handles\n");
            break; // no connection allocated??
          }
#ifdef EXTENSIBLE_MTU
          m_central_effective_mtu = GATT_MTU_SIZE_DEFAULT;
#endif
#if NRF_SD_BLE_API_VERSION>=3
#if (NRF_BLE_MAX_MTU_SIZE > GATT_MTU_SIZE_DEFAULT)
          err_code = sd_ble_gattc_exchange_mtu_request(p_ble_evt->evt.gap_evt.conn_handle, NRF_BLE_MAX_MTU_SIZE);
          APP_ERROR_CHECK_NOT_URGENT(err_code);
#endif
#endif
          jsble_queue_pending(BLEP_TASK_CENTRAL_CONNECTED, centralIdx); // index in m_central_conn_handles
        }
#endif
        break;

      case BLE_GAP_EVT_DISCONNECTED: {

#ifdef DYNAMIC_INTERVAL_ADJUSTMENT
        // set to high interval ready for next connection
        if (!bleHighInterval) {
          bleHighInterval = true;
          /* On NRF_SD_BLE_API_VERSION<5 the interval is remembered between connections, so when we
           * disconnect we need to set it back to default. On later versions it always goes back to
           * what was used with ble_conn_params_init so we don't have to worry. */
#if NRF_SD_BLE_API_VERSION<5
          jsble_set_periph_connection_interval(BLE_DYNAMIC_INTERVAL_HIGH_RATE, BLE_DYNAMIC_INTERVAL_HIGH_RATE);
#endif
        }
#endif

#if PEER_MANAGER_ENABLED
        ble_update_whitelist();
#endif

#if CENTRAL_LINK_COUNT>0
        int centralIdx = jsble_get_central_connection_idx(p_ble_evt->evt.gap_evt.conn_handle);
        if (centralIdx>=0) {
          jsble_queue_pending(BLEP_CENTRAL_DISCONNECTED, p_ble_evt->evt.gap_evt.params.disconnected.reason | (centralIdx<<8));
          m_central_conn_handles[centralIdx] = BLE_CONN_HANDLE_INVALID;

          BleTask task = bleGetCurrentTask();
          if (BLETASK_IS_CENTRAL(task)) {
            jsble_queue_pending(BLEP_TASK_FAIL_DISCONNECTED, 0);
          }
        }
#else
        int centralIdx = -1;
#endif
        if (centralIdx<0) {
          bleStatus &= ~BLE_IS_RSSI_SCANNING; // scanning will have stopped now we're disconnected
          m_peripheral_conn_handle = BLE_CONN_HANDLE_INVALID;
          // if we were on bluetooth and we disconnected, clear the input line so we're fresh next time (#2219)
          if (jsiGetConsoleDevice()==EV_BLUETOOTH) {
            jsiClearInputLine(false);
            if (!jsiIsConsoleDeviceForced())
              jsiSetConsoleDevice(jsiGetPreferredConsoleDevice(), 0);
          }
          // by calling nus_transmit_string here without a connection, we clear the Bluetooth output buffer
          nus_transmit_string();
          // send disconnect event
          jsble_queue_pending(BLEP_DISCONNECTED, p_ble_evt->evt.gap_evt.params.disconnected.reason);
          // restart advertising after disconnection
          if (!(bleStatus & BLE_IS_SLEEPING))
            jsble_queue_pending(BLEP_ADVERTISING_START, 0); // start advertising again
        }
        if ((bleStatus & BLE_NEEDS_SOFTDEVICE_RESTART) && !jsble_has_connection())
          jsble_queue_pending(BLEP_RESTART_SOFTDEVICE, 0);
      } break;

      case BLE_GAP_EVT_RSSI_CHANGED:  {
        int centralIdx = jsble_get_central_connection_idx(p_ble_evt->evt.gap_evt.conn_handle);
#if CENTRAL_LINK_COUNT>0
        if (centralIdx) {
          jsble_queue_pending(BLEP_RSSI_CENTRAL, (p_ble_evt->evt.gap_evt.params.rssi_changed.rssi&255) | (centralIdx<<8));
        }
#endif
        if (centralIdx < 0) {
          jsble_queue_pending(BLEP_RSSI_PERIPH, p_ble_evt->evt.gap_evt.params.rssi_changed.rssi);
        }
      } break;

#if PEER_MANAGER_ENABLED && (NRF_SD_BLE_API_VERSION < 5)
      case BLE_GAP_EVT_SEC_PARAMS_REQUEST: {
        ble_gap_sec_params_t own_params = m_sec_params;
        ble_gap_sec_params_t peer_params = p_ble_evt->evt.gap_evt.params.sec_params_request.peer_params;
        if (!jsble_can_pair_with_peer(&own_params, &peer_params)) {
          // reject security procedure
          ble_gap_sec_params_t sec_params = m_sec_params;
          err_code = sd_ble_gap_sec_params_reply(m_peripheral_conn_handle,
                                                 BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP,
                                                 &sec_params,
                                                 NULL);
          APP_ERROR_CHECK_NOT_URGENT(err_code);
        }
      } break;
#endif

#if PEER_MANAGER_ENABLED==0
      case BLE_GAP_EVT_SEC_PARAMS_REQUEST:{
        //jsiConsolePrintf("BLE_GAP_EVT_SEC_PARAMS_REQUEST\n");
        ble_gap_sec_params_t sec_param = get_gap_sec_params();
        err_code = sd_ble_gap_sec_params_reply(m_peripheral_conn_handle, BLE_GAP_SEC_STATUS_SUCCESS, &sec_param, NULL);
        // or BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP to disable pairing
        APP_ERROR_CHECK_NOT_URGENT(err_code);
      } break; // BLE_GAP_EVT_SEC_PARAMS_REQUEST

      case BLE_GATTS_EVT_SYS_ATTR_MISSING:
        // No system attributes have been stored.
        err_code = sd_ble_gatts_sys_attr_set(m_peripheral_conn_handle, NULL, 0, 0);
        APP_ERROR_CHECK_NOT_URGENT(err_code);
        break;
#endif
#ifdef LINK_SECURITY
      case BLE_GAP_EVT_PASSKEY_DISPLAY:
          jsble_queue_pending_buf(
              BLEP_TASK_PASSKEY_DISPLAY,
              p_ble_evt->evt.gap_evt.conn_handle,
              (char*)p_ble_evt->evt.gap_evt.params.passkey_display.passkey,
              BLE_GAP_PASSKEY_LEN);
          break;
      case BLE_GAP_EVT_AUTH_KEY_REQUEST:
          jsble_queue_pending(BLEP_TASK_AUTH_KEY_REQUEST, p_ble_evt->evt.gap_evt.conn_handle);
          break;
      case BLE_GAP_EVT_LESC_DHKEY_REQUEST: {
        /* Nordic SDK gives us request.p_pk_peer, but it's UNALIGNED! Then the next command
           (taken straight from their SDK) fails with an error because it isn't aligned.
           We have to manually copy the key to a new, aligned value.  */
          ble_gap_lesc_p256_pk_t key;
          memcpy(&key.pk[0], &p_ble_evt->evt.gap_evt.params.lesc_dhkey_request.p_pk_peer->pk[0], sizeof(ble_gap_lesc_p256_pk_t));
          err_code = ecc_p256_shared_secret_compute(&m_lesc_sk.sk[0], &key.pk[0], &m_lesc_dhkey.key[0]);
          APP_ERROR_CHECK_NOT_URGENT(err_code);
          err_code = sd_ble_gap_lesc_dhkey_reply(p_ble_evt->evt.gap_evt.conn_handle, &m_lesc_dhkey);
          APP_ERROR_CHECK_NOT_URGENT(err_code);
          break;
      }
       case BLE_GAP_EVT_AUTH_STATUS:
          jsble_queue_pending_buf(
              BLEP_TASK_AUTH_STATUS,
              p_ble_evt->evt.gap_evt.conn_handle,
              (char*)&p_ble_evt->evt.gap_evt.params.auth_status,
              sizeof(ble_gap_evt_auth_status_t));
          break;
#endif

      case BLE_GATTC_EVT_TIMEOUT:
          // Disconnect on GATT Client timeout event.
          err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                           BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
          APP_ERROR_CHECK_NOT_URGENT(err_code);
          break; // BLE_GATTC_EVT_TIMEOUT

      case BLE_GATTS_EVT_TIMEOUT:
          // Disconnect on GATT Server timeout event.
          err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                           BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
          APP_ERROR_CHECK_NOT_URGENT(err_code);
          break; // BLE_GATTS_EVT_TIMEOUT

      case BLE_EVT_USER_MEM_REQUEST:
          err_code = sd_ble_user_mem_reply(p_ble_evt->evt.gattc_evt.conn_handle, NULL);
          APP_ERROR_CHECK_NOT_URGENT(err_code);
          break; // BLE_EVT_USER_MEM_REQUEST

      case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST:
      {
          ble_gatts_evt_rw_authorize_request_t  req;
          ble_gatts_rw_authorize_reply_params_t auth_reply;

          req = p_ble_evt->evt.gatts_evt.params.authorize_request;

          if (req.type != BLE_GATTS_AUTHORIZE_TYPE_INVALID)
          {
              if ((req.request.write.op == BLE_GATTS_OP_PREP_WRITE_REQ)     ||
                  (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_NOW) ||
                  (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL))
              {
                  if (req.type == BLE_GATTS_AUTHORIZE_TYPE_WRITE)
                  {
                      auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
                  }
                  else
                  {
                      auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
                  }

                  if (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL)
                  {
                      auth_reply.params.write.gatt_status = BLE_GATT_STATUS_SUCCESS;
                  }
                  else
                  {
                      auth_reply.params.write.gatt_status = APP_FEATURE_NOT_SUPPORTED;
                  }
                  err_code = sd_ble_gatts_rw_authorize_reply(p_ble_evt->evt.gatts_evt.conn_handle,
                                                             &auth_reply);
                  // This can return an error when connecting to EQ3 CC-RT-BLE
                  APP_ERROR_CHECK_NOT_URGENT(err_code);
              }
          }
      } break; // BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST

#if (NRF_SD_BLE_API_VERSION >= 3)
#ifdef EXTENSIBLE_MTU
        case BLE_GATTC_EVT_EXCHANGE_MTU_RSP: {
          uint16_t conn_handle   = p_ble_evt->evt.gattc_evt.conn_handle;
          uint16_t effective_mtu = p_ble_evt->evt.gattc_evt.params.exchange_mtu_rsp.server_rx_mtu;
          effective_mtu = MIN(MAX(GATT_MTU_SIZE_DEFAULT,effective_mtu),NRF_BLE_MAX_MTU_SIZE);
#if CENTRAL_LINK_COUNT>0
          int centralIdx = jsble_get_central_connection_idx(conn_handle);
          if (centralIdx >= 0) {
            m_central_effective_mtu = effective_mtu;
#if (NRF_SD_BLE_API_VERSION > 3)
            if (effective_mtu > GATT_MTU_SIZE_DEFAULT) {
              ble_gap_data_length_params_t const dlp =
              {
                  .max_rx_octets =  effective_mtu + 4,
                  .max_tx_octets =  effective_mtu + 4,
              };
              err_code = sd_ble_gap_data_length_update(conn_handle, &dlp, NULL);
              APP_ERROR_CHECK_NOT_URGENT(err_code);
            }
#endif
          }
#endif
          if (m_peripheral_conn_handle == conn_handle){
            m_peripheral_effective_mtu = effective_mtu;
          }
        } break; // BLE_GATTC_EVT_EXCHANGE_MTU_RSP
#endif
      case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST: {
#ifdef EXTENSIBLE_MTU
        uint16_t conn_handle   = p_ble_evt->evt.gatts_evt.conn_handle;
        uint16_t effective_mtu = p_ble_evt->evt.gatts_evt.params.exchange_mtu_request.client_rx_mtu;
        effective_mtu = MIN(MAX(GATT_MTU_SIZE_DEFAULT,effective_mtu),NRF_BLE_MAX_MTU_SIZE);
#if CENTRAL_LINK_COUNT>0
        int centralIdx = jsble_get_central_connection_idx(conn_handle);
        if (centralIdx >= 0) {
          m_central_effective_mtu = effective_mtu;
        }
#endif
        if (m_peripheral_conn_handle == conn_handle){
                 m_peripheral_effective_mtu = effective_mtu;
        }
#endif
        err_code = sd_ble_gatts_exchange_mtu_reply(p_ble_evt->evt.gatts_evt.conn_handle,
                                                   NRF_BLE_MAX_MTU_SIZE);
        // This can return an error when connecting to EQ3 CC-RT-BLE (which requests an MTU of 0!!)
        // Ignore this error as it's non-fatal, just some negotiation.
        APP_ERROR_CHECK_NOT_URGENT(err_code);
      } break; // BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST
#endif
#if (NRF_SD_BLE_API_VERSION >= 4)
          case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST:{
            /* Allow SoftDevice to choose Data Length Update Procedure parameters
            automatically. */
            sd_ble_gap_data_length_update(p_ble_evt->evt.gap_evt.conn_handle, NULL, NULL);
            break;
          }
          case BLE_GAP_EVT_DATA_LENGTH_UPDATE:{
            /* Data Length Update Procedure completed, see
            p_ble_evt->evt.gap_evt.params.data_length_update.effective_params for negotiated
            parameters. */
            break;
          }
#endif
#if (NRF_SD_BLE_API_VERSION >= 5)
          case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
            /* The PHYs requested by the peer can be read from the event parameters:
            p_ble_evt->evt.gap_evt.params.phy_update_request.peer_preferred_phys.
            * Note that the peer's TX correponds to our RX and vice versa. */
            /* Allow SoftDevice to choose PHY Update Procedure parameters automatically. */
            ble_gap_phys_t phys = {BLE_GAP_PHY_AUTO, BLE_GAP_PHY_AUTO};
            sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
            break;
          }
          case BLE_GAP_EVT_PHY_UPDATE: {
            if (p_ble_evt->evt.gap_evt.params.phy_update.status == BLE_HCI_STATUS_CODE_SUCCESS) {
              /* PHY Update Procedure completed, see
              p_ble_evt->evt.gap_evt.params.phy_update.tx_phy and
              p_ble_evt->evt.gap_evt.params.phy_update.rx_phy for the currently active PHYs of
              the link. */
            }
            break;
          }
#endif

#if NRF_SD_BLE_API_VERSION<5
      case BLE_EVT_TX_COMPLETE:
#else
      case BLE_GATTC_EVT_WRITE_CMD_TX_COMPLETE: // Write without Response transmission complete.
      case BLE_GATTS_EVT_HVN_TX_COMPLETE: // Handle Value Notification transmission complete
        // FIXME: was just BLE_EVT_TX_COMPLETE - do we now get called twice in some cases?
#endif
      {
        // BLE transmit finished - reset flags
#if CENTRAL_LINK_COUNT>0
        int centralIdx = jsble_get_central_connection_idx(p_ble_evt->evt.common_evt.conn_handle);
        if (centralIdx>=0) {
          if (bleInTask(BLETASK_CHARACTERISTIC_WRITE))
            jsble_queue_pending(BLEP_TASK_CHARACTERISTIC_WRITE, 0);
        }
#endif
        if (p_ble_evt->evt.common_evt.conn_handle == m_peripheral_conn_handle) {
          jsble_peripheral_activity(); // flag that we've been busy
          //TODO: probably want to figure out *which write* finished?
          bleStatus &= ~BLE_IS_SENDING;
#if NRF_SD_BLE_API_VERSION>=5
          if (bleStatus & BLE_NUS_INITED) // push more UART data out if we can
            nus_transmit_string();
#endif

#if BLE_HIDS_ENABLED
          if (bleStatus & BLE_IS_SENDING_HID) {
            // If we could tell which characteristic this was for
            // then we could check m_hids.inp_rep_array[HID_INPUT_REPORT_KEYS_INDEX].char_handles.value_handle
            // ... but it seems we can't!
            bleStatus &= ~BLE_IS_SENDING_HID;
            jsble_queue_pending(BLEP_HID_SENT, 0);
          }
#endif
        }
      } break;

      case BLE_GAP_EVT_ADV_REPORT: {
        // Advertising data received
        const ble_gap_evt_adv_report_t *p_adv = &p_ble_evt->evt.gap_evt.params.adv_report;
        BLEAdvReportData adv;
        adv.peer_addr = p_adv->peer_addr;
        adv.rssi = p_adv->rssi;
#if NRF_SD_BLE_API_VERSION<6
        adv.dlen = p_adv->dlen;
        memcpy(adv.data, p_adv->data, adv.dlen);
#else
        adv.dlen = p_adv->data.len;
        memcpy(adv.data, p_adv->data.p_data, adv.dlen);
#endif
        size_t len = sizeof(BLEAdvReportData) + adv.dlen - BLE_GAP_ADV_MAX_SIZE;
        jsble_queue_pending_buf(BLEP_ADV_REPORT, 0, (char*)&adv, len);
#if NRF_SD_BLE_API_VERSION>5
        // On new APIs we need to continue scanning
        err_code = sd_ble_gap_scan_start(NULL, &m_scan_buffer);
        APP_ERROR_CHECK(err_code);
#endif
        break;
        }

      case BLE_GATTS_EVT_WRITE: {
        // Peripheral's Characteristic was written to
        const ble_gatts_evt_write_t * p_evt_write = &p_ble_evt->evt.gatts_evt.params.write;
        // TODO: detect if this was a nus write. If so, DO NOT create an event for it!
        // We got a param write event - add this to the bluetooth event queue
        jsble_queue_pending_buf(BLEP_WRITE, p_evt_write->handle, (char*)p_evt_write->data, p_evt_write->len);
        jsble_peripheral_activity(); // flag that we've been busy
        break;
      }

#if CENTRAL_LINK_COUNT>0
      // For discovery....
      case BLE_GATTC_EVT_PRIM_SRVC_DISC_RSP: if (bleInTask(BLETASK_PRIMARYSERVICE)) {
        bool done = true;
        if (p_ble_evt->evt.gattc_evt.gatt_status == BLE_GATT_STATUS_SUCCESS &&
            p_ble_evt->evt.gattc_evt.params.prim_srvc_disc_rsp.count!=0) {
          int i;
          // Should actually return 'BLEService' object here
          for (i=0;i<p_ble_evt->evt.gattc_evt.params.prim_srvc_disc_rsp.count;i++) {
            const ble_gattc_service_t *p_srv = &p_ble_evt->evt.gattc_evt.params.prim_srvc_disc_rsp.services[i];
            // filter based on bleUUIDFilter if it's not invalid
            if (bleUUIDFilter.type != BLE_UUID_TYPE_UNKNOWN)
              if (!bleUUIDEqual(p_srv->uuid, bleUUIDFilter)) continue;
            jsble_queue_pending_buf(BLEP_TASK_DISCOVER_SERVICE, 0, (char*)p_srv, sizeof(ble_gattc_service_t));
          }

          uint16_t last = p_ble_evt->evt.gattc_evt.params.prim_srvc_disc_rsp.count-1;
          if (p_ble_evt->evt.gattc_evt.params.prim_srvc_disc_rsp.services[last].handle_range.end_handle < 0xFFFF) {
            // Now try again
            uint16_t start_handle = p_ble_evt->evt.gattc_evt.params.prim_srvc_disc_rsp.services[last].handle_range.end_handle+1;
            done = sd_ble_gattc_primary_services_discover(p_ble_evt->evt.gap_evt.conn_handle, start_handle, NULL) != NRF_SUCCESS;;
          }
        }
        if (done)
          jsble_queue_pending(BLEP_TASK_DISCOVER_SERVICE_COMPLETE, 0);
      } break;
      case BLE_GATTC_EVT_CHAR_DISC_RSP: if (bleInTask(BLETASK_CHARACTERISTIC)) {
        bool done = true;

        if (p_ble_evt->evt.gattc_evt.gatt_status == BLE_GATT_STATUS_SUCCESS &&
            p_ble_evt->evt.gattc_evt.params.char_disc_rsp.count!=0) {
          int i;
          for (i=0;i<p_ble_evt->evt.gattc_evt.params.char_disc_rsp.count;i++) {
            const ble_gattc_char_t *p_chr = &p_ble_evt->evt.gattc_evt.params.char_disc_rsp.chars[i];
            // filter based on bleUUIDFilter if it's not invalid
            if (bleUUIDFilter.type != BLE_UUID_TYPE_UNKNOWN)
              if (!bleUUIDEqual(p_chr->uuid, bleUUIDFilter)) continue;
            jsble_queue_pending_buf(BLEP_TASK_DISCOVER_CHARACTERISTIC, 0, (char*)p_chr, sizeof(ble_gattc_char_t));
          }

          uint16_t last = p_ble_evt->evt.gattc_evt.params.char_disc_rsp.count-1;
          if (p_ble_evt->evt.gattc_evt.params.char_disc_rsp.chars[last].handle_value < bleFinalHandle) {
            // Now try again
            uint16_t start_handle = p_ble_evt->evt.gattc_evt.params.char_disc_rsp.chars[last].handle_value+1;
            ble_gattc_handle_range_t range;
            range.start_handle = start_handle;
            range.end_handle = bleFinalHandle;

            /* Might report an error for invalid handle (we have no way to know for the last characteristic
             * in the last service it seems). If it does, we're sorted */
            done = sd_ble_gattc_characteristics_discover(p_ble_evt->evt.gap_evt.conn_handle, &range) != NRF_SUCCESS;
          }
        }


        if (done)
          jsble_queue_pending(BLEP_TASK_DISCOVER_CHARACTERISTIC_COMPLETE, 0);
      } break;
      case BLE_GATTC_EVT_DESC_DISC_RSP: if (bleInTask(BLETASK_CHARACTERISTIC_DESC_AND_STARTNOTIFY)) {
        // trigger this with sd_ble_gattc_descriptors_discover(conn_handle, &handle_range);
        uint16_t cccd_handle = 0;
        const ble_gattc_evt_desc_disc_rsp_t * p_desc_disc_rsp_evt = &p_ble_evt->evt.gattc_evt.params.desc_disc_rsp;
        if (p_ble_evt->evt.gattc_evt.gatt_status == BLE_GATT_STATUS_SUCCESS) {
          // The descriptor was found at the peer.
          // If the descriptor was a CCCD, then the cccd_handle needs to be populated.
          uint32_t i;
          // Loop through all the descriptors to find the CCCD.
          for (i = 0; i < p_desc_disc_rsp_evt->count; i++) {
            if (p_desc_disc_rsp_evt->descs[i].uuid.uuid ==
                BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG) {
              cccd_handle = p_desc_disc_rsp_evt->descs[i].handle;
            }
          }
        }

        jsble_queue_pending(BLEP_TASK_DISCOVER_CCCD, cccd_handle);
      } break;

      case BLE_GATTC_EVT_READ_RSP: if (bleInTask(BLETASK_CHARACTERISTIC_READ)) { // ignore read responses if not in a READ task (ANCS/AMS/CTS/etc can create READ_RSP events)
        const ble_gattc_evt_read_rsp_t *p_read = &p_ble_evt->evt.gattc_evt.params.read_rsp;
        jsble_queue_pending_buf(BLEP_TASK_CHARACTERISTIC_READ, 0, (char*)&p_read->data[0], p_read->len);
      } break;

      case BLE_GATTC_EVT_WRITE_RSP: {
        if (bleInTask(BLETASK_CHARACTERISTIC_NOTIFY))
          jsble_queue_pending(BLEP_TASK_CHARACTERISTIC_NOTIFY, 0);
        else if (bleInTask(BLETASK_CHARACTERISTIC_WRITE))
          jsble_queue_pending(BLEP_TASK_CHARACTERISTIC_WRITE, 0);
      } break;

      case BLE_GATTC_EVT_HVX: {
        // Notification/Indication
        const ble_gattc_evt_hvx_t *p_hvx = &p_ble_evt->evt.gattc_evt.params.hvx;
        // p_hvx->type is BLE_GATT_HVX_NOTIFICATION or BLE_GATT_HVX_INDICATION
        jsble_queue_pending_buf(BLEP_CENTRAL_NOTIFICATION, p_hvx->handle, (char*)p_hvx->data, p_hvx->len);
        if (p_hvx->type == BLE_GATT_HVX_INDICATION) {
          sd_ble_gattc_hv_confirm(p_ble_evt->evt.gattc_evt.conn_handle, p_hvx->handle);
        }
      } break;
#endif

      default:
          // No implementation needed.
          break;
    }
}


#ifdef USE_NFC
/// Callback function for handling NFC events.
static void nfc_callback(void * p_context, hal_nfc_event_t event, const uint8_t * p_data, size_t data_length) {
  (void)p_context;

  switch (event) {
    case HAL_NFC_EVENT_FIELD_ON:
      jsble_queue_pending(BLEP_NFC_STATUS,1);
      break;
    case HAL_NFC_EVENT_FIELD_OFF:
      jsble_queue_pending(BLEP_NFC_STATUS,0);
      break;
    case HAL_NFC_EVENT_DATA_RECEIVED: {
      jsble_queue_pending_buf(BLEP_NFC_RX, 0, (char*)p_data, data_length);
      break;
    }
    case HAL_NFC_EVENT_DATA_TRANSMITTED:
      jsble_queue_pending(BLEP_NFC_TX,0);
      break;
    default:
      break;
  }
}
#endif

#if NRF_SD_BLE_API_VERSION<5
/// Function for dispatching a SoftDevice event to all modules with a SoftDevice event handler.
static void ble_evt_dispatch(ble_evt_t * p_ble_evt) {
#if PEER_MANAGER_ENABLED
  ble_conn_state_on_ble_evt(p_ble_evt);
  pm_on_ble_evt(p_ble_evt);
#endif
  if (!((p_ble_evt->header.evt_id==BLE_GAP_EVT_CONNECTED) &&
        (p_ble_evt->evt.gap_evt.params.connected.role != BLE_GAP_ROLE_PERIPH)) &&
      !((p_ble_evt->header.evt_id==BLE_GAP_EVT_DISCONNECTED) &&
         m_peripheral_conn_handle != p_ble_evt->evt.gap_evt.conn_handle)) {
    // Stuff in here should ONLY get called for Peripheral events (not central)
    ble_conn_params_on_ble_evt(p_ble_evt);
    if (bleStatus & BLE_NUS_INITED)
      ble_nus_on_ble_evt(&m_nus, p_ble_evt);
  }
#if BLE_HIDS_ENABLED
  if (bleStatus & BLE_HID_INITED)
    ble_hids_on_ble_evt(&m_hids, p_ble_evt);
#endif
  ble_evt_handler(p_ble_evt);
}
#endif

/// Function for dispatching a system event to interested modules.
#if NRF_SD_BLE_API_VERSION<5
static void soc_evt_handler(uint32_t sys_evt) {
#if PEER_MANAGER_ENABLED
  // Dispatch the system event to the fstorage module, where it will be
  // dispatched to the Flash Data Storage (FDS) module.
  fs_sys_event_handler(sys_evt);
#endif
#else // NRF_SD_BLE_API_VERSION>=5
static void soc_evt_handler(uint32_t sys_evt, void * p_context) {
#endif
  void jsh_sys_evt_handler(uint32_t sys_evt);
  jsh_sys_evt_handler(sys_evt);
}

#if PEER_MANAGER_ENABLED
/**@brief Function for handling File Data Storage events.
 *
 * @param[in] p_evt  Peer Manager event.
 * @param[in] cmd
 */
static void fds_evt_handler(fds_evt_t const * const p_evt)
{
    if (p_evt->id == FDS_EVT_GC)
    {
        //NRF_LOG_DEBUG("GC completed\n");
    }
}


/// Function for handling Peer Manager events.
static void pm_evt_handler(pm_evt_t const * p_evt) {
    ret_code_t err_code;

    //jsiConsolePrintf("PM [%d]\n", p_evt->evt_id );
    switch (p_evt->evt_id)
    {
        case PM_EVT_BONDED_PEER_CONNECTED:
        {
            //NRF_LOG_DEBUG("Connected to previously bonded device\r\n");
            m_peer_id = p_evt->peer_id;
            err_code  = pm_peer_rank_highest(p_evt->peer_id);
            if (err_code != NRF_ERROR_BUSY)
            {
              APP_ERROR_CHECK_NOT_URGENT(err_code);
            }
        } break;

        case PM_EVT_CONN_SEC_START: {
          jsble_queue_pending(BLEP_BONDING_STATUS, BLE_BOND_START);
        } break;

        case PM_EVT_CONN_SEC_SUCCEEDED:
        {
            jsble_queue_pending(BLEP_BONDING_STATUS, BLE_BOND_SUCCESS);
            /*NRF_LOG_DEBUG("Link secured. Role: %d. conn_handle: %d, Procedure: %d\r\n",
                                 -1, // ble_conn_state_role(p_evt->conn_handle)
                                 p_evt->conn_handle,
                                 p_evt->params.conn_sec_succeeded.procedure);*/
            m_peer_id = p_evt->peer_id;
            err_code  = pm_peer_rank_highest(p_evt->peer_id);
            if (err_code != NRF_ERROR_BUSY)
            {
              APP_ERROR_CHECK_NOT_URGENT(err_code);
            }
            if (
#if NRF_SD_BLE_API_VERSION>5
                p_evt->params.conn_sec_succeeded.procedure == PM_CONN_SEC_PROCEDURE_ENCRYPTION &&
#else
                p_evt->params.conn_sec_succeeded.procedure == PM_LINK_SECURED_PROCEDURE_BONDING &&
#endif
                bleStatus & BLE_WHITELIST_ON_BOND)
            {
                /*NRF_LOG_DEBUG("New Bond, add the peer to the whitelist if possible\r\n");
                NRF_LOG_DEBUG("\tm_whitelist_peer_cnt %d, MAX_PEERS_WLIST %d\r\n",
                               m_whitelist_peer_cnt + 1,
                               BLE_GAP_WHITELIST_ADDR_MAX_COUNT);*/
                if (m_whitelist_peer_cnt < BLE_GAP_WHITELIST_ADDR_MAX_COUNT)
                {
                    //bonded to a new peer, add it to the whitelist.
                    // but first check it's not in there already!
                    uint32_t i;
                    bool found = false;
                    for (i=0;i<m_whitelist_peer_cnt;i++)
                      if (m_whitelist_peers[i]==m_peer_id)
                        found = true;
                    // not in already, so add it!
                    if (!found) {
                      m_whitelist_peers[m_whitelist_peer_cnt++] = m_peer_id;
                      m_is_wl_changed = true;
                    }
                }
                //Note: This code will use the older bonded device in the white list and not add any newer bonded to it
                //      You should check on what kind of white list policy your application should use.
            }
#if ESPR_BLUETOOTH_ANCS
            if (bleStatus & BLE_ANCS_AMS_OR_CTS_INITED)
              ble_ancs_bonding_succeeded(p_evt->conn_handle);
#endif

        } break;

        case PM_EVT_CONN_SEC_FAILED:
        {
          jsble_queue_pending(BLEP_BONDING_STATUS, BLE_BOND_FAIL);
          /** In some cases, when securing fails, it can be restarted directly. Sometimes it can
           *  be restarted, but only after changing some Security Parameters. Sometimes, it cannot
           *  be restarted until the link is disconnected and reconnected. Sometimes it is
           *  impossible, to secure the link, or the peer device does not support it. How to
           *  handle this error is highly application dependent. */
          switch (p_evt->params.conn_sec_failed.error)
          {
              case PM_CONN_SEC_ERROR_PIN_OR_KEY_MISSING:
                  // Rebond if one party has lost its keys.
                  err_code = pm_conn_secure(p_evt->conn_handle, true);
                  if (err_code != NRF_ERROR_INVALID_STATE)
                  {
                    APP_ERROR_CHECK_NOT_URGENT(err_code);
                  }
                  break; // PM_CONN_SEC_ERROR_PIN_OR_KEY_MISSING

              default:
                  break;
          }
        } break;

        case PM_EVT_CONN_SEC_CONFIG_REQ:
        {
            // Reject pairing request from an already bonded peer.
            // Still allow a device to pair if it doesn't have bonding info for us
            /* TODO: we could turn this off with a flag? Stops someone reconnecting
             * by spoofing a peer. */
            pm_conn_sec_config_t conn_sec_config = {.allow_repairing = true };
            pm_conn_sec_config_reply(p_evt->conn_handle, &conn_sec_config);
        } break;

#if (NRF_SD_BLE_API_VERSION >= 5)
        case PM_EVT_CONN_SEC_PARAMS_REQ: {
            ble_gap_sec_params_t own_params = m_sec_params;
            const ble_gap_sec_params_t *peer_params = p_evt->params.conn_sec_params_req.p_peer_params;
            if (!jsble_can_pair_with_peer(&own_params, peer_params)) {
                // reject security procedure
                err_code = pm_conn_sec_params_reply(p_evt->conn_handle, NULL,
                                                    p_evt->params.conn_sec_params_req.p_context);
                APP_ERROR_CHECK_NOT_URGENT(err_code);
            }
        } break;
#endif

        case PM_EVT_STORAGE_FULL:
        {
            jsWarn("PM: PM_EVT_STORAGE_FULL - running garbage collection");
            // Run garbage collection on the flash.
            err_code = fds_gc();
            jsWarn("Garbage collection result: %d", err_code);
            if (err_code == FDS_ERR_BUSY || err_code == FDS_ERR_NO_SPACE_IN_QUEUES)
            {
                // Retry.
            }
            else
            {
              APP_ERROR_CHECK_NOT_URGENT(err_code);
            }
        } break;

        case PM_EVT_ERROR_UNEXPECTED:
            // Assert.
            jsWarn("PM: PM_EVT_ERROR_UNEXPECTED %d", p_evt->params.error_unexpected.error);
            //APP_ERROR_CHECK(p_evt->params.error_unexpected.error);
            break;

        case PM_EVT_PEER_DATA_UPDATE_SUCCEEDED:
            break;

        case PM_EVT_PEER_DATA_UPDATE_FAILED:
          // Used to assert here
            jsWarn("PM: DATA_UPDATE_FAILED");
            break;

        case PM_EVT_PEER_DELETE_SUCCEEDED:
            break;

        case PM_EVT_PEER_DELETE_FAILED:
            // Assert.
            jsWarn("PM: PM_EVT_PEER_DELETE_FAILED %d", p_evt->params.peer_delete_failed.error);
            break;

        case PM_EVT_PEERS_DELETE_SUCCEEDED:
            if (!(bleStatus & BLE_IS_SLEEPING))
              jsble_queue_pending(BLEP_ADVERTISING_START, 0); // start advertising again if not asleep
            break;

        case PM_EVT_PEERS_DELETE_FAILED:
            // Assert.
            jsWarn("PM: PM_EVT_PEERS_DELETE_FAILED %d", p_evt->params.peers_delete_failed_evt.error);
            //APP_ERROR_CHECK(p_evt->params.peers_delete_failed_evt.error);
            break;

        case PM_EVT_LOCAL_DB_CACHE_APPLIED:
            break;

        case PM_EVT_LOCAL_DB_CACHE_APPLY_FAILED:
            // The local database has likely changed, send service changed indications.
            pm_local_database_has_changed();
            break;

        case PM_EVT_SERVICE_CHANGED_IND_SENT:
        case PM_EVT_SERVICE_CHANGED_IND_CONFIRMED:
            break;

        default:
            // No implementation needed.
            break;
    }
}
#endif


#if BLE_HIDS_ENABLED
/// Function for handling the HID Report Characteristic Write event.
static void on_hid_rep_char_write(ble_hids_evt_t * p_evt) {
    if (p_evt->params.char_write.char_id.rep_type == BLE_HIDS_REP_TYPE_OUTPUT){
        uint32_t err_code;
        uint8_t  report_val;
        uint8_t  report_index = p_evt->params.char_write.char_id.rep_index;

        if (report_index == HID_OUTPUT_REPORT_INDEX) {
            // This code assumes that the output report is one byte long. Hence the following
            // static assert is made.
            STATIC_ASSERT(HID_OUTPUT_REPORT_MAX_LEN == 1);

            err_code = ble_hids_outp_rep_get(&m_hids,
                                             report_index,
                                             HID_OUTPUT_REPORT_MAX_LEN,
                                             0,
#if NRF_SD_BLE_API_VERSION>5
                                             m_peripheral_conn_handle,
#endif
                                             &report_val);
            APP_ERROR_CHECK_NOT_URGENT(err_code);
            // (report_val & 2) is caps lock
            jsble_queue_pending(BLEP_HID_VALUE, report_val);
        }
    }
}

/// Function for handling HID events.
static void on_hids_evt(ble_hids_t * p_hids, ble_hids_evt_t * p_evt) {
    switch (p_evt->evt_type)
    {
        case BLE_HIDS_EVT_BOOT_MODE_ENTERED:
            m_in_boot_mode = true;
            break;

        case BLE_HIDS_EVT_REPORT_MODE_ENTERED:
            m_in_boot_mode = false;
            break;

        case BLE_HIDS_EVT_REP_CHAR_WRITE:
            on_hid_rep_char_write(p_evt);
            break;

        case BLE_HIDS_EVT_NOTIF_ENABLED:
            break;

        default:
            // No implementation needed.
            break;
    }
}
#endif

// -----------------------------------------------------------------------------------
// -------------------------------------------------------------------- INITIALISATION

static void gap_params_init() {
    uint32_t                err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    char deviceName[BLE_GAP_DEVNAME_MAX_LEN];
#if defined(BLUETOOTH_NAME_PREFIX)
    strcpy(deviceName,BLUETOOTH_NAME_PREFIX);
#else
    strcpy(deviceName,"Espruino "PC_BOARD_ID);
#endif

    size_t len = strlen(deviceName);
#if defined(BLUETOOTH_NAME_PREFIX)
    // append last 2 bytes of MAC address to name
    uint32_t addr =  NRF_FICR->DEVICEADDR[0];
    deviceName[len++] = ' ';
    deviceName[len++] = itoch((addr>>12)&15);
    deviceName[len++] = itoch((addr>>8)&15);
    deviceName[len++] = itoch((addr>>4)&15);
    deviceName[len++] = itoch((addr)&15);
    // not null terminated
#endif

    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&sec_mode); // don't allow device name change via BLE
    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (const uint8_t *)deviceName,
                                          len);
    APP_ERROR_CHECK(err_code);

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    BLEFlags flags = jsvGetIntegerAndUnLock(jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_FLAGS));
    if (flags & BLE_FLAGS_LOW_POWER) {
      gap_conn_params.min_conn_interval = MSEC_TO_UNITS(500, UNIT_1_25_MS);   // Minimum acceptable connection interval (500 ms)
      gap_conn_params.max_conn_interval = MSEC_TO_UNITS(1000, UNIT_1_25_MS);    // Maximum acceptable connection interval (1000 ms)
    } else {
      gap_conn_params.min_conn_interval = MSEC_TO_UNITS(7.5, UNIT_1_25_MS);   // Minimum acceptable connection interval (7.5 ms)
      gap_conn_params.max_conn_interval = MSEC_TO_UNITS(DEFAULT_PERIPH_MAX_CONN_INTERVAL, UNIT_1_25_MS);    // Maximum acceptable connection interval (20 ms)
    }
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;
#ifdef DYNAMIC_INTERVAL_ADJUSTMENT
    bleHighInterval = BLE_DEFAULT_HIGH_INTERVAL; // set default speed
#endif

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}

static uint32_t radio_notification_init(uint32_t irq_priority, uint8_t notification_type, uint8_t notification_distance) {
    uint32_t err_code;

    err_code = sd_nvic_ClearPendingIRQ(SWI1_IRQn);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    err_code = sd_nvic_SetPriority(SWI1_IRQn, irq_priority);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    err_code = sd_nvic_EnableIRQ(SWI1_IRQn);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    // Configure the event
    return sd_radio_notification_cfg_set(notification_type, notification_distance);
}

static ble_gap_sec_params_t get_gap_sec_params() {
  ble_gap_sec_params_t sec_param;
  memset(&sec_param, 0, sizeof(ble_gap_sec_params_t));

  // Security parameters to be used for all security procedures.
  // All set to 0 beforehand, so
  sec_param.bond           = 1;                     /**< Perform bonding. */
  //sec_param.mitm           = 0;                     /**< Man In The Middle protection not required. */
  //sec_param.lesc           = 0;                     /**< LE Secure Connections not enabled. */
  //sec_param.keypress       = 0;                     /**< Keypress notifications not enabled. */
  sec_param.io_caps        = BLE_GAP_IO_CAPS_NONE;  /**< No I/O capabilities. */
  //sec_param.oob            = 0;                     /**< Out Of Band data not available. */
  sec_param.min_key_size   = 7;                     /**< Minimum encryption key size. */
  sec_param.max_key_size   = 16;                    /**< Maximum encryption key size. */
#if PEER_MANAGER_ENABLED
  sec_param.kdist_own.enc  = 1;
  sec_param.kdist_own.id   = 1;
  sec_param.kdist_peer.enc = 1;
  sec_param.kdist_peer.id  = 1;
#else
  // LE Secure Connections were enabled if we didn't have peer manager
  // Don't think this is valid, and it only would have appeared on Micro:bit
  // sec_param.lesc           = 1;
#endif

  JsVar *options = jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_SECURITY);
  if (jsvIsObject(options)) {
    bool display = jsvObjectGetBoolChild(options, "display");
    bool keyboard = jsvObjectGetBoolChild(options, "keyboard");
    if (display && keyboard) sec_param.io_caps = BLE_GAP_IO_CAPS_KEYBOARD_DISPLAY;
    else if (display) sec_param.io_caps = BLE_GAP_IO_CAPS_DISPLAY_ONLY;
    else if (keyboard) sec_param.io_caps = BLE_GAP_IO_CAPS_KEYBOARD_ONLY;
    JsVar *v;
    v = jsvObjectGetChildIfExists(options, "bond");
    if (!jsvIsUndefined(v) && !jsvGetBool(v)) sec_param.bond=0;
    jsvUnLock(v);
    if (jsvObjectGetBoolChild(options, "mitm")) sec_param.mitm=1;
    if (jsvObjectGetBoolChild(options, "lesc")) sec_param.lesc=1;
    if (jsvObjectGetBoolChild(options, "oob")) sec_param.oob=1;
  }
  return sec_param;
}

#if PEER_MANAGER_ENABLED
static bool jsble_can_pair_with_peer(const ble_gap_sec_params_t *own_params, const ble_gap_sec_params_t *peer_params) {
  if (bleStatus & BLE_IS_NOT_PAIRABLE) {
    // reject all security procedures to prevent any peer from pairing with us
    return false;
  } else {
    if (own_params && peer_params) {
      // reject security procedure if we want mitm protection, but this is impossible with the current peer
      bool lesc = own_params->lesc == 1 && peer_params->lesc == 1;
      bool useOob = lesc ?
                    (own_params->oob == 1 || peer_params->oob == 1) :
                    (own_params->oob == 1 && peer_params->oob == 1);
      bool hasKeyboard = own_params->io_caps == BLE_GAP_IO_CAPS_KEYBOARD_ONLY ||
                         own_params->io_caps == BLE_GAP_IO_CAPS_KEYBOARD_DISPLAY;
      bool authenticated = false;
      switch (peer_params->io_caps) {
        case BLE_GAP_IO_CAPS_DISPLAY_ONLY:
          authenticated = hasKeyboard;
          break;
        case BLE_GAP_IO_CAPS_DISPLAY_YESNO:
          authenticated = lesc ?
                          (hasKeyboard || own_params->io_caps == BLE_GAP_IO_CAPS_DISPLAY_YESNO) :
                          hasKeyboard;
          break;
        case BLE_GAP_IO_CAPS_KEYBOARD_ONLY:
          authenticated = own_params->io_caps != BLE_GAP_IO_CAPS_NONE;
          break;
        case BLE_GAP_IO_CAPS_NONE:
          authenticated = false;
          break;
        case BLE_GAP_IO_CAPS_KEYBOARD_DISPLAY:
          authenticated = own_params->io_caps != BLE_GAP_IO_CAPS_NONE;
          break;
        default:
          authenticated = false;
          break;
      }
      if (!useOob && own_params->mitm == 1 && !authenticated) {
        // reject security procedure
        return false;
      }
    }
  }
  return true;
}
#endif

void jsble_update_security() {
#if PEER_MANAGER_ENABLED
  bool encryptUart = false;
  bool mitmProtect = false;
  ble_gap_sec_params_t sec_param = get_gap_sec_params();
  m_sec_params = sec_param;
  // encrypt UART and require mitm protection for out of band pairing
  if (sec_param.oob) {
    encryptUart = true;
    mitmProtect = true;
  }
  // if mitm protection is requested, we also need to encrypt the UART
  if (sec_param.mitm) {
    encryptUart = true;
    mitmProtect = true;
  }


  uint32_t err_code = pm_sec_params_set(&sec_param);
  jsble_check_error(err_code);

  JsVar *options = jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_SECURITY);
  if (jsvIsObject(options)) {
    JsVar *v;
    if (jsvObjectGetBoolChild(options, "encryptUart"))
      encryptUart = true;
    {
      JsVar *pair;
      pair = jsvObjectGetChildIfExists(options, "pair");
      if (!jsvIsUndefined(pair) && !jsvGetBool(pair)) {
        bleStatus |= BLE_IS_NOT_PAIRABLE;
      } else {
        bleStatus &= ~BLE_IS_NOT_PAIRABLE;
      }
      jsvUnLock(pair);
    }
    // Check for passkey
    uint8_t passkey[BLE_GAP_PASSKEY_LEN+1];
    memset(passkey, 0, sizeof(passkey));
    v = jsvObjectGetChildIfExists(options, "passkey");
    if (jsvIsString(v)) jsvGetString(v, (char*)passkey, sizeof(passkey));
    jsvUnLock(v);
    //jsiConsolePrintf("PASSKEY %d %d %d %d %d %d\n",passkey[0],passkey[1],passkey[2],passkey[3],passkey[4],passkey[5]);
    ble_opt_t pin_option;
    pin_option.gap_opt.passkey.p_passkey = NULL;
    if (passkey[0]) {
      pin_option.gap_opt.passkey.p_passkey = passkey;
      encryptUart = true;
      mitmProtect = true;
    }
    uint32_t err_code =  sd_ble_opt_set(BLE_GAP_OPT_PASSKEY, &pin_option);
    jsble_check_error(err_code);
  }
  // If UART encryption or mitm protection status changed, we need to update flags and restart Bluetooth
  if (((bleStatus & BLE_ENCRYPT_UART) != 0) != encryptUart || ((bleStatus & BLE_SECURITY_MITM) != 0) != mitmProtect) {
    if (encryptUart) bleStatus |= BLE_ENCRYPT_UART;
    else bleStatus &= ~BLE_ENCRYPT_UART;
    if (mitmProtect) bleStatus |= BLE_SECURITY_MITM;
    else bleStatus &= ~BLE_SECURITY_MITM;
    // But only restart if the UART was enabled
    if (bleStatus & BLE_NUS_INITED)
      bleStatus |= BLE_NEEDS_SOFTDEVICE_RESTART;
  }
#endif
}

#if PEER_MANAGER_ENABLED

/**@brief Fetch the list of peer manager peer IDs.
 *
 * @param[inout] p_peers   The buffer where to store the list of peer IDs.
 * @param[inout] p_size    In: The size of the @p p_peers buffer.
 *                         Out: The number of peers copied in the buffer.
 */
static void peer_list_get(pm_peer_id_t * p_peers, uint32_t * p_size)
{
    pm_peer_id_t peer_id;
    uint32_t     peers_to_copy;

    peers_to_copy = (*p_size < BLE_GAP_WHITELIST_ADDR_MAX_COUNT) ?
                     *p_size : BLE_GAP_WHITELIST_ADDR_MAX_COUNT;

    peer_id = pm_next_peer_id_get(PM_PEER_ID_INVALID);
    *p_size = 0;

    while ((peer_id != PM_PEER_ID_INVALID) && (peers_to_copy--))
    {
        p_peers[(*p_size)++] = peer_id;
        peer_id = pm_next_peer_id_get(peer_id);
    }
}

static uint32_t flash_end_addr(void)
{
#if NRF_SD_BLE_API_VERSION>=5
// Copied from fds.c because this isn't exported :(
    uint32_t const bootloader_addr = NRF_UICR->NRFFW[0];
    uint32_t const page_sz         = NRF_FICR->CODEPAGESIZE;
    uint32_t const code_sz         = NRF_FICR->CODESIZE;
    return (bootloader_addr != 0xFFFFFFFF) ? bootloader_addr : (code_sz * page_sz);
#else
    return (uint32_t)FS_PAGE_END_ADDR;
#endif
}
static void peer_manager_erase_pages(){
    int i;
    for (i=1;i<=FDS_PHY_PAGES;i++)
      jshFlashErasePage((flash_end_addr()) - i*FDS_PHY_PAGE_SIZE);
}
static void peer_manager_init(bool erase_bonds) {

  /* Only initialise the peer manager once. This stops
   * crashes caused by repeated SD restarts (jsble_restart_softdevice) */
  if (bleStatus & BLE_PM_INITIALISED) return;
  bleStatus |= BLE_PM_INITIALISED;

  /* If ALL buttons are pressed at boot, clear out flash
   pages as well. Nice easy way to reset!

   We know this only happens at boot because of the
   BLE_PM_INITIALISED check above.
  */
  bool buttonPressed = false;
#ifdef BTN1_PININDEX
  buttonPressed = jshPinGetValue(BTN1_PININDEX) == BTN1_ONSTATE;
#endif
#ifdef BTN2_PININDEX
  buttonPressed &= jshPinGetValue(BTN2_PININDEX) == BTN2_ONSTATE;
#endif
#ifdef BTN3_PININDEX
  buttonPressed &= jshPinGetValue(BTN3_PININDEX) == BTN3_ONSTATE;
#endif
#ifdef BTN4_PININDEX
  buttonPressed &= jshPinGetValue(BTN4_PININDEX) == BTN4_ONSTATE;
#endif
  if (buttonPressed) {
    peer_manager_erase_pages();
  }

  ret_code_t           err_code;

  err_code = pm_init();
  /* If pm init failed, erase pm/fds storage to prevent reboot loop
   * This can happen if fds storage is full */
  if (err_code != NRF_SUCCESS){
    peer_manager_erase_pages();
    err_code = pm_init();
  }
  APP_ERROR_CHECK(err_code);

  if (erase_bonds)
  {
      err_code = pm_peers_delete();
      APP_ERROR_CHECK_NOT_URGENT(err_code);
  }

  jsble_update_security(); // pm_sec_params_set

  err_code = pm_register(pm_evt_handler);
  APP_ERROR_CHECK(err_code);

  err_code = fds_register(fds_evt_handler);
  APP_ERROR_CHECK(err_code);

  memset(m_whitelist_peers, PM_PEER_ID_INVALID, sizeof(m_whitelist_peers));
  m_whitelist_peer_cnt = (sizeof(m_whitelist_peers) / sizeof(pm_peer_id_t));

  peer_list_get(m_whitelist_peers, &m_whitelist_peer_cnt);

  err_code = pm_whitelist_set(m_whitelist_peers, m_whitelist_peer_cnt);
  APP_ERROR_CHECK_NOT_URGENT(err_code);

  // Setup the device identies list.
  // Some SoftDevices do not support this feature.
  err_code = pm_device_identities_list_set(m_whitelist_peers, m_whitelist_peer_cnt);
  if (err_code != NRF_ERROR_NOT_SUPPORTED) {
    APP_ERROR_CHECK_NOT_URGENT(err_code);
  }

#ifdef LINK_SECURITY
  ecc_init(true);

  err_code = ecc_p256_keypair_gen(m_lesc_sk.sk, m_lesc_pk.pk);
  APP_ERROR_CHECK_NOT_URGENT(err_code);

  /* Set the public key */
  err_code = pm_lesc_public_key_set(&m_lesc_pk);
  APP_ERROR_CHECK_NOT_URGENT(err_code);
#endif
}
#endif

#if BLE_HIDS_ENABLED
static void hids_init(uint8_t *reportPtr, size_t reportLen) {
    uint32_t                   err_code;
    ble_hids_init_t            hids_init_obj;
    static ble_hids_inp_rep_init_t    input_report_array[1];
    ble_hids_inp_rep_init_t  * p_input_report;
    static ble_hids_outp_rep_init_t   output_report_array[1];
    ble_hids_outp_rep_init_t * p_output_report;
    uint8_t                    hid_info_flags;

    memset((void *)input_report_array, 0, sizeof(ble_hids_inp_rep_init_t));
    memset((void *)output_report_array, 0, sizeof(ble_hids_outp_rep_init_t));

    // Initialize HID Service
    p_input_report                      = &input_report_array[HID_INPUT_REPORT_KEYS_INDEX];
    p_input_report->max_len             = HID_KEYS_MAX_LEN;
    p_input_report->rep_ref.report_id   = HID_INPUT_REP_REF_ID;
    p_input_report->rep_ref.report_type = BLE_HIDS_REP_TYPE_INPUT;

#if NRF_SD_BLE_API_VERSION>=7 || NRF5X_SDK_15_3
    p_input_report->sec.cccd_wr = SEC_JUST_WORKS;
    p_input_report->sec.wr      = SEC_JUST_WORKS;
    p_input_report->sec.rd      = SEC_JUST_WORKS;
#else
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&p_input_report->security_mode.cccd_write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&p_input_report->security_mode.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&p_input_report->security_mode.write_perm);
#endif

    p_output_report                      = &output_report_array[HID_OUTPUT_REPORT_INDEX];
    p_output_report->max_len             = HID_OUTPUT_REPORT_MAX_LEN;
    p_output_report->rep_ref.report_id   = HID_OUTPUT_REP_REF_ID;
    p_output_report->rep_ref.report_type = BLE_HIDS_REP_TYPE_OUTPUT;

#if NRF_SD_BLE_API_VERSION>=7 || NRF5X_SDK_15_3
    p_output_report->sec.wr      = SEC_JUST_WORKS;
    p_output_report->sec.rd      = SEC_JUST_WORKS;
#else
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&p_output_report->security_mode.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&p_output_report->security_mode.write_perm);
#endif

    hid_info_flags = HID_INFO_FLAG_REMOTE_WAKE_MSK | HID_INFO_FLAG_NORMALLY_CONNECTABLE_MSK;

    memset(&hids_init_obj, 0, sizeof(hids_init_obj));

    hids_init_obj.evt_handler                    = on_hids_evt;
    hids_init_obj.error_handler                  = service_error_handler;
    hids_init_obj.is_kb                          = true;
    hids_init_obj.is_mouse                       = false;
    hids_init_obj.inp_rep_count                  = 1;
    hids_init_obj.p_inp_rep_array                = input_report_array;
    hids_init_obj.outp_rep_count                 = 1;
    hids_init_obj.p_outp_rep_array               = output_report_array;
    hids_init_obj.feature_rep_count              = 0;
    hids_init_obj.p_feature_rep_array            = NULL;
    hids_init_obj.rep_map.data_len               = reportLen;
    hids_init_obj.rep_map.p_data                 = reportPtr;
    hids_init_obj.hid_information.bcd_hid        = BASE_USB_HID_SPEC_VERSION;
    hids_init_obj.hid_information.b_country_code = 0;
    hids_init_obj.hid_information.flags          = hid_info_flags;
    hids_init_obj.included_services_count        = 0;
    hids_init_obj.p_included_services_array      = NULL;

#if NRF_SD_BLE_API_VERSION>=7 || NRF5X_SDK_15_3
    hids_init_obj.rep_map.rd_sec         = SEC_JUST_WORKS;
    hids_init_obj.hid_information.rd_sec = SEC_JUST_WORKS;

    hids_init_obj.boot_kb_inp_rep_sec.cccd_wr = SEC_JUST_WORKS;
    hids_init_obj.boot_kb_inp_rep_sec.rd      = SEC_JUST_WORKS;

    hids_init_obj.boot_kb_outp_rep_sec.rd = SEC_JUST_WORKS;
    hids_init_obj.boot_kb_outp_rep_sec.wr = SEC_JUST_WORKS;

    hids_init_obj.protocol_mode_rd_sec = SEC_JUST_WORKS;
    hids_init_obj.protocol_mode_wr_sec = SEC_JUST_WORKS;
    hids_init_obj.ctrl_point_wr_sec    = SEC_JUST_WORKS;
#else
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.rep_map.security_mode.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&hids_init_obj.rep_map.security_mode.write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.hid_information.security_mode.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&hids_init_obj.hid_information.security_mode.write_perm);

    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(
        &hids_init_obj.security_mode_boot_kb_inp_rep.cccd_write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.security_mode_boot_kb_inp_rep.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&hids_init_obj.security_mode_boot_kb_inp_rep.write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.security_mode_boot_kb_outp_rep.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.security_mode_boot_kb_outp_rep.write_perm);

    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.security_mode_protocol.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.security_mode_protocol.write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&hids_init_obj.security_mode_ctrl_point.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.security_mode_ctrl_point.write_perm);
#endif

    err_code = ble_hids_init(&m_hids, &hids_init_obj);
    APP_ERROR_CHECK(err_code);
}
#endif

static void conn_params_init() {
    uint32_t               err_code;
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;

    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}

/// Function for initializing services that will be used by the application.
static void services_init() {
    uint32_t       err_code;

    JsVar *usingNus = jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_NUS);
    if (!usingNus || jsvGetBool(usingNus)) { // default is on
      ble_nus_init_t nus_init;
      memset(&nus_init, 0, sizeof(nus_init));
      nus_init.data_handler = nus_data_handler;
#if (NRF_SD_BLE_API_VERSION==3) || (NRF_SD_BLE_API_VERSION==6)
      if (bleStatus & BLE_ENCRYPT_UART)
        nus_init.encrypt = true;
      if (bleStatus & BLE_SECURITY_MITM)
        nus_init.mitmProtect = true;
#else
#if PEER_MANAGER_ENABLED
#warning "No security on Nordic UART for this softdevice"
#endif
#endif
      err_code = ble_nus_init(&m_nus, &nus_init);
      APP_ERROR_CHECK(err_code);
      bleStatus |= BLE_NUS_INITED;
    }
    jsvUnLock(usingNus);
#if BLE_HIDS_ENABLED
    JsVar *hidReport = jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_HID_DATA);
    if (hidReport) {
      JSV_GET_AS_CHAR_ARRAY(hidPtr, hidLen, hidReport);
      if (hidPtr && hidLen) {
        hids_init((uint8_t*)hidPtr, hidLen);
        bleStatus |= BLE_HID_INITED;
      } else {
        jsiConsolePrintf("Not initialising HID - unable to get report descriptor\n");
      }
    }
    jsvUnLock(hidReport);
#endif
#if ESPR_BLUETOOTH_ANCS
    bool useANCS = jsvGetBoolAndUnLock(jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_ANCS));
    bool useAMS = jsvGetBoolAndUnLock(jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_AMS));
    bool useCTS = jsvGetBoolAndUnLock(jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_CTS));
    if (useANCS || useAMS || useCTS) {
      if (useANCS) bleStatus |= BLE_ANCS_INITED;
      if (useAMS) bleStatus |= BLE_AMS_INITED;
      if (useCTS) bleStatus |= BLE_CTS_INITED;
      ble_ancs_init();
    }
#endif
}

uint32_t app_ram_base;

/// Function for the SoftDevice initialization.
static void ble_stack_init() {
#if defined ( __GNUC__ )
    extern uint32_t __data_start__;
    uint32_t orig_app_ram_base = (uint32_t) &__data_start__;
    app_ram_base = orig_app_ram_base;
#else
#error "unsupported compiler"
#endif

#if CENTRAL_LINK_COUNT>0
    for (int i=0;i<CENTRAL_LINK_COUNT;i++)
      m_central_conn_handles[i] = BLE_CONN_HANDLE_INVALID;
#endif
    m_peripheral_conn_handle = BLE_CONN_HANDLE_INVALID;


#if NRF_SD_BLE_API_VERSION<5
    uint32_t err_code;

    nrf_clock_lf_cfg_t clock_lf_cfg = {
#ifdef ESPR_LSE_ENABLE
    // enable if we're on a device with 32kHz xtal
        .source        = NRF_CLOCK_LF_SRC_XTAL,
        .rc_ctiv       = 0,
        .rc_temp_ctiv  = 0,
        .xtal_accuracy = NRF_CLOCK_LF_XTAL_ACCURACY_50_PPM,
#else
        .source        = NRF_CLOCK_LF_SRC_RC,
        .rc_ctiv       = 16, // recommended for nRF52
        .rc_temp_ctiv  = 2,  // recommended for nRF52
        .xtal_accuracy = 0 // NRF_CLOCK_LF_ACCURACY_250_PPM
#endif
    };

    // Initialize SoftDevice.
    SOFTDEVICE_HANDLER_INIT(&clock_lf_cfg, false);

    ble_enable_params_t ble_enable_params;
    err_code = softdevice_enable_get_default_config(CENTRAL_LINK_COUNT,
                                                    PERIPHERAL_LINK_COUNT,
                                                    &ble_enable_params);
    APP_ERROR_CHECK(err_code);

#ifdef NRF52_SERIES
    ble_enable_params.common_enable_params.vs_uuid_count = 10;
#else
    ble_enable_params.common_enable_params.vs_uuid_count = 3;
#endif

    //Check the ram settings against the used number of links
    CHECK_RAM_START_ADDR(CENTRAL_LINK_COUNT, PERIPHERAL_LINK_COUNT);

    // Enable BLE stack.
#if (NRF_SD_BLE_API_VERSION >= 3)
    ble_enable_params.gatt_enable_params.att_mtu = NRF_BLE_MAX_MTU_SIZE;
#endif

    err_code = sd_ble_enable(&ble_enable_params,&app_ram_base);
    APP_ERROR_CHECK(err_code);
    // if the RAM base is correct, set it to 0 so we don't include it in process.env
    if (app_ram_base == orig_app_ram_base) app_ram_base=0;

#if (NRF_BLE_MAX_MTU_SIZE > GATT_MTU_SIZE_DEFAULT)
    {
        ble_opt_t gap_opt;
        gap_opt.gap_opt.ext_len.rxtx_max_pdu_payload_size = NRF_BLE_MAX_MTU_SIZE+4;
        err_code = sd_ble_opt_set(BLE_GAP_OPT_EXT_LEN, &gap_opt);
        APP_ERROR_CHECK(err_code);
        gap_opt.common_opt.conn_evt_ext.enable = 1;
        err_code = sd_ble_opt_set(BLE_COMMON_OPT_CONN_EVT_EXT, &gap_opt); // enable DLE
        APP_ERROR_CHECK(err_code);
    }
#endif
    // Subscribe for BLE events.
    err_code = softdevice_ble_evt_handler_set(ble_evt_dispatch);
    APP_ERROR_CHECK(err_code);

    // Register with the SoftDevice handler module for BLE events.
    err_code = softdevice_sys_evt_handler_set(soc_evt_handler);
    APP_ERROR_CHECK(err_code);


#else // NRF_SD_BLE_API_VERSION>=5
    ret_code_t err_code;

    err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    // Configure the BLE stack using the default settings.
    // Fetch the start address of the application RAM.
    uint32_t ram_start = 0;
    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);
#if (NRF_BLE_MAX_MTU_SIZE > GATT_MTU_SIZE_DEFAULT)
    {
        ble_cfg_t cfg;
        memset(&cfg, 0, sizeof(ble_cfg_t));
        cfg.conn_cfg.conn_cfg_tag = APP_BLE_CONN_CFG_TAG;
        cfg.conn_cfg.params.gatt_conn_cfg.att_mtu = NRF_BLE_MAX_MTU_SIZE;
        err_code = sd_ble_cfg_set(BLE_CONN_CFG_GATT, &cfg, app_ram_base);
        APP_ERROR_CHECK(err_code);
    }
#endif

    // Enable BLE stack.
    err_code = nrf_sdh_ble_enable(&app_ram_base);
    APP_ERROR_CHECK(err_code);

    // Register a handler for BLE events.
    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
    NRF_SDH_SOC_OBSERVER(m_soc_observer, APP_SOC_OBSERVER_PRIO, soc_evt_handler, NULL);
#endif

#if defined(PUCKJS) || defined(RUUVITAG) || defined(ESPR_DCDC_ENABLE)
    // can only be enabled if we're sure we have a DC-DC
    err_code = sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
    APP_ERROR_CHECK(err_code);
#endif
#if defined(ESPR_DCDC_HV_ENABLE)
    err_code = sd_power_dcdc0_mode_set(NRF_POWER_DCDC_ENABLE);
    APP_ERROR_CHECK(err_code);
#endif
#ifdef DEBUG
    // disable watchdog timer in debug mode, so we can use GDB
    NRF_WDT->CONFIG &= ~8;
#endif
}

// -----------------------------------------------------------------------------------
// -------------------------------------------------------------------- OTHER


/// Build advertising data struct to pass into @ref ble_advertising_init.
void jsble_setup_advdata(ble_advdata_t *advdata) {
  memset(advdata, 0, sizeof(*advdata));
  advdata->name_type          = BLE_ADVDATA_FULL_NAME;
  advdata->include_appearance = false;
  advdata->flags              = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
}

#if NRF_SD_BLE_API_VERSION>5
static ble_gap_adv_data_t m_ble_gap_adv_data;
// SoftDevice >= 6.1.0 needs this as static buffers
static uint8_t m_adv_data[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t m_scan_rsp_data[BLE_GAP_ADV_SET_DATA_SIZE_MAX]; // 31
#endif

uint32_t jsble_advertising_update(uint8_t *advPtr, unsigned int advLen, uint8_t *rspPtr, unsigned int rspLen, ble_gap_adv_params_t *adv_params) {
#if NRF_SD_BLE_API_VERSION>5
  if (bleStatus & BLE_IS_ADVERTISING){
    // first we need to switch away from our static live advertising data buffers
    // otherwise we would get NRF_ERROR_INVALID_STATE from sd_ble_gap_adv_set_configure
    ble_gap_adv_data_t tmp;
    uint8_t tmp_adv_data[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
    uint8_t tmp_scan_rsp_data[BLE_GAP_ADV_SET_DATA_SIZE_MAX];

    tmp.adv_data.len = m_ble_gap_adv_data.adv_data.len;
    if (tmp.adv_data.len){
      memcpy(tmp_adv_data, m_adv_data, tmp.adv_data.len);
      tmp.adv_data.p_data = tmp_adv_data;
    } else tmp.adv_data.p_data = NULL;

    tmp.scan_rsp_data.len = m_ble_gap_adv_data.scan_rsp_data.len;
    if (tmp.scan_rsp_data.len){
      memcpy(tmp_scan_rsp_data, m_scan_rsp_data, tmp.scan_rsp_data.len);
      tmp.scan_rsp_data.p_data = tmp_scan_rsp_data;
    } else tmp.scan_rsp_data.p_data = NULL;

    sd_ble_gap_adv_set_configure(&m_adv_handle, &tmp, NULL);
  }

  // now we can modify data and switch back to it
  if (advPtr){
    if (advLen){
      advLen=MIN(advLen,BLE_GAP_ADV_SET_DATA_SIZE_MAX);
      memcpy(m_adv_data, advPtr, advLen);
    }
    m_ble_gap_adv_data.adv_data.p_data = m_adv_data;
    m_ble_gap_adv_data.adv_data.len = advLen;
  }
  if (rspPtr){
    if (rspLen){
      rspLen=MIN(rspLen,BLE_GAP_ADV_SET_DATA_SIZE_MAX);
      memcpy(m_scan_rsp_data, rspPtr, rspLen);
    }
    m_ble_gap_adv_data.scan_rsp_data.p_data = m_scan_rsp_data;
    m_ble_gap_adv_data.scan_rsp_data.len = rspLen;
  }
  return sd_ble_gap_adv_set_configure(&m_adv_handle, &m_ble_gap_adv_data, adv_params);
#else
  return sd_ble_gap_adv_data_set((uint8_t *)advPtr, advLen, (uint8_t *)rspPtr, rspLen);
#endif
}

uint32_t jsble_advertising_start() {
  jsDebug(DBG_INFO,"jsble_advertising_start\n");
  // try not to call from IRQ as we might want to allocate JsVars
  if (bleStatus & BLE_IS_ADVERTISING) return 0;

#ifndef SAVE_ON_FLASH
  /* If we're not allowed to advertise because we are connected, just return */
  if (!(bleStatus & BLE_ADVERTISE_WHEN_CONNECTED) && jsble_has_peripheral_connection())
    return 0;
#endif

  ble_advdata_t scanrsp;

  JsVar *advDataVar = jswrap_ble_getCurrentAdvertisingData();
  JSV_GET_AS_CHAR_ARRAY(advPtr, advLen, advDataVar);

  // Set up scan response packet's contents
  ble_uuid_t adv_uuids[ADVERTISE_MAX_UUIDS];
  int adv_uuid_count = 0;
  if (bleStatus & BLE_HID_INITED) {
    adv_uuids[adv_uuid_count].uuid = BLE_UUID_HUMAN_INTERFACE_DEVICE_SERVICE;
    adv_uuids[adv_uuid_count].type = BLE_UUID_TYPE_BLE;
    adv_uuid_count++;
  }
  if (bleStatus & BLE_NUS_INITED) {
    adv_uuids[adv_uuid_count].uuid = BLE_UUID_NUS_SERVICE;
    adv_uuids[adv_uuid_count].type = BLE_UUID_TYPE_VENDOR_BEGIN; ///< We just assume we're the first 128 bit UUID in the list!
    adv_uuid_count++;
  }
  // add any user-defined services
  JsVar *advServices = jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_SERVICE_ADVERTISE);
  if (jsvIsArray(advServices)) {
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, advServices);
    while (jsvObjectIteratorHasValue(&it)) {
      ble_uuid_t ble_uuid;
      if (adv_uuid_count < ADVERTISE_MAX_UUIDS &&
          !bleVarToUUIDAndUnLock(&ble_uuid, jsvObjectIteratorGetValue(&it))) {
        adv_uuids[adv_uuid_count++] = ble_uuid;
      }
      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);
  }
  jsvUnLock(advServices);
  // check for any options set up
  JsVar *advOptions = jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_ADVERTISE_OPTIONS);
  // update scan response packet
  memset(&scanrsp, 0, sizeof(scanrsp));
  scanrsp.uuids_complete.uuid_cnt = adv_uuid_count;
  scanrsp.uuids_complete.p_uuids  = &adv_uuids[0];


  ble_gap_adv_params_t adv_params;
  memset(&adv_params, 0, sizeof(adv_params));
  bool non_connectable = (bleStatus & BLE_IS_NOT_CONNECTABLE) || jsble_has_peripheral_connection(); // can't advertise connectable if we already have a connection
  bool non_scannable = bleStatus & BLE_IS_NOT_SCANNABLE;
#if NRF_SD_BLE_API_VERSION>5
  adv_params.primary_phy     = BLE_GAP_PHY_1MBPS;
  adv_params.secondary_phy   = BLE_GAP_PHY_AUTO; // the default
  adv_params.filter_policy   = BLE_GAP_ADV_FP_ANY;
  adv_params.duration  = BLE_GAP_ADV_TIMEOUT_GENERAL_UNLIMITED;
  //adv_params.scan_req_notification = 1; // creates a BLE_GAP_EVT_SCAN_REQ_REPORT event
  if (jsvIsObject(advOptions)) { // extended SDK15+ options
    JsVar *advPhy = jsvObjectGetChildIfExists(advOptions, "phy");
    if (jsvIsUndefined(advPhy) || jsvIsStringEqual(advPhy,"1mbps")) {
      // default
    } else if (jsvIsStringEqual(advPhy,"2mbps")) {
      adv_params.primary_phy     = BLE_GAP_PHY_1MBPS;
      adv_params.secondary_phy   = BLE_GAP_PHY_2MBPS;
    } else if (jsvIsStringEqual(advPhy,"coded")) {
      adv_params.primary_phy     = BLE_GAP_PHY_CODED; // must use 1mbps phy if connectable?
      adv_params.secondary_phy   = BLE_GAP_PHY_CODED;
    } else jsWarn("Unknown phy %q\n", advPhy);
    jsvUnLock(advPhy);
  }
  if (adv_params.secondary_phy == BLE_GAP_PHY_AUTO) {
    // the default...
    adv_params.properties.type = non_connectable
          ? (non_scannable ? BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED : BLE_GAP_ADV_TYPE_NONCONNECTABLE_SCANNABLE_UNDIRECTED)
          : (non_scannable ? BLE_GAP_ADV_TYPE_CONNECTABLE_NONSCANNABLE_DIRECTED : BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
  } else { // coded/2mbps - force use of extended advertising
    adv_params.properties.type = non_connectable
        ? (non_scannable ? BLE_GAP_ADV_TYPE_EXTENDED_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED : BLE_GAP_ADV_TYPE_EXTENDED_NONCONNECTABLE_SCANNABLE_UNDIRECTED)
        : (non_scannable ? BLE_GAP_ADV_TYPE_EXTENDED_CONNECTABLE_NONSCANNABLE_UNDIRECTED : BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
  }
#else
  adv_params.type        = non_connectable
      ? (non_scannable ? BLE_GAP_ADV_TYPE_ADV_NONCONN_IND : BLE_GAP_ADV_TYPE_ADV_SCAN_IND)
      : (non_scannable ? BLE_GAP_ADV_TYPE_ADV_DIRECT_IND : BLE_GAP_ADV_TYPE_ADV_IND);
  adv_params.fp          = BLE_GAP_ADV_FP_ANY;
  adv_params.timeout  = APP_ADV_TIMEOUT_IN_SECONDS;
#endif
  jsvUnLock(advOptions);
  adv_params.p_peer_addr = NULL;
  adv_params.interval = bleAdvertisingInterval;

  uint32_t err_code = 0;
  uint8_t m_enc_scan_response_data[31]; // BLE_GAP_ADV_SET_DATA_SIZE_MAX
  uint16_t m_enc_scan_response_data_len = sizeof(m_enc_scan_response_data);
#if NRF_SD_BLE_API_VERSION<5
  err_code = adv_data_encode(&scanrsp, m_enc_scan_response_data, &m_enc_scan_response_data_len);
#else
  err_code = ble_advdata_encode(&scanrsp, m_enc_scan_response_data, &m_enc_scan_response_data_len);
#endif
  if (jsble_check_error(err_code)) {
    jsvUnLock(advDataVar);
    return err_code;
  }

  //jsiConsolePrintf("adv_data_set %d %d\n", advPtr, advLen);
#if NRF_SD_BLE_API_VERSION>5
  memset(&m_ble_gap_adv_data, 0, sizeof(m_ble_gap_adv_data));
#endif
  err_code = jsble_advertising_update(
    (uint8_t*)advPtr, advLen,
    non_scannable ? NULL : m_enc_scan_response_data, non_scannable ? 0 : m_enc_scan_response_data_len,
    &adv_params
  );
  jsble_check_error(err_code);
#if NRF_SD_BLE_API_VERSION>5
  if (!err_code) {
    jsble_check_error(sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_ADV, m_adv_handle, m_tx_power));
    err_code = sd_ble_gap_adv_start(m_adv_handle, APP_BLE_CONN_CFG_TAG);
    jsble_check_error(err_code);
  }
#elif NRF_SD_BLE_API_VERSION<5
  if (!err_code)
    err_code = sd_ble_gap_adv_start(&adv_params);
#else
  // do we care about SDK14?
  if (!err_code)
    err_code = sd_ble_gap_adv_start(&adv_params, APP_BLE_CONN_CFG_TAG);
#endif
  bleStatus |= BLE_IS_ADVERTISING;
  jsvUnLock(advDataVar);
  if (execInfo.root) // JS interpreter may not be initialised yet!
    bleQueueEventAndUnLock(JS_EVENT_PREFIX"advertising", jsvNewFromBool(true));
  return err_code;
}

uint32_t jsble_advertising_update_advdata(char *dPtr, unsigned int dLen) {
  return jsble_advertising_update((uint8_t *)dPtr, dLen, NULL, 0, NULL);
}
uint32_t jsble_advertising_update_scanresponse(char *dPtr, unsigned int dLen) {
  return jsble_advertising_update(NULL, 0, (uint8_t*)dPtr, dLen, NULL);
}

void jsble_advertising_stop() {
  jsDebug(DBG_INFO,"jsble_advertising_stop\n");
  if (!(bleStatus & BLE_IS_ADVERTISING)) return;
#if NRF_SD_BLE_API_VERSION > 5
  sd_ble_gap_adv_stop(m_adv_handle);
#else
  sd_ble_gap_adv_stop();
#endif
  bleStatus &= ~BLE_IS_ADVERTISING;
  bleQueueEventAndUnLock(JS_EVENT_PREFIX"advertising", jsvNewFromBool(false));
}

/** Initialise the BLE stack */
 void jsble_init() {
  jsDebug(DBG_INFO,"jsble_init\n");
   uint32_t err_code;
   ble_stack_init();
   err_code = radio_notification_init(
 #ifdef NRF52_SERIES
                           6, /* IRQ Priority -  Must be 6 on nRF52. 7 doesn't work */
 #else
                           3, /* IRQ Priority -  nRF51 has different IRQ structure */
 #endif
                           NRF_RADIO_NOTIFICATION_TYPE_INT_ON_INACTIVE,
                           NRF_RADIO_NOTIFICATION_DISTANCE_NONE);
   APP_ERROR_CHECK(err_code);

#ifdef NRF52_SERIES
   // Set MAC address
   JsVar *v = jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_MAC_ADDRESS);
   if (v) {
     ble_gap_addr_t p_addr;
     if (bleVarToAddr(v, &p_addr)) {
#if NRF_SD_BLE_API_VERSION < 3
       err_code = sd_ble_gap_address_set(BLE_GAP_ADDR_CYCLE_MODE_NONE,&p_addr);
#else
       err_code = sd_ble_gap_addr_set(&p_addr);
#endif
       if (err_code) jsiConsolePrintf("sd_ble_gap_addr_set failed: 0x%x\n", err_code);
     }
   }
   jsvUnLock(v);
/*
   // This sets the MAC address to the default address, but "public", not "random"
   uint32_t addr0 =  NRF_FICR->DEVICEADDR[0];
   uint32_t addr1 =  NRF_FICR->DEVICEADDR[1];
   ble_gap_addr_t addr;
   addr.addr_type = BLE_GAP_ADDR_TYPE_PUBLIC;
   addr.addr[5] = ((addr1>>8 )&0xFF)|0xC0;
   addr.addr[4] = ((addr1    )&0xFF);
   addr.addr[3] = ((addr0>>24)&0xFF);
   addr.addr[2] = ((addr0>>16)&0xFF);
   addr.addr[1] = ((addr0>> 8)&0xFF);
   addr.addr[0] = ((addr0    )&0xFF);
   err_code = sd_ble_gap_addr_set(&addr);
   if (err_code) jsiConsolePrintf("sd_ble_gap_addr_set failed: 0x%x\n", err_code);
 */
#endif

#if PEER_MANAGER_ENABLED
   peer_manager_init(false /*don't erase_bonds*/);
#endif
   gap_params_init();
   services_init();
   conn_params_init();

   // reset the status for things that aren't happening now we're rebooted
   bleStatus &= ~BLE_RESET_ON_SOFTDEVICE_START;
   // if we weren't sleeping, start advertising again
   if (!(bleStatus & BLE_IS_SLEEPING))
    jswrap_ble_wake();
}

/** Completely deinitialise the BLE stack */
bool jsble_kill() {
  jsDebug(DBG_INFO,"jsble_kill\n");
  jswrap_ble_sleep();
  // BLE NUS doesn't need deinitialising (no ble_nus_kill)
  bleStatus &= ~BLE_NUS_INITED;
  // BLE HID doesn't need deinitialising (no ble_hids_kill)
  bleStatus &= ~BLE_HID_INITED;
#if ESPR_BLUETOOTH_ANCS
  // BLE ANCS/AMS doesn't need deinitialising
  bleStatus &= ~BLE_ANCS_AMS_OR_CTS_INITED;
#endif
  uint32_t err_code;
#if NRF_SD_BLE_API_VERSION < 5
  err_code = sd_softdevice_disable();
#else
#ifndef NRF5X_SDK_15_3
  nrf_drv_clock_lfclk_request(NULL); // https://devzone.nordicsemi.com/f/nordic-q-a/56256/disabling-the-softdevice-hangs-when-using-lfrc-with-an-active-watchdog
#endif
  err_code = nrf_sdh_disable_request();
#endif
  return !jsble_check_error(err_code);
}


/** Stop and restart the softdevice so that we can update the services in it -
 * both user-defined as well as UART/HID */
void jsble_restart_softdevice(JsVar *jsFunction) {
  jsDebug(DBG_INFO,"jsble_restart_softdevice (%s)\n", (bleStatus&BLE_IS_SLEEPING)?"sleep":"awake");
  assert(!jsble_has_connection());
  bleStatus &= ~(BLE_NEEDS_SOFTDEVICE_RESTART | BLE_SERVICES_WERE_SET);
  bool bleWasSleeping = bleStatus & BLE_IS_SLEEPING;;
  // if we were scanning, make sure we stop
  if (bleStatus & BLE_IS_SCANNING) {
    sd_ble_gap_scan_stop();
  }
  //jsiConsolePrintf("Restart softdevice\n");
  jshUtilTimerDisable(); // don't want the util timer firing during this!
  JsSysTime lastTime = jshGetSystemTime();
  if (jsble_kill()) {
    if (jsvIsFunction(jsFunction))
      jspExecuteFunction(jsFunction,NULL,0,NULL);
    jsble_init();
    // reinitialise everything
    jswrap_ble_reconfigure_softdevice();
    jshSetSystemTime(lastTime); // Softdevice resets the RTC - so we must reset our offsets
  }
  jstRestartUtilTimer(); // restart the util timer
  if (!bleWasSleeping)
    jswrap_ble_wake();
  jsDebug(DBG_INFO,"jsble_restart_softdevice ends (%s)\n", (bleStatus&BLE_IS_SLEEPING)?"sleep":"awake");
}

uint32_t jsble_set_scanning(bool enabled, JsVar *options) {
  uint32_t err_code = 0;
  if (enabled) {
    if (bleStatus & BLE_IS_SCANNING) return 0;
    bleStatus |= BLE_IS_SCANNING;
    ble_gap_scan_params_t     m_scan_param;
    memset(&m_scan_param,0,sizeof(m_scan_param));
#if NRF_SD_BLE_API_VERSION>5
    m_scan_param.scan_phys         = BLE_GAP_PHY_AUTO;
    m_scan_param.filter_policy     = BLE_GAP_SCAN_FP_ACCEPT_ALL;
#endif
    m_scan_param.interval     = SCAN_INTERVAL;// Scan interval.
    m_scan_param.window       = SCAN_WINDOW;  // Scan window.
    m_scan_param.timeout      = 0x0000;       // No timeout - BLE_GAP_SCAN_TIMEOUT_UNLIMITED

    if (jsvIsObject(options)) {
      m_scan_param.active = jsvObjectGetBoolChild(options, "active"); // Active scanning set.
#if NRF_SD_BLE_API_VERSION>5
      if (jsvObjectGetBoolChild(options, "extended"))
        m_scan_param.extended = 1;
      JsVar *advPhy = jsvObjectGetChildIfExists(options, "phy");
      if (jsvIsUndefined(advPhy) || jsvIsStringEqual(advPhy,"1mbps")) {
        // default
      } else if (jsvIsStringEqual(advPhy,"2mbps")) {
        m_scan_param.scan_phys = BLE_GAP_PHY_2MBPS;
        m_scan_param.extended = 1;
      } else if (jsvIsStringEqual(advPhy,"both")) {
        m_scan_param.scan_phys = BLE_GAP_PHY_1MBPS|BLE_GAP_PHY_CODED;
        m_scan_param.extended = 1;
      } else if (jsvIsStringEqual(advPhy,"coded")) {
        m_scan_param.scan_phys = BLE_GAP_PHY_CODED;
        m_scan_param.extended = 1;
      } else jsWarn("Unknown phy %q\n", advPhy);
      // BLE_GAP_PHYS_SUPPORTED (all 3) doesn't appear to work - see https://github.com/espruino/Espruino/issues/2465
      jsvUnLock(advPhy);
#endif
      uint32_t scan_window = MSEC_TO_UNITS(jsvObjectGetIntegerChild(options, "window"), UNIT_0_625_MS);
      if (scan_window>=4 && scan_window<=16384)
        m_scan_param.window = scan_window;
      uint32_t scan_interval = MSEC_TO_UNITS(jsvObjectGetIntegerChild(options, "interval"), UNIT_0_625_MS);
      if (scan_interval>=4 && scan_interval<=16384)
        m_scan_param.interval = scan_interval;
      if (m_scan_param.interval < m_scan_param.window)
        m_scan_param.interval = m_scan_param.window;
    }

    err_code = sd_ble_gap_scan_start(&m_scan_param
#if NRF_SD_BLE_API_VERSION>5
         , &m_scan_buffer
#endif
         );
  } else {
    if (!(bleStatus & BLE_IS_SCANNING)) return 0;
    bleStatus &= ~BLE_IS_SCANNING;
    err_code = sd_ble_gap_scan_stop();
  }
  return err_code;
}

uint32_t jsble_set_rssi_scan(bool enabled) {
  uint32_t err_code = 0;
  if (enabled) {
     if (bleStatus & BLE_IS_RSSI_SCANNING) return 0;
     bleStatus |= BLE_IS_RSSI_SCANNING;
     if (jsble_has_peripheral_connection())
       err_code = sd_ble_gap_rssi_start(m_peripheral_conn_handle, 0, 0);
   } else {
     if (!(bleStatus & BLE_IS_RSSI_SCANNING)) return 0;
     bleStatus &= ~BLE_IS_RSSI_SCANNING;
     if (jsble_has_peripheral_connection())
       err_code = sd_ble_gap_rssi_stop(m_peripheral_conn_handle);
   }
  return err_code;
}

#if CENTRAL_LINK_COUNT>0
uint32_t jsble_set_central_rssi_scan(uint16_t central_conn_handle, bool enabled) {
  uint32_t err_code = 0;
  if (enabled) {
    err_code = sd_ble_gap_rssi_start(central_conn_handle, 0, 0);
  } else {
    err_code = sd_ble_gap_rssi_stop(central_conn_handle);
  }
  if (err_code == NRF_ERROR_INVALID_STATE) {
    // We either tried to start when already started, or stop when
    // already stopped, so we can simply ignore this condition.
    err_code = 0;
  }
  return err_code;
}
#endif

/** Sets security mode for a characteristic configuration */
void set_security_mode(ble_gap_conn_sec_mode_t *perm, JsVar *configVar) {
  if (jsvObjectGetBoolChild(configVar, "signed")) {
    if (jsvObjectGetBoolChild(configVar, "mitm")) {
      BLE_GAP_CONN_SEC_MODE_SET_SIGNED_WITH_MITM(perm);
    } else {
      BLE_GAP_CONN_SEC_MODE_SET_SIGNED_NO_MITM(perm);
    }
  } else {
    if (jsvObjectGetBoolChild(configVar, "lesc")) {
      BLE_GAP_CONN_SEC_MODE_SET_LESC_ENC_WITH_MITM(perm);
    } else if (jsvObjectGetBoolChild(configVar, "encrypted")) {
      if (jsvObjectGetBoolChild(configVar, "mitm")) {
        BLE_GAP_CONN_SEC_MODE_SET_ENC_WITH_MITM(perm);
      } else {
        BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(perm);
      }
    }
  }
}

/** Actually set the services defined in the 'data' object. Note: we can
 * only do this *once* - so to change it we must reset the softdevice and
 * then call this again */
void jsble_set_services(JsVar *data) {
  uint32_t err_code;

  if (jsvIsObject(data)) {
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, data);
    while (jsvObjectIteratorHasValue(&it)) {
      ble_uuid_t ble_uuid;
      uint16_t service_handle;

      // Add the service
      const char *errorStr;
      if ((errorStr=bleVarToUUIDAndUnLock(&ble_uuid, jsvObjectIteratorGetKey(&it)))) {
        jsExceptionHere(JSET_ERROR, "Invalid Service UUID: %s", errorStr);
        break;
      }

      // Ok, now we're setting up servcies
      bleStatus |= BLE_SERVICES_WERE_SET;
      err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,
                                              &ble_uuid,
                                              &service_handle);
      if (jsble_check_error(err_code)) {
        break;
      }


      // Now add characteristics
      JsVar *serviceVar = jsvObjectIteratorGetValue(&it);
      JsvObjectIterator serviceit;
      jsvObjectIteratorNew(&serviceit, serviceVar);
      while (jsvObjectIteratorHasValue(&serviceit)) {
        ble_uuid_t          char_uuid;
        ble_gatts_char_md_t char_md;
        ble_gatts_attr_t    attr_char_value;
        ble_gatts_attr_md_t attr_md;
        ble_gatts_char_handles_t  characteristic_handles;
        char description[32];

        if ((errorStr=bleVarToUUIDAndUnLock(&char_uuid, jsvObjectIteratorGetKey(&serviceit)))) {
          jsExceptionHere(JSET_ERROR, "Invalid Characteristic UUID: %s", errorStr);
          break;
        }
        JsVar *charVar = jsvObjectIteratorGetValue(&serviceit);

        memset(&char_md, 0, sizeof(char_md));
        if (jsvObjectGetBoolChild(charVar, "broadcast"))
          char_md.char_props.broadcast = 1;
        if (jsvObjectGetBoolChild(charVar, "notify"))
          char_md.char_props.notify = 1;
        if (jsvObjectGetBoolChild(charVar, "indicate"))
          char_md.char_props.indicate = 1;
        if (jsvObjectGetBoolChild(charVar, "readable"))
          char_md.char_props.read = 1;
        if (jsvObjectGetBoolChild(charVar, "writable")) {
          char_md.char_props.write = 1;
          char_md.char_props.write_wo_resp = 1;
        }
        char_md.p_char_user_desc         = NULL;
        char_md.p_char_pf                = NULL;
        char_md.p_user_desc_md           = NULL;
        char_md.p_cccd_md                = NULL;
        char_md.p_sccd_md                = NULL;
        JsVar *charDescriptionVar = jsvObjectGetChildIfExists(charVar, "description");
        if (charDescriptionVar && jsvHasCharacterData(charDescriptionVar)) {
          int8_t len = jsvGetString(charDescriptionVar, description, sizeof(description));
          char_md.p_char_user_desc = (uint8_t *)description;
          char_md.char_user_desc_size = len;
          char_md.char_user_desc_max_size = len;
        }
        jsvUnLock(charDescriptionVar);
        memset(&attr_md, 0, sizeof(attr_md));
        // init access with default values
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
#ifndef SAVE_ON_FLASH
        // set up access with user configs
        JsVar *securityVar = jsvObjectGetChildIfExists(charVar, "security");
        if (securityVar != NULL) {
          JsVar *readVar = jsvObjectGetChildIfExists(securityVar, "read");
          if (readVar != NULL) {
            set_security_mode(&attr_md.read_perm, readVar);
            jsvUnLock(readVar);
          }
          JsVar *writeVar = jsvObjectGetChildIfExists(securityVar, "write");
          if (writeVar != NULL) {
            set_security_mode(&attr_md.write_perm, writeVar);
            jsvUnLock(writeVar);
          }
        }
        jsvUnLock(securityVar);
#endif

        attr_md.vloc       = BLE_GATTS_VLOC_STACK;
        attr_md.rd_auth    = 0;
        attr_md.wr_auth    = 0;
        attr_md.vlen       = 1; // TODO: variable length?

        memset(&attr_char_value, 0, sizeof(attr_char_value));
        attr_char_value.p_uuid       = &char_uuid;
        attr_char_value.p_attr_md    = &attr_md;
        attr_char_value.init_len     = 0;
        attr_char_value.init_offs    = 0;
        attr_char_value.p_value      = 0;
        attr_char_value.max_len      = (uint16_t)jsvObjectGetIntegerChild(charVar, "maxLen");
        if (attr_char_value.max_len==0) attr_char_value.max_len=1;

        // get initial data
        JsVar *charValue = jsvObjectGetChildIfExists(charVar, "value");
        if (charValue) {
          JSV_GET_AS_CHAR_ARRAY(vPtr, vLen, charValue);
          if (vPtr && vLen) {
            attr_char_value.p_value = (uint8_t*)vPtr;
            attr_char_value.init_len = vLen;
            if (attr_char_value.init_len > attr_char_value.max_len)
              attr_char_value.max_len = attr_char_value.init_len;
          }
        }

        err_code = sd_ble_gatts_characteristic_add(service_handle,
                                                   &char_md,
                                                   &attr_char_value,
                                                   &characteristic_handles);
        jsble_check_error(err_code);
        jsvUnLock(charValue); // unlock here in case we were storing data in a flat string

        // Add onWrite callback
        JsVar *writeCb = jsvObjectGetChildIfExists(charVar, "onWrite");
        if (writeCb) {
          char eventName[12];
          bleGetWriteEventName(eventName, characteristic_handles.value_handle);
          jsvObjectSetChildAndUnLock(execInfo.root, eventName, writeCb);
        }
        // Add onWriteDesc callback for writes to the CCCD
        writeCb = jsvObjectGetChildIfExists(charVar, "onWriteDesc");
        if (writeCb) {
          char eventName[12];
          bleGetWriteEventName(eventName, characteristic_handles.cccd_handle);
          jsvObjectSetChildAndUnLock(execInfo.root, eventName, writeCb);
        }

        jsvUnLock(charVar);

        jsvObjectIteratorNext(&serviceit);
      }
      jsvObjectIteratorFree(&serviceit);
      jsvUnLock(serviceVar);

      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);
  }
}

/// Disconnect from the given connection
uint32_t jsble_disconnect(uint16_t conn_handle) {
  return sd_ble_gap_disconnect(conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
}

void jsble_startBonding(bool forceRePair) {
#if PEER_MANAGER_ENABLED
  if (!jsble_has_peripheral_connection())
      return bleCompleteTaskFailAndUnLock(BLETASK_BONDING, jsvNewFromString("Not connected"));
  jsble_queue_pending(BLEP_BONDING_STATUS, BLE_BOND_REQUEST); // report that we've requested bonding
  uint32_t err_code = pm_conn_secure(m_peripheral_conn_handle, forceRePair);
  JsVar *errStr = jsble_get_error_string(err_code);
  if (errStr) {
    bleCompleteTaskFail(BLETASK_BONDING, errStr);
    jsvUnLock(errStr);
  }
#else
  return bleCompleteTaskFailAndUnLock(BLETASK_BONDING, jsvNewFromString("Peer Manager not compiled in"));
#endif
}

#if BLE_HIDS_ENABLED
void jsble_send_hid_input_report(uint8_t *data, int length) {
  if (!(bleStatus & BLE_HID_INITED)) {
    jsExceptionHere(JSET_ERROR, "BLE HID not enabled");
    return;
  }
  if (!jsble_has_peripheral_connection()) {
    jsExceptionHere(JSET_ERROR, "Not connected");
    return;
  }
  if (bleStatus & BLE_IS_SENDING_HID) {
    jsExceptionHere(JSET_ERROR, "BLE HID already sending");
    return;
  }
  if (length > HID_KEYS_MAX_LEN) {
    jsExceptionHere(JSET_ERROR, "BLE HID report too long - max length = %d", HID_KEYS_MAX_LEN);
    return;
  }

  uint32_t err_code;
  if (!m_in_boot_mode) {
      err_code = ble_hids_inp_rep_send(&m_hids,
                                       HID_INPUT_REPORT_KEYS_INDEX,
                                       length,
                                       data
#if NRF_SD_BLE_API_VERSION>5
                                       ,m_peripheral_conn_handle
#endif
                                       );
  } else {
      err_code = ble_hids_boot_kb_inp_rep_send(&m_hids,
                                               length,
                                               data
#if NRF_SD_BLE_API_VERSION>5
                                       ,m_peripheral_conn_handle
#endif
                                               );
  }
  if (!jsble_check_error(err_code))
    bleStatus |= BLE_IS_SENDING_HID;
}
#endif

#ifdef USE_NFC
void jsble_nfc_stop() {
  nfcEnabled = false;
  hal_nfc_stop();
  hal_nfc_done();
}

void jsble_nfc_set_atqa(uint16_t data) {
  uint32_t ret_val;
  ret_val = hal_nfc_parameter_set(HAL_NFC_PARAM_ID_SENSRES, &data, 2);
  if (ret_val)
    return jsExceptionHere(JSET_ERROR, "nfcSetSAK: Got NFC error code %d", ret_val);
  // NRF_NFCT->SENSRES = data;
}

void jsble_nfc_set_sak(uint8_t data) {
  uint32_t ret_val;
  ret_val = hal_nfc_parameter_set(HAL_NFC_PARAM_ID_SELRES, &data, 1);
  if (ret_val)
    return jsExceptionHere(JSET_ERROR, "nfcSetSAK: Got NFC error code %d", ret_val);
  // NRF_NFCT->SELRES = data;
}

void jsble_nfc_get_internal(uint8_t *data, size_t *max_len) {

  uint32_t ret_val;

  ret_val = hal_nfc_parameter_get(HAL_NFC_PARAM_ID_INTERNAL, data, max_len);
  if (ret_val)
    return jsExceptionHere(JSET_ERROR, "nfcGetInternal: Got NFC error code %d", ret_val);
}

void jsble_nfc_start(const uint8_t *data, size_t len) {
  jsble_nfc_stop();

  uint32_t ret_val;

  /* Set UID / UID Length */
  if (len)
    ret_val = hal_nfc_parameter_set(HAL_NFC_PARAM_ID_NFCID1, data, len);
  else
    ret_val = hal_nfc_parameter_set(HAL_NFC_PARAM_ID_NFCID1, "\x07", 1);
  if (ret_val)
    return jsExceptionHere(JSET_ERROR, "nfcSetUid: Got NFC error code %d", ret_val);

  ret_val = hal_nfc_setup(nfc_callback, NULL);
  if (ret_val)
    return jsExceptionHere(JSET_ERROR, "nfcSetup: Got NFC error code %d", ret_val);

  /* Start sensing NFC field */
  ret_val = hal_nfc_start();
  if (ret_val)
    return jsExceptionHere(JSET_ERROR, "nfcStartEmulation: NFC error code %d", ret_val);

  nfcEnabled = true;
}

void jsble_nfc_send(const uint8_t *data, size_t len) {
  if (!nfcEnabled) return;

  uint32_t ret_val;

  ret_val = hal_nfc_send(data, len);
  if (ret_val)
    return jsExceptionHere(JSET_ERROR, "nfcSend: NFC error code %d", ret_val);
}

void jsble_nfc_send_rsp(const uint8_t data, size_t len) {
  if (!nfcEnabled) return;

  uint32_t ret_val;

  ret_val = hal_nfc_send_rsp(data, len);
  if (ret_val)
    return jsExceptionHere(JSET_ERROR, "nfcSend: NFC error code %d", ret_val);
}
#endif

JsVar *jsble_get_security_status(uint16_t conn_handle) {
#if PEER_MANAGER_ENABLED
  JsVar *result = jsvNewWithFlags(JSV_OBJECT);
  if (!result) return 0;

  if (conn_handle == BLE_CONN_HANDLE_INVALID ||
      conn_handle == m_peripheral_conn_handle) {
    // is this NRF.getSecurityStatus?
    bool isAdvertising = bleStatus & BLE_IS_ADVERTISING;
    jsvObjectSetChildAndUnLock(result, "advertising", jsvNewFromBool(isAdvertising));
  }
  if (conn_handle == BLE_CONN_HANDLE_INVALID) {
    jsvObjectSetChildAndUnLock(result, "connected", jsvNewFromBool(false));
    return result;
  }
  pm_conn_sec_status_t status;
  uint32_t err_code = pm_conn_sec_status_get(conn_handle, &status);
  if (!jsble_check_error(err_code)) {
    jsvObjectSetChildAndUnLock(result, "connected", jsvNewFromBool(status.connected));
    jsvObjectSetChildAndUnLock(result, "encrypted", jsvNewFromBool(status.encrypted));
    jsvObjectSetChildAndUnLock(result, "mitm_protected", jsvNewFromBool(status.mitm_protected));
    jsvObjectSetChildAndUnLock(result, "bonded", jsvNewFromBool(status.bonded));
#ifndef SAVE_ON_FLASH
    if (status.connected && conn_handle==m_peripheral_conn_handle)
      jsvObjectSetChildAndUnLock(result, "connected_addr", bleAddrToStr(m_peripheral_addr));
#endif
    return result;
  }
  return 0;
#else
  jsExceptionHere(JSET_ERROR,"Peer Manager not compiled in");
  return 0;
#endif
}


/// Set the transmit power of the current (and future) connections
void jsble_set_tx_power(int8_t pwr) {
  uint32_t              err_code;
#if NRF_SD_BLE_API_VERSION > 5
  if (m_peripheral_conn_handle != BLE_CONN_HANDLE_INVALID)
    err_code = sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_CONN, m_peripheral_conn_handle, pwr);
#if CENTRAL_LINK_COUNT>0
  for (int i=0;i<CENTRAL_LINK_COUNT;i++)
    if (m_central_conn_handles[i] != BLE_CONN_HANDLE_INVALID)
      err_code = sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_CONN, m_central_conn_handles[i], pwr);
#endif
  if (m_adv_handle != BLE_GAP_ADV_SET_HANDLE_NOT_SET)
    err_code = sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_ADV, m_adv_handle, pwr);
  err_code = sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_SCAN_INIT, 0/*ignored*/, pwr);
  if (!err_code)
    m_tx_power = pwr;
#else
  err_code = sd_ble_gap_tx_power_set(pwr);
#endif
  jsble_check_error(err_code);
}

#if CENTRAL_LINK_COUNT>0
void jsble_central_connect(ble_gap_addr_t peer_addr, JsVar *options) {
  uint32_t              err_code;

  ble_gap_scan_params_t     m_scan_param;
  memset(&m_scan_param, 0, sizeof(m_scan_param));
  m_scan_param.active       = 1;            // Active scanning set.
  m_scan_param.interval     = MSEC_TO_UNITS(100, UNIT_0_625_MS); // Scan interval.
  m_scan_param.window       = MSEC_TO_UNITS(90, UNIT_0_625_MS); // Scan window.
  m_scan_param.timeout      = 4;            // 4 second timeout.
#if NRF_SD_BLE_API_VERSION>5
  // It seems we could force connect on coded phy with:
  // m_scan_param.extended = 1;
  // m_scan_param.scan_phys = BLE_GAP_PHY_CODED; BLE_GAP_PHYS_SUPPORTED results in INVALID_PARAM
#endif

  ble_gap_conn_params_t   gap_conn_params;
  memset(&gap_conn_params, 0, sizeof(gap_conn_params));
  BLEFlags flags = jsvGetIntegerAndUnLock(jsvObjectGetChildIfExists(execInfo.hiddenRoot, BLE_NAME_FLAGS));
  if (flags & BLE_FLAGS_LOW_POWER) {
    gap_conn_params.min_conn_interval = MSEC_TO_UNITS(500, UNIT_1_25_MS);   // Minimum acceptable connection interval (500 ms)
    gap_conn_params.max_conn_interval = MSEC_TO_UNITS(1000, UNIT_1_25_MS);    // Maximum acceptable connection interval (1000 ms)
  } else {
    gap_conn_params.min_conn_interval = MSEC_TO_UNITS(20, UNIT_1_25_MS);   // Minimum acceptable connection interval (20 ms)
    gap_conn_params.max_conn_interval = MSEC_TO_UNITS(200, UNIT_1_25_MS);    // Maximum acceptable connection interval (200 ms)
  }
  gap_conn_params.slave_latency     = SLAVE_LATENCY_CENTRAL;
  gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;
  // handle options
  if (jsvIsObject(options)) {
    JsVarFloat v;
    v = jsvObjectGetFloatChild(options,"minInterval");
    if (!isnan(v)) gap_conn_params.min_conn_interval = (uint16_t)(MSEC_TO_UNITS(v, UNIT_1_25_MS)+0.5);
    v = jsvObjectGetFloatChild(options,"maxInterval");
    if (!isnan(v)) gap_conn_params.max_conn_interval = (uint16_t)(MSEC_TO_UNITS(v, UNIT_1_25_MS)+0.5);
  }
  /* From NRF SDK: If both conn_sup_timeout and max_conn_interval are specified, then the following constraint applies:
     conn_sup_timeout * 4 > (1 + slave_latency) * max_conn_interval
     that corresponds to the following Bluetooth Spec requirement:
     The Supervision_Timeout in milliseconds shall be larger than
     (1 + Conn_Latency) * Conn_Interval_Max * 2, where Conn_Interval_Max is given in milliseconds.
  */
  unsigned int minSupTimeout = (((1+gap_conn_params.slave_latency) * gap_conn_params.max_conn_interval) + 4) >> 2; // round up (ceil)
  if (gap_conn_params.conn_sup_timeout < minSupTimeout)
    gap_conn_params.conn_sup_timeout = minSupTimeout;

  ble_gap_addr_t addr;
  addr = peer_addr;

#if NRF_SD_BLE_API_VERSION<5
  err_code = sd_ble_gap_connect(&addr, &m_scan_param, &gap_conn_params);
#else
  err_code = sd_ble_gap_connect(&addr, &m_scan_param, &gap_conn_params, APP_BLE_CONN_CFG_TAG);
#endif
  JsVar *errStr = jsble_get_error_string(err_code);
  if (errStr) {
    bleCompleteTaskFail(BLETASK_CONNECT, errStr);
    jsvUnLock(errStr);
  }
}

void jsble_central_getPrimaryServices_retry() {
  /* bleTaskInfo = BluetoothDevice */
  jsble_central_getPrimaryServices(jswrap_ble_BluetoothDevice_getHandle(bleTaskInfo), bleUUIDFilter);
}

void jsble_central_getPrimaryServices(uint16_t central_conn_handle, ble_uuid_t uuid) {
  if (central_conn_handle == BLE_CONN_HANDLE_INVALID)
    return bleCompleteTaskFailAndUnLock(BLETASK_PRIMARYSERVICE, jsvNewFromString("Not connected"));

  bleUUIDFilter = uuid;

  uint32_t              err_code;
  err_code = sd_ble_gattc_primary_services_discover(central_conn_handle, 1 /* start handle */, NULL);
  if (err_code == NRF_ERROR_BUSY) {
    // we're busy, so reschedule this for 500ms later
    // https://devzone.nordicsemi.com/f/nordic-q-a/76504/when-can-sd_ble_gattc_primary_services_discover-be-called-nrf_error_busy
    jsvUnLock(jsiSetTimeout(jsble_central_getPrimaryServices_retry, 500));
  } else {
    JsVar *errStr = jsble_get_error_string(err_code);
    if (errStr) {
      bleCompleteTaskFail(BLETASK_PRIMARYSERVICE, errStr);
      jsvUnLock(errStr);
    }
  }
}

void jsble_central_getCharacteristics(uint16_t central_conn_handle, JsVar *service, ble_uuid_t uuid) {
  if (central_conn_handle == BLE_CONN_HANDLE_INVALID)
      return bleCompleteTaskFailAndUnLock(BLETASK_CHARACTERISTIC, jsvNewFromString("Not connected"));

  bleUUIDFilter = uuid;
  ble_gattc_handle_range_t range;
  range.start_handle = jsvObjectGetIntegerChild(service, "start_handle");
  range.end_handle = jsvObjectGetIntegerChild(service, "end_handle");
  bleFinalHandle = range.end_handle;

  uint32_t              err_code;
  err_code = sd_ble_gattc_characteristics_discover(central_conn_handle, &range);
  JsVar *errStr = jsble_get_error_string(err_code);
  if (errStr) {
    bleCompleteTaskFail(BLETASK_CHARACTERISTIC, errStr);
    jsvUnLock(errStr);
  }
}

void jsble_central_characteristicWrite(uint16_t central_conn_handle, JsVar *characteristic, char *dataPtr, size_t dataLen) {
  if (central_conn_handle == BLE_CONN_HANDLE_INVALID)
    return bleCompleteTaskFailAndUnLock(BLETASK_CHARACTERISTIC_WRITE, jsvNewFromString("Not connected"));

  uint16_t handle = jsvObjectGetIntegerChild(characteristic, "handle_value");
  bool writeWithoutResponse = false;
  JsVar *properties = jsvObjectGetChildIfExists(characteristic, "properties");
  if (properties) {
    writeWithoutResponse = jsvObjectGetBoolChild(properties, "writeWithoutResponse");
    jsvUnLock(properties);
  }


  ble_gattc_write_params_t write_params;
  memset(&write_params, 0, sizeof(write_params));
  if (writeWithoutResponse)
    write_params.write_op = BLE_GATT_OP_WRITE_CMD; // write without response
  else
    write_params.write_op = BLE_GATT_OP_WRITE_REQ; // write with response
  // BLE_GATT_OP_WRITE_REQ ===> BLE_GATTC_EVT_WRITE_RSP (write with response)
  // or BLE_GATT_OP_WRITE_CMD ===> BLE_EVT_TX_COMPLETE (simple write)
  // or send multiple BLE_GATT_OP_PREP_WRITE_REQ,...,BLE_GATT_OP_EXEC_WRITE_REQ (with offset + 18 bytes in each for 'long' write)
  write_params.flags    = BLE_GATT_EXEC_WRITE_FLAG_PREPARED_WRITE;
  write_params.handle   = handle;
  write_params.offset   = 0;
  write_params.len      = dataLen;
  write_params.p_value  = (uint8_t*)dataPtr;

  uint32_t              err_code;
  err_code = sd_ble_gattc_write(central_conn_handle, &write_params);
  JsVar *errStr = jsble_get_error_string(err_code);
  if (errStr) {
    bleCompleteTaskFail(BLETASK_CHARACTERISTIC_WRITE, errStr);
    jsvUnLock(errStr);
  }
}

void jsble_central_characteristicRead(uint16_t central_conn_handle, JsVar *characteristic) {
  if (central_conn_handle == BLE_CONN_HANDLE_INVALID)
    return bleCompleteTaskFailAndUnLock(BLETASK_CHARACTERISTIC_READ, jsvNewFromString("Not connected"));

  uint16_t handle = jsvObjectGetIntegerChild(characteristic, "handle_value");
  uint32_t              err_code;
  err_code = sd_ble_gattc_read(central_conn_handle, handle, 0/*offset*/);
  JsVar *errStr = jsble_get_error_string(err_code);
  if (errStr) {
    bleCompleteTaskFail(BLETASK_CHARACTERISTIC_READ, errStr);
    jsvUnLock(errStr);
  }
}

void jsble_central_characteristicDescDiscover(uint16_t central_conn_handle, JsVar *characteristic) {
  if (central_conn_handle == BLE_CONN_HANDLE_INVALID)
    return bleCompleteTaskFailAndUnLock(BLETASK_CHARACTERISTIC_DESC_AND_STARTNOTIFY, jsvNewFromString("Not connected"));

  // start discovery for our single handle only
  uint16_t handle_value = (uint16_t)jsvObjectGetIntegerChild(characteristic, "handle_value");

  ble_gattc_handle_range_t range;
  range.start_handle = handle_value+1;
  range.end_handle = handle_value+1;

  uint32_t              err_code;
  err_code = sd_ble_gattc_descriptors_discover(central_conn_handle, &range);
  JsVar *errStr = jsble_get_error_string(err_code);
  if (errStr) {
    bleCompleteTaskFail(BLETASK_CHARACTERISTIC_DESC_AND_STARTNOTIFY, errStr);
    jsvUnLock(errStr);
  }
}

void jsble_central_characteristicNotify(uint16_t central_conn_handle, JsVar *characteristic, bool enable) {
  if (central_conn_handle == BLE_CONN_HANDLE_INVALID)
    return bleCompleteTaskFailAndUnLock(BLETASK_CHARACTERISTIC_NOTIFY, jsvNewFromString("Not connected"));

  JsVar *cccdVar = jsvObjectGetChildIfExists(characteristic, "handle_cccd");
  if (!cccdVar)
    return bleCompleteTaskFailAndUnLock(BLETASK_CHARACTERISTIC_NOTIFY, jsvNewFromString("handle_cccd not set"));
  uint16_t cccd_handle = jsvGetIntegerAndUnLock(cccdVar);

  uint8_t buf[BLE_CCCD_VALUE_LEN];
  buf[0] = 0;
  buf[1] = 0;

  if (enable) {
    JsVar *properties = jsvObjectGetChildIfExists(characteristic,"properties");
    if (properties && jsvObjectGetBoolChild(properties,"notify")) {
      // use notification if it exists
      buf[0] = BLE_GATT_HVX_NOTIFICATION;
    } else if (properties && jsvObjectGetBoolChild(properties,"indicate")) {
      // otherwise default to indication
      buf[0] = BLE_GATT_HVX_INDICATION;
    } else {
      // no properties, or no notification or indication bit set
    }
    jsvUnLock(properties);
  }

  const ble_gattc_write_params_t write_params = {
      .write_op = BLE_GATT_OP_WRITE_REQ,
      .flags    = BLE_GATT_EXEC_WRITE_FLAG_PREPARED_WRITE,
      .handle   = cccd_handle,
      .offset   = 0,
      .len      = sizeof(buf),
      .p_value  = buf
  };

  uint32_t              err_code;
  err_code = sd_ble_gattc_write(central_conn_handle, &write_params);
  JsVar *errStr = jsble_get_error_string(err_code);
  if (errStr) {
    bleCompleteTaskFail(BLETASK_CHARACTERISTIC_NOTIFY, errStr);
    jsvUnLock(errStr);
  }
}

void jsble_central_startBonding(uint16_t central_conn_handle, bool forceRePair) {
#if PEER_MANAGER_ENABLED
  if (central_conn_handle == BLE_CONN_HANDLE_INVALID)
      return bleCompleteTaskFailAndUnLock(BLETASK_BONDING, jsvNewFromString("Not connected"));

  uint32_t err_code = pm_conn_secure(central_conn_handle, forceRePair);
  JsVar *errStr = jsble_get_error_string(err_code);
  if (errStr) {
    bleCompleteTaskFail(BLETASK_BONDING, errStr);
    jsvUnLock(errStr);
  }
#else
  return bleCompleteTaskFailAndUnLock(BLETASK_BONDING, jsvNewFromString("Peer Manager not compiled in"));
#endif
}

uint32_t jsble_central_send_passkey(uint16_t central_conn_handle, char *passkey) {
#ifdef LINK_SECURITY
  if (central_conn_handle == BLE_CONN_HANDLE_INVALID)
      return BLE_ERROR_INVALID_CONN_HANDLE;
  return sd_ble_gap_auth_key_reply(central_conn_handle, BLE_GAP_AUTH_KEY_TYPE_PASSKEY, (uint8_t*)passkey);
#endif
}

#endif // CENTRAL_LINK_COUNT>0

void jsble_central_setWhitelist(bool whitelist) {
#if PEER_MANAGER_ENABLED
  if (whitelist) {
    bleStatus |= BLE_WHITELIST_ON_BOND;
  } else {
    bleStatus &= ~BLE_WHITELIST_ON_BOND;
    m_whitelist_peer_cnt = 0;
    m_is_wl_changed = true;
    ble_update_whitelist();
  }
#endif
}

void jsble_central_eraseBonds() {
#if PEER_MANAGER_ENABLED
  jsble_check_error(pm_peers_delete());
#endif
}

#if PEER_MANAGER_ENABLED
JsVar *jsble_resolveAddress(JsVar *address) {
  ble_gap_addr_t addr;
  if (bleVarToAddr(address, &addr)) {
    if (addr.addr_type == BLE_GAP_ADDR_TYPE_RANDOM_PRIVATE_RESOLVABLE) {
      pm_peer_data_bonding_t peer_data_bonding;
      memset(&peer_data_bonding, 0, sizeof(peer_data_bonding));
      // iterate over all known peers
      pm_peer_id_t peer_id = PM_PEER_ID_INVALID;
      while ((peer_id = pm_next_peer_id_get(peer_id)) != PM_PEER_ID_INVALID) {
        if (pm_peer_data_bonding_load(peer_id, &peer_data_bonding) == NRF_SUCCESS){
          // address match?
          if (im_address_resolve(&addr, &peer_data_bonding.peer_ble_id.id_info)) {
            return bleAddrToStr(peer_data_bonding.peer_ble_id.id_addr_info);
          }
        }
      }
    }
  }
  return 0;
}
#endif // PEER_MANAGER_ENABLED

#endif // BLUETOOTH


