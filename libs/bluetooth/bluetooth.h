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

#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#ifdef NRF5X
#include "app_config.h"
#endif
#include "jsdevices.h"

#ifdef NRF5X
#if NRF_SD_BLE_API_VERSION>5
#include "nrf_sdh_ble.h"
#define BLE_GAP_ADV_MAX_SIZE BLE_GAP_ADV_SET_DATA_SIZE_MAX
#else
#include "ble.h"
#endif
#include "ble_advdata.h"

/* Check for errors when in an IRQ, when we're pretty sure an error won't
 * cause a hard reset. Error is then reported outside of the IRQ without
 * rebooting Espruino. */
#define APP_ERROR_CHECK_NOT_URGENT(ERR_CODE) if (ERR_CODE) { uint32_t line = __LINE__; jsble_queue_pending_buf(BLEP_ERROR, ERR_CODE, (char*)&line, 4); }

#else // !NRF5X
typedef struct {
  uint16_t uuid;
  uint8_t type;			//see BLE_UUID_TYPE_... definitions
  uint8_t uuid128[16];  //BLE knows 16/32/128 bit uuids. Espruino supports 16/128.
} PACKED_FLAGS ble_uuid_t;
typedef struct {
  //uint8_t addr_id_peer;
  uint8_t addr_type;
  uint8_t addr[6];
} ble_gap_addr_t;
#define BLE_GATT_HANDLE_INVALID (0)
#define BLE_GAP_ADDR_TYPE_PUBLIC (0)
#define BLE_GAP_ADDR_TYPE_RANDOM_STATIC (1)
#define BLE_GAP_ADDR_TYPE_RANDOM_PRIVATE_RESOLVABLE (2)
#define BLE_GAP_ADDR_TYPE_RANDOM_PRIVATE_NON_RESOLVABLE (3)
#define BLE_GAP_ADV_MAX_SIZE (31)
#define BLE_GAP_AD_TYPE_16BIT_SERVICE_UUID_MORE_AVAILABLE   0x02
#define BLE_GAP_AD_TYPE_16BIT_SERVICE_UUID_COMPLETE         0x03
#define BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_MORE_AVAILABLE  0x06
#define BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE        0x07
#define BLE_GAP_AD_TYPE_SERVICE_DATA                        0x16
#define BLE_GAP_AD_TYPE_SERVICE_DATA_128BIT_UUID            0x21
#define BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME                    0x08
#define BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME                 0x09
#define BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA          0xFF
#define BLE_UUID_TYPE_UNKNOWN (0)
#define BLE_UUID_TYPE_BLE (1)
#define BLE_UUID_TYPE_128 2
#define MSEC_TO_UNITS(MS,MEH) MS
#define GATT_MTU_SIZE_DEFAULT 23
#define BLE_NUS_MAX_DATA_LEN 20 //GATT_MTU_SIZE_DEFAULT - 3
#define BLE_CCCD_VALUE_LEN 2
#define BLE_GATT_HVX_NOTIFICATION 1 // flag in CCCD
#define BLE_GATT_HVX_INDICATION 2 // flag in CCCD
#endif //!NRF5X (fudge NRF5X API for ESP32)

#ifndef CENTRAL_LINK_COUNT /**<number of central links used by the application. When changing this number remember to adjust the RAM settings*/
#if defined(NRF52_SERIES) || defined(ESP32) // nRF52 gets the ability to connect to other devices
#define CENTRAL_LINK_COUNT              1
#else
#define CENTRAL_LINK_COUNT              0                                           /**<number of central links used by the application. When changing this number remember to adjust the RAM settings*/
#endif
#endif

#ifndef PERIPHERAL_LINK_COUNT /**<number of peripheral links used by the application. When changing this number remember to adjust the RAM settings*/
#if defined(NRF52_SERIES) || defined(ESP32) // nRF52 gets the ability to connect to other devices
#define PERIPHERAL_LINK_COUNT           1
#else
#define PERIPHERAL_LINK_COUNT           1                                           /**<number of peripheral links used by the application. When changing this number remember to adjust the RAM settings*/
#endif
#endif

#ifndef APP_TIMER_OP_QUEUE_SIZE
#define APP_TIMER_OP_QUEUE_SIZE         2                                           /**< Size of timer operation queues. */
#endif
#define APP_TIMER_PRESCALER             0                                           /**< Value of the RTC1 PRESCALER register. */

#define APP_BLE_CONN_CFG_TAG                1                                       /**< A tag identifying the SoftDevice BLE configuration. */

// BLE HID stuff
#define HID_KEYS_MAX_LEN                     16                                      /**< Maximum length of the Input Report characteristic. */
#define HID_MODIFIER_KEY_POS                 0                                       /**< Position of the modifier byte in the Input Report. */
#define HID_SCAN_CODE_POS                    2                                       /**< This macro indicates the start position of the key scan code in a HID Report. As per the document titled 'Device Class Definition for Human Interface Devices (HID) V1.11, each report shall have one modifier byte followed by a reserved constant byte and then the key scan code. */

#ifndef BLUETOOTH_ADVERTISING_INTERVAL
#define BLUETOOTH_ADVERTISING_INTERVAL 375
#endif

typedef enum  {
  BLE_NONE = 0,
  BLE_IS_SENDING = 1,         //< sending data with jswrap_ble_transmit_string?
  BLE_IS_SCANNING = 2,        //< scanning for BLE devices?
  BLE_IS_ADVERTISING = 4,     //< currently advertising info? stops when connected
  BLE_NEEDS_SOFTDEVICE_RESTART = 8,  //< We need to reset the services we're reporting, but we can't because we're connected
  BLE_SERVICES_WERE_SET = 16, //< setServices was called already, so we need to restart softdevice before we can call it again

  BLE_NUS_INITED = 32,        //< Has the Nordic UART service been initialised?
  BLE_HID_INITED = 64,        //< Has the BLE HID service been initialised?
  BLE_IS_SENDING_HID = 128,   //< Are we waiting to send data for USB HID?
  BLE_IS_RSSI_SCANNING = 256, //< Are we scanning for RSSI values
  BLE_IS_SLEEPING = 512,      //< NRF.sleep has been called
  BLE_PM_INITIALISED = 1<<10,  //< Set when the Peer Manager has been initialised (only needs doing once, even after SD restart)
  BLE_IS_NOT_CONNECTABLE = 1<<11, //< Is the device connectable?
  BLE_IS_NOT_SCANNABLE = 1<<12, //< Is the device scannable? eg, scan response
  BLE_IS_NOT_PAIRABLE = 1<<13, //< Is the device pairable?
  BLE_WHITELIST_ON_BOND = 1<<14,  //< Should we write to the whitelist whenever we bond to a device?
  BLE_DISABLE_DYNAMIC_INTERVAL = 1<<15, //< Disable automatically changing interval based on BLE peripheral activity
  BLE_ENCRYPT_UART = 1<<16,  //< Has security with encryption been requested (if so UART must require it)
  BLE_SECURITY_MITM = 1 << 17, //< Has security with mitm protection been requested (if so UART must require it)
#ifdef ESPR_BLUETOOTH_ANCS
  BLE_ANCS_INITED = 1<<18,   //< Apple Notification Centre enabled
  BLE_AMS_INITED = 1<<19,   //< Apple Media Service enabled
  BLE_CTS_INITED = 1<<20,   //< Apple Notification Centre enabled
  BLE_ANCS_AMS_OR_CTS_INITED = BLE_ANCS_INITED|BLE_AMS_INITED|BLE_CTS_INITED, //< Apple Notifications or Media Service enabled
#endif
#ifndef SAVE_ON_FLASH
  BLE_ADVERTISE_WHEN_CONNECTED = 1<<21, // Do we keep advertising when we're connected?
#endif
  BLE_IS_ADVERTISING_MULTIPLE = 1<<22, // We have multiple different advertising packets
  BLE_ADVERTISING_MULTIPLE_SHIFT = 23,//GET_BIT_NUMBER(BLE_ADVERTISING_MULTIPLE_ONE),
  BLE_ADVERTISING_MULTIPLE_ONE = 1 << BLE_ADVERTISING_MULTIPLE_SHIFT,
  BLE_ADVERTISING_MULTIPLE_MASK = 255 << BLE_ADVERTISING_MULTIPLE_SHIFT,

  /// These are flags that should be reset when the softdevice starts up
  BLE_RESET_ON_SOFTDEVICE_START = BLE_IS_SENDING|BLE_IS_SCANNING|BLE_IS_ADVERTISING
} BLEStatus;

typedef enum  {
  BLE_BOND_REQUEST,
  BLE_BOND_START,
  BLE_BOND_SUCCESS,
  BLE_BOND_FAIL
} BLEBondingStatus;

typedef enum {
  BLEP_NONE,
  BLEP_ERROR,                       //< Softdevice threw some error (code in data)
  BLEP_CONNECTED,                   //< Peripheral connected (address as buffer)
  BLEP_DISCONNECTED,                //< Peripheral disconnected
  BLEP_ADVERTISING_START,           //< Start advertising - do it outside of IRQ because we may need to allocate JsVars
  BLEP_ADVERTISING_STOP,            //< Stop advertising - do it outside of IRQ
  BLEP_RESTART_SOFTDEVICE,          //< Perform a softdevice restart (again, we don't want to do this in an IRQ!)
  BLEP_RSSI_PERIPH,                 //< RSSI data from peripheral connection (rssi as data)
  BLEP_ADV_REPORT,                  //< Advertising received (as buffer)
#if CENTRAL_LINK_COUNT>0
  BLEP_RSSI_CENTRAL,                //< RSSI data from central connection (rssi as data low byte, index in m_central_conn_handles as high byte )
  BLEP_TASK_FAIL,                   //< Task failed because unknown
  BLEP_TASK_FAIL_CONN_TIMEOUT,      //< Task failed becauseConnection timeout
  BLEP_TASK_FAIL_DISCONNECTED,      //< Task failed because disconnected
  BLEP_TASK_CENTRAL_CONNECTED,      //< Central: Connected, (m_central_conn_handles index as data)
  BLEP_TASK_DISCOVER_SERVICE,       //< New service discovered (as buffer)
  BLEP_TASK_DISCOVER_SERVICE_COMPLETE,       //< Service discovery complete
  BLEP_TASK_DISCOVER_CHARACTERISTIC, //< New characteristic discovered (as buffer)
  BLEP_TASK_DISCOVER_CHARACTERISTIC_COMPLETE, //< Characteristic discovery complete
  BLEP_TASK_DISCOVER_CCCD,          //< Discovery of CCCD for characteristic finished (cccd in data)
  BLEP_TASK_CHARACTERISTIC_READ,    //< Central: Characteristic read finished (as buffer)
  BLEP_TASK_CHARACTERISTIC_WRITE,   //< Central: Characteristic write finished
  BLEP_TASK_CHARACTERISTIC_NOTIFY,  //< Central: Started requesting notifications
  BLEP_CENTRAL_NOTIFICATION,        //< A characteristic we were watching has changed
  BLEP_CENTRAL_DISCONNECTED,        //< Central: Disconnected (reason as data low byte, index in m_central_conn_handles as high byte )
#endif
#if PEER_MANAGER_ENABLED
  BLEP_BONDING_STATUS,              //< Bonding negotiation status (data is one of BLEBondingStatus)
#endif
  BLEP_WRITE,                       //< One of our characteristics written by someone else
  BLEP_TASK_PASSKEY_DISPLAY,        //< We're pairing and have been provided with a passkey to display (data = conn_handle)
  BLEP_TASK_AUTH_KEY_REQUEST,       //< We're pairing and the device wants a passkey from us (data = conn_handle)
  BLEP_TASK_AUTH_STATUS,            //< Data on how authentication was going has been received
#ifdef USE_NFC
  BLEP_NFC_STATUS,                  //< NFC changed state
  BLEP_NFC_RX,                      //< NFC data received (as buffer)
  BLEP_NFC_TX,                      //< NFC data sent
#endif
#if BLE_HIDS_ENABLED
  BLEP_HID_SENT,                    //< A HID report has been sent
  BLEP_HID_VALUE,                   //< A HID value was received (eg caps lock)
#endif
#ifdef ESPR_BLUETOOTH_ANCS
  BLEP_ANCS_DISCOVERED,             //< Apple ANCS discovered (need to request notifications)
  BLEP_ANCS_NOTIF,                  //< Apple Notification Centre notification received
  BLEP_ANCS_NOTIF_ATTR,             //< Apple Notification Centre notification attributes received
  BLEP_ANCS_APP_ATTR,               //< Apple Notification Centre app attributes received
  BLEP_ANCS_ERROR,                  //< Apple Notification Centre error - cancel any active tasks
  BLEP_AMS_DISCOVERED,              //< Apple AMS discovered (need to request notifications)
  BLEP_AMS_TRACK_UPDATE,            //< Apple Media Service Track info updated
  BLEP_AMS_PLAYER_UPDATE,           //< Apple Media Service Player info updated
  BLEP_AMS_ATTRIBUTE,               //< Apple Media Service Track or Player info read response
  BLEP_CTS_DISCOVERED,              //< Apple Current Time Service discovered (need to request notifications)
  BLEP_CTS_TIME                     //< Apple Current Time Service data (data = current_time_char_t + optional local_time_char_t)
#endif
} BLEPending;


extern volatile BLEStatus bleStatus;
/// Filter to use when discovering BLE Services/Characteristics
extern ble_uuid_t bleUUIDFilter;

extern uint16_t bleAdvertisingInterval;           /**< The advertising interval (in units of 0.625 ms). */

extern volatile uint16_t                         m_peripheral_conn_handle;    /**< Handle of the current connection. */
#if CENTRAL_LINK_COUNT>0
extern volatile uint16_t                         m_central_conn_handles[CENTRAL_LINK_COUNT]; /**< Handle for central mode connection */
#endif


/// for BLEP_ADV_REPORT
typedef struct {
  ble_gap_addr_t            peer_addr;
  int8_t                    rssi;                  /**< Received Signal Strength Indication in dBm of the last packet received. */
  uint8_t        dlen;                  /**< Advertising or scan response data length. */
  uint8_t        data[BLE_GAP_ADV_MAX_SIZE];    /**< Advertising or scan response data. */
} BLEAdvReportData;

/** Initialise the BLE stack */
void jsble_init();
/** Completely deinitialise the BLE stack. Return true on success */
bool jsble_kill();

/// Checks for error and reports an exception string if there was one, else 0 if no error
JsVar *jsble_get_error_string(uint32_t err_code);

/** Execute a task that was added by jsble_queue_pending - this is done outside of IRQ land. Returns number of events handled */
int jsble_exec_pending(IOEvent *event);

/** Stop and restart the softdevice so that we can update the services in it -
 * both user-defined as well as UART/HID. If jsFunction is a function it is
 * called when the softdevice is uninitialised. */
void jsble_restart_softdevice(JsVar *jsFunction);

uint32_t jsble_advertising_start();
uint32_t jsble_advertising_update_advdata(char *dPtr, unsigned int dLen);
uint32_t jsble_advertising_update_scanresponse(char *dPtr, unsigned int dLen);
void jsble_advertising_stop();

/** Is BLE connected to any device at all? */
bool jsble_has_connection();

/** Is BLE connected to a central device at all? */
bool jsble_has_central_connection();

/** Return the index of the central connection in m_central_conn_handles, or -1 */
int jsble_get_central_connection_idx(uint16_t handle);

/** Is BLE connected to a server device at all (eg, the simple, 'slave' mode)? */
bool jsble_has_peripheral_connection();

/** Call this when something happens on BLE with this as
 * a peripheral - used with Dynamic Interval Adjustment  */
void jsble_peripheral_activity();

#ifndef SAVE_ON_FLASH_EXTREME
#define jsble_check_error(X) jsble_check_error_line(X, __LINE__)
/// Checks for error and reports an exception if there was one. Return true on error
bool jsble_check_error_line(uint32_t err_code, int lineNumber);
#else
/// Checks for error and reports an exception if there was one. Return true on error
bool jsble_check_error(uint32_t err_code);
#endif

/** Set the connection interval of the peripheral connection. Returns an error code */
uint32_t jsble_set_periph_connection_interval(JsVarFloat min, JsVarFloat max);

/// Scanning for advertising packets. options can be an object with optional {active:bool, phy:"1mbps/2mbps/coded"}
uint32_t jsble_set_scanning(bool enabled, JsVar *options);

/// returning RSSI values for current connection
uint32_t jsble_set_rssi_scan(bool enabled);

/** Actually set the services defined in the 'data' object. Note: we can
 * only do this *once* - so to change it we must reset the softdevice and
 * then call this again */
void jsble_set_services(JsVar *data);

/// Disconnect from the given connection
uint32_t jsble_disconnect(uint16_t conn_handle);

/// Start bonding on the peripheral connection, returns a promise
void jsble_startBonding(bool forceRePair);

/// For BLE HID, send an input report to the receiver. Must be <= HID_KEYS_MAX_LEN
void jsble_send_hid_input_report(uint8_t *data, int length);

/// Update the current security settings from the info in hiddenRoot.BLE_NAME_SECURITY
void jsble_update_security();

/// Return an object showing the security status of the given connection
JsVar *jsble_get_security_status(uint16_t conn_handle);

/// Set the transmit power of the current (and future) connections
void jsble_set_tx_power(int8_t pwr);


// ------------------------------------------------- lower-level utility fns

#ifdef NRF5X
/// Build advertising data struct to pass into @ref ble_advertising_init.
void jsble_setup_advdata(ble_advdata_t *advdata);
#endif

#ifdef USE_NFC

#define TAG_HEADER_LEN            0x0A

/*
TT = Tag Type           
ML = NDEF Message Length
RT = Record Type
TF = TNF and Flags
TL = Type Legnth
IC = URI Identifier Code
*/

#define NDEF_HEADER "\x00\x00\x00\x00" /* |      UID/BCC          | */ \
                    "\x00\x00\x00\x00" /* |      UID/BCC          | */ \
                    "\x00\x00\xFF\xFF" /* | UID/BCC |   LOCK      | */ \
                    "\xE1\x11\x7C\x0F" /* |  Cap. Container       | */ \
                    "\x03\x00\x00\x00" /* | TT | ML (1 or 3 bytes)| */ 

#define NDEF_HEADER_LEN_SHORT      0x12 /* with 1 byte length */
#define NDEF_HEADER_LEN_LONG       0x14 /* with 3 byte length */
#define NDEF_HEADER_MSG_LEN_OFFSET 0x11

#define NDEF_URL_RECORD_HEADER \
                    "\xC1\x01"         /* | TF | TL         |  */ \
                    "\x00\x00\x00\x00" /* |  Payload Length |  */ \
                    "\x55\x00"         /* | RT | IC         | 0x00: No prepending */

#define NDEF_URL_RECORD_HEADER_LEN    0x08 /* record header (TF, TL, PL, RT, IC ) */
#define NDEF_IC_LEN               0x01

#define NDEF_MSG_IC_OFFSET         7
#define NDEF_MSG_PL_LEN_MSB_OFFSET 4

#define NDEF_TERM_TLV             0xfe /* last TLV block / byte */
#define NDEF_TERM_TLV_LEN         0x01
#define NDEF_TAG2_VALUE_MAXLEN (992 - 4 - NDEF_TERM_TLV_LEN) /* max NDEF data size for 0x7C size in cap. container (*8=992)*/
void jsble_nfc_stop();
void jsble_nfc_set_atqa(uint16_t data);
void jsble_nfc_set_sak(uint8_t data);
void jsble_nfc_start(const uint8_t *data, size_t len);
void jsble_nfc_get_internal(uint8_t *data, size_t *max_len);
void jsble_nfc_send(const uint8_t *data, size_t len);
void jsble_nfc_send_rsp(const uint8_t data, size_t len);
#endif

#if CENTRAL_LINK_COUNT>0
/** Connect to the given peer address. When done call bleCompleteTask.
 options is an optional object containing optional fields:
 {
   minInterval // min connection interval in milliseconds, 7.5 ms to 4 s
   maxInterval // max connection interval in milliseconds, 7.5 ms to 4 s
 }
 See BluetoothRemoteGATTServer.connect docs for more docs */
void jsble_central_connect(ble_gap_addr_t peer_addr, JsVar *options);
/// Get primary services. Filter by UUID unless UUID is invalid, in which case return all. When done call bleCompleteTask
void jsble_central_getPrimaryServices(uint16_t central_conn_handle, ble_uuid_t uuid);
/// Get characteristics. Filter by UUID unless UUID is invalid, in which case return all. When done call bleCompleteTask
void jsble_central_getCharacteristics(uint16_t central_conn_handle, JsVar *service, ble_uuid_t uuid);
// Write data to the given characteristic. When done call bleCompleteTask
void jsble_central_characteristicWrite(uint16_t central_conn_handle, JsVar *characteristic, char *dataPtr, size_t dataLen);
// Read data from the given characteristic. When done call bleCompleteTask
void jsble_central_characteristicRead(uint16_t central_conn_handle, JsVar *characteristic);
// Discover descriptors of characteristic
void jsble_central_characteristicDescDiscover(uint16_t central_conn_handle, JsVar *characteristic);
// Set whether to notify on the given characteristic. When done call bleCompleteTask
void jsble_central_characteristicNotify(uint16_t central_conn_handle, JsVar *characteristic, bool enable);
/// Start bonding on the current central connection
void jsble_central_startBonding(uint16_t central_conn_handle, bool forceRePair);
/// Get the security status of the current link
JsVar *jsble_central_getSecurityStatus(uint16_t central_conn_handle);
/// RSSI monitoring in central mode
uint32_t jsble_set_central_rssi_scan(uint16_t central_conn_handle, bool enabled);
/// Send a passkey if one was requested (passkey = 6 bytes long)
uint32_t jsble_central_send_passkey(uint16_t central_conn_handle, char *passkey);
#endif
#if PEER_MANAGER_ENABLED
/// Set whether or not the whitelist is enabled
void jsble_central_setWhitelist(bool whitelist);
/// Erase any saved bonding info for peers
void jsble_central_eraseBonds();
/// Try to resolve a bonded peer's address from a random private resolvable address
JsVar *jsble_resolveAddress(JsVar *address);
#endif

#endif // BLUETOOTH_H
