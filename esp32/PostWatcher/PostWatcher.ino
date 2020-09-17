#include <Arduino.h>
#include <WiFi.h>
//#include <string.h>
#include <EEPROM.h>
#include <ssl_client.h>
#include <WiFiClientSecure.h>
//#include <esp_wifi.h>
#include "./secrets.h"

#define EEPROM_SIZE 192
#define SETTING_BUTTON GPIO_NUM_16

// 扉が閉じているときHIGH
#define SENSOR_PIN GPIO_NUM_15

struct st_wifi_conf {
    char ssid[64];
    char pass[64];
    char line_user_id[64]; //userIDに限らずgroupIDの入力も可能
};

const char *ap_ssid = "PostWatcher-Setting";
const char *ap_pass = "12345678";

IPAddress LIP;
WiFiServer server(80);

st_wifi_conf wifi_conf;

// このへんを secrets.h に移して公開
//const char CHANNEL_SECRET[] = "";
//const char CHANNEL_TOKEN[] = "";

// ステータス変数
bool setting_mode = false;
bool setting_mode_configured = false;
bool wifi_connected = false;
bool message_sent = false;

// deep sleep前のセンサーの状態を記録する変数
RTC_DATA_ATTR bool door_open = true;

void setup() {
    Serial.begin(115200);
    delay(100);

    EEPROM.begin(EEPROM_SIZE);
    delay(100);

    EEPROM.get<st_wifi_conf>(0, wifi_conf);

    pinMode(SENSOR_PIN, INPUT);
    pinMode(SETTING_BUTTON, INPUT_PULLUP);
    delay(100);

    Serial.println("Wake Up.");
    Serial.println(esp_sleep_get_wakeup_cause());
    if( digitalRead(SETTING_BUTTON) ){
        // 監視モードで起動
        if(door_open) {
            door_open = false;
            esp_sleep_enable_ext0_wakeup(SENSOR_PIN, 0);
        } else {
            door_open = true;
            if( connect_wifi() ){
                send_message("何かが投函されたようです！");
                Serial.println("Posting!");
                WiFi.disconnect(true);
            }
            esp_sleep_enable_ext0_wakeup(SENSOR_PIN, 1);
        }
        Serial.println("Going to sleep now.");
        esp_deep_sleep_start();
    } else {
        // 設定モード
        //Serial.setDebugOutput(true);
        Serial.print("Setting WIFI_MODE_NULL...");
        //WiFi.mode(WIFI_MODE_NULL);
         WiFi.disconnect(true, true);
        Serial.println("Done.");
        delay(100);
        //esp_wifi_restore();
        Serial.print("Setting WIFI_AP_STA...");
        WiFi.mode(WIFI_AP_STA);
        Serial.println("Done.");
        delay(500);

        Serial.print("Starting WiFi AP...");
        WiFi.softAP(ap_ssid, ap_pass);
        Serial.println("Done.");
        delay(500);
                
        server.begin();
        Serial.println("Server Started.");
        //Serial.println( WiFi.getMode() );
    }
    Serial.println("Setup done.");
}

void loop() {
    check_client();
}

void check_client() {
    String response_for_get_prefix = "<!DOCTYPE html>\
<html><head><meta charset='UTF-8'><title>Post form test</title></head>\
<body><form method='POST'>\
<p>SSID: ";
    String response_for_get_suffix = "</p>\
<p>Password: <input type='password' name='pass'></p>\
<p>トークID: <input type='text' name='lineid'></p>\
<input type='submit' value='送信'></form>";

    WiFiClient client = server.available();

    if( client ) {
        String _str = "";
        uint16_t status_code = 500;
        while( client.connected() ) {
            // int client.available() -> 読み出し可能バイト数
            if( client.available() >= 3) {
                _str = client.readStringUntil(' ');
                if( _str.startsWith("POST") ){
                    status_code = post_parser(client);
                } else if( _str.startsWith("GET") ){
                    status_code = get_parser(client);
                }
            }

            while( client.available() ) {
                client.readString();
            }

            client.print("HTTP/1.1 ");
            client.println(status_code);
            if( status_code == 200 ) {
                String current_status = "<h3>現在の設定</h3>\r\n<p>SSID: ";
                current_status += escapeHTML( String(wifi_conf.ssid) );
                current_status += " (HEX: ";
                for(uint8_t i=0; i<64; i++) {
                    current_status += (String(wifi_conf.ssid[i], HEX));
                }

                current_status += ")</p><p>Password: ";
                for(uint8_t i=0; i<strlen(wifi_conf.pass); i++) {
                    current_status += "●";    // セキュリティのためパスワードは表示しない
                }
                current_status += "</p>";

                client.println("Content-Type:text/html");
                client.println();
                client.println(response_for_get_prefix);
                int count = list_ssid(client);
                if( count==0 ) {
                    client.println("No Access Points Found.</p><p>Input SSID: <input type='text' name='ssid'>");
                }
                client.println(response_for_get_suffix);
                client.println(current_status);
                client.println("</body></html>");
            } else {
                client.println(status_code);
            }
            client.println();
            break;
        }

        client.stop();
        if( message_sent ) {
            door_open = true;
            Serial.println("Deep sleep start.");
            esp_sleep_enable_ext0_wakeup(SENSOR_PIN, 1);
        }
    }
}


uint16_t post_parser(WiFiClient client) {
    Serial.println("POST request coming!");
    String _str, line;
    do {
        line = client.readStringUntil('\n');
        if( 0 && line.startsWith("Content-Length") ) {
            // Content-Length の検証

            // strlen("Content-Length") => 14
            char _len[6];
            unsigned int len;
            line.substring(15).toCharArray(_len,5);
            sscanf(_len, "%ul", &len);
        }
    } while( line.length()>1 );

    if( client.available() ){
        String host_ssid, host_pass, lineid;

        line = client.readStringUntil('\r');

        int amp_index=0;
        unsigned int head=0;
        
        while( amp_index != -1 ) {
            amp_index = line.indexOf('&', head);
            String section;
            
            if( amp_index == -1) {
                section = line.substring(head);
            } else { 
                section = line.substring(head, amp_index);
            }

            Serial.println(section);

            if(section.startsWith("ssid")) {
                Serial.println("SSID!");
                host_ssid = decodeURI( section.substring(section.indexOf('=')+1) );
                Serial.println(host_ssid);
            } else if( section.startsWith("pass") ) {
                Serial.println("Password!");
                host_pass = decodeURI( section.substring(section.indexOf('=')+1) );
                Serial.println(host_pass);
            } else if( section.startsWith("lineid") ) {
                Serial.println("Talk room ID!");
                lineid = decodeURI( section.substring(section.indexOf('=')+1) );
                Serial.println(lineid);
            } else {
                Serial.println("no match.");
            }

            head = amp_index+1;
        };
        
        write_eeprom(host_ssid, host_pass, lineid);

        if(! message_sent ) {
            message_sent = send_testmessage();
        }
    }
    return 200;
}


uint16_t get_parser(WiFiClient client) {
    Serial.println("GET request coming!");
    String path = client.readStringUntil(' ');
    if( path == "/favicon.ico" ) {
        Serial.println("Icon Requested!");
        return 404;
    }
    return 200;
}


bool send_message(String message) {
    const char* host = "api.line.me";
    const char* token = CHANNEL_TOKEN;
    char user_id[64];
    strcpy(user_id, wifi_conf.line_user_id);

    String query = "{ \"to\": \""+ String(user_id) + "\", \r\n" +
    "\"messages\": [ {\"type\": \"text\", \"text\": \""+ message + "\"}]}";

    String request = String("") +
                "POST /v2/bot/message/push HTTP/1.1\r\n" +
                "Host: " + host + "\r\n" +
                "Authorization: Bearer " + token + "\r\n" +
                "Content-Length: " + String(query.length()) +  "\r\n" + 
                "Content-Type: application/json\r\n\r\n" +
                query + "\r\n";

    Serial.println(query);

    WiFiClientSecure client;
    Serial.println("Try");
    //LineのAPIサーバに接続
    if (!client.connect(host, 443)) {
        Serial.println("Connection failed");
        return false;
    }
    Serial.println("Connected");

    //リクエストを送信
    client.print(request);

    client.readStringUntil(' ');
    String status_code_string = client.readStringUntil(' ');
    //uint16_t status_code = status_code_string.toInt();

    //受信終了まで待つ 
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        Serial.println(line);
        if (line == "\r") {
            break;
        }
    }

    String line = client.readStringUntil('\n');
    Serial.println(line);
    
    if( status_code_string.startsWith("2") ) {
        return true;
    } else {
        return false;
    }
}

bool connect_wifi() {
    uint8_t i=0;

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_conf.ssid, wifi_conf.pass);
    while(WiFi.status() != WL_CONNECTED) {
        delay(1000);
        if(++i>5) {
            Serial.println("Cannot connect WiFi.");
            return false;
        }
    }
    wifi_connected = true;
    Serial.println("WiFi Connected.");
    return true;
}

bool write_eeprom(String ssid, String pass, String lineid) {
    Serial.print("SSID: ");
    Serial.println(ssid);
    Serial.print("Password: ");
    Serial.println(pass);

    if( ssid.length() ) 
        ssid.toCharArray(wifi_conf.ssid, 64, 0);
    if( pass.length() )    
        pass.toCharArray(wifi_conf.pass, 64, 0);
    if( lineid.length() )
        lineid.toCharArray(wifi_conf.line_user_id, 64, 0);
    
    EEPROM.put<st_wifi_conf>(0, wifi_conf);
    EEPROM.commit();
}

bool send_testmessage(void) {
    uint8_t i=0;
    WiFi.begin(wifi_conf.ssid, wifi_conf.pass);
    while(WiFi.status() != WL_CONNECTED) {
        delay(1000);
        if(++i>5) {
            return false;
        }
    }
    wifi_connected = true;
    Serial.println("WiFi Connected.");
    message_sent = send_message("ようこそ、Post-Watcherへ！");
    if( message_sent ){
        Serial.println("LINE message sent.");
        return true;
    } else {
        Serial.println("ERROR while sending LINE message.");
        return false;
    }
}

String decodeURI(String encoded) {
    String decoded = encoded;
    int head = 0;
    int index;
    char hex[2];
    
    while( (index=decoded.indexOf('+', head) ) >= 0) {
        decoded.setCharAt(index, ' ');
        head = index+1;
    }
    
    head = 0;
    while( (index=decoded.indexOf('%', head) ) >= 0) {
        hex[0] = decoded.charAt(index+1);
        hex[1] = decoded.charAt(index+2);
        decoded.setCharAt(index, hex2char(hex));
        decoded.remove(index+1,2);
        head = index+1;
    }
    return decoded;
}

char hex2char(char* hex) {
    return hex2int(hex[0])<<4 | hex2int(hex[1]);
}

uint8_t hex2int(char hex) {
    if( hex < '0') {
        return 0;
    } else if( hex <= '9') {
        return hex-'0';
    } else if( hex < 'A') {
        return 10;
    } else if( hex <= 'F') {
        return 10+hex-'A';
    } else {
        return 0;
    }
}

int list_ssid(WiFiClient client) {
    Serial.println("Scan start");

    WiFi.disconnect();
    int n = WiFi.scanNetworks();
    Serial.println("Scan done.");
    
    if( n==0 ) {
        Serial.println("no access point found");
    } else {
        Serial.print(n);
        Serial.println(" networks found.");
        client.println("<select name='ssid'>");
        for(int i=0; i<n; ++i) {
            String ssid = WiFi.SSID(i);
            client.print(String("")+"<option value='"+ssid+"'>");
            client.print(escapeHTML(ssid));
            client.print(" (");
            client.print(WiFi.RSSI(i));
            client.print(") ");
            client.println("</option>");
        }
        client.println("</select>");
    }
    return n;
}

String escapeHTML(String str) {
    String escaped = String(str);
    escaped.replace(String('"'), "&quot;");
    escaped.replace(">", "&gt;");
    escaped.replace("<", "&lt;");
    return escaped;
}

String escapeURI(String str) {
    String escaped = String(str);
    escaped.replace(String('"'), "%3C");
    escaped.replace(">", "%3E");
    escaped.replace("<", "%3C");
    return escaped;
}