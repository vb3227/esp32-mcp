#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include "device_config.h"
#include "native_runtime.h"
#include "relay_client.h"
#include "voice_activity.h"

namespace {
WebServer server(MAIN_PORT);
Preferences skills;
uint32_t reconnectAt = 0;
bool ready = false;
uint32_t resetAt = 0;
bool authorized() {
    const String token = server.header("X-Board-Token");
    size_t expected = strlen(BOARD_TOKEN); unsigned diff = token.length() ^ expected;
    for (size_t i = 0; i < expected; ++i) diff |= static_cast<unsigned char>(BOARD_TOKEN[i]) ^ (i < token.length() ? static_cast<unsigned char>(token[i]) : 0);
    if (!expected || diff) { server.send(403,"application/json","{\"error\":\"Unauthorized\"}"); return false; }
    return true;
}
void reply(JsonDocument &value, int status = 200) { String text; serializeJson(value,text); server.send(status,"application/json",text); }
bool readBody(JsonDocument &value) {
    if (!authorized()) return false;
    if (server.arg("plain").length()>8192 || deserializeJson(value,server.arg("plain"))) { server.send(400,"application/json","{\"error\":\"Invalid bounded JSON\"}"); return false; }
    return true;
}
bool integer(JsonVariantConst value,int low,int high) { return value.is<int>() && !value.is<bool>() && value.as<int>()>=low && value.as<int>()<=high; }
bool programToLua(JsonVariantConst input,String &code,String &error) {
    JsonVariantConst program=input["program"].is<JsonObjectConst>()?input["program"]:input;
    JsonArrayConst steps=program["steps"].as<JsonArrayConst>();
    if(steps.isNull()||!steps.size()||steps.size()>32){error="Expected 1..32 steps";return false;}
    code="local result=nil\n";
    for(JsonObjectConst step:steps){
        String op=step["op"]|"";
        if(op=="delay"&&integer(step["duration_ms"],0,60000)){
            int d=step["duration_ms"].as<int>();code+="for i=1,"+String(d/5000)+" do device.sleep_ms(5000) end\ndevice.sleep_ms("+String(d%5000)+")\n";
        } else if(op=="sensor.read"||op=="sensor.read.display")code+="result={sensors=device.info()}\n";
        else if(op=="gpio.write"&&integer(step["pin"],0,63)&&integer(step["value"],0,1))code+="device.digital_write("+String(step["pin"].as<int>())+","+String(step["value"].as<int>())+")\n";
        else if((op=="gpio.read"||op=="adc.read")&&integer(step["pin"],0,63))code+="result={value=device."+String(op=="gpio.read"?"digital_read":"analog_read")+"("+String(step["pin"].as<int>())+")}\n";
        else {error="Unsupported operation in this board profile: "+op;return false;}
    }
    code+="return result";return code.length()<=4096;
}
bool validName(const String &name){if(!name.length()||name.length()>31)return false;for(char c:name)if(!(c>='a'&&c<='z')&&!(c>='0'&&c<='9')&&c!='_'&&c!='-')return false;return true;}
String keyFor(const String &name){uint32_t h=2166136261UL;for(char c:name)h=(h^static_cast<uint8_t>(c))*16777619UL;return String("s")+String(h,HEX);}
}

// There is no voice UI on a generic board; ignore cosmetic relay events.
bool receiveVoiceActivity(const char *,uint32_t,const char *){return false;}

void setup(){
    Serial.begin(115200);
    initDeviceConfig();
    if (!deviceConfigReady()) return;
    Serial.println("[boot] 5% display / serial; by vb3227");
    if(strlen(BOARD_TOKEN)<24){Serial.println("Set BOARD_TOKEN to at least 24 random characters");return;}
    Serial.println("[boot] 30% security ready");
    initNativeRuntime();
    Serial.println("[boot] 60% native runtime ready");
    skills.begin("mcp_json",false);
    const char *headers[]={"X-Board-Token","X-Voice-Epoch"};server.collectHeaders(headers,2);
    server.on("/ping",HTTP_GET,[](){if(authorized())server.send(200,"text/plain","pong");});
    attachNativeRuntime(server);
    server.on("/reset",HTTP_POST,[](){if(!authorized())return;server.send(202,"application/json","{\"reset\":true}");resetAt=millis()+250;});
    server.on("/run",HTTP_POST,[](){
        JsonDocument request,value;if(!readBody(request))return;String code,error;
        if(!programToLua(request.as<JsonVariantConst>(),code,error)){value["error"]=error;reply(value,400);return;}
        bool ok=startNativeScript(code,60000,value);reply(value,ok?202:400);
    });
    server.on("/status",HTTP_GET,[](){if(!authorized())return;JsonDocument value;if(!readNativeJob(server.arg("job_id"),value)){value["error"]="Unknown job ID";reply(value,404);return;}reply(value);});
    server.on("/stop",HTTP_POST,[](){JsonDocument request,value;if(!readBody(request))return;bool ok=stopNativeJob(request["job_id"]|"");value["stop_requested"]=ok;reply(value,ok?200:404);});
    server.on("/skills/install",HTTP_POST,[](){
        JsonDocument request,value;if(!readBody(request))return;String name=request["name"]|"",code,error;
        if(!validName(name)||!programToLua(request["program"],code,error)){value["error"]="Invalid skill: "+error;reply(value,400);return;}
        String key=keyFor(name),previous=skills.getString(key.c_str(),"");JsonDocument old;
        if(previous.length()&&(deserializeJson(old,previous)||old["name"].as<String>()!=name)){value["error"]="Storage key collision";reply(value,409);return;}
        JsonDocument index;deserializeJson(index,skills.getString("index","[]"));bool listed=false;size_t total=0;
        for(JsonVariant entry:index.as<JsonArray>()){String n=entry.as<String>();if(n==name)listed=true;else total+=skills.getString(keyFor(n).c_str(),"").length();}
        String data;serializeJson(request,data);if((!listed&&index.size()>=12)||data.length()>4096||total+data.length()>12000){value["error"]="Skill quota exceeded";reply(value,413);return;}
        if(!listed) index.add(name);
        String names;
        serializeJson(index,names);
        bool ok=skills.putString(key.c_str(),data)==data.length()&&skills.putString("index",names)==names.length();value["stored"]=ok;value["name"]=name;
        if(ok&&(request["run"]|false)) ok=startNativeScript(code,60000,value);
        reply(value,ok?200:507);
    });
    server.on("/skills/list",HTTP_GET,[](){if(!authorized())return;JsonDocument index,value;deserializeJson(index,skills.getString("index","[]"));value["skills"].set(index);reply(value);});
    server.on("/skills/run",HTTP_POST,[](){JsonDocument request,saved,value;if(!readBody(request))return;String name=request["name"]|"";if(!validName(name)||deserializeJson(saved,skills.getString(keyFor(name).c_str(),""))||saved["name"].as<String>()!=name){value["error"]="Unknown skill";reply(value,404);return;}String code,error;bool ok=programToLua(saved["program"],code,error)&&startNativeScript(code,60000,value);if(!ok&&error.length())value["error"]=error;reply(value,ok?202:400);});
    server.on("/skills/delete",HTTP_POST,[](){JsonDocument request,index,value;if(!readBody(request))return;String name=request["name"]|"";if(!validName(name)){value["error"]="Invalid name";reply(value,400);return;}deserializeJson(index,skills.getString("index","[]"));JsonDocument kept;JsonArray a=kept.to<JsonArray>();for(JsonVariant entry:index.as<JsonArray>())if(entry.as<String>()!=name)a.add(entry);skills.remove(keyFor(name).c_str());String names;serializeJson(kept,names);bool ok=skills.putString("index",names)==names.length();value["removed"]=ok;reply(value,ok?200:507);});
    WiFi.mode(WIFI_STA);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);server.begin();startRelayClient();
    Serial.println("[boot] 100% core ready; by vb3227");
    Serial.println("ESP32 MCP ready; native Lua enabled; GPIO restricted by board policy");
    ready = true;
}
void loop(){if(!ready){serviceDeviceSetup();delay(5);return;}server.handleClient();if(resetAt&&static_cast<int32_t>(millis()-resetAt)>=0)ESP.restart();serviceRelaySerial();if(WiFi.status()!=WL_CONNECTED&&millis()-reconnectAt>10000){reconnectAt=millis();WiFi.reconnect();}delay(1);}
