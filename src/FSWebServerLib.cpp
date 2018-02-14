#include "FSWebServerLib.h"

AsyncFSWebServer ESPHTTPServer(80);

AsyncFSWebServer::AsyncFSWebServer(uint16_t port) : AsyncWebServer(port) {}

void AsyncFSWebServer::s_secondTick(void* arg) {
	AsyncFSWebServer* self = reinterpret_cast<AsyncFSWebServer*>(arg);
	if (self->_evs.count() > 0) {
		self->sendTimeData();
	}
}

void AsyncFSWebServer::sendTimeData() {
	String data = "{";
	data += "\"time\":\"" + NTP.getTimeStr() + "\",";
	data += "\"date\":\"" + NTP.getDateStr() + "\",";
	data += "\"lastSync\":\"" + NTP.getTimeDateString(NTP.getLastNTPSync()) + "\",";
	data += "\"uptime\":\"" + NTP.getUptimeString() + "\",";
	data += "\"lastBoot\":\"" + NTP.getTimeDateString(NTP.getLastBootTime()) + "\"";
	data += "}\r\n";
	DEBUGLOG(data.c_str());
	_evs.send(data.c_str(), "timeDate", 0, 500);
	DEBUGLOG("%s\r\n", NTP.getTimeDateString().c_str());
	data = String();
}

void AsyncFSWebServer::begin(FS* fs) {
	//Serial Interface
	if (!DBG_OUTPUT_PORT) {
		DBG_OUTPUT_PORT.begin(115200);
	}
	DBG_OUTPUT_PORT.print("\r\n\r\n");
	DEBUGLOG("START Setup\r\n");
	//Init
	_restartPending = false;
	_apConfig.APenable = false;
#ifndef RELEASE
	//DBG_OUTPUT_PORT.setDebugOutput(true); //uncomment for general Debugging of ESP
#endif // RELEASE	
	//Start filesystem
	DEBUGLOG("starting SPIFFS...\r\n");
	if (!_fs) {
		_fs = fs;
		if (!_fs) _fs->begin();
	}
#ifndef RELEASE
	// List files
	DEBUGLOG("SPIFFS Content:\r\n");
	Dir dir = _fs->openDir("/");
	while (dir.next()) {
		String fileName = dir.fileName();
		size_t fileSize = dir.fileSize();

		DEBUGLOG("FS File: %s, size: %s\r\n", fileName.c_str(), formatBytes(fileSize).c_str());
	}
	DEBUGLOG("\r\n");
#endif // RELEASE
	//Load Config
	_ConfigFileHandler.begin(_fs);
	if (!load_config()) { // Try to load configuration from file system
		defaultConfig(); // Load defaults if any error
	}
	loadHTTPAuth();

	//Connection LED & AP Mode Input
	DEBUGLOG("Checking if AP needs to be enabled...\r\n");
	if (CONNECTION_LED >= 0) {
		pinMode(CONNECTION_LED, OUTPUT); // CONNECTION_LED pin defined as output
		digitalWrite(CONNECTION_LED, HIGH); // Turn LED high
	}
	//check SSID Settings + startAP Flag
	if (_config.ssid == "") {
		_apConfig.APenable = true;
		DEBUGLOG("AP triggered by missing SSID\r\n");
	}
	if (_config.startAP) {
		_apConfig.APenable = true;
		DEBUGLOG("AP triggered by startAP Flag\r\n");
	}

	//NTP Init
	DEBUGLOG("\r\nInit NTP...\r\n");
	if (_config.updateNTPTimeEvery > 0) { // Enable NTP sync
		NTP.begin(_config.ntpServerName, _config.timezone / 10, _config.daylight);
		NTP.setInterval(15, _config.updateNTPTimeEvery * 60);
	}
	// Register wifi Events	
	DEBUGLOG("Init wifi events...\r\n");
	if (_apConfig.APenable) {
		onSoftAPModeStationConnectedHandler = WiFi.onSoftAPModeStationConnected([this](WiFiEventSoftAPModeStationConnected data) {
			this->onWiFiAPClientConnected(data);
		});
		onSoftAPModeStationDisconnectedHandler = WiFi.onSoftAPModeStationDisconnected([this](WiFiEventSoftAPModeStationDisconnected data) {
			this->onWiFiAPClientDisconnected(data);
		});
	}
	else {
		onStationModeConnectedHandler = WiFi.onStationModeConnected([this](WiFiEventStationModeConnected data) {
			this->onWiFiConnected(data);
		});
		onStationModeDisconnectedHandler = WiFi.onStationModeDisconnected([this](WiFiEventStationModeDisconnected data) {
			this->onWiFiDisconnected(data);
		});
	}

	//Configure WiFi
	WiFi.hostname(_config.deviceName.c_str());
	if (_apConfig.APenable) {
		configureWifiAP(); // Set AP mode
	}
	else {
		configureWifi(); // Set WiFi config
		DEBUGLOG("\r\nOpen http://");
		DEBUGLOG(_config.deviceName.c_str());
		DEBUGLOG(".local/edit to see the file browser\r\n");
	}
	DEBUGLOG("\r\nFlash chip size: %u\r\n", ESP.getFlashChipRealSize());
	DEBUGLOG("Sketch size: %u\r\n", ESP.getSketchSize());
	DEBUGLOG("Free flash space: %u\r\n", ESP.getFreeSketchSpace());

	// Attach 1 second Ticker
	_secondTk.attach(1.0f, &AsyncFSWebServer::s_secondTick, static_cast<void*>(this)); // Task to run periodic things every second

	// Configure and start web server
	AsyncWebServer::begin();
	serverInit();

	//Start MDNS service
	MDNS.begin(_config.deviceName.c_str());
	MDNS.addService("http", "tcp", 80);
	//Start Arduino OTA
	configureOTA(_httpAuth.wwwPassword.c_str());
	DEBUGLOG("END Setup\n");
}

void AsyncFSWebServer::defaultConfig() {
	DEBUGLOG(__PRETTY_FUNCTION__);
	DEBUGLOG("\r\n");
	// DEFAULT CONFIG
	_config.ssid = "";
	_config.password = "";
	_config.dhcp = 1;
	_config.ip = IPAddress(192, 168, 1, 4);
	_config.netmask = IPAddress(255, 255, 255, 0);
	_config.gateway = IPAddress(192, 168, 1, 1);
	_config.dns = IPAddress(192, 168, 1, 1);
	_config.ntpServerName = "pool.ntp.org";
	_config.updateNTPTimeEvery = 15;
	_config.timezone = 10;
	_config.daylight = 1;
	_config.deviceName = "ESP8266_Default";
	_config.startAP = false;
	_firmware.server = "";
	_firmware.path = "";
	save_config();
}

bool AsyncFSWebServer::load_config() {
	DEBUGLOG(__PRETTY_FUNCTION__);
	DEBUGLOG("\r\n");
	if (!_ConfigFileHandler.loadConfigFile(CONFIG_FILE)) return false;
	bool okay = true;
	okay &= _ConfigFileHandler.getValue("ssid", _config.ssid);
	okay &= _ConfigFileHandler.getValue("pass", _config.password);

	okay &= _ConfigFileHandler.getValue("ip", _config.ip);
	okay &= _ConfigFileHandler.getValue("netmask", _config.netmask);
	okay &= _ConfigFileHandler.getValue("gateway", _config.gateway);
	okay &= _ConfigFileHandler.getValue("dns", _config.dns);

	okay &= _ConfigFileHandler.getValue("dhcp", _config.dhcp);

	okay &= _ConfigFileHandler.getValue("ntp", _config.ntpServerName);
	okay &= _ConfigFileHandler.getValue("NTPperiod", _config.updateNTPTimeEvery);
	okay &= _ConfigFileHandler.getValue("timeZone", _config.timezone);
	okay &= _ConfigFileHandler.getValue("daylight", _config.daylight);
	okay &= _ConfigFileHandler.getValue("deviceName", _config.deviceName);
	okay &= _ConfigFileHandler.getValue("firmwareServer", _firmware.server);
	okay &= _ConfigFileHandler.getValue("firmwarePath", _firmware.path);
	okay &= _ConfigFileHandler.getValue("startAP", _config.startAP);

	okay &= _ConfigFileHandler.closeConfigFile();

	if (_config.deviceName == "") _config.deviceName = "ESP8266_Default";

	DEBUGLOG("Data initialized.\r\n");
	DEBUGLOG("SSID: %s ", _config.ssid.c_str());
	DEBUGLOG("PASS: %s\r\n", _config.password.c_str());
	DEBUGLOG("NTP Server: %s\r\n", _config.ntpServerName.c_str());
	DEBUGLOG("\r\n");

	return okay;
}

bool AsyncFSWebServer::save_config() {
	DEBUGLOG(__PRETTY_FUNCTION__);
	DEBUGLOG("\r\n");
	if (!_ConfigFileHandler.loadConfigFile(CONFIG_FILE)) return false;
	bool okay = true;
	okay &= _ConfigFileHandler.setValue("ssid", static_cast<String>(_config.ssid));
	okay &= _ConfigFileHandler.setValue("pass", static_cast<String>(_config.password));

	okay &= _ConfigFileHandler.setValue("ip", _config.ip);
	okay &= _ConfigFileHandler.setValue("netmask", _config.netmask);
	okay &= _ConfigFileHandler.setValue("gateway", _config.gateway);
	okay &= _ConfigFileHandler.setValue("dns", _config.dns);

	okay &= _ConfigFileHandler.setValue("dhcp", _config.dhcp);

	okay &= _ConfigFileHandler.setValue("ntp", static_cast<String>(_config.ntpServerName));
	okay &= _ConfigFileHandler.setValue("NTPperiod", _config.updateNTPTimeEvery);
	okay &= _ConfigFileHandler.setValue("timeZone", _config.timezone);
	okay &= _ConfigFileHandler.setValue("daylight", _config.daylight);
	okay &= _ConfigFileHandler.setValue("firmwareServer", static_cast<String>(_firmware.server));
	okay &= _ConfigFileHandler.setValue("firmwarePath", static_cast<String>(_firmware.path));
	okay &= _ConfigFileHandler.setValue("deviceName", static_cast<String>(_config.deviceName));
	okay &= _ConfigFileHandler.setValue("startAP", _config.startAP);

	okay &= _ConfigFileHandler.saveConfigFile();

	return okay;
}

bool AsyncFSWebServer::save_startAP(bool value) {
	DEBUGLOG(__PRETTY_FUNCTION__);
	DEBUGLOG("\r\n");
	if (!_ConfigFileHandler.loadConfigFile(CONFIG_FILE)) return false;
	_config.startAP = value;
	bool okay = true;
	okay &= _ConfigFileHandler.setValue("startAP", _config.startAP);
	okay &= _ConfigFileHandler.saveConfigFile();

	return okay;
}

bool AsyncFSWebServer::loadHTTPAuth() {
	DEBUGLOG(__PRETTY_FUNCTION__);
	DEBUGLOG("\r\n");
	if (!_ConfigFileHandler.loadConfigFile(SECRET_FILE)) return false;
	bool okay = true;
	okay &= _ConfigFileHandler.getValue("auth", _httpAuth.auth);
	okay &= _ConfigFileHandler.getValue("user", _httpAuth.wwwUsername);
	okay &= _ConfigFileHandler.getValue("pass", _httpAuth.wwwPassword);

	okay &= _ConfigFileHandler.closeConfigFile();

	DEBUGLOG(_httpAuth.auth ? "Secret initialized.\r\n" : "Auth disabled.\r\n");
	if (_httpAuth.auth) {
		DEBUGLOG("User: %s\r\n", _httpAuth.wwwUsername.c_str());
		DEBUGLOG("Pass: %s\r\n", _httpAuth.wwwPassword.c_str());
	}
	return okay;
}

bool AsyncFSWebServer::saveHTTPAuth() {
	DEBUGLOG(__PRETTY_FUNCTION__);
	DEBUGLOG("\r\n");
	if (!_ConfigFileHandler.loadConfigFile(SECRET_FILE)) return false;
	bool okay = true;
	okay &= _ConfigFileHandler.setValue("auth", _httpAuth.auth);
	okay &= _ConfigFileHandler.setValue("user", static_cast<String>(_httpAuth.wwwUsername));
	okay &= _ConfigFileHandler.setValue("pass", static_cast<String>(_httpAuth.wwwPassword));
	okay &= _ConfigFileHandler.saveConfigFile();

	return okay;
}

void AsyncFSWebServer::handle() {
	ArduinoOTA.handle();
}

void AsyncFSWebServer::configureWifiAP() {
	DEBUGLOG(__PRETTY_FUNCTION__);
	DEBUGLOG("\r\n");
	//Configure Wifi AP
	WiFi.disconnect();
	WiFi.mode(WIFI_AP);
	String APname = _apConfig.APssid + String(ESP.getChipId());
	DEBUGLOG("AP Name: %s\r\n", APname);
	if (_httpAuth.auth) {
		WiFi.softAP(APname.c_str(), _httpAuth.wwwPassword.c_str());
		DEBUGLOG("AP Pass enabled: %s\r\n", _httpAuth.wwwPassword.c_str());
	}
	else {
		WiFi.softAP(APname.c_str());
		DEBUGLOG("AP Pass disabled\r\n");
	}
	//Set LED Blink Ticker
	_LEDTk.attach(0.8f, &s_toggleLED);
	this->save_startAP(false);
	//Start AP Timeout if SSID is set
	if (_config.ssid != "") {
		_APTimeout.once(30, &AsyncFSWebServer::restartESP, static_cast<void*>(this));
		DEBUGLOG("AP Timeout started\r\n");
	}
}

void AsyncFSWebServer::configureWifi() {
	DEBUGLOG(__PRETTY_FUNCTION__);
	DEBUGLOG("\r\n");
	WiFi.disconnect();
	WiFi.mode(WIFI_STA);
	DEBUGLOG("Connecting to %s\r\n", _config.ssid.c_str());
	WiFi.begin(_config.ssid.c_str(), _config.password.c_str());
	if (!_config.dhcp) {
		DEBUGLOG("NO DHCP\r\n");
		WiFi.config(_config.ip, _config.gateway, _config.netmask, _config.dns);
	}
}

void AsyncFSWebServer::configureOTA(String password) {
	DEBUGLOG(__PRETTY_FUNCTION__);
	DEBUGLOG("\r\n");
	// Port defaults to 8266
	// ArduinoOTA.setPort(8266);
	ArduinoOTA.setHostname(_config.deviceName.c_str());
	// No authentication by default
	if (password != "") {
		ArduinoOTA.setPassword(password.c_str());
		DEBUGLOG("OTA password set %s\r\n", password.c_str());
	}
#ifndef RELEASE
	ArduinoOTA.onStart([]() {
		DEBUGLOG("OTA Start\r\n");
	});
	ArduinoOTA.onEnd(std::bind([](FS *fs) {
		fs->end();
		DEBUGLOG("OTA End\r\n");
	}, _fs));
	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
		int perc_progress = progress / (total / 100);
		if ((perc_progress % 5) == 0) {
			DEBUGLOG("OTA Progress: %u%%\r\n", perc_progress);
		}
	});
	ArduinoOTA.onError([](ota_error_t error) {
		DEBUGLOG("OTA Error[%u]: ", error);
		if (error == OTA_AUTH_ERROR) DEBUGLOG("Auth Failed\r\n");
		else if (error == OTA_BEGIN_ERROR) DEBUGLOG("Begin Failed\r\n");
		else if (error == OTA_CONNECT_ERROR) DEBUGLOG("Connect Failed\r\n");
		else if (error == OTA_RECEIVE_ERROR) DEBUGLOG("Receive Failed\r\n");
		else if (error == OTA_END_ERROR) DEBUGLOG("End Failed\r\n");
	});
	DEBUGLOG("OTA Ready\r\n");
#endif // RELEASE
	ArduinoOTA.begin();
}

void AsyncFSWebServer::onWiFiConnected(WiFiEventStationModeConnected data) {
	DEBUGLOG("\r\ncase STA_CONNECTED\r\n");
	if (CONNECTION_LED >= 0) {
		digitalWrite(CONNECTION_LED, LOW); // Turn LED low
	}
	wifiDisconnectedSince = 0;
	_wifiWasConnected = true;
}

void AsyncFSWebServer::onWiFiDisconnected(WiFiEventStationModeDisconnected data) {
	DEBUGLOG("\r\ncase STA_DISCONNECTED\r\n");
	if (CONNECTION_LED >= 0) {
		digitalWrite(CONNECTION_LED, HIGH); // Turn LED high
	}
	if (wifiDisconnectedSince == 0) {
		wifiDisconnectedSince = millis();
	}
	int disconSince = (int)((millis() - wifiDisconnectedSince) / 1000);
	DEBUGLOG("Disconnected since %d seconds\r\n", disconSince);
	//Start in AP Mode after 30 Seconds if Wifi was not connected
	if ((disconSince >= 30) && !_restartPending && !_wifiWasConnected) {
		DEBUGLOG("Wifi connect timeout... starting AP\r\n");
		this->save_startAP(true);
		this->restartESP(static_cast<void*>(this));
	}
}

void AsyncFSWebServer::onWiFiAPClientConnected(WiFiEventSoftAPModeStationConnected data) {
	_WiFiAPConnectedClients++;
	//Stop Timeout
	if (_config.ssid != "") _APTimeout.detach();
	DEBUGLOG("\r\ncase AP_CLIENT_CONNECTED\r\n");
	DEBUGLOG("connected Clients: %d\r\n", _WiFiAPConnectedClients);
}

void AsyncFSWebServer::onWiFiAPClientDisconnected(WiFiEventSoftAPModeStationDisconnected data) {
	if (--_WiFiAPConnectedClients <= 0) {
		//Start Timeout
		if (_config.ssid != "") _APTimeout.once(30, &AsyncFSWebServer::restartESP, static_cast<void*>(this));
	}
	DEBUGLOG("\r\ncase AP_CLIENT_DISCONNECTED\r\n");
	DEBUGLOG("connected Clients: %d\r\n", _WiFiAPConnectedClients);
}

void AsyncFSWebServer::handleFileList(AsyncWebServerRequest *request) {
	if (!request->hasArg("dir")) { request->send(500, "text/plain", "BAD ARGS"); return; }

	String path = request->arg("dir");
	DEBUGLOG("handleFileList: %s\r\n", path.c_str());
	Dir dir = _fs->openDir(path);
	path = String();

	String output = "[";
	while (dir.next()) {
		File entry = dir.openFile("r");
		if (output != "[")
			output += ',';
		bool isDir = false;
		output += "{\"type\":\"";
		output += (isDir) ? "dir" : "file";
		output += "\",\"name\":\"";
		output += String(entry.name()).substring(1);
		output += "\"}";
		entry.close();
	}

	output += "]";
	DEBUGLOG("%s\r\n", output.c_str());
	request->send(200, "text/json", output);
}

String getContentType(String filename, AsyncWebServerRequest *request) {
	if (request->hasArg("download")) return "application/octet-stream";
	else if (filename.endsWith(".htm")) return "text/html";
	else if (filename.endsWith(".html")) return "text/html";
	else if (filename.endsWith(".css")) return "text/css";
	else if (filename.endsWith(".js")) return "application/javascript";
	else if (filename.endsWith(".json")) return "application/json";
	else if (filename.endsWith(".png")) return "image/png";
	else if (filename.endsWith(".gif")) return "image/gif";
	else if (filename.endsWith(".jpg")) return "image/jpeg";
	else if (filename.endsWith(".ico")) return "image/x-icon";
	else if (filename.endsWith(".xml")) return "text/xml";
	else if (filename.endsWith(".pdf")) return "application/x-pdf";
	else if (filename.endsWith(".zip")) return "application/x-zip";
	else if (filename.endsWith(".gz")) return "application/x-gzip";
	return "text/plain";
}

bool AsyncFSWebServer::handleFileRead(String path, AsyncWebServerRequest *request) {
	DEBUGLOG("handleFileRead: %s\r\n", path.c_str());
	if (path.endsWith("/"))
		path += "index.html";
	String contentType = getContentType(path, request);
	String pathWithGz = path + ".gz";
	if (_fs->exists(pathWithGz) || _fs->exists(path)) {
		if (_fs->exists(pathWithGz)) {
			path += ".gz";
		}
		DEBUGLOG("Content type: %s\r\n", contentType.c_str());
		AsyncWebServerResponse *response = request->beginResponse(*_fs, path, contentType);
		if (path.endsWith(".gz"))
			response->addHeader("Content-Encoding", "gzip");
		//File file = SPIFFS.open(path, "r");
		DEBUGLOG("File %s exist\r\n", path.c_str());
		request->send(response);
		DEBUGLOG("File %s Sent\r\n", path.c_str());

		return true;
	}
	else {
		DEBUGLOG("Cannot find %s\n", path.c_str());
		return false;
	}
}

void AsyncFSWebServer::handleFileCreate(AsyncWebServerRequest *request) {
	if (!checkAuth(request))
		return request->requestAuthentication();
	if (request->args() == 0)
		return request->send(500, "text/plain", "BAD ARGS");
	String path = request->arg(0U);
	DEBUGLOG("handleFileCreate: %s\r\n", path.c_str());
	if (path == "/")
		return request->send(500, "text/plain", "BAD PATH");
	if (_fs->exists(path))
		return request->send(500, "text/plain", "FILE EXISTS");
	File file = _fs->open(path, "w");
	if (file)
		file.close();
	else
		return request->send(500, "text/plain", "CREATE FAILED");
	request->send(200, "text/plain", "");
	path = "";
}

void AsyncFSWebServer::handleFileDelete(AsyncWebServerRequest *request) {
	if (!checkAuth(request))
		return request->requestAuthentication();
	if (request->args() == 0) return request->send(500, "text/plain", "BAD ARGS");
	String path = request->arg(0U);
	DEBUGLOG("handleFileDelete: %s\r\n", path.c_str());
	if (path == "/")
		return request->send(500, "text/plain", "BAD PATH");
	if (!_fs->exists(path))
		return request->send(404, "text/plain", "FileNotFound");
	_fs->remove(path);
	request->send(200, "text/plain", "");
	path = "";
}

void AsyncFSWebServer::handleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
	static File fsUploadFile;
	static size_t fileSize = 0;

	if (!index) { // Start
		DEBUGLOG("handleFileUpload Name: %s\r\n", filename.c_str());
		if (!filename.startsWith("/")) filename = "/" + filename;
		fsUploadFile = _fs->open(filename, "w");
		DEBUGLOG("First upload part.\r\n");
	}
	// Continue
	if (fsUploadFile) {
		DEBUGLOG("Continue upload part. Size = %u\r\n", len);
		if (fsUploadFile.write(data, len) != len) {
			DBG_OUTPUT_PORT.println("Write error during upload");
		}
		else
			fileSize += len;
	}
	if (final) { // End
		if (fsUploadFile) {
			fsUploadFile.close();
		}
		DEBUGLOG("handleFileUpload Size: %u\n", fileSize);
		fileSize = 0;
	}
}

void AsyncFSWebServer::send_network_configuration_values_html(AsyncWebServerRequest *request) {
	//Micro-AJAX
	DEBUGLOG(__FUNCTION__);
	DEBUGLOG("\r\n");
	String values = "";
	values += "ssid|" + String(_config.ssid) + "|input\n";
	values += "password|" + String(_config.password) + "|input\n";
	values += "ip|" + _config.ip.toString() + "|input\n";
	values += "nm|" + _config.netmask.toString() + "|input\n";
	values += "gw|" + _config.gateway.toString() + "|input\n";
	values += "dns|" + _config.dns.toString() + "|input\n";
	values += "dhcp|" + String(_config.dhcp ? "checked" : "") + "|chk\n";
	request->send(200, "text/plain", values);
	values = "";
}

void AsyncFSWebServer::send_connection_state_values_html(AsyncWebServerRequest *request) {
	//Micro-AJAX
	DEBUGLOG(__FUNCTION__);
	DEBUGLOG("\r\n");
	String state;
	switch (WiFi.status())
	{
	case 0: state = "Idle"; break;
	case 1: state = "NO SSID AVAILBLE"; break;
	case 2: state = "SCAN COMPLETED"; break;
	case 3: state = "CONNECTED"; break;
	case 4: state = "CONNECT FAILED"; break;
	case 5: state = "CONNECTION LOST"; break;
	case 6: state = "DISCONNECTED"; break;
	default: state = "N/A"; break;
	}
	//WiFi.scanNetworks(true);
	String values = "";
	values += "connectionstate|" + state + "|div\n";
	request->send(200, "text/plain", values);
	state = "";
	values = "";
}

void AsyncFSWebServer::send_information_values_html(AsyncWebServerRequest *request) {
	//Micro-AJAX
	DEBUGLOG(__FUNCTION__);
	DEBUGLOG("\r\n");
	String values = "";

	values += "x_ssid|" + String(WiFi.SSID()) + "|div\n";
	values += "x_ip|" + String(WiFi.localIP()[0]) + "." + String(WiFi.localIP()[1]) + "." + String(WiFi.localIP()[2]) + "." + String(WiFi.localIP()[3]) + "|div\n";
	values += "x_gateway|" + String(WiFi.gatewayIP()[0]) + "." + String(WiFi.gatewayIP()[1]) + "." + String(WiFi.gatewayIP()[2]) + "." + String(WiFi.gatewayIP()[3]) + "|div\n";
	values += "x_netmask|" + String(WiFi.subnetMask()[0]) + "." + String(WiFi.subnetMask()[1]) + "." + String(WiFi.subnetMask()[2]) + "." + String(WiFi.subnetMask()[3]) + "|div\n";
	values += "x_mac|" + getMacAddress() + "|div\n";
	values += "x_dns|" + String(WiFi.dnsIP()[0]) + "." + String(WiFi.dnsIP()[1]) + "." + String(WiFi.dnsIP()[2]) + "." + String(WiFi.dnsIP()[3]) + "|div\n";
	values += "x_ntp_sync|" + NTP.getTimeDateString(NTP.getLastNTPSync()) + "|div\n";
	values += "x_ntp_time|" + NTP.getTimeStr() + "|div\n";
	values += "x_ntp_date|" + NTP.getDateStr() + "|div\n";
	values += "x_uptime|" + NTP.getUptimeString() + "|div\n";
	values += "x_last_boot|" + NTP.getTimeDateString(NTP.getLastBootTime()) + "|div\n";

	request->send(200, "text/plain", values);
	values = "";
}

String AsyncFSWebServer::getMacAddress() {
	uint8_t mac[6];
	char macStr[18] = { 0 };
	WiFi.macAddress(mac);
	sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return  String(macStr);
}

void AsyncFSWebServer::send_NTP_configuration_values_html(AsyncWebServerRequest *request) {
	//Micro-AJAX
	DEBUGLOG(__FUNCTION__);
	DEBUGLOG("\r\n");
	String values = "";
	values += "ntpserver|" + String(_config.ntpServerName) + "|input\n";
	values += "update|" + String(_config.updateNTPTimeEvery) + "|input\n";
	values += "tz|" + String(_config.timezone) + "|input\n";
	values += "dst|" + String((_config.daylight ? "checked" : "")) + "|chk\n";
	request->send(200, "text/plain", values);
	values = "";
}

void AsyncFSWebServer::send_system_configuration_values_html(AsyncWebServerRequest *request) {
	//Micro-AJAX
	DEBUGLOG(__FUNCTION__);
	DEBUGLOG("\r\n");
	String values = "";
	values += String("devicename|" + _config.deviceName + "|input\n");
	values += String("updateServer|" + _firmware.server + _firmware.path + "|input\n");
	values += String("wwwauth|" + String(_httpAuth.auth ? "checked" : "") + "|chk\n");
	values += String("wwwuser|" + _httpAuth.wwwUsername + "|input\n");
	values += String("wwwpass|" + _httpAuth.wwwPassword + "|input\n");
	request->send(200, "text/plain", values);
	values = "";
}

void AsyncFSWebServer::evaluate_network_post_html(AsyncWebServerRequest *request) {
	if (request->args() == 7) {
		bool okay = true;
		for (uint8_t i = 0; i < request->args(); i++) {
			if (request->argName(i) == "ssid") {
				_config.ssid = urldecode(request->arg(i));
				continue;
			}
			if (request->argName(i) == "password") {
				_config.password = urldecode(request->arg(i));
				continue;
			}
			if (request->argName(i) == "ip") {
				okay &= _config.ip.fromString(urldecode(request->arg(i)));
				continue;
			}
			if (request->argName(i) == "nm") {
				okay &= _config.netmask.fromString(urldecode(request->arg(i)));
				continue;
			}
			if (request->argName(i) == "gw") {
				okay &= _config.gateway.fromString(urldecode(request->arg(i)));
				continue;
			}
			if (request->argName(i) == "dns") {
				okay &= _config.dns.fromString(urldecode(request->arg(i)));
				continue;
			}
			if (request->argName(i) == "dhcp") {
				_config.dhcp = ((urldecode(request->arg(i)) == "true") ? true : false);
				continue;
			}
		}
		if (okay && save_config()) {
			request->send(200, "text/plain", "OK");
		}
		else {
			request->send(200, "text/plain", "NOK: Error saving");
		}
	}
	else {
		request->send(200, "text/plain", "NOK: Bad Args");
	}
}

void AsyncFSWebServer::evaluate_NTP_post_html(AsyncWebServerRequest *request) {
	if (request->args() == 4) {
		_config.daylight = false;
		for (uint8_t i = 0; i < request->args(); i++) {
			if (request->argName(i) == "ntpserver") {
				_config.ntpServerName = urldecode(request->arg(i));
				continue;
			}
			if (request->argName(i) == "update") {
				_config.updateNTPTimeEvery = request->arg(i).toInt();
				continue;
			}
			if (request->argName(i) == "tz") {
				_config.timezone = request->arg(i).toInt();
				continue;
			}
			if (request->argName(i) == "dst") {
				_config.daylight = ((urldecode(request->arg(i)) == "true") ? true : false);
				continue;
			}
		}
		if (save_config()) {
			NTP.setNtpServerName(_config.ntpServerName);
			NTP.setInterval(_config.updateNTPTimeEvery * 60);
			NTP.setTimeZone(_config.timezone / 10.0);
			NTP.setDayLight(_config.daylight);
			setTime(NTP.getTime());
			request->send(200, "text/plain", "OK");
		}
		else request->send(200, "text/plain", "NOK: Error saving");
	}
	else {
		request->send(200, "text/plain", "NOK: Bad Args");
	}
}

void AsyncFSWebServer::evaluate_system_post_html(AsyncWebServerRequest *request) {
	if (request->args() == 5) {
		bool req_restart = false;
		for (uint8_t i = 0; i < request->args(); i++) {
			if (request->argName(i) == "devicename") {
				String tmpDevicename = urldecode(request->arg(i));
				req_restart = (_config.deviceName != tmpDevicename);
				_config.deviceName = tmpDevicename;
				continue;
			}
			if (request->argName(i) == "updateServer") {
				String tmpString = urldecode(request->arg(i));
				//remove http:// or https://
				if (tmpString.startsWith("http://") && (tmpString.length() > 7)) tmpString = tmpString.substring(7);
				if (tmpString.startsWith("https://") && (tmpString.length() > 8)) tmpString = tmpString.substring(8);
				//split at /
				int i = tmpString.indexOf('/');
				if (i >= 0) { // "/" found
					if (!tmpString.endsWith("/")) tmpString += "/";
					_firmware.server = tmpString.substring(0, i);
					_firmware.path = tmpString.substring(i);
				}
				else {
					_firmware.server = tmpString;
					_firmware.path = "/";
				}
				continue;
			}
			if (request->argName(i) == "wwwuser") {
				_httpAuth.wwwUsername = urldecode(request->arg(i));
				continue;
			}
			if (request->argName(i) == "wwwpass") {
				_httpAuth.wwwPassword = urldecode(request->arg(i));
				continue;
			}
			if (request->argName(i) == "wwwauth") {
				_httpAuth.auth = ((urldecode(request->arg(i)) == "true") ? true : false);
				continue;
			}
		}
		if (saveHTTPAuth() && save_config()) {
			if (req_restart) request->send(200, "text/plain", "OK-RESTART");
			else request->send(200, "text/plain", "OK");
		}
		else request->send(200, "text/plain", "NOK: Error saving");
	}
	else {
		request->send(200, "text/plain", "NOK: Bad Args");
	}
}

void AsyncFSWebServer::setModelName(String s) {
	_firmware.modelName = s;
}

void AsyncFSWebServer::setVersionString(String s) {
	_firmware.clientVersion = s;
}

void AsyncFSWebServer::checkUpdate() {
	checkFirmware();
}

bool AsyncFSWebServer::runUpdate() {
	if (!_firmware.updatePossible) return false;
	updateFirmware(true);
	return true;
}

void AsyncFSWebServer::sendUpdateData() {
	String data = "{";
	data += "\"serverVer\":\"" + _firmware.serverVersion + "\",";
	data += "\"clientVer\":\"" + _firmware.clientVersion + "\",";
	data += "\"updPoss\":\"" + String((_firmware.updateAvailable && (ESP.getFreeSketchSpace() >= _firmware.updateSize))?"ja":"nein") + "\"";
	data += "}\r\n";
	_evsUpd.send(data.c_str(), "UpdData", 0, 500);
	data = String();
}

void AsyncFSWebServer::checkFirmware() {
	DEBUGLOG(__FUNCTION__);
	DEBUGLOG("\r\n");
	if (_firmware.state == FW_IDLE || _firmware.state == FW_ERROR || _firmware.state == FW_NO_UPDATE) {
		//set state
		_firmware.state = FW_REQ_AV_PENDING;
		_firmware.lastError = FW_ERROR_NONE;
		_firmware.updatePossible = false;
		//allocate new Client if it's not existing
		if (!_asyncClient) {
			_asyncClient = new AsyncClient();
		}
		if (!_asyncClient) return;
		//define Error callback
		_asyncClient->onError([this](void* arg, AsyncClient* client, int error) {
			_firmware.state = FW_ERROR;
			_firmware.lastError = HTTP_ERROR_CONNECT_FAILED;
			DEBUGLOG("[UPDATECHECK] Connect failed\r\n");
			_evsUpd.send("10.20", "state",0,500);
			if (updatecallback) updatecallback(false, true, false, _firmware.lastError, _firmware.serverVersion, _firmware.updateSize);
		}, NULL);
		//define further callbacks
		_asyncClient->onConnect([this](void* arg, AsyncClient* client) {
			client->onError(NULL, NULL);
			client->onDisconnect([this](void* arg, AsyncClient* c) {
				DEBUGLOG("[UPDATECHECK] HTTP Client disconnected\r\n");
				if (_firmware.state != FW_ERROR && _firmware.state != FW_IDLE && _firmware.state != FW_NO_UPDATE) {
					_firmware.state = FW_ERROR;
					_firmware.lastError = HTTP_ERROR_SERVER_DISCONNECTED;
				}
				if (_firmware.state == FW_IDLE) _firmware.lastError = FW_ERROR_NONE;
				//check if Update is possible
				_firmware.updatePossible = ((ESP.getFreeSketchSpace() >= _firmware.updateSize) && _firmware.updateAvailable);
				//send update status
				if (updatecallback) updatecallback(false, ((_firmware.state == FW_ERROR) ? true : false), _firmware.updatePossible, _firmware.lastError, _firmware.serverVersion, _firmware.updateSize);

				//Event message
				String msg;
				if (_firmware.state == FW_IDLE && _firmware.updateAvailable) {  //Update available
					if (_firmware.updatePossible) msg = "7"; //Update possible
					else msg = "6"; //Update not possible
				}
				else if (_firmware.state == FW_NO_UPDATE) msg = "8"; //No Update available
				else { //Error
					msg = "10.";
					msg += String(_firmware.lastError);
				}
				_evsUpd.send(msg.c_str(), "state", 0, 500);
				sendUpdateData();
			}, NULL);

			client->onData([this](void* arg, AsyncClient* c, void* data, size_t len) {
				if (_firmware.state == FW_REQ_AV_PENDING) {
					_firmware.state == FW_RECV_AV_PENDING;
					//Parse header
					uint8_t* d = (uint8_t*)data;
					int statusCode = 0;
					String headerLine = "";
					bool CRFound = false;
					enum ParseState {
						//status code
						PARSE_WAIT_STATUS_CODE,
						PARSE_READING_STATUS_CODE,
						PARSE_STATUS_CODE_READ,
						PARSE_FIRST_SIGN,
						PARSE_READING_HEADER,
						PARSE_STARTING_CR_FOUND,
						PARSE_BODY,
						PARSE_SUCCESS
					};
					ParseState parseState = PARSE_WAIT_STATUS_CODE;
					//pseudo regex
					const char* statusPrefix = "HTTP/*.* ";
					const char* statusPtr = statusPrefix;
					for (size_t i = 0; ((i < len) && (_firmware.state != FW_ERROR) && (parseState != PARSE_SUCCESS)); i++) {
						switch (parseState)
						{
						case PARSE_WAIT_STATUS_CODE:
							//Skip Status Code Prefix (HTTP/*.* )
							if ((*statusPtr == '*') || (*statusPtr == d[i]))
							{
								statusPtr++;
								if (*statusPtr == '\0')
								{
									parseState = PARSE_READING_STATUS_CODE;
								}
							}
							else
							{
								_firmware.lastError = HTTP_ERROR_INVALID_RESPONSE;
								_firmware.state = FW_ERROR;
							}
							break;
						case PARSE_READING_STATUS_CODE:
							//Read Status Code
							if (isdigit(d[i]))
							{
								statusCode = statusCode * 10 + (d[i] - '0');
							}
							else
							{
								//Check Status Code
								if (statusCode == 200) {
									_firmware.updateAvailable = false;
									_firmware.updateSize = 0;
									_firmware.serverVersion = "";
									parseState = PARSE_STATUS_CODE_READ;
								}
								else if (statusCode == 416) {
										_firmware.lastError = FW_ERROR_NO_VERSION_FOR_MODEL;
										_firmware.state = FW_ERROR;
								}
								else {
									_firmware.lastError = HTTP_ERROR_INVALID_STATUSCODE;
									_firmware.state = FW_ERROR;
								}
							}
							break;
						case PARSE_STATUS_CODE_READ:
							if (d[i] == '\n') {
								parseState = PARSE_FIRST_SIGN;
							}
							break;
						case PARSE_FIRST_SIGN:
							if (d[i] == '\r')
								parseState = PARSE_STARTING_CR_FOUND;
							else {
								headerLine = String(char(d[i])); //first sign of headerline
								parseState = PARSE_READING_HEADER;
							}
							break;
						case PARSE_READING_HEADER:
							if (d[i] == '\r') CRFound = true;
							else {
								if (CRFound) {
									CRFound = false;
									if (d[i] == '\n') {
										//evaluate header here
										if (headerLine.indexOf(':') == -1) {
											_firmware.lastError = HTTP_ERROR_INVALID_HEADER;
											_firmware.state = FW_ERROR;
										}
										else {
											String headerName = headerLine.substring(0, headerLine.indexOf(':'));
											String headerValue = headerLine.substring(headerLine.indexOf(':') + 2);
											if (headerName == "x-esp8266-updateAvailable") _firmware.updateAvailable = true;
											if (headerName == "x-esp8266-serverVersion") _firmware.serverVersion = headerValue;
											if (headerName == "x-esp8266-updateSize") _firmware.updateSize = static_cast<uint32_t>(headerValue.toInt());
											//parse next header
											parseState = PARSE_FIRST_SIGN;
										}
									}
									else {
										_firmware.lastError = HTTP_ERROR_INVALID_HEADER;
										_firmware.state = FW_ERROR;
									}
								}
								else {
									//read header
									headerLine += String(char(d[i]));
								}
							}
							break;
						case PARSE_STARTING_CR_FOUND:
							if (d[i] == '\n') {
								parseState = PARSE_SUCCESS;
							}
							else {
								_firmware.lastError = HTTP_ERROR_INVALID_HEADER;
								_firmware.state = FW_ERROR;
							}
							break;
						default:
							_firmware.lastError = HTTP_ERROR_INVALID_PARSESTATE;
							_firmware.state = FW_ERROR;
							break;
						}
					}
					if (_firmware.state != FW_ERROR) {
						if (_firmware.updateAvailable) _firmware.state = FW_IDLE;
						else _firmware.state = FW_NO_UPDATE;
					}
					c->stop();
				}
				else {
					//Error
					//Disconnect Client
					if (_asyncClient->connected()) c->stop();
					DEBUGLOG("[UPDATECHECK] Error FW state\r\n");
					_firmware.lastError = FW_ERROR_FW_STATE;
					_firmware.state = FW_ERROR;
				}
			}, NULL);

			//send the http request
			String request = "GET ";
			request += _firmware.path;
			request += " HTTP/1.1\r\nHost: ";
			request += _firmware.server;
			request += "\r\nConnection: keep-alive\r\nUser-Agent: ESP8266-http-Update\r\nX-ESP8266-MODEL: ";
			request += _firmware.modelName;
			request += "\r\nX-ESP8266-CHECKUPDATE:\r\nX-ESP8266-VERSION: ";
			request += _firmware.clientVersion;
			request += "\r\n\r\n";
			client->write(request.c_str());
		}, NULL);

		//connect to Server to send the request
		if (!_asyncClient->connect(_firmware.server.c_str(), 80)) {
			_firmware.state = FW_ERROR;
			_firmware.lastError = HTTP_ERROR_CONNECT_FAILED;
			DEBUGLOG("[UPDATECHECK] Connect failed\r\n");
			_evsUpd.send("10.20", "state", 0, 500);
			if (updatecallback) updatecallback(false, true, false, _firmware.lastError, _firmware.serverVersion, _firmware.updateSize);
		}
	}
	else {
		_evsUpd.send("10.21", "state", 0, 500);
	}
}

void AsyncFSWebServer::updateFirmware(bool updSpiffs) {
	DEBUGLOG(__FUNCTION__);
	DEBUGLOG("\r\n");
	if (_firmware.state == FW_IDLE || _firmware.state == FW_ERROR || _firmware.state == FW_NO_UPDATE) {
		//spiffs or firmware?
		_firmware.updSpiffs = updSpiffs;
		if (_firmware.updSpiffs) _evsUpd.send("1", "state", 0, 500);
		else _evsUpd.send("3", "state", 0, 500);
		//set state
		_firmware.state = FW_REQ_BIN_PENDING;
		//allocate new Client if it's not existing
		if (!_asyncClient) {
			_asyncClient = new AsyncClient();
		}
		if (!_asyncClient) return;
		//define Error callback
		_asyncClient->onError([this](void* arg, AsyncClient* client, int error) {
			_firmware.state = FW_ERROR;
			_firmware.lastError = HTTP_ERROR_CONNECT_FAILED;
			DEBUGLOG("[UPDATE] Connect failed\r\n");
			_evsUpd.send("10.20", "state", 0, 500);
			if (updatecallback) updatecallback(true, true, false, _firmware.lastError, _firmware.serverVersion, _firmware.updateSize);
		}, NULL);
		//define further callbacks
		_asyncClient->onConnect([this](void* arg, AsyncClient* client) {
			client->onError(NULL, NULL);
			client->onDisconnect([this](void* arg, AsyncClient* c) {
				if (_firmware.state != FW_ERROR && _firmware.state != FW_IDLE && !_firmware.startFWupdate) {
					_firmware.state = FW_ERROR;
					_firmware.lastError = HTTP_ERROR_SERVER_DISCONNECTED;
				}
				//start FS if there was an error
				if (_firmware.state == FW_ERROR) if (!_fs) _fs->begin();
				DEBUGLOG("[UPDATE] HTTP Client disconnected\r\n");
				//start FW Update
				if (_firmware.startFWupdate && _firmware.state != FW_ERROR) {
					_firmware.startFWupdate = false;
					_firmware.state = FW_IDLE;
					updateFirmware(false);
				}
				else {
					//ERROR
					//callback for info
					if (_firmware.state == FW_IDLE) _firmware.lastError = FW_ERROR_NONE;
					if (updatecallback) updatecallback(true, ((_firmware.state == FW_ERROR) ? true : false), _firmware.updatePossible, _firmware.lastError, _firmware.serverVersion, _firmware.updateSize);
					//Event message
					String msg;
					if (_firmware.state == FW_IDLE) msg = "9"; //Update erfolgreich
					else { //Error
						msg = "10.";
						msg += String(_firmware.lastError);
					}
					_evsUpd.send(msg.c_str(), "state", 0, 500);
				}
				//restart ESP if Update completed
				if (_restartESP) restartESP(static_cast<void*>(this));
			}, NULL);

			client->onData([this](void* arg, AsyncClient* c, void* data, size_t len) {
				//first call => Parse Header
				if (_firmware.state == FW_REQ_BIN_PENDING) {
					if (_firmware.updSpiffs) _evsUpd.send("2", "state", 0, 500);
					else _evsUpd.send("4", "state", 0, 500);
					DEBUGLOG("[UPDATE] parsing HTTP response...\r\n");
					_firmware.state == FW_RECV_BIN_PENDING;
					//Parse header
					uint8_t* d = (uint8_t*)data;
					int statusCode = 0;
					String headerLine = "";
					size_t headerLength;
					bool CRFound = false;
					enum ParseState {
						//status code
						PARSE_WAIT_STATUS_CODE,
						PARSE_READING_STATUS_CODE,
						PARSE_STATUS_CODE_READ,
						PARSE_FIRST_SIGN,
						PARSE_READING_HEADER,
						PARSE_STARTING_CR_FOUND,
						PARSE_BODY,
						PARSE_SUCCESS
					};
					ParseState parseState = PARSE_WAIT_STATUS_CODE;
					//pseudo regex
					const char* statusPrefix = "HTTP/*.* ";
					const char* statusPtr = statusPrefix;
					for (size_t i = 0; ((i < len) && (_firmware.state != FW_ERROR)); i++) {
						switch (parseState)
						{
						case PARSE_WAIT_STATUS_CODE:
							//Skip Status Code Prefix (HTTP/*.* )
							if ((*statusPtr == '*') || (*statusPtr == d[i]))
							{
								statusPtr++;
								if (*statusPtr == '\0')
								{
									parseState = PARSE_READING_STATUS_CODE;
									DEBUGLOG("[UPDATE] parsing HTTP Status Code...\r\n");
								}
							}
							else
							{
								_firmware.lastError = HTTP_ERROR_INVALID_RESPONSE;
								_firmware.state = FW_ERROR;
								DEBUGLOG("[UPDATE] HTTP ERROR: Invalid response\r\n");
							}
							break;
						case PARSE_READING_STATUS_CODE:
							//Read Status Code
							if (isdigit(d[i]))
							{
								statusCode = statusCode * 10 + (d[i] - '0');
							}
							else
							{
								DEBUGLOG("[UPDATE] HTTP Status Code: %d\r\n", statusCode);
								//Check Status Code
								if (statusCode == 200) {
									_firmware.updateAvailable = false;
									_firmware.updateSize = 0;
									_firmware.serverMD5 = "";
									_firmware.serverVersion = "";
									_firmware.rcvdSpiffs = false;
									parseState = PARSE_STATUS_CODE_READ;
									DEBUGLOG("[UPDATE] Status Code okay\r\n");
								}
								else {
									_firmware.lastError = HTTP_ERROR_INVALID_STATUSCODE;
									_firmware.state = FW_ERROR;
									//DEBUG****
#ifndef RELEASE
									switch (statusCode)
									{
									case 304: //no new Version
										DEBUGLOG("[UPDATE] No NEW Version available for this model\r\n");
										break;
									case 400: //bad request
										DEBUGLOG("[UPDATE] Bad HTTP Request\r\n");
										break;
									case 403: //forbidden
										DEBUGLOG("[UPDATE] No Access\r\n");
										break;
									case 416: //no version for this model
										DEBUGLOG("[UPDATE] No Version available for this model\r\n");
										break;
									}
#endif //RELEASE
								}
							}
							break;
						case PARSE_STATUS_CODE_READ:
							if (d[i] == '\n') {
								parseState = PARSE_FIRST_SIGN;
							}
							break;
						case PARSE_FIRST_SIGN:
							if (d[i] == '\r')
								parseState = PARSE_STARTING_CR_FOUND;
							else {
								headerLine = String(char(d[i]));
								parseState = PARSE_READING_HEADER;
								DEBUGLOG("[UPDATE] parsing Header..\r\n");
							}
							break;
						case PARSE_READING_HEADER:
							if (d[i] == '\r') CRFound = true;
							else {
								if (CRFound) {
									CRFound = false;
									if (d[i] == '\n') {
										//evaluate header here
										if (headerLine.indexOf(':') == -1) {
											_firmware.lastError = HTTP_ERROR_INVALID_HEADER;
											_firmware.state = FW_ERROR;
											DEBUGLOG("[UPDATE] invalid Header..\r\n");
										}
										else {
											String headerName = headerLine.substring(0, headerLine.indexOf(':'));
											String headerValue = headerLine.substring(headerLine.indexOf(':') + 2);
											if (headerName == "x-esp8266-updateSize") _firmware.updateSize = static_cast<uint32_t>(headerValue.toInt());
											if (headerName == "x-esp8266-MD5") _firmware.serverMD5 = headerValue;
											if (headerName == "x-esp8266-SPIFFS") _firmware.rcvdSpiffs = true;
											DEBUGLOG("[UPDATE] Header parsed => name: %s; value: %s\r\n", headerName.c_str(), headerValue.c_str());
											//parse next header
											parseState = PARSE_FIRST_SIGN;
										}
									}
									else {
										_firmware.lastError = HTTP_ERROR_INVALID_HEADER;
										_firmware.state = FW_ERROR;
										DEBUGLOG("[UPDATE] invalid Header..\r\n");
									}
								}
								else {
									headerLine += String(char(d[i]));
								}
							}
							break;
						case PARSE_STARTING_CR_FOUND:
							if (d[i] == '\n') {
								parseState = PARSE_BODY;
								headerLength = i + 1;
								DEBUGLOG("[UPDATE] parsing Header finished, now parse body\r\n");
							}
							else {
								_firmware.lastError = HTTP_ERROR_INVALID_HEADER;
								_firmware.state = FW_ERROR;
								DEBUGLOG("[UPDATE] invalid Header..\r\n");
							}
							break;
						case PARSE_BODY:
							//check Header Data
							if (_firmware.updateSize <= 0 || _firmware.serverMD5 == "") {
								_firmware.lastError = HTTP_ERROR_INVALID_HEADER;
								_firmware.state = FW_ERROR;
								DEBUGLOG("[UPDATE] Error: Size or MD5 Information missing...\r\n");
							}
							//check if it is SPIFFS bin if SPIFFS was requested
							if (_firmware.rcvdSpiffs != _firmware.updSpiffs) {
								_firmware.lastError = FW_ERROR_SPIFFS;
								_firmware.state = FW_ERROR;
								DEBUGLOG("[UPDATE] Error: SPIFFS mismatch...\r\n");
							}
							//start Update
							if (_firmware.state != FW_UPDATE_RUNNING && _firmware.state != FW_ERROR) {						
								if (_firmware.updSpiffs) {
									DEBUGLOG("[UPDATE] Updating Spiffs\r\n");
								}
								else {
									DEBUGLOG("[UPDATE] Updating FW");
								}
								_firmware.state = FW_UPDATE_RUNNING;
								_fs->end();
								Update.runAsync(true);
								Update.setMD5(_firmware.serverMD5.c_str());
								//start Updater or set Error
								if (!Update.begin(_firmware.updateSize,(_firmware.updSpiffs?U_SPIFFS:U_FLASH))) {
									_firmware.lastError = FW_ERROR_BEGIN_UPDATE;
									_firmware.state = FW_ERROR;
									Update.end(false); //reset Updater
									DEBUGLOG("[UPDATE] Error begin Update...\r\n");
								}
								else {
									_firmware.actSize = 0;
									//write bin Bytes without http Header
									uint8_t* d2 = (uint8_t*)data;
									d2 += headerLength;
									size_t written = Update.write(d2, len - headerLength);
									_firmware.actSize += written;
								}
							}
							break;
						default:
							_firmware.lastError = HTTP_ERROR_INVALID_PARSESTATE;
							_firmware.state = FW_ERROR;
							break;
						}
					}
					if ((_firmware.state == FW_ERROR) && _asyncClient->connected()) {
						DEBUGLOG("[UPDATE] Terminating Connection...\r\n");
						c->stop();
					}
				}
				else if (_firmware.state == FW_UPDATE_RUNNING) {
					//write Date
					uint8_t* d = (uint8_t*)data;
					size_t written = Update.write(d, len);
					_firmware.actSize += written;
#ifndef RELEASE
					static int lastProgress;
					int tmpProgress = (_firmware.actSize * 100) / _firmware.updateSize;
					if (tmpProgress != lastProgress) {//&& (tmpProgress % 10 == 0)) {
						lastProgress = tmpProgress;						
						DEBUGLOG("[UPDATE] updating... %d %%\r\n", tmpProgress);
					}
#endif
					//end Update
					if (_firmware.actSize >= _firmware.updateSize) {
						if (Update.end(true)) {
							if (_firmware.updSpiffs) {
								DEBUGLOG("[UPDATE] SPIFFS Update finished => disconnecting and saving data...\r\n");
								//SPIFFS update finished => start FS again and save config + callback, so user can save his config too
								_fs->begin();
								save_config();
								saveHTTPAuth();
								if (saveconfigcallback) saveconfigcallback();
								//and start Firmware Update
								DEBUGLOG("[UPDATE] data saved => disconnect Client and start FW update\r\n");
								//start FW Update flag
								_firmware.startFWupdate = true;
								//Disconnect Client
								if (_asyncClient->connected()) c->stop();
							}
							else {
								//firmware update complete
								_firmware.lastError = FW_ERROR_NONE;
								_firmware.state = FW_IDLE;
								//Disconnect Client
								if (_asyncClient->connected()) c->stop();
								DEBUGLOG("[UPDATE] FW update complete => restart\r\n");
								//restart ESP
								_restartESP = true;
							}
						}
						else {
							_firmware.lastError = FW_ERROR_END_UPDATE;
							_firmware.state = FW_ERROR;
							//Disconnect Client
							if (_asyncClient->connected()) c->stop();
							DEBUGLOG("[UPDATE] Error end update => restart\r\n");
							//restart ESP
							_restartESP = true;
						}
					}
				}
				else {
					//Error
					//Disconnect Client
					if (_asyncClient->connected()) c->stop();
					DEBUGLOG("[UPDATE] Error FW state\r\n");
					_firmware.lastError = FW_ERROR_FW_STATE;
					_firmware.state = FW_ERROR;
				}
			}, NULL);

			//send the http request
			String request = "GET ";
			request += _firmware.path;
			request += " HTTP/1.1\r\nHost: ";
			request += _firmware.server;
			request += "\r\nConnection: keep-alive\r\nUser-Agent: ESP8266-http-Update\r\nX-ESP8266-MODEL: ";
			request += _firmware.modelName;
			request += "\r\nX-ESP8266-VERSION: ";
			request += _firmware.clientVersion;
			//send header for receiving SPIFFS first
			if (_firmware.updSpiffs) {
				DEBUGLOG("[UPDATE] Preparing SPIFFS request...\r\n");
				request += "\r\nX-ESP8266-SPIFFS: ";
			}
#ifndef RELEASE
			else {
				DEBUGLOG("[UPDATE] Preparing FW request...\r\n");
			}
#endif
			request += "\r\n\r\n";
			DEBUGLOG("[UPDATE] Sending request...\r\n");
			client->write(request.c_str());
		}, NULL);

		//connect to Server to send the request
		if (!_asyncClient->connect(_firmware.server.c_str(), 80)) {
			_firmware.state = FW_ERROR;
			_firmware.lastError = HTTP_ERROR_CONNECT_FAILED;
			DEBUGLOG("[UPDATE] Connect failed\r\n");
			_evsUpd.send("10.20", "state", 0, 500);
			if (updatecallback) updatecallback(true, true, false, _firmware.lastError, _firmware.serverVersion, _firmware.updateSize);
		}
	}
	else {
	 _evsUpd.send("10.21", "state", 0, 500);
	}
}

void AsyncFSWebServer::serverInit() {
	DEBUGLOG(__FUNCTION__);
	DEBUGLOG("\r\n");
	//SERVER INIT
	//list directory
	on("/list", HTTP_GET, [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->handleFileList(request);
	});
	//load editor
	on("/edit", HTTP_GET, [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		if (!this->handleFileRead("/edit.html", request))
			request->send(404, "text/plain", "FileNotFound");
	});
	//create file
	on("/edit", HTTP_PUT, [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->handleFileCreate(request);
	});
	//delete file
	on("/edit", HTTP_DELETE, [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->handleFileDelete(request);
	});
	//first callback is called after the request has ended with all parsed arguments
	//second callback handles file uploads at that location
	on("/edit", HTTP_POST, [](AsyncWebServerRequest *request) { request->send(200, "text/plain", ""); }, [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
		this->handleFileUpload(request, filename, index, data, len, final);
	});

	on("/admin/values/network", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->send_network_configuration_values_html(request);
	});
	on("/admin/values/connectionstate", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->send_connection_state_values_html(request);
	});
	on("/admin/values/info", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->send_information_values_html(request);
	});
	on("/admin/values/ntp", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->send_NTP_configuration_values_html(request);
	});
	on("/admin/values/system", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->send_system_configuration_values_html(request);
	});
	on("/admin/post/network", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->evaluate_network_post_html(request);
	});
	on("/admin/post/ntp", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->evaluate_NTP_post_html(request);
	});
	on("/admin/post/system", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->evaluate_system_post_html(request);
	});
	on("/admin/actions/scan", HTTP_GET, [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		String json = "[";
		int n = WiFi.scanComplete();
		if (n == WIFI_SCAN_FAILED) {
			WiFi.scanNetworks(true);
		}
		else if (n) {
			for (int i = 0; i < n; ++i) {
				if (i) json += ",";
				json += "{";
				json += "\"rssi\":" + String(WiFi.RSSI(i));
				json += ",\"ssid\":\"" + WiFi.SSID(i) + "\"";
				json += ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\"";
				json += ",\"channel\":" + String(WiFi.channel(i));
				json += ",\"secure\":" + String(WiFi.encryptionType(i));
				json += ",\"hidden\":" + String(WiFi.isHidden(i) ? "true" : "false");
				json += "}";
			}
			WiFi.scanDelete();
		}
		json += "]";
		request->send(200, "text/json", json);
		json = "";
	});
	on("/admin/actions/restart", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		request->send_P(200, "text/html", "Restarting...");
		this->restartESP(static_cast<void*>(this));
	});
	on("/admin/actions/factoryReset", HTTP_POST, [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		if ((request->argName(size_t(0)) == "ack") && (request->arg(size_t(0)) == "OK") && (request->args() == 1)) {
			this->factoryReset(true);
			request->send_P(200, "text/html", "OK");
		} else request->send_P(500, "text/html", "BAD ARGS");
	});
	on("/admin/update/checkUpdate", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		this->checkUpdate();
		request->send(200, "text/plain", "OK");
	});
	on("/admin/update/doUpdate", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		if (this->runUpdate()) request->send(200, "text/plain", "OK");
		else request->send(200, "text/plain", "Check Update first!");
	});
	on("/admin", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		if (!this->handleFileRead("/admin.html", request))
			request->send(404, "text/plain", "FileNotFound");
	});

	on("/json", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		if (jsoncallback)
			this->jsoncallback(request);
		else
			request->send(404, "text/plain", "FileNotFound");
	});
	on("/rest", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		if (restcallback)
			this->restcallback(request);
		else
			request->send(404, "text/plain", "FileNotFound");
	});
	on("/post", [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		if (postcallback)
			this->postcallback(request);
		else
			request->send(404, "text/plain", "FileNotFound");
	});

	//called when the url is not defined here
	//use it to load content from SPIFFS
	onNotFound([this](AsyncWebServerRequest *request) {
		DEBUGLOG("Not found: %s\r\n", request->url().c_str());
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		AsyncWebServerResponse *response = request->beginResponse(200);
		response->addHeader("Connection", "close");
		response->addHeader("Access-Control-Allow-Origin", "*");
		if (!this->handleFileRead(request->url(), request))
			request->send(404, "text/plain", "FileNotFound");
		delete response; // Free up memory!
	});

#ifdef HIDE_SECRET
	on(SECRET_FILE, HTTP_GET, [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		AsyncWebServerResponse *response = request->beginResponse(403, "text/plain", "Forbidden");
		response->addHeader("Connection", "close");
		response->addHeader("Access-Control-Allow-Origin", "*");
		request->send(response);
	});
#endif // HIDE_SECRET

#ifdef HIDE_CONFIG
	on(CONFIG_FILE, HTTP_GET, [this](AsyncWebServerRequest *request) {
		if (!this->checkAuth(request))
			return request->requestAuthentication();
		AsyncWebServerResponse *response = request->beginResponse(403, "text/plain", "Forbidden");
		response->addHeader("Connection", "close");
		response->addHeader("Access-Control-Allow-Origin", "*");
		request->send(response);
	});
#endif // HIDE_CONFIG

	//get heap status, analog input value and all GPIO statuses in one json call
	on("/all", HTTP_GET, [](AsyncWebServerRequest *request) {
		String json = "{";
		json += "\"heap\":" + String(ESP.getFreeHeap());
		json += ", \"analog\":" + String(analogRead(A0));
		json += ", \"gpio\":" + String((uint32_t)(((GPI | GPO) & 0xFFFF) | ((GP16I & 0x01) << 16)));
		json += "}";
		request->send(200, "text/json", json);
		json = String();
	});

#ifndef RELEASE
	_evs.onConnect([](AsyncEventSourceClient* client) {
		DEBUGLOG("Event source client connected from %s\r\n", client->client()->remoteIP().toString().c_str());
	});
#endif //	!RELEASE

	_evsUpd.onConnect([this](AsyncEventSourceClient* client) {
		_evsUpd.send("", "rdy", 0, 500);
	});
	addHandler(&_evs);
	addHandler(&_evsUpd);

	DEBUGLOG("HTTP server started\r\n");
}

bool AsyncFSWebServer::checkAuth(AsyncWebServerRequest *request) {
	if (!_httpAuth.auth) {
		return true;
	}
	else {
		return request->authenticate(_httpAuth.wwwUsername.c_str(), _httpAuth.wwwPassword.c_str());
	}

}

const char* AsyncFSWebServer::getHostName() {
	return _config.deviceName.c_str();
}

bool AsyncFSWebServer::factoryReset(bool all) {
	//Delete all json Files
	if (all) {
		Dir dir = _fs->openDir("/");
		while (dir.next()) {
			File entry = dir.openFile("r");
			String filename = String(entry.name());
			entry.close();
			if (filename.endsWith(".json")) _fs->remove(filename);
			filename = "";
		}
	}
	else {
		//delete Lib json files only
		return _ConfigFileHandler.deleteConfigFile(CONFIG_FILE) && _ConfigFileHandler.deleteConfigFile(SECRET_FILE);
	}
}

void AsyncFSWebServer::setJSONCallback(JSON_CALLBACK_SIGNATURE) {
	this->jsoncallback = jsoncallback;
}

void AsyncFSWebServer::setRESTCallback(REST_CALLBACK_SIGNATURE) {
	this->restcallback = restcallback;
}

void AsyncFSWebServer::setPOSTCallback(POST_CALLBACK_SIGNATURE) {
	this->postcallback = postcallback;
}

void AsyncFSWebServer::setSaveConfigCallback(SAVE_CONFIG_CALLBACK_SIGNATURE) {
	this->saveconfigcallback = saveconfigcallback;
}

void AsyncFSWebServer::setUpdateCallback(UPDATE_CALLBACK_SIGNATURE) {
	this->updatecallback = updatecallback;
}

void AsyncFSWebServer::setRestartCallback(RESTART_CALLBACK_SIGNATURE) {
	this->restartcallback = restartcallback;
}

void AsyncFSWebServer::restartESP(void* arg) {
	AsyncFSWebServer* self = reinterpret_cast<AsyncFSWebServer*>(arg);
	if (!self->_restartPending) {
		DEBUGLOG("Restart triggered...\r\n");
		if (self->restartcallback) self->restartcallback();
		self->_restartESPTicker.once(2, &AsyncFSWebServer::s_restartESP, static_cast<void*>(self));
		self->_restartPending = true;
	}
}

void AsyncFSWebServer::s_restartESP(void* arg) {
	AsyncFSWebServer* self = reinterpret_cast<AsyncFSWebServer*>(arg);
	self->_fs->end();
	DEBUGLOG("Restarting...\r\n");
	delay(200);
	ESP.restart();
}

void AsyncFSWebServer::s_toggleLED() {
	static bool ledState = true;
	digitalWrite(CONNECTION_LED, ledState);
	ledState = !ledState;
}

//
// Check the Values is between 0-255
//
boolean AsyncFSWebServer::checkRange(String Value) {
	return ((Value.toInt() >= 0) && (Value.toInt() <= 255));
}

// convert a single hex digit character to its integer value (from https://code.google.com/p/avr-netino/)
unsigned char AsyncFSWebServer::h2int(char c) {
	if (c >= '0' && c <= '9') {
		return((unsigned char)c - '0');
	}
	if (c >= 'a' && c <= 'f') {
		return((unsigned char)c - 'a' + 10);
	}
	if (c >= 'A' && c <= 'F') {
		return((unsigned char)c - 'A' + 10);
	}
	return(0);
}

// (based on https://code.google.com/p/avr-netino/)
String AsyncFSWebServer::urldecode(String input) {
	char c;
	String ret = "";

	for (byte t = 0; t < input.length(); t++) {
		c = input[t];
		if (c == '+') c = ' ';
		if (c == '%') {
			t++;
			c = input[t];
			t++;
			c = (h2int(c) << 4) | h2int(input[t]);
		}
		ret.concat(c);
	}
	return ret;
}

String AsyncFSWebServer::formatBytes(size_t bytes) {
	if (bytes < 1024) {
		return String(bytes) + "B";
	}
	else if (bytes < (1024 * 1024)) {
		return String(bytes / 1024.0) + "KB";
	}
	else if (bytes < (1024 * 1024 * 1024)) {
		return String(bytes / 1024.0 / 1024.0) + "MB";
	}
	else {
		return String(bytes / 1024.0 / 1024.0 / 1024.0) + "GB";
	}
}