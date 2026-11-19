/*
  Rookmachine-besturing (JB Systems FX 1700 ombouw) - ESP32 + BLE
  ----------------------------------------------------------------
  Vervangt de FM-3C print:
    - temperatuurregeling verwarmingsblok (thermokoppel + MAX6675 -> SSR)
    - pompbesturing via relais, alleen als blok op temperatuur is
    - vlotter/tankniveau, ready-led, heating-led
    - bediening via Bluetooth Low Energy (Nordic UART Service)

  Bibliotheek: "MAX6675 library" van Adafruit (Library Manager).
  Voeler: K-type thermokoppel (getest: min-draad magnetisch). Plus = rood-gemarkeerde
  draad -> MAX6675 "+", min (magnetisch) -> MAX6675 "-".
  Gemeten: opwarmen ~30 C/min, doorschieten na uitschakelen ~7-8 C.
  Board: ESP32 Dev Module (Arduino-ESP32 core 2.x of 3.x).

  BLE-commando's (tekst, naar RX-characteristic):
    F            -> rook geven (houd-ingedrukt: elke ~300 ms opnieuw sturen)
    X            -> rookknop losgelaten (direct stoppen, timer blijft)
    S            -> stop (pomp uit, timer uit)
    R            -> fout resetten
    SP:250       -> setpunt in graden C
    TM:60,5      -> timermodus: elke 60 s, 5 s rook   (TM:0 = timer uit)
    H1 / H0      -> verwarming aan / standby
  Status (notify op TX, elke 500 ms), JSON:
    {"t":245.5,"sp":250,"ready":1,"heat":1,"pump":0,"tank":1,"fault":"","tm":0}

  VEILIGHEID: de thermische beveiliging (klixon/zekering) op het blok blijft
  ALTIJD in serie met het element. Deze software is geen vervanging daarvoor.
*/

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <max6675.h>
#include <Preferences.h>   // setpunt bewaren in flashgeheugen (blijft na stroom eraf)

Preferences prefs;

// ======================= INSTELLINGEN =======================
// Pinnen
const int PIN_SSR       = 27;   // via NPN-transistor naar SSR-ingang
const int PIN_PUMP      = 26;   // relaismodule IN
const int PIN_LEVEL     = 33;   // vlotter (andere kant naar GND)
const int PIN_LED_READY = 25;   // groene led voorkant
const int PIN_LED_HEAT  = 32;   // optionele heating-led
const int PIN_TC_SCK    = 18;
const int PIN_TC_CS     = 5;
const int PIN_TC_SO     = 19;

// Hardware-gedrag (aanpassen na meten)
const bool PUMP_ACTIVE_LOW        = false;  // relaismodule met jumper op H (high-actief)
const bool SSR_ACTIVE_HIGH        = true;   // met NPN-transistor: HIGH = aan
const bool TANK_OK_WHEN_CLOSED    = false;  // gemeten: volle tank = open, dus gesloten = LEEG
const bool USE_LEVEL_SENSOR       = true;   // reed-contact werkt (0,4 Ohm met magneet)
                                            // let op: losse draad = open = "tank ok"

// Temperatuur (graden C) - start voorzichtig, daarna afstemmen
float setpoint               = 230.0;  // doeltemperatuur blok
const float SP_MIN           = 150.0;
const float SP_MAX           = 300.0;
const float HYST             = 4.0;    // schakelbandbreedte
const float OVERSHOOT        = 6.0;    // gemeten doorschieten ~7-8 C: element eerder uit
                                       // element uit bij SP-OVERSHOOT, aan onder SP-OVERSHOOT-HYST
const float READY_ON_MARGIN  = 10.0;   // ready vanaf SP-10
const float READY_OFF_MARGIN = 30.0;   // niet meer ready onder SP-30
const float TEMP_ABS_MAX     = 330.0;  // daarboven: fout, alles uit

// Pomp
const uint32_t FOG_KEEPALIVE_MS = 800;    // zonder nieuw 'F' binnen deze tijd: stop
const uint32_t FOG_MAX_MS       = 20000;  // max. aaneengesloten rookstoot
const uint32_t FOG_COOLDOWN_MS  = 8000;   // verplichte pauze na max. stoot

// Opwarm-watchdog: element aan maar temperatuur stijgt niet -> voeler los?
const uint32_t HEAT_CHECK_MS    = 180000; // 3 minuten
const float    HEAT_CHECK_RISE  = 8.0;    // minimaal zoveel graden stijging
// ============================================================

MAX6675 tc(PIN_TC_SCK, PIN_TC_CS, PIN_TC_SO);

// BLE Nordic UART Service
#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
BLECharacteristic *txChar = nullptr;
volatile bool bleConnected = false;

// Toestand
float    tempC        = NAN;
bool     heaterEnabled = true;
bool     heaterOn     = false;
bool     ready        = false;
bool     pumpOn       = false;
bool     tankOk       = true;
String   fault        = "";

uint32_t lastFogCmd   = 0;
uint32_t fogStart     = 0;
uint32_t cooldownUntil = 0;
bool     fogRequested = false;

uint32_t timerInterval = 0;  // s, 0 = uit
uint32_t timerDuration = 0;  // s
uint32_t timerNext     = 0;
uint32_t timerFogUntil = 0;

uint32_t heatCheckStart = 0;
float    heatCheckTemp  = NAN;

// Ontvangen commando's worden in de loop verwerkt (niet in BLE-callback)
String   pendingCmd;
portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- uitgangen ----------------
void setPump(bool on) {
  pumpOn = on;
  digitalWrite(PIN_PUMP, (on ^ PUMP_ACTIVE_LOW) ? HIGH : LOW);
}
void setHeater(bool on) {
  if (on && !heaterOn) { heatCheckStart = millis(); heatCheckTemp = tempC; }
  heaterOn = on;
  digitalWrite(PIN_SSR, (on == SSR_ACTIVE_HIGH) ? HIGH : LOW);
}
void allOff() { setPump(false); setHeater(false); }

void setFault(const String &f) {
  if (fault.length() == 0) Serial.println("FOUT: " + f);
  fault = f;
  allOff();
  ready = false;
  fogRequested = false;
  timerInterval = 0;
}

// ---------------- BLE ----------------
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { bleConnected = true; }
  void onDisconnect(BLEServer *s) override {
    bleConnected = false;
    fogRequested = false;           // verbinding weg: geen rook meer
    BLEDevice::startAdvertising();
  }
};
class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue().c_str();
    v.trim();
    portENTER_CRITICAL(&cmdMux);
    pendingCmd = v;
    portEXIT_CRITICAL(&cmdMux);
  }
};

void setupBLE() {
  // Unieke naam per machine (laatste 4 hex van het MAC-adres), zodat de app
  // meerdere rookmachines uit elkaar kan houden: bijv. "Rookmachine-3F2A"
  uint64_t mac = ESP.getEfuseMac();
  char bleName[24];
  snprintf(bleName, sizeof(bleName), "Rookmachine-%04X", (unsigned)((mac >> 32) & 0xFFFF));
  BLEDevice::init(bleName);
  Serial.printf("Bluetooth-naam: %s\n", bleName);
  BLEDevice::setPower(ESP_PWR_LVL_P9);       // maximaal zendvermogen (+9 dBm) voor meer bereik
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  BLEService *svc = server->createService(NUS_SERVICE);
  txChar = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());
  BLECharacteristic *rx = svc->createCharacteristic(
      NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void sendStatus() {
  if (!bleConnected || !txChar) return;
  char buf[320];
  int32_t cdLeft = (int32_t)(cooldownUntil - millis());
  float cd = cdLeft > 0 ? cdLeft / 1000.0f : 0;
  int n = snprintf(buf, sizeof(buf),
    "{\"t\":%.1f,\"sp\":%.0f,\"ready\":%d,\"heat\":%d,\"pump\":%d,\"tank\":%d,\"en\":%d,\"fault\":\"%s\",\"tm\":%lu,\"td\":%lu,\"cd\":%.1f,\"mn\":%.0f,\"mx\":%.0f}",
    isnan(tempC) ? -1.0 : tempC, setpoint, ready, heaterOn, pumpOn, tankOk,
    heaterEnabled, fault.c_str(), (unsigned long)timerInterval, (unsigned long)timerDuration, cd, SP_MIN, SP_MAX);
  if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
  // In stukjes van 20 bytes sturen: dat past altijd in één Bluetooth-pakket,
  // ook als de telefoon geen groter pakket afspreekt. De app plakt ze weer aan elkaar.
  for (int i = 0; i < n; i += 20) {
    int len = min(20, n - i);
    txChar->setValue((uint8_t *)buf + i, len);
    txChar->notify();
    delay(4);
  }
}

// ---------------- commando's ----------------
void handleCommand(String cmd) {
  if (cmd.length() == 0) return;
  uint32_t now = millis();

  if (cmd == "F") {
    if (!fogRequested) fogStart = now;
    fogRequested = true;
    lastFogCmd = now;
  } else if (cmd == "X") {                   // handmatige rook loslaten
    fogRequested = false;
  } else if (cmd == "S") {
    fogRequested = false;
    timerInterval = 0;
    timerFogUntil = 0;
  } else if (cmd.startsWith("SP:")) {
    float v = cmd.substring(3).toFloat();
    if (v >= SP_MIN && v <= SP_MAX && v != setpoint) {
      setpoint = v;
      prefs.putFloat("sp", setpoint);        // alleen schrijven bij een echte wijziging
    }
  } else if (cmd.startsWith("TM:")) {
    String a = cmd.substring(3);
    int comma = a.indexOf(',');
    uint32_t iv = a.toInt();
    uint32_t du = comma > 0 ? a.substring(comma + 1).toInt() : 0;
    if (iv == 0 || du == 0) { timerInterval = 0; timerFogUntil = 0; }
    else {
      timerInterval = constrain(iv, 5UL, 3600UL);
      timerDuration = constrain(du, 1UL, FOG_MAX_MS / 1000);
      timerNext = now;                       // direct eerste stoot (als ready)
    }
  } else if (cmd == "H1") {
    heaterEnabled = true;
  } else if (cmd == "H0") {
    heaterEnabled = false;
    fogRequested = false;
  } else if (cmd == "R") {                   // fout resetten
    fault = "";
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_SSR, OUTPUT);
  pinMode(PIN_LED_READY, OUTPUT);
  pinMode(PIN_LED_HEAT, OUTPUT);
  pinMode(PIN_LEVEL, INPUT_PULLUP);
  allOff();                                  // eerst alles veilig uit
  prefs.begin("rook", false);                // eigen setpunt van deze machine ophalen
  setpoint = constrain(prefs.getFloat("sp", setpoint), SP_MIN, SP_MAX);
  Serial.printf("Setpunt uit geheugen: %.0f C (max %.0f C)\n", setpoint, SP_MAX);
  delay(500);                                // MAX6675 opstarttijd
  setupBLE();
  Serial.println("Rookmachine gestart");
}

void loop() {
  uint32_t now = millis();

  // Commando ophalen
  String cmd;
  portENTER_CRITICAL(&cmdMux);
  cmd = pendingCmd; pendingCmd = "";
  portEXIT_CRITICAL(&cmdMux);
  handleCommand(cmd);

  // Temperatuur lezen (MAX6675 max ~4x per seconde)
  static uint32_t lastRead = 0;
  if (now - lastRead >= 300) {
    lastRead = now;
    float t = tc.readCelsius();
    tempC = t;
    if (isnan(t))               setFault("thermokoppel los");
    else if (t > TEMP_ABS_MAX)  setFault("te heet");
  }

  // Tankniveau (simpele ontdendering)
  static uint8_t levelCount = 0;
  bool closed = digitalRead(PIN_LEVEL) == LOW;
  bool okNow = !USE_LEVEL_SENSOR || (closed == TANK_OK_WHEN_CLOSED);
  if (okNow != tankOk) { if (++levelCount > 20) { tankOk = okNow; levelCount = 0; } }
  else levelCount = 0;

  if (fault.length() == 0) {
    // --- temperatuurregeling (aan/uit met hysterese) ---
    if (!heaterEnabled)                    setHeater(false);
    else if (tempC >= setpoint - OVERSHOOT)          setHeater(false);
    else if (tempC <= setpoint - OVERSHOOT - HYST)   setHeater(true);

    // --- opwarm-watchdog ---
    if (heaterOn && now - heatCheckStart > HEAT_CHECK_MS) {
      if (tempC - heatCheckTemp < HEAT_CHECK_RISE && tempC < setpoint - OVERSHOOT - HYST - 10)
        setFault("temperatuur stijgt niet (voeler los of element defect?)");
      heatCheckStart = now; heatCheckTemp = tempC;
    }

    // --- ready met hysterese ---
    if (!ready && tempC >= setpoint - READY_ON_MARGIN)  ready = true;
    if (ready  && tempC <  setpoint - READY_OFF_MARGIN) ready = false;
    if (!heaterEnabled) ready = false;

    // --- handmatige rook: keepalive verlopen? ---
    if (fogRequested && now - lastFogCmd > FOG_KEEPALIVE_MS) fogRequested = false;

    // --- timermodus ---
    bool timerFog = false;
    if (timerInterval > 0) {
      if ((int32_t)(now - timerNext) >= 0) {
        timerNext = now + timerInterval * 1000UL;
        if (ready && tankOk) timerFogUntil = now + timerDuration * 1000UL;
      }
      timerFog = (int32_t)(timerFogUntil - now) > 0;
    }

    // --- pompbeslissing ---
    bool want = (fogRequested || timerFog);
    bool cooling = (int32_t)(cooldownUntil - now) > 0;
    bool allowed = ready && tankOk && !cooling;

    if (want && allowed) {
      if (!pumpOn) fogStart = now;
      if (now - fogStart > FOG_MAX_MS) {     // te lange stoot
        setPump(false);
        fogRequested = false;
        timerFogUntil = 0;
        cooldownUntil = now + FOG_COOLDOWN_MS;
      } else {
        setPump(true);
      }
    } else {
      setPump(false);
    }
  }

  // --- leds ---
  bool blinkSlow = (now / 500) % 2;
  bool blinkFast = (now / 120) % 2;
  if (fault.length())          digitalWrite(PIN_LED_READY, blinkFast);
  else if (!tankOk)            digitalWrite(PIN_LED_READY, blinkFast);
  else if (ready)              digitalWrite(PIN_LED_READY, HIGH);
  else if (heaterEnabled)      digitalWrite(PIN_LED_READY, blinkSlow);
  else                         digitalWrite(PIN_LED_READY, LOW);
  digitalWrite(PIN_LED_HEAT, heaterOn);

  // --- status ---
  static uint32_t lastStatus = 0;
  if (now - lastStatus >= 500) {
    lastStatus = now;
    sendStatus();
    Serial.printf("T=%.1f SP=%.0f heat=%d ready=%d pump=%d tank=%d %s\n",
                  tempC, setpoint, heaterOn, ready, pumpOn, tankOk, fault.c_str());
  }
  delay(5);
}
