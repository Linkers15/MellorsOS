#include "MellorsOSCore.h"
#include <Arduino.h>
#include <memory>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wpa2.h>
#include <driver/i2s.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiUdp.h> // MScript UDP transport
#include <ESPmDNS.h>
#include <esp_system.h>
#include <math.h>
#include <time.h>
#include "MellorsTimeFormat.h"
#include <new>
#include "esp32-hal-psram.h"
#include "cyberpi.h"
extern "C" {
#include "gyro/gyro.h"
#include "io/aw9523b.h"
}


CyberPi cyber;

struct AccentChoice{const char*name;const char*hex;};
static const AccentChoice ACCENTS[]={
  {"Indigo","#4F46E5"},
  {"Pink","#EC4899"},
  {"Teal","#0D9488"},
  {"Orange","#F97316"},
  {"Violet","#8B5CF6"},
  {"Blue","#2563EB"},
  {"Gold","#EAB308"}
};
static const int ACCENT_COUNT=sizeof(ACCENTS)/sizeof(ACCENTS[0]);
static String accentNameFor(String hex){
  hex.toUpperCase();
  for(int i=0;i<ACCENT_COUNT;i++){String h=ACCENTS[i].hex;h.toUpperCase();if(h==hex)return ACCENTS[i].name;}
  return "Custom";
}
static String canonicalAccent(String hex){
  String h=hex;h.toUpperCase();
  if(h=="#6D7CFF")return "#4F46E5";
  if(h=="#F0528A")return "#EC4899";
  if(h=="#21B8A6")return "#0D9488";
  if(h=="#FF914D")return "#F97316";
  if(h=="#9A64FF")return "#8B5CF6";
  if(h=="#3C94FF")return "#2563EB";
  if(h=="#E6B63C")return "#EAB308";
  return hex;
}

static TaskHandle_t G_SOUND_TASK=nullptr;
static TaskHandle_t G_SENSOR_TASK=nullptr;
static bool G_SOUND_TASK_AWAKE=false;
static unsigned long G_SOUND_ACTIVE_UNTIL=0;
static unsigned long G_LAST_GYRO_REFRESH=0;

static void synthSleep(){
  if(G_SOUND_TASK&&G_SOUND_TASK_AWAKE){vTaskSuspend(G_SOUND_TASK);G_SOUND_TASK_AWAKE=false;}
  aw_digitalWrite(AW_P1_3,LOW);
}
static void synthWake(){
  if(G_SOUND_TASK&&!G_SOUND_TASK_AWAKE){
    vTaskResume(G_SOUND_TASK);
    G_SOUND_TASK_AWAKE=true;
    // Give Makeblock's renderer one scheduler turn to refill I2S before a note
    // is queued. This keeps reliable tones without burning CPU while idle.
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
static void quietMakeblockTasks(){
  delay(30);
  G_SOUND_TASK=xTaskGetHandle("on_sound_read");
  G_SENSOR_TASK=xTaskGetHandle("on_sensor_read");
  if(G_SOUND_TASK){vTaskSuspend(G_SOUND_TASK);G_SOUND_TASK_AWAKE=false;}
  aw_digitalWrite(AW_P1_3,LOW);
  if(G_SENSOR_TASK)vTaskSuspend(G_SENSOR_TASK);
}
static void serviceQuietHardware(){
  unsigned long now=millis();
  if(now-G_LAST_GYRO_REFRESH>=25){
    G_LAST_GYRO_REFRESH=now;
    gyro_read();
  }
  if(G_SOUND_ACTIVE_UNTIL&&(long)(now-G_SOUND_ACTIVE_UNTIL)>=0){
    G_SOUND_ACTIVE_UNTIL=0;
    synthSleep();
  }
}
static void quietBeep(int midi,int durationMs){
  durationMs=constrain(durationMs,1,255);
  synthWake();
  cyber.set_pitch(0,(uint8_t)constrain(midi,0,127),(uint8_t)durationMs);
  aw_digitalWrite(AW_P1_3,HIGH);
  // Keep the renderer awake briefly after the requested note so queued I2S
  // samples finish before the task is suspended again.
  unsigned long until=millis()+(unsigned long)durationMs+120;
  if((long)(until-G_SOUND_ACTIVE_UNTIL)>0)G_SOUND_ACTIVE_UNTIL=until;
}
static void quietSoundStop(){
  G_SOUND_ACTIVE_UNTIL=0;
  synthSleep();
}


// Optional battery provider. The open CyberPi Arduino library does not expose
// the Pocket Shield state-of-charge. A board/shield driver may provide this
// weak hook later; Home only renders a percentage when a real value exists.
extern "C" int mellorsBatteryPercent() __attribute__((weak));
static int readBatteryPercent(){
  if(mellorsBatteryPercent){int p=mellorsBatteryPercent();if(p>=0&&p<=100)return p;}
  return -1;
}

static const int SCREEN_W=128, SCREEN_H=128;
static const int MAX_APPS=24, MAX_UI=32, MAX_VARS=32;
static const int MAX_TIMERS=12, MAX_NOTIFICATIONS=24, MAX_ENTITIES=24;
static const char* OS_NAME="MellorsOS";
static const char* OS_VERSION="2.0";
static const char* ENGINE_VERSION="MScript 2.0";
static const char* FIRMWARE_BUILD=__DATE__ " " __TIME__;

static const uint8_t FONT5X10[95][10] = {
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x04,0x04,0x04,0x04,0x00,0x00,0x04,0x00,0x00,0x00},
  {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x0C,0x08,0x0E,0x0A,0x0F,0x06,0x04,0x00,0x00,0x00},
  {0x0E,0x06,0x06,0x0C,0x0C,0x0D,0x06,0x00,0x00,0x00},
  {0x03,0x0B,0x01,0x0C,0x16,0x12,0x09,0x00,0x00,0x00},
  {0x06,0x06,0x06,0x1F,0x05,0x09,0x1E,0x00,0x00,0x00},
  {0x04,0x04,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x0C,0x04,0x02,0x02,0x02,0x04,0x0C,0x00,0x00},
  {0x00,0x02,0x04,0x0C,0x08,0x0C,0x04,0x02,0x00,0x00},
  {0x00,0x04,0x04,0x0A,0x06,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x04,0x04,0x0F,0x04,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x04,0x04,0x04,0x04,0x00},
  {0x00,0x00,0x00,0x00,0x0F,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x04,0x04,0x00,0x00,0x00},
  {0x08,0x08,0x08,0x04,0x04,0x02,0x02,0x02,0x00,0x00},
  {0x0E,0x0A,0x09,0x1D,0x0B,0x0A,0x0E,0x00,0x00,0x00},
  {0x04,0x06,0x04,0x04,0x04,0x04,0x1E,0x00,0x00,0x00},
  {0x0E,0x08,0x08,0x0C,0x04,0x02,0x0F,0x00,0x00,0x00},
  {0x0E,0x08,0x08,0x06,0x08,0x08,0x0E,0x00,0x00,0x00},
  {0x0C,0x0C,0x0A,0x08,0x0F,0x08,0x0C,0x00,0x00,0x00},
  {0x0E,0x02,0x02,0x06,0x08,0x08,0x0E,0x00,0x00,0x00},
  {0x0E,0x02,0x0F,0x0B,0x1B,0x0A,0x0E,0x00,0x00,0x00},
  {0x0E,0x08,0x04,0x04,0x04,0x02,0x02,0x00,0x00,0x00},
  {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,0x00,0x00,0x00},
  {0x0E,0x09,0x09,0x09,0x0E,0x08,0x06,0x00,0x00,0x00},
  {0x00,0x00,0x04,0x04,0x00,0x04,0x04,0x00,0x00,0x00},
  {0x00,0x00,0x04,0x04,0x00,0x04,0x04,0x04,0x04,0x00},
  {0x00,0x00,0x00,0x0C,0x02,0x04,0x08,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x0F,0x00,0x0F,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x01,0x06,0x08,0x04,0x01,0x00,0x00,0x00},
  {0x0E,0x0A,0x08,0x04,0x04,0x00,0x04,0x00,0x00,0x00},
  {0x0E,0x0C,0x01,0x0B,0x0B,0x16,0x0E,0x00,0x00,0x00},
  {0x04,0x04,0x0E,0x0A,0x0E,0x08,0x19,0x00,0x00,0x00},
  {0x0F,0x0A,0x0A,0x0E,0x0A,0x0A,0x0F,0x00,0x00,0x00},
  {0x0E,0x02,0x03,0x01,0x03,0x02,0x0E,0x00,0x00,0x00},
  {0x0F,0x0A,0x12,0x12,0x12,0x0A,0x0F,0x00,0x00,0x00},
  {0x0F,0x02,0x02,0x0E,0x02,0x02,0x0F,0x00,0x00,0x00},
  {0x1E,0x02,0x02,0x0E,0x02,0x02,0x06,0x00,0x00,0x00},
  {0x0E,0x02,0x01,0x01,0x09,0x0A,0x0E,0x00,0x00,0x00},
  {0x0B,0x0A,0x0A,0x0E,0x0A,0x0A,0x0B,0x00,0x00,0x00},
  {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E,0x00,0x00,0x00},
  {0x0E,0x08,0x08,0x08,0x08,0x09,0x07,0x00,0x00,0x00},
  {0x0B,0x02,0x06,0x06,0x06,0x0A,0x1B,0x00,0x00,0x00},
  {0x06,0x02,0x02,0x02,0x02,0x12,0x1E,0x00,0x00,0x00},
  {0x19,0x1B,0x1B,0x1B,0x15,0x15,0x19,0x00,0x00,0x00},
  {0x0B,0x0A,0x0E,0x0C,0x0C,0x08,0x0B,0x00,0x00,0x00},
  {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E,0x00,0x00,0x00},
  {0x0E,0x0A,0x0A,0x0A,0x06,0x02,0x06,0x00,0x00,0x00},
  {0x0E,0x0A,0x11,0x11,0x11,0x09,0x0E,0x08,0x00,0x00},
  {0x0F,0x0A,0x0A,0x06,0x0A,0x0A,0x1B,0x00,0x00,0x00},
  {0x0E,0x0A,0x02,0x0C,0x08,0x09,0x0E,0x00,0x00,0x00},
  {0x1F,0x04,0x04,0x04,0x04,0x04,0x0E,0x00,0x00,0x00},
  {0x0B,0x0A,0x0A,0x0A,0x0A,0x0A,0x0E,0x00,0x00,0x00},
  {0x1B,0x02,0x0A,0x0A,0x0A,0x04,0x04,0x00,0x00,0x00},
  {0x11,0x15,0x05,0x0F,0x0A,0x0A,0x0A,0x00,0x00,0x00},
  {0x1B,0x0A,0x06,0x04,0x06,0x0A,0x1B,0x00,0x00,0x00},
  {0x1B,0x0A,0x0A,0x04,0x04,0x04,0x06,0x00,0x00,0x00},
  {0x0E,0x08,0x04,0x04,0x02,0x02,0x0F,0x00,0x00,0x00},
  {0x0E,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x0C,0x00},
  {0x01,0x02,0x02,0x04,0x04,0x04,0x08,0x08,0x00,0x00},
  {0x06,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x06,0x00},
  {0x00,0x04,0x06,0x0A,0x09,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x00,0x00},
  {0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x0E,0x08,0x0E,0x0A,0x0E,0x00,0x00,0x00},
  {0x03,0x02,0x0E,0x0A,0x0A,0x0A,0x0E,0x00,0x00,0x00},
  {0x00,0x00,0x0E,0x03,0x01,0x02,0x0E,0x00,0x00,0x00},
  {0x08,0x08,0x0E,0x0A,0x09,0x09,0x0E,0x00,0x00,0x00},
  {0x00,0x00,0x0E,0x0A,0x0F,0x02,0x0E,0x00,0x00,0x00},
  {0x1C,0x04,0x0E,0x04,0x04,0x04,0x06,0x00,0x00,0x00},
  {0x00,0x00,0x1E,0x0A,0x09,0x0B,0x0E,0x08,0x0E,0x00},
  {0x03,0x02,0x0E,0x0A,0x0A,0x0A,0x0B,0x00,0x00,0x00},
  {0x04,0x00,0x06,0x04,0x04,0x04,0x0E,0x00,0x00,0x00},
  {0x0C,0x00,0x0E,0x08,0x08,0x08,0x08,0x08,0x06,0x00},
  {0x03,0x02,0x0A,0x06,0x06,0x06,0x0B,0x00,0x00,0x00},
  {0x06,0x04,0x04,0x04,0x04,0x04,0x0C,0x00,0x00,0x00},
  {0x00,0x00,0x0F,0x15,0x15,0x15,0x15,0x00,0x00,0x00},
  {0x00,0x00,0x0F,0x0A,0x0A,0x0A,0x0B,0x00,0x00,0x00},
  {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E,0x00,0x00,0x00},
  {0x00,0x00,0x0F,0x0A,0x0A,0x0A,0x0E,0x02,0x03,0x00},
  {0x00,0x00,0x0E,0x0A,0x09,0x09,0x0E,0x08,0x08,0x00},
  {0x00,0x00,0x0F,0x02,0x02,0x02,0x06,0x00,0x00,0x00},
  {0x00,0x00,0x0E,0x02,0x0E,0x08,0x0E,0x00,0x00,0x00},
  {0x00,0x02,0x0E,0x02,0x02,0x02,0x0C,0x00,0x00,0x00},
  {0x00,0x00,0x0B,0x0A,0x0A,0x0A,0x0E,0x00,0x00,0x00},
  {0x00,0x00,0x1B,0x0A,0x0A,0x06,0x04,0x00,0x00,0x00},
  {0x00,0x00,0x11,0x15,0x0F,0x0A,0x0A,0x00,0x00,0x00},
  {0x00,0x00,0x1B,0x0A,0x04,0x0E,0x1B,0x00,0x00,0x00},
  {0x00,0x00,0x1B,0x0A,0x0A,0x06,0x04,0x04,0x06,0x00},
  {0x00,0x00,0x0F,0x0C,0x04,0x02,0x0F,0x00,0x00,0x00},
  {0x0C,0x04,0x04,0x04,0x02,0x04,0x04,0x04,0x0C,0x00},
  {0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00,0x00},
  {0x06,0x04,0x04,0x04,0x0C,0x04,0x04,0x04,0x02,0x00},
  {0x00,0x00,0x00,0x06,0x0D,0x00,0x00,0x00,0x00,0x00}
};

static uint32_t rgb(const String&s){const char*p=s.c_str();while(*p&&isspace((unsigned char)*p))p++;if(*p=='#')p++;else if(p[0]=='0'&&(p[1]=='x'||p[1]=='X'))p+=2;return strtoul(p,nullptr,16)&0xFFFFFF;}
static uint32_t hash32(const String&s){uint32_t h=2166136261UL;for(size_t i=0;i<s.length();++i){h^=(uint8_t)s[i];h*=16777619UL;}return h;}
static String hex8(uint32_t v){char b[9];snprintf(b,sizeof(b),"%08lx",(unsigned long)v);return String(b);}
static String boolText(bool v){return v?"ON":"OFF";}
static String safeName(String s){s.replace("/","_");s.replace("\\","_");if(!s.length()||s=="."||s=="..")s="item";return s;}
static String baseName(String p){while(p.length()>1&&p.endsWith("/"))p.remove(p.length()-1);int q=p.lastIndexOf('/');return q>=0?p.substring(q+1):p;}
static String parentPath(String p){while(p.length()>1&&p.endsWith("/"))p.remove(p.length()-1);int q=p.lastIndexOf('/');return q>0?p.substring(0,q):String("/");}
static bool textFilePath(String p){String x=p;x.toLowerCase();return x.endsWith(".txt")||x.endsWith(".md")||x.endsWith(".json")||x.endsWith(".csv")||x.endsWith(".log")||x.endsWith(".mscript")||x.endsWith(".mapp")||x.endsWith(".ini")||x.endsWith(".cfg");}
static bool copyFilePath(String from,String to){File in=LittleFS.open(from,"r");if(!in||in.isDirectory())return false;int slash=to.lastIndexOf('/');if(slash>0){String dir=to.substring(0,slash);if(!LittleFS.exists(dir))LittleFS.mkdir(dir);}File out=LittleFS.open(to,"w");if(!out){in.close();return false;}uint8_t buf[256];while(in.available()){size_t n=in.read(buf,sizeof(buf));if(n)out.write(buf,n);}in.close();out.close();return true;}
static bool removeTree(String path){File f=LittleFS.open(path);if(!f)return false;if(!f.isDirectory()){f.close();return LittleFS.remove(path);}File child=f.openNextFile();while(child){String cp=child.path();bool dir=child.isDirectory();child.close();if(dir)removeTree(cp);else LittleFS.remove(cp);child=f.openNextFile();}f.close();return LittleFS.rmdir(path);}
static void ensureFolder(String path){if(!LittleFS.exists(path))LittleFS.mkdir(path);}
// Private app packages share LittleFS storage but are outside the public Files namespace.
static String normalizedFsPath(String path){
  if(!path.startsWith("/")||path.indexOf('\\')>=0)return "";
  String out="";int st=1;
  while(st<=(int)path.length()){
    int q=path.indexOf('/',st);String part=q<0?path.substring(st):path.substring(st,q);
    if(part=="..")return "";
    for(size_t i=0;i<part.length();i++)if((unsigned char)part[i]<32)return "";
    if(part.length()&&part!=".")out+="/"+part;
    if(q<0)break;st=q+1;
  }
  return out.length()?out:String("/");
}
static bool privateAppPath(String path){path=normalizedFsPath(path);return path=="/system"||path.startsWith("/system/")||path=="/installed"||path.startsWith("/installed/");}
static bool publicFilePath(String path){path=normalizedFsPath(path);return path.length()&&!privateAppPath(path);}
static bool ensureDirectories(String path){
  path=normalizedFsPath(path);if(!path.length())return false;
  for(int i=1;i<=(int)path.length();i++)if(i==(int)path.length()||path[i]=='/'){
    String p=path.substring(0,i);if(!LittleFS.exists(p)&&!LittleFS.mkdir(p))return false;
  }return true;
}
static String stagingPath(){
  if(!ensureDirectories("/system/staging"))return "";
  for(int i=0;i<16;i++){String p=String("/system/staging/")+hex8(esp_random())+".tmp";if(!LittleFS.exists(p))return p;}
  return "";
}
static bool atomicWriteFile(String path,const uint8_t*bytes,size_t count){
  path=normalizedFsPath(path);if(path=="/"||!path.length())return false;
  if(!ensureDirectories(path.substring(0,path.lastIndexOf('/'))))return false;
  String temp=stagingPath();if(!temp.length())return false;File f=LittleFS.open(temp,"w");if(!f)return false;
  size_t wrote=f.write(bytes,count);f.close();
  if(wrote!=count){LittleFS.remove(temp);return false;}
  if(!LittleFS.rename(temp,path)){LittleFS.remove(temp);return false;}return true;
}
static bool isPkg(const String&s){if(!s.length()||s.length()>64||!isalpha((unsigned char)s[0]))return false;for(size_t i=1;i<s.length();++i){char c=s[i];if(!(isalnum((unsigned char)c)||c=='.'||c=='_'||c=='-'))return false;}return true;}

static String safeHostname(String s){
  s.toLowerCase();String out="";
  for(size_t i=0;i<s.length()&&out.length()<32;i++){
    char c=s[i];
    if(isalnum((unsigned char)c)||c=='-')out+=c;
    else if((c==' '||c=='_'||c=='.')&&out.length()&&out[out.length()-1]!='-')out+='-';
  }
  while(out.startsWith("-"))out.remove(0,1);
  while(out.endsWith("-")&&out.length())out.remove(out.length()-1);
  if(!out.length())out="mellorsos";
  return out;
}
static String defaultHostname(){
  uint64_t id=ESP.getEfuseMac();
  char b[24];snprintf(b,sizeof(b),"mellorsos-%04x",(unsigned int)(id&0xFFFF));
  return String(b);
}
static String pinDigest(const String&pin){
  uint64_t id=ESP.getEfuseMac();
  String material=String((uint32_t)(id>>32),HEX)+":"+String((uint32_t)id,HEX)+":"+pin+":MellorsOS-v0.9.1";
  return hex8(hash32(material));
}

struct InputState{int x=0,y=0;bool select=false,a=false,b=false,home=false;};
enum Action{NONE,UP,DOWN,LEFT,RIGHT,SELECT,BACK,HOME_SINGLE,HOME_DOUBLE,HOME_HOLD};

class HomeGesture{
  bool down=false,longSent=false;unsigned long pressedAt=0,lastRelease=0;int clicks=0;
public:
  void reset(){down=false;longSent=false;pressedAt=lastRelease=0;clicks=0;}
  Action update(bool isDown){
    unsigned long now=millis();
    if(isDown&&!down){down=true;longSent=false;pressedAt=now;}
    if(isDown&&down&&!longSent&&now-pressedAt>=700){longSent=true;clicks=0;return HOME_HOLD;}
    if(!isDown&&down){
      down=false;
      if(longSent){longSent=false;return NONE;}
      if(clicks==1&&now-lastRelease<=360){clicks=0;lastRelease=0;return HOME_DOUBLE;}
      clicks=1;lastRelease=now;
    }
    if(!isDown&&!down&&clicks==1&&now-lastRelease>360){clicks=0;lastRelease=0;return HOME_SINGLE;}
    return NONE;
  }
};
static HomeGesture HOME_GESTURE;
static bool G_HOME_REQUEST=false;
static bool G_RECENTS_REQUEST=false;
static bool G_CLOSE_REQUEST=false;
static bool G_SETTINGS_REQUEST=false;
static bool G_FILES_REQUEST=false;
static bool G_NOTES_REQUEST=false;
static bool G_CALC_REQUEST=false;
static bool G_MESSAGES_REQUEST=false;
static String G_EDIT_FILE_REQUEST="";
static String G_OPEN_REQUEST="";

// Blocking native screens (Settings, keyboard, lists, etc.) still need to
// service Wi-Fi servers. The hook stays disabled until Kernel::begin() has
// completed enough initialization for the background network services to run.
static bool G_BACKGROUND_READY=false;
static void backgroundServiceHook();

// Native notification banner state. The kernel paints this directly into the
// top bar; MScript notifications no longer open a modal dialog.
static volatile bool G_NOTIFICATION_DIRTY=false;
static String G_NOTIFICATION_TITLE="";
static String G_NOTIFICATION_BODY="";
static unsigned long G_NOTIFICATION_UNTIL=0;

// Microphone capture state. CyberPi's sensor task invokes this callback.
static volatile bool G_MIC_RECORDING=false;
static uint8_t* G_MIC_BUFFER=nullptr;
static volatile size_t G_MIC_LENGTH=0;
static size_t G_MIC_CAPACITY=0;
static const uint32_t G_MIC_RATE=10000;
static volatile bool G_PCM_PLAYING=false;

static void mellorsMicCapture(uint8_t* samples,int len){
  if(!G_MIC_RECORDING||!G_MIC_BUFFER||len<2)return;
  // CyberPi samples at 160 kHz, 16-bit. Downsample 16:1 to compact 10 kHz,
  // unsigned 8-bit mono PCM suitable for speech/short sound effects.
  for(int i=0;i+1<len;i+=32){
    size_t at=G_MIC_LENGTH;if(at>=G_MIC_CAPACITY){G_MIC_RECORDING=false;break;}
    int16_t sample=(int16_t)((uint16_t)samples[i]|((uint16_t)samples[i+1]<<8));
    G_MIC_BUFFER[at]=(uint8_t)constrain(128+(sample>>8),0,255);
    G_MIC_LENGTH=at+1;
  }
}

static void putLE16(File&f,uint16_t v){f.write((uint8_t)(v&255));f.write((uint8_t)(v>>8));}
static void putLE32(File&f,uint32_t v){for(int i=0;i<4;i++)f.write((uint8_t)(v>>(8*i)));}
static bool writeRecordedWav(const String&path,const uint8_t*data,size_t len){
  File f=LittleFS.open(path,"w");if(!f)return false;
  f.write((const uint8_t*)"RIFF",4);putLE32(f,36+(uint32_t)len);f.write((const uint8_t*)"WAVE",4);
  f.write((const uint8_t*)"fmt ",4);putLE32(f,16);putLE16(f,1);putLE16(f,1);putLE32(f,G_MIC_RATE);
  putLE32(f,G_MIC_RATE);putLE16(f,1);putLE16(f,8);f.write((const uint8_t*)"data",4);putLE32(f,(uint32_t)len);
  if(len)f.write(data,len);f.close();return true;
}
static bool recordMicrophoneWav(const String&path,int seconds){
  seconds=constrain(seconds,1,15);size_t cap=(size_t)G_MIC_RATE*seconds;
  uint8_t*buf=psramFound()?(uint8_t*)ps_malloc(cap):(uint8_t*)malloc(cap);if(!buf)return false;
  G_MIC_BUFFER=buf;G_MIC_CAPACITY=cap;G_MIC_LENGTH=0;G_MIC_RECORDING=true;
  cyber.on_microphone_data(mellorsMicCapture);if(G_SENSOR_TASK)vTaskResume(G_SENSOR_TASK);
  unsigned long until=millis()+(unsigned long)seconds*1000UL;
  while(G_MIC_RECORDING&&(long)(until-millis())>0){serviceQuietHardware();delay(10);}
  G_MIC_RECORDING=false;delay(12);if(G_SENSOR_TASK)vTaskSuspend(G_SENSOR_TASK);cyber.on_microphone_data(nullptr);
  size_t got=G_MIC_LENGTH;G_MIC_BUFFER=nullptr;G_MIC_CAPACITY=0;G_MIC_LENGTH=0;
  bool ok=writeRecordedWav(path,buf,got);free(buf);return ok;
}
static double readMicrophoneLoudness(){
  cyber.on_microphone_data(nullptr);if(G_SENSOR_TASK)vTaskResume(G_SENSOR_TASK);delay(45);double v=cyber.get_loudness();if(G_SENSOR_TASK)vTaskSuspend(G_SENSOR_TASK);return v;
}
static uint16_t rdLE16(const uint8_t*b){return (uint16_t)b[0]|((uint16_t)b[1]<<8);}
static uint32_t rdLE32(const uint8_t*b){return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
static bool playPcmWav(const String&path,int volumePercent){
  File f=LittleFS.open(path,"r");if(!f||f.size()<44)return false;uint8_t h[44];if(f.read(h,44)!=44){f.close();return false;}
  if(memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4)||memcmp(h+12,"fmt ",4)||rdLE16(h+20)!=1){f.close();return false;}
  uint16_t channels=rdLE16(h+22),bits=rdLE16(h+34);uint32_t rate=rdLE32(h+24);
  if((channels!=1&&channels!=2)||(bits!=8&&bits!=16)||rate<4000||rate>48000){f.close();return false;}
  // Find the data chunk even when the WAV contains optional metadata chunks.
  f.seek(12);uint32_t dataSize=0;bool found=false;
  while(f.available()>=8){uint8_t ch[8];if(f.read(ch,8)!=8)break;uint32_t sz=rdLE32(ch+4);if(!memcmp(ch,"data",4)){dataSize=sz;found=true;break;}f.seek(f.position()+sz+(sz&1));}
  if(!found){f.close();return false;}volumePercent=constrain(volumePercent,0,100);quietSoundStop();G_PCM_PLAYING=true;
  // WAV playback writes directly to I2S0, so temporarily give it exclusive
  // ownership. The synth task is normally suspended while idle.
  if(G_SOUND_TASK)vTaskSuspend(G_SOUND_TASK);
  i2s_zero_dma_buffer(I2S_NUM_0);
  aw_digitalWrite(AW_P1_3,HIGH);i2s_set_sample_rates(I2S_NUM_0,rate);
  uint16_t out[128];size_t outN=0;uint32_t frameBytes=channels*(bits/8),readBytes=0;
  while(readBytes+frameBytes<=dataSize&&f.available()){
    int sample=0;if(bits==8){int v=f.read();if(v<0)break;sample=(v-128)<<8;readBytes++;for(int c=1;c<channels;c++){f.read();readBytes++;}}
    else{uint8_t b[2];if(f.read(b,2)!=2)break;sample=(int16_t)rdLE16(b);readBytes+=2;for(int c=1;c<channels;c++){f.read(b,2);readBytes+=2;}}
    sample=(sample*volumePercent)/100;uint8_t dac=(uint8_t)constrain(128+(sample>>8),0,255);out[outN++]=(uint16_t)dac<<8;
    if(outN==128){size_t wrote=0;i2s_write(I2S_NUM_0,out,sizeof(out),&wrote,portMAX_DELAY);outN=0;}
  }
  if(outN){size_t wrote=0;i2s_write(I2S_NUM_0,out,outN*sizeof(uint16_t),&wrote,portMAX_DELAY);}
  f.close();i2s_set_sample_rates(I2S_NUM_0,20000);i2s_zero_dma_buffer(I2S_NUM_0);aw_digitalWrite(AW_P1_3,LOW);G_PCM_PLAYING=false;
  // Leave the synth task asleep after PCM playback. The next tone wakes it
  // through quietBeep(), so games do not pay for an idle audio renderer.
  G_SOUND_TASK_AWAKE=false;
  return true;
}

static InputState readInput(){InputState s;s.x=cyber.get_joystick_x();s.y=cyber.get_joystick_y();s.select=cyber.get_joystick_pressed();s.a=cyber.get_button_a();s.b=cyber.get_button_b();s.home=cyber.get_button_menu();return s;}
static Action inputAction(const InputState&n,const InputState&p){
  Action h=HOME_GESTURE.update(n.home);if(h!=NONE)return h;
  if(n.b&&!p.b)return BACK;
  if((n.a&&!p.a)||(n.select&&!p.select))return SELECT;
  if(n.y<0&&p.y==0)return UP;if(n.y>0&&p.y==0)return DOWN;
  if(n.x<0&&p.x==0)return LEFT;if(n.x>0&&p.x==0)return RIGHT;
  return NONE;
}
static void released(){
  unsigned long st=millis();
  while(millis()-st<1000){auto s=readInput();if(!s.x&&!s.y&&!s.select&&!s.a&&!s.b&&!s.home){HOME_GESTURE.reset();return;}backgroundServiceHook();delay(8);}
  HOME_GESTURE.reset();
}


static bool prepareFirmwareInstall(){
  // Build changes are updates, not factory resets. Keep files, NVS and Wi-Fi.
  // Do not auto-format an unmountable filesystem during an update.
  Preferences meta;meta.begin("fwmeta",false);
  if(!LittleFS.begin(false)){
    if(meta.getString("build","").length()||!LittleFS.begin(true)){
      Serial.println("[MellorsOS] Filesystem unavailable; refusing to erase existing data");
      while(true)delay(1000);
    }
  }
  bool changed=meta.getString("build","")!=FIRMWARE_BUILD;
  meta.putString("build",FIRMWARE_BUILD);meta.end();return changed;
}

class Value{
public:
  enum Type{NIL,NUMBER,BOOLEAN,STRING}; Type type=NIL; double number=0; bool boolean=false; String string;
  Value(){} Value(double v):type(NUMBER),number(v){} Value(int v):type(NUMBER),number(v){} Value(bool v):type(BOOLEAN),boolean(v){} Value(const String&v):type(STRING),string(v){} Value(const char*v):type(STRING),string(v?v:""){}
  String str()const{if(type==STRING)return string;if(type==BOOLEAN)return boolean?"true":"false";if(type==NUMBER){if(fabs(number-round(number))<0.000001)return String((long)round(number));return String(number,4);}return "";}
  double num()const{if(type==NUMBER)return number;if(type==BOOLEAN)return boolean?1:0;if(type==STRING)return string.toDouble();return 0;}
  bool truth()const{if(type==BOOLEAN)return boolean;if(type==NUMBER)return number!=0;if(type==STRING)return string.length()>0&&string!="false"&&string!="0";return false;}
  String typeName()const{if(type==NUMBER)return "number";if(type==BOOLEAN)return "boolean";if(type==STRING)return "string";return "null";}
};

struct Var{String name;Value value;bool used=false;};
class Vars{public:Var a[MAX_VARS];void clear(){for(auto &v:a){v.used=false;v.name="";v.value=Value();}}Value get(const String&n)const{for(const auto&v:a)if(v.used&&v.name==n)return v.value;return Value();}void set(const String&n,const Value&x){for(auto&v:a)if(v.used&&v.name==n){v.value=x;return;}for(auto&v:a)if(!v.used){v.used=true;v.name=n;v.value=x;return;}}};
class MemoryStore{public:Var a[64];void clear(){for(auto &v:a){v.used=false;v.name="";v.value=Value();}}Value get(const String&n)const{for(const auto&v:a)if(v.used&&v.name==n)return v.value;return Value();}void set(const String&n,const Value&x){for(auto&v:a)if(v.used&&v.name==n){v.value=x;return;}for(auto&v:a)if(!v.used){v.used=true;v.name=n;v.value=x;return;}}};

class SettingsStore{
  Preferences p;
public:
  void begin(){p.begin("mellors",false);if(!p.isKey("init")){reset();p.putBool("init",true);}p.remove("btEnabled");p.remove("btName");String ca=canonicalAccent(accent());if(ca!=accent())accent(ca);}
  bool bootedBefore(){return p.getBool("booted",false);} void markBooted(){p.putBool("booted",true);}
  String accent(){return p.getString("accent","#4F46E5");} void accent(const String&v){p.putString("accent",v);}
  String accent2(){return p.getString("accent2","#a96cff");} void accent2(const String&v){p.putString("accent2",v);}
  String wallpaper(){return p.getString("wallpaper","aurora");} void wallpaper(const String&v){p.putString("wallpaper",v);}
  String wallpaperFile(){return p.getString("wallFile","");} void wallpaperFile(const String&v){p.putString("wallFile",v);}
  int gridCols(){return p.getInt("gridCols",2);} void gridCols(int v){p.putInt("gridCols",constrain(v,1,3));}
  int iconSize(){return p.getInt("iconSize",32);} void iconSize(int v){p.putInt("iconSize",constrain(v,16,32));}
  String iconShape(){String v=p.getString("iconShape","squircle");return (v=="square"||v=="squircle"||v=="round"||v=="circle")?v:String("squircle");} void iconShape(const String&v){if(v=="square"||v=="squircle"||v=="round"||v=="circle")p.putString("iconShape",v);}
  bool showAppLabels(){return p.getBool("appLabels",true);} void showAppLabels(bool v){p.putBool("appLabels",v);}
  String homeTitle(){return p.getString("homeTitle","MellorsOS");} void homeTitle(const String&v){p.putString("homeTitle",v);}
  String homeSubtitle(){return p.getString("homeSub","Programmable by design");} void homeSubtitle(const String&v){p.putString("homeSub",v);}
  String username(){return p.getString("username","Owner");} void username(const String&v){p.putString("username",v);}
  float fontScale(){return p.getFloat("fontScale",1.0f);} void fontScale(float v){p.putFloat("fontScale",constrain(v,.85f,1.45f));}
  bool compact(){return p.getBool("compact",false);} void compact(bool v){p.putBool("compact",v);}
  bool highContrast(){return p.getBool("hiContrast",false);} void highContrast(bool v){p.putBool("hiContrast",v);}
  bool reduceMotion(){return p.getBool("redMotion",false);} void reduceMotion(bool v){p.putBool("redMotion",v);}
  bool reduceTransparency(){return p.getBool("redTrans",false);} void reduceTransparency(bool v){p.putBool("redTrans",v);}
  bool largeTargets(){return p.getBool("largeTarget",false);} void largeTargets(bool v){p.putBool("largeTarget",v);}
  bool boldText(){return p.getBool("boldText",false);} void boldText(bool v){p.putBool("boldText",v);}
  bool haptics(){return p.getBool("haptics",true);} void haptics(bool v){p.putBool("haptics",v);}
  bool showSeconds(){return p.getBool("seconds",false);} void showSeconds(bool v){p.putBool("seconds",v);}
  bool clock24h(){return p.getBool("clock24",false);} void clock24h(bool v){p.putBool("clock24",v);}
  bool showBattery(){return p.getBool("battery",true);} void showBattery(bool v){p.putBool("battery",v);}
  bool showStatusIcons(){return p.getBool("statusIcons",true);} void showStatusIcons(bool v){p.putBool("statusIcons",v);}
  bool navCompact(){return p.getBool("navCompact",false);} void navCompact(bool v){p.putBool("navCompact",v);}
  bool notificationToasts(){return p.getBool("notifToast",true);} void notificationToasts(bool v){p.putBool("notifToast",v);}
  bool notificationPreview(){return p.getBool("notifPrev",true);} void notificationPreview(bool v){p.putBool("notifPrev",v);}
  int notificationLimit(){return p.getInt("notifLimit",50);} void notificationLimit(int v){p.putInt("notifLimit",constrain(v,5,100));}
  bool developerMode(){return p.getBool("devMode",false);} void developerMode(bool v){p.putBool("devMode",v);}
  bool showFPS(){return p.getBool("showFPS",false);} void showFPS(bool v){p.putBool("showFPS",v);}
  bool wifiEnabled(){return p.getBool("wifiEnabled",true);} void wifiEnabled(bool v){p.putBool("wifiEnabled",v);}
  bool silentMode(){return p.getBool("silent",false);} void silentMode(bool v){p.putBool("silent",v);}
  int volume(){return p.getInt("volume",70);} void volume(int v){p.putInt("volume",constrain(v,0,100));}
  bool pinEnabled(){return p.getBool("pinEnabled",false)&&p.getString("pinHash","").length();}
  void setPin(const String&pin){if(pin.length()==4){p.putString("pinHash",pinDigest(pin));p.putBool("pinEnabled",true);}}
  bool verifyPin(const String&pin){return pinEnabled()&&p.getString("pinHash","")==pinDigest(pin);}
  void clearPin(){p.putBool("pinEnabled",false);p.remove("pinHash");}
  String lastWifi(){return p.getString("lastWifi","");} void lastWifi(const String&v){p.putString("lastWifi",v);}
  String wifiHostname(){return p.getString("wifiHost","");} void wifiHostname(const String&v){p.putString("wifiHost",safeHostname(v));}
  String setupVersion(){return p.getString("setupVer","");} void setupVersion(const String&v){p.putString("setupVer",v);}
  String hiddenApps(){return p.getString("hidden","");} void hiddenApps(const String&v){p.putString("hidden",v);}
  String homeOrder(){return p.getString("homeOrder","");} void homeOrder(const String&v){p.putString("homeOrder",v);}
  String clipboard(){return p.getString("clipboard","");} void clipboard(const String&v){p.putString("clipboard",v);}
  void reset(){
    p.putString("accent","#4F46E5");p.putString("accent2","#8B5CF6");p.putString("wallpaper","aurora");p.putString("wallFile","");p.putInt("gridCols",2);p.putInt("iconSize",32);p.putString("iconShape","squircle");p.putBool("appLabels",true);p.putString("homeTitle","MellorsOS");p.putString("homeSub","Programmable by design");p.putString("username","Owner");p.putFloat("fontScale",1.0f);p.putBool("compact",false);p.putBool("hiContrast",false);p.putBool("redMotion",false);p.putBool("redTrans",false);p.putBool("largeTarget",false);p.putBool("boldText",false);p.putBool("haptics",true);p.putBool("seconds",false);p.putBool("clock24",false);p.putBool("battery",true);p.putBool("statusIcons",true);p.putBool("navCompact",false);p.putBool("notifToast",true);p.putBool("notifPrev",true);p.putInt("notifLimit",50);p.putBool("devMode",false);p.putBool("showFPS",false);p.putBool("wifiEnabled",true);p.putBool("silent",false);p.putInt("volume",70);p.putBool("pinEnabled",false);p.remove("pinHash");p.putString("wifiHost","");p.putString("setupVer","");p.putBool("booted",false);
  }
  Value get(const String&k,Value fallback=Value("")){
    if(k=="accent")return accent();if(k=="accent2")return accent2();if(k=="wallpaper")return wallpaper();if(k=="wallpaperFile")return wallpaperFile();if(k=="gridCols")return gridCols();if(k=="iconSize")return iconSize();if(k=="iconShape")return iconShape();if(k=="showAppLabels")return showAppLabels();if(k=="homeTitle")return homeTitle();if(k=="homeSubtitle")return homeSubtitle();if(k=="username")return username();if(k=="fontScale")return (double)fontScale();if(k=="compact")return compact();if(k=="highContrast")return highContrast();if(k=="reduceMotion")return reduceMotion();if(k=="reduceTransparency")return reduceTransparency();if(k=="largeTargets")return largeTargets();if(k=="boldText")return boldText();if(k=="haptics")return haptics();if(k=="showSeconds")return showSeconds();if(k=="clock24h")return clock24h();if(k=="showBattery")return showBattery();if(k=="showStatusIcons")return showStatusIcons();if(k=="navCompact")return navCompact();if(k=="notificationToasts")return notificationToasts();if(k=="notificationPreview")return notificationPreview();if(k=="notificationLimit")return notificationLimit();if(k=="developerMode")return developerMode();if(k=="showFPS")return showFPS();if(k=="wifiHostname")return wifiHostname();if(k=="wifiEnabled")return wifiEnabled();if(k=="silentMode")return silentMode();if(k=="volume")return volume();if(k=="pinEnabled")return pinEnabled();return fallback;
  }
  bool has(const String&k){String ks=keys();return ("|"+ks+"|").indexOf("|"+k+"|")>=0;}
  String keys(){return "accent|accent2|wallpaper|wallpaperFile|gridCols|iconSize|iconShape|showAppLabels|homeTitle|homeSubtitle|username|fontScale|compact|highContrast|reduceMotion|reduceTransparency|largeTargets|boldText|haptics|showSeconds|clock24h|showBattery|showStatusIcons|navCompact|notificationToasts|notificationPreview|notificationLimit|developerMode|showFPS|wifiHostname|wifiEnabled|silentMode|volume|pinEnabled";}
  void set(const String&k,const Value&v){if(k=="accent")accent(v.str());else if(k=="accent2")accent2(v.str());else if(k=="wallpaper")wallpaper(v.str());else if(k=="wallpaperFile")wallpaperFile(v.str());else if(k=="gridCols")gridCols(v.num());else if(k=="iconSize")iconSize(v.num());else if(k=="iconShape")iconShape(v.str());else if(k=="showAppLabels")showAppLabels(v.truth());else if(k=="homeTitle")homeTitle(v.str());else if(k=="homeSubtitle")homeSubtitle(v.str());else if(k=="username")username(v.str());else if(k=="fontScale")fontScale(v.num());else if(k=="compact")compact(v.truth());else if(k=="highContrast")highContrast(v.truth());else if(k=="reduceMotion")reduceMotion(v.truth());else if(k=="reduceTransparency")reduceTransparency(v.truth());else if(k=="largeTargets")largeTargets(v.truth());else if(k=="boldText")boldText(v.truth());else if(k=="haptics")haptics(v.truth());else if(k=="showSeconds")showSeconds(v.truth());else if(k=="clock24h")clock24h(v.truth());else if(k=="showBattery")showBattery(v.truth());else if(k=="showStatusIcons")showStatusIcons(v.truth());else if(k=="navCompact")navCompact(v.truth());else if(k=="notificationToasts")notificationToasts(v.truth());else if(k=="notificationPreview")notificationPreview(v.truth());else if(k=="notificationLimit")notificationLimit(v.num());else if(k=="developerMode")developerMode(v.truth());else if(k=="showFPS")showFPS(v.truth());else if(k=="wifiHostname")wifiHostname(v.str());else if(k=="wifiEnabled")wifiEnabled(v.truth());else if(k=="silentMode")silentMode(v.truth());else if(k=="volume")volume(v.num());}
};

class Display{
  SettingsStore&st; unsigned long last=0;int clipTop=0,clipBottom=128;
public:
  Display(SettingsStore&s):st(s){}
  uint16_t c565(uint32_t c){return cyber.swap_color(cyber.color24_to_16(c));}
  uint32_t accent(){return rgb(st.accent());}
  uint32_t bg(){if(st.highContrast())return 0x000000;String w=st.wallpaper();if(w=="ocean")return 0x0B2A3A;if(w=="sunset")return 0x3C1E2D;if(w=="forest")return 0x102B24;if(w=="mono")return 0x101218;return 0x0C1020;}
  uint32_t panel(){return st.highContrast()?0x000000:0x161D31;} uint32_t fg(){return 0xFFFFFF;} uint32_t muted(){return st.highContrast()?0xFFFFFF:0xA9B4C6;}
  uint32_t contrast(uint32_t c){int r=(c>>16)&255,g=(c>>8)&255,b=c&255;return (r*299+g*587+b*114)/1000>145?0x000000:0xFFFFFF;}
  void setClip(int top,int bottom){clipTop=constrain(top,0,128);clipBottom=constrain(bottom,clipTop,128);}
  void resetClip(){clipTop=0;clipBottom=128;}
  void pixel(int x,int y,uint32_t c){if(x<0||y<clipTop||x>=128||y>=clipBottom)return;cyber.set_lcd_pixel(x,y,c565(c));}
  void pixel565(int x,int y,uint16_t c){if(x<0||y<clipTop||x>=128||y>=clipBottom)return;cyber.set_lcd_pixel(x,y,c);}
  void fill(int x,int y,int w,int h,uint32_t c){int x0=max(0,x),y0=max(clipTop,y),x1=min(128,x+w),y1=min(clipBottom,y+h);if(x1<=x0||y1<=y0)return;uint16_t cc=c565(c);for(int yy=y0;yy<y1;yy++)for(int xx=x0;xx<x1;xx++)cyber.set_lcd_pixel(xx,yy,cc);}
  void outline(int x,int y,int w,int h,uint32_t c){fill(x,y,w,1,c);fill(x,y+h-1,w,1,c);fill(x,y,1,h,c);fill(x+w-1,y,1,h,c);}
  void clear(){
    resetClip();fill(0,0,128,128,bg());if(st.highContrast())return;
    String w=st.wallpaper();
    if(w=="aurora"){for(int y=20;y<128;y+=28){fill(0,y,128,2,0x222451);for(int x=(y/2)%24;x<128;x+=36)fill(x,y-7,22,2,0x173A52);}}
    else if(w=="ocean"){for(int y=22;y<128;y+=22){for(int x=0;x<128;x+=16)fill(x,y+((x/16)&1?2:0),12,1,0x13516A);}}
    else if(w=="sunset"){fill(0,82,128,46,0x271526);fill(0,68,128,2,0x73304B);fill(92,30,18,18,0x8A4B52);}
    else if(w=="forest"){for(int x=7;x<128;x+=21){fill(x,18,2,110,0x174737);fill(x-4,22,10,1,0x255B45);}}
    else if(w=="mono"){for(int y=0;y<128;y+=16)fill(0,y,128,1,0x1D2028);for(int x=0;x<128;x+=16)fill(x,0,1,128,0x1D2028);}
  }
  int scale(){return st.fontScale()>=1.35f?2:1;}
  int charW(int sc=0){if(!sc)sc=scale();return 6*sc;} int lineH(int sc=0){if(!sc)sc=scale();return 10*sc;}
  void glyph(int x,int y,char ch,uint32_t color,int sc=1,bool bold=false){
    if(ch<32||ch>126)ch='?';const uint8_t* g=FONT5X10[ch-32];
    for(int yy=0;yy<10;yy++){uint8_t m=g[yy];for(int xx=0;xx<5;xx++)if(m&(1<<xx)){
      for(int sy=0;sy<sc;sy++)for(int sx=0;sx<sc;sx++)pixel(x+xx*sc+sx,y+yy*sc+sy,color);
      if(bold&&sc==1)pixel(x+xx+1,y+yy,color);
    }}
  }
  String wrapText(String s,int width,int sc=0){
    if(!sc)sc=scale();
    int maxChars=max(2,width/charW(sc));
    String out="",line="",word="";
    auto flushLine=[&](){if(out.length())out+='\n';out+=line;line="";};
    auto placeWord=[&](String w){
      while((int)w.length()>maxChars){
        if(line.length())flushLine();
        int take=max(1,maxChars-1);
        line=w.substring(0,take)+"-";flushLine();
        w=w.substring(take);
      }
      if(!w.length())return;
      if(!line.length())line=w;
      else if((int)(line.length()+1+w.length())<=maxChars)line+=" "+w;
      else{flushLine();line=w;}
    };
    for(size_t i=0;i<=s.length();i++){
      char c=i<s.length()?s[i]:'\0';
      if(c=='\r')continue;
      if(c=='\n'){
        if(word.length()){placeWord(word);word="";}
        flushLine();
      }else if(c==' '||c=='\t'||c=='\0'){
        if(word.length()){placeWord(word);word="";}
        if(c=='\0')break;
      }else word+=c;
    }
    if(line.length()||!out.length()){if(out.length())out+='\n';out+=line;}
    return out;
  }
  int wrappedLines(const String&s,int width,int sc=0){String w=wrapText(s,width,sc);int lines=1;for(size_t i=0;i<w.length();i++)if(w[i]=='\n')lines++;return lines;}
  int textHeight(const String&s,int width,int sc=0){return wrappedLines(s,width,sc)*lineH(sc);}
  void text(int x,int y,String s,uint32_t color=0xFFFFFF,int sc=0,bool wrap=false,int maxW=0){
    if(!sc)sc=scale();if(maxW<=0)maxW=128-x;String draw=wrap?wrapText(s,maxW,sc):s;int ox=x;
    for(size_t i=0;i<draw.length();++i){char ch=draw[i];if(ch=='\n'){x=ox;y+=lineH(sc);continue;}glyph(x,y,ch,color,sc,st.boldText());x+=charW(sc);}
  }
  void header(String title,int h=18){
    resetClip();h=max(18,min(60,h));uint32_t a=accent();
    if(st.highContrast()){fill(0,0,128,h,0x000000);outline(0,0,128,h,0xFFFFFF);text(4,4,title,0xFFFFFF,1,true,120);}
    else{fill(0,0,128,h,a);text(4,4,title,contrast(a),1,true,120);}
  }
  void icon(int x,int y,const uint16_t data[256],int scale=1,const String&shape="squircle"){
    for(int py=0;py<16;py++)for(int px=0;px<16;px++){bool draw=true;
      if(shape=="circle"){float dx=px-7.5f,dy=py-7.5f;draw=dx*dx+dy*dy<=58;}
      else if(shape=="round"||shape=="squircle"){if((px<2&&py<2)||(px>13&&py<2)||(px<2&&py>13)||(px>13&&py>13))draw=false;}
      if(draw)for(int sy=0;sy<scale;sy++)for(int sx=0;sx<scale;sx++)pixel565(x+px*scale+sx,y+py*scale+sy,data[py*16+px]);
    }
  }
  void render(){unsigned long n=millis();if(n-last<22)delay(22-(n-last));cyber.render_lcd();last=millis();}
};

class Modal{
  Display&d;
public:Modal(Display&x):d(x){}
  void msg(String title,String body,String detail=""){
    released();d.clear();int hh=max(18,6+d.wrappedLines(title,120,1)*d.lineH(1));d.header(title,hh);
    d.setClip(hh,128);int y=hh+5;d.text(5,y,body,d.fg(),1,true,118);y+=d.textHeight(body,118,1)+4;
    if(detail.length()){d.text(5,y,detail,d.muted(),1,true,118);}
    d.resetClip();d.text(5,117,"A OK",d.muted(),1);d.render();
    InputState p;while(1){auto n=readInput();Action a=inputAction(n,p);p=n;
      if(a==SELECT||a==BACK||a==HOME_SINGLE)break;
      if(a==HOME_DOUBLE){G_RECENTS_REQUEST=true;break;}
      if(a==HOME_HOLD){G_HOME_REQUEST=true;break;}
      backgroundServiceHook();delay(8);
    }released();
  }
  bool confirm(String title,String body,String detail="",String yes="Allow",String no="Deny"){
    released();d.clear();int hh=max(18,6+d.wrappedLines(title,120,1)*d.lineH(1));d.header(title,hh);
    d.setClip(hh,128);int y=hh+5;d.text(5,y,body,d.fg(),1,true,118);y+=d.textHeight(body,118,1)+3;
    if(detail.length())d.text(5,y,detail,d.muted(),1,true,118);
    d.resetClip();d.text(5,106,"A "+yes,d.fg(),1,true,118);d.text(5,118,"B "+no,d.muted(),1,true,118);d.render();
    InputState p;while(1){auto n=readInput();Action a=inputAction(n,p);p=n;
      if(a==SELECT){released();return true;}
      if(a==BACK||a==HOME_SINGLE){released();return false;}
      if(a==HOME_DOUBLE){G_RECENTS_REQUEST=true;released();return false;}
      if(a==HOME_HOLD){G_HOME_REQUEST=true;released();return false;}
      backgroundServiceHook();delay(8);
    }
  }
};

// MScript drawing uses the physical 128x128 LCD one-to-one, with origin (0,0)
// at top-left and bottom-right at (127,127). Use lambda constants here so
// Arduino's .ino auto-prototyper cannot insert prototypes before Display exists.
static const auto canvasPixel = [](Display&d,int x,int y,uint32_t c)->void {
  if(x>=0&&x<SCREEN_W&&y>=0&&y<SCREEN_H)d.pixel(x,y,c);
};
static const auto canvasLine = [](Display&d,int x0,int y0,int x1,int y1,uint32_t c)->void {
  // Cap path length to bound CPU usage even for wildly out-of-range arguments.
  x0=constrain(x0,-256,384);y0=constrain(y0,-256,384);
  x1=constrain(x1,-256,384);y1=constrain(y1,-256,384);
  int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
  for(int i=0;i<1300;i++){
    canvasPixel(d,x0,y0,c);
    if(x0==x1&&y0==y1)break;
    int e2=2*err;
    if(e2>=dy){err+=dy;x0+=sx;}
    if(e2<=dx){err+=dx;y0+=sy;}
  }
};
static const auto canvasRect = [](Display&d,int x,int y,int w,int h,uint32_t c,bool filled)->void {
  if(w<=0||h<=0)return;
  if(filled)d.fill(x,y,w,h,c);
  else{
    canvasLine(d,x,y,x+w-1,y,c);
    canvasLine(d,x,y+h-1,x+w-1,y+h-1,c);
    canvasLine(d,x,y,x,y+h-1,c);
    canvasLine(d,x+w-1,y,x+w-1,y+h-1,c);
  }
};
static const auto canvasCircle = [](Display&d,int cx,int cy,int r,uint32_t c,bool filled)->void {
  r=constrain(r,0,128);
  int x=r,y=0,err=1-r;
  while(x>=y){
    if(filled){
      canvasLine(d,cx-x,cy+y,cx+x,cy+y,c);
      canvasLine(d,cx-x,cy-y,cx+x,cy-y,c);
      canvasLine(d,cx-y,cy+x,cx+y,cy+x,c);
      canvasLine(d,cx-y,cy-x,cx+y,cy-x,c);
    } else {
      canvasPixel(d,cx+x,cy+y,c);canvasPixel(d,cx-x,cy+y,c);
      canvasPixel(d,cx+x,cy-y,c);canvasPixel(d,cx-x,cy-y,c);
      canvasPixel(d,cx+y,cy+x,c);canvasPixel(d,cx-y,cy+x,c);
      canvasPixel(d,cx+y,cy-x,c);canvasPixel(d,cx-y,cy-x,c);
    }
    y++;
    if(err<0)err+=2*y+1;
    else{x--;err+=2*(y-x+1);}
  }
};
static const auto canvasTriangle = [](Display&d,int x1,int y1,int x2,int y2,int x3,int y3,uint32_t c,bool filled)->void {
  if(!filled){
    canvasLine(d,x1,y1,x2,y2,c);canvasLine(d,x2,y2,x3,y3,c);canvasLine(d,x3,y3,x1,y1,c);
    return;
  }
  int miny=max(0,min(y1,min(y2,y3))),maxy=min(SCREEN_H-1,max(y1,max(y2,y3)));
  for(int y=miny;y<=maxy;y++){
    int xs[3],n=0;int px[]={x1,x2,x3},py[]={y1,y2,y3};
    for(int i=0;i<3;i++){
      int j=(i+1)%3,a=py[i],b=py[j];
      if(a==b)continue;
      if(y>=min(a,b)&&y<max(a,b))xs[n++]=px[i]+(int)((long)(y-a)*(px[j]-px[i])/(b-a));
    }
    if(n>=2){
      if(xs[0]>xs[1]){int t=xs[0];xs[0]=xs[1];xs[1]=t;}
      canvasLine(d,xs[0],y,xs[1],y,c);
    }
  }
  // Draw the boundary as well (including the triangle's bottom row).
  canvasLine(d,x1,y1,x2,y2,c);canvasLine(d,x2,y2,x3,y3,c);canvasLine(d,x3,y3,x1,y1,c);
};


class Keyboard{
  Display&d;
  const char* rows(bool symbols,int page,bool upper,int row){
    if(!symbols){
      if(!upper){if(row==0)return"qwertyuiop";if(row==1)return"asdfghjkl";if(row==2)return"zxcvbnm.";}
      else{if(row==0)return"QWERTYUIOP";if(row==1)return"ASDFGHJKL";if(row==2)return"ZXCVBNM.";}
    }else if(page==0){
      if(row==0)return"1234567890";
      if(row==1)return"!@#$%^&*()";
      if(row==2)return".,?;:'\"/\\|";
    }else{
      if(row==0)return"-_=+[]{}<>";
      if(row==1)return"`~@#$%^&*?";
      if(row==2)return"()_+{}[]=:";
    }
    return"";
  }
  int count(bool symbols,int page,bool upper,int r){return r==3?6:(r<0?1:(int)strlen(rows(symbols,page,upper,r)));}
  void clampCol(bool symbols,int page,bool upper,int row,int&col){int n=count(symbols,page,upper,row);if(n<1){col=0;return;}if(col>=n)col=n-1;if(col<0)col=0;}
  void lineInfo(const String&v,int cursor,int&line,int&lines,int&lineStart,int&lineEnd,int&column){
    line=1;lines=1;lineStart=0;cursor=constrain(cursor,0,(int)v.length());
    for(int i=0;i<(int)v.length();i++){if(v[i]=='\n'){lines++;if(i<cursor){line++;lineStart=i+1;}}}
    lineEnd=v.indexOf('\n',lineStart);if(lineEnd<0)lineEnd=v.length();column=cursor-lineStart;
  }
  String insertAt(String v,int&cursor,char ch,int maxLen){if((int)v.length()>=maxLen)return v;cursor=constrain(cursor,0,(int)v.length());v=v.substring(0,cursor)+String(ch)+v.substring(cursor);cursor++;return v;}
  void backspaceAt(String&v,int&cursor){cursor=constrain(cursor,0,(int)v.length());if(cursor<=0)return;v.remove(cursor-1,1);cursor--;}
  void draw(String prompt,String value,bool password,bool symbols,int page,bool upper,bool caps,int row,int col,int cursorPos,bool cursorOn){
    d.clear();int line=1,lines=1,lineStart=0,lineEnd=0,column=0;lineInfo(value,cursorPos,line,lines,lineStart,lineEnd,column);
    String top=prompt;if(lines>1)top+="  L"+String(line)+"/"+String(lines);d.text(2,2,top,d.fg(),1,true,124);
    bool editing=row<0;d.fill(2,13,124,13,d.panel());if(editing)d.outline(2,13,124,13,d.accent());
    String logical=value.substring(lineStart,lineEnd);if(password){String stars="";for(size_t i=0;i<logical.length();i++)stars+='*';logical=stars;}
    int localCursor=constrain(column,0,(int)logical.length());
    const int maxChars=18;int windowStart=max(0,localCursor-maxChars/2);if(windowStart+maxChars>(int)logical.length())windowStart=max(0,(int)logical.length()-maxChars);
    String shown=logical.substring(windowStart,min((int)logical.length(),windowStart+maxChars));int caret=constrain(localCursor-windowStart,0,(int)shown.length());
    if(cursorOn)shown=shown.substring(0,caret)+"|"+shown.substring(caret);else shown=shown.substring(0,caret)+" "+shown.substring(caret);
    d.text(5,15,shown,d.fg(),1,true,118);
    for(int r=0;r<3;r++){const char*keys=rows(symbols,page,upper,r);int n=strlen(keys);int cw=n?min(12,122/n):11;int total=n*cw;int x0=(128-total)/2;int y=31+r*22;
      for(int i=0;i<n;i++){bool sel=r==row&&i==col;d.fill(x0+i*cw,y,cw-1,18,sel?d.accent():d.panel());String c="";c+=keys[i];d.text(x0+i*cw+2,y+4,c,sel?d.contrast(d.accent()):d.fg(),1);}
    }
    String labels[6];labels[0]=symbols?(page==0?"More":"Less"):"Shift";labels[1]=caps?"CAP":"Caps";labels[2]="space";labels[3]="NL";labels[4]="<-";labels[5]="OK";
    int xs[6]={1,22,43,75,92,109},ws[6]={20,20,30,16,16,18};
    for(int i=0;i<6;i++){bool sel=row==3&&col==i;d.fill(xs[i],99,ws[i],25,sel?d.accent():d.panel());d.text(xs[i]+2,107,labels[i],sel?d.contrast(d.accent()):d.fg(),1);}
    if(editing)d.text(73,2,"L/R cursor",d.muted(),1,true,53);
    d.render();
  }
public:Keyboard(Display&x):d(x){}
  String prompt(String label,String initial,bool password=false,int maxLen=96,bool*cancel=nullptr){
    if(cancel)*cancel=false;String v=initial;int cursorPos=v.length();bool symbols=false,caps=false,shift=false;int page=0,row=0,col=0;released();InputState prev;bool dirty=true;bool lastCursor=false;
    while(1){
      bool cursor=((millis()/450)%2)!=0;bool upper=caps||shift;
      if(dirty||cursor!=lastCursor){draw(label,v,password,symbols,page,upper,caps,row,col,cursorPos,cursor);dirty=false;lastCursor=cursor;}
      auto n=readInput();Action h=HOME_GESTURE.update(n.home);
      if(h==HOME_SINGLE){if(cancel)*cancel=true;released();return initial;}
      if(h==HOME_DOUBLE){G_RECENTS_REQUEST=true;if(cancel)*cancel=true;released();return initial;}
      if(h==HOME_HOLD){G_HOME_REQUEST=true;if(cancel)*cancel=true;released();return initial;}
      if(row<0){
        if(n.x<0&&prev.x==0){if(cursorPos>0)cursorPos--;dirty=true;}
        else if(n.x>0&&prev.x==0){if(cursorPos<(int)v.length())cursorPos++;dirty=true;}
        else if(n.y>0&&prev.y==0){row=0;col=0;dirty=true;}
        else if(n.a&&!prev.a){row=0;col=0;dirty=true;}
      }else if(n.select&&!prev.select){symbols=!symbols;page=0;row=0;col=0;dirty=true;}
      else if(n.b&&!prev.b){if(!symbols){shift=!shift;dirty=true;}}
      else if(n.x<0&&prev.x==0){col--;if(col<0)col=count(symbols,page,caps||shift,row)-1;dirty=true;}
      else if(n.x>0&&prev.x==0){col++;if(col>=count(symbols,page,caps||shift,row))col=0;dirty=true;}
      else if(n.y<0&&prev.y==0){if(row==0){row=-1;col=0;}else{row--;clampCol(symbols,page,caps||shift,row,col);}dirty=true;}
      else if(n.y>0&&prev.y==0){row++;if(row>3)row=0;clampCol(symbols,page,caps||shift,row,col);dirty=true;}
      else if(n.a&&!prev.a){
        bool upperNow=caps||shift;
        if(row<3){
          const char*keys=rows(symbols,page,upperNow,row);
          if(col<(int)strlen(keys)&&(int)v.length()<maxLen){char ch=keys[col];v=insertAt(v,cursorPos,ch,maxLen);if(!symbols&&shift&&!caps&&isalpha((unsigned char)ch))shift=false;dirty=true;}
        }else if(col==0){if(symbols)page=1-page;else shift=!shift;dirty=true;}
        else if(col==1){caps=!caps;shift=false;dirty=true;}
        else if(col==2){v=insertAt(v,cursorPos,' ',maxLen);dirty=true;}
        else if(col==3){v=insertAt(v,cursorPos,'\n',maxLen);dirty=true;}
        else if(col==4){backspaceAt(v,cursorPos);dirty=true;}
        else{released();return v;}
      }
      prev=n;backgroundServiceHook();delay(12);
    }
  }
};

class PinPad{
  Display&d;SettingsStore&s;
  void draw(String title,const String&digits,int sel,bool error=false){
    d.clear();d.header(title,18);d.text(8,25,error?"Incorrect PIN":"Enter 4 digits",error?0xFF8A8A:d.muted(),1);
    String dots="";for(size_t i=0;i<digits.length();i++)dots+="* ";for(size_t i=digits.length();i<4;i++)dots+="_ ";d.text(43,39,dots,d.fg(),1);
    static const char*keys[12]={"1","2","3","4","5","6","7","8","9","<-","0",""};
    for(int i=0;i<12;i++){int r=i/3,c=i%3,x=14+c*34,y=54+r*18;bool on=i==sel;d.fill(x,y,30,15,on?d.accent():d.panel());if(i!=11)d.text(x+(i==9?7:12),y+3,keys[i],on?d.contrast(d.accent()):d.fg(),1);}
    d.text(5,119,"A select",d.muted(),1);d.render();
  }
  String readFour(String title,bool allowCancel,bool*cancel=nullptr){
    if(cancel)*cancel=false;String pin="";int sel=0;released();InputState prev;draw(title,pin,sel);
    while(true){auto n=readInput();Action h=HOME_GESTURE.update(n.home);
      if(allowCancel&&h==HOME_SINGLE){if(cancel)*cancel=true;released();return "";}
      if(n.x<0&&prev.x==0){sel=(sel+11)%12;draw(title,pin,sel);}else if(n.x>0&&prev.x==0){sel=(sel+1)%12;draw(title,pin,sel);}
      else if(n.y<0&&prev.y==0){sel=(sel+9)%12;draw(title,pin,sel);}else if(n.y>0&&prev.y==0){sel=(sel+3)%12;draw(title,pin,sel);}
      else if(n.b&&!prev.b){if(pin.length())pin.remove(pin.length()-1);else if(allowCancel){if(cancel)*cancel=true;released();return "";}draw(title,pin,sel);}
      else if((n.a&&!prev.a)||(n.select&&!prev.select)){if(sel==9){if(pin.length())pin.remove(pin.length()-1);}else if(sel!=11&&pin.length()<4){char digit=(sel==10)?'0':char('1'+sel);pin+=digit;}draw(title,pin,sel);if(pin.length()==4){released();return pin;}}
      prev=n;backgroundServiceHook();delay(12);
    }
  }
public:
  PinPad(Display&dd,SettingsStore&ss):d(dd),s(ss){}
  bool configure(bool allowCancel=true){
    while(true){bool c=false;String first=readFour("Set lock PIN",allowCancel,&c);if(c)return false;String second=readFour("Confirm PIN",allowCancel,&c);if(c)return false;if(first==second){s.setPin(first);return true;}draw("PINs did not match","",0,true);delay(850);}
  }
  bool verifyCurrent(bool allowCancel=true){bool c=false;String v=readFour("Current PIN",allowCancel,&c);return !c&&s.verifyPin(v);}
  void unlock(){
    if(!s.pinEnabled())return;bool bad=false;while(true){String v=readFour("MellorsOS locked",false,nullptr);if(s.verifyPin(v)){released();return;}draw("MellorsOS locked","",0,true);bad=true;delay(900);(void)bad;}
  }
};


static volatile bool G_PEER_PENDING=false;
static char G_PEER_MESSAGE[384]={0};
static char G_PEER_SENDER[64]={0};

// Native Messages consumes the ingress queue immediately so port 81 can keep
// accepting messages even while a blocking system screen is open. MScript gets
// a separate one-message event queue on the next VM tick.
static volatile bool G_SCRIPT_PEER_PENDING=false;
static char G_SCRIPT_PEER_MESSAGE[384]={0};
static char G_SCRIPT_PEER_SENDER[64]={0};

static bool G_MDNS_STARTED=false;
static String G_MDNS_HOST="";

static String stripPeerHost(String h){
  h.trim();
  if(h.startsWith("http://"))h.remove(0,7);
  else if(h.startsWith("https://"))h.remove(0,8);
  int slash=h.indexOf('/');
  if(slash>=0)h=h.substring(0,slash);
  while(h.endsWith("/")&&h.length())h.remove(h.length()-1);
  return h;
}

static String httpRequestRaw(String method,String url,String body,String contentType,int&status,String headerName="",String headerValue=""){
  status=-1;
  if(WiFi.status()!=WL_CONNECTED)return "";
  // MellorsOS v1.0 keeps device-side web requests on plain HTTP. Public HTTPS
  // downloads for Mellors Store are intentionally handled by the MConnect
  // browser client, which then sends validated MScript source to the device.
  if(url.startsWith("https://")){status=-3;return "";}
  if(!url.startsWith("http://")){status=-2;return "";}
  HTTPClient http;WiFiClient client;
  http.setConnectTimeout(2500);http.setTimeout(3500);http.useHTTP10(true);
  http.setUserAgent(String("MellorsOS/")+OS_VERSION+" CyberPi");
  if(!http.begin(client,url)){status=-2;return "";}
  if(contentType.length())http.addHeader("Content-Type",contentType);
  if(headerName.length())http.addHeader(headerName,headerValue);
  method.toUpperCase();
  if(method=="GET")status=http.GET();else status=http.sendRequest(method.c_str(),body);
  String out=status>0?http.getString():String("");http.end();return out;
}

enum WifiSecurityType:uint8_t{WIFI_SECURITY_OPEN=0,WIFI_SECURITY_PERSONAL=1,WIFI_SECURITY_ENTERPRISE=2};
static uint8_t wifiSecurityClass(wifi_auth_mode_t auth){return auth==WIFI_AUTH_OPEN?WIFI_SECURITY_OPEN:auth==WIFI_AUTH_WPA2_ENTERPRISE?WIFI_SECURITY_ENTERPRISE:WIFI_SECURITY_PERSONAL;}
static String wifiSecurityLabel(wifi_auth_mode_t auth){
  switch(auth){
    case WIFI_AUTH_OPEN:return "Open";
    case WIFI_AUTH_WEP:return "WEP";
    case WIFI_AUTH_WPA_PSK:return "WPA";
    case WIFI_AUTH_WPA2_PSK:return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:return "WPA2 Enterprise";
#ifdef WIFI_AUTH_WPA3_PSK
    case WIFI_AUTH_WPA3_PSK:return "WPA3";
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
    case WIFI_AUTH_WPA2_WPA3_PSK:return "WPA2/WPA3";
#endif
    default:return "WPA";
  }
}
static String wifiSecurityName(uint8_t security){return security==WIFI_SECURITY_OPEN?String("Open"):security==WIFI_SECURITY_ENTERPRISE?String("WPA Enterprise"):String("WPA");}

struct WifiEntry{String ssid,password,username;uint8_t security=WIFI_SECURITY_OPEN;};
static const int MAX_SAVED_WIFI=12;
class WifiManager{
  Preferences p;WifiEntry saved[MAX_SAVED_WIFI];int n=0;String host="mellorsos";
  bool backgroundEnabled=true,attempting=false,foregroundScan=false,sleepConfigured=false;int bgCursor=0;unsigned long attemptStarted=0,nextAttempt=0;String preferred="";
  String key(int i,const char*s){return String(s)+String(i);}
  void disableEnterprise(){esp_wifi_sta_wpa2_ent_disable();}
  bool beginEntry(const WifiEntry&e){
    if(!backgroundEnabled)return false;
    WiFi.mode(WIFI_STA);WiFi.setHostname(host.c_str());WiFi.disconnect();disableEnterprise();
    if(e.security==WIFI_SECURITY_ENTERPRISE){
      if(!e.username.length()||!e.password.length())return false;
      // Arduino-ESP32 2.0.17 provides a native WPA2-Enterprise PEAP overload.
      // Use the username as both outer identity and PEAP username, matching
      // the two-field (username/password) MellorsOS setup requested here.
      WiFi.begin(e.ssid.c_str(),WPA2_AUTH_PEAP,e.username.c_str(),e.username.c_str(),e.password.c_str());
    }else if(e.security==WIFI_SECURITY_OPEN)WiFi.begin(e.ssid.c_str());
    else WiFi.begin(e.ssid.c_str(),e.password.c_str());
    return true;
  }
  int nextIndex(){
    if(n<=0)return-1;
    int pref=find(preferred);
    if(pref<0){int ix=constrain(bgCursor,0,n-1);bgCursor=(ix+1)%n;return ix;}
    // Each reconnect cycle tries the preferred network first, then every other
    // saved network exactly once. This lets MellorsOS roam among multiple
    // remembered networks instead of getting stuck retrying one SSID forever.
    if(bgCursor<=0){bgCursor=n>1?1:0;return pref;}
    int wanted=bgCursor-1,seen=0;
    for(int i=0;i<n;i++)if(i!=pref){if(seen==wanted){bgCursor++;if(bgCursor>=n)bgCursor=0;return i;}seen++;}
    bgCursor=0;return pref;
  }
public:
  void begin(const String&hostname){p.begin("wifiList",false);load();host=safeHostname(hostname);WiFi.mode(WIFI_MODE_NULL);delay(25);WiFi.setHostname(host.c_str());WiFi.mode(WIFI_STA);backgroundEnabled=true;}
  void setHostname(const String&hostname){host=safeHostname(hostname);WiFi.setHostname(host.c_str());if(WiFi.status()==WL_CONNECTED)return;enable();}
  String hostname(){return host;}
  void disable(){backgroundEnabled=false;attempting=false;WiFi.disconnect(true);WiFi.mode(WIFI_MODE_NULL);}
  void enable(){backgroundEnabled=true;WiFi.mode(WIFI_STA);WiFi.setHostname(host.c_str());nextAttempt=0;}
  void load(){n=constrain(p.getInt("count",0),0,MAX_SAVED_WIFI);for(int i=0;i<n;i++){saved[i].ssid=p.getString(key(i,"s").c_str(),"");saved[i].password=p.getString(key(i,"p").c_str(),"");saved[i].username=p.getString(key(i,"u").c_str(),"");int sec=p.getInt(key(i,"t").c_str(),-1);saved[i].security=sec<0?(saved[i].password.length()?WIFI_SECURITY_PERSONAL:WIFI_SECURITY_OPEN):(uint8_t)constrain(sec,0,2);}}
  void saveAll(){p.putInt("count",n);for(int i=0;i<n;i++){p.putString(key(i,"s").c_str(),saved[i].ssid);p.putString(key(i,"p").c_str(),saved[i].password);p.putString(key(i,"u").c_str(),saved[i].username);p.putInt(key(i,"t").c_str(),saved[i].security);}}
  int count(){return n;}String ssid(int i){return i>=0&&i<n?saved[i].ssid:"";}String password(int i){return i>=0&&i<n?saved[i].password:"";}String username(int i){return i>=0&&i<n?saved[i].username:"";}uint8_t security(int i){return i>=0&&i<n?saved[i].security:WIFI_SECURITY_OPEN;}
  int find(const String&s){for(int i=0;i<n;i++)if(saved[i].ssid==s)return i;return-1;}
  void remember(String ssid,uint8_t security,String username,String password){int i=find(ssid);if(i<0){if(n<MAX_SAVED_WIFI)i=n++;else{for(int j=MAX_SAVED_WIFI-1;j>0;j--)saved[j]=saved[j-1];i=0;}}saved[i].ssid=ssid;saved[i].security=security;saved[i].username=security==WIFI_SECURITY_ENTERPRISE?username:String("");saved[i].password=security==WIFI_SECURITY_OPEN?String(""):password;saveAll();}
  void remember(String ssid,String password){remember(ssid,password.length()?WIFI_SECURITY_PERSONAL:WIFI_SECURITY_OPEN,"",password);}
  void forget(int i){if(i<0||i>=n)return;for(int j=i;j<n-1;j++)saved[j]=saved[j+1];n--;saveAll();if(bgCursor>n)bgCursor=0;}
  void clearSaved(){p.clear();n=0;attempting=false;bgCursor=0;}
  bool connect(String ssid,uint8_t security,String username,String password,int timeout=9000){
    backgroundEnabled=true;WifiEntry e;e.ssid=ssid;e.security=security;e.username=username;e.password=password;if(!beginEntry(e))return false;
    unsigned long st=millis();while(WiFi.status()!=WL_CONNECTED&&millis()-st<(unsigned)timeout){delay(80);}
    if(WiFi.status()==WL_CONNECTED){WiFi.setSleep(false);remember(ssid,security,username,password);preferred=ssid;attempting=false;configTime(0,0,"pool.ntp.org","time.nist.gov");return true;}return false;
  }
  bool connect(String ssid,String password,int timeout=9000){return connect(ssid,password.length()?WIFI_SECURITY_PERSONAL:WIFI_SECURITY_OPEN,"",password,timeout);}
  bool connectSaved(int i,int timeout=9000){if(i<0||i>=n)return false;return connect(saved[i].ssid,saved[i].security,saved[i].username,saved[i].password,timeout);}
  void startBackground(String preferredSsid=""){preferred=preferredSsid;backgroundEnabled=true;attempting=false;bgCursor=0;nextAttempt=0;enable();}
  void scheduleConnect(String ssid,uint8_t security,String username,String password){remember(ssid,security,username,password);preferred=ssid;backgroundEnabled=true;attempting=false;bgCursor=0;WiFi.disconnect();nextAttempt=millis()+250;}
  void beginForegroundScan(){
    foregroundScan=true;
    // A pending association blocks/poisons scans on some ESP32 stacks. Pause
    // only that attempt; keep an already-connected station attached while
    // scanning so opening the network list does not unnecessarily drop Wi-Fi.
    if(attempting&&WiFi.status()!=WL_CONNECTED){WiFi.disconnect();attempting=false;}
    WiFi.mode(WIFI_STA);WiFi.setHostname(host.c_str());delay(60);
  }
  void endForegroundScan(){
    foregroundScan=false;
    if(backgroundEnabled&&WiFi.status()!=WL_CONNECTED){bgCursor=0;nextAttempt=millis()+200;}
  }
  void backgroundTick(){
    if(foregroundScan||!backgroundEnabled||n<=0)return;
    if(WiFi.status()==WL_CONNECTED){if(!sleepConfigured){WiFi.setSleep(false);sleepConfigured=true;}if(attempting){attempting=false;configTime(0,0,"pool.ntp.org","time.nist.gov");}return;}
    sleepConfigured=false;unsigned long now=millis();
    if(attempting){if((unsigned long)(now-attemptStarted)<9000)return;WiFi.disconnect();attempting=false;nextAttempt=now+1200;return;}
    if((long)(now-nextAttempt)<0)return;
    int ix=nextIndex();if(ix<0){nextAttempt=now+5000;return;}
    if(beginEntry(saved[ix])){attempting=true;attemptStarted=now;nextAttempt=now+10000;}else nextAttempt=now+2000;
  }
  bool autoConnect(String preferredSsid=""){startBackground(preferredSsid);unsigned long st=millis();while(WiFi.status()!=WL_CONNECTED&&millis()-st<5000){backgroundTick();delay(50);}return WiFi.status()==WL_CONNECTED;}
};

struct AppMeta{String name,id,version,color,accepts;};
struct App{AppMeta meta;uint16_t icon[256];bool system=false,developer=false,dynamic=false,valid=false;const char* builtin=nullptr;String path;};

static bool nextLine(const char*src,int&pos,String&out){if(!src||!src[pos])return false;out="";while(src[pos]&&src[pos]!='\n'&&src[pos]!='\r')out+=src[pos++];while(src[pos]=='\n'||src[pos]=='\r')pos++;return true;}
static String qval(const String&l){int a=l.indexOf('"'),b=l.lastIndexOf('"');return a>=0&&b>a?l.substring(a+1,b):"";}
static bool manifest(const char*src,AppMeta&m,uint16_t icon[256],Display&d,String&err){m=AppMeta();int pos=0,cnt=0;bool ico=false,saw=false;String l;while(nextLine(src,pos,l)){l.trim();if(!l.length())continue;if(!ico&&l.startsWith("app "))m.name=qval(l);else if(!ico&&(l.startsWith("meta id ")||l.startsWith("meta package ")))m.id=qval(l);else if(!ico&&l.startsWith("meta version "))m.version=qval(l);else if(!ico&&l.startsWith("meta color "))m.color=qval(l);else if(!ico&&l.startsWith("meta accepts "))m.accepts=qval(l);else if(!ico&&l=="icon16"){ico=true;saw=true;}else if(ico&&l=="endicon")ico=false;else if(ico){String tok="";for(int p=0;p<=(int)l.length();p++){char c=p<(int)l.length()?l[p]:' ';if(c==','||isspace((unsigned char)c)||p==(int)l.length()){tok.trim();if(tok.length()){if(cnt>=256){err="Icon >256 colors";return false;}icon[cnt++]=d.c565(rgb(tok));tok="";}}else tok+=c;}}}if(!m.name.length()){err="Missing app name";return false;}if(!isPkg(m.id)){err="Invalid/missing package";return false;}if(!saw||cnt!=256){err=String("Icon needs 256 RGB values; got ")+cnt;return false;}if(!m.version.length())m.version="1.0";if(!m.color.length())m.color="#6d7cff";return true;}

static bool validateCodeShape(const String&raw,int lineNo,String&err){
  bool quote=false,escape=false;int paren=0;
  for(size_t i=0;i<raw.length();i++){
    unsigned char c=(unsigned char)raw[i];
    if(quote){if(escape){escape=false;continue;}if(c=='\\'){escape=true;continue;}if(c=='"')quote=false;continue;}
    if(c=='"'){quote=true;continue;}
    if(c=='(')paren++;
    else if(c==')'){if(paren<=0){err=String("Line ")+lineNo+": unexpected )";return false;}paren--;}
    else if(c<32&&c!='\t'){err=String("Line ")+lineNo+": invalid control character";return false;}
  }
  if(quote){err=String("Line ")+lineNo+": unterminated string";return false;}
  if(paren){err=String("Line ")+lineNo+": unmatched parentheses";return false;}
  return true;
}

// Structural/lexical MScript verifier used before *every* app installation.
// It intentionally does not execute app code.
static bool validateMScriptSyntax(const String&src,String&err){
  if(!src.length()){err="Empty app";return false;}
  // There is deliberately no policy limit on source byte count. Large apps are
  // accepted until the device reaches a real storage or runtime-memory limit.
  String blocks="",elseSeen="";int depth=0,pos=0,lineNo=0;bool icon=false,sawCode=false;String raw;
  while(nextLine(src.c_str(),pos,raw)){
    lineNo++;String l=raw;l.trim();if(!l.length()||l.startsWith("#"))continue;
    if(icon){if(l=="endicon")icon=false;continue;}
    if(l=="icon16"){icon=true;continue;}
    if(l.startsWith("app ")||l.startsWith("meta ")){if(sawCode){err=String("Line ")+lineNo+": manifest entry after code";return false;}continue;}
    sawCode=true;
    if(!validateCodeShape(l,lineNo,err))return false;
    char kind=0;
    if(l.startsWith("on ")){if(l.substring(3).length()==0){err=String("Line ")+lineNo+": missing event name";return false;}kind='E';}
    else if(l.startsWith("function ")){String h=l.substring(9);h.trim();if(!h.length()){err=String("Line ")+lineNo+": missing function name";return false;}kind='F';}
    else if(l.startsWith("if ")){if(l.substring(3).length()==0){err=String("Line ")+lineNo+": missing if expression";return false;}kind='I';}
    else if(l.startsWith("while ")){if(l.substring(6).length()==0){err=String("Line ")+lineNo+": missing while expression";return false;}kind='W';}
    else if(l.startsWith("repeat ")){if(l.substring(7).length()==0){err=String("Line ")+lineNo+": missing repeat count";return false;}kind='R';}
    else if(l.startsWith("for ")){String h=l.substring(4);if(h.indexOf(" from ")<=0||h.indexOf(" to ")<0){err=String("Line ")+lineNo+": malformed for statement";return false;}kind='L';}
    if(kind){blocks+=kind;elseSeen+='0';depth++;continue;}
    if(l=="else"){
      if(depth<=0||blocks[depth-1]!='I'||elseSeen[depth-1]=='1'){err=String("Line ")+lineNo+": unexpected else";return false;}
      elseSeen.setCharAt(depth-1,'1');continue;
    }
    if(l=="end"){
      if(depth<=0){err=String("Line ")+lineNo+": unexpected end";return false;}
      depth--;blocks.remove(depth);elseSeen.remove(depth);continue;
    }
    if(l.startsWith("let ")||l.startsWith("set ")){
      String r=l.substring(4);int eq=r.indexOf('=');if(eq<=0){err=String("Line ")+lineNo+": assignment needs =";return false;}
      String name=r.substring(0,eq);name.trim();if(!name.length()){err=String("Line ")+lineNo+": missing variable name";return false;}
    }
  }
  if(icon){err="Missing endicon";return false;}
  if(depth){err="Missing end for code block";return false;}
  if(!sawCode){err="App has no executable code";return false;}
  return true;
}

class Registry{
  Display&d;SettingsStore&settings;App a[MAX_APPS];int n=0;
public:Registry(Display&x,SettingsStore&s):d(x),settings(s){}
  int count(){return n;} App&at(int i){return a[i];}
  String source(int i,String&tmp){if(i<0||i>=n)return"";if(a[i].builtin)return String(a[i].builtin);File f=LittleFS.open(a[i].path,"r");if(!f)return"";tmp=f.readString();f.close();return tmp;}
  int find(String id){for(int i=0;i<n;i++)if(a[i].meta.id==id)return i;return-1;}
  void ensureAppFolder(const String&id){ensureFolder("/apps");ensureFolder(String("/apps/")+safeName(id));}
  void addBuiltin(const char*src,bool system,bool developer){if(n>=MAX_APPS)return;String e;a[n].builtin=src;a[n].system=system;a[n].developer=developer;a[n].dynamic=false;a[n].valid=manifest(src,a[n].meta,a[n].icon,d,e)&&validateMScriptSyntax(String(src),e);if(!a[n].valid){Serial.println("Built-in app syntax error: "+e);}else ensureAppFolder(a[n].meta.id);n++;}
  String packagePath(const String&id){return String("/system/apps/")+id+"/main.mapp";}
  void scanInstalled(){
    ensureDirectories("/system/apps");
    // Migrate only verified legacy packages, leaving originals intact on failure.
    File legacy=LittleFS.open("/installed");String paths[MAX_APPS];int count=0;
    if(legacy&&legacy.isDirectory()){File f=legacy.openNextFile();while(f&&count<MAX_APPS){if(!f.isDirectory())paths[count++]=f.path();f.close();f=legacy.openNextFile();}legacy.close();}
    for(int i=0;i<count;i++){
      File f=LittleFS.open(paths[i]);String src=f?f.readString():String("");f.close();AppMeta m;uint16_t icon[256];String why;
      if(!manifest(src.c_str(),m,icon,d,why)||!validateMScriptSyntax(src,why))continue;
      String to=packagePath(m.id);ensureDirectories(String("/system/apps/")+m.id);
      if(LittleFS.exists(to))LittleFS.remove(paths[i]);
      else if(!LittleFS.rename(paths[i],to))Serial.println("[MellorsOS] Package migration failed: "+m.id);
    }
    File root=LittleFS.open("/system/apps");if(!root)return;
    File dir=root.openNextFile();while(dir&&n<MAX_APPS){
      String path=String(dir.path())+"/main.mapp";bool folder=dir.isDirectory();dir.close();
      if(folder){File f=LittleFS.open(path);String src=f?f.readString():String("");f.close();String why;App t;t.dynamic=true;t.path=path;
        t.valid=manifest(src.c_str(),t.meta,t.icon,d,why)&&validateMScriptSyntax(src,why);
        if(t.valid&&find(t.meta.id)<0&&path==packagePath(t.meta.id)){ensureAppFolder(t.meta.id);a[n++]=t;}
      }dir=root.openNextFile();
    }root.close();
  }
  bool install(String src,String&why){
    AppMeta m;uint16_t ic[256];
    if(!manifest(src.c_str(),m,ic,d,why))return false;
    if(!validateMScriptSyntax(src,why))return false;
    int existing=find(m.id);
    if(existing>=0&&!a[existing].dynamic){why="Cannot replace built-in app";return false;}
    if(existing<0&&n>=MAX_APPS){why="App limit reached";return false;}
    String path=packagePath(m.id);
    // Stage the complete package before replacement; failed updates keep the old app.
    if(!atomicWriteFile(path,(const uint8_t*)src.c_str(),src.length())){
      why="STORAGE_FULL|Could not stage the complete app. Existing app was preserved.";return false;
    }
    ensureAppFolder(m.id);
    int ix=find(m.id);
    if(ix>=0){a[ix].meta=m;memcpy(a[ix].icon,ic,sizeof(ic));a[ix].dynamic=true;a[ix].path=path;a[ix].builtin=nullptr;a[ix].valid=true;}
    else{a[n].meta=m;memcpy(a[n].icon,ic,sizeof(ic));a[n].dynamic=true;a[n].path=path;a[n].valid=true;n++;}
    return true;
  }
  bool remove(String id){int ix=find(id);if(ix<0||!a[ix].dynamic)return false;LittleFS.remove(a[ix].path);for(int i=ix;i<n-1;i++)a[i]=a[i+1];n--;return true;}
  bool hidden(String id){String h="|"+settings.hiddenApps()+"|";return h.indexOf("|"+id+"|")>=0;}
  void setHidden(String id,bool hide){if(id=="settings")return;String list=settings.hiddenApps();String wrapped="|"+list+"|";bool has=wrapped.indexOf("|"+id+"|")>=0;if(hide&&!has){if(list.length())list+='|';list+=id;}if(!hide&&has){String out="";int st=0;while(st<=list.length()){int q=list.indexOf('|',st);String x=q<0?list.substring(st):list.substring(st,q);if(x.length()&&x!=id){if(out.length())out+='|';out+=x;}if(q<0)break;st=q+1;}list=out;}settings.hiddenApps(list);}
  String normalizedOrder(){String raw=settings.homeOrder();String ids[MAX_APPS];int c=0,st=0;while(st<=raw.length()&&c<MAX_APPS){int q=raw.indexOf('|',st);String id=q<0?raw.substring(st):raw.substring(st,q);if(id.length()&&find(id)>=0){bool dup=false;for(int j=0;j<c;j++)if(ids[j]==id)dup=true;if(!dup)ids[c++]=id;}if(q<0)break;st=q+1;}for(int i=0;i<n&&c<MAX_APPS;i++){bool has=false;for(int j=0;j<c;j++)if(ids[j]==a[i].meta.id)has=true;if(!has)ids[c++]=a[i].meta.id;}String out="";for(int i=0;i<c;i++){if(out.length())out+='|';out+=ids[i];}if(out!=raw)settings.homeOrder(out);return out;}
  void buildVisible(int*out,int&count){count=0;String raw=normalizedOrder();int st=0;while(st<=raw.length()&&count<MAX_APPS){int q=raw.indexOf('|',st);String id=q<0?raw.substring(st):raw.substring(st,q);int ix=find(id);if(ix>=0&&visible(ix))out[count++]=ix;if(q<0)break;st=q+1;}}
  void moveHome(String id,int delta){String raw=normalizedOrder();String ids[MAX_APPS];int c=0,st=0;while(st<=raw.length()&&c<MAX_APPS){int q=raw.indexOf('|',st);String x=q<0?raw.substring(st):raw.substring(st,q);if(x.length())ids[c++]=x;if(q<0)break;st=q+1;}int at=-1;for(int i=0;i<c;i++)if(ids[i]==id)at=i;if(at<0)return;int to=constrain(at+delta,0,c-1);String tmp=ids[at];ids[at]=ids[to];ids[to]=tmp;String o="";for(int i=0;i<c;i++){if(o.length())o+='|';o+=ids[i];}settings.homeOrder(o);}
  bool visible(int i){if(i<0||i>=n||!a[i].valid||hidden(a[i].meta.id))return false;if(a[i].developer&&!settings.developerMode())return false;return true;}
  String listIds(){String s="";for(int i=0;i<n;i++){if(s.length())s+='|';s+=a[i].meta.id;}return s;}
};


class MellorsPeerServer{
  WifiManager& wifi;
  WebServer server;
  bool active=false;
  bool routesReady=false;
  TaskHandle_t taskHandle=nullptr;
  void routes(){
    if(routesReady)return;
    routesReady=true;
    server.on("/mellors/ping",HTTP_GET,[this](){
      String body=String("{\"os\":\"MellorsOS\",\"version\":\"")+OS_VERSION+"\",\"hostname\":\""+wifi.hostname()+"\",\"ip\":\""+WiFi.localIP().toString()+"\"}";
      server.sendHeader("Cache-Control","no-store");
      server.send(200,"application/json",body);
    });
    server.on("/mellors/message",HTTP_POST,[this](){
      if(G_PEER_PENDING){server.send(429,"text/plain","busy");return;}
      String msg=server.arg("plain");
      String from=server.arg("from");
      if(!from.length())from="peer";
      size_t mn=msg.length();if(mn>383)mn=383;
      size_t fn=from.length();if(fn>63)fn=63;
      memcpy(G_PEER_MESSAGE,msg.c_str(),mn);G_PEER_MESSAGE[mn]=0;
      memcpy(G_PEER_SENDER,from.c_str(),fn);G_PEER_SENDER[fn]=0;
      G_PEER_PENDING=true;
      server.send(200,"application/json","{\"ok\":true}");
    });
    server.onNotFound([this](){server.send(404,"text/plain","MellorsOS peer endpoint");});
  }
  void service(){
    bool wanted=WiFi.status()==WL_CONNECTED;
    String host=wifi.hostname();
    if(wanted){
      if(!G_MDNS_STARTED||G_MDNS_HOST!=host){
        if(G_MDNS_STARTED)MDNS.end();
        G_MDNS_STARTED=MDNS.begin(host.c_str());
        G_MDNS_HOST=G_MDNS_STARTED?host:String("");
        if(G_MDNS_STARTED){
          MDNS.addService("mellors","tcp",81);
          MDNS.addServiceTxt("mellors","tcp","os","MellorsOS");
          MDNS.addServiceTxt("mellors","tcp","version",OS_VERSION);
        }
      }
      if(!active){routes();server.begin();active=true;}
    }else{
      if(active){server.stop();active=false;}
      if(G_MDNS_STARTED){MDNS.end();G_MDNS_STARTED=false;G_MDNS_HOST="";}
    }
    if(active)server.handleClient();
  }
  static void taskEntry(void*arg){
    MellorsPeerServer*self=(MellorsPeerServer*)arg;
    while(true){self->service();vTaskDelay(pdMS_TO_TICKS(20));}
  }
public:
  MellorsPeerServer(WifiManager&w):wifi(w),server(81){}
  void beginTask(){/* v0.9: serviced by kernel task */}
  void tick(){service();}
  String address(){return WiFi.status()==WL_CONNECTED?String("http://")+WiFi.localIP().toString()+":81":String("");}
};

static const char* PERM_IDS[]={"settings","notifications","installApps","modifyPermissions","allFiles","background","superuser","network","microphone"};
static const char* PERM_LABELS[]={"Settings","Notifications","Install apps","Modify permissions","All files","Background","Superuser","Network","Microphone"};
static const int PERM_COUNT=9;
static int permId(String s){for(int i=0;i<PERM_COUNT;i++)if(s==PERM_IDS[i])return i;return-1;}
class Permissions{
  Preferences p;Modal&m;
  String key(String id){return "p"+hex8(hash32(id));}
public:Permissions(Modal&x):m(x){}void begin(){p.begin("permissions",false);}bool has(const App&a,int pi){if(a.system)return true;String h=p.getString("superuser","");if(h==a.meta.id)return true;if(pi==6)return false;uint32_t mask=p.getUInt(key(a.meta.id).c_str(),0);return pi>=0&&(mask&(1UL<<pi));}
  bool ask(App&a,int pi){if(a.system)return true;if(pi<0)return false;if(has(a,pi))return true;if(pi==6){String h=p.getString("superuser","");if(h.length()&&h!=a.meta.id){m.msg("Superuser","Already owned by",h);return false;}if(!m.confirm("Superuser 1/3",a.meta.name,"Requests full control"))return false;if(!m.confirm("Superuser 2/3","Can alter system","and system apps"))return false;if(!m.confirm("Superuser 3/3","Grant Superuser?","Factory reset clears it"))return false;p.putString("superuser",a.meta.id);return true;}if(!m.confirm("Permission",a.meta.name,PERM_LABELS[pi]))return false;uint32_t mask=p.getUInt(key(a.meta.id).c_str(),0);p.putUInt(key(a.meta.id).c_str(),mask|(1UL<<pi));return true;}
  bool set(App&a,int pi,bool v){if(a.system||holder()==a.meta.id||pi<0||pi==6)return false;uint32_t mask=p.getUInt(key(a.meta.id).c_str(),0);if(v)mask|=1UL<<pi;else mask&=~(1UL<<pi);p.putUInt(key(a.meta.id).c_str(),mask);return true;}
  String holder(){return p.getString("superuser","");}bool disable(App&a){if(holder()!=a.meta.id)return false;p.remove("superuser");return true;}void clearApp(String id){p.remove(key(id).c_str());if(holder()==id)p.remove("superuser");}void reset(){p.clear();}
};

class DataStore{
  Preferences p;
  String key(String app,String k){String a=hex8(hash32(app)),b=hex8(hash32(k));return "d"+a+b.substring(0,6);}
  String idxKey(String app){return "i"+hex8(hash32(app));}
  String pack(const Value&v){if(v.type==Value::NUMBER)return"N:"+v.str();if(v.type==Value::BOOLEAN)return String("B:")+(v.boolean?"1":"0");if(v.type==Value::STRING)return"S:"+v.string;return"Z:";}
  Value unpack(String s,Value fallback){if(s.length()<2)return fallback;String b=s.substring(2);if(s.startsWith("N:"))return Value(b.toDouble());if(s.startsWith("B:"))return Value(b=="1");if(s.startsWith("S:"))return Value(b);if(s.startsWith("Z:"))return Value();return fallback;}
  void addIdx(String app,String k){String ik=idxKey(app),list=p.getString(ik.c_str(),"");String w="|"+list+"|";if(w.indexOf("|"+k+"|")<0){if(list.length())list+='|';list+=k;p.putString(ik.c_str(),list);}}
public:void begin(){p.begin("msdata",false);}Value get(String app,String k,Value f=Value("")){return unpack(p.getString(key(app,k).c_str(),""),f);}Value set(String app,String k,Value v){p.putString(key(app,k).c_str(),pack(v));addIdx(app,k);return v;}bool del(String app,String k){p.remove(key(app,k).c_str());return true;}String keys(String app,String prefix=""){String list=p.getString(idxKey(app).c_str(),"");if(!prefix.length())return list;String out="";int st=0;while(st<=list.length()){int q=list.indexOf('|',st);String x=q<0?list.substring(st):list.substring(st,q);if(x.startsWith(prefix)){if(out.length())out+='|';out+=x;}if(q<0)break;st=q+1;}return out;}bool has(String app,String k){return p.isKey(key(app,k).c_str());}void clear(String app){String l=keys(app);int st=0;while(st<=l.length()){int q=l.indexOf('|',st);String x=q<0?l.substring(st):l.substring(st,q);if(x.length())p.remove(key(app,x).c_str());if(q<0)break;st=q+1;}p.remove(idxKey(app).c_str());}int count(String app,String prefix=""){String l=keys(app,prefix);if(!l.length())return 0;int c=1;for(size_t i=0;i<l.length();i++)if(l[i]=='|')c++;return c;}void reset(){p.clear();}Value append(String app,String k,Value v,String sep=""){String n=get(app,k,Value("")).str()+sep+v.str();return set(app,k,n);}Value increment(String app,String k,double amt=1){double n=get(app,k,Value(0)).num()+amt;return set(app,k,Value(n));}
};

static const char MCONNECT_HTML[] PROGMEM = R"MCONNECT(
<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MConnect</title>
<style>
:root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#0b1020;color:#edf2ff}.shell{max-width:940px;margin:auto;padding:22px}header{display:flex;align-items:center;gap:12px;margin-bottom:18px}.logo{width:42px;height:42px;border-radius:13px;background:#4F46E5;display:grid;place-items:center;font-weight:900}h1,h2,h3,p{margin-top:0}.card{background:#151d31;border:1px solid #2d3a59;border-radius:18px;padding:20px;margin:14px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.service,.sectionCard{cursor:pointer;min-height:120px}.service:hover,.sectionCard:hover{border-color:#6d7cff}button,input,select{font:inherit}button{border:0;border-radius:11px;padding:11px 15px;background:#4F46E5;color:#fff;font-weight:750;cursor:pointer}button.secondary{background:#26334f}.danger{background:#b42318}.row{display:flex;gap:10px;flex-wrap:wrap}.grow{flex:1}input,select{width:100%;padding:11px;border-radius:10px;border:1px solid #42516f;background:#0d1424;color:#fff;margin:5px 0 12px}label{font-size:.9rem;color:#bcc8df}.muted{color:#9dadc8}.bad{color:#ff9c9c}.hidden{display:none!important}.setting{display:grid;grid-template-columns:1fr minmax(160px,280px);gap:15px;align-items:center;padding:9px 0;border-bottom:1px solid #26334f}.app{padding:14px;border:1px solid #2d3a59;border-radius:13px;margin:10px 0}.app p{margin:6px 0;color:#aebbd3}.search{font-size:1rem}.progress{height:12px;background:#26334f;border-radius:999px;overflow:hidden;margin-top:10px}.progress>div{height:100%;width:0;background:#4F46E5;transition:width .15s}.progressText{font-size:.86rem;color:#bcc8df;margin-top:6px}.savePopup{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:#111a2d;border:1px solid #435270;padding:11px 16px;border-radius:12px;display:none;z-index:30;min-width:140px;text-align:center}.savePopup.bad{border-color:#8f3636;color:#ffb4b4}.modal{position:fixed;inset:0;background:#050814cc;display:grid;place-items:center;padding:18px;z-index:50}.modalBox{width:min(420px,100%);background:#151d31;border:1px solid #435270;border-radius:18px;padding:20px}.pill{display:inline-block;padding:4px 8px;border-radius:999px;background:#26334f;color:#cbd6ec;font-size:.78rem;margin-left:6px}@media(max-width:600px){.setting{grid-template-columns:1fr}.shell{padding:14px}}
</style></head><body><div class="shell">
<header><div class="logo">M</div><div><h1 style="margin:0">MConnect</h1><div class="muted">MellorsOS device services</div></div><div class="grow"></div><button id="lockSession" class="secondary hidden">Lock session</button></header>
<section id="home"><div class="card"><h2 id="deviceName">MellorsOS</h2><p id="deviceInfo" class="muted">Connecting to the local device…</p><p class="muted">Store browsing does not require a PIN. MellorsOS asks for the device PIN when you open Remote Settings, Files, or install an app.</p></div><div class="grid"><div class="card service" data-open="store"><h3>Mellors Store</h3><p class="muted">Browse and search the GitHub catalog without signing in.</p></div><div class="card service" data-open="settings"><h3>Remote Settings</h3><p class="muted">PIN protected. Settings are organized like the on-device Settings menu.</p></div><div class="card service" data-open="sideload"><h3>App Sideloading</h3><p class="muted">Choose a .mapp or .txt file freely. The PIN is requested only when you install.</p></div><div class="card service" data-open="files"><h3>Files</h3><p class="muted">PIN-protected filesystem browser, editor, upload and download.</p></div></div></section>
<section id="store" class="hidden"><div class="card"><div class="row"><button class="secondary back">← Services</button><button id="refreshStore" class="secondary">Refresh</button></div><h2 style="margin-top:16px">Mellors Store</h2><input id="storeSearch" class="search" type="search" placeholder="Search apps…"><p class="muted">Catalog source: Linkers15/Mellors-Store</p><div id="storeApps">Loading…</div></div></section>
<section id="settings" class="hidden"><div class="card"><div class="row"><button class="secondary back">← Services</button><button id="settingsHome" class="secondary hidden">← Settings sections</button></div><h2 style="margin-top:16px">Remote Settings</h2><div id="settingsSections" class="grid"></div><div id="settingsPanel" class="hidden"></div></div></section>
<section id="sideload" class="hidden"><div class="card"><div class="row"><button class="secondary back">← Services</button></div><h2 style="margin-top:16px">App Sideloading</h2><p id="sideNote" class="muted">Choose a file. MellorsOS will request the PIN only when installation starts.</p><input id="appFile" type="file" accept=".mapp,.txt,text/plain"><button id="installFile">Verify & install</button><div id="sideProgress" class="progress hidden"><div></div></div><div id="sideProgressText" class="progressText"></div></div></section>
<section id="files" class="hidden"><div class="card"><div class="row"><button class="secondary back">← Services</button><button id="fsUp" class="secondary">Parent folder</button><button id="fsRefresh" class="secondary">Refresh</button></div><h2 style="margin-top:16px">Files</h2><p class="muted">Read and write files anywhere on the device. Internal app packages are under /system/apps; changing those files affects installed apps after restart.</p><div class="row"><input id="fsPath" value="/" aria-label="Folder path"><button id="fsGo">Open folder</button></div><div class="row"><button id="fsNewFolder" class="secondary">New folder</button><button id="fsNewText" class="secondary">New text file</button><label>Upload files<input id="fsUpload" type="file" multiple></label></div><p id="fsStatus" class="muted" role="status"></p><div id="fsEntries"></div><button id="fsMore" class="secondary hidden">Load more</button><div id="fsEditor" class="hidden"><h3 id="fsEditTitle"></h3><textarea id="fsText" style="width:100%;min-height:260px;background:#0d1424;color:white" aria-label="File contents"></textarea><div class="row"><button id="fsSave">Save text</button><button id="fsClose" class="secondary">Close editor</button></div><p class="muted">Text editing uses UTF-8. Use upload/download to preserve arbitrary binary files.</p></div></div></section>

</div><div id="savePopup" class="savePopup"></div>
<div id="pinModal" class="modal hidden"><div class="modalBox"><h2 id="pinTitle">Device PIN required</h2><p id="pinReason" class="muted"></p><label>4-digit MellorsOS PIN</label><input id="pinInput" inputmode="numeric" maxlength="4" pattern="[0-9]{4}" placeholder="••••"><p id="pinError" class="bad"></p><div class="row"><button id="pinSubmit">Continue</button><button id="pinCancel" class="secondary">Cancel</button></div></div></div>
<script>
const STORE_URL='https://raw.githubusercontent.com/Linkers15/Mellors-Store/refs/heads/main/store.txt';
let token='',device=null,pinResolve=null,storeCatalog=[];
const $=s=>document.querySelector(s), $$=s=>[...document.querySelectorAll(s)];
function show(id){['home','store','settings','sideload','files'].forEach(x=>$('#'+x).classList.toggle('hidden',x!==id))}
function setSave(text,bad=false,hide=0){const e=$('#savePopup');e.textContent=text;e.className='savePopup'+(bad?' bad':'');e.style.display='block';clearTimeout(e._t);if(hide)e._t=setTimeout(()=>e.style.display='none',hide)}
function lockSession(){token='';$('#lockSession').classList.add('hidden')}
$('#lockSession').onclick=lockSession;
async function publicApi(path,opt={}){return fetch(path,opt)}
async function protectedApi(path,opt={},retry=true){if(!token&&!(await ensureAuth('This action is protected by your MellorsOS lock PIN.')))throw new Error('PIN required');opt.headers=Object.assign({},opt.headers||{},token?{'X-MConnect-Token':token}:{});const r=await fetch(path,opt);if(r.status===401&&retry){lockSession();if(await ensureAuth('Your protected session expired. Enter the PIN again.'))return protectedApi(path,opt,false)}return r}
function ensureAuth(reason){if(token)return Promise.resolve(true);$('#pinReason').textContent=reason||'Enter the same PIN used by the MellorsOS lock screen.';$('#pinError').textContent='';$('#pinInput').value='';$('#pinModal').classList.remove('hidden');setTimeout(()=>$('#pinInput').focus(),50);return new Promise(resolve=>pinResolve=resolve)}
async function submitPin(){const p=$('#pinInput').value.trim();if(!/^\d{4}$/.test(p)){return $('#pinError').textContent='Enter a 4-digit PIN.'}$('#pinError').textContent='Checking PIN…';try{const r=await fetch('/api/auth',{method:'POST',headers:{'Content-Type':'text/plain'},body:p});if(r.status===409){$('#pinError').textContent='Set a lock PIN on the MellorsOS device first.';return}if(!r.ok){$('#pinError').textContent='Incorrect PIN.';return}const j=await r.json();token=j.token;device=j;$('#lockSession').classList.remove('hidden');$('#pinModal').classList.add('hidden');const done=pinResolve;pinResolve=null;if(done)done(true)}catch(e){$('#pinError').textContent='Could not reach the MellorsOS device.'}}
$('#pinSubmit').onclick=submitPin;$('#pinInput').addEventListener('keydown',e=>{if(e.key==='Enter')submitPin()});$('#pinCancel').onclick=()=>{$('#pinModal').classList.add('hidden');const done=pinResolve;pinResolve=null;if(done)done(false)};
async function loadDevice(){try{const r=await publicApi('/api/device');if(!r.ok)throw new Error();device=await r.json();$('#deviceName').textContent=(device.hostname||'MellorsOS')+' is ready';$('#deviceInfo').textContent='MellorsOS '+device.version+' • '+device.ip+' • '+device.hostname+'.local'+(device.pinEnabled?'':' • Lock PIN not set')}catch(e){$('#deviceInfo').textContent='Could not read device information.'}}
$$('.service').forEach(e=>e.onclick=async()=>{const id=e.dataset.open;if(id==='store'){show('store');loadStore()}else if(id==='settings'){if(await ensureAuth('Enter the PIN to open Remote Settings.')){show('settings');renderSettingsSections()}}else if(id==='files'){if(await ensureAuth('Enter the PIN to access the filesystem.')){show('files');loadFiles('/')}}else{show('sideload');loadSideload()}});$$('.back').forEach(e=>e.onclick=()=>show('home'));
function parseCatalog(text){return text.split(/\r?\n/).map(x=>x.trim()).filter(Boolean).map(line=>{const p=line.split('|');return p.length>=5?{id:p[0],name:p[1],version:p[2],description:p[3],url:p.slice(4).join('|')}:null}).filter(Boolean)}
async function loadStore(){const box=$('#storeApps');box.textContent='Loading catalog from GitHub…';try{const r=await fetch(STORE_URL,{cache:'no-store'});if(!r.ok)throw new Error('GitHub HTTP '+r.status);storeCatalog=parseCatalog(await r.text());renderStore()}catch(e){box.textContent='Could not load the Mellors Store: '+e.message}}
function renderStore(){const box=$('#storeApps'),q=$('#storeSearch').value.trim().toLowerCase();box.innerHTML='';const rows=storeCatalog.filter(a=>!q||a.name.toLowerCase().includes(q)||a.id.toLowerCase().includes(q)||a.description.toLowerCase().includes(q));if(!rows.length){box.textContent=q?'No apps match your search.':'The store is empty.';return}for(const a of rows){const d=document.createElement('div');d.className='app';const h=document.createElement('strong');h.textContent=a.name+'  v'+a.version;const p=document.createElement('p');p.textContent=a.description;const b=document.createElement('button');b.textContent='Install';const prog=document.createElement('div');prog.className='progress hidden';prog.innerHTML='<div></div>';const pt=document.createElement('div');pt.className='progressText';b.onclick=()=>installStore(a,b,prog,pt);d.append(h,p,b,prog,pt);box.appendChild(d)}}
$('#storeSearch').addEventListener('input',renderStore);$('#refreshStore').onclick=loadStore;
function setProgress(box,textEl,pct,text){box.classList.remove('hidden');box.firstElementChild.style.width=Math.max(0,Math.min(100,pct))+'%';textEl.textContent=text}
async function downloadWithProgress(url,onProgress){const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error('GitHub HTTP '+r.status);const total=Number(r.headers.get('content-length')||0);if(!r.body){const text=await r.text();onProgress(1);return text}const reader=r.body.getReader(),chunks=[];let got=0;while(true){const {done,value}=await reader.read();if(done)break;chunks.push(value);got+=value.length;onProgress(total?got/total:Math.min(.95,got/65536))}let size=chunks.reduce((n,c)=>n+c.length,0),all=new Uint8Array(size),off=0;for(const c of chunks){all.set(c,off);off+=c.length}onProgress(1);return new TextDecoder().decode(all)}
function uploadSource(path,source,onProgress){return new Promise((resolve,reject)=>{const x=new XMLHttpRequest();x.open('POST',path);x.setRequestHeader('Content-Type','text/plain');if(token)x.setRequestHeader('X-MConnect-Token',token);x.upload.onprogress=e=>{if(e.lengthComputable)onProgress(e.loaded/e.total)};x.onload=()=>resolve({status:x.status,ok:x.status>=200&&x.status<300,text:x.responseText});x.onerror=()=>reject(new Error('Device upload failed'));x.send(source)})}
async function installStore(app,button,prog,pt){if(!/^https:\/\/raw\.githubusercontent\.com\//i.test(app.url)){setProgress(prog,pt,0,'Blocked non-GitHub app URL');return}if(!(await ensureAuth('Enter the PIN to install '+app.name+'.')))return;button.disabled=true;try{setProgress(prog,pt,2,'Downloading app…');const source=await downloadWithProgress(app.url,p=>setProgress(prog,pt,5+p*45,'Downloading '+Math.round(p*100)+'%'));setProgress(prog,pt,52,'Sending to MellorsOS…');let result=await uploadSource('/api/store-install',source,p=>setProgress(prog,pt,52+p*43,'Installing '+Math.round(p*100)+'%'));if(result.status===401){lockSession();if(!(await ensureAuth('Enter the PIN again to finish installing '+app.name+'.')))throw new Error('PIN required');result=await uploadSource('/api/store-install',source,p=>setProgress(prog,pt,52+p*43,'Installing '+Math.round(p*100)+'%'))}if(result.status===507)throw new Error('DEVICE STORAGE FULL — '+result.text);if(!result.ok)throw new Error(result.text||('HTTP '+result.status));setProgress(prog,pt,100,'Installed');setTimeout(()=>prog.classList.add('hidden'),1600)}catch(e){setProgress(prog,pt,100,'Install failed: '+e.message)}finally{button.disabled=false}}

const generalSchema={
'Customization':[['accent','Accent color','color'],['accent2','Secondary accent','color'],['wallpaper','Wallpaper','select',[['aurora','Aurora'],['ocean','Ocean'],['sunset','Sunset'],['forest','Forest'],['mono','Monochrome']]],['gridCols','Home columns','select',[['1','1'],['2','2'],['3','3']]],['iconSize','Icon size','select',[['16','16 px'],['24','24 px'],['32','32 px']]],['iconShape','Icon shape','select',[['square','Square'],['squircle','Squircle'],['round','Rounded'],['circle','Circle']]],['showAppLabels','Show app labels','checkbox'],['homeTitle','Home title','text'],['homeSubtitle','Home subtitle','text'],['username','User name','text'],['fontScale','Font scale','number',{min:.85,max:1.45,step:.05}],['compact','Compact UI','checkbox']],
'Accessibility':[['highContrast','High contrast','checkbox'],['reduceMotion','Reduce motion','checkbox'],['reduceTransparency','Reduce transparency','checkbox'],['largeTargets','Large targets','checkbox'],['boldText','Bold text','checkbox'],['haptics','Haptics','checkbox']],
'Notifications':[['notificationToasts','Top-bar banners','checkbox'],['notificationPreview','Show previews','checkbox'],['notificationLimit','History limit','number',{min:5,max:100,step:5}]],
'Status & navigation':[['showSeconds','Show seconds','checkbox'],['clock24h','24-hour clock','checkbox'],['showBattery','Show battery','checkbox'],['showStatusIcons','Show status icons','checkbox'],['navCompact','Compact navigation','checkbox']],
'Sound':[['silentMode','Silent mode','checkbox'],['volume','Volume','number',{min:0,max:100,step:1}]],
'Developer options':[['developerMode','Developer mode','checkbox'],['showFPS','FPS overlay','checkbox']]
};
const settingSections=[['Customization','Wallpaper, Home, colors'],['Accessibility','Text, contrast, motion'],['Notifications','Top bar, previews, history'],['Status & navigation','Clock and status'],['Wi-Fi','Networks and saved Wi-Fi'],['Sound','Volume and silent mode'],['Lock screen','4-digit device PIN'],['App Manager','Permissions, Home, data'],['Storage & privacy','LittleFS and local data'],['Developer options','MScript tools'],['System','About and reset']];
function renderSettingsSections(){$('#settingsPanel').classList.add('hidden');$('#settingsHome').classList.add('hidden');const box=$('#settingsSections');box.classList.remove('hidden');box.innerHTML='';for(const [name,desc] of settingSections){const d=document.createElement('div');d.className='card sectionCard';d.innerHTML='<h3></h3><p class="muted"></p>';d.querySelector('h3').textContent=name;d.querySelector('p').textContent=desc;d.onclick=()=>openSettingsSection(name);box.appendChild(d)}}
$('#settingsHome').onclick=renderSettingsSections;
function settingControl(k,type,meta,value){let input;if(type==='select'){input=document.createElement('select');for(const item of meta||[]){const o=document.createElement('option');o.value=item[0];o.textContent=item[1];o.selected=String(value)===String(item[0]);input.appendChild(o)}}else{input=document.createElement('input');input.type=type;if(type==='checkbox')input.checked=!!value;else input.value=value??'';if(type==='number'&&meta){input.min=meta.min;input.max=meta.max;input.step=meta.step}}input.dataset.key=k;input.onchange=()=>saveSetting(input);return input}
async function loadGeneralSection(name){const r=await protectedApi('/api/settings?section='+encodeURIComponent(name));if(!r.ok)throw new Error(await r.text());const values=await r.json(),box=$('#settingsPanel');box.innerHTML='<h3></h3>';box.querySelector('h3').textContent=name;for(const [k,label,type,meta] of generalSchema[name]){const row=document.createElement('div');row.className='setting';const l=document.createElement('label');l.textContent=label;row.append(l,settingControl(k,type,meta,values[k]));box.appendChild(row)}}
async function saveSetting(input){if(input.checkValidity&&!input.checkValidity()){setSave('Choose a valid value',true,2200);return}const k=input.dataset.key,v=input.type==='checkbox'?(input.checked?'true':'false'):input.value;input.disabled=true;setSave('Saving…');try{const b=new URLSearchParams({key:k,value:v});const r=await protectedApi('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});const t=await r.text();if(!r.ok)throw new Error(t);setSave('Saved',false,1000);if(k==='developerMode'&&device)device.developerMode=input.checked}catch(e){setSave('Save failed: '+e.message,true,2600)}finally{input.disabled=false}}
async function openSettingsSection(name){$('#settingsSections').classList.add('hidden');$('#settingsPanel').classList.remove('hidden');$('#settingsHome').classList.remove('hidden');$('#settingsPanel').innerHTML='<p class="muted">Loading '+name+'…</p>';try{if(generalSchema[name])await loadGeneralSection(name);else if(name==='Wi-Fi')await loadWifiSection();else if(name==='Lock screen')renderLockSection();else if(name==='App Manager')await loadApps();else if(name==='Storage & privacy')await loadStorage();else if(name==='System')await loadSystem();}catch(e){$('#settingsPanel').innerHTML='<p class="bad">Could not load '+name+': '+e.message+'</p>'}}
async function postForm(path,obj){const b=new URLSearchParams(obj);const r=await protectedApi(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));return t}
async function loadWifiSection(){const panel=$('#settingsPanel');const r=await protectedApi('/api/settings?section=Wi-Fi');if(!r.ok)throw new Error(await r.text());const s=await r.json();panel.innerHTML='<h3>Wi-Fi</h3>';for(const [k,label,type] of [['wifiHostname','Hostname','text'],['wifiEnabled','Wi-Fi enabled','checkbox']]){const row=document.createElement('div');row.className='setting';const l=document.createElement('label');l.textContent=label;row.append(l,settingControl(k,type,null,s[k]));panel.appendChild(row)}const row=document.createElement('div');row.className='row';const scan=document.createElement('button');scan.className='secondary';scan.textContent='Scan networks';scan.onclick=scanWifi;const saved=document.createElement('button');saved.className='secondary';saved.textContent='Saved networks';saved.onclick=loadSavedWifi;row.append(scan,saved);panel.appendChild(row);const box=document.createElement('div');box.id='wifiNetworks';panel.appendChild(box);await loadSavedWifi()}
async function loadSavedWifi(){const box=$('#wifiNetworks');if(!box)return;const r=await protectedApi('/api/wifi/saved');if(!r.ok)throw new Error(await r.text());const rows=await r.json();box.innerHTML=rows.length?'<p class="muted">Saved networks</p>':'<p class="muted">No saved networks.</p>';for(const n of rows){const d=document.createElement('div');d.className='app';const h=document.createElement('strong');h.textContent=n.ssid;const p=document.createElement('p');p.textContent=n.security+(n.username?' • '+n.username:'');const b=document.createElement('button');b.className='secondary';b.textContent='Forget';b.onclick=async()=>{if(!confirm('Forget '+n.ssid+'?'))return;try{await postForm('/api/wifi/forget',{ssid:n.ssid});setSave('Saved',false,900);loadSavedWifi()}catch(e){setSave(e.message,true,2200)}};d.append(h,p,b);box.appendChild(d)}}
async function scanWifi(){const box=$('#wifiNetworks');box.textContent='Scanning…';try{const r=await protectedApi('/api/wifi/scan');if(!r.ok)throw new Error(await r.text());const nets=await r.json();box.innerHTML='';if(!nets.length){box.textContent='No networks found.';return}for(const n of nets){const d=document.createElement('div');d.className='app';const h=document.createElement('strong');h.textContent=n.ssid+'  '+n.rssi+' dBm';const p=document.createElement('p');p.textContent=n.label;const b=document.createElement('button');b.textContent='Connect';b.onclick=()=>connectWifi(n);d.append(h,p,b);box.appendChild(d)}}catch(e){box.textContent='Wi-Fi scan failed: '+e.message}}
function connectWifi(n){
  const box=$('#wifiNetworks');box.innerHTML='';
  const d=document.createElement('div');d.className='app';
  const h=document.createElement('h3');h.textContent='Connect to '+n.ssid;
  const p=document.createElement('p');p.className='muted';p.textContent='Security: '+n.label;
  d.append(h,p);
  let user=null,pass=null;
  if(n.security==='enterprise'){
    const ul=document.createElement('label');ul.textContent='Username';user=document.createElement('input');user.type='text';user.autocomplete='username';user.placeholder='Network username';
    const pl=document.createElement('label');pl.textContent='Password';pass=document.createElement('input');pass.type='password';pass.autocomplete='current-password';pass.placeholder='Network password';
    d.append(ul,user,pl,pass);
  }else if(n.security==='personal'){
    const pl=document.createElement('label');pl.textContent='Password';pass=document.createElement('input');pass.type='password';pass.autocomplete='current-password';pass.placeholder='Network password';d.append(pl,pass);
  }else{
    const note=document.createElement('p');note.className='muted';note.textContent='This is an open network. No username or password is required.';d.append(note);
  }
  const row=document.createElement('div');row.className='row';
  const go=document.createElement('button');go.textContent='Save & connect';
  const cancel=document.createElement('button');cancel.className='secondary';cancel.textContent='Back to networks';cancel.onclick=scanWifi;
  go.onclick=async()=>{
    const username=user?user.value.trim():'',password=pass?pass.value:'';
    if(n.security==='enterprise'&&(!username||!password)){setSave('Username and password are required',true,2200);return}
    if(n.security==='personal'&&!password){setSave('Password is required',true,2200);return}
    go.disabled=true;setSave('Saving…');
    try{await postForm('/api/wifi/connect',{ssid:n.ssid,security:n.security,username,password});setSave('Saved. Connecting in the background…',false,1800)}
    catch(e){setSave('Save failed: '+e.message,true,2600);go.disabled=false}
  };
  row.append(go,cancel);d.append(row);box.appendChild(d);
  setTimeout(()=>{if(user)user.focus();else if(pass)pass.focus()},40);
}
function renderLockSection(){const p=$('#settingsPanel');p.innerHTML='<h3>Lock screen</h3><p class="muted">The PIN protects Remote Settings and app installation. Store browsing remains available without it.</p><label>New 4-digit PIN</label><input id="newPin" inputmode="numeric" maxlength="4" placeholder="1234"><div class="row"><button id="changePin">Change PIN</button><button id="disablePin" class="danger">Disable lock PIN</button></div>';$('#changePin').onclick=changePin;$('#disablePin').onclick=disablePin}
async function changePin(){const p=$('#newPin').value.trim();if(!/^\d{4}$/.test(p))return setSave('Enter a 4-digit PIN',true,2200);const b=new URLSearchParams({pin:p});const r=await protectedApi('/api/pin',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});if(!r.ok)return setSave(await r.text(),true,2200);lockSession();setSave('PIN changed',false,1300)}
async function disablePin(){if(!confirm('Disable the device lock PIN? Settings and installs will be unavailable through MConnect until a new PIN is set on-device.'))return;const r=await protectedApi('/api/pin',{method:'DELETE'});if(r.ok){lockSession();setSave('PIN disabled',false,1300)}else setSave(await r.text(),true,2200)}
const PERMS=['Settings','Notifications','Install apps','Modify permissions','All files','Background','Superuser','Network','Microphone'];
async function appAct(id,action,extra={}){try{await postForm('/api/app',Object.assign({id,action},extra));setSave('Saved',false,900);loadApps()}catch(e){setSave(e.message,true,2200)}}
async function loadApps(){const box=$('#settingsPanel');box.innerHTML='<h3>App Manager</h3><p class="muted">Loading apps…</p>';const r=await protectedApi('/api/apps');if(!r.ok)throw new Error(await r.text());const rows=await r.json();box.innerHTML='<h3>App Manager</h3>';for(const a of rows){const d=document.createElement('div');d.className='app';const h=document.createElement('strong');h.textContent=a.name+'  v'+a.version;const p=document.createElement('p');p.textContent=(a.system?'Built-in system app':a.dynamic?'Installed app':'App')+(a.hidden?' • hidden from Home':'')+(a.superuser?' • Superuser holder':'');const controls=document.createElement('div');controls.className='row';const btn=(label,fn,cls='secondary')=>{const b=document.createElement('button');b.textContent=label;b.className=cls;b.onclick=fn;controls.appendChild(b)};btn(a.hidden?'Show on Home':'Hide from Home',()=>appAct(a.id,a.hidden?'show':'hide'));btn('Move earlier',()=>appAct(a.id,'earlier'));btn('Move later',()=>appAct(a.id,'later'));btn('Erase data',()=>{if(confirm('Erase data for '+a.name+'?'))appAct(a.id,'eraseData')});if(a.dynamic)btn('Uninstall',()=>{if(confirm('Uninstall '+a.name+'?'))appAct(a.id,'uninstall')},'danger');d.append(h,p,controls);if(!a.system){for(let i=0;i<PERMS.length;i++){const row=document.createElement('label');row.style.display='block';row.style.margin='6px 0';const cb=document.createElement('input');cb.type='checkbox';cb.style.width='auto';cb.style.margin='0 8px 0 0';cb.checked=!!a.permissions[i];cb.disabled=i===6||a.superuser;cb.onchange=()=>appAct(a.id,'permission',{permission:String(i),value:cb.checked?'true':'false'});row.append(cb,document.createTextNode(PERMS[i]+(i===6?' (app-controlled)':'')));d.appendChild(row)}}box.appendChild(d)}}
async function loadStorage(){const p=$('#settingsPanel');const r=await protectedApi('/api/storage');if(!r.ok)throw new Error(await r.text());const j=await r.json();p.innerHTML='<h3>Storage & privacy</h3><p class="muted">LittleFS '+j.used+' / '+j.total+' bytes • '+j.apps+' apps</p><button id="formatStorage" class="danger">Format filesystem</button>';$('#formatStorage').onclick=async()=>{if(!confirm('Format filesystem? Installed apps and files will be erased.'))return;await protectedApi('/api/storage/format',{method:'POST'});setSave('Filesystem format started',false,1600)}}
async function loadSystem(){const p=$('#settingsPanel');const r=await protectedApi('/api/system');if(!r.ok)throw new Error(await r.text());const j=await r.json();p.innerHTML='<h3>System</h3><p class="muted"></p><div class="row"><button id="resetSettings" class="secondary">Reset settings</button><button id="factoryReset" class="danger">Factory reset</button><button id="reboot" class="secondary">Restart device</button></div>';p.querySelector('p').textContent=`${j.os} ${j.version} • ${j.engine} • Wi-Fi: ${j.wifi} • IP: ${j.ip} • Heap: ${j.heap} B • PSRAM: ${j.psram} B • Storage: ${j.storageUsed}/${j.storageTotal} B`;$('#resetSettings').onclick=()=>deviceAction('resetSettings');$('#factoryReset').onclick=()=>deviceAction('factoryReset');$('#reboot').onclick=()=>deviceAction('reboot')}
async function deviceAction(name){if(!confirm(name==='factoryReset'?'Factory reset MellorsOS?':name==='resetSettings'?'Reset settings to defaults?':'Restart MellorsOS?'))return;const b=new URLSearchParams({action:name});const r=await protectedApi('/api/action',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});if(r.ok){lockSession();setSave(name==='reboot'?'Restarting…':'Action started',false,1600)}else setSave(await r.text(),true,2200)}
async function loadSideload(){await loadDevice();$('#sideNote').textContent=device&&device.developerMode?'Developer Mode is enabled. Choose an app; the PIN will be requested when you install.':'Developer Mode is OFF. You may choose a file, but installation is blocked until Developer Mode is enabled in Remote Settings.'}
$('#installFile').onclick=async()=>{const f=$('#appFile').files[0];if(!f)return setSave('Choose a .mapp or .txt file',true,2200);if(!/\.(mapp|txt)$/i.test(f.name))return setSave('Only .mapp or .txt files are accepted',true,2200);if(!(await ensureAuth('Enter the PIN to sideload '+f.name+'.')))return;const prog=$('#sideProgress'),pt=$('#sideProgressText');try{const source=await f.text();setProgress(prog,pt,5,'Reading file…');let result=await uploadSource('/api/sideload',source,p=>setProgress(prog,pt,10+p*85,'Installing '+Math.round(p*100)+'%'));if(result.status===401){lockSession();if(!(await ensureAuth('Enter the PIN again to finish sideloading.')))throw new Error('PIN required');result=await uploadSource('/api/sideload',source,p=>setProgress(prog,pt,10+p*85,'Installing '+Math.round(p*100)+'%'))}if(result.status===507)throw new Error('DEVICE STORAGE FULL — '+result.text);if(!result.ok)throw new Error(result.text);setProgress(prog,pt,100,'Installed')}catch(e){setProgress(prog,pt,100,'Install failed: '+e.message)}};
let fsDir='/',fsNext=-1,fsEditing='';
const fsJoin=(p,n)=>(p==='/'?'':p)+'/'+n;
async function fsRequest(path,opt={}){const r=await protectedApi(path,opt);if(!r.ok)throw new Error(await r.text()||('HTTP '+r.status));return r}
function fsError(e){$('#fsStatus').textContent=e.message}
async function loadFiles(path=fsDir,offset=0){try{const r=await fsRequest('/api/fs/list?path='+encodeURIComponent(path)+'&offset='+offset),j=await r.json();fsDir=j.path;$('#fsPath').value=fsDir;if(!offset)$('#fsEntries').replaceChildren();for(const f of j.entries){const row=document.createElement('div');row.className='app';const label=document.createElement('strong');label.textContent=(f.directory?'📁 ':'')+f.path.split('/').pop()+(f.directory?'':' ('+f.size+' B)');row.append(label);const actions=document.createElement('div');actions.className='row';const button=(name,run)=>{const b=document.createElement('button');b.className='secondary';b.textContent=name;b.onclick=()=>Promise.resolve().then(run).catch(fsError);actions.append(b)};if(f.directory)button('Open',()=>loadFiles(f.path));else{button('Download',async()=>{const r=await fsRequest('/api/fs/file?path='+encodeURIComponent(f.path));const url=URL.createObjectURL(await r.blob());const a=document.createElement('a');a.href=url;a.download=f.path.split('/').pop();a.click();setTimeout(()=>URL.revokeObjectURL(url),30000)});button('Edit text',async()=>{const r=await fsRequest('/api/fs/file?path='+encodeURIComponent(f.path));const bytes=new Uint8Array(await r.arrayBuffer());if(bytes.includes(0))throw new Error('This appears to be binary. Use download and upload instead.');let text;try{text=new TextDecoder('utf-8',{fatal:true}).decode(bytes)}catch{throw new Error('This file is not UTF-8 text. Use download and upload.')}fsEditing=f.path;$('#fsText').value=text;$('#fsEditTitle').textContent=f.path;$('#fsEditor').classList.remove('hidden')})}button('Rename / move',async()=>{const to=prompt('New full path:',f.path);if(to===null||to===f.path)return;await fsRequest('/api/fs/rename?path='+encodeURIComponent(f.path)+'&to='+encodeURIComponent(to),{method:'POST'});await loadFiles()});button('Delete',async()=>{if(!confirm('Delete '+f.path+'? Folders must be empty.'))return;await fsRequest('/api/fs/file?path='+encodeURIComponent(f.path),{method:'DELETE'});await loadFiles()});row.append(actions);$('#fsEntries').append(row)}fsNext=j.next;$('#fsMore').classList.toggle('hidden',fsNext<0);$('#fsStatus').textContent='Folder: '+fsDir}catch(e){fsError(e)}}
async function fsWrite(path,file){const form=new FormData();form.append('file',file,file.name||'upload');await fsRequest('/api/fs/upload?path='+encodeURIComponent(path),{method:'POST',body:form})}
$('#fsGo').onclick=()=>loadFiles($('#fsPath').value);$('#fsRefresh').onclick=()=>loadFiles();$('#fsMore').onclick=()=>loadFiles(fsDir,fsNext);$('#fsUp').onclick=()=>loadFiles(fsDir.slice(0,fsDir.lastIndexOf('/'))||'/');
$('#fsNewFolder').onclick=async()=>{try{const n=prompt('Folder name:');if(!n)return;await fsRequest('/api/fs/mkdir?path='+encodeURIComponent(fsJoin(fsDir,n)),{method:'POST'});await loadFiles()}catch(e){fsError(e)}};
$('#fsNewText').onclick=()=>{const n=prompt('New text filename:','untitled.txt');if(!n)return;fsEditing=fsJoin(fsDir,n);$('#fsText').value='';$('#fsEditTitle').textContent=fsEditing;$('#fsEditor').classList.remove('hidden')};
$('#fsUpload').onchange=async e=>{try{for(const f of e.target.files){const path=fsJoin(fsDir,f.name);const listing=await fsRequest('/api/fs/file?path='+encodeURIComponent(path)).catch(()=>null);if(listing&&!confirm('Replace '+path+'?'))continue;$('#fsStatus').textContent='Uploading '+f.name;await fsWrite(path,f)}await loadFiles()}catch(err){fsError(err)}finally{e.target.value=''}};
$('#fsSave').onclick=async()=>{try{await fsWrite(fsEditing,new File([$('#fsText').value],fsEditing.split('/').pop(),{type:'text/plain;charset=utf-8'}));$('#fsStatus').textContent='Saved '+fsEditing;await loadFiles()}catch(e){fsError(e)}};$('#fsClose').onclick=()=>$('#fsEditor').classList.add('hidden');

loadDevice();
</script></body></html>
)MCONNECT";

class MConnectServer{
  SettingsStore&settings;Registry&apps;WifiManager&wifi;Permissions&perms;DataStore&data;WebServer server;bool active=false,routesReady=false,requested=false;
  File fsUpload;String fsUploadPath,fsUploadTemp;bool fsUploadOK=false;
  struct Session{bool used=false;String token;IPAddress ip;unsigned long seen=0;}sessions[4];
  static String jsonEscape(String s){s.replace("\\","\\\\");s.replace("\"","\\\"");s.replace("\r","\\r");s.replace("\n","\\n");return s;}
  String tokenFor(){return hex8(esp_random())+hex8(esp_random())+hex8(esp_random())+hex8(esp_random());}
  void invalidateAll(){for(auto&s:sessions){s.used=false;s.token="";s.seen=0;}}
  void expire(){unsigned long now=millis();for(auto&s:sessions)if(s.used&&(unsigned long)(now-s.seen)>1800000){s.used=false;s.token="";}}
  Session*session(bool touch=true){String tok=server.header("X-MConnect-Token");if(!tok.length())return nullptr;IPAddress remote=server.client().remoteIP();expire();for(auto&s:sessions)if(s.used&&s.token==tok&&s.ip==remote){if(touch)s.seen=millis();return&s;}return nullptr;}
  void headers(){server.sendHeader("Access-Control-Allow-Origin","*");server.sendHeader("Access-Control-Allow-Methods","GET,POST,DELETE,OPTIONS");server.sendHeader("Access-Control-Allow-Headers","Content-Type,X-MConnect-Token");server.sendHeader("Cache-Control","no-store");}
  void sendText(int code,const String&text){headers();server.send(code,"text/plain; charset=utf-8",text);}void sendJson(int code,const String&text){headers();server.send(code,"application/json",text);}bool requireSession(){if(session())return true;sendText(401,"PIN session required");return false;}
  String deviceJson(const String&tok=""){String j=String("{\"version\":\"")+OS_VERSION+"\",\"ip\":\""+WiFi.localIP().toString()+"\",\"hostname\":\""+jsonEscape(settings.wifiHostname())+"\",\"developerMode\":"+(settings.developerMode()?"true":"false")+",\"pinEnabled\":"+(settings.pinEnabled()?"true":"false");if(tok.length())j+=",\"token\":\""+tok+"\"";j+="}";return j;}
  void addSettingJson(String&j,bool&first,const char*k,const Value&v){if(!first)j+=",";first=false;j+="\""+String(k)+"\":";if(v.type==Value::BOOLEAN)j+=(v.boolean?"true":"false");else if(v.type==Value::NUMBER)j+=String(v.number,4);else j+="\""+jsonEscape(v.str())+"\"";}
  String settingsSectionJson(String section){String j="{";bool first=true;auto add=[&](const char*k){addSettingJson(j,first,k,settings.get(k));};
    if(section=="Customization"){const char*ks[]={"accent","accent2","wallpaper","gridCols","iconSize","iconShape","showAppLabels","homeTitle","homeSubtitle","username","fontScale","compact"};for(auto k:ks)add(k);}
    else if(section=="Accessibility"){const char*ks[]={"highContrast","reduceMotion","reduceTransparency","largeTargets","boldText","haptics"};for(auto k:ks)add(k);}
    else if(section=="Notifications"){const char*ks[]={"notificationToasts","notificationPreview","notificationLimit"};for(auto k:ks)add(k);}
    else if(section=="Status & navigation"){const char*ks[]={"showSeconds","clock24h","showBattery","showStatusIcons","navCompact"};for(auto k:ks)add(k);}
    else if(section=="Wi-Fi"){add("wifiHostname");add("wifiEnabled");}
    else if(section=="Sound"){add("silentMode");add("volume");}
    else if(section=="Developer options"){add("developerMode");add("showFPS");}
    j+="}";return j;}
  static bool oneOf(const String&v,const char*const*items,int count){for(int i=0;i<count;i++)if(v==items[i])return true;return false;}static bool strictNumber(const String&v,double&out){if(!v.length())return false;char*end=nullptr;out=strtod(v.c_str(),&end);return end&&end!=v.c_str()&&*end=='\0'&&isfinite(out);}static bool hexColor(const String&v){if(v.length()!=7||v[0]!='#')return false;for(int i=1;i<7;i++)if(!isxdigit((unsigned char)v[i]))return false;return true;}static bool boolValue(const String&v){return v=="true"||v=="false"||v=="1"||v=="0";}
  bool validateRemoteSetting(const String&key,const String&value,String&why){const char*boolKeys[]={"showAppLabels","compact","highContrast","reduceMotion","reduceTransparency","largeTargets","boldText","haptics","notificationToasts","notificationPreview","showSeconds","clock24h","showBattery","showStatusIcons","navCompact","wifiEnabled","silentMode","developerMode","showFPS"};for(const char*k:boolKeys)if(key==k){if(!boolValue(value)){why="Expected true or false";return false;}return true;}if(key=="accent"||key=="accent2"){if(!hexColor(value)){why="Expected a #RRGGBB color";return false;}return true;}if(key=="wallpaper"){const char*vals[]={"aurora","ocean","sunset","forest","mono"};if(!oneOf(value,vals,5)){why="Unsupported wallpaper";return false;}return true;}if(key=="iconShape"){const char*vals[]={"square","squircle","round","circle"};if(!oneOf(value,vals,4)){why="Unsupported icon shape";return false;}return true;}double n=0;if(key=="gridCols"){if(!strictNumber(value,n)||(int)n!=n||n<1||n>3){why="Home columns must be 1, 2, or 3";return false;}return true;}if(key=="iconSize"){if(!strictNumber(value,n)||!((int)n==16||(int)n==24||(int)n==32)){why="Icon size must be 16, 24, or 32";return false;}return true;}if(key=="fontScale"){if(!strictNumber(value,n)||n<0.85||n>1.45){why="Font scale must be between 0.85 and 1.45";return false;}return true;}if(key=="notificationLimit"){if(!strictNumber(value,n)||(int)n!=n||n<5||n>100||((int)n%5)!=0){why="Notification limit must be 5-100 in steps of 5";return false;}return true;}if(key=="volume"){if(!strictNumber(value,n)||(int)n!=n||n<0||n>100){why="Volume must be 0-100";return false;}return true;}return true;}
  void applySetting(const String&key,const String&value){settings.set(key,Value(value));if(key=="wifiHostname")wifi.setHostname(settings.wifiHostname());else if(key=="wifiEnabled"&&settings.wifiEnabled())wifi.startBackground(settings.lastWifi());}
  String appsJson(){String j="[";for(int i=0;i<apps.count();i++){App&a=apps.at(i);if(i)j+=",";j+="{\"id\":\""+jsonEscape(a.meta.id)+"\",\"name\":\""+jsonEscape(a.meta.name)+"\",\"version\":\""+jsonEscape(a.meta.version)+"\",\"system\":"+(a.system?"true":"false")+",\"dynamic\":"+(a.dynamic?"true":"false")+",\"developer\":"+(a.developer?"true":"false")+",\"hidden\":"+(apps.hidden(a.meta.id)?"true":"false")+",\"superuser\":"+(perms.holder()==a.meta.id?"true":"false")+",\"permissions\":[";for(int q=0;q<PERM_COUNT;q++){if(q)j+=",";j+=(perms.has(a,q)?"true":"false");}j+="]}";}j+="]";return j;}
  String savedWifiJson(){String j="[";for(int i=0;i<wifi.count();i++){if(i)j+=",";j+="{\"ssid\":\""+jsonEscape(wifi.ssid(i))+"\",\"security\":\""+jsonEscape(wifiSecurityName(wifi.security(i)))+"\",\"username\":\""+jsonEscape(wifi.username(i))+"\"}";}j+="]";return j;}
  String scanWifiJson(){wifi.beginForegroundScan();int n=WiFi.scanNetworks();String j="[";for(int i=0;i<n;i++){if(i)j+=",";wifi_auth_mode_t auth=WiFi.encryptionType(i);uint8_t sec=wifiSecurityClass(auth);String type=sec==WIFI_SECURITY_OPEN?"open":sec==WIFI_SECURITY_ENTERPRISE?"enterprise":"personal";j+="{\"ssid\":\""+jsonEscape(WiFi.SSID(i))+"\",\"rssi\":"+String(WiFi.RSSI(i))+",\"security\":\""+type+"\",\"label\":\""+jsonEscape(wifiSecurityLabel(auth))+"\"}";}WiFi.scanDelete();wifi.endForegroundScan();j+="]";return j;}
  String storageJson(){return String("{\"used\":")+String(LittleFS.usedBytes())+",\"total\":"+String(LittleFS.totalBytes())+",\"apps\":"+String(apps.count())+"}";}String systemJson(){return String("{\"os\":\"")+OS_NAME+"\",\"version\":\""+OS_VERSION+"\",\"engine\":\""+ENGINE_VERSION+"\",\"wifi\":\""+jsonEscape(WiFi.status()==WL_CONNECTED?WiFi.SSID():String("Disconnected"))+"\",\"ip\":\""+WiFi.localIP().toString()+"\",\"heap\":"+String(ESP.getFreeHeap())+",\"psram\":"+String(ESP.getFreePsram())+",\"storageUsed\":"+String(LittleFS.usedBytes())+",\"storageTotal\":"+String(LittleFS.totalBytes())+"}";}
  bool installSource(bool developerOnly){if(developerOnly&&!settings.developerMode()){sendText(403,"Developer Mode is required for app sideloading");return false;}String src=server.arg("plain");if(!src.length()){sendText(400,"No app source was received");return false;}String why;if(!apps.install(src,why)){if(why.startsWith("STORAGE_FULL|")){sendText(507,why.substring(13));return false;}sendText(400,String("MScript validation/install failed: ")+why);return false;}sendText(200,"Installed");return true;}
  String directoryJson(String path){
    File dir=LittleFS.open(path);if(!dir||!dir.isDirectory())return "";
    int offset=max(0,(int)server.arg("offset").toInt()),seen=0,shown=0;String j="{\"path\":\""+jsonEscape(path)+"\",\"entries\":[";
    File f=dir.openNextFile();while(f){if(seen++>=offset&&shown<64){if(shown++)j+=",";j+="{\"name\":\""+jsonEscape(String(f.name()))+"\",\"path\":\""+jsonEscape(String(f.path()))+"\",\"directory\":"+(f.isDirectory()?"true":"false")+",\"size\":"+String(f.size())+"}";}f.close();f=dir.openNextFile();}
    dir.close();j+="],\"next\":"+String(offset+shown<seen?offset+shown:-1)+"}";return j;
  }
  void filesystemRoutes(){
    server.on("/api/fs/list",HTTP_GET,[this](){if(!requireSession())return;String p=normalizedFsPath(server.arg("path"));if(!p.length()){sendText(400,"Absolute path required");return;}String j=directoryJson(p);if(!j.length()){sendText(404,"Folder not found");return;}sendJson(200,j);});
    server.on("/api/fs/file",HTTP_GET,[this](){if(!requireSession())return;String p=normalizedFsPath(server.arg("path"));File f=p.length()?LittleFS.open(p,"r"):File();if(!f||f.isDirectory()){sendText(404,"File not found");return;}headers();server.streamFile(f,"application/octet-stream");f.close();});
    server.on("/api/fs/file",HTTP_DELETE,[this](){if(!requireSession())return;String p=normalizedFsPath(server.arg("path"));if(!p.length()||p=="/"){sendText(400,"File or empty folder path required");return;}bool ok=LittleFS.remove(p)||LittleFS.rmdir(p);sendText(ok?200:409,ok?"Deleted":"Delete failed; folders must be empty");});
    server.on("/api/fs/mkdir",HTTP_POST,[this](){if(!requireSession())return;String p=normalizedFsPath(server.arg("path"));bool ok=p.length()&&ensureDirectories(p);sendText(ok?200:400,ok?"Folder created":"Invalid path or storage unavailable");});
    server.on("/api/fs/rename",HTTP_POST,[this](){if(!requireSession())return;String p=normalizedFsPath(server.arg("path")),to=normalizedFsPath(server.arg("to"));bool ok=p.length()&&to.length()&&p!="/"&&to!="/"&&!LittleFS.exists(to)&&LittleFS.rename(p,to);sendText(ok?200:409,ok?"Renamed":"Rename failed; destination must not exist");});
    server.on("/api/fs/upload",HTTP_POST,[this](){if(!requireSession())return;bool ok=fsUploadOK;fsUploadOK=false;sendText(ok?200:507,ok?"Saved":"Upload failed; previous file preserved");},[this](){
      HTTPUpload&u=server.upload();
      if(u.status==UPLOAD_FILE_START){
        if(fsUpload)fsUpload.close();fsUploadOK=false;fsUploadPath="";fsUploadTemp="";
        if(!session())return;String p=normalizedFsPath(server.arg("path"));if(!p.length()||p=="/")return;
        if(!ensureDirectories(p.substring(0,p.lastIndexOf('/'))))return;
        fsUploadPath=p;fsUploadTemp=stagingPath();if(!fsUploadTemp.length())return;fsUpload=LittleFS.open(fsUploadTemp,"w");fsUploadOK=(bool)fsUpload;
      }else if(u.status==UPLOAD_FILE_WRITE){if(fsUploadOK&&fsUpload.write(u.buf,u.currentSize)!=u.currentSize)fsUploadOK=false;}
      else if(u.status==UPLOAD_FILE_END||u.status==UPLOAD_FILE_ABORTED){
        if(fsUpload)fsUpload.close();if(u.status==UPLOAD_FILE_ABORTED)fsUploadOK=false;
        if(fsUploadOK)fsUploadOK=LittleFS.rename(fsUploadTemp,fsUploadPath);
        if(!fsUploadOK&&fsUploadTemp.length())LittleFS.remove(fsUploadTemp);
      }
    });
  }
  void routes(){if(routesReady)return;const char*keys[]={"X-MConnect-Token"};server.collectHeaders(keys,1);filesystemRoutes();server.on("/",HTTP_GET,[this](){headers();server.send_P(200,"text/html; charset=utf-8",MCONNECT_HTML);});server.on("/index.html",HTTP_GET,[this](){headers();server.send_P(200,"text/html; charset=utf-8",MCONNECT_HTML);});server.on("/favicon.ico",HTTP_GET,[this](){headers();server.send(204,"text/plain","");});
    server.on("/api/device",HTTP_GET,[this](){sendJson(200,deviceJson());});
    server.on("/api/auth",HTTP_POST,[this](){if(!settings.pinEnabled()){sendText(409,"Set a lock PIN on the MellorsOS device first");return;}String pin=server.arg("plain");pin.trim();if(pin.length()!=4||!settings.verifyPin(pin)){sendText(403,"Incorrect PIN");return;}expire();int slot=-1;for(int i=0;i<4;i++)if(!sessions[i].used){slot=i;break;}if(slot<0)slot=0;sessions[slot].used=true;sessions[slot].token=tokenFor();sessions[slot].ip=server.client().remoteIP();sessions[slot].seen=millis();sendJson(200,deviceJson(sessions[slot].token));});
    server.on("/api/logout",HTTP_POST,[this](){Session*s=session(false);if(s){s->used=false;s->token="";}headers();server.send(204,"text/plain","");});
    server.on("/api/settings",HTTP_GET,[this](){if(!requireSession())return;sendJson(200,settingsSectionJson(server.arg("section")));});
    server.on("/api/settings",HTTP_POST,[this](){if(!requireSession())return;String k=server.arg("key"),v=server.arg("value");if(!settings.has(k)||k=="pinEnabled"){sendText(400,"Unknown or read-only setting");return;}String why;if(!validateRemoteSetting(k,v,why)){sendText(400,String("Invalid setting value: ")+why);return;}if(k=="wifiEnabled"&&(v=="false"||v=="0")){settings.wifiEnabled(false);sendText(200,"Saved");wifi.disable();return;}applySetting(k,v);sendText(200,"Saved");});
    server.on("/api/wifi/saved",HTTP_GET,[this](){if(!requireSession())return;sendJson(200,savedWifiJson());});server.on("/api/wifi/scan",HTTP_GET,[this](){if(!requireSession())return;if(!settings.wifiEnabled()){sendText(409,"Wi-Fi is disabled");return;}sendJson(200,scanWifiJson());});
    server.on("/api/wifi/connect",HTTP_POST,[this](){if(!requireSession())return;String ssid=server.arg("ssid"),kind=server.arg("security"),user=server.arg("username"),pw=server.arg("password");ssid.trim();user.trim();if(!ssid.length()){sendText(400,"SSID required");return;}uint8_t sec=kind=="enterprise"?WIFI_SECURITY_ENTERPRISE:kind=="open"?WIFI_SECURITY_OPEN:WIFI_SECURITY_PERSONAL;if(sec==WIFI_SECURITY_ENTERPRISE&&(!user.length()||!pw.length())){sendText(400,"Username and password are required for WPA Enterprise");return;}if(sec==WIFI_SECURITY_PERSONAL&&!pw.length()){sendText(400,"Password required");return;}settings.wifiEnabled(true);settings.lastWifi(ssid);wifi.enable();wifi.scheduleConnect(ssid,sec,user,pw);sendText(202,"Credentials saved. MellorsOS will connect in the background.");});
    server.on("/api/wifi/forget",HTTP_POST,[this](){if(!requireSession())return;String ssid=server.arg("ssid");int ix=wifi.find(ssid);if(ix<0){sendText(404,"Saved network not found");return;}wifi.forget(ix);sendText(200,"Forgot network");});
    server.on("/api/apps",HTTP_GET,[this](){if(!requireSession())return;sendJson(200,appsJson());});server.on("/api/app",HTTP_POST,[this](){if(!requireSession())return;String id=server.arg("id"),action=server.arg("action");int ix=apps.find(id);if(ix<0){sendText(404,"App not found");return;}App&a=apps.at(ix);if(action=="hide"){if(id=="settings"){sendText(403,"Settings cannot be hidden");return;}apps.setHidden(id,true);sendText(200,"Hidden");return;}if(action=="show"){apps.setHidden(id,false);sendText(200,"Shown");return;}if(action=="earlier"||action=="later"){apps.moveHome(id,action=="earlier"?-1:1);sendText(200,"Moved");return;}if(action=="permission"){int pi=server.arg("permission").toInt();bool on=server.arg("value")=="true"||server.arg("value")=="1";if(a.system||pi<0||pi>=PERM_COUNT||pi==6){sendText(403,"That permission cannot be changed remotely");return;}if(!perms.set(a,pi,on)){sendText(409,"Permission change was rejected");return;}sendText(200,"Permission updated");return;}if(action=="eraseData"){data.clear(id);sendText(200,"App data erased");return;}if(action=="uninstall"){if(!a.dynamic){sendText(403,"Built-in apps cannot be uninstalled");return;}if(perms.holder()==id){sendText(403,"Disable Superuser from the app before uninstalling");return;}perms.clearApp(id);data.clear(id);apps.remove(id);sendText(200,"Uninstalled");return;}sendText(400,"Unknown app action");});
    server.on("/api/storage",HTTP_GET,[this](){if(!requireSession())return;sendJson(200,storageJson());});server.on("/api/system",HTTP_GET,[this](){if(!requireSession())return;sendJson(200,systemJson());});server.on("/api/storage/format",HTTP_POST,[this](){if(!requireSession())return;sendText(200,"Formatting filesystem and restarting");delay(80);LittleFS.format();ESP.restart();});
    server.on("/api/pin",HTTP_POST,[this](){if(!requireSession())return;String p=server.arg("pin");if(p.length()!=4){sendText(400,"PIN must contain 4 digits");return;}for(size_t i=0;i<p.length();i++)if(p[i]<'0'||p[i]>'9'){sendText(400,"PIN must contain 4 digits");return;}settings.setPin(p);sendText(200,"PIN changed");invalidateAll();});server.on("/api/pin",HTTP_DELETE,[this](){if(!requireSession())return;sendText(200,"PIN disabled");settings.clearPin();invalidateAll();});
    server.on("/api/store-install",HTTP_POST,[this](){if(!requireSession())return;installSource(false);});server.on("/api/sideload",HTTP_POST,[this](){if(!requireSession())return;installSource(true);});
    server.on("/api/action",HTTP_POST,[this](){if(!requireSession())return;String a=server.arg("action");if(a=="resetSettings"){sendText(200,"Settings reset");settings.reset();invalidateAll();return;}if(a=="factoryReset"){sendText(200,"Factory reset starting");delay(80);LittleFS.format();perms.reset();data.reset();wifi.clearSaved();settings.reset();ESP.restart();return;}if(a=="reboot"){sendText(200,"Restarting");delay(80);ESP.restart();return;}sendText(400,"Unknown action");});
    server.onNotFound([this](){if(server.method()==HTTP_OPTIONS){headers();server.send(204,"text/plain","");return;}if(server.method()==HTTP_GET){headers();server.send_P(200,"text/html; charset=utf-8",MCONNECT_HTML);}else sendText(404,"Not found");});routesReady=true;}
  void start(){if(active||!requested||WiFi.status()!=WL_CONNECTED)return;routes();server.begin();active=true;Serial.println("[MellorsOS] MConnect listening on "+address());}void stop(){if(!active){invalidateAll();return;}server.stop();active=false;invalidateAll();Serial.println("[MellorsOS] MConnect stopped");}
  void service(){bool wanted=requested&&WiFi.status()==WL_CONNECTED;if(wanted&&!active)start();if(!wanted&&active)stop();if(!active)return;expire();for(int i=0;i<4;i++){server.handleClient();yield();}}
public:MConnectServer(SettingsStore&s,Registry&a,WifiManager&w,Permissions&p,DataStore&d):settings(s),apps(a),wifi(w),perms(p),data(d),server(80){}void setRequested(bool on){if(requested==on)return;requested=on;if(!requested)stop();}bool running()const{return active;}String address()const{return WiFi.status()==WL_CONNECTED?String("http://")+WiFi.localIP().toString()+"/":String("Wi-Fi disconnected");}String localAddress()const{return settings.wifiHostname().length()?String("http://")+settings.wifiHostname()+".local/":String("");}void tick(){service();}
};


struct UIElement{enum Type{UI_TEXT,UI_SECTION,UI_CARD,UI_MENU,UI_BUTTON,UI_INPUT,UI_TOGGLE,UI_SLIDER,UI_SELECT,UI_LABEL,UI_PROGRESS,UI_DIVIDER,UI_SPACER}type=UI_TEXT;String id,title,body,value,options;double number=0,minValue=0,maxValue=100,step=1;bool state=false,visible=true,enabled=true,password=false,filePicker=false;int height=22;};
class AppView{
public:String title,subtitle;UIElement e[MAX_UI];int n=0,selected=0,scroll=0;void clear(){title="";subtitle="";n=0;selected=0;scroll=0;}UIElement*add(UIElement::Type t){if(n>=MAX_UI)return nullptr;e[n]=UIElement();e[n].type=t;return&e[n++];}int find(String id){for(int i=0;i<n;i++)if(e[i].id==id)return i;return-1;}
};

struct Notification{uint32_t id=0;String title,body,app;unsigned long time=0;};
struct TimerEntry{bool active=false,background=false,repeating=false;int app=-1;String name,event;unsigned long due=0,period=0;};
struct GameEntity{bool used=false;String id,type,text,color;float x=0,y=0,w=0,h=0,vx=0,vy=0,rotation=0,opacity=1;bool visible=true;};
struct RuntimeState{Vars vars;MemoryStore memory;AppView view;bool initialized=false;bool canvasMode=false;};

class Kernel; // forward

class MScript{
  friend class Expr;
  Display&d;Modal&modal;Keyboard&kbd;SettingsStore&settings;Registry&apps;Permissions&perms;DataStore&data;WifiManager&wifi;Kernel*kernel=nullptr;RuntimeState* states=nullptr;TimerEntry timers[MAX_TIMERS];Notification notes[MAX_NOTIFICATIONS];int noteCount=0;uint32_t noteSerial=0;int current=-1;String sourceCache;App*app=nullptr;RuntimeState*state=nullptr;bool returnFlag=false,breakFlag=false,continueFlag=false,inSystemExecute=false;Value returnValue;String currentEvent;
  struct Fn{String name;String args;int start=0,end=0;};
  struct Ev{String name;int start=0,end=0;};
  struct CodeLine{String text;int end=-1,elseAt=-1;};
  Fn* funcs=nullptr;int fnCount=0,fnCap=0;
  Ev* events=nullptr;int evCount=0,evCap=0;
  CodeLine* lines=nullptr;int lineCount=0,lineCap=0;
  WiFiServer* appServer=nullptr;uint16_t appServerPort=0;int appServerOwner=-1;
  String serverPath,serverMethod,serverBody,serverReply,serverType;
  int serverStatus=200;bool serverPrepared=false;uint32_t serverRequestCount=0;
  WiFiUDP udp;uint16_t udpPort=0;String udpSender;
  GameEntity entities[MAX_ENTITIES];bool gameCreated=false,gameRunning=false;uint32_t gameBg=0;int gameW=128,gameH=128,gameFrame=0;float gameDelta=0,gameTime=0;unsigned long gameLast=0,gamePeriod=33;String gameEvent="gameFrame";
  int functionDepth=0;Value invokeFunction(int ix,Value*args,int argc);Value eval(const String&expr,int start=0);Value call(String name,Value*args,int argc);bool compile(const String&src);void execRange(int from,int to);int blockEnd(int from,int to,int*elseAt=nullptr);void setEventVars(String type,String id="",Value value=Value(),String path="");
  const String& lineAt(int i)const;
  bool ensureLineCapacity(int need);bool ensureFnCapacity(int need);bool ensureEvCapacity(int need);void clearProgramTables();
  bool require(int p);String resolvePath(String path,bool write=false);String fsList(String path,int kind);Value uiCall(String name,Value*args,int argc);Value gameCall(String name,Value*args,int argc);Value fileCall(String name,Value*args,int argc);Value appCall(String name,Value*args,int argc);Value canvasCall(String name,Value*args,int argc);Value serverCall(String name,Value*args,int argc);void notify(String title,String body);
public:
  MScript(Display&dd,Modal&mm,Keyboard&kk,SettingsStore&ss,Registry&aa,Permissions&pp,DataStore&ds,WifiManager&ww):d(dd),modal(mm),kbd(kk),settings(ss),apps(aa),perms(pp),data(ds),wifi(ww){}
  bool beginMemory(){
    if(states)return true;
    size_t bytes=sizeof(RuntimeState)*MAX_APPS;
    void* mem=nullptr;
    if(psramFound())mem=ps_malloc(bytes);
    if(!mem)return false;
    states=(RuntimeState*)mem;
    for(int i=0;i<MAX_APPS;i++)new (&states[i]) RuntimeState();
    return true;
  }
  void attach(Kernel*k){kernel=k;}RuntimeState&rt(int i){return states[i];}AppView&view(){return state->view;}int currentApp(){return current;}App*currentMeta(){return app;}bool rawCanvas()const{return current>=0&&states&&states[current].canvasMode;}bool gameOwnsDisplay()const{return current>=0&&(gameCreated||(states&&states[current].canvasMode));}
  void pollHttpServer();bool launch(int ix);bool resume(int ix);void pause(int ix);void close(int ix);bool isOpen(int ix){return ix>=0&&ix<MAX_APPS&&states&&states[ix].initialized;}Value trigger(int ix,String ev,String id="",Value val=Value(),String path="");void tick();void stopForeground(int appIx);String notificationList();int notifications(){return noteCount;}
  Notification notification(int newest){return newest>=0&&newest<noteCount?notes[noteCount-1-newest]:Notification();}
  void deleteNotification(int newest){int at=noteCount-1-newest;if(at<0||at>=noteCount)return;for(int i=at;i<noteCount-1;i++)notes[i]=notes[i+1];notes[--noteCount]=Notification();G_NOTIFICATION_UNTIL=0;G_NOTIFICATION_DIRTY=false;}
  void deleteNotificationId(uint32_t id){for(int i=0;i<noteCount;i++)if(notes[i].id==id){deleteNotification(noteCount-1-i);return;}}
  void clearNotifications(){for(int i=0;i<noteCount;i++)notes[i]=Notification();noteCount=0;G_NOTIFICATION_UNTIL=0;G_NOTIFICATION_DIRTY=false;}String apiList();
};

class Expr{
  MScript&vm;const String&s;int p=0;Vars&vars;
  void ws(){while(p<(int)s.length()&&isspace((unsigned char)s[p]))p++;}
  bool eat(const char*x){ws();size_t n=strlen(x);if(p+(int)n<=(int)s.length()&&memcmp(s.c_str()+p,x,n)==0){p+=(int)n;return true;}return false;}
  String ident(){ws();int st=p;while(p<(int)s.length()&&(isalnum((unsigned char)s[p])||s[p]=='_'||s[p]=='.'))p++;return s.substring(st,p);}Value primary();Value unary();Value mul();Value add();Value cmp();Value eq();Value land();Value lor();
public:Expr(MScript&v,const String&x,Vars&vv,int start=0):vm(v),s(x),p(start),vars(vv){}Value run(){return lor();}
};

Value Expr::primary(){ws();if(p>=(int)s.length())return Value();if(s[p]=='"'){p++;String o="";while(p<(int)s.length()){char c=s[p++];if(c=='"')break;if(c=='\\'&&p<(int)s.length()){char e=s[p++];if(e=='n')o+='\n';else if(e=='t')o+='\t';else o+=e;}else o+=c;}return Value(o);}if(s[p]=='('){p++;Value v=lor();eat(")");return v;}if(isdigit((unsigned char)s[p])||(s[p]=='.'&&p+1<(int)s.length()&&isdigit((unsigned char)s[p+1]))){int st=p;while(p<(int)s.length()&&(isdigit((unsigned char)s[p])||s[p]=='.'))p++;return Value(s.substring(st,p).toDouble());}String id=ident();if(id=="true")return Value(true);if(id=="false")return Value(false);if(id=="null")return Value();ws();if(p<(int)s.length()&&s[p]=='('){p++;std::unique_ptr<Value[]> storage(new(std::nothrow) Value[10]);if(!storage){Serial.println("[MellorsOS] Expression argument allocation failed");G_HOME_REQUEST=true;return Value();}Value*a=storage.get();int n=0;ws();if(p<(int)s.length()&&s[p]!=')'){while(n<10){a[n++]=lor();ws();if(eat(")"))break;eat(",");}}else eat(")");return vm.call(id,a,n);}return vars.get(id);}
Value Expr::unary(){ws();if(eat("!"))return Value(!unary().truth());if(eat("-"))return Value(-unary().num());return primary();}
Value Expr::mul(){Value v=unary();while(1){if(eat("*"))v=Value(v.num()*unary().num());else if(eat("/")){double b=unary().num();v=Value(b==0?0:v.num()/b);}else if(eat("%")){double b=unary().num();v=Value(b==0?0:fmod(v.num(),b));}else break;}return v;}
Value Expr::add(){Value v=mul();while(1){if(eat("+")){Value b=mul();if(v.type==Value::STRING||b.type==Value::STRING)v=Value(v.str()+b.str());else v=Value(v.num()+b.num());}else if(eat("-"))v=Value(v.num()-mul().num());else break;}return v;}
Value Expr::cmp(){Value v=add();while(1){if(eat(">="))v=Value(v.num()>=add().num());else if(eat("<="))v=Value(v.num()<=add().num());else if(eat(">"))v=Value(v.num()>add().num());else if(eat("<"))v=Value(v.num()<add().num());else break;}return v;}
Value Expr::eq(){Value v=cmp();while(1){if(eat("==")){Value b=cmp();v=Value(v.str()==b.str());}else if(eat("!=")){Value b=cmp();v=Value(v.str()!=b.str());}else break;}return v;}
Value Expr::land(){Value v=eq();while(eat("&&")){Value b=eq();v=Value(v.truth()&&b.truth());}return v;}
Value Expr::lor(){Value v=land();while(eat("||")){Value b=land();v=Value(v.truth()||b.truth());}return v;}

Value MScript::eval(const String&e,int start){Expr p(*this,e,state->vars,start);return p.run();}

const String& MScript::lineAt(int i)const{
  static const String empty="";
  return (i>=0&&i<lineCount&&lines)?lines[i].text:empty;
}
void MScript::clearProgramTables(){
  for(int i=0;i<fnCount;i++){funcs[i].name="";funcs[i].args="";funcs[i].start=funcs[i].end=0;}
  for(int i=0;i<evCount;i++){events[i].name="";events[i].start=events[i].end=0;}
  for(int i=0;i<lineCount;i++){lines[i].text="";lines[i].end=-1;lines[i].elseAt=-1;}
  lineCount=0;fnCount=0;evCount=0;
}
bool MScript::ensureLineCapacity(int need){
  if(need<=lineCap)return true;
  int cap=lineCap?lineCap:128;while(cap<need){if(cap>0x3fffffff/2)return false;cap*=2;}
  CodeLine*p=new(std::nothrow) CodeLine[cap];if(!p)return false;
  for(int i=0;i<lineCount;i++)p[i]=lines[i];
  delete[] lines;lines=p;lineCap=cap;return true;
}
bool MScript::ensureFnCapacity(int need){
  if(need<=fnCap)return true;int cap=fnCap?fnCap:8;while(cap<need)cap*=2;Fn*p=new(std::nothrow) Fn[cap];if(!p)return false;
  for(int i=0;i<fnCount;i++)p[i]=funcs[i];delete[] funcs;funcs=p;fnCap=cap;return true;
}
bool MScript::ensureEvCapacity(int need){
  if(need<=evCap)return true;int cap=evCap?evCap:8;while(cap<need)cap*=2;Ev*p=new(std::nothrow) Ev[cap];if(!p)return false;
  for(int i=0;i<evCount;i++)p[i]=events[i];delete[] events;events=p;evCap=cap;return true;
}

bool MScript::compile(const String&src){
  String syntaxWhy;
  if(!validateMScriptSyntax(src,syntaxWhy)){
    Serial.println("[MellorsOS] MScript syntax rejected: "+syntaxWhy);
    clearProgramTables();return false;
  }
  clearProgramTables();
  int st=0;bool inIcon=false;
  while(st<=(int)src.length()){
    int q=src.indexOf('\n',st);int en=q<0?(int)src.length():q;
    String t=src.substring(st,en);t.replace("\r","");t.trim();
    if(t=="icon16")inIcon=true;
    else if(t=="endicon")inIcon=false;
    else if(!inIcon&&t.length()&&!t.startsWith("#")&&!t.startsWith("app ")&&!t.startsWith("meta ")){
      if(!ensureLineCapacity(lineCount+1)){
        Serial.println("[MellorsOS] MScript compile failed: not enough memory for executable lines");clearProgramTables();return false;
      }
      lines[lineCount].text=t;lines[lineCount].end=-1;lines[lineCount].elseAt=-1;lineCount++;
    }
    if(q<0)break;st=q+1;
  }

  // Precompute matching end/else locations once. Game-frame execution can then
  // jump over blocks in O(1) instead of rescanning source on every frame.
  int* stack=nullptr;int sp=0;
  if(lineCount){
    size_t bytes=(size_t)lineCount*sizeof(int);
    stack=(int*)(psramFound()?ps_malloc(bytes):malloc(bytes));
    if(!stack){Serial.println("[MellorsOS] MScript compile failed: not enough memory for block map");clearProgramTables();return false;}
  }
  for(int i=0;i<lineCount;i++){
    const String& l=lineAt(i);
    bool opens=l.startsWith("if ")||l.startsWith("while ")||l.startsWith("repeat ")||l.startsWith("for ")||l.startsWith("function ")||l.startsWith("on ");
    if(opens)stack[sp++]=i;
    else if(l=="else"){
      if(sp>0&&lineAt(stack[sp-1]).startsWith("if "))lines[stack[sp-1]].elseAt=i;
    }else if(l=="end"&&sp>0){int op=stack[--sp];lines[op].end=i;}
  }
  if(stack)free(stack);

  for(int i=0;i<lineCount;i++){
    const String& l=lineAt(i);
    if(l.startsWith("function ")){
      if(!ensureFnCapacity(fnCount+1)){Serial.println("[MellorsOS] MScript compile failed: not enough memory for functions");clearProgramTables();return false;}
      String h=l.substring(9);int op=h.indexOf('('),cp=h.lastIndexOf(')');
      funcs[fnCount].name=op>=0?h.substring(0,op):h;funcs[fnCount].name.trim();
      funcs[fnCount].args=(op>=0&&cp>op)?h.substring(op+1,cp):"";
      int e=lines[i].end>=0?lines[i].end:blockEnd(i+1,lineCount);funcs[fnCount].start=i+1;funcs[fnCount].end=e-1;fnCount++;i=e;continue;
    }
    if(l.startsWith("on ")){
      if(!ensureEvCapacity(evCount+1)){Serial.println("[MellorsOS] MScript compile failed: not enough memory for events");clearProgramTables();return false;}
      events[evCount].name=l.substring(3);events[evCount].name.trim();
      int e=lines[i].end>=0?lines[i].end:blockEnd(i+1,lineCount);events[evCount].start=i+1;events[evCount].end=e-1;evCount++;i=e;continue;
    }
  }
  return true;
}
int MScript::blockEnd(int from,int to,int*elseAt){
  if(elseAt)*elseAt=-1;int op=from-1;
  if(op>=0&&op<lineCount&&lines&&lines[op].end>=0){if(elseAt)*elseAt=lines[op].elseAt;return lines[op].end;}
  int depth=0;for(int i=from;i<to;i++){const String& l=lineAt(i);bool opens=l.startsWith("if ")||l.startsWith("while ")||l.startsWith("repeat ")||l.startsWith("for ")||l.startsWith("function ")||l.startsWith("on ");if(opens)depth++;else if(l=="end"){if(depth==0)return i;depth--;}else if(l=="else"&&depth==0&&elseAt)*elseAt=i;}return to-1;
}
void MScript::execRange(int from,int to){
  int watchdogCounter=0;
  for(int i=from;i<=to&&i<lineCount;i++){
    if((++watchdogCounter&511)==0)yield();
    const String&l=lineAt(i);if(!l.length())continue;
    if(l.startsWith("let ")||l.startsWith("set ")){
      int eq=l.indexOf('=',4);
      if(eq>4){String n=l.substring(4,eq);n.trim();state->vars.set(n,eval(l,eq+1));}
      continue;
    }
    if(l.startsWith("if ")){
      int en=(lines&&lines[i].end>=0)?lines[i].end:blockEnd(i+1,to+1);
      int els=(lines?lines[i].elseAt:-1);
      if(eval(l,3).truth()){int e=els>=0?els-1:en-1;execRange(i+1,e);}
      else if(els>=0)execRange(els+1,en-1);
      if(returnFlag||breakFlag||continueFlag)return;i=en;continue;
    }
    if(l.startsWith("while ")){
      int en=(lines&&lines[i].end>=0)?lines[i].end:blockEnd(i+1,to+1);int loops=0;
      while(eval(l,6).truth()&&loops++<1000){execRange(i+1,en-1);if(returnFlag)return;if(breakFlag){breakFlag=false;break;}if(continueFlag)continueFlag=false;}
      i=en;continue;
    }
    if(l.startsWith("repeat ")){
      int en=(lines&&lines[i].end>=0)?lines[i].end:blockEnd(i+1,to+1);
      int c=constrain((int)eval(l,7).num(),0,1000);
      for(int k=0;k<c;k++){state->vars.set("index",Value(k));execRange(i+1,en-1);if(returnFlag)return;if(breakFlag){breakFlag=false;break;}if(continueFlag)continueFlag=false;}
      i=en;continue;
    }
    if(l.startsWith("for ")){
      int en=(lines&&lines[i].end>=0)?lines[i].end:blockEnd(i+1,to+1);String h=l.substring(4);int f=h.indexOf(" from "),t=h.indexOf(" to ");
      if(f>0&&t>f){String name=h.substring(0,f);double a=eval(h.substring(f+6,t)).num(),b=eval(h.substring(t+4)).num();int step=a<=b?1:-1;int loops=0;for(double v=a;(step>0?v<=b:v>=b)&&loops++<1000;v+=step){state->vars.set(name,Value(v));execRange(i+1,en-1);if(returnFlag)return;if(breakFlag){breakFlag=false;break;}if(continueFlag)continueFlag=false;}}
      i=en;continue;
    }
    if(l=="break"){breakFlag=true;return;}
    if(l=="continue"){continueFlag=true;return;}
    if(l=="return"){returnFlag=true;returnValue=Value();return;}
    if(l.startsWith("return ")){returnFlag=true;returnValue=eval(l,7);return;}
    eval(l);if(returnFlag||breakFlag||continueFlag)return;
  }
}

bool MScript::launch(int ix){
  if(ix<0||ix>=apps.count())return false;
  current=ix;app=&apps.at(ix);state=&states[ix];String tmp;sourceCache=apps.source(ix,tmp);if(!sourceCache.length())return false;
  gameCreated=false;gameRunning=false;gameFrame=0;gameDelta=0;gameTime=0;for(auto&e:entities)e.used=false;
  if(!compile(sourceCache)){modal.msg("MScript Error","This app could not be loaded. The device may be out of runtime memory.");return false;}state->view.clear();state->vars.clear();state->memory.clear();state->canvasMode=false;state->initialized=true;trigger(ix,"start");trigger(ix,"resume");Serial.printf("[MellorsOS] app launched; kernel stack free watermark=%u bytes\n",(unsigned)uxTaskGetStackHighWaterMark(nullptr));return true;
}
bool MScript::resume(int ix){
  if(ix<0||ix>=apps.count())return false;if(!states[ix].initialized)return launch(ix);
  current=ix;app=&apps.at(ix);state=&states[ix];String tmp;sourceCache=apps.source(ix,tmp);if(!sourceCache.length())return false;
  if(!compile(sourceCache)){modal.msg("MScript Error","This app could not be resumed. The device may be out of runtime memory.");return false;}trigger(ix,"resume");return true;
}
void MScript::pause(int ix){if(ix<0||ix>=apps.count()||!states[ix].initialized)return;trigger(ix,"pause");stopForeground(ix);}
void MScript::close(int ix){
  if(ix<0||ix>=apps.count()||!states[ix].initialized)return;
  trigger(ix,"close");for(auto&t:timers)if(t.active&&t.app==ix)t.active=false;
  if(appServerOwner==ix&&appServer){appServer->stop();delete appServer;appServer=nullptr;appServerOwner=-1;appServerPort=0;}
  if(udpPort){udp.stop();udpPort=0;}
  states[ix].view.clear();states[ix].vars.clear();states[ix].memory.clear();states[ix].canvasMode=false;states[ix].initialized=false;
  if(current==ix){gameRunning=false;gameCreated=false;current=-1;app=nullptr;state=nullptr;sourceCache="";clearProgramTables();}
}
void MScript::setEventVars(String type,String id,Value value,String path){state->vars.set("event.type",type);state->vars.set("event.id",id);state->vars.set("event.value",value);state->vars.set("event.path",path);state->vars.set("event.key",value);state->vars.set("event.fileName",path.length()?safeName(path):"");state->vars.set("event.delta",Value(gameDelta));state->vars.set("event.frame",Value(gameFrame));state->vars.set("event.appId",app?Value(app->meta.id):Value(""));state->vars.set("event.permission",Value(""));state->vars.set("event.hidden",Value(false));state->vars.set("event.x",Value(0));state->vars.set("event.y",Value(0));state->vars.set("event.time",Value((double)millis()));state->vars.set("event.width",Value(128));state->vars.set("event.height",Value(128));state->vars.set("event.sender",Value(id));}
Value MScript::trigger(int ix,String ev,String id,Value val,String path){
  if(ix<0||ix>=apps.count())return Value();
  int old=current;App*oa=app;RuntimeState*os=state;
  bool sameContext=old==ix&&oa==&apps.at(ix)&&os==&states[ix]&&sourceCache.length();
  current=ix;app=&apps.at(ix);state=&states[ix];
  if(!sameContext){String tmp;sourceCache=apps.source(ix,tmp);if(!sourceCache.length()||!compile(sourceCache)){current=old;app=oa;state=os;if(old>=0&&oa){String restore;sourceCache=apps.source(old,restore);compile(sourceCache);}else sourceCache="";return Value();}}
  setEventVars(ev,id,val,path);String exact=id.length()?ev+" "+id:ev;int chosen=-1,generic=-1;
  for(int i=0;i<evCount;i++){if(events[i].name==exact){chosen=i;break;}if(events[i].name==ev)generic=i;}
  if(chosen<0)chosen=generic;returnFlag=breakFlag=continueFlag=false;returnValue=Value();if(chosen>=0)execRange(events[chosen].start,events[chosen].end);Value out=returnValue;
  current=old;app=oa;state=os;
  if(!sameContext){if(old>=0&&oa){String restore;sourceCache=apps.source(old,restore);compile(sourceCache);}else{sourceCache="";clearProgramTables();}}
  return out;
}

bool MScript::require(int p){if(perms.has(*app,p))return true;modal.msg("MScript crash",app->meta.name,String("Missing permission: ")+PERM_LABELS[p]);G_HOME_REQUEST=true;return false;}
void MScript::notify(String title,String body){if(noteCount>=MAX_NOTIFICATIONS){for(int i=1;i<noteCount;i++)notes[i-1]=notes[i];noteCount--;}notes[noteCount].id=++noteSerial;notes[noteCount].title=title;notes[noteCount].body=body;notes[noteCount].app=app?app->meta.id:String("system");notes[noteCount].time=millis();noteCount++;if(settings.notificationToasts()){G_NOTIFICATION_TITLE=title;G_NOTIFICATION_BODY=settings.notificationPreview()?body:String("");G_NOTIFICATION_UNTIL=millis()+3200;G_NOTIFICATION_DIRTY=true;}if(!settings.silentMode()&&settings.volume()>0)quietBeep(84,90);}
String MScript::notificationList(){String s="";for(int i=noteCount-1;i>=0;i--){if(s.length())s+='|';s+=notes[i].title+": "+(settings.notificationPreview()?notes[i].body:"Preview hidden");}return s;}

String MScript::resolvePath(String path,bool write){
  String base="/apps/"+safeName(app->meta.id);String legacy="/apps/"+hex8(hash32(app->meta.id));ensureFolder("/apps");
  if(!LittleFS.exists(base)&&LittleFS.exists(legacy))LittleFS.rename(legacy,base);ensureFolder(base);
  bool global=app->system||perms.has(*app,4)||perms.has(*app,6);
  String p=normalizedFsPath(path.length()?(path.startsWith("/")&&global?path:base+(path.startsWith("/")?String(""):String("/"))+path):base);
  if(!publicFilePath(p))return "";if(!global&&p!=base&&!p.startsWith(base+"/"))return "";return p;
}
String MScript::fsList(String path,int kind){String p=resolvePath(path);File dir=LittleFS.open(p);if(!dir||!dir.isDirectory())return"";String out="";File f=dir.openNextFile();while(f){bool ok=publicFilePath(f.path())&&(kind==0||(kind==1&&!f.isDirectory())||(kind==2&&f.isDirectory()));if(ok){if(out.length())out+='|';out+=safeName(f.name());}f=dir.openNextFile();}return out;}
Value MScript::fileCall(String name,Value*a,int n){if(name=="file.root")return resolvePath("");if(name=="file.resolve")return resolvePath(n?a[0].str():"");if(name=="file.read"){String p=resolvePath(n?a[0].str():"");File f=LittleFS.open(p,"r");if(!f)return n>1?a[1]:Value("");String s=f.readString();f.close();return s;}if(name=="file.write"||name=="file.append"){if(n<2)return false;String p=resolvePath(a[0].str(),true);int slash=p.lastIndexOf('/');if(slash>0){String dir=p.substring(0,slash);if(!LittleFS.exists(dir))LittleFS.mkdir(dir);}String content=a[1].str();bool saved=false;if(name.endsWith("append")){File f=LittleFS.open(p,"a");if(f){saved=f.print(content)==content.length();f.close();}}else saved=atomicWriteFile(p,(const uint8_t*)content.c_str(),content.length());if(!saved)return false;trigger(current,"fileChange","",Value(name.endsWith("append")?"append":"write"),p);return p;}if(name=="file.delete"){if(!n)return false;String p=resolvePath(a[0].str(),true);bool ok=LittleFS.remove(p);if(!ok)ok=LittleFS.rmdir(p);return ok;}if(name=="file.exists")return n&&LittleFS.exists(resolvePath(a[0].str()));if(name=="file.mkdir"){if(!n)return false;String p=resolvePath(a[0].str(),true);LittleFS.mkdir(p);return p;}if(name=="file.list")return fsList(n?a[0].str():"",0);if(name=="file.listFiles")return fsList(n?a[0].str():"",1);if(name=="file.listFolders")return fsList(n?a[0].str():"",2);if(name=="file.listPaths"){String dirPath=resolvePath(n?a[0].str():"");File dir=LittleFS.open(dirPath);if(!dir||!dir.isDirectory())return "";String out="";File f=dir.openNextFile();while(f){if(publicFilePath(f.path())){if(out.length())out+='|';out+=f.path();}f.close();f=dir.openNextFile();}return out;}if(name=="file.name"){String p=resolvePath(n?a[0].str():"");int q=p.lastIndexOf('/');return p.substring(q+1);}if(name=="file.extension"){String nm=fileCall("file.name",a,n).str();int q=nm.lastIndexOf('.');return q>=0?nm.substring(q+1):"";}if(name=="file.parent"){String p=resolvePath(n?a[0].str():"");int q=p.lastIndexOf('/');return q>0?p.substring(0,q):String("/");}if(name=="file.size"){File f=LittleFS.open(resolvePath(n?a[0].str():""));return f?(double)f.size():0;}if(name=="file.isFile"){File f=LittleFS.open(resolvePath(n?a[0].str():""));return f&&!f.isDirectory();}if(name=="file.isFolder"){File f=LittleFS.open(resolvePath(n?a[0].str():""));return f&&f.isDirectory();}if(name=="file.mime")return "text/plain";if(name=="file.modified")return 0;if(name=="file.copy"||name=="file.move"){if(n<2)return false;String from=resolvePath(a[0].str()),to=resolvePath(a[1].str(),true);File fi=LittleFS.open(from,"r"),fo=LittleFS.open(to,"w");if(!fi||!fo)return false;while(fi.available())fo.write(fi.read());fi.close();fo.close();if(name=="file.move")LittleFS.remove(from);return to;}if(name=="file.rename"){if(n<2)return false;String from=resolvePath(a[0].str()),par=fileCall("file.parent",a,1).str(),to=par+"/"+safeName(a[1].str());return LittleFS.rename(from,to)?Value(to):Value(false);}if(name=="file.changeExtension"){if(n<2)return false;String from=resolvePath(a[0].str());int dot=from.lastIndexOf('.');String ext=a[1].str();if(ext.startsWith("."))ext.remove(0,1);String to=(dot>from.lastIndexOf('/')?from.substring(0,dot):from)+(ext.length()?"."+ext:"");return LittleFS.rename(from,to)?Value(to):Value(false);}if(name=="file.export"){if(!n)return false;String from=resolvePath(a[0].str());String dn=n>1&&a[1].str().length()?safeName(a[1].str()):fileCall("file.name",a,1).str();if(!LittleFS.exists("/exports"))LittleFS.mkdir("/exports");if(!LittleFS.exists("/notes"))LittleFS.mkdir("/notes");if(!LittleFS.exists("/messages"))LittleFS.mkdir("/messages");if(!LittleFS.exists("/incoming"))LittleFS.mkdir("/incoming");Value cp[2]={from,String("/exports/")+dn};return fileCall("file.copy",cp,2);}return Value();}

Value MScript::uiCall(String name,Value*a,int n){if(name!="ui.value"&&name!="ui.exists"&&name!="ui.setValue"){state->canvasMode=false;gameCreated=false;gameRunning=false;}AppView&v=state->view;if(name=="ui.clear"){v.clear();return true;}if(name=="ui.header"){v.title=n?a[0].str():"";v.subtitle=n>1?a[1].str():"";return true;}auto add=[&](UIElement::Type t){return v.add(t);};if(name=="ui.section"||name=="ui.text"||name=="ui.code"){auto e=add(name=="ui.section"?UIElement::UI_SECTION:UIElement::UI_TEXT);if(e)e->title=n?a[0].str():"";return true;}if(name=="ui.rich"||name=="ui.card"||name=="ui.alert"||name=="ui.infoRow"){auto e=add(UIElement::UI_CARD);if(e){e->title=n?a[0].str():"";e->body=n>1?a[1].str():"";}return true;}if(name=="ui.divider"){add(UIElement::UI_DIVIDER);return true;}if(name=="ui.spacer"){auto e=add(UIElement::UI_SPACER);if(e)e->height=n?constrain((int)a[0].num(),2,50):10;return true;}if(name=="ui.badge"||name=="ui.stat"){auto e=add(UIElement::UI_TEXT);if(e)e->title=n?a[0].str():"";return true;}if(name=="ui.progress"){auto e=add(UIElement::UI_PROGRESS);if(e){e->title=n?a[0].str():"";e->number=n>1?a[1].num():0;e->maxValue=n>2?a[2].num():100;}return true;}if(name=="ui.label"){auto e=add(UIElement::UI_LABEL);if(e){e->id=n?a[0].str():"";e->title=n>1?a[1].str():"";}return true;}if(name=="ui.button"||name=="ui.menu"){auto e=add(name=="ui.menu"?UIElement::UI_MENU:UIElement::UI_BUTTON);if(e){e->id=n?a[0].str():"";e->title=n>1?a[1].str():"";e->body=n>2?a[2].str():"";}return true;}if(name=="ui.input"||name=="ui.password"||name=="ui.search"||name=="ui.colorInput"||name=="ui.dateInput"||name=="ui.timeInput"||name=="ui.textarea"||name=="ui.codeEditor"||name=="ui.numberInput"){auto e=add(UIElement::UI_INPUT);if(e){e->id=n?a[0].str():"";e->title=n>1?a[1].str():"";e->value=n>2?a[2].str():"";e->password=name=="ui.password";}return true;}if(name=="ui.toggle"){auto e=add(UIElement::UI_TOGGLE);if(e){e->id=n?a[0].str():"";e->title=n>1?a[1].str():"";e->state=n>2?a[2].truth():false;e->body=n>3?a[3].str():"";}return true;}if(name=="ui.slider"){auto e=add(UIElement::UI_SLIDER);if(e){e->id=n?a[0].str():"";e->title=n>1?a[1].str():"";e->number=n>2?a[2].num():0;e->minValue=n>3?a[3].num():0;e->maxValue=n>4?a[4].num():100;e->step=n>5?a[5].num():1;}return true;}if(name=="ui.select"||name=="ui.segmented"||name=="ui.chips"){auto e=add(UIElement::UI_SELECT);if(e){e->id=n?a[0].str():"";e->title=n>1?a[1].str():"";e->value=n>2?a[2].str():"";e->options=n>3?a[3].str():"";}return true;}if(name=="ui.value"){int i=n?v.find(a[0].str()):-1;if(i<0)return Value("");auto&e=v.e[i];if(e.type==UIElement::UI_TOGGLE)return e.state;if(e.type==UIElement::UI_SLIDER)return e.number;return e.value;}if(name=="ui.setValue"){if(n<2)return false;int i=v.find(a[0].str());if(i<0)return false;v.e[i].value=a[1].str();v.e[i].number=a[1].num();v.e[i].state=a[1].truth();return a[1];}if(name=="ui.clearValue"){if(!n)return false;int i=v.find(a[0].str());if(i<0)return false;v.e[i].value="";return true;}if(name=="ui.setText"){if(n<2)return false;int i=v.find(a[0].str());if(i<0)return false;v.e[i].title=a[1].str();return true;}if(name=="ui.setVisible"){if(n<2)return false;int i=v.find(a[0].str());if(i<0)return false;v.e[i].visible=a[1].truth();return true;}if(name=="ui.setEnabled"){if(n<2)return false;int i=v.find(a[0].str());if(i<0)return false;v.e[i].enabled=a[1].truth();return true;}if(name=="ui.remove"){if(!n)return false;int i=v.find(a[0].str());if(i<0)return false;for(int j=i;j<v.n-1;j++)v.e[j]=v.e[j+1];v.n--;return true;}if(name=="ui.exists")return n&&v.find(a[0].str())>=0;if(name=="ui.focus"){if(!n)return false;int i=v.find(a[0].str());if(i>=0)v.selected=i;return i>=0;}if(name=="ui.scrollTop"){v.scroll=0;return true;}if(name=="ui.notificationList"){String list=notificationList();int st=0;while(st<=list.length()){int q=list.indexOf('|',st);String x=q<0?list.substring(st):list.substring(st,q);if(x.length()){auto e=add(UIElement::UI_TEXT);if(e)e->title=x;}if(q<0)break;st=q+1;}return true;}if(name=="ui.appList"){for(int i=0;i<apps.count();i++){auto e=add(UIElement::UI_TEXT);if(e)e->title=apps.at(i).meta.name+" ("+apps.at(i).meta.id+")";}return true;}if(name=="ui.storageView"){auto e=add(UIElement::UI_TEXT);if(e)e->title=String("LittleFS used: ")+LittleFS.usedBytes()+" B";return true;}if(name=="ui.logView"){auto e=add(UIElement::UI_TEXT);if(e)e->title="Logs are available over Serial at 115200.";return true;}if(name=="ui.apiReference"){auto e=add(UIElement::UI_TEXT);if(e)e->title=apiList();return true;}if(name=="ui.wallpaperPicker"||name=="ui.accentPicker"||name=="ui.homeLayoutEditor"||name=="ui.appManager"){auto e=add(UIElement::UI_MENU);if(e){e->id="__settings";e->title="Open Settings";e->body="Managed by MellorsOS";}return true;}if(name=="ui.refreshAppManager")return true;if(name=="ui.image"){auto e=add(UIElement::UI_CARD);if(e){e->title="Image";e->body=n?a[0].str():"";}return true;}if(name=="ui.openWithList"){auto e=add(UIElement::UI_TEXT);if(e)e->title="Open With is handled by Files.";return "";}if(name=="ui.fileInput"){auto e=add(UIElement::UI_MENU);if(e){e->id=n?a[0].str():"file";e->title=n>1?a[1].str():"Choose file";e->body="Choose a file";e->filePicker=true;}return true;}if(name=="ui.fileBrowser"||name=="ui.noteList"){String ls=fsList(n?a[0].str():"",0);auto e=add(UIElement::UI_TEXT);if(e)e->title=ls.length()?ls:"Empty";return true;}if(name=="ui.calculator"){auto e=add(UIElement::UI_TEXT);if(e)e->id="calcDisplay",e->title="0";return true;}if(name=="ui.calcSet"){int i=v.find("calcDisplay");if(i>=0&&n)v.e[i].title=a[0].str();return true;}return Value();}

Value MScript::appCall(String name,Value*a,int n){if(name=="app.current")return app->meta.id;if(name=="app.exists")return n&&apps.find(a[0].str())>=0;if(name=="app.list")return apps.listIds();if(name=="app.name"){int i=n?apps.find(a[0].str()):-1;return i>=0?apps.at(i).meta.name:String("");}if(name=="app.version"){int i=n?apps.find(a[0].str()):-1;return i>=0?apps.at(i).meta.version:String("");}if(name=="app.isSystem"){int i=n?apps.find(a[0].str()):-1;return i>=0&&apps.at(i).system;}if(name=="app.isHidden"){int i=n?apps.find(a[0].str()):-1;return i>=0&&apps.hidden(apps.at(i).meta.id);}if(name=="app.source"){int i=n?apps.find(a[0].str()):-1;String t;return i>=0?apps.source(i,t):String("");}if(name=="app.install"){if(!require(2)||!n)return false;String why;bool ok=apps.install(a[0].str(),why);if(!ok)modal.msg("Install failed",why);return ok;}if(name=="app.remove"){if(!require(2)||!n)return false;int i=apps.find(a[0].str());if(i>=0&&apps.at(i).system)return false;perms.clearApp(a[0].str());data.clear(a[0].str());return apps.remove(a[0].str());}if(name=="app.setHidden"){if(!app->system&&!perms.has(*app,6))return false;if(n>=2)apps.setHidden(a[0].str(),a[1].truth());return true;}if(name=="app.disabledSystems")return "";if(name=="app.restoreSystem")return false;if(name=="app.openers")return apps.listIds();if(name=="app.canOpen")return true;if(name=="app.openFile"){if(n<2)return false;int ix=apps.find(a[0].str());if(ix<0)return false;trigger(ix,"openFile","",Value(),a[1].str());return true;}if(name=="app.preview"){if(!n)return false;String why;String src=a[0].str();AppMeta m;uint16_t ic[256];return manifest(src.c_str(),m,ic,d,why)&&validateMScriptSyntax(src,why);}if(name=="app.home"){/* kernel handles signal through special system call */return call("system.home",nullptr,0);}if(name=="app.open"){if(n&&kernel)return call("system.openApp",a,1);return false;}if(name=="app.close"){G_CLOSE_REQUEST=true;return true;}return Value();}

Value MScript::gameCall(String name,Value*a,int n){
  if(name=="game.text"||name=="game.outlineRect"||name=="game.outlineCircle"||name=="game.pixel"||name=="game.triangle"||name=="game.outlineTriangle")state->canvasMode=true;
if(name=="game.create"){state->canvasMode=true;gameW=SCREEN_W;gameH=SCREEN_H;gameBg=n>2?rgb(a[2].str()):0;gameCreated=true;for(auto&e:entities)e.used=false;return true;}if(name=="game.resize"){if(n>1){gameW=SCREEN_W;gameH=SCREEN_H;}return true;}if(name=="game.start"){gameRunning=true;double fps=n?a[0].num():30;if(fps<1)fps=1;if(fps>60)fps=60;gamePeriod=1000/fps;gameEvent=n>1?a[1].str():"gameFrame";gameLast=millis();return true;}if(name=="game.stop"){gameRunning=false;return true;}if(name=="game.running")return gameRunning;if(name=="game.width")return gameW;if(name=="game.height")return gameH;if(name=="game.frame")return gameFrame;if(name=="game.delta")return gameDelta;if(name=="game.time")return gameTime;if(name=="game.clear"){state->canvasMode=true;gameBg=n?rgb(a[0].str()):gameBg;d.fill(0,0,128,128,gameBg);return true;}if(name=="game.setBackground"){if(n)gameBg=rgb(a[0].str());return String("#")+hex8(gameBg).substring(2);}if(name=="game.rect"){state->canvasMode=true;if(n>=5)canvasRect(d,(int)a[0].num(),(int)a[1].num(),(int)a[2].num(),(int)a[3].num(),rgb(a[4].str()),true);return true;}if(name=="game.line"){state->canvasMode=true;if(n>=5)canvasLine(d,a[0].num(),a[1].num(),a[2].num(),a[3].num(),rgb(a[4].str()));return true;}
if(name=="game.circle"){state->canvasMode=true;if(n>=4)canvasCircle(d,a[0].num(),a[1].num(),a[2].num(),rgb(a[3].str()),true);return true;}
if(name=="game.outlineRect"){if(n>=5)canvasRect(d,a[0].num(),a[1].num(),a[2].num(),a[3].num(),rgb(a[4].str()),false);return true;}
if(name=="game.outlineCircle"){if(n>=4)canvasCircle(d,a[0].num(),a[1].num(),a[2].num(),rgb(a[3].str()),false);return true;}
if(name=="game.pixel"){if(n>=3)canvasPixel(d,a[0].num(),a[1].num(),rgb(a[2].str()));return true;}
if(name=="game.triangle"||name=="game.outlineTriangle"){if(n>=7)canvasTriangle(d,a[0].num(),a[1].num(),a[2].num(),a[3].num(),a[4].num(),a[5].num(),rgb(a[6].str()),name=="game.triangle");return true;}
if(name=="game.text"){if(n>=3)d.text(a[1].num(),a[2].num(),a[0].str(),n>4?rgb(a[4].str()):0xFFFFFF,n>3?constrain((int)a[3].num(),1,3):1,true,128);return true;}if(name=="game.keyDown"){if(!n)return false;String k=a[0].str();InputState s=readInput();if(k=="ArrowLeft")return s.x<0;if(k=="ArrowRight")return s.x>0;if(k=="ArrowUp")return s.y<0;if(k=="ArrowDown")return s.y>0;if(k==" "||k=="a"||k=="A")return s.a||s.select;if(k=="b"||k=="B")return s.b;return false;}if(name=="game.pointerX"||name=="game.pointerY")return 0;if(name=="game.pointerDown")return false;if(name=="game.control"||name=="game.dpad")return true;if(name=="game.controlDown"){if(!n)return false;String id=a[0].str();InputState s=readInput();if(id=="left")return s.x<0;if(id=="right")return s.x>0;if(id=="up")return s.y<0;if(id=="down")return s.y>0;if(id=="a")return s.a||s.select;if(id=="b")return s.b;return false;}if(name=="game.removeControl"||name=="game.clearControls")return true;if(name=="game.entity"||name=="game.circleEntity"||name=="game.textEntity"||name=="game.imageEntity"){if(n<1)return "";for(auto&e:entities)if(!e.used){e.used=true;e.id=a[0].str();e.type=name=="game.circleEntity"?"circle":name=="game.textEntity"?"text":name=="game.imageEntity"?"image":"rect";if(name=="game.textEntity"){e.text=n>1?a[1].str():"";e.x=n>2?a[2].num():0;e.y=n>3?a[3].num():0;e.h=n>4?a[4].num():10;e.color=n>5?a[5].str():"#ffffff";}else{e.x=n>1?a[1].num():0;e.y=n>2?a[2].num():0;e.w=n>3?a[3].num():8;e.h=n>4?a[4].num():e.w;e.color=n>5?a[5].str():"#ffffff";}return e.id;}return "";}auto ent=[&](String id)->GameEntity*{for(auto&e:entities)if(e.used&&e.id==id)return&e;return nullptr;};if(name=="game.removeEntity"){auto e=n?ent(a[0].str()):nullptr;if(!e)return false;e->used=false;return true;}if(name=="game.clearEntities"){for(auto&e:entities)e.used=false;return true;}if(name=="game.entityExists")return n&&ent(a[0].str());if(name=="game.entityCount"){int c=0;for(auto&e:entities)if(e.used)c++;return c;}if(name=="game.entities"){String s="";for(auto&e:entities)if(e.used){if(s.length())s+='|';s+=e.id;}return s;}if(name.startsWith("game.set")||name=="game.move"){if(!n)return false;auto e=ent(a[0].str());if(!e)return false;if(name=="game.setPosition"&&n>=3){e->x=a[1].num();e->y=a[2].num();}else if(name=="game.move"&&n>=3){e->x+=a[1].num();e->y+=a[2].num();}else if(name=="game.setVelocity"&&n>=3){e->vx=a[1].num();e->vy=a[2].num();}else if(name=="game.setSize"&&n>=3){e->w=a[1].num();e->h=a[2].num();}else if(name=="game.setColor"&&n>=2)e->color=a[1].str();else if(name=="game.setRotation"&&n>=2)e->rotation=a[1].num();else if(name=="game.setOpacity"&&n>=2)e->opacity=a[1].num();else if(name=="game.setVisible"&&n>=2)e->visible=a[1].truth();else if(name=="game.setText"&&n>=2)e->text=a[1].str();return true;}if(name=="game.entityX"||name=="game.entityY"||name=="game.velocityX"||name=="game.velocityY"){auto e=n?ent(a[0].str()):nullptr;if(!e)return 0;if(name.endsWith("X"))return name.indexOf("velocity")>=0?e->vx:e->x;return name.indexOf("velocity")>=0?e->vy:e->y;}if(name=="game.update"){float dt=n?a[0].num():gameDelta;for(auto&e:entities)if(e.used){e.x+=e.vx*dt;e.y+=e.vy*dt;}return true;}if(name=="game.draw"){state->canvasMode=true;if(!n||a[0].truth())d.fill(0,0,128,128,gameBg);for(auto&e:entities)if(e.used&&e.visible){if(e.type=="circle"){Value aa[4]={e.x,e.y,e.w/2,e.color};gameCall("game.circle",aa,4);}else if(e.type=="text")d.text(e.x,e.y,e.text,rgb(e.color),1);else canvasRect(d,e.x,e.y,e.w,e.h,rgb(e.color),true);}d.render();return true;}if(name=="game.hit"&&n>=8)return a[0].num()<a[4].num()+a[6].num()&&a[0].num()+a[2].num()>a[4].num()&&a[1].num()<a[5].num()+a[7].num()&&a[1].num()+a[3].num()>a[5].num();if(name=="game.collides"&&n>=2){auto x=ent(a[0].str()),y=ent(a[1].str());if(!x||!y)return false;Value aa[8]={x->x,x->y,x->w,x->h,y->x,y->y,y->w,y->h};return gameCall("game.hit",aa,8);}if(name=="game.contains"&&n>=3){auto e=ent(a[0].str());return e&&a[1].num()>=e->x&&a[1].num()<=e->x+e->w&&a[2].num()>=e->y&&a[2].num()<=e->y+e->h;}if(name=="game.distance"&&n>=4)return hypot(a[2].num()-a[0].num(),a[3].num()-a[1].num());if(name=="game.keepInside"&&n){auto e=ent(a[0].str());if(!e)return false;bool hit=false;if(e->x<0){e->x=0;hit=true;}if(e->y<0){e->y=0;hit=true;}if(e->x+e->w>gameW){e->x=gameW-e->w;hit=true;}if(e->y+e->h>gameH){e->y=gameH-e->h;hit=true;}return hit;}if(name=="game.wrap"&&n){auto e=ent(a[0].str());if(!e)return false;if(e->x>gameW)e->x=-e->w;if(e->x+e->w<0)e->x=gameW;if(e->y>gameH)e->y=-e->h;if(e->y+e->h<0)e->y=gameH;return true;}if(name=="game.preloadImage"||name=="game.drawImage")return false;if(name=="game.save"||name=="game.restore"||name=="game.translate"||name=="game.rotate"||name=="game.scale"||name=="game.opacity"||name=="game.resetTransform")return true;return Value();}

Value MScript::canvasCall(String name,Value*a,int n){
  if(name=="screen.width"||name=="screen.height")return SCREEN_W;
  if(name=="screen.nativeWidth"||name=="screen.nativeHeight")return 128;
  if(name=="screen.present"){d.render();return true;}
  state->canvasMode=true;
  if(name=="screen.clear"){d.resetClip();d.fill(0,0,128,128,n?rgb(a[0].str()):0);return true;}
  if(name=="screen.pixel"&&n>=3){canvasPixel(d,a[0].num(),a[1].num(),rgb(a[2].str()));return true;}
  if(name=="screen.line"&&n>=5){canvasLine(d,a[0].num(),a[1].num(),a[2].num(),a[3].num(),rgb(a[4].str()));return true;}
  if((name=="screen.rect"||name=="screen.outlineRect")&&n>=5){canvasRect(d,a[0].num(),a[1].num(),a[2].num(),a[3].num(),rgb(a[4].str()),name=="screen.rect");return true;}
  if((name=="screen.circle"||name=="screen.outlineCircle")&&n>=4){canvasCircle(d,a[0].num(),a[1].num(),a[2].num(),rgb(a[3].str()),name=="screen.circle");return true;}
  if((name=="screen.triangle"||name=="screen.outlineTriangle")&&n>=7){canvasTriangle(d,a[0].num(),a[1].num(),a[2].num(),a[3].num(),a[4].num(),a[5].num(),rgb(a[6].str()),name=="screen.triangle");return true;}
  if(name=="screen.text"&&n>=3){d.text(a[1].num(),a[2].num(),a[0].str(),n>3?rgb(a[3].str()):0xFFFFFF,n>4?constrain((int)a[4].num(),1,3):1,false,128);return true;}
  return false;
}
Value MScript::serverCall(String name,Value*a,int n){
  if(name=="server.running")return appServer&&appServerOwner==current;
  if(name=="server.port")return appServerPort;
  if(name=="server.requests")return (double)serverRequestCount;
  if(name=="server.path")return serverPath;
  if(name=="server.method")return serverMethod;
  if(name=="server.body")return serverBody;
  if(!require(7))return false;
  if(name=="server.start"){
    if(WiFi.status()!=WL_CONNECTED)return false;
    int port=n?(int)a[0].num():8082;
    if(port<1024||port>65535)return false; // protect system endpoints 80 / 81
    if(appServer&&appServerOwner!=current)return false;
    if(appServer){if(appServerPort==port)return true;appServer->stop();delete appServer;}
    appServer=new(std::nothrow) WiFiServer((uint16_t)port);
    if(!appServer){appServerOwner=-1;return false;}
    appServer->begin();appServer->setNoDelay(true);
    appServerPort=port;appServerOwner=current;serverRequestCount=0;
    return true;
  }
  if(name=="server.stop"){
    if(appServer&&appServerOwner==current){appServer->stop();delete appServer;appServer=nullptr;appServerPort=0;appServerOwner=-1;return true;}return false;
  }
  if(name=="server.respond"){
    if(appServerOwner!=current||n<2)return false;
    serverStatus=constrain((int)a[0].num(),100,599);
    serverReply=a[1].str();if(serverReply.length()>4096)serverReply.remove(4096);
    serverType=n>2?a[2].str():"text/plain; charset=utf-8";
    serverPrepared=true;return true;
  }
  return false;
}
void MScript::pollHttpServer(){
  if(!appServer||WiFi.status()!=WL_CONNECTED||appServerOwner<0||!states||!states[appServerOwner].initialized)return;
  WiFiClient client=appServer->available();if(!client)return;
  client.Stream::setTimeout(500);client.setNoDelay(true);
  // bounded parsing: avoid holding up the OS on slow or malicious clients.
  String head=client.readStringUntil('\n');head.trim();
  int sp=head.indexOf(' '),sp2=sp>=0?head.indexOf(' ',sp+1):-1;
  if(sp<1||sp2<=sp||head.length()>256){client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");client.stop();return;}
  serverMethod=head.substring(0,sp);serverPath=head.substring(sp+1,sp2);
  if(serverPath.length()>160)serverPath.remove(160);
  int contentLen=0;
  for(int i=0;i<32&&client.connected();i++){
    String header=client.readStringUntil('\n');header.trim();if(header.length()==0)break;
    if(header.length()>512)break;
    if(header.startsWith("Content-Length:")||header.startsWith("content-length:"))contentLen=header.substring(15).toInt();
  }
  serverBody="";int want=constrain(contentLen,0,2048);unsigned long until=millis()+1500;
  while((int)serverBody.length()<want&&(long)(until-millis())>0){while(client.available()&&(int)serverBody.length()<want)serverBody+=(char)client.read();if((int)serverBody.length()<want)delay(1);}
  if(contentLen<0||contentLen>2048||(int)serverBody.length()!=want){
    client.print("HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");client.stop();return;
  }
  serverStatus=200;serverReply="Not Found";serverType="text/plain; charset=utf-8";serverPrepared=false;serverRequestCount++;
  int owner=appServerOwner;
  trigger(owner,"httpRequest",serverPath,Value(serverBody),serverPath);
  if(!serverPrepared)serverStatus=404;
  String reason=serverStatus==200?"OK":serverStatus==404?"Not Found":serverStatus==201?"Created":"Response";
  client.print("HTTP/1.1 "+String(serverStatus)+" "+reason+"\r\nContent-Type: "+serverType+"\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: "+String(serverReply.length())+"\r\n\r\n");
  client.print(serverReply);client.flush();client.stop();
}

Value MScript::call(String name,Value*a,int n){
  if(name.startsWith("memory.")){
    if(name=="memory.get")return n?(state->memory.get(a[0].str()).type==Value::NIL&&n>1?a[1]:state->memory.get(a[0].str())):Value();
    if(name=="memory.set"&&n>1){state->memory.set(a[0].str(),a[1]);return a[1];}
    if(name=="memory.clear"){state->memory.clear();return true;}
    return Value();
  }
  if(name.startsWith("screen."))return canvasCall(name,a,n);if(name.startsWith("server."))return serverCall(name,a,n);
  if(name.startsWith("ui."))return uiCall(name,a,n);if(name.startsWith("file."))return fileCall(name,a,n);if(name.startsWith("game."))return gameCall(name,a,n);if(name.startsWith("app."))return appCall(name,a,n);
  if(name=="device.ledAll"){if(n>=3)for(int i=0;i<5;i++)cyber.set_rgb(i,constrain((int)a[0].num(),0,255),constrain((int)a[1].num(),0,255),constrain((int)a[2].num(),0,255));return true;}if(name=="device.light")return cyber.get_light();if(name=="device.roll")return cyber.get_roll();if(name=="device.pitch")return cyber.get_pitch();if(name=="device.angle"){String axis=n?a[0].str():String("roll");axis.toLowerCase();return axis=="pitch"?Value(cyber.get_pitch()):Value(cyber.get_roll());}if(name=="device.gyroX")return cyber.get_gyro_x();if(name=="device.gyroY")return cyber.get_gyro_y();if(name=="device.gyroZ")return cyber.get_gyro_z();if(name=="device.accelX")return cyber.get_acc_x();if(name=="device.accelY")return cyber.get_acc_y();if(name=="device.accelZ")return cyber.get_acc_z();if(name=="device.buttonA")return cyber.get_button_a();if(name=="device.buttonB")return cyber.get_button_b();if(name=="device.joystickX")return cyber.get_joystick_x();if(name=="device.joystickY")return cyber.get_joystick_y();if(name=="device.joystickPressed")return cyber.get_joystick_pressed();
  if(name.startsWith("web.")){
    if(name=="web.connected")return WiFi.status()==WL_CONNECTED;
    if(name=="web.status")return state?state->vars.get("__web.status"):Value(0);
    if(!require(7))return false;
    String method="GET",url="",body="",type="text/plain";
    if(name=="web.get"){if(n<1)return "";url=a[0].str();}
    else if(name=="web.post"){if(n<2)return "";method="POST";url=a[0].str();body=a[1].str();if(n>2)type=a[2].str();}
    else if(name=="web.request"){if(n<2)return "";method=a[0].str();url=a[1].str();if(n>2)body=a[2].str();if(n>3)type=a[3].str();}
    else return Value();
    int status=0;String out=httpRequestRaw(method,url,body,type,status);
    if(state)state->vars.set("__web.status",Value(status));
    return out;
  }
  if(name.startsWith("network.")){
    if(name=="network.online")return WiFi.status()==WL_CONNECTED;
    if(name=="network.hostname")return settings.wifiHostname();
    if(name=="network.address")return WiFi.status()==WL_CONNECTED?String("http://")+WiFi.localIP().toString()+":81":String("");
    if(name=="network.port")return 81;
    if(name=="network.lastSender")return state?state->vars.get("event.sender"):Value("");
    if(name=="network.localIP")return WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("");
    if(name=="network.gateway")return WiFi.status()==WL_CONNECTED?WiFi.gatewayIP().toString():String("");
    if(name=="network.subnet")return WiFi.status()==WL_CONNECTED?WiFi.subnetMask().toString():String("");
    if(name=="network.rssi")return WiFi.status()==WL_CONNECTED?WiFi.RSSI():0;
    if(name=="network.mac")return WiFi.macAddress();
    if(name=="network.udpPort")return udpPort;
    if(name=="network.udpSender")return udpSender;
    if(!require(7))return false;
    if(name=="network.discover"){
      if(WiFi.status()!=WL_CONNECTED||!G_MDNS_STARTED)return "";
      // Same service and scan method as native Messages / PeerDiscovery.
      int count=MDNS.queryService("mellors","tcp");String peers="";
      for(int i=0;i<count&&i<16;i++){String ip=MDNS.IP(i).toString();if(ip==WiFi.localIP().toString()||!MDNS.hostname(i).length())continue;
        if((String("|")+peers+"|").indexOf("|"+ip+"|")>=0)continue;if(peers.length())peers+='|';peers+=ip;
      }return peers;
    }
    if(name=="network.resolve"&&n){IPAddress ip;return WiFi.hostByName(a[0].str().c_str(),ip)?Value(ip.toString()):Value("");}
    if(name=="network.tcpSend"&&n>=3){if(WiFi.status()!=WL_CONNECTED)return false;WiFiClient c;bool ok=c.connect(a[0].str().c_str(),constrain((int)a[1].num(),1,65535),1200);if(ok){c.setNoDelay(true);c.print(a[2].str());c.stop();}return ok;}
    if(name=="network.udpStart"&&n){int p=(int)a[0].num();if(p<1024||p>65535)return false;if(udpPort)udp.stop();udpPort=udp.begin(p)?p:0;return udpPort>0;}
    if(name=="network.udpStop"){udp.stop();udpPort=0;return true;}
    if(name=="network.udpSend"&&n>=3){if(WiFi.status()!=WL_CONNECTED)return false;IPAddress ip;if(!WiFi.hostByName(a[0].str().c_str(),ip))return false;if(!udpPort&&!udp.begin(0))return false;bool ok=udp.beginPacket(ip,(int)a[1].num());if(ok){udp.print(a[2].str());ok=udp.endPacket()==1;}return ok;}
    if(name=="network.udpRead"){int len=udp.parsePacket();if(len<=0)return "";udpSender=udp.remoteIP().toString();String s;while(udp.available()&&s.length()<2048)s+=(char)udp.read();while(udp.available())udp.read();return s;}

    if(name=="network.ping"){
      if(n<1)return "";
      String h=stripPeerHost(a[0].str());if(!h.length())return "";
      if(h.indexOf(':')<0)h+=":81";
      int status=0;String out=httpRequestRaw("GET",String("http://")+h+"/mellors/ping","","",status);
      if(state)state->vars.set("__web.status",Value(status));
      return out;
    }
    if(name=="network.send"){
      if(n<2)return false;
      String h=stripPeerHost(a[0].str());if(!h.length())return false;
      if(h.indexOf(':')<0)h+=":81";
      String from=settings.wifiHostname();
      int status=0;String out=httpRequestRaw("POST",String("http://")+h+"/mellors/message?from="+from,a[1].str(),"text/plain",status);
      if(state)state->vars.set("__web.status",Value(status));
      return status>=200&&status<300;
    }
    if(name=="network.isMellors"){
      if(n<1)return false;
      String h=stripPeerHost(a[0].str());if(!h.length())return false;
      if(h.indexOf(':')<0)h+=":81";
      int status=0;String out=httpRequestRaw("GET",String("http://")+h+"/mellors/ping","","",status);
      if(state)state->vars.set("__web.status",Value(status));
      return status==200&&out.indexOf("\"os\":\"MellorsOS\"")>=0;
    }
  }
  if(name.startsWith("data.")){String id=app->meta.id;if(name=="data.get")return n?data.get(id,a[0].str(),n>1?a[1]:Value("")):Value("");if(name=="data.set")return n>1?data.set(id,a[0].str(),a[1]):Value();if(name=="data.delete")return n&&data.del(id,a[0].str());if(name=="data.keys")return data.keys(id,n?a[0].str():"");if(name=="data.has")return n&&data.has(id,a[0].str());if(name=="data.clear"){data.clear(id);return true;}if(name=="data.count")return data.count(id,n?a[0].str():"");if(name=="data.append")return n>1?data.append(id,a[0].str(),a[1],n>2?a[2].str():""):Value();if(name=="data.increment")return n?data.increment(id,a[0].str(),n>1?a[1].num():1):Value();if(name=="data.globalGet")return n?data.get(app->system||perms.has(*app,6)?"GLOBAL":"SHARED",a[0].str(),n>1?a[1]:Value("")):Value();if(name=="data.globalSet")return n>1?data.set(app->system||perms.has(*app,6)?"GLOBAL":"SHARED",a[0].str(),a[1]):Value();if(name=="data.globalDelete")return n&&data.del(app->system||perms.has(*app,6)?"GLOBAL":"SHARED",a[0].str());if(name=="data.globalKeys")return data.keys(app->system||perms.has(*app,6)?"GLOBAL":"SHARED",n?a[0].str():"");if(name=="data.globalHas")return n&&data.has(app->system||perms.has(*app,6)?"GLOBAL":"SHARED",a[0].str());}
  if(name.startsWith("permission.")){if(name=="permission.ask"){int p=n?permId(a[0].str()):-1;return p>=0&&perms.ask(*app,p);}if(name=="permission.has"){int p=n?permId(a[0].str()):-1;return p>=0&&perms.has(*app,p);}if(name=="permission.set"){if(!app->system){modal.msg("MScript crash",app->meta.name,"permission.set is system-only");G_HOME_REQUEST=true;return false;}if(n>=3){int ix=apps.find(a[0].str()),p=permId(a[1].str());return ix>=0&&perms.set(apps.at(ix),p,a[2].truth());}if(n>=2){int p=permId(a[0].str());return perms.set(*app,p,a[1].truth());}return false;}if(name=="permission.disableSuperuser")return perms.disable(*app);if(name=="permission.name"){int p=n?permId(a[0].str()):-1;return p>=0?String(PERM_LABELS[p]):String("");}if(name=="permission.list")return "settings|notifications|installApps|modifyPermissions|allFiles|background|superuser|network|microphone";if(name=="permission.holder")return perms.holder();if(name=="permission.isSuperuser")return perms.holder()==app->meta.id;}
  if(name.startsWith("settings.")){if(name=="settings.get")return n?settings.get(a[0].str(),n>1?a[1]:Value("")):Value();if(name=="settings.has")return n&&settings.has(a[0].str());if(name=="settings.keys")return settings.keys();if(name=="settings.set"){if(!app->system){modal.msg("MScript crash",app->meta.name,"settings.set is system-only");G_HOME_REQUEST=true;return false;}if(n<2)return false;settings.set(a[0].str(),a[1]);return a[1];}if(name=="settings.toggle"){if(!app->system){modal.msg("MScript crash",app->meta.name,"settings.toggle is system-only");G_HOME_REQUEST=true;return false;}if(!n)return false;Value v=settings.get(a[0].str());Value z(!v.truth());settings.set(a[0].str(),z);return z;}if(name=="settings.reset"){if(!app->system){modal.msg("MScript crash",app->meta.name,"settings.reset is system-only");G_HOME_REQUEST=true;return false;}settings.reset();return true;}}
  if(name.startsWith("notify.")){if(name=="notify.send"){if(!require(1)||n<2)return false;notify(a[0].str(),a[1].str());return true;}if(name=="notify.clear"){if(!require(1))return false;clearNotifications();return true;}if(name=="notify.count")return notifications();if(name=="notify.latestTitle")return noteCount?notes[noteCount-1].title:String("");if(name=="notify.latestBody")return noteCount?notes[noteCount-1].body:String("");if(name=="notify.remove"){if(!require(1)||!n)return false;int ix=(int)a[0].num();if(ix<0||ix>=noteCount)return false;for(int i=ix;i<noteCount-1;i++)notes[i]=notes[i+1];noteCount--;return true;}}
  if(name.startsWith("dialog.")){String t=n?a[0].str():"Message",b=n>1?a[1].str():"",det=n>2?a[2].str():"";if(name=="dialog.alert"){modal.msg(t,b,det);return true;}if(name=="dialog.confirm"||name=="dialog.danger")return modal.confirm(t,b,det,name=="dialog.danger"?"Continue":"Confirm","Cancel");if(name=="dialog.choose")return modal.confirm(t,b,n>4?a[4].str():"",n>2?a[2].str():"Yes",n>3?a[3].str():"No");}
  if(name=="events.emit"){if(n)trigger(current,a[0].str(),"",n>1?a[1]:Value());return true;}
  if(name.startsWith("timer.")||name.startsWith("background.")){bool bg=name.startsWith("background.");String op=name.substring(name.indexOf('.')+1);if(bg&&!perms.has(*app,5)&&op=="start"){if(!perms.ask(*app,5))return false;}if(op=="start"||op=="after"||op=="every"){if(n<3)return false;for(auto&t:timers)if(!t.active){t.active=true;t.background=bg;t.repeating=bg||op=="every";t.app=current;t.name=a[0].str();{unsigned long requested=(unsigned long)a[1].num();unsigned long floorMs=(unsigned long)(bg?250:50);t.period=requested>floorMs?requested:floorMs;}t.due=millis()+t.period;t.event=a[2].str();return true;}return false;}if(op=="stop"){if(!n)return false;for(auto&t:timers)if(t.active&&t.app==current&&t.name==a[0].str())t.active=false;return true;}if(op=="isRunning"){if(!n)return false;for(auto&t:timers)if(t.active&&t.app==current&&t.name==a[0].str())return true;return false;}if(op=="list"){String s="";for(auto&t:timers)if(t.active&&t.app==current&&t.background==bg){if(s.length())s+='|';s+=t.name;}return s;}}
  if(name.startsWith("sound.")){if(name=="sound.beep"){double f=n?a[0].num():440;int dur=n>1?a[1].num():120;if(f<=0||settings.volume()<=0)return false;int midi=constrain((int)round(69+12*log(f/440.0)/log(2.0)),0,127);quietBeep(midi,dur);return true;}if(name=="sound.note"){if(settings.volume()<=0)return false;int midi=n?constrain((int)a[0].num(),0,127):69;int dur=n>1?constrain((int)a[1].num(),1,255):120;if(n>2)cyber.set_instrument(constrain((int)a[2].num(),0,127));quietBeep(midi,dur);return true;}if(name=="sound.instrument"){if(n)cyber.set_instrument(constrain((int)a[0].num(),0,127));return true;}if(name=="sound.play"){if(!n)return false;String path=resolvePath(a[0].str());double appVolume=n>3?a[3].num():1.0;if(appVolume>1)appVolume/=100.0;int vol=constrain((int)round(settings.volume()*constrain(appVolume,0.0,1.0)),0,100);return playPcmWav(path,vol);}if(name=="sound.stop"||name=="sound.pause"){quietSoundStop();return true;}if(name=="sound.resume")return true;if(name=="sound.volume")return (double)settings.volume()/100.0;if(name=="sound.isPlaying")return G_SOUND_ACTIVE_UNTIL!=0||G_PCM_PLAYING;}
  if(name.startsWith("microphone.")){if(!require(8))return false;if(name=="microphone.loudness")return readMicrophoneLoudness();if(name=="microphone.isRecording")return G_MIC_RECORDING;if(name=="microphone.record"){if(!n)return false;String path=resolvePath(a[0].str(),true);if(!path.endsWith(".wav"))path+=".wav";int slash=path.lastIndexOf('/');if(slash>0)ensureFolder(path.substring(0,slash));int sec=n>1?(int)a[1].num():3;return recordMicrophoneWav(path,sec)?Value(path):Value(false);}}
  if(name.startsWith("math.")){if(name=="math.round"){double p=pow(10,n>1?a[1].num():0);return round(a[0].num()*p)/p;}if(name=="math.abs")return fabs(a[0].num());if(name=="math.floor")return floor(a[0].num());if(name=="math.ceil")return ceil(a[0].num());if(name=="math.min")return min(a[0].num(),a[1].num());if(name=="math.max")return max(a[0].num(),a[1].num());if(name=="math.clamp")return max(a[1].num(),min(a[2].num(),a[0].num()));if(name=="math.random"||name=="math.randomInt"){double lo=a[0].num(),hi=a[1].num();if(lo>hi){double t=lo;lo=hi;hi=t;}if(name.endsWith("Int"))return (double)random((long)ceil(lo),(long)floor(hi)+1);return lo+((double)esp_random()/UINT32_MAX)*(hi-lo);}if(name=="math.sqrt")return sqrt(a[0].num());if(name=="math.pow")return pow(a[0].num(),a[1].num());if(name=="math.sin")return sin(a[0].num());if(name=="math.cos")return cos(a[0].num());if(name=="math.tan")return tan(a[0].num());if(name=="math.atan2")return atan2(a[0].num(),a[1].num());if(name=="math.lerp")return a[0].num()+(a[1].num()-a[0].num())*a[2].num();if(name=="math.radians")return a[0].num()*M_PI/180.0;if(name=="math.degrees")return a[0].num()*180.0/M_PI;if(name=="math.eval")return eval(a[0].str());}
  if(name.startsWith("text.")){String s=n?a[0].str():"";if(name=="text.length")return (int)s.length();if(name=="text.upper"){s.toUpperCase();return s;}if(name=="text.lower"){s.toLowerCase();return s;}if(name=="text.trim"){s.trim();return s;}if(name=="text.contains")return n>1&&s.indexOf(a[1].str())>=0;if(name=="text.startsWith")return n>1&&s.startsWith(a[1].str());if(name=="text.endsWith")return n>1&&s.endsWith(a[1].str());if(name=="text.replace"){if(n>2)s.replace(a[1].str(),a[2].str());return s;}if(name=="text.slice"){int st=n>1?a[1].num():0,en=n>2?a[2].num():s.length();return s.substring(st,en);}if(name=="text.indexOf")return n>1?s.indexOf(a[1].str()):-1;if(name=="text.part"){if(n<3)return "";String sep=a[1].str();int want=a[2].num(),st=0;for(int i=0;;i++){int q=s.indexOf(sep,st);if(i==want)return q<0?s.substring(st):s.substring(st,q);if(q<0)break;st=q+sep.length();}return "";}if(name=="text.count"){String sep=n>1?a[1].str():"|";if(!s.length())return 0;int c=1,st=0;while((st=s.indexOf(sep,st))>=0){c++;st+=sep.length();}return c;}if(name=="text.repeat"){int c=n>1?constrain((int)a[1].num(),0,1000):0;String o="";while(c--)o+=s;return o;}}
  if(name.startsWith("value.")){Value v=n?a[0]:Value();if(name=="value.number")return v.num();if(name=="value.string")return v.str();if(name=="value.bool")return v.truth();if(name=="value.type")return v.typeName();}
  if(name.startsWith("json.")){if(name=="json.stringify")return n?a[0].str():"";if(name=="json.parse")return n?a[0]:Value();if(name=="json.get"){if(n<2)return Value();String s=a[0].str(),k=a[1].str();String pat="\""+k+"\"";int p=s.indexOf(pat);if(p<0)return n>2?a[2]:Value("");p=s.indexOf(':',p+pat.length());if(p<0)return n>2?a[2]:Value("");int e=s.indexOf(',',p+1);if(e<0)e=s.indexOf('}',p+1);String v=s.substring(p+1,e<0?s.length():e);v.trim();if(v.startsWith("\"")&&v.endsWith("\""))v=v.substring(1,v.length()-1);return v;}if(name=="json.set"){if(n<3)return "{}";return String("{\"")+a[1].str()+"\":\""+a[2].str()+"\"}";}}
  if(name.startsWith("system.")){if(name=="system.toast"){if(n)modal.msg("MellorsOS",a[0].str());return true;}if(name=="system.log"){if(n)Serial.println("[MScript] "+a[0].str());return true;}if(name=="system.info"){String k=n?a[0].str():"";if(k=="name")return String(OS_NAME);if(k=="version")return String(OS_VERSION);if(k=="engine")return String(ENGINE_VERSION);if(k=="platform")return String("CyberPi ESP32");if(k=="stack")return String((unsigned long)uxTaskGetStackHighWaterMark(nullptr))+" words";if(k=="language")return "en";if(k=="apps")return apps.count();if(k=="systemApps"){int c=0;for(int i=0;i<apps.count();i++)if(apps.at(i).system)c++;return c;}if(k=="userApps"){int c=0;for(int i=0;i<apps.count();i++)if(!apps.at(i).system)c++;return c;}if(k=="superuser")return perms.holder().length()?perms.holder():String("None");if(k=="files")return (double)LittleFS.usedBytes();if(k=="appServer"||k=="mconnect")return WiFi.status()==WL_CONNECTED;if(k=="peerPort")return 81;if(k=="wifi")return WiFi.status()==WL_CONNECTED?WiFi.SSID():String("Disconnected");if(k=="online")return WiFi.status()==WL_CONNECTED;if(k=="hostname")return settings.wifiHostname();if(k=="ip")return WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("");if(k=="heap")return String(ESP.getFreeHeap()/1024)+" KB";if(k=="psram")return String(ESP.getFreePsram()/1024)+" KB";if(k=="storageKeys")return settings.keys();if(k=="userAgent")return "MellorsOS/CyberPi";return "";}if(name=="system.time"||name=="system.date"){time_t t=time(nullptr);if(t<100000)return name.endsWith("time")?String(millis()/1000)+"s":String("No network time");struct tm tm;localtime_r(&t,&tm);char b[32];mellorsFormatDateTime(b,sizeof(b),tm,name.endsWith("time"),settings.clock24h(),settings.showSeconds());return String(b);}if(name=="system.wait"){if(n){unsigned long until=millis()+(unsigned long)constrain((long)a[0].num(),0L,60000L);while((long)(until-millis())>0){serviceQuietHardware();delay(12);}}return true;}if(name=="system.now")return (double)millis();if(name=="system.uptime")return (double)(millis()/1000);if(name=="system.random")return n>1?(double)random((long)a[0].num(),(long)a[1].num()+1):0;if(name=="system.storageBytes")return (double)LittleFS.usedBytes();if(name=="system.screenWidth")return 128;if(name=="system.screenHeight")return 128;if(name=="system.vibrate")return false;if(name=="system.clipboard"){if(n)settings.clipboard(a[0].str());return true;}if(name=="system.execute"){if(!app->system&&!perms.has(*app,6))return "Error: restricted";if(!n)return Value();if(inSystemExecute)return "Error: recursive execute";String cmd=a[0].str();String why;if(!validateCodeShape(cmd,1,why))return String("Error: ")+why;inSystemExecute=true;Value out=eval(cmd);inSystemExecute=false;return out;}if(name=="system.reboot"){if(inSystemExecute)return "Blocked in Terminal";if(!perms.has(*app,6)&&!app->system)return false;ESP.restart();return true;}if(name=="system.reset"){if(inSystemExecute)return "Blocked in Terminal";if(!perms.has(*app,6)&&!app->system)return false;LittleFS.format();settings.reset();perms.reset();data.reset();wifi.clearSaved();ESP.restart();return true;}if(name=="system.setAppHidden"){if(!app->system&&!perms.has(*app,6))return false;if(n>1)apps.setHidden(a[0].str(),a[1].truth());return true;}if(name=="system.home"){G_HOME_REQUEST=true;return true;}if(name=="system.openApp"){if(n)G_OPEN_REQUEST=a[0].str();return true;}if(name=="system.settingsScreen"){G_SETTINGS_REQUEST=true;return true;}if(name=="system.filesScreen"){if(!app->system)return false;G_FILES_REQUEST=true;return true;}if(name=="system.notesScreen"){if(!app->system)return false;G_NOTES_REQUEST=true;return true;}if(name=="system.calculatorScreen"){if(!app->system)return false;G_CALC_REQUEST=true;return true;}if(name=="system.messagesScreen"){if(!app->system)return false;G_MESSAGES_REQUEST=true;return true;}if(name=="system.editFile"){if(!app->system||!n)return false;G_EDIT_FILE_REQUEST=a[0].str();return true;}}
  if(name=="toast"){if(n)modal.msg("MellorsOS",a[0].str());return true;}
  for(int i=0;i<fnCount;i++)if(funcs[i].name==name)return invokeFunction(i,a,n);
  return Value();
}
Value MScript::invokeFunction(int fn,Value*a,int n){
  if(functionDepth>=12){Serial.println("[MellorsOS] MScript function nesting limit");G_HOME_REQUEST=true;return Value();}
  std::unique_ptr<Vars> backup(new(std::nothrow) Vars(state->vars));
  if(!backup){Serial.println("[MellorsOS] Function scope allocation failed");G_HOME_REQUEST=true;return Value();}
  functionDepth++;
String list=funcs[fn].args;int st=0,ix=0;while(st<=list.length()){int q=list.indexOf(',',st);String k=q<0?list.substring(st):list.substring(st,q);k.trim();if(k.length())state->vars.set(k,ix<n?a[ix]:Value());ix++;if(q<0)break;st=q+1;}returnFlag=false;returnValue=Value();execRange(funcs[fn].start,funcs[fn].end);Value r=returnValue;state->vars=*backup;functionDepth--;returnFlag=false;return r;
}

void MScript::tick(){
  if(G_SCRIPT_PEER_PENDING){
    char msgBuf[384],senderBuf[64];
    strncpy(msgBuf,G_SCRIPT_PEER_MESSAGE,sizeof(msgBuf)-1);msgBuf[sizeof(msgBuf)-1]=0;
    strncpy(senderBuf,G_SCRIPT_PEER_SENDER,sizeof(senderBuf)-1);senderBuf[sizeof(senderBuf)-1]=0;
    G_SCRIPT_PEER_PENDING=false;
    String msg(msgBuf),sender(senderBuf);
    for(int i=0;i<apps.count();i++){
      if(states&&states[i].initialized&&perms.has(apps.at(i),7)&&(i==current||perms.has(apps.at(i),5))){
        trigger(i,"networkMessage",sender,Value(msg),sender);
      }
    }
  }
  unsigned long now=millis();for(auto&t:timers)if(t.active&&(long)(now-t.due)>=0){int ix=t.app;String ev=t.event;if(t.repeating)t.due=now+t.period;else t.active=false;trigger(ix,ev,"",Value());}if(gameRunning&&current>=0&&now-gameLast>=gamePeriod){gameDelta=(now-gameLast)/1000.0f;gameLast=now;gameTime+=gameDelta;gameFrame++;trigger(current,gameEvent,"",Value(gameDelta));}}
void MScript::stopForeground(int ix){for(auto&t:timers)if(t.active&&t.app==ix&&!t.background)t.active=false;}
String MScript::apiList(){return "ui screen game server web network data file sound microphone settings app notify permission background timer dialog events system math text json value device";}

static const char APP_SETTINGS[] = R"MSCRIPT(
app "Settings"
meta id "settings"
meta color "#737d95"
meta version "0.9.1"
icon16
#10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A
#10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A
#10182A #10182A #10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A #10182A #10182A
#10182A #10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A #10182A
#10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A
#10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A #10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A
#10182A #10182A #DCE7F7 #DCE7F7 #10182A #10182A #6D7CFF #6D7CFF #6D7CFF #6D7CFF #10182A #10182A #DCE7F7 #DCE7F7 #10182A #10182A
#10182A #10182A #DCE7F7 #DCE7F7 #10182A #10182A #6D7CFF #6D7CFF #6D7CFF #6D7CFF #10182A #10182A #DCE7F7 #DCE7F7 #10182A #10182A
#10182A #10182A #DCE7F7 #DCE7F7 #10182A #10182A #6D7CFF #6D7CFF #6D7CFF #6D7CFF #10182A #10182A #DCE7F7 #DCE7F7 #10182A #10182A
#10182A #10182A #DCE7F7 #DCE7F7 #10182A #10182A #6D7CFF #6D7CFF #6D7CFF #6D7CFF #10182A #10182A #DCE7F7 #DCE7F7 #10182A #10182A
#10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A #10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A
#10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A
#10182A #10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A #10182A
#10182A #10182A #10182A #10182A #10182A #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #DCE7F7 #10182A #10182A #10182A #10182A #10182A
#10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A
#10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A #10182A
endicon
on start
  system.settingsScreen()
end
)MSCRIPT";

static const char APP_MCONNECT[] = R"MSCRIPT(
app "MConnect"
meta id "mconnect"
meta color "#4F46E5"
meta version "1.0"
icon16
#312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81
#312E81 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #FDE68A #FDE68A #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #FDE68A #FDE68A #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #A5F3FC #F8FAFC #F8FAFC #A5F3FC #A5F3FC #F8FAFC #F8FAFC #A5F3FC #A5F3FC #F8FAFC #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #F8FAFC #A5F3FC #A5F3FC #A5F3FC #F8FAFC #312E81 #312E81 #312E81 #F8FAFC #F8FAFC #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #F8FAFC #A5F3FC #A5F3FC #A5F3FC #F8FAFC #312E81 #312E81 #312E81 #F8FAFC #F8FAFC #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #F8FAFC #A5F3FC #A5F3FC #A5F3FC #F8FAFC #312E81 #312E81 #312E81 #F8FAFC #F8FAFC #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #312E81 #312E81 #312E81 #F8FAFC #F8FAFC #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #F8FAFC #F8FAFC #F8FAFC #F8FAFC #F8FAFC #312E81 #312E81 #312E81 #F8FAFC #F8FAFC #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #312E81
#312E81 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #4F46E5 #312E81
#312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81 #312E81
endicon

function render()
  ui.clear()
  ui.header("MConnect", "Connect a phone or computer")
  if !settings.get("wifiEnabled", true)
    ui.alert("Wi-Fi is off", "Turn on Wi-Fi in Settings before using MConnect.", "warning")
    ui.button("settings", "Open Settings", "primary")
    return ""
  end
  if !system.info("online")
    ui.alert("Connecting to Wi-Fi", "MellorsOS keeps trying saved Wi-Fi networks in the background. You can leave MConnect and keep using the device.", "info")
    ui.button("settings", "Open Wi-Fi Settings", "secondary")
    return ""
  end
  ui.card("1. Use the same Wi-Fi", "Connect your phone or computer to the Wi-Fi network used by this CyberPi.")
  ui.card("2. Open MConnect", "In a browser, enter either address below.")
  ui.infoRow("IP address", "http://" + system.info("ip"))
  ui.infoRow(".local address", "http://" + system.info("hostname") + ".local")
  ui.card("3. Keep this app open", "The local MConnect server runs only while this app is open on the CyberPi.")
  ui.card("4. Browse freely", "Mellors Store browsing does not need a PIN. MellorsOS asks for the lock PIN only when you install an app or open Remote Settings.")
  if !settings.get("pinEnabled", false)
    ui.alert("Protected actions unavailable", "Set a 4-digit lock PIN in Settings > Lock screen before installing apps or opening Remote Settings.", "warning")
  end
  ui.button("settings", "Open Settings", "secondary")
end

on start
  render()
end

on click settings
  system.settingsScreen()
end
)MSCRIPT";

static const char APP_SYSTEM[] = R"MSCRIPT(
app "System Monitor"
meta id "monitor"
meta color "#2e9bb7"
meta version "0.9.1"
icon16
#082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025
#082025 #082025 #082025 #082025 #082025 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #082025 #082025 #082025 #082025 #082025
#082025 #082025 #082025 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #082025 #082025 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #082025 #082025 #082025
#082025 #082025 #21B8A6 #21B8A6 #21B8A6 #082025 #082025 #FFFFFF #FFFFFF #082025 #082025 #21B8A6 #21B8A6 #21B8A6 #082025 #082025
#082025 #082025 #21B8A6 #21B8A6 #082025 #082025 #082025 #FFFFFF #FFFFFF #082025 #082025 #082025 #21B8A6 #21B8A6 #082025 #082025
#082025 #21B8A6 #21B8A6 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #21B8A6 #21B8A6 #082025
#082025 #21B8A6 #21B8A6 #082025 #082025 #082025 #082025 #FFFFFF #FFFFFF #082025 #082025 #082025 #082025 #21B8A6 #21B8A6 #082025
#082025 #21B8A6 #082025 #082025 #082025 #082025 #082025 #FFFFFF #FFFFFF #082025 #082025 #082025 #082025 #082025 #21B8A6 #082025
#082025 #21B8A6 #082025 #082025 #082025 #082025 #082025 #FFFFFF #FFFFFF #082025 #082025 #082025 #082025 #082025 #21B8A6 #082025
#082025 #21B8A6 #21B8A6 #082025 #082025 #082025 #082025 #FFFFFF #FFFFFF #082025 #082025 #082025 #082025 #21B8A6 #21B8A6 #082025
#082025 #21B8A6 #21B8A6 #082025 #082025 #082025 #082025 #FFFFFF #FFFFFF #082025 #082025 #082025 #082025 #21B8A6 #21B8A6 #082025
#082025 #082025 #21B8A6 #21B8A6 #082025 #082025 #082025 #FFFFFF #FFFFFF #082025 #082025 #082025 #21B8A6 #21B8A6 #082025 #082025
#082025 #082025 #21B8A6 #21B8A6 #21B8A6 #082025 #082025 #082025 #082025 #082025 #082025 #21B8A6 #21B8A6 #21B8A6 #082025 #082025
#082025 #082025 #082025 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #082025 #082025 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #082025 #082025 #082025
#082025 #082025 #082025 #082025 #082025 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #21B8A6 #082025 #082025 #082025 #082025 #082025
#082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025 #082025
endicon
on start
  ui.header("System Monitor", "MellorsOS diagnostics")
  ui.infoRow("System", system.info("name") + " " + system.info("version"))
  ui.infoRow("MScript", system.info("engine"))
  ui.infoRow("Wi-Fi", system.info("wifi"))
  ui.infoRow("Heap", system.info("heap"))
  ui.infoRow("PSRAM", system.info("psram"))
  ui.infoRow("Storage", system.storageBytes() + " B")
  ui.button("refresh", "Refresh", "primary")
end
on click refresh
  app.open("monitor")
end
)MSCRIPT";

static const char APP_TERMINAL[] = R"MSCRIPT(
app "Terminal"
meta id "terminal"
meta color "#18a66f"
meta version "0.9.1"
icon16
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #38E28D #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #38E28D #38E28D #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #38E28D #38E28D #38E28D #38E28D #38E28D #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #38E28D #38E28D #38E28D #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #38E28D #38E28D #38E28D #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #38E28D #38E28D #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #38E28D #38E28D #38E28D #38E28D #38E28D #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #A8FFD1 #A8FFD1 #A8FFD1 #A8FFD1 #A8FFD1 #A8FFD1 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
#0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18 #0C1D18
endicon
on start
  ui.header("Terminal", "Run one MScript expression")
  ui.input("command", "Command", data.get("lastCommand", "system.info(\"version\")"))
  ui.button("run", "Run", "primary")
  ui.label("output", data.get("lastOutput", "Ready."))
end
on click run
  let command = ui.value("command")
  data.set("lastCommand", command)
  let result = system.execute(command)
  let output = "> " + command + "\n" + value.string(result)
  data.set("lastOutput", output)
  ui.setText("output", output)
end
)MSCRIPT";

static const char APP_GUIDE[] = R"MSCRIPT(
app "MScript Guide"
meta id "guide"
meta color "#4474d9"
meta version "0.9.1"
icon16
#172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #FFFFFF #FFFFFF #FFFFFF #FFFFFF #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #FFFFFF #FFFFFF #FFFFFF #FFFFFF #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #FFFFFF #FFFFFF #FFFFFF #FFFFFF #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #FFFFFF #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #FFFFFF #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #4474D9 #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D
#172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D #172A5D
endicon
on start
  ui.header("MScript Guide", "Physical MellorsOS")
  ui.section("Language")
  ui.text("let/set, if/else, while, repeat, for, functions, return, break and continue are supported.")
  ui.section("Input")
  ui.text("ui.input uses the system QWERTY keyboard. Press B to toggle Shift; the on-screen backspace key deletes text.")
  ui.section("Permissions")
  ui.text(permission.list())
  ui.section("Hardware")
  ui.text("device.roll/pitch/angle, gyro, acceleration, buttons and joystick need no hardware permission. sound.note and sound.play use the speaker. microphone.record and microphone.loudness require microphone permission.")
  ui.section("Network")
  ui.text("web.get/web.post/web.request use HTTP/local-network URLs in v1.0. MConnect handles public HTTPS downloads such as Mellors Store packages. Apps should request the network permission.")
  ui.section("API")
  ui.text("UI, data, file, game, sound, microphone, settings, app, notify, permission, background, timer, dialog, events, system, math, text, json, value, device, web and network namespaces are available.")
end
)MSCRIPT";


static const char APP_FILES[] = R"MSCRIPT(
app "Files"
meta id "files"
meta color "#3c94ff"
meta version "0.9.1"
meta accepts "*/*"
icon16
#132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238
#132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238
#132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238
#132238 #132238 #132238 #A7D3FF #A7D3FF #A7D3FF #A7D3FF #A7D3FF #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238
#132238 #132238 #132238 #A7D3FF #A7D3FF #A7D3FF #A7D3FF #A7D3FF #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238
#132238 #132238 #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #132238 #132238
#132238 #132238 #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #132238 #132238
#132238 #132238 #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #132238 #132238
#132238 #132238 #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #132238 #132238
#132238 #132238 #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #132238 #132238
#132238 #132238 #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #132238 #132238
#132238 #132238 #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #132238 #132238
#132238 #132238 #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #62AFFF #132238 #132238
#132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238
#132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238
#132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238 #132238
endicon
on start
  system.filesScreen()
end
)MSCRIPT";

static const char APP_NOTES[] = R"MSCRIPT(
app "Notes"
meta id "notes"
meta color "#e6b63c"
meta version "0.9.1"
meta accepts "txt|md|json|csv|log|mscript|mapp|text/*"
icon16
#302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFF0B5 #FFF0B5 #FFF0B5 #FFF0B5 #FFF0B5 #FFF0B5 #FFF0B5 #FFF0B5 #FFF0B5 #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #66521A #66521A #66521A #66521A #66521A #66521A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #66521A #66521A #66521A #66521A #66521A #66521A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #66521A #66521A #66521A #66521A #66521A #66521A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #FFF0B5 #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #FFE28A #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16
#302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16 #302A16
endicon
on start
  system.notesScreen()
end
on openFile
  system.editFile(event.path)
end
)MSCRIPT";


static const char APP_MESSAGES[] = R"MSCRIPT(
app "Messages"
meta id "messages"
meta color "#4FA3FF"
meta version "0.9.1"
icon16
#10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A
#10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A
#10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A
#10253A #10253A #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #10253A #10253A
#10253A #10253A #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #10253A #10253A
#10253A #10253A #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #10253A #10253A
#10253A #10253A #4FA3FF #4FA3FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #4FA3FF #4FA3FF #10253A #10253A
#10253A #10253A #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #10253A #10253A
#10253A #10253A #4FA3FF #4FA3FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #BFE0FF #4FA3FF #4FA3FF #10253A #10253A
#10253A #10253A #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #10253A #10253A
#10253A #10253A #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #10253A #10253A
#10253A #10253A #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #4FA3FF #10253A #10253A
#10253A #10253A #10253A #10253A #4FA3FF #4FA3FF #4FA3FF #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A
#10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A
#10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A
#10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A #10253A
endicon
on start
  system.messagesScreen()
end
)MSCRIPT";

static const char APP_CALCULATOR[] = R"MSCRIPT(
app "Calculator"
meta id "calculator"
meta color "#21b8a6"
meta version "0.9.1"
icon16
#0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #123F36 #123F36 #123F36 #123F36 #123F36 #123F36 #43C8A8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #123F36 #123F36 #123F36 #123F36 #123F36 #123F36 #43C8A8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #123F36 #123F36 #123F36 #123F36 #123F36 #123F36 #43C8A8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #E6FFF8 #43C8A8 #43C8A8 #E6FFF8 #43C8A8 #43C8A8 #E6FFF8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #E6FFF8 #43C8A8 #43C8A8 #E6FFF8 #43C8A8 #43C8A8 #E6FFF8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #E6FFF8 #43C8A8 #43C8A8 #E6FFF8 #43C8A8 #43C8A8 #E6FFF8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #43C8A8 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25
#0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25 #0E2A25
endicon
on start
  system.calculatorScreen()
end
)MSCRIPT";

struct ListItem{String title,value,detail;};
class ListUI{
  Display&d;
  struct Memory{String title;int sel=0;bool used=false;} mem[16];
  int remembered(const String&t){for(auto&m:mem)if(m.used&&m.title==t)return m.sel;return 0;}
  void remember(const String&t,int s){for(auto&m:mem)if(m.used&&m.title==t){m.sel=s;return;}for(auto&m:mem)if(!m.used){m.used=true;m.title=t;m.sel=s;return;}mem[0].title=t;mem[0].sel=s;mem[0].used=true;}
  int itemHeight(const ListItem&i){
    int lines=d.wrappedLines(i.title,114,1);
    if(i.value.length())lines+=d.wrappedLines(i.value,114,1);
    if(i.detail.length())lines+=d.wrappedLines(i.detail,114,1);
    return max(24,8+lines*d.lineH(1));
  }
  String combined(const ListItem&i){
    String s=i.title;if(i.value.length())s+="\n"+i.value;if(i.detail.length())s+="\n"+i.detail;return s;
  }
public:ListUI(Display&x):d(x){}
  int choose(String title,ListItem*items,int n,int initial=-1){
    if(n<=0)return-1;int sel=constrain(initial>=0?initial:remembered(title),0,n-1),innerLine=0;
    released();InputState prev;
    while(1){
      int headerH=max(18,5+d.wrappedLines(title,120,1)*d.lineH(1));int viewTop=headerH+2,viewH=128-viewTop;
      int selectedH=itemHeight(items[sel]);int selectedLines=max(1,(selectedH-8)/d.lineH(1));int visibleLines=max(1,(viewH-8)/d.lineH(1));
      if(selectedH<=viewH)innerLine=0;else innerLine=constrain(innerLine,0,max(0,selectedLines-visibleLines));

      d.clear();d.header(title,headerH);d.setClip(viewTop,128);
      if(selectedH>viewH){
        d.fill(3,viewTop,122,viewH,d.accent());uint32_t tc=d.contrast(d.accent());
        String all=d.wrapText(combined(items[sel]),114,1);String shown="";int line=0,start=0;
        for(size_t p=0;p<=all.length();p++){if(p==all.length()||all[p]=='\n'){if(line>=innerLine&&line<innerLine+visibleLines){if(shown.length())shown+='\n';shown+=all.substring(start,p);}line++;start=p+1;}}
        d.text(7,viewTop+4,shown,tc,1,false,114);
        d.resetClip();d.text(98,117,String(innerLine+1)+"/"+max(1,selectedLines-visibleLines+1),d.muted(),1);
      }else{
        int before=0;for(int i=0;i<sel;i++)before+=itemHeight(items[i]);
        int total=before+selectedH;for(int i=sel+1;i<n;i++)total+=itemHeight(items[i]);
        int scroll=max(0,before); // selected starts at top when lists are dense, preventing overlap.
        // Pull upward to show prior items when space allows.
        int room=viewH-selectedH;int j=sel-1;while(j>=0&&room>0){int h=itemHeight(items[j]);if(h>room)break;scroll-=h;room-=h;j--;}scroll=max(0,scroll);
        int cy=viewTop-scroll;
        for(int i=0;i<n;i++){int h=itemHeight(items[i]);if(cy+h>viewTop&&cy<128){bool ac=i==sel;d.fill(3,cy,122,h-2,ac?d.accent():d.panel());uint32_t tc=ac?d.contrast(d.accent()):d.fg();int ty=cy+4;
            d.text(7,ty,items[i].title,tc,1,true,114);ty+=d.textHeight(items[i].title,114,1);
            if(items[i].value.length()){d.text(7,ty,items[i].value,ac?tc:d.muted(),1,true,114);ty+=d.textHeight(items[i].value,114,1);}
            if(items[i].detail.length())d.text(7,ty,items[i].detail,ac?tc:d.muted(),1,true,114);
          }cy+=h;}
        d.resetClip();d.text(101,117,String(sel+1)+"/"+n,d.muted(),1);
      }
      d.render();

      auto now=readInput();Action a=inputAction(now,prev);prev=now;
      if(a==UP){
        if(selectedH>viewH&&innerLine>0)innerLine--;
        else{sel=(sel+n-1)%n;innerLine=0;}
      }else if(a==DOWN){
        if(selectedH>viewH&&innerLine<max(0,selectedLines-visibleLines))innerLine++;
        else{sel=(sel+1)%n;innerLine=0;}
      }else if(a==SELECT){remember(title,sel);released();return sel;}
      else if(a==BACK||a==HOME_SINGLE){remember(title,sel);released();return-1;}
      else if(a==HOME_DOUBLE){remember(title,sel);G_RECENTS_REQUEST=true;released();return-2;}
      else if(a==HOME_HOLD){remember(title,sel);G_HOME_REQUEST=true;released();return-2;}
      backgroundServiceHook();delay(8);
    }
  }
};


class TextFileEditorUI{
  Display&d;Modal&m;Keyboard&kbd;
public:TextFileEditorUI(Display&dd,Modal&mm,Keyboard&kk):d(dd),m(mm),kbd(kk){}
  bool edit(String path,String label="Text Editor"){
    if(!publicFilePath(path)){m.msg(label,"Protected app package");return false;}
    File f=LittleFS.open(path,"r");String content="";if(f){if(f.isDirectory()){f.close();m.msg(label,"Cannot edit folder",path);return false;}if(f.size()>8192){f.close();m.msg(label,"File is too large","8 KB editor limit");return false;}content=f.readString();f.close();}
    bool cancel=false;String changed=kbd.prompt(label,content,false,8192,&cancel);if(cancel)return false;int slash=path.lastIndexOf('/');if(slash>0)ensureFolder(path.substring(0,slash));File out=LittleFS.open(path,"w");if(!out){m.msg(label,"Could not save",path);return false;}out.print(changed);out.close();m.msg(label,"Saved",path);return true;
  }
};

class FileManagerUI{
  Display&d;Modal&m;Keyboard&kbd;ListUI&list;TextFileEditorUI editor;
  // v0.9: keep directory listing scratch off the kernel task stack.
  ListItem browseItems[32];String browsePaths[32];bool browseDirs[32];
  bool navRequested(){return G_HOME_REQUEST||G_RECENTS_REQUEST;}
  bool copyOne(String from,String to){if(!publicFilePath(from)||!publicFilePath(to))return false;if(LittleFS.exists(to)&&!m.confirm("Replace file?",baseName(to)))return false;return copyFilePath(from,to);}
  void fileActions(String path){if(!publicFilePath(path))return;while(!navRequested()){File f=LittleFS.open(path);if(!f)return;size_t size=f.size();f.close();ListItem it[6]={{textFilePath(path)?"Open in Notes":"File info",String(size)+" B"},{"Rename",""},{"Copy to...",""},{"Move to...",""},{"Delete",""},{"Back",""}};int c=list.choose(baseName(path),it,6);if(c<0||c==5)return;if(c==0){if(textFilePath(path))editor.edit(path,"Notes");else m.msg("File",baseName(path),String(size)+" bytes");}else if(c==1){bool cancel=false;String nn=kbd.prompt("Rename",baseName(path),false,64,&cancel);if(!cancel&&nn.length()){String np=parentPath(path)+"/"+safeName(nn);if(publicFilePath(np)&&LittleFS.rename(path,np)){path=np;m.msg("Files","Renamed",baseName(np));}else m.msg("Files","Rename failed");}}else if(c==2||c==3){bool cancel=false;String to=kbd.prompt(c==2?"Copy destination":"Move destination",path,false,120,&cancel);if(!cancel&&to.length()){if(!to.startsWith("/"))to=String("/")+to;bool ok=copyOne(path,to);if(ok&&c==3){LittleFS.remove(path);path=to;}m.msg("Files",ok?(c==2?"Copied":"Moved"):"Operation failed",to);}}else if(c==4){if(m.confirm("Delete file?",baseName(path),"This cannot be undone","Delete","Cancel")){LittleFS.remove(path);return;}}}}
  void folderActions(String path,bool root){if(!publicFilePath(path))return;while(!navRequested()){ListItem it[6];int n=0;it[n++]={"Open",""};it[n++]={"New folder",""};it[n++]={"New text file",""};if(!root){it[n++]={"Rename folder",""};it[n++]={"Delete folder","Recursive"};}it[n++]={"Back",""};int c=list.choose(root?"Filesystem":baseName(path),it,n);if(c<0||c==n-1)return;if(c==0){return;}if(c==1){bool cancel=false;String name=kbd.prompt("Folder name","New Folder",false,48,&cancel);if(!cancel&&name.length()){String p=(path=="/"?String(""):path)+"/"+safeName(name);if(publicFilePath(p)&&LittleFS.mkdir(p))m.msg("Files","Folder created",p);else m.msg("Files","Could not create folder");}}else if(c==2){bool cancel=false;String name=kbd.prompt("File name","untitled.txt",false,64,&cancel);if(!cancel&&name.length()){String p=(path=="/"?String(""):path)+"/"+safeName(name);File f=publicFilePath(p)?LittleFS.open(p,"w"):File();if(f){f.close();editor.edit(p,"Notes");}else m.msg("Files","Could not create file");}}else if(!root&&c==3){bool cancel=false;String nn=kbd.prompt("Rename folder",baseName(path),false,48,&cancel);if(!cancel&&nn.length()){String np=parentPath(path)+"/"+safeName(nn);if(publicFilePath(np)&&LittleFS.rename(path,np)){path=np;m.msg("Files","Folder renamed",np);}else m.msg("Files","Rename failed");}}else if(!root&&c==4){if(m.confirm("Delete folder?",baseName(path),"All contents will be erased","Delete","Cancel")){removeTree(path);return;}}}}
public:FileManagerUI(Display&dd,Modal&mm,Keyboard&kk,ListUI&ll):d(dd),m(mm),kbd(kk),list(ll),editor(dd,mm,kk){}
  String pickFile(String start="/",String floor="/"){
    String path=start.length()?start:String("/");if(!publicFilePath(path))path="/";if(!floor.length())floor="/";
    while(!navRequested()){
      if(!LittleFS.exists(path))path=floor;
      File root=LittleFS.open(path);if(!root||!root.isDirectory()){if(root)root.close();return LittleFS.exists(path)?path:String("");}
      int n=0;if(path!=floor){browseItems[n]={"..","Parent folder"};browsePaths[n]=parentPath(path);browseDirs[n++]=true;}
      File f=root.openNextFile();while(f&&n<32){String fp=f.path();if(!publicFilePath(fp)){f.close();f=root.openNextFile();continue;}String nm=baseName(fp);bool isdir=f.isDirectory();browseItems[n].title=(isdir?String("[DIR] "):String(""))+nm;browseItems[n].value=isdir?String(""):String(f.size())+" B";browseItems[n].detail="";browsePaths[n]=fp;browseDirs[n]=isdir;n++;f.close();f=root.openNextFile();}if(f)f.close();root.close();
      if(n==0){m.msg("Choose file","No files here",path);return "";}
      int c=list.choose("Choose file",browseItems,n);if(c<0||c>=n)return "";
      if(path!=floor&&c==0){path=browsePaths[c];continue;}
      if(browseDirs[c])path=browsePaths[c];else return browsePaths[c];
    }
    return "";
  }
  void browse(String start="/"){String path=start.length()?start:String("/");if(!publicFilePath(path))path="/";while(!navRequested()){if(!LittleFS.exists(path))path="/";File root=LittleFS.open(path);if(!root||!root.isDirectory()){if(root)root.close();fileActions(path);return;}int n=0;if(path!="/"){browseItems[n]={"..","Parent folder"};browsePaths[n]=parentPath(path);browseDirs[n++]=true;}browseItems[n]={"+ Manage folder",path};browsePaths[n]=path;browseDirs[n++]=true;File f=root.openNextFile();while(f&&n<32){String fp=f.path();if(!publicFilePath(fp)){f.close();f=root.openNextFile();continue;}String nm=baseName(fp);bool isdir=f.isDirectory();browseItems[n].title=(isdir?String("[DIR] "):String(""))+nm;browseItems[n].value=isdir?String(""):String(f.size())+" B";browseItems[n].detail="";browsePaths[n]=fp;browseDirs[n]=isdir;n++;f.close();f=root.openNextFile();}if(f)f.close();root.close();int c=list.choose(path,browseItems,n);if(c<0||c>=n){if(path=="/")return;path=parentPath(path);continue;}if(c==0&&path!="/"){path=browsePaths[c];continue;}if(browseItems[c].title=="+ Manage folder"){folderActions(path,path=="/");continue;}if(browseDirs[c]){ListItem act[5]={{"Open",""},{"Rename",""},{"Move to...",""},{"Delete","Recursive"},{"Back",""}};String chosen=browsePaths[c];int a=list.choose(baseName(chosen),act,5);if(a==0)path=chosen;else if(a==1){bool cancel=false;String nn=kbd.prompt("Rename folder",baseName(chosen),false,48,&cancel);if(!cancel&&nn.length()){String dest=parentPath(chosen)+"/"+safeName(nn);if(publicFilePath(dest))LittleFS.rename(chosen,dest);}}else if(a==2){bool cancel=false;String to=kbd.prompt("Move folder to",chosen,false,120,&cancel);if(!cancel&&to.length()){if(!to.startsWith("/"))to=String("/")+to;if(publicFilePath(to))LittleFS.rename(chosen,to);}}else if(a==3&&m.confirm("Delete folder?",baseName(chosen),"All contents will be erased","Delete","Cancel"))removeTree(chosen);}else fileActions(browsePaths[c]);}}
  bool editFile(String path){return editor.edit(path,"Notes");}
};

class NotesUI{
  Display&d;Modal&m;Keyboard&kbd;ListUI&list;TextFileEditorUI editor;
  ListItem noteItems[32];String notePaths[32];
  String notePath(String name){if(!name.endsWith(".txt")&&!name.endsWith(".md"))name+=".txt";return String("/notes/")+safeName(name);}
  void actions(String path){while(!G_HOME_REQUEST&&!G_RECENTS_REQUEST){ListItem a[4]={{"Edit",""},{"Rename",""},{"Delete",""},{"Back",""}};int c=list.choose(baseName(path),a,4);if(c<0||c==3)return;if(c==0)editor.edit(path,"Notes");else if(c==1){bool cancel=false;String nn=kbd.prompt("Rename note",baseName(path),false,64,&cancel);if(!cancel&&nn.length()){String np=notePath(nn);if(publicFilePath(np)&&LittleFS.rename(path,np))path=np;}}else if(c==2&&m.confirm("Delete note?",baseName(path),"This cannot be undone","Delete","Cancel")){LittleFS.remove(path);return;}}}
public:NotesUI(Display&dd,Modal&mm,Keyboard&kk,ListUI&ll):d(dd),m(mm),kbd(kk),list(ll),editor(dd,mm,kk){}
  void run(){ensureFolder("/notes");while(!G_HOME_REQUEST&&!G_RECENTS_REQUEST){int n=0;noteItems[n]={"+ New note",""};notePaths[n++]="";File root=LittleFS.open("/notes");if(!root||!root.isDirectory()){if(root)root.close();m.msg("Notes","Could not open notes folder");return;}File f=root.openNextFile();while(f&&n<32){if(!f.isDirectory()){notePaths[n]=f.path();noteItems[n].title=baseName(notePaths[n]);noteItems[n].value=String(f.size())+" B";noteItems[n].detail="";n++;}f.close();f=root.openNextFile();}if(f)f.close();root.close();int c=list.choose("Notes",noteItems,n);if(c<0||c>=n)return;if(c==0){bool cancel=false;String name=kbd.prompt("Note name","Untitled",false,48,&cancel);if(!cancel&&name.length()){String p=notePath(name);if(!LittleFS.exists(p)){File nf=LittleFS.open(p,"w");if(nf)nf.close();}editor.edit(p,"Notes");}}else actions(notePaths[c]);}}
  bool editPath(String p){return editor.edit(p,"Notes");}
};


class MessageStore{
  static String esc(String v){v.replace("%","%25");v.replace("|","%7C");v.replace("\r","%0D");v.replace("\n","%0A");return v;}
  static String unesc(String v){v.replace("%0A","\n");v.replace("%0D","\r");v.replace("%7C","|");v.replace("%25","%");return v;}
  String pathFor(const String&peer){return String("/messages/")+hex8(hash32(peer))+".chat";}
public:
  void begin(){ensureFolder("/messages");}
  void append(String peer,bool outgoing,String body){
    peer.trim();if(!peer.length()||!body.length())return;if(body.length()>383)body.remove(383);
    begin();String path=pathFor(peer);bool fresh=!LittleFS.exists(path);File f=LittleFS.open(path,"a");if(!f)return;
    if(fresh)f.println(String("P|")+esc(peer));
    f.println(String(outgoing?"O|":"I|")+esc(body));f.close();
  }
  int conversations(String*peers,String*previews,int maxCount){
    begin();int n=0;File root=LittleFS.open("/messages");if(!root||!root.isDirectory())return 0;File f=root.openNextFile();
    while(f&&n<maxCount){if(!f.isDirectory()){
      String first=f.readStringUntil('\n');first.trim();String peer=first.startsWith("P|")?unesc(first.substring(2)):String("");String last="";
      while(f.available()){String line=f.readStringUntil('\n');line.trim();if(line.startsWith("I|")||line.startsWith("O|"))last=line;}
      if(peer.length()){peers[n]=peer;previews[n]=last.length()>2?unesc(last.substring(2)):String("");if(previews[n].length()>30)previews[n]=previews[n].substring(0,29)+"~";n++;}
    }f.close();f=root.openNextFile();}if(f)f.close();root.close();return n;
  }
  int read(String peer,String*lines,bool*outgoing,int maxCount){
    int n=0;File f=LittleFS.open(pathFor(peer),"r");if(!f)return 0;f.readStringUntil('\n');
    while(f.available()){String line=f.readStringUntil('\n');line.trim();if(!(line.startsWith("I|")||line.startsWith("O|")))continue;String body=unesc(line.substring(2));bool out=line[0]=='O';
      if(n<maxCount){lines[n]=body;outgoing[n]=out;n++;}else{for(int i=1;i<maxCount;i++){lines[i-1]=lines[i];outgoing[i-1]=outgoing[i];}lines[maxCount-1]=body;outgoing[maxCount-1]=out;}
    }f.close();return n;
  }
};

struct DiscoveredPeer{String hostname;IPAddress address;};
class PeerDiscovery{
  DiscoveredPeer found[16];int n=0;
public:
  int scan(const String&selfHost){
    n=0;if(WiFi.status()!=WL_CONNECTED||!G_MDNS_STARTED)return 0;
    int count=MDNS.queryService("mellors","tcp");
    for(int i=0;i<count&&n<16;i++){
      String h=MDNS.hostname(i);IPAddress ip=MDNS.IP(i);if(!h.length()||h==selfHost)continue;
      bool dup=false;for(int j=0;j<n;j++)if(found[j].hostname==h)dup=true;if(dup)continue;
      found[n].hostname=h;found[n].address=ip;n++;
    }
    return n;
  }
  int count()const{return n;}
  String hostname(int i)const{return i>=0&&i<n?found[i].hostname:String("");}
  String target(String host)const{for(int i=0;i<n;i++)if(found[i].hostname==host&&(found[i].address[0]||found[i].address[1]||found[i].address[2]||found[i].address[3]))return found[i].address.toString();return host.endsWith(".local")?host:host+".local";}
};

class MessagesUI{
  Display&d;Modal&m;Keyboard&kbd;ListUI&list;WifiManager&wifi;MessageStore&store;PeerDiscovery&discovery;
  ListItem mainItems[17];String mainPeers[16],mainPreview[16];
  ListItem peerItems[16];
  ListItem chatItems[25];String chatLines[24];bool chatOutgoing[24];
  bool navRequested(){return G_HOME_REQUEST||G_RECENTS_REQUEST;}
  bool sendMessage(String peer,String body){
    if(WiFi.status()!=WL_CONNECTED)return false;String h=stripPeerHost(discovery.target(peer));if(!h.length())return false;if(h.indexOf(':')<0)h+=":81";
    int status=0;httpRequestRaw("POST",String("http://")+h+"/mellors/message?from="+wifi.hostname(),body,"text/plain",status);return status>=200&&status<300;
  }
  void conversation(String peer){
    while(!navRequested()){
      int count=store.read(peer,chatLines,chatOutgoing,24);int n=0;chatItems[n++]={"+ Send message",peer,""};
      for(int i=0;i<count&&n<25;i++){chatItems[n].title=chatOutgoing[i]?"You":peer;chatItems[n].value=chatLines[i];chatItems[n].detail="";n++;}
      int c=list.choose(peer,chatItems,n);if(c<0)return;if(c==0){bool cancel=false;String body=kbd.prompt(String("Message to ")+peer,"",false,280,&cancel);body.trim();if(cancel||!body.length())continue;
        if(sendMessage(peer,body)){store.append(peer,true,body);m.msg("Message sent",peer);}else m.msg("Send failed",peer,"Make sure the other MellorsOS device is online");
      }
    }
  }
  void newMessage(){
    if(WiFi.status()!=WL_CONNECTED){m.msg("Messages","Connect to Wi-Fi first");return;}
    d.clear();d.header("Messages");d.text(12,50,"Scanning LAN...",d.fg(),1);d.render();backgroundServiceHook();
    int n=discovery.scan(wifi.hostname());if(n<=0){m.msg("Messages","No MellorsOS devices found","Both devices must be on the same LAN");return;}
    for(int i=0;i<n;i++){peerItems[i].title=discovery.hostname(i);peerItems[i].value="MellorsOS";peerItems[i].detail="";}
    int c=list.choose("New message",peerItems,n);if(c>=0)conversation(discovery.hostname(c));
  }
public:
  MessagesUI(Display&dd,Modal&mm,Keyboard&kk,ListUI&ll,WifiManager&ww,MessageStore&ss,PeerDiscovery&pd):d(dd),m(mm),kbd(kk),list(ll),wifi(ww),store(ss),discovery(pd){}
  void run(){
    store.begin();while(!navRequested()){
      int cc=store.conversations(mainPeers,mainPreview,16);int n=0;mainItems[n++]={"+ New message","Scan the LAN for MellorsOS devices",""};
      for(int i=0;i<cc;i++){mainItems[n].title=mainPeers[i];mainItems[n].value=mainPreview[i];mainItems[n].detail="";n++;}
      int c=list.choose("Messages",mainItems,n);if(c<0)return;if(c==0)newMessage();else if(c-1<cc)conversation(mainPeers[c-1]);
    }
  }
};

class CalculatorUI{
  Display&d;Modal&m;Keyboard&kbd;ListUI&list;
  class Parser{const char*p;bool bad=false;void ws(){while(*p&&isspace((unsigned char)*p))p++;}double number(){ws();if(*p=='('){p++;double v=expr();ws();if(*p==')')p++;else bad=true;return v;}char*e=nullptr;double v=strtod(p,&e);if(e==p){bad=true;return 0;}p=e;return v;}double unary(){ws();if(*p=='+'){p++;return unary();}if(*p=='-'){p++;return-unary();}return number();}double term(){double v=unary();while(1){ws();char op=*p;if(op!='*'&&op!='/')break;p++;double r=unary();if(op=='/'&&r==0){bad=true;return NAN;}v=op=='*'?v*r:v/r;}return v;}double expr(){double v=term();while(1){ws();char op=*p;if(op!='+'&&op!='-')break;p++;double r=term();v=op=='+'?v+r:v-r;}return v;}public:double eval(const String&s){p=s.c_str();bad=false;double v=expr();ws();if(*p)bad=true;return bad?NAN:v;}};
  const char* key(int i){static const char*keys[20]={"C","(",")","/","7","8","9","*","4","5","6","-","1","2","3","+","0",".","<-","="};return keys[i];}
  void draw(const String&expr,const String&result,int sel){
    d.clear();d.header("Calculator",18);d.setClip(18,128);
    d.fill(2,20,124,14,d.panel());String ex=expr;if(ex.length()>20)ex=ex.substring(ex.length()-20);d.text(5,23,ex.length()?ex:"0",d.fg(),1,false,118);
    d.fill(2,35,124,13,d.panel());String rr=result;if(rr.length()>20)rr=rr.substring(rr.length()-20);d.text(5,37,rr,d.muted(),1,false,118);
    int top=50,cellW=31,cellH=15;
    for(int i=0;i<20;i++){int row=i/4,col=i%4,x=2+col*31,y=top+row*15;bool ac=i==sel;d.fill(x,y,29,13,ac?d.accent():d.panel());String k=key(i);d.text(x+10-(k.length()>1?3:0),y+3,k,ac?d.contrast(d.accent()):d.fg(),1);}
    d.resetClip();d.render();
  }
public:CalculatorUI(Display&dd,Modal&mm,Keyboard&kk,ListUI&ll):d(dd),m(mm),kbd(kk),list(ll){}
  void run(){
    String expression="",result="0";int sel=16;released();InputState prev;draw(expression,result,sel);
    while(!G_HOME_REQUEST&&!G_RECENTS_REQUEST){
      auto n=readInput();Action a=inputAction(n,prev);prev=n;
      if(a==HOME_SINGLE||a==BACK){released();return;}
      if(a==HOME_DOUBLE){G_RECENTS_REQUEST=true;released();return;}
      if(a==HOME_HOLD){G_HOME_REQUEST=true;released();return;}
      if(a==LEFT){sel=(sel/4)*4+(sel%4+3)%4;draw(expression,result,sel);}
      else if(a==RIGHT){sel=(sel/4)*4+(sel%4+1)%4;draw(expression,result,sel);}
      else if(a==UP){sel=(sel+16)%20;draw(expression,result,sel);}
      else if(a==DOWN){sel=(sel+4)%20;draw(expression,result,sel);}
      else if(a==SELECT){
        String k=key(sel);
        if(k=="C"){expression="";result="0";}
        else if(k=="<-"){if(expression.length())expression.remove(expression.length()-1);}
        else if(k=="="){Parser p;double v=p.eval(expression);result=isnan(v)?String("Error"):Value(v).str();}
        else if(expression.length()<48)expression+=k;
        draw(expression,result,sel);
      }
      serviceQuietHardware();delay(12);
    }
  }
};

class SystemSettingsUI{
  Display&d;Modal&m;Keyboard&kbd;ListUI&list;SettingsStore&s;WifiManager&wifi;Permissions&perms;Registry&apps;DataStore&data;MConnectServer&mconnectServer;PinPad&pin;
  int chooseVals(String title,const char*const*vals,int n,String current){ListItem it[12];for(int i=0;i<n;i++){it[i].title=vals[i];it[i].value=String(vals[i])==current?"SELECTED":"";}return list.choose(title,it,n);}
public:SystemSettingsUI(Display&dd,Modal&mm,Keyboard&kk,ListUI&ll,SettingsStore&ss,WifiManager&ww,Permissions&pp,Registry&aa,DataStore&ds,MConnectServer&dv,PinPad&pn):d(dd),m(mm),kbd(kk),list(ll),s(ss),wifi(ww),perms(pp),apps(aa),data(ds),mconnectServer(dv),pin(pn){}
  int run(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return-2;ListItem i[11]={{"Customization","Wallpaper, Home, colors"},{"Accessibility","Text, contrast, motion"},{"Notifications","Top bar, previews, history"},{"Status & navigation","Clock and status"},{"Wi-Fi","Networks and saved Wi-Fi"},{"Sound","Volume and silent mode"},{"Lock screen",s.pinEnabled()?"4-digit PIN enabled":"Off"},{"App Manager","Permissions, Home, data"},{"Storage & privacy","LittleFS and local data"},{"Developer options","MScript tools"},{"System","About and reset"}};int c=list.choose("Settings",i,11);if(c==-2)return-2;if(c<0)return-1;if(c==0)customize();else if(c==1)access();else if(c==2)notifications();else if(c==3)status();else if(c==4)wifiPage();else if(c==5)sound();else if(c==6)lockPage();else if(c==7)appManager();else if(c==8)storage();else if(c==9)developer();else system();}}
  void customize(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;ListItem i[11];i[0]={"Wallpaper",s.wallpaper()};i[1]={"Home app layout","Order and visibility"};i[2]={"Accent color",accentNameFor(s.accent())+" "+s.accent()};i[3]={"Owner name",s.username()};i[4]={"Home title",s.homeTitle()};i[5]={"Home subtitle",s.homeSubtitle()};i[6]={"Grid columns",String(s.gridCols())};i[7]={"Icon size",String(s.iconSize())+" px"};i[8]={"Icon shape",s.iconShape()};i[9]={"Show app labels",boolText(s.showAppLabels())};i[10]={"Compact interface",boolText(s.compact())};int c=list.choose("Customization",i,11);if(c<0)return;if(c==0){const char*v[]={"aurora","ocean","sunset","forest","mono"};int q=chooseVals("Wallpaper",v,5,s.wallpaper());if(q>=0)s.wallpaper(v[q]);}else if(c==1)homeLayout();else if(c==2){ListItem p[ACCENT_COUNT];for(int x=0;x<ACCENT_COUNT;x++){p[x].title=ACCENTS[x].name;p[x].value=ACCENTS[x].hex;}int q=list.choose("Accent color",p,ACCENT_COUNT);if(q>=0)s.accent(ACCENTS[q].hex);}else if(c==3){bool x=false;String v=kbd.prompt("Owner name",s.username(),false,24,&x);if(!x)s.username(v);}else if(c==4){bool x=false;String v=kbd.prompt("Home title",s.homeTitle(),false,24,&x);if(!x)s.homeTitle(v);}else if(c==5){bool x=false;String v=kbd.prompt("Home subtitle",s.homeSubtitle(),false,40,&x);if(!x)s.homeSubtitle(v);}else if(c==6)s.gridCols(s.gridCols()%3+1);else if(c==7)s.iconSize(s.iconSize()==16?24:s.iconSize()==24?32:16);else if(c==8){String v=s.iconShape();s.iconShape(v=="square"?"squircle":v=="squircle"?"round":v=="round"?"circle":"square");}else if(c==9)s.showAppLabels(!s.showAppLabels());else s.compact(!s.compact());}}
  void homeLayout(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;String order=apps.normalizedOrder();ListItem it[MAX_APPS];int ids[MAX_APPS],count=0,st=0;while(st<=order.length()&&count<MAX_APPS){int q=order.indexOf('|',st);String id=q<0?order.substring(st):order.substring(st,q);int ix=apps.find(id);if(ix>=0){ids[count]=ix;it[count].title=apps.at(ix).meta.name;it[count].value=apps.hidden(id)?"HIDDEN":String(count+1);count++;}if(q<0)break;st=q+1;}int c=list.choose("Home app layout",it,count);if(c<0)return;App&a=apps.at(ids[c]);ListItem act[4]={{"Move earlier",""},{"Move later",""},{apps.hidden(a.meta.id)?"Show on Home":"Hide from Home",""},{"Back",""}};int q=list.choose(a.meta.name,act,4);if(q==0)apps.moveHome(a.meta.id,-1);else if(q==1)apps.moveHome(a.meta.id,1);else if(q==2&&a.meta.id!="settings")apps.setHidden(a.meta.id,!apps.hidden(a.meta.id));}}
  void access(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;ListItem i[7]={{"Text size",String(s.fontScale(),2)},{"Bold text",boolText(s.boldText())},{"High contrast",boolText(s.highContrast())},{"Reduce motion",boolText(s.reduceMotion())},{"Reduce transparency",boolText(s.reduceTransparency())},{"Larger targets",boolText(s.largeTargets())},{"System haptics",boolText(s.haptics())}};int c=list.choose("Accessibility",i,7);if(c<0)return;if(c==0){float x=s.fontScale()+.15f;if(x>1.45f)x=.85f;s.fontScale(x);}else if(c==1)s.boldText(!s.boldText());else if(c==2)s.highContrast(!s.highContrast());else if(c==3)s.reduceMotion(!s.reduceMotion());else if(c==4)s.reduceTransparency(!s.reduceTransparency());else if(c==5)s.largeTargets(!s.largeTargets());else s.haptics(!s.haptics());}}
  void notifications(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;ListItem i[3]={{"Top-bar banners",boolText(s.notificationToasts())},{"Show previews",boolText(s.notificationPreview())},{"History limit",String(s.notificationLimit())}};int c=list.choose("Notifications",i,3);if(c<0)return;if(c==0)s.notificationToasts(!s.notificationToasts());else if(c==1)s.notificationPreview(!s.notificationPreview());else{s.notificationLimit(s.notificationLimit()>=100?5:s.notificationLimit()+5);}}}
  void status(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;ListItem i[5]={{"24-hour clock",boolText(s.clock24h())},{"Show seconds",boolText(s.showSeconds())},{"Show battery",boolText(s.showBattery())},{"Show status icons",boolText(s.showStatusIcons())},{"Compact navigation",boolText(s.navCompact())}};int c=list.choose("Status & navigation",i,5);if(c<0)return;if(c==0)s.clock24h(!s.clock24h());else if(c==1)s.showSeconds(!s.showSeconds());else if(c==2)s.showBattery(!s.showBattery());else if(c==3)s.showStatusIcons(!s.showStatusIcons());else s.navCompact(!s.navCompact());}}
  void wifiPage(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;
    ListItem root[5]={{"Hostname",s.wifiHostname()},{"Scan networks",WiFi.status()==WL_CONNECTED?WiFi.SSID():"Disconnected"},{"Saved networks",String(wifi.count())},{"Reconnect",""},{"Back",""}};int c=list.choose("Wi-Fi",root,5);if(c<0||c==4)return;
    if(c==0){bool cancel=false;String h=kbd.prompt("Wi-Fi hostname",s.wifiHostname(),false,32,&cancel);if(!cancel&&h.length()){h=safeHostname(h);s.wifiHostname(h);wifi.setHostname(h);}continue;}
    if(c==3){WiFi.disconnect();wifi.startBackground(s.lastWifi());continue;}if(c==2){savedWifi();continue;}
    d.clear();d.header("Wi-Fi");d.text(5,30,"Scanning...",d.fg(),1);d.render();wifi.beginForegroundScan();int found=WiFi.scanNetworks();if(found<=0){WiFi.scanDelete();wifi.endForegroundScan();m.msg("Wi-Fi","No networks found","Try scanning again");continue;}int maxn=min(found,12);ListItem net[12];for(int x=0;x<maxn;x++){net[x].title=WiFi.SSID(x);net[x].value=wifiSecurityLabel(WiFi.encryptionType(x))+"  "+String(WiFi.RSSI(x))+" dBm";}int q=list.choose("Nearby Wi-Fi",net,maxn);
    if(q>=0){String ss=WiFi.SSID(q);wifi_auth_mode_t auth=WiFi.encryptionType(q);uint8_t sec=wifiSecurityClass(auth);String user="",pw="";int old=wifi.find(ss);bool cancel=false;
      if(sec==WIFI_SECURITY_ENTERPRISE){user=kbd.prompt("Wi-Fi username",old>=0?wifi.username(old):"",false,64,&cancel);if(cancel){WiFi.scanDelete();wifi.endForegroundScan();continue;}pw=kbd.prompt("Wi-Fi password",old>=0?wifi.password(old):"",true,64,&cancel);if(cancel){WiFi.scanDelete();wifi.endForegroundScan();continue;}}
      else if(sec==WIFI_SECURITY_PERSONAL){pw=kbd.prompt("Wi-Fi password",old>=0?wifi.password(old):"",true,64,&cancel);if(cancel){WiFi.scanDelete();wifi.endForegroundScan();continue;}}
      WiFi.scanDelete();wifi.endForegroundScan();d.clear();d.header("Wi-Fi");d.text(5,30,"Connecting to",d.fg(),1);d.text(5,45,ss,d.muted(),1,true,118);d.render();if(wifi.connect(ss,sec,user,pw)){s.lastWifi(ss);m.msg("Wi-Fi","Connected",ss);}else{wifi.remember(ss,sec,user,pw);s.lastWifi(ss);wifi.startBackground(ss);m.msg("Wi-Fi","Saved network","MellorsOS will keep trying in the background");}
    }else{WiFi.scanDelete();wifi.endForegroundScan();}
  }}
  void savedWifi(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;int n=wifi.count();if(!n){m.msg("Saved Wi-Fi","No saved networks");return;}ListItem it[MAX_SAVED_WIFI];for(int x=0;x<n;x++){it[x].title=wifi.ssid(x);it[x].value=wifiSecurityName(wifi.security(x))+(wifi.ssid(x)==s.lastWifi()?" • PREFERRED":"");}int c=list.choose("Saved Wi-Fi",it,n);if(c<0)return;ListItem act[3]={{"Connect",""},{"Forget",""},{"Back",""}};int a=list.choose(wifi.ssid(c),act,3);if(a==0){s.lastWifi(wifi.ssid(c));if(wifi.connectSaved(c)){m.msg("Wi-Fi","Connected",wifi.ssid(c));}else{wifi.startBackground(wifi.ssid(c));m.msg("Wi-Fi","Connecting in background",wifi.ssid(c));}}else if(a==1)wifi.forget(c);}}
  void sound(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;ListItem i[2]={{"Silent mode",boolText(s.silentMode())},{"Volume",String(s.volume())+"%"}};int c=list.choose("Sound",i,2);if(c<0)return;if(c==0)s.silentMode(!s.silentMode());else{s.volume(s.volume()>=100?0:s.volume()+10);quietBeep(84,80);}}}
  void lockPage(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;if(!s.pinEnabled()){ListItem off[2]={{"Enable 4-digit PIN","Locks at boot and after screen-off"},{"Back",""}};int c=list.choose("Lock screen",off,2);if(c<0||c==1)return;if(c==0&&pin.configure(true))m.msg("Lock screen","PIN enabled","No recovery if forgotten");}else{ListItem on[3]={{"Change PIN","Current PIN required"},{"Disable PIN","Current PIN required"},{"Back",""}};int c=list.choose("Lock screen",on,3);if(c<0||c==2)return;if(!pin.verifyCurrent(true)){m.msg("Lock screen","PIN not changed","Current PIN was not accepted");continue;}if(c==0){if(pin.configure(true))m.msg("Lock screen","PIN changed");}else if(c==1&&m.confirm("Disable PIN?","Device will no longer lock")){s.clearPin();m.msg("Lock screen","PIN disabled");}}}}
  void appManager(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;int n=apps.count();ListItem it[MAX_APPS];for(int x=0;x<n;x++){it[x].title=apps.at(x).meta.name;it[x].value=apps.at(x).system?"SYSTEM":perms.holder()==apps.at(x).meta.id?"SUPERUSER":"USER";}int c=list.choose("App Manager",it,n);if(c<0)return;manageApp(c);}}
  void manageApp(int ix){App&a=apps.at(ix);while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;ListItem it[5]={{"Permissions",a.system?"Inherited":"Manage"},{"Shown on Home",boolText(!apps.hidden(a.meta.id))},{"Erase app data",""},{"Clear cache",""},{a.dynamic?"Uninstall":"Built-in app",""}};int c=list.choose(a.meta.name,it,5);if(c<0)return;if(c==0)permissionPage(a);else if(c==1){if(a.meta.id!="settings")apps.setHidden(a.meta.id,!apps.hidden(a.meta.id));}else if(c==2){if(m.confirm("Erase app data?",a.meta.name))data.clear(a.meta.id);}else if(c==3)m.msg("Cache","Runtime cache clears","when the app closes");else if(c==4&&a.dynamic){if(perms.holder()==a.meta.id)m.msg("Uninstall blocked","Disable Superuser first");else if(m.confirm("Uninstall app?",a.meta.name)){perms.clearApp(a.meta.id);data.clear(a.meta.id);apps.remove(a.meta.id);return;}}}}
  void permissionPage(App&a){if(a.system){m.msg("Permissions","Built-in system app","All permissions inherited");return;}while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;ListItem it[PERM_COUNT];for(int p=0;p<PERM_COUNT;p++){it[p].title=PERM_LABELS[p];it[p].value=p==6?(perms.has(a,p)?"HOLDER":"APP REQUEST ONLY"):boolText(perms.has(a,p));}int c=list.choose("Permissions",it,PERM_COUNT);if(c<0)return;if(c==6)m.msg("Superuser","Settings cannot change it","App must request/relinquish");else perms.set(a,c,!perms.has(a,c));}}
  void storage(){ListItem it[4]={{"LittleFS used",String(LittleFS.usedBytes())+" B"},{"LittleFS total",String(LittleFS.totalBytes())+" B"},{"Stored apps",String(apps.count())},{"Format filesystem","DANGER"}};int c=list.choose("Storage & privacy",it,4);if(c==3&&m.confirm("Format filesystem?","Installed apps/files erased")){LittleFS.format();ESP.restart();}}
  void developer(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;ListItem i[4]={{"Developer mode",boolText(s.developerMode())},{"FPS overlay",boolText(s.showFPS())},{"MScript Guide","Home when Dev mode ON"},{"Terminal","Home when Dev mode ON"}};int c=list.choose("Developer options",i,4);if(c<0)return;if(c==0)s.developerMode(!s.developerMode());else if(c==1)s.showFPS(!s.showFPS());else m.msg("Developer tools","Enable Developer mode","then open from Home");}}
  void system(){while(1){if(G_HOME_REQUEST||G_RECENTS_REQUEST)return;ListItem i[7]={{"Operating system",String(OS_NAME)+" "+OS_VERSION},{"Language engine",ENGINE_VERSION},{"Platform","CyberPi ESP32"},{"Wi-Fi",WiFi.status()==WL_CONNECTED?WiFi.SSID():"Disconnected"},{"Reset settings",""},{"Factory reset","DANGER"},{"Restart",""}};int c=list.choose("System",i,7);if(c<0)return;if(c==4&&m.confirm("Reset settings?","Restore defaults")){s.reset();m.msg("Settings","Defaults restored");}else if(c==5&&m.confirm("Factory reset?","Erase apps, files, permissions")){LittleFS.format();perms.reset();data.reset();wifi.clearSaved();s.reset();ESP.restart();}else if(c==6)ESP.restart();}}
};

class Kernel{
  enum ScreenMode{MODE_HOME,MODE_APP,MODE_RECENTS};
  struct NavState{ScreenMode mode=MODE_HOME;int app=-1;};

  SettingsStore settings;Display display;Modal modal;Keyboard keyboard;PinPad pinPad;WifiManager wifi;Registry apps;Permissions perms;DataStore data;ListUI lists;MConnectServer mconnectServer;MellorsPeerServer peerServer;SystemSettingsUI settingsUi;MScript vm;
  MessageStore messageStore;PeerDiscovery peerDiscovery;FileManagerUI filesUi;NotesUI notesUi;MessagesUI messagesUi;CalculatorUI calculatorUi;
  InputState prev;ScreenMode mode=MODE_HOME;int current=-1,homeSel=0;unsigned long fpsAt=0;int frames=0,fps=0;bool screenOff=false,notificationVisible=false;
  int visibleIndices[MAX_APPS],visibleN=0;
  NavState history[16];int historyN=0;
  int recents[MAX_APPS],recentN=0,recentSel=0;

  void clearNavSignals(){G_HOME_REQUEST=false;G_RECENTS_REQUEST=false;G_CLOSE_REQUEST=false;G_SETTINGS_REQUEST=false;G_FILES_REQUEST=false;G_NOTES_REQUEST=false;G_CALC_REQUEST=false;G_MESSAGES_REQUEST=false;G_EDIT_FILE_REQUEST="";G_OPEN_REQUEST="";}

  void setupWifiWizard(){
    if(!modal.confirm("Wi-Fi setup","Set up Wi-Fi now?","You can do this later"))return;
    while(true){
      display.clear();display.header("Wi-Fi",18);display.setClip(18,128);display.text(8,45,"Scanning...",display.fg(),1);display.resetClip();display.render();wifi.beginForegroundScan();int found=WiFi.scanNetworks();int maxn=min(found,12);if(maxn<=0){WiFi.scanDelete();wifi.endForegroundScan();if(!modal.confirm("Wi-Fi","No networks found","Scan again?"))return;continue;}ListItem net[12];for(int i=0;i<maxn;i++){net[i].title=WiFi.SSID(i);net[i].value=wifiSecurityLabel(WiFi.encryptionType(i))+"  "+String(WiFi.RSSI(i))+" dBm";}int c=lists.choose("Choose Wi-Fi",net,maxn);if(c<0){WiFi.scanDelete();wifi.endForegroundScan();return;}String ss=WiFi.SSID(c);uint8_t sec=wifiSecurityClass(WiFi.encryptionType(c));String user="",pw="";bool cancel=false;if(sec==WIFI_SECURITY_ENTERPRISE){user=keyboard.prompt("Wi-Fi username","",false,64,&cancel);if(cancel){WiFi.scanDelete();wifi.endForegroundScan();return;}pw=keyboard.prompt("Wi-Fi password","",true,64,&cancel);if(cancel){WiFi.scanDelete();wifi.endForegroundScan();return;}}else if(sec==WIFI_SECURITY_PERSONAL){pw=keyboard.prompt("Wi-Fi password","",true,64,&cancel);if(cancel){WiFi.scanDelete();wifi.endForegroundScan();return;}}WiFi.scanDelete();wifi.endForegroundScan();wifi.remember(ss,sec,user,pw);settings.lastWifi(ss);wifi.startBackground(ss);modal.msg("Wi-Fi","Network saved","Connecting in the background");return;
    }
  }

  void initialSetup(){
    if(settings.setupVersion().length()&&settings.wifiHostname().length())return;
    clearNavSignals();
    modal.msg("Welcome","Set up MellorsOS","Accent + hostname");

    while(true){
      ListItem items[ACCENT_COUNT];for(int i=0;i<ACCENT_COUNT;i++){items[i].title=ACCENTS[i].name;items[i].value=ACCENTS[i].hex;}
      int c=lists.choose("Accent color",items,ACCENT_COUNT);
      if(c>=0){settings.accent(ACCENTS[c].hex);break;}
      clearNavSignals();
    }

    String initial=settings.wifiHostname();if(!initial.length())initial=defaultHostname();
    while(true){
      bool cancel=false;String h=keyboard.prompt("Wi-Fi hostname",initial,false,32,&cancel);
      h=safeHostname(h);
      if(!cancel&&h.length()){settings.wifiHostname(h);wifi.setHostname(h);break;}
      clearNavSignals();
    }
    setupWifiWizard();
    modal.msg("MConnect","Store browsing works without a PIN","A lock PIN protects Settings and app installation.");
    if(modal.confirm("Screen lock","Set a 4-digit PIN now?","Needed for Remote Settings and installs; no recovery if forgotten"))pinPad.configure(false);
    settings.setupVersion(FIRMWARE_BUILD);
    modal.msg("Setup complete","Hostname",settings.wifiHostname());
    clearNavSignals();
  }

  void addBuiltins(){apps.addBuiltin(APP_SETTINGS,true,false);apps.addBuiltin(APP_FILES,true,false);apps.addBuiltin(APP_NOTES,true,false);apps.addBuiltin(APP_MESSAGES,true,false);apps.addBuiltin(APP_CALCULATOR,true,false);apps.addBuiltin(APP_MCONNECT,true,false);apps.addBuiltin(APP_SYSTEM,true,false);apps.addBuiltin(APP_TERMINAL,true,true);apps.addBuiltin(APP_GUIDE,true,true);}
  void rebuildVisible(){apps.buildVisible(visibleIndices,visibleN);if(homeSel>=visibleN)homeSel=max(0,visibleN-1);}

  void pushCurrent(){
    NavState s;s.mode=mode;s.app=current;
    if(historyN>0&&history[historyN-1].mode==s.mode&&history[historyN-1].app==s.app)return;
    if(historyN>=16){for(int i=1;i<16;i++)history[i-1]=history[i];historyN=15;}
    history[historyN++]=s;
  }

  void addRecent(int ix){
    if(ix<0)return;int at=-1;for(int i=0;i<recentN;i++)if(recents[i]==ix)at=i;
    if(at>=0){for(int i=at;i<recentN-1;i++)recents[i]=recents[i+1];recentN--;}
    if(recentN<MAX_APPS)recents[recentN++]=ix;
    else{for(int i=1;i<MAX_APPS;i++)recents[i-1]=recents[i];recents[MAX_APPS-1]=ix;}
    recentSel=max(0,recentN-1);
  }
  void removeRecent(int ix){
    int at=-1;for(int i=0;i<recentN;i++)if(recents[i]==ix)at=i;
    if(at<0)return;for(int i=at;i<recentN-1;i++)recents[i]=recents[i+1];recentN--;
    if(recentN<=0)recentSel=0;else recentSel=constrain(recentSel,0,recentN-1);
  }

  void pauseCurrent(){
    if(mode==MODE_APP&&current>=0&&vm.isOpen(current)){vm.trigger(current,"homeButton");vm.pause(current);}
  }

  void goHome(bool clearHistory=false){
    if(mode==MODE_APP&&current>=0&&vm.isOpen(current))vm.pause(current);
    if(clearHistory)historyN=0;mode=MODE_HOME;current=-1;rebuildVisible();drawHome();
  }

  void goPrevious(){
    if(mode==MODE_APP&&current>=0&&vm.isOpen(current))vm.pause(current);
    if(historyN<=0){goHome(false);return;}
    NavState s=history[--historyN];
    if(s.mode==MODE_APP&&s.app>=0&&s.app<apps.count()&&vm.isOpen(s.app)){
      mode=MODE_APP;current=s.app;vm.resume(current);renderApp();
    }else if(s.mode==MODE_RECENTS){mode=MODE_RECENTS;current=-1;drawRecents();}
    else{mode=MODE_HOME;current=-1;rebuildVisible();drawHome();}
  }

  int contentTop(AppView&v,String appTitle){
    int headerH=max(18,5+display.wrappedLines(appTitle,120,1)*display.lineH(1));
    int y=headerH+2;if(v.subtitle.length())y+=display.textHeight(v.subtitle,120,1)+2;return min(120,y);
  }
  String elementMain(UIElement&e){
    String line=e.title;
    if(e.type==UIElement::UI_INPUT)line=e.title+": "+e.value;
    else if(e.type==UIElement::UI_TOGGLE)line=e.title+"\n"+boolText(e.state);
    else if(e.type==UIElement::UI_SLIDER)line=e.title+"\n"+String(e.number);
    else if(e.type==UIElement::UI_SELECT)line=e.title+"\n"+e.value;
    return line;
  }
  int elementHeight(UIElement&e){
    if(e.type==UIElement::UI_SPACER)return max(1,e.height);
    if(e.type==UIElement::UI_DIVIDER)return 7;
    int lines=display.wrappedLines(elementMain(e),114,1);
    if(e.body.length())lines+=display.wrappedLines(e.body,114,1);
    int minH=(e.type==UIElement::UI_TEXT||e.type==UIElement::UI_SECTION||e.type==UIElement::UI_LABEL||e.type==UIElement::UI_CARD)?14:(settings.largeTargets()?30:22);
    return max(minH,6+lines*display.lineH(1));
  }
  int selectedStart(AppView&v){
    int y=0;for(int i=0;i<v.selected&&i<v.n;i++)if(v.e[i].visible)y+=elementHeight(v.e[i]);return y;
  }

  void drawHome(){
    display.clear();int hh=max(18,5+display.wrappedLines(settings.homeTitle(),120,1)*display.lineH(1));display.header(settings.homeTitle(),hh);
    int cols=max(1,settings.gridCols()),iconPx=settings.iconSize(),scale=iconPx>=28?2:1,cellW=128/cols;
    int labelH=settings.showAppLabels()?display.lineH(1)+2:0;
    int cellH=max(36,16*scale+8+labelH),startY=hh+3,rowsVisible=max(1,(128-startY)/cellH);
    int selectedRow=homeSel/cols,rowScroll=max(0,selectedRow-rowsVisible+1),startIndex=rowScroll*cols;
    display.setClip(startY,128);
    for(int k=0;k<rowsVisible*cols;k++){int vi=startIndex+k;if(vi>=visibleN)break;int ix=visibleIndices[vi],r=k/cols,c=k%cols,x=c*cellW,y=startY+r*cellH;bool sel=vi==homeSel;
      if(sel)display.outline(x+1,y+1,cellW-2,cellH-2,display.accent());int iw=16*scale;int iconY=y+3;display.icon(x+(cellW-iw)/2,iconY,apps.at(ix).icon,scale,settings.iconShape());
      if(settings.showAppLabels()){String nm=apps.at(ix).meta.name;int maxChars=max(3,(cellW-4)/6);if((int)nm.length()>maxChars)nm=nm.substring(0,max(1,maxChars-1))+"~";display.text(x+2,min(y+cellH-display.lineH(1)-1,iconY+iw+2),nm,sel?display.fg():display.muted(),1,false,cellW-4);}
    }
    display.resetClip();
    uint32_t statusColor=settings.highContrast()?display.fg():display.contrast(display.accent());
    int batteryPercent=settings.showBattery()?readBatteryPercent():-1;
    if(batteryPercent>=0){String bt=String(batteryPercent)+"%";int bx=max(84,124-(int)bt.length()*6);display.text(bx,5,bt,statusColor,1);}
    if(WiFi.status()==WL_CONNECTED){int wx=batteryPercent>=0?80:120;display.fill(wx,6,5,5,statusColor);}
    if(settings.showFPS())display.text(101,117,String(fps)+"f",display.muted(),1);display.render();
  }

  void showLaunchSplash(int ix){
    display.clear();int scale=3,iw=48;display.icon((128-iw)/2,24,apps.at(ix).icon,scale,settings.iconShape());
    String name=apps.at(ix).meta.name;int th=display.textHeight(name,118,1);int y=min(116-th,82);display.text(5,y,name,display.fg(),1,true,118);display.render();
    unsigned long st=millis();while(millis()-st<1000)delay(5);
  }

  void renderGameChrome(){
    if(current<0||vm.rawCanvas())return;AppView&v=vm.rt(current).view;String appTitle=v.title.length()?v.title:apps.at(current).meta.name;
    // Games own the 110-pixel body. Only refresh the fixed OS/app header so
    // normal UI rendering never destroys a game's back buffer.
    display.resetClip();display.header(appTitle,18);display.render();
  }

  void renderApp(){
    if(current<0||vm.rawCanvas())return;AppView&v=vm.rt(current).view;display.clear();
    if(v.n==0){
      if(v.title.length()||v.subtitle.length()){
        String t=v.title.length()?v.title:apps.at(current).meta.name;
        int h=max(18,5+display.wrappedLines(t,120,1)*display.lineH(1));
        display.header(t,h);
        if(v.subtitle.length())display.text(4,h+2,v.subtitle,display.muted(),1,true,120);
      }
      display.render();return;
    }
    String appTitle=v.title.length()?v.title:apps.at(current).meta.name;
    bool showHeader=apps.at(current).system||v.title.length()>0;int headerH=showHeader?max(18,5+display.wrappedLines(appTitle,120,1)*display.lineH(1)):0;if(showHeader)display.header(appTitle,headerH);
    int y=headerH+2;if(v.subtitle.length()){display.setClip(headerH,128);display.text(4,y,v.subtitle,display.muted(),1,true,120);y+=display.textHeight(v.subtitle,120,1)+2;}int contentTopY=min(124,y);
    int total=0;for(int i=0;i<v.n;i++)if(v.e[i].visible){v.e[i].height=elementHeight(v.e[i]);total+=v.e[i].height;}
    if(v.n<=0){display.resetClip();display.render();return;}
    v.selected=constrain(v.selected,0,v.n-1);
    int sy=selectedStart(v),sh=v.e[v.selected].visible?v.e[v.selected].height:0,viewH=max(1,128-contentTopY);
    if(sh>viewH){int maxScroll=sy+sh-viewH;if(v.scroll<sy||v.scroll>maxScroll)v.scroll=sy;v.scroll=constrain(v.scroll,sy,maxScroll);}
    else{if(sy<v.scroll)v.scroll=sy;if(sy+sh>v.scroll+viewH)v.scroll=sy+sh-viewH;v.scroll=max(0,min(v.scroll,max(0,total-viewH)));}
    int cy=contentTopY-v.scroll;display.setClip(contentTopY,128);
    for(int i=0;i<v.n;i++){UIElement&e=v.e[i];if(!e.visible)continue;int h=e.height;
      if(cy+h>contentTopY&&cy<128){bool sel=i==v.selected;uint32_t tc=display.fg();
        if(e.type==UIElement::UI_DIVIDER)display.fill(5,cy+h/2,118,1,display.muted());
        else if(e.type==UIElement::UI_SPACER){}
        else if(e.type==UIElement::UI_SECTION){if(sel)display.outline(2,cy,124,h,display.accent());display.text(5,cy+3,e.title,display.accent(),1,true,114);}
        else if(e.type==UIElement::UI_TEXT||e.type==UIElement::UI_LABEL||e.type==UIElement::UI_CARD){
          if(sel)display.outline(2,cy,124,h,display.accent());int ty=cy+3;display.text(5,ty,e.title,tc,1,true,114);ty+=display.textHeight(e.title,114,1);
          if(e.body.length())display.text(5,ty,e.body,display.muted(),1,true,114);
        }else{
          uint32_t bgc=sel?display.accent():display.panel();display.fill(3,cy+1,122,h-2,bgc);tc=sel?display.contrast(display.accent()):display.fg();int ty=cy+3;
          String line=elementMain(e);display.text(6,ty,line,tc,1,true,114);ty+=display.textHeight(line,114,1);
          if(e.body.length())display.text(6,ty,e.body,sel?tc:display.muted(),1,true,114);
        }
      }cy+=h;
    }
    display.resetClip();display.render();
  }

  void drawRecents(){
    display.clear();display.header("Recents");
    if(recentN<=0){display.text(9,48,"No recent apps",display.fg(),1,true,110);display.text(9,110,"HOME: back",display.muted(),1);display.render();return;}
    recentSel=constrain(recentSel,0,recentN-1);int ix=recents[recentSel];AppView&v=vm.rt(ix).view;
    display.fill(8,24,112,94,display.panel());display.outline(8,24,112,94,display.accent());display.icon(48,31,apps.at(ix).icon,2,settings.iconShape());
    display.text(13,66,apps.at(ix).meta.name,display.fg(),1,true,102);
    String preview=v.title.length()?v.title:v.subtitle;if(!preview.length()&&v.n>0)preview=elementMain(v.e[0]);if(preview.length())display.text(13,82,preview,display.muted(),1,true,102);
    display.text(12,108,"< > browse  UP close",display.muted(),1,true,104);display.text(105,2,String(recentSel+1)+"/"+recentN,settings.highContrast()?display.fg():display.contrast(display.accent()),1);display.render();
  }

  void openRecents(bool push=true){
    if(push)pushCurrent();if(mode==MODE_APP&&current>=0&&vm.isOpen(current)){vm.trigger(current,"recentsButton");vm.pause(current);}
    mode=MODE_RECENTS;current=-1;if(recentN>0)recentSel=recentN-1;drawRecents();
  }

  void launchFresh(int ix,bool push=true){
    if(ix<0||ix>=apps.count())return;if(push)pushCurrent();if(mode==MODE_APP&&current>=0&&vm.isOpen(current))vm.pause(current);
    if(vm.isOpen(ix))vm.close(ix);mode=MODE_APP;current=ix;addRecent(ix);
    display.clear();display.render();if(!vm.launch(ix)){modal.msg("App error","Could not launch");goPrevious();return;}handleKernelSignal();if(mode==MODE_APP&&current==ix){if(vm.gameOwnsDisplay())renderGameChrome();else renderApp();}
  }

  void resumeRecent(int ix,bool push=true){
    if(ix<0||ix>=apps.count()||!vm.isOpen(ix)){launchFresh(ix,push);return;}if(push)pushCurrent();
    if(mode==MODE_APP&&current>=0&&current!=ix&&vm.isOpen(current))vm.pause(current);mode=MODE_APP;current=ix;addRecent(ix);vm.resume(ix);handleKernelSignal();if(mode==MODE_APP&&current==ix){if(vm.gameOwnsDisplay())renderGameChrome();else renderApp();}
  }

  void closeCurrentAndBack(){
    if(mode==MODE_APP&&current>=0){int ix=current;vm.close(ix);removeRecent(ix);}goPrevious();
  }

  void handleKernelSignal(){
    if(G_HOME_REQUEST){G_HOME_REQUEST=false;goHome(true);return;}
    if(G_RECENTS_REQUEST){G_RECENTS_REQUEST=false;openRecents(true);return;}
    if(G_CLOSE_REQUEST){G_CLOSE_REQUEST=false;closeCurrentAndBack();return;}
    if(G_SETTINGS_REQUEST){
      G_SETTINGS_REQUEST=false;int result=settingsUi.run();
      if(G_HOME_REQUEST){G_HOME_REQUEST=false;goHome(true);return;}
      if(G_RECENTS_REQUEST){G_RECENTS_REQUEST=false;openRecents(true);return;}
      if(mode==MODE_APP&&current>=0){int ix=current;vm.close(ix);removeRecent(ix);}goPrevious();return;
    }
    if(G_FILES_REQUEST){G_FILES_REQUEST=false;filesUi.browse("/");if(G_HOME_REQUEST){G_HOME_REQUEST=false;goHome(true);return;}if(G_RECENTS_REQUEST){G_RECENTS_REQUEST=false;openRecents(true);return;}if(mode==MODE_APP&&current>=0){int ix=current;vm.close(ix);removeRecent(ix);}goPrevious();return;}
    if(G_NOTES_REQUEST){G_NOTES_REQUEST=false;notesUi.run();if(G_HOME_REQUEST){G_HOME_REQUEST=false;goHome(true);return;}if(G_RECENTS_REQUEST){G_RECENTS_REQUEST=false;openRecents(true);return;}if(mode==MODE_APP&&current>=0){int ix=current;vm.close(ix);removeRecent(ix);}goPrevious();return;}
    if(G_CALC_REQUEST){G_CALC_REQUEST=false;calculatorUi.run();if(G_HOME_REQUEST){G_HOME_REQUEST=false;goHome(true);return;}if(G_RECENTS_REQUEST){G_RECENTS_REQUEST=false;openRecents(true);return;}if(mode==MODE_APP&&current>=0){int ix=current;vm.close(ix);removeRecent(ix);}goPrevious();return;}
    if(G_MESSAGES_REQUEST){G_MESSAGES_REQUEST=false;messagesUi.run();if(G_HOME_REQUEST){G_HOME_REQUEST=false;goHome(true);return;}if(G_RECENTS_REQUEST){G_RECENTS_REQUEST=false;openRecents(true);return;}if(mode==MODE_APP&&current>=0){int ix=current;vm.close(ix);removeRecent(ix);}goPrevious();return;}
    if(G_EDIT_FILE_REQUEST.length()){String p=G_EDIT_FILE_REQUEST;G_EDIT_FILE_REQUEST="";notesUi.editPath(p);if(G_HOME_REQUEST){G_HOME_REQUEST=false;goHome(true);return;}if(G_RECENTS_REQUEST){G_RECENTS_REQUEST=false;openRecents(true);return;}return;}
    if(G_OPEN_REQUEST.length()){String id=G_OPEN_REQUEST;G_OPEN_REQUEST="";int ix=apps.find(id);if(ix>=0)launchFresh(ix,true);return;}
  }

  void redrawCurrent(){if(mode==MODE_HOME)drawHome();else if(mode==MODE_RECENTS)drawRecents();else if(mode==MODE_APP&&current>=0){if(vm.gameOwnsDisplay())renderGameChrome();else renderApp();}}

  void drawNotificationBar(){
    uint32_t c=display.accent();display.resetClip();display.fill(0,0,128,18,c);String line=G_NOTIFICATION_TITLE;if(G_NOTIFICATION_BODY.length())line+=" "+G_NOTIFICATION_BODY;display.text(3,4,line,display.contrast(c),1,false,122);display.render();notificationVisible=true;
  }

  void notificationCenter(){
    while(!G_HOME_REQUEST&&!G_RECENTS_REQUEST){
      ListItem rows[MAX_NOTIFICATIONS+1];Notification snapshot[MAX_NOTIFICATIONS];int count=vm.notifications();
      rows[0]={count?"Clear all":"No notifications",count?String(count)+" stored":""};
      for(int i=0;i<count;i++){Notification n=vm.notification(i);snapshot[i]=n;rows[i+1]={n.title,settings.notificationPreview()?n.body:String("Preview hidden")};}
      int c=lists.choose("Notifications",rows,count+1);if(c<0)return;
      if(c==0){if(count&&modal.confirm("Clear notifications?","Remove all stored notices"))vm.clearNotifications();}
      else {Notification n=snapshot[c-1];modal.msg(n.title,n.body,n.app);ListItem actions[2]={{"Delete notification",""},{"Keep",""}};if(lists.choose("Notification",actions,2)==0)vm.deleteNotificationId(n.id);}
      notificationVisible=false;
    }
  }
  void drawQuickSettings(int sel){
    display.clear();display.header("Quick settings",18);
    String rows[5]={String("Wi-Fi  ")+(settings.wifiEnabled()?"ON":"OFF"),String("Silent  ")+(settings.silentMode()?"ON":"OFF"),"Power  screen off",String("Volume  ")+settings.volume()+"%",String("Notifications  ")+vm.notifications()};
    for(int i=0;i<5;i++){int y=20+i*19;bool on=i==sel;display.fill(4,y,120,17,on?display.accent():display.panel());display.text(8,y+3,rows[i],on?display.contrast(display.accent()):display.fg(),1,false,112);}
    display.text(4,119,"B/Home close",display.muted(),1);display.render();
  }
  void sleepDisplay(){screenOff=true;notificationVisible=false;quietSoundStop();prev=readInput();cyber.set_lcd_light(false);}
  void wakeDisplay(){cyber.set_lcd_light(true);screenOff=false;released();if(settings.pinEnabled())pinPad.unlock();redrawCurrent();}
  void quickSettings(){int sel=0;released();InputState p;drawQuickSettings(sel);while(true){if(G_HOME_REQUEST||G_RECENTS_REQUEST){released();return;}auto n=readInput();Action a=inputAction(n,p);p=n;if(a==UP){sel=(sel+4)%5;drawQuickSettings(sel);}else if(a==DOWN){sel=(sel+1)%5;drawQuickSettings(sel);}else if((a==LEFT||a==RIGHT)&&sel==3){settings.volume(constrain(settings.volume()+(a==RIGHT?10:-10),0,100));drawQuickSettings(sel);}else if(a==SELECT){if(sel==0){bool en=!settings.wifiEnabled();settings.wifiEnabled(en);if(en)wifi.startBackground(settings.lastWifi());else wifi.disable();drawQuickSettings(sel);}else if(sel==1){settings.silentMode(!settings.silentMode());drawQuickSettings(sel);}else if(sel==2){sleepDisplay();return;}else if(sel==3){settings.volume(settings.volume()>=100?0:settings.volume()+10);drawQuickSettings(sel);}else{notificationCenter();drawQuickSettings(sel);}}else if(a==BACK||a==HOME_SINGLE||a==HOME_HOLD){released();return;}backgroundServiceHook();delay(12);}}


  void processIncomingMessages(){
    if(!G_PEER_PENDING)return;
    char msgBuf[384],senderBuf[64];
    strncpy(msgBuf,G_PEER_MESSAGE,sizeof(msgBuf)-1);msgBuf[sizeof(msgBuf)-1]=0;
    strncpy(senderBuf,G_PEER_SENDER,sizeof(senderBuf)-1);senderBuf[sizeof(senderBuf)-1]=0;
    G_PEER_PENDING=false;String msg(msgBuf),sender(senderBuf);if(!sender.length())sender="peer";
    messageStore.append(sender,false,msg);
    if(!G_SCRIPT_PEER_PENDING){
      strncpy(G_SCRIPT_PEER_MESSAGE,msgBuf,sizeof(G_SCRIPT_PEER_MESSAGE)-1);G_SCRIPT_PEER_MESSAGE[sizeof(G_SCRIPT_PEER_MESSAGE)-1]=0;
      strncpy(G_SCRIPT_PEER_SENDER,senderBuf,sizeof(G_SCRIPT_PEER_SENDER)-1);G_SCRIPT_PEER_SENDER[sizeof(G_SCRIPT_PEER_SENDER)-1]=0;
      G_SCRIPT_PEER_PENDING=true;
    }
    if(settings.notificationToasts()){G_NOTIFICATION_TITLE=String("Message from ")+sender;G_NOTIFICATION_BODY=settings.notificationPreview()?msg:String("");G_NOTIFICATION_UNTIL=millis()+3200;G_NOTIFICATION_DIRTY=true;}
    if(!settings.silentMode()&&settings.volume()>0)quietBeep(84,90);
  }

  void activate(UIElement&e){
    if(!e.enabled)return;
    if(e.id=="__settings"){G_SETTINGS_REQUEST=true;handleKernelSignal();return;}
    if(e.type==UIElement::UI_INPUT){bool cancel=false;String v=keyboard.prompt(e.title,e.value,e.password,96,&cancel);if(!cancel){e.value=v;vm.trigger(current,"input",e.id,Value(v));vm.trigger(current,"input","",Value(v));}}
    else if(e.type==UIElement::UI_TOGGLE){e.state=!e.state;vm.trigger(current,"change",e.id,Value(e.state));vm.trigger(current,"change","",Value(e.state));}
    else if(e.type==UIElement::UI_SLIDER){e.number+=e.step;if(e.number>e.maxValue)e.number=e.minValue;vm.trigger(current,"change",e.id,Value(e.number));}
    else if(e.type==UIElement::UI_SELECT){String opts=e.options;int count=opts.length()?1:0;for(size_t i=0;i<opts.length();i++)if(opts[i]=='|')count++;if(count){int pos=0,found=-1;String values[12],labels[12];for(int i=0;i<count&&i<12;i++){int q=opts.indexOf('|',pos);String z=q<0?opts.substring(pos):opts.substring(pos,q);int c=z.indexOf(':');values[i]=c>=0?z.substring(0,c):z;labels[i]=c>=0?z.substring(c+1):z;if(values[i]==e.value)found=i;pos=q<0?opts.length()+1:q+1;}found=(found+1)%min(count,12);e.value=values[found];vm.trigger(current,"change",e.id,Value(e.value));}}
    else if(e.type==UIElement::UI_MENU&&e.filePicker){
      App&owner=apps.at(current);bool global=owner.system||perms.has(owner,4)||perms.has(owner,6);String base=global?String("/"):String("/apps/")+safeName(owner.meta.id);ensureFolder("/apps");if(!global)ensureFolder(base);
      String chosen=filesUi.pickFile(base,base);if(chosen.length()){String eventPath=chosen;if(!global&&chosen.startsWith(base+"/"))eventPath=chosen.substring(base.length()+1);e.value=eventPath;vm.trigger(current,"file",e.id,Value(eventPath),eventPath);}
    }
    else if(e.type==UIElement::UI_BUTTON||e.type==UIElement::UI_MENU){vm.trigger(current,"click",e.id);vm.trigger(current,"click");}
    handleKernelSignal();if(mode==MODE_APP&&current>=0)renderApp();
  }

public:
  Kernel():display(settings),modal(display),keyboard(display),pinPad(display,settings),apps(display,settings),perms(modal),lists(display),mconnectServer(settings,apps,wifi,perms,data),peerServer(wifi),settingsUi(display,modal,keyboard,lists,settings,wifi,perms,apps,data,mconnectServer,pinPad),vm(display,modal,keyboard,settings,apps,perms,data,wifi),filesUi(display,modal,keyboard,lists),notesUi(display,modal,keyboard,lists),messagesUi(display,modal,keyboard,lists,wifi,messageStore,peerDiscovery),calculatorUi(display,modal,keyboard,lists){vm.attach(this);}
  void begin(){
    Serial.begin(115200);delay(200);Serial.printf("[MellorsOS] reset reason=%d\n",(int)esp_reset_reason());cyber.begin();quietMakeblockTasks();
    bool freshFirmware=prepareFirmwareInstall();
    settings.begin();
    if(freshFirmware)Serial.println("[MellorsOS] New firmware image: existing data preserved.");
    String bootHost=settings.wifiHostname();if(!bootHost.length())bootHost=defaultHostname();
    wifi.begin(bootHost);
    if(!settings.wifiEnabled())wifi.disable();
    initialSetup();
    perms.begin();data.begin();
    if(!vm.beginMemory()){display.clear();display.text(9,43,"MellorsOS memory",display.fg(),1);display.text(9,58,"PSRAM unavailable",display.fg(),1);display.text(9,73,"Enable PSRAM",display.muted(),1);display.render();while(true)delay(1000);}
    if(!LittleFS.exists("/apps"))LittleFS.mkdir("/apps");if(!LittleFS.exists("/installed"))LittleFS.mkdir("/installed");if(!LittleFS.exists("/exports"))LittleFS.mkdir("/exports");if(!LittleFS.exists("/notes"))LittleFS.mkdir("/notes");if(!LittleFS.exists("/messages"))LittleFS.mkdir("/messages");if(!LittleFS.exists("/incoming"))LittleFS.mkdir("/incoming");
    addBuiltins();apps.scanInstalled();
    display.clear();display.text(31,48,"MellorsOS",display.fg(),1);display.text(45,62,"v2.0",display.muted(),1);display.render();delay(settings.reduceMotion()?150:500);
    bool old=settings.bootedBefore();settings.markBooted();
    if(old&&settings.pinEnabled())pinPad.unlock();
    if(settings.wifiEnabled())wifi.startBackground(settings.lastWifi());
    messageStore.begin();G_BACKGROUND_READY=true;goHome(true);backgroundTick();
  }

  void backgroundTick(){
    if(!G_BACKGROUND_READY)return;
    serviceQuietHardware();
    static bool lastWifiConnected=false;bool beforeWifi=WiFi.status()==WL_CONNECTED;wifi.backgroundTick();bool afterWifi=WiFi.status()==WL_CONNECTED;if(afterWifi!=lastWifiConnected){lastWifiConnected=afterWifi;if(mode==MODE_HOME&&!screenOff)drawHome();}

    // MConnect is intentionally foreground-only. Service it on every kernel pass
    // while the MConnect app is open, so its local web UI feels responsive and
    // large app uploads are not starved by the background LAN cadence.
    bool mconnectOpen=mode==MODE_APP&&current>=0&&current<apps.count()&&apps.at(current).meta.id=="mconnect";
    mconnectServer.setRequested(mconnectOpen);
    if(mconnectOpen)mconnectServer.tick();

    // Peer messaging remains a background service and stays throttled so games do
    // not pay for network polling at the 1 ms game-mode cadence.
    static unsigned long lastPeerService=0;unsigned long now=millis();
    if((unsigned long)(now-lastPeerService)>=20){
      lastPeerService=now;
      peerServer.tick();
      processIncomingMessages();
    }
  }

  void tick(){
    backgroundTick();vm.tick();vm.pollHttpServer();handleKernelSignal();
    if(screenOff){auto n=readInput();bool wake=n.a&&!prev.a;prev=n;if(wake)wakeDisplay();else delay(40);return;}
    if(G_NOTIFICATION_DIRTY){G_NOTIFICATION_DIRTY=false;drawNotificationBar();}
    if(notificationVisible&&G_NOTIFICATION_UNTIL&&(long)(millis()-G_NOTIFICATION_UNTIL)>=0){notificationVisible=false;G_NOTIFICATION_UNTIL=0;redrawCurrent();}
    frames++;if(millis()-fpsAt>=1000){fps=frames;frames=0;fpsAt=millis();}
    auto now=readInput();Action a=inputAction(now,prev);prev=now;if(a==NONE){delay(vm.gameOwnsDisplay()?1:12);return;}
    if(a==HOME_HOLD){if(mode==MODE_APP&&current>=0)vm.trigger(current,"homeButton");goHome(true);return;}
    if(a==HOME_DOUBLE){openRecents(true);return;}
    if(a==HOME_SINGLE){if(mode==MODE_HOME)return;if(mode==MODE_APP&&current>=0)vm.trigger(current,"homeButton");goPrevious();return;}

    if(mode==MODE_HOME){
      if(!visibleN)return;
      int cols=max(1,settings.gridCols());int row=homeSel/cols,col=homeSel%cols;
      if(a==UP){
        if(row==0){quickSettings();if(!screenOff)drawHome();}
        else{int target=homeSel-cols;if(target>=0){homeSel=target;drawHome();}}
      }else if(a==DOWN){
        int target=homeSel+cols;if(target<visibleN){homeSel=target;drawHome();}
      }else if(a==LEFT){
        int rowStart=row*cols;if(homeSel>rowStart){homeSel--;drawHome();}
      }else if(a==RIGHT){
        int rowEnd=min(visibleN-1,row*cols+cols-1);if(homeSel<rowEnd){homeSel++;drawHome();}
      }else if(a==SELECT)launchFresh(visibleIndices[homeSel],true);
      return;
    }

    if(mode==MODE_RECENTS){
      if(recentN<=0){if(a==BACK)goPrevious();return;}
      if(a==LEFT){recentSel=(recentSel+recentN-1)%recentN;drawRecents();}
      else if(a==RIGHT||a==DOWN){recentSel=(recentSel+1)%recentN;drawRecents();}
      else if(a==UP){int ix=recents[recentSel];vm.close(ix);removeRecent(ix);drawRecents();}
      else if(a==SELECT){int ix=recents[recentSel];resumeRecent(ix,true);}
      else if(a==BACK)goPrevious();
      return;
    }

    if(current<0)return;AppView&v=vm.rt(current).view;
    if(a==BACK){Value handled=vm.trigger(current,"back");if(!handled.truth())goPrevious();else if(vm.gameOwnsDisplay())renderGameChrome();else renderApp();return;}
    // While a game owns the display, directional/A input belongs to game.keyDown().
    // Do not run AppView navigation because it clears the game body to wallpaper.
    if(vm.gameOwnsDisplay())return;
    if(v.n<=0)return;
    v.selected=constrain(v.selected,0,v.n-1);
    String title=v.title.length()?v.title:apps.at(current).meta.name;int viewTop=contentTop(v,title),viewH=max(1,128-viewTop);
    UIElement&e=v.e[v.selected];int eh=elementHeight(e),sy=selectedStart(v);
    if(a==UP){
      if(eh>viewH&&v.scroll>sy){v.scroll=max(sy,v.scroll-display.lineH(1));renderApp();}
      else{v.selected=(v.selected+v.n-1)%v.n;renderApp();}
    }else if(a==DOWN){
      int maxScroll=sy+eh-viewH;
      if(eh>viewH&&v.scroll<maxScroll){v.scroll=min(maxScroll,v.scroll+display.lineH(1));renderApp();}
      else{v.selected=(v.selected+1)%v.n;renderApp();}
    }else if(a==LEFT){v.scroll=max(0,v.scroll-display.lineH(1));renderApp();}
    else if(a==RIGHT){v.scroll+=display.lineH(1);renderApp();}
    else if(a==SELECT)activate(v.e[v.selected]);
  }

  void requestHome(){goHome(true);}void requestOpen(String id){int ix=apps.find(id);if(ix>=0)launchFresh(ix,true);}void requestSettings(){G_SETTINGS_REQUEST=true;handleKernelSignal();}
};

static Kernel kernel;
static void backgroundServiceHook(){if(G_BACKGROUND_READY)kernel.backgroundTick();}
static TaskHandle_t G_KERNEL_TASK=nullptr;
static bool G_KERNEL_FALLBACK=false;
static void mellorsKernelTask(void*){
  kernel.begin();
  for(;;){kernel.tick();vTaskDelay(pdMS_TO_TICKS(1));}
}

// v0.9 runs the UI/runtime on a dedicated 16 KB task instead of Arduino's
// smaller loop-task stack. This prevents deep Files/MScript/UI paths from
// turning ordinary actions into stack-overflow reboots.
void mellorsOSSetup(){
  Serial.begin(115200);delay(40);
  BaseType_t ok=xTaskCreatePinnedToCore(mellorsKernelTask,"mellors_kernel",32768,nullptr,2,&G_KERNEL_TASK,1);
  if(ok!=pdPASS){G_KERNEL_FALLBACK=true;Serial.println("[MellorsOS] Kernel task allocation failed; using loop task");kernel.begin();}
}
void mellorsOSLoop(){if(G_KERNEL_FALLBACK)kernel.tick();else delay(100);}

