/*
 * The BLE receiver connecting to the device with particular name and subscribing
 * to the characteristic updates. Outputs received data to the hardware serial port.
 * Its based on ../ble_receiver example with the following additions:
 *  1. Output received data to hardware UART port. 
 *  2. Enclose data between the start marker '\1'
 *     and the end marker '\0'. So the receiving application will
 *     be able to split data stream onto packets provided that the data is textual.
 *     Undefine UART_BEGIN and UART_END to disable this feature.
 *  3. Optional hard reset on watchdog timeout for better reliability.
 *  4. Increased MTU
 *
 * Use ../ble_transmitter or ../ble_uart_tx for other side of the connection.
 */

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_task_wdt.h>
#include <driver/uart.h>

#undef  LED_BUILTIN
#define LED_BUILTIN 8

#define DEV_NAME               "TestC3"
#define SERVICE_UUID           "0000ffe0-0000-1000-8000-00805f9b34fb"
#define CHARACTERISTIC_UUID_TX "0000ffe1-0000-1000-8000-00805f9b34fb"
#define SCAN_TIME              5     // sec
#define CONNECT_TOUT           5000  // msec
#define WDT_TIMEOUT            20000 // msec

#define UART_BAUD_RATE 115200
#define UART_MODE SERIAL_8N1
#define UART_RX_PIN  6
#define UART_TX_PIN  7
#define UART_CTS_PIN 5

#define UART_BEGIN '\1'
#define UART_END   '\0'

/*
 The BT stack is complex and not well tested bunch of software. Using it one can easily be trapped onto the state
 where there is no way out. The biggest problem is that connect routine may hung forever. Although the connect call
 has timeout parameter, it does not help. The call may complete on timeout without errors, but the connection will
 not actually be established. That's why we are using watchdog to detect connection timeout. Its unclear if soft
 reset by watchdog is equivalent to the power cycle or reset by pulling low EN pin. That's why there is an option
 to implement hard reset on connect timeout by hard wiring some output pin to EN input of the chip.
*/
// If defined use hard reset on connect timeout
#define RST_OUT_PIN 3

// Undefine to keep default power level
#define TX_PW_BOOST ESP_PWR_LVL_P21

// If defined the receiver will reset itself after being in connected state for the specified time (for testing)
//#define SELF_RESET_AFTER_CONNECTED 60000 // msec

// The remote service we wish to connect to.
static BLEUUID serviceUUID(SERVICE_UUID);
// The characteristic of the remote service we are interested in.
static BLEUUID charUUID(CHARACTERISTIC_UUID_TX);

BLEScan *pBLEScan;
BLEAdvertisedDevice *myDevice;
bool is_scanning;
bool is_connected;
uint32_t connected_ts;

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  // Called for each advertising BLE server.
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    Serial.print("BLE Device found: ");
    Serial.println(advertisedDevice.toString().c_str());
    if (advertisedDevice.getName() == DEV_NAME)
    {
      Serial.println("Peer device found");
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
    }
  }
};

static void reset_self()
{
#ifdef RST_OUT_PIN
  pinMode(RST_OUT_PIN, OUTPUT);
  digitalWrite(RST_OUT_PIN, LOW);
#else
  esp_restart();
#endif
}

void esp_task_wdt_isr_user_handler(void)
{
  reset_self();
}

static inline void watchdog_init()
{
  esp_task_wdt_config_t wdt_cfg = {.timeout_ms = WDT_TIMEOUT, .idle_core_mask = (1<<portNUM_PROCESSORS)-1, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt_cfg); // enable panic so ESP32 restarts
  esp_task_wdt_add(NULL);             // add current thread to WDT watch
}

void setup()
{
  Serial.begin(115200);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Serial1.begin(UART_BAUD_RATE, UART_MODE, UART_RX_PIN, UART_TX_PIN);
#ifdef UART_CTS_PIN
  uart_set_pin(UART_NUM_1, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_CTS_PIN);  
  uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_CTS, 0);
#endif

  watchdog_init();
  BLEDevice::init("");

#ifdef TX_PW_BOOST
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, TX_PW_BOOST); 
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     TX_PW_BOOST);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN,    TX_PW_BOOST);
#endif

  pBLEScan = BLEDevice::getScan(); // create new scan
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);   // active scan uses more power, but get results faster
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);  // less or equal setInterval value
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient *pclient) {
    is_connected = true;
    connected_ts = millis();
    Serial.println("connected");
    digitalWrite(LED_BUILTIN, LOW);
  }
  void onDisconnect(BLEClient *pclient) {
    is_connected = false;
    Serial.println("disconnected");
    digitalWrite(LED_BUILTIN, HIGH);
  }
};

static void notifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify)
{
#ifdef UART_BEGIN
  Serial1.print(UART_BEGIN);
#endif
  Serial1.write(pData, length);
#ifdef UART_END
  Serial1.print(UART_END);
#endif
}

void connectToServer()
{
  Serial.print("Connecting to ");
  Serial.println(myDevice->getAddress().toString().c_str());

  BLEClient *pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  // Connect to the remove BLE Server.
  pClient->connect(myDevice);
  pClient->setMTU(247);  // set client to request maximum MTU from server (default is 23 otherwise)

  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService *pRemoteService = pClient->getService(serviceUUID);
  if (!pRemoteService) {
    Serial.print("Failed to find our service UUID");
    pClient->disconnect();
    return;
  }
  // Obtain a reference to the characteristic in the service of the remote BLE server.
  BLERemoteCharacteristic *pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (!pRemoteCharacteristic) {
    Serial.print("Failed to find our characteristic UUID");
    pClient->disconnect();
    return;
  }
  // Subscribe to updates
  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
  } else {
    Serial.print("Notification not supported by the server");
    pClient->disconnect();
  }
}

void scan_complete_cb(BLEScanResults res)
{
  Serial.print("Devices found: ");
  Serial.println(res.getCount());
  Serial.println("Scan done!");
  is_scanning = false;
}

void loop()
{
  if (!myDevice && !is_scanning) {
    pBLEScan->clearResults();  // delete results fromBLEScan buffer to release memory
    Serial.println("Scanning...");
    pBLEScan->start(SCAN_TIME, scan_complete_cb, true);
    is_scanning = true;
  }
  if (myDevice && !is_scanning && !is_connected)
    connectToServer();

#ifdef SELF_RESET_AFTER_CONNECTED
  if (is_connected && millis() - connected_ts > SELF_RESET_AFTER_CONNECTED) {
    Serial.println("reset itself for testing");
    delay(100);
    reset_self();
  }
#endif

  esp_task_wdt_reset();
}

