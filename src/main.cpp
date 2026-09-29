#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <qrcode.h>
#include <NimBLEDevice.h>
#include <LittleFS.h>
#include <Preferences.h>

// Audio Libraries
#include "AudioFileSourceLittleFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <TJpg_Decoder.h>

// ==========================================
// 1. CONSTANTS
// ==========================================
//  #define DEVICE_NAME  "Ajeevi_Smart_sound_Box_2"
#define SERVICE_UUID "87654321-4321-4321-4321-cba987654321"
#define WRITE_UUID   "87654321-4321-4321-4321-cba987654322"
#define NOTIFY_UUID  "87654321-4321-4321-4321-cba987654323"

#define ADV_RESTART_DELAY_MS 300

// ==========================================
// 2. GLOBAL OBJECTS
// ==========================================
TFT_eSPI tft = TFT_eSPI();
NimBLEServer* pServer = NULL;
NimBLECharacteristic* pTxCharacteristic = NULL;
Preferences preferences;

// Audio controllers
AudioGeneratorMP3 *mp3 = NULL;
AudioFileSourceLittleFS *file = NULL;
AudioOutputI2S *out = NULL;

// ==========================================
// 3. STATE VARIABLES
// ==========================================
volatile bool commandReceived = false;
String commandData = "";
String baseUPI = "";

// [FIX - RACE CONDITION]: commandData ek String hai aur isko BLE host task
// (onWrite callback) aur Arduino loop() task dono se chhua jaata hai. String
// copy/assignment atomic nahi hota (internally heap pointer + length, multi-
// step operation), to agar dono task ek hi waqt access karein, torn/garbage
// data ban sakta hai. Ek FreeRTOS mutex se isko protect kar rahe hain.
SemaphoreHandle_t commandMutex = NULL;

volatile bool deviceConnected = false;
unsigned long disconnectedAt = 0;
bool advertisingRestartPending = false;

// [NAYA FLAG]: Screensaver ko rokne ke liye
bool isQRShowing = false;

// [FIX]: Agar LittleFS mount fail ho jaaye to loop() ko safe mode me daal do,
// taaki device "silently dead" na dikhe aur baar baar FS calls na kare.
bool fsMountFailed = false;

// ==========================================
// 4. SCREENSAVER
// ==========================================
unsigned long lastActionTime = 0;
unsigned long lastImageChange = 0;
const unsigned long IDLE_TIMEOUT = 30000;
bool isIdleMode = false;
int currentPromoIndex = 1;
const int TOTAL_PROMOS = 2;

// ==========================================
// 5. HELPER FUNCTIONS
// ==========================================

void bleLog(String msg) {
    // 1. Mobile terminal me text ko nayi line par dikhane ke liye \r\n add kiya
    String formattedMsg = msg + "\r\n";
    Serial.print("[LOG] " + formattedMsg);

    if (pServer != NULL && deviceConnected && pTxCharacteristic != NULL) {
        // 2. [EXACT FIX]: Yahan hum NimBLE ko exact lambai (length) bata rahe hain,
        // isse aage-peeche ka koi garbage character nahi aayega!
        pTxCharacteristic->setValue((const uint8_t*)formattedMsg.c_str(), formattedMsg.length());
        pTxCharacteristic->notify();
    }
}

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= tft.height()) return 0;
    tft.pushImage(x, y, w, h, bitmap);
    return 1;
}

// [FIX - QR FAILURE LOGIC BUG]: Pehle yeh function 'void' tha. Agar QR
// generation fail hota (empty text ya text version-14 capacity se bada),
// to function andar hi error screen draw kar ke 'return' kar deta — lekin
// caller ko iska koi pata nahi chalta tha, isliye caller "To Pay" UI, save,
// aur isQRShowing/audio sab kuch success maan kar chalata rehta tha. Ab bool
// return hota hai: true = QR successfully drawn, false = failed (error
// screen already shown internally).
bool displayQR(String text) {
    bleLog("Generating QR for URL: " + text);

    if (text.length() == 0) {
        bleLog("ERROR: Empty UPI text, skipping QR generation");
        return false;
    }

    tft.fillScreen(TFT_WHITE);

    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(14)];

    int8_t qrResult = qrcode_initText(&qrcode, qrcodeData, 14, ECC_LOW, text.c_str());
    if (qrResult != 0) {
        bleLog("ERROR: QR generation failed, text too long for version 14");
        tft.setTextColor(TFT_RED, TFT_WHITE);
        tft.drawCentreString("QR Error: Data too long", 160, 240, 2);
        return false;
    }

    int scale = 4;
    int offset_x = (320 - (qrcode.size * scale)) / 2;
    int offset_y = (480 - (qrcode.size * scale)) / 2 - 20;

    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                tft.fillRect(offset_x + (x * scale), offset_y + (y * scale), scale, scale, TFT_BLACK);
            }
        }
        delay(2);  // Critical for BLE stability
    }

    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.drawCentreString("Scan via any UPI App", 160, offset_y + (qrcode.size * scale) + 20, 2);
    bleLog("QR Generation Success");
    return true;
}

// [FIX]: Helper to fully release the current audio file source.
// Pehle ye logic 3 jagah copy-paste tha; ab ek hi jagah se control hota hai
// taaki "delete kiya par pointer null nahi kiya" jaisi galti kabhi na ho.
void releaseAudioFile() {
    if (file != NULL) {
        delete file;
        file = NULL;
    }
}

void playAudioFile(const char* filepath) {
    // [FIX]: Agar filesystem hi mount nahi hua, to audio attempt karna crash
    // ya undefined behaviour de sakta hai. Safe-mode me silently skip karo.
    if (fsMountFailed) {
        bleLog("ERROR: Cannot play audio, filesystem not mounted");
        return;
    }

    if (mp3 == NULL || out == NULL) {
        bleLog("ERROR: Audio system not initialized");
        return;
    }

    if (!LittleFS.exists(filepath)) {
        bleLog("ERROR: MP3 File '" + String(filepath) + "' not found!");
        return;
    }

    if (mp3->isRunning()) mp3->stop();

    releaseAudioFile();

    file = new AudioFileSourceLittleFS(filepath);

    if (file != NULL && file->isOpen()) {
        // [FIX - MEMORY LEAK]: Pehle yahan mp3->begin() ka return value check
        // nahi hota tha. Agar begin() fail hota (e.g. corrupt mp3), to 'file'
        // allocated reh jaata kyunki mp3->isRunning() false hone ki wajah se
        // loop() ka cleanup code use kabhi free nahi karta tha.
        // Ab hum yahin turant check karke free kar dete hain.
        if (!mp3->begin(file, out)) {
            bleLog("ERROR: mp3->begin() failed for: " + String(filepath));
            mp3->stop();
            releaseAudioFile();
        }
    } else {
        bleLog("ERROR: Failed to open MP3: " + String(filepath));
        releaseAudioFile();
    }
}

// ==========================================
// 6. BLE CALLBACKS (Compatible with most NimBLE versions)
// ==========================================
class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) override {
        deviceConnected = true;
        lastActionTime = millis();
        isIdleMode = false;
        Serial.println("[BLE] Mobile App Connected!");
    }

    void onDisconnect(NimBLEServer* pServer) override {
        deviceConnected = false;
        disconnectedAt = millis();
        advertisingRestartPending = true;
        Serial.println("[BLE] Mobile App Disconnected.");
    }
};

class MyCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic) override {
        std::string rxValue = pCharacteristic->getValue();

        if (rxValue.length() > 0) {
            // [FIX]: Guard against an oversized/garbage payload before doing
            // any String work on it. NimBLE MTU already caps this, but this
            // keeps the parser defensive without changing any command format.
            if (rxValue.length() > 480) {
                Serial.println("[BLE RX] WARNING: Oversized payload ignored");
                return;
            }

            String tempStr = String(rxValue.c_str());
            tempStr.trim();

            if (tempStr.length() == 0) {
                return;
            }

            // [FIX - RACE CONDITION]: Mutex le kar hi shared command state
            // likho. Short timeout (20ms) rakha hai taaki BLE stack kabhi
            // forever block na ho agar loop() kisi wajah se busy ho.
            if (commandMutex != NULL && xSemaphoreTake(commandMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                commandData = tempStr;
                commandReceived = true;
                xSemaphoreGive(commandMutex);
            } else {
                Serial.println("[BLE RX] WARNING: Could not lock command buffer, dropping packet");
                return;
            }

            lastActionTime = millis();
            isIdleMode = false;

            Serial.println("[BLE RX] Data received: " + tempStr);
        }
    }
};

// ==========================================
// 7. SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- Starting Soundbox System ---");

    // [FIX - RACE CONDITION]: Mutex BLE start hone se pehle bana lo, taaki
    // jaise hi NimBLE host task callback fire kare, protection already ready ho.
    commandMutex = xSemaphoreCreateMutex();

    pinMode(4, OUTPUT);
    digitalWrite(4, HIGH);

    Serial.println("[TRACK] TFT Init...");
    tft.init();
    tft.setRotation(2);
    tft.fillScreen(TFT_BLACK);

    Serial.println("[TRACK] JPEG Decoder Setup...");
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(tft_output);

    Serial.println("[TRACK] Mounting LittleFS...");
    if (!LittleFS.begin(true)) {
        // [FIX]: Pehle yahan sirf 'return' hota tha — screen black reh jaati
        // thi aur loop() hamesha ke liye chalta rehta tha bina kisi indication
        // ke. Field me is se "device dead lag raha hai" jaisa confusion hota.
        // Ab hum screen par clear error dikhate hain aur safe-mode flag set
        // karte hain taaki loop() filesystem-dependent kaam skip kare.
        Serial.println("[SYS ERROR] LittleFS Mount Failed!");
        fsMountFailed = true;

        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.drawCentreString("STORAGE ERROR", 160, 220, 4);
        tft.drawCentreString("Contact Support", 160, 260, 2);
        // Intentionally NOT returning: BLE still starts so the device remains
        // discoverable/diagnosable (e.g. for the MAC# command) even in this
        // degraded state, instead of being completely unreachable.
    } else {
        Serial.println("[TRACK] LittleFS Mounted.");
    }

    preferences.begin("soundbox", false);
    baseUPI = preferences.getString("upi_url", "BLANK");
    Serial.println("[SYS] Loaded UPI: " + baseUPI);

    Serial.println("[TRACK] Initializing Audio...");
    out = new AudioOutputI2S();
    out->SetPinout(13, 12, 11);
    mp3 = new AudioGeneratorMP3();

    // ========================================================
    // [VOLUME FIX]: Memory se load karke Cubic curve lagana
    // ========================================================
    int savedVol = preferences.getInt("sys_vol", 100);
    float floatV = savedVol / 100.0;
    out->SetGain(floatV * floatV * floatV);
    Serial.println("[SYS] Loaded Volume: " + String(savedVol) + "%");


   // ========================================================
    // [DYNAMIC BLE NAME]: Hardware MAC Address Extract Karna
    // ========================================================
    uint64_t chipid = ESP.getEfuseMac(); // ESP32 ka unique hardware ID nikalo
    uint8_t* mac = (uint8_t*)&chipid;    // Usko bytes mein tod do
    char dynamicName[35];

    // mac[4] aur mac[5] aakhiri ke do bytes hain (Total 4 characters banenge)
    sprintf(dynamicName, "Ajeevi_QR_Display_%02X%02X", mac[4], mac[5]);

    Serial.println("[TRACK] Initializing NimBLE with Name: " + String(dynamicName));

    // Naya generated naam BLE ko de do
    NimBLEDevice::init(dynamicName);

    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Antenna ko MAX Power (+9dBm) par set kiya
    NimBLEDevice::setMTU(512);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    NimBLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
        WRITE_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    pTxCharacteristic = pService->createCharacteristic(
        NOTIFY_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);


    // Fixed for older NimBLE versions

    // pAdvertising->setMinPreferred(0x06);
    // pAdvertising->setMaxPreferred(0x12);

    // ========================================================
    // [SMOOTHNESS FIX]: Fast Advertising Intervals
    // ========================================================
    // 160 = 100ms, 320 = 200ms. Isse phone turant device dhoondh lega.
    pAdvertising->setMinInterval(160);
    pAdvertising->setMaxInterval(320);

    NimBLEDevice::startAdvertising();
    Serial.println("[BLE] Advertising Started!");

    lastActionTime = millis();

    if (!fsMountFailed) {
        playAudioFile("/welcome.mp3");

        // Initial Screen
        if (LittleFS.exists("/promo1.jpg")) {
            TJpgDec.drawFsJpg(0, 0, "/promo1.jpg", LittleFS);
        } else {
            tft.fillScreen(TFT_BLUE);
            tft.setTextColor(TFT_WHITE, TFT_BLUE);
            tft.drawCentreString("System Ready", 160, 240, 4);
        }
    }
}

// ==========================================
// 8. LOOP
// ==========================================
void loop() {

    // Audio Stream
    if (mp3 != NULL && mp3->isRunning()) {
        if (!mp3->loop()) {
            mp3->stop();
            releaseAudioFile();
        }
    }

    // Advertising Restart
    if (advertisingRestartPending && (millis() - disconnectedAt > ADV_RESTART_DELAY_MS)) {
        advertisingRestartPending = false;
        if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
            NimBLEDevice::startAdvertising();
        }
        Serial.println("[BLE] Advertising Restarted.");
    }

    // Screensaver
    // [FIX]: '!isQRShowing' aur '!fsMountFailed' dono check kiye gaye hain —
    // storage error state me screensaver images load karne ki koshish nahi
    // honi chahiye.
    if (!fsMountFailed && !isIdleMode && !isQRShowing && (millis() - lastActionTime > IDLE_TIMEOUT)) {
        isIdleMode = true;
        lastImageChange = millis() - 6000;
        bleLog("System Idle. Starting Screensaver.");
    }

    if (!fsMountFailed && isIdleMode && (millis() - lastImageChange > 5000)) {
        lastImageChange = millis();

        String imgPath = "/promo" + String(currentPromoIndex) + ".jpg";
        if (LittleFS.exists(imgPath)) {
            TJpgDec.drawFsJpg(0, 0, imgPath, LittleFS);
        } else {
            currentPromoIndex = 1;
            imgPath = "/promo1.jpg";
            if (LittleFS.exists(imgPath)) TJpgDec.drawFsJpg(0, 0, imgPath, LittleFS);
        }
        currentPromoIndex++;
        if (currentPromoIndex > TOTAL_PROMOS) currentPromoIndex = 1;
    }

    // Command Processing
    // [FIX - RACE CONDITION]: Pehle yahan global 'commandData' ko seedha
    // poore processing block me access kiya jaata tha — agar isi beech BLE
    // task se naya onWrite() fire ho jaaye, to mid-processing string change/
    // torn ho sakti thi. Ab hum mutex le kar command ko ek LOCAL copy ('cmd')
    // me nikaal lete hain aur turant mutex chhod dete hain. Baaki sara
    // processing isi local copy par hota hai, jisse global buffer se koi
    // interference nahi ho sakta.
    bool hasCommand = false;
    String cmd = "";

    if (commandMutex != NULL && xSemaphoreTake(commandMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (commandReceived) {
            hasCommand = true;
            cmd = commandData;
            commandReceived = false;
        }
        xSemaphoreGive(commandMutex);
    }

    if (hasCommand) {
        bleLog("Processing Command: " + cmd);

        int upiIndex = cmd.indexOf("upi://");

        if (upiIndex != -1) {
            int endOfUrl = cmd.indexOf("**", upiIndex);
            String upiUrl = "";
            String amount = "0.00";

            if (endOfUrl != -1) {
                upiUrl = cmd.substring(upiIndex, endOfUrl);
                int startOfAmount = endOfUrl + 2;
                int endOfAmount = cmd.indexOf("**", startOfAmount);

                if (endOfAmount != -1) {
                    amount = cmd.substring(startOfAmount, endOfAmount);
                } else {
                    amount = cmd.substring(startOfAmount);
                }
            } else {
                upiUrl = cmd.substring(upiIndex);
            }

            upiUrl.trim();
            amount.trim();
            if (amount.length() == 0) amount = "0.00";

            if (upiUrl.length() > 0)
            {
                // [FIX - QR FAILURE LOGIC BUG]: displayQR() ab bool return
                // karta hai. Pehle iska return value check hi nahi hota tha,
                // isliye QR generation fail hone par bhi (e.g. "Data too long"
                // error already screen par hai) code aage badh kar "To Pay"
                // bar draw kar deta, upi_url ko valid maan kar save kar deta,
                // isQRShowing = true kar deta, aur QR confirmation sound bhi
                // bajata — sab kuch jaise QR safalta se ban gaya ho. Ab hum
                // success ke case me hi yeh sab karte hain.
                bool qrOk = displayQR(upiUrl);

                if (qrOk) {
                    tft.fillRect(0, 420, 320, 60, TFT_BLUE);
                    tft.setTextColor(TFT_WHITE, TFT_BLUE);
                    tft.drawCentreString("To Pay: Rs. " + amount, 160, 435, 4);

                    preferences.putString("upi_url", upiUrl);
                    baseUPI = upiUrl;
                    // [FIX]: QR aa gaya hai, Screensaver band kar do!
                    isQRShowing = true;

                    // ========================================================
                    // [NAYA FEATURE]: QR Code aate hi audio play karein
                    // ========================================================
                    if (!fsMountFailed) {
                        if (LittleFS.exists("/qr.mp3")) {
                            playAudioFile("/qr.mp3");
                        } else if (LittleFS.exists("/QR.mp3")) {
                            playAudioFile("/QR.mp3"); // Capital name fallback
                        }
                    }
                } else {
                    // QR banane me fail hua — displayQR() ne already error
                    // screen dikha diya hai. isQRShowing ko true NAHI karte,
                    // taaki screensaver normally chalta rahe aur "To Pay" bar
                    // ek invalid QR ke upar na chadhe.
                    bleLog("ERROR: QR display failed, payment screen not shown");
                }
            }
        }

         // --- VOLUME COMMAND (Ultra-Smart Parser & Audio Curve Fix) ---
        // startsWith ki jagah indexOf use kiya hai taaki app kisi bhi format me bheje (e.g. SetVol**50)
        else if (cmd.indexOf("VOL") != -1 || cmd.indexOf("Vol") != -1 || cmd.indexOf("vol") != -1) {

            // String mein jahan numbers hain, wahan se sirf digits extract kar lo
            String volStr = "";
            for (int i = 0; i < cmd.length(); i++) {
                if (isDigit(cmd[i])) {
                    volStr += cmd[i];
                }
            }

            if (volStr.length() > 0) {
                int volLevel = volStr.toInt();

                // 1. Clamping (Aawaz ko 0 aur 100 ke beech hi rakhna hai)
                if (volLevel < 0) volLevel = 0;
                if (volLevel > 100) volLevel = 100;

                // Mute condition ko forcefully catch karna
                if (volLevel == 0 || cmd.indexOf("Mute") != -1 || cmd.indexOf("mute") != -1) {
                    volLevel = 0;
                }

                // 2. The Pro-Audio Fix: Cubic Logarithmic Curve
                // Linear volume kaano ko samajh nahi aati, isliye hum isko mathematically curve karenge
                float floatV = volLevel / 100.0;
                float realGain = floatV * floatV * floatV; // Isse 50% actually half sound karega

                if (out != NULL) out->SetGain(realGain); // Hardware I2S pe apply
                preferences.putInt("sys_vol", volLevel); // Memory me save

                bleLog("Volume Updated to: " + String(volLevel) + "%");

                // 3. Screen UI Update
                tft.fillRect(0, 420, 320, 60, TFT_BLUE);
                tft.setTextColor(TFT_WHITE, TFT_BLUE);
                tft.drawCentreString("Volume: " + String(volLevel) + "%", 160, 435, 4);

                // 4. Instant Test Sound (Taaki aapko result turant sunayi de)
                if (!fsMountFailed && LittleFS.exists("/welcome.mp3")) {
                    playAudioFile("/welcome.mp3");
                }
            }
        }

        else if (cmd.indexOf("SUCCESS") != -1 || cmd.indexOf("Success") != -1) {
            // [FIX]: Payment ho gayi, Screensaver timer ko wapas allow kar do
            isQRShowing = false;
            // [TIMING FIX]: Timer ko dhokha do! 30s ki jagah sirf 5s baad screensaver chalega
            lastActionTime = millis() - (IDLE_TIMEOUT - 5000);

            if (!fsMountFailed && LittleFS.exists("/success.jpg")) {
                TJpgDec.drawFsJpg(0, 0, "/success.jpg", LittleFS);
            } else {
                tft.fillScreen(TFT_DARKGREEN);
                tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
                tft.drawCentreString("Payment Successful", 160, 240, 4);
            }
            playAudioFile("/success.mp3");
        }
        else if (cmd.indexOf("FAIL") != -1 || cmd.indexOf("Fail") != -1) {
            // [FIX]: Payment fail, Screensaver wapas allow kar do
            isQRShowing = false;
            // [TIMING FIX]: Timer ko dhokha do! 30s ki jagah sirf 5s baad screensaver chalega
            lastActionTime = millis() - (IDLE_TIMEOUT - 5000);

            if (!fsMountFailed && LittleFS.exists("/fail.jpg")) {
                TJpgDec.drawFsJpg(0, 0, "/fail.jpg", LittleFS);
            } else {
                tft.fillScreen(TFT_RED);
                tft.setTextColor(TFT_WHITE, TFT_RED);
                tft.drawCentreString("Payment Failed", 160, 240, 4);
            }
            playAudioFile("/fail.mp3");
        }
        else if (cmd == "FACTORY#") {
            // [FIX]: Reset hone par screensaver allow kar do
            isQRShowing = false;
            bleLog("Factory Reset...");
            preferences.clear();
            baseUPI = "BLANK";
            //esp_restart();

            // [FIX]: preferences.clear() volume ko bhi reset kar deta tha NVS
            // me, lekin live audio gain (out->SetGain) wahi purana rehta tha.
            // Ab hum reset ke baad gain ko bhi default 100% par wapas le aate
            // hain taaki state consistent rahe.
            if (out != NULL) out->SetGain(1.0f);

            tft.fillScreen(TFT_RED);
            tft.setTextColor(TFT_WHITE, TFT_RED);
            tft.drawCentreString("FACTORY RESET DONE", 160, 220, 4);
            tft.drawCentreString("Device Unmapped", 160, 260, 2);
        }
        // ========================================================
        // [NAYA FEATURE]: MAC Address Request Command
        // ========================================================

        else if (cmd == "MAC#" || cmd == "mac#") {
            uint64_t chipid = ESP.getEfuseMac(); // ESP32 ka hardware ID
            uint8_t* mac = (uint8_t*)&chipid;    // Bytes mein convert kiya

            char macStr[40];
            // Yahan se saare colons (:) hata diye hain, ab ye ek plain line me print hoga
            sprintf(macStr, "MAC :  %02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

            // Terminal par bhej diya
            bleLog(String(macStr));
        }
        else {
            bleLog("WARNING: Unknown command: " + cmd);
        }
    }
}