#pragma once

#include "usermod_fseq.h"
#include "xlz_unzip.h"
#include "wled.h"

#ifdef WLED_USE_SD_SPI
#include <SD.h>
#include <SPI.h>
#elif defined(WLED_USE_SD_MMC)
#include "SD_MMC.h"
#endif

#include <AsyncUDP.h>
#include <ESPAsyncWebServer.h>

uint16_t FSEQ_refreshFileIndexCache();
int16_t FSEQ_findFileIndexByName(const String &name);
void FSEQ_markFppControlActivity();
void FSEQ_clearFppOverride();
bool FSEQ_isFppOverrideActive();
void FSEQ_invalidateFileIndexCache();

class WriteBufferingStream : public Stream {
public:
  WriteBufferingStream(Stream &upstream, size_t capacity)
      : _upstream(upstream) {
    _capacity = capacity;
    _buffer = (uint8_t *)malloc(capacity);
    _offset = 0;
    if (!_buffer) {
      DEBUG_PRINTLN(F("[WBS] ERROR: Buffer allocation failed"));
    }
  }
  ~WriteBufferingStream() {
    flush();
    if (_buffer) free(_buffer);
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    if (!_buffer) return 0;
    size_t total = 0;
    while (size > 0) {
      size_t space = _capacity - _offset;
      size_t toCopy = (size < space) ? size : space;
      memcpy(_buffer + _offset, buffer, toCopy);
      _offset += toCopy;
      buffer += toCopy;
      size -= toCopy;
      total += toCopy;
      if (_offset == _capacity) flush();
    }
    return total;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  void flush() override {
    if (_offset > 0) {
      _upstream.write(_buffer, _offset);
      _offset = 0;
    }
    _upstream.flush();
  }
  int available() override { return _upstream.available(); }
  int read() override { return _upstream.read(); }
  int peek() override { return _upstream.peek(); }

private:
  Stream &_upstream;
  uint8_t *_buffer = nullptr;
  size_t _capacity = 0;
  size_t _offset = 0;
};

#define FILE_UPLOAD_BUFFER_SIZE 8192
#define CTRL_PKT_SYNC 1
#define CTRL_PKT_PING 4
#define CTRL_PKT_BLANK 3

class UsermodFPP : public Usermod {
private:
  AsyncUDP udp;
  bool udpStarted = false;
  const IPAddress multicastAddr = IPAddress(239, 70, 80, 80);
  const uint16_t udpPort = 32320;

  File currentUploadFile;
  String currentUploadFileName = "";
  unsigned long uploadStartTime = 0;
  WriteBufferingStream *uploadStream = nullptr;

  bool xlzChecked = false;
  unsigned long xlzStartTime = 0;
  bool uploadSessionActive = false;
  bool xlzPendingScan = false;
  bool xlzProcessing = false;
  unsigned long lastUploadActivity = 0;
  unsigned long lastUploadFinished = 0;

  enum PendingCommandType : uint8_t {
    PENDING_NONE = 0,
    PENDING_START,
    PENDING_STOP,
    PENDING_SYNC,
    PENDING_BLANK
  };

  portMUX_TYPE fppMux = portMUX_INITIALIZER_UNLOCKED;
  volatile PendingCommandType pendingCommand = PENDING_NONE;
  char pendingFileName[65] = {0};
  float pendingSecondsElapsed = 0.0f;
  IPAddress lastFppSenderIP = IPAddress(0, 0, 0, 0);

  String getDeviceName() { return String(serverDescription); }

  String buildSystemInfoJSON() {
    DynamicJsonDocument doc(1024);
    String devName = getDeviceName();
    String id = "WLED-" + WiFi.macAddress();
    id.replace(":", "");
    doc["HostName"] = id;
    doc["HostDescription"] = devName;
    doc["Platform"] = "ESP32";
    doc["Variant"] = "WLED";
    doc["Mode"] = "remote";
    doc["Version"] = versionString;
    uint16_t major = 0, minor = 0;
    String ver = versionString;
    int dashPos = ver.indexOf('-');
    if (dashPos > 0) ver = ver.substring(0, dashPos);
    int dotPos = ver.indexOf('.');
    if (dotPos > 0) {
      major = ver.substring(0, dotPos).toInt();
      minor = ver.substring(dotPos + 1).toInt();
    } else {
      major = ver.toInt();
    }
    doc["majorVersion"] = major;
    doc["minorVersion"] = minor;
    doc["typeId"] = 195;
    doc["UUID"] = WiFi.macAddress();
    doc["zip"] = true;
    JsonObject utilization = doc.createNestedObject("Utilization");
    utilization["MemoryFree"] = ESP.getFreeHeap();
    utilization["Uptime"] = millis();
    doc["rssi"] = WiFi.RSSI();
    JsonArray ips = doc.createNestedArray("IPS");
    ips.add(WiFi.localIP().toString());
    String json;
    serializeJson(doc, json);
    return json;
  }

  String buildSystemStatusJSON() {
    DynamicJsonDocument doc(2048);
    JsonObject mqtt = doc.createNestedObject("MQTT");
    mqtt["configured"] = false;
    mqtt["connected"] = false;

    JsonObject currentPlaylist = doc.createNestedObject("current_playlist");
    currentPlaylist["count"] = "0";
    currentPlaylist["description"] = "";
    currentPlaylist["index"] = "0";
    currentPlaylist["playlist"] = "";
    currentPlaylist["type"] = "";

    doc["volume"] = 70;
    doc["media_filename"] = "";
    doc["fppd"] = "running";
    doc["current_song"] = "";

    if (FSEQPlayer::isPlaying()) {
      String fileName = FSEQPlayer::getFileName();
      float elapsedF = FSEQPlayer::getElapsedSeconds();
      uint32_t elapsed = (uint32_t)elapsedF;
      doc["current_sequence"] = fileName;
      doc["playlist"] = "";
      doc["seconds_elapsed"] = String(elapsed);
      doc["seconds_played"] = String(elapsed);
      doc["seconds_remaining"] = "0";
      doc["sequence_filename"] = fileName;
      uint32_t mins = elapsed / 60;
      uint32_t secs = elapsed % 60;
      char timeStr[16];
      snprintf(timeStr, sizeof(timeStr), "%02u:%02u", mins, secs);
      doc["time_elapsed"] = timeStr;
      doc["time_remaining"] = "00:00";
      doc["status"] = 1;
      doc["status_name"] = "playing";
      doc["mode"] = 8;
      doc["mode_name"] = "remote";
    } else {
      doc["current_sequence"] = "";
      doc["playlist"] = "";
      doc["seconds_elapsed"] = "0";
      doc["seconds_played"] = "0";
      doc["seconds_remaining"] = "0";
      doc["sequence_filename"] = "";
      doc["time_elapsed"] = "00:00";
      doc["time_remaining"] = "00:00";
      doc["status"] = 0;
      doc["status_name"] = "idle";
      doc["mode"] = 8;
      doc["mode_name"] = "remote";
    }

    JsonObject adv = doc.createNestedObject("advancedView");
    String devName = getDeviceName();
    String id = "WLED-" + WiFi.macAddress();
    id.replace(":", "");
    adv["HostName"] = id;
    adv["HostDescription"] = devName;
    adv["Platform"] = "WLED";
    adv["Variant"] = "ESP32";
    adv["Mode"] = "remote";
    adv["Version"] = versionString;
    uint16_t major = 0;
    uint16_t minor = 0;
    String ver = versionString;
    int dashPos = ver.indexOf('-');
    if (dashPos > 0) ver = ver.substring(0, dashPos);
    int dotPos = ver.indexOf('.');
    if (dotPos > 0) {
      major = ver.substring(0, dotPos).toInt();
      minor = ver.substring(dotPos + 1).toInt();
    } else {
      major = ver.toInt();
    }
    adv["majorVersion"] = major;
    adv["minorVersion"] = minor;
    adv["typeId"] = 195;
    adv["UUID"] = WiFi.macAddress();
    JsonObject util = adv.createNestedObject("Utilization");
    util["MemoryFree"] = ESP.getFreeHeap();
    util["Uptime"] = millis();
    adv["rssi"] = WiFi.RSSI();
    JsonArray ips = adv.createNestedArray("IPS");
    ips.add(WiFi.localIP().toString());
    String json;
    serializeJson(doc, json);
    return json;
  }

  String buildFppdMultiSyncSystemsJSON() {
    DynamicJsonDocument doc(1024);
    JsonArray systems = doc.createNestedArray("systems");
    JsonObject sys = systems.createNestedObject();
    String devName = getDeviceName();
    String id = "WLED-" + WiFi.macAddress();
    id.replace(":", "");
    sys["hostname"] = devName;
    sys["id"] = id;
    sys["ip"] = WiFi.localIP().toString();
    sys["version"] = versionString;
    sys["hardwareType"] = "WLED";
    sys["type"] = 195;
    sys["num_chan"] = strip.getLength() * 3;
    sys["NumPixelPort"] = 1;
    sys["NumSerialPort"] = 0;
    sys["mode"] = "remote";
    String json;
    serializeJson(doc, json);
    return json;
  }

  void sendPingPacket(IPAddress destination = IPAddress(255, 255, 255, 255)) {
    uint8_t buf[301];
    memset(buf, 0, sizeof(buf));
    buf[0] = 'F'; buf[1] = 'P'; buf[2] = 'P'; buf[3] = 'D';
    buf[4] = 0x04;
    uint16_t dataLen = 294;
    buf[5] = dataLen & 0xFF;
    buf[6] = (dataLen >> 8) & 0xFF;
    buf[7] = 0x03;
    buf[8] = 0x00;
    buf[9] = 0xC3;
    uint16_t versionMajor = 0, versionMinor = 0;
    String ver = versionString;
    int dashPos = ver.indexOf('-');
    if (dashPos > 0) ver = ver.substring(0, dashPos);
    int dotPos = ver.indexOf('.');
    if (dotPos > 0) {
      versionMajor = ver.substring(0, dotPos).toInt();
      versionMinor = ver.substring(dotPos + 1).toInt();
    }
    buf[10] = (versionMajor >> 8) & 0xFF;
    buf[11] = versionMajor & 0xFF;
    buf[12] = (versionMinor >> 8) & 0xFF;
    buf[13] = versionMinor & 0xFF;
    buf[14] = 0x08;
    IPAddress ip = WiFi.localIP();
    buf[15] = ip[0]; buf[16] = ip[1]; buf[17] = ip[2]; buf[18] = ip[3];
    String id = "WLED-" + WiFi.macAddress();
    id.replace(":", "");
    if (id.length() > 64) id = id.substring(0, 64);
    for (int i = 0; i < 64; i++) buf[19 + i] = (i < id.length()) ? id[i] : 0;
    String verStr = versionString;
    for (int i = 0; i < 40; i++) buf[84 + i] = (i < verStr.length()) ? verStr[i] : 0;
    String hwType = "WLED";
    for (int i = 0; i < 40; i++) buf[125 + i] = (i < hwType.length()) ? hwType[i] : 0;
    for (int i = 0; i < 120; i++) buf[166 + i] = 0;
    udp.writeTo(buf, sizeof(buf), destination, udpPort);
  }

  void queuePendingCommand(PendingCommandType cmd,
                           const String &fileName = "",
                           float seconds = 0.0f,
                           IPAddress senderIP = IPAddress(0, 0, 0, 0)) {
    portENTER_CRITICAL(&fppMux);
    pendingCommand = cmd;
    pendingSecondsElapsed = seconds;
    lastFppSenderIP = senderIP;
    memset(pendingFileName, 0, sizeof(pendingFileName));
    if (fileName.length() > 0) {
      String tmp = fileName;
      if (tmp.length() > 64) tmp = tmp.substring(0, 64);
      memcpy(pendingFileName, tmp.c_str(), tmp.length());
    }
    portEXIT_CRITICAL(&fppMux);
  }

  void processUdpPacket(AsyncUDPPacket packet) {
    if (packet.length() < 5) return;
    if (packet.data()[0] != 'F' || packet.data()[1] != 'P' ||
        packet.data()[2] != 'P' || packet.data()[3] != 'D') return;

    uint8_t packetType = packet.data()[4];
    switch (packetType) {
    case CTRL_PKT_SYNC: {
      const size_t baseSize = 17;
      if (packet.length() <= baseSize) {
        DEBUG_PRINTLN(F("[FPP] Sync packet too short, ignoring"));
        break;
      }
      uint8_t syncAction = packet.data()[7];
      float secondsElapsed = 0.0f;
      memcpy(&secondsElapsed, packet.data() + 13, sizeof(secondsElapsed));
      size_t filenameOffset = 17;
      size_t maxFilenameLen = min((size_t)64, packet.length() - filenameOffset);
      char safeFilename[65];
      memcpy(safeFilename, packet.data() + filenameOffset, maxFilenameLen);
      safeFilename[maxFilenameLen] = '\0';

      switch (syncAction) {
        case 0:
          queuePendingCommand(PENDING_START, String(safeFilename), secondsElapsed, packet.remoteIP());
          break;
        case 1:
          queuePendingCommand(PENDING_STOP);
          break;
        case 2:
          queuePendingCommand(PENDING_SYNC, String(safeFilename), secondsElapsed, packet.remoteIP());
          break;
        default:
          break;
      }
      break;
    }
    case CTRL_PKT_PING:
      sendPingPacket(packet.remoteIP());
      break;
    case CTRL_PKT_BLANK:
      queuePendingCommand(PENDING_BLANK);
      break;
    default:
      break;
    }
  }

  void startRealtimeFppPlayback(const String &fileName, float secondsElapsed) {
    String normalized = fileName;
    if (!normalized.startsWith("/")) normalized = "/" + normalized;

    FSEQ_markFppControlActivity();
    realtimeIP = lastFppSenderIP;
    useMainSegmentOnly = false;
    realtimeLock(3000, REALTIME_MODE_FSEQ);
    FSEQPlayer::loadRecording(normalized.c_str(), secondsElapsed, true);
    FSEQPlayer::renderRealtimeFrame();
  }

  void stopRealtimeFppPlayback() {
    FSEQ_clearFppOverride();
    FSEQPlayer::clearLastPlayback();
    exitRealtime();
  }

  void processPendingFppCommand() {
    PendingCommandType cmd;
    char fileName[65];
    float seconds;

    portENTER_CRITICAL(&fppMux);
    cmd = pendingCommand;
    if (cmd == PENDING_NONE) {
      portEXIT_CRITICAL(&fppMux);
      return;
    }
    pendingCommand = PENDING_NONE;
    memcpy(fileName, pendingFileName, sizeof(fileName));
    seconds = pendingSecondsElapsed;
    portEXIT_CRITICAL(&fppMux);

    String fn = String(fileName);
    switch (cmd) {
      case PENDING_START:
        startRealtimeFppPlayback(fn, seconds);
        break;
      case PENDING_STOP:
      case PENDING_BLANK:
        stopRealtimeFppPlayback();
        break;
      case PENDING_SYNC: {
        String normalized = fn;
        if (!normalized.startsWith("/")) normalized = "/" + normalized;
        FSEQ_markFppControlActivity();
        realtimeIP = Network.localIP();
        useMainSegmentOnly = false;
        realtimeLock(3000, REALTIME_MODE_FSEQ);
        if (!FSEQPlayer::isPlaying() || !FSEQPlayer::getFileName().equalsIgnoreCase(normalized.substring(1))) {
          startRealtimeFppPlayback(normalized, seconds);
        } else {
          FSEQPlayer::syncPlayback(seconds);
        }
        break;
      }
      default:
        break;
    }
  }

public:
  static const char _name[];

  void setup() {
    DEBUG_PRINTF("[%s] FPP Usermod loaded\n", _name);
    server.on("/api/system/info", HTTP_GET, [this](AsyncWebServerRequest *request) {
      request->send(200, "application/json", buildSystemInfoJSON());
    });
    server.on("/api/system/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
      request->send(200, "application/json", buildSystemStatusJSON());
    });
    server.on("/api/fppd/multiSyncSystems", HTTP_GET, [this](AsyncWebServerRequest *request) {
      request->send(200, "application/json", buildFppdMultiSyncSystemsJSON());
    });

    server.on("/fpp", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
      [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        uploadSessionActive = true;
        lastUploadActivity = millis();

        if (index == 0) {
          if (uploadStream || currentUploadFile) {
            request->send(409, "text/plain", "Upload already in progress");
            return;
          }
          String fileParam = "";
          if (request->hasParam("filename")) fileParam = request->arg("filename");
          currentUploadFileName = (fileParam != "") ? (fileParam.startsWith("/") ? fileParam : "/" + fileParam) : "/default.fseq";
          if (SD_ADAPTER.exists(currentUploadFileName.c_str())) SD_ADAPTER.remove(currentUploadFileName.c_str());
          currentUploadFile = SD_ADAPTER.open(currentUploadFileName.c_str(), FILE_WRITE);
          if (!currentUploadFile) {
            request->send(500, "text/plain", "File open failed");
            return;
          }
          uploadStream = new WriteBufferingStream(currentUploadFile, FILE_UPLOAD_BUFFER_SIZE);
          uploadStartTime = millis();
        }

        if (uploadStream) uploadStream->write(data, len);

        if (index + len == total) {
          if (uploadStream) {
            uploadStream->flush();
            delete uploadStream;
            uploadStream = nullptr;
          }
          String uploadedFile = currentUploadFileName;
          if (currentUploadFile) currentUploadFile.close();
          String lowerName = uploadedFile;
          lowerName.toLowerCase();
          if (lowerName.endsWith(".xlz")) xlzPendingScan = true;
          else FSEQ_invalidateFileIndexCache();
          lastUploadFinished = millis();
          lastUploadActivity = lastUploadFinished;
          currentUploadFileName = "";
          request->send(200, "text/plain", "Upload complete");
        }
      });

    server.on("/fseqfilelist", HTTP_GET, [](AsyncWebServerRequest *request) {
      DynamicJsonDocument doc(1024);
      JsonArray files = doc.createNestedArray("files");
      File root = SD_ADAPTER.open("/");
      if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
          String name = file.name();
          if (name.endsWith(".fseq") || name.endsWith(".FSEQ")) {
            JsonObject fileObj = files.createNestedObject();
            fileObj["name"] = name;
            fileObj["size"] = file.size();
          }
          file.close();
          file = root.openNextFile();
        }
      } else {
        doc["error"] = "Cannot open SD root directory";
      }
      String json;
      serializeJson(doc, json);
      request->send(200, "application/json", json);
    });

    if (!udpStarted && (WiFi.status() == WL_CONNECTED)) {
      if (udp.listenMulticast(multicastAddr, udpPort)) {
        udpStarted = true;
        udp.onPacket([this](AsyncUDPPacket packet) { processUdpPacket(packet); });
      }
    }
  }

  void loop() {
    if (!udpStarted && (WiFi.status() == WL_CONNECTED)) {
      if (udp.listenMulticast(multicastAddr, udpPort)) {
        udpStarted = true;
        udp.onPacket([this](AsyncUDPPacket packet) { processUdpPacket(packet); });
      }
    }

    processPendingFppCommand();

    if (FSEQ_isFppOverrideActive()) {
      realtimeIP = lastFppSenderIP;
      realtimeLock(3000, REALTIME_MODE_FSEQ);
      FSEQPlayer::renderRealtimeFrame();
    } else if (realtimeMode == REALTIME_MODE_FSEQ && FSEQPlayer::isPlaying()) {
      stopRealtimeFppPlayback();
    }

    if (xlzStartTime == 0) xlzStartTime = millis();
    if (!xlzChecked && (millis() - xlzStartTime >= 2000)) {
      File root = SD_ADAPTER.open("/");
      if (root && root.isDirectory()) {
        root.close();
        if (!FSEQPlayer::isAnyPlaybackActive()) {
          XLZUnzip::processAllPendingXLZ();
          xlzChecked = true;
        }
      } else if (root) {
        root.close();
        xlzChecked = true;
      }
    }

    if (uploadSessionActive && xlzPendingScan && !xlzProcessing) {
      if (millis() - lastUploadActivity >= 10000) {
        if (FSEQPlayer::isAnyPlaybackActive()) return;
        xlzProcessing = true;
        XLZUnzip::processAllPendingXLZ();
        xlzProcessing = false;
        xlzPendingScan = false;
        uploadSessionActive = false;
      }
    }
  }

  uint16_t getId() override { return USERMOD_ID_FPP; }
  void addToConfig(JsonObject &root) override {}
  bool readFromConfig(JsonObject &root) override { return true; }
};

inline const char UsermodFPP::_name[] PROGMEM = "FPP Connect";
