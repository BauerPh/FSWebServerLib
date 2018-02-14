// FSWebServerLib.h

#ifndef _FSWEBSERVERLIB_h
#define _FSWEBSERVERLIB_h

#if defined(ARDUINO) && ARDUINO >= 100
#include "Arduino.h"
#else
#include "WProgram.h"
#endif

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <TimeLib.h>
#include <NtpClientLib.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESP8266mDNS.h>
#include <StreamString.h>
#include <FS.h>
#include <Ticker.h>
#include <ArduinoOTA.h>
#include <JSONtoSPIFFS.h>

#define RELEASE  // Comment to enable debug output

#define DBG_OUTPUT_PORT Serial

#ifndef RELEASE
#define DEBUGLOG(...) DBG_OUTPUT_PORT.printf(__VA_ARGS__)
#else
#define DEBUGLOG(...)
#endif

#define CONNECTION_LED 15 // Connection LED pin (LOW when connected!!!). -1 to disable

#define HIDE_SECRET
//#define HIDE_CONFIG
#define CONFIG_FILE "config_WebServerLib.json"
#define SECRET_FILE "config_HTTPAuth.json"

#define JSON_CALLBACK_SIGNATURE std::function<void(AsyncWebServerRequest *request)> jsoncallback
#define REST_CALLBACK_SIGNATURE std::function<void(AsyncWebServerRequest *request)> restcallback
#define POST_CALLBACK_SIGNATURE std::function<void(AsyncWebServerRequest *request)> postcallback

#define RESTART_CALLBACK_SIGNATURE std::function<void()> restartcallback
#define SAVE_CONFIG_CALLBACK_SIGNATURE std::function<void()> saveconfigcallback
#define UPDATE_CALLBACK_SIGNATURE std::function<void(bool upd, bool error, bool updatePossible, enumFirmwareLastError lastError, const String &serverVersion, const uint32_t &updateSize)> updatecallback

typedef struct {
	String ssid;
	String password;
	IPAddress  ip;
	IPAddress  netmask;
	IPAddress  gateway;
	IPAddress  dns;
	bool dhcp;
	String ntpServerName;
	long updateNTPTimeEvery;
	long timezone;
	bool daylight;
	String deviceName;
	bool startAP = false;
} strConfig;

typedef struct {
	String APssid = "ESP"; // ChipID is appended to this name
	bool APenable = false; // AP disabled by default
} strApConfig;

typedef struct {
	bool auth;
	String wwwUsername;
	String wwwPassword;
} strHTTPAuth;

typedef enum {
	FW_IDLE,
	FW_REQ_AV_PENDING,
	FW_RECV_AV_PENDING,
	FW_REQ_BIN_PENDING,
	FW_RECV_BIN_PENDING,
	FW_UPDATE_RUNNING,
	FW_NO_UPDATE,
	FW_ERROR
} enumFirmwareState;

typedef enum {
	FW_ERROR_NONE,
	HTTP_ERROR_INVALID_PARSESTATE,
	HTTP_ERROR_INVALID_RESPONSE,
	HTTP_ERROR_INVALID_STATUSCODE,
	HTTP_ERROR_INVALID_HEADER,
	HTTP_ERROR_CONNECT_FAILED,
	HTTP_ERROR_SERVER_DISCONNECTED,
	FW_ERROR_NO_VERSION_FOR_MODEL,
	FW_ERROR_BEGIN_UPDATE,
	FW_ERROR_END_UPDATE,
	FW_ERROR_SPIFFS,
	FW_ERROR_FW_STATE
} enumFirmwareLastError;

typedef struct {
	String server;
	String path;
	bool updateAvailable = false;
	bool updatePossible = false;
	enumFirmwareState state = FW_IDLE;
	enumFirmwareLastError lastError = FW_ERROR_NONE;
	String serverVersion = "0.0";
	String clientVersion = "0.0";
	String modelName = "Default";
	String serverMD5 = "";
	uint32_t updateSize = 0;
	uint32_t actSize = 0;
	bool updSpiffs = false;
	bool rcvdSpiffs = false;
	bool startFWupdate = false;
} strFirmware;

class AsyncFSWebServer : public AsyncWebServer {
public:
	AsyncFSWebServer(uint16_t port);
	void begin(FS* fs);
	void handle();
	const char* getHostName();

	void checkUpdate();
	bool runUpdate();

	void setModelName(String s);
	void setVersionString(String s);

	bool factoryReset(bool all);

	void setJSONCallback(JSON_CALLBACK_SIGNATURE);
	void setRESTCallback(REST_CALLBACK_SIGNATURE);
	void setPOSTCallback(POST_CALLBACK_SIGNATURE);
	void setRestartCallback(RESTART_CALLBACK_SIGNATURE);
	void setSaveConfigCallback(SAVE_CONFIG_CALLBACK_SIGNATURE);
	void setUpdateCallback(UPDATE_CALLBACK_SIGNATURE);

private:
	JSON_CALLBACK_SIGNATURE;
	REST_CALLBACK_SIGNATURE;
	POST_CALLBACK_SIGNATURE;
	RESTART_CALLBACK_SIGNATURE;
	SAVE_CONFIG_CALLBACK_SIGNATURE;
	UPDATE_CALLBACK_SIGNATURE;

protected:
	strConfig _config; // General and WiFi configuration
	strApConfig _apConfig; // Static AP config settings
	strHTTPAuth _httpAuth;
	strFirmware _firmware;
	FS* _fs;
	long wifiDisconnectedSince = 0;
	String _browserMD5 = "";
	uint32_t _updateSize = 0;
	bool _wifiWasConnected = false;
	bool _restartESP = false;

	JSONtoSPIFFS _ConfigFileHandler;

	WiFiEventHandler onStationModeConnectedHandler, onStationModeDisconnectedHandler, onSoftAPModeStationConnectedHandler, onSoftAPModeStationDisconnectedHandler;
	
	AsyncEventSource _evs = AsyncEventSource("/events");
	AsyncEventSource _evsUpd = AsyncEventSource("/updEvents");
	AsyncClient* _asyncClient = NULL;

	void sendTimeData();
	bool load_config();
	void defaultConfig();
	bool save_config();
	bool save_startAP(bool value);
	bool loadHTTPAuth();
	bool saveHTTPAuth();
	void configureWifiAP();
	void configureWifi();
	void configureOTA(String password);
	void serverInit();

	Ticker _APTimeout;
	int _WiFiAPConnectedClients;
	void onWiFiConnected(WiFiEventStationModeConnected data);
	void onWiFiDisconnected(WiFiEventStationModeDisconnected data);
	void onWiFiAPClientConnected(WiFiEventSoftAPModeStationConnected data);
	void onWiFiAPClientDisconnected(WiFiEventSoftAPModeStationDisconnected data);

	Ticker _secondTk;
	static void s_secondTick(void* arg);

	String getMacAddress();

	bool checkAuth(AsyncWebServerRequest *request);
	void handleFileList(AsyncWebServerRequest *request);
	bool handleFileRead(String path, AsyncWebServerRequest *request);
	void handleFileCreate(AsyncWebServerRequest *request);
	void handleFileDelete(AsyncWebServerRequest *request);
	void handleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
	void send_network_configuration_values_html(AsyncWebServerRequest *request);
	void send_connection_state_values_html(AsyncWebServerRequest *request);
	void send_information_values_html(AsyncWebServerRequest *request);
	void send_NTP_configuration_values_html(AsyncWebServerRequest *request);
	void evaluate_network_post_html(AsyncWebServerRequest *request);
	void evaluate_NTP_post_html(AsyncWebServerRequest *request);
	void send_system_configuration_values_html(AsyncWebServerRequest *request);
	void evaluate_system_post_html(AsyncWebServerRequest *request);

	void sendUpdateData();
	void checkFirmware();
	void updateFirmware(bool updSpiffs);
	
	bool _restartPending;
	Ticker _restartESPTicker;
	static void restartESP(void* arg);
	static void s_restartESP(void* arg);
	
	Ticker _LEDTk;
	static void s_toggleLED();

	static String urldecode(String input);
	static unsigned char h2int(char c);
	static boolean checkRange(String Value);
	static String formatBytes(size_t bytes);
};

extern AsyncFSWebServer ESPHTTPServer;

#endif // _FSWEBSERVERLIB_h