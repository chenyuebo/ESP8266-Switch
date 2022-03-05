/*
 Name:    ESP8266_Switch.cpp
 Created: 2021/8/21 6:58:45
 Author:  ChenYueBo
 智能开关，ESP-01s添加开发的扩展板控制舵机实现对传统开关控制

 IO0通过MOS管控制舵机电源，防止不使用舵机时不合格舵机抖动
 IO2舵机PWM控制引脚
 硬件电路V1.1版本不带按钮
 硬件电路V1.2版本带按钮，RX引脚为按钮输入
 TX打印串口数据
*/

#include <Arduino.h>
#include <Ticker.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Servo.h>
#include <NTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <FS.h>
#include <OneButton.h>

Ticker ticker;
WiFiUDP Udp;
unsigned int localUdpPort = 1234;   //
char buffer[255];                   // UDP缓冲区
char replay[] = "Got the message."; //

String ssid = "";
String password = "";

int IO0 = 0; // 舵机电源MOS管控制
int IO2 = 2; // 舵机PWM控制引脚
int IO3 = 3; // 按键
OneButton btn = OneButton(IO3, true, true);

byte enableLED = 0;          // 是否开启夜灯
int brightness = 100;        // 小夜灯LED亮度
int servo_reset_angle = 90;  // 舵机复位转动的角度
int servo_open_angle = 45;   // 舵机执行打开转动的角度
int servo_close_angle = 130; // 舵机执行关闭转动的角度
int servo_hold_ms = 1000;    // 舵机多长时间复位
Servo servo;

unsigned long autoResetTime = 1000;       // 舵机未收到新指令，1s后自动归位
unsigned long autoReleaseTime = 5 * 1000; // 舵机未收到新指令，5s自动释放
unsigned long lastServoUseTime = 0;       // 舵机开灯或者关灯的时间

WiFiUDP ntpUDP;
unsigned long epochTime;                                                   // 当前的时间，距1970-01-01的秒数
unsigned long timeOffset = 3600 * 8;                                       // 北京时间+8
unsigned long updateInterval = 5 * 60 * 1000;                              // 每5分钟同步一次服务器时间
NTPClient ntpClient(ntpUDP, "time.apple.com", timeOffset, updateInterval); // 时区8小时
unsigned long lastPrintTime = 0;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
unsigned long lastConnectMQTTTime = 0;

// dev004 测试设备
const char *productId = "";
const char *productSecret = "";
const char *mqtt_server = "";
const char *clientId = "";
const char *mqttUserName = "";
const char *mqttPassword = "";

char buffer_mq[512];

String readFile(const char *path)
{
  Serial.printf("Reading file: %s\n", path);
  File file = LittleFS.open(path, "r");
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return "";
  }
  Serial.print("Read from file: ");
  String content = file.readString();
  Serial.println(content);
  file.close();
  return content;
}

void writeFile(const char *path, const char *content)
{
  Serial.printf("Writing file: %s\n", path);
  File file = LittleFS.open(path, "w");
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }
  if (file.print(content))
  {
    Serial.println("File written");
  }
  else
  {
    Serial.println("Write failed");
  }
  file.close();
}

void readConfig()
{
  String config = readFile("/config.txt");
  if (config.length() > 0)
  {
    StaticJsonDocument<512> doc;
    deserializeJson(doc, config);
    JsonObject obj = doc.as<JsonObject>();
    if (obj.containsKey("wifi_ssid"))
    {
      ssid = obj["wifi_ssid"].as<String>();
      password = obj["wifi_password"].as<String>();
      enableLED = obj["open_light"];
      brightness = obj["brightness"];
      servo_reset_angle = obj["servo_reset_angle"];
      servo_open_angle = obj["servo_open_angle"];
      servo_close_angle = obj["servo_close_angle"];
      servo_hold_ms = obj["servo_hold_time"];
    }
  }
}

void writeConfig()
{
  StaticJsonDocument<512> doc;                  //
  doc["wifi_ssid"] = ssid;                      //
  doc["wifi_password"] = password;              //
  doc["open_light"] = enableLED;                // 是否开启小夜灯
  doc["brightness"] = 100;                      // 小夜灯亮度
  doc["servo_reset_angle"] = servo_reset_angle; //
  doc["servo_open_angle"] = servo_open_angle;   //
  doc["servo_close_angle"] = servo_close_angle; //
  doc["servo_hold_time"] = servo_hold_ms;       //
  String config;
  serializeJson(doc, config);
  const char *config_c_str = config.c_str();
  Serial.printf("write config: %s\n", config_c_str);
  writeFile("/config.txt", config_c_str);
}

void handleClick()
{
  Serial.println("handleClick()");
}

void handleDoubleClick()
{
  Serial.println("handleDoubleClick()");
}

void handleLongClick()
{
  Serial.println("handleLongClick()");
}

void syncServerTime();
void readUdp();
void checkAction();

void openLED();
void closeLED();
void checkLEDState();

void openServoPower();
void closeServoPower();
void servoPushUp();
void servoPushDown();

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  if (length < 512)
  {
    for (int i = 0; i < length; i++)
    {
      char c = (char)payload[i];
      buffer_mq[i] = c;
    }
    buffer_mq[length] = 0;
  }

  StaticJsonDocument<512> doc;
  deserializeJson(doc, buffer_mq);
  JsonObject obj = doc.as<JsonObject>();
  String method = obj["method"];
  Serial.printf("method=%s\n", method.c_str());
  if (method.equals("control"))
  {
    if (obj.containsKey("params"))
    {
      JsonObject obj_params = obj.getMember("params");
      if (obj_params.containsKey("open_light")) // 设置小夜灯状态
      {
        int open_light = obj_params["open_light"];
        Serial.printf("open_light=%d\n", open_light);
        enableLED = (open_light == 1) ? 1 : 0;
        writeConfig();
      }
      else if (obj_params.containsKey("servo_open")) // 舵机执行打开操作
      {
        int servo_open = obj_params["servo_open"];
        Serial.printf("servo_open=%d\n", servo_open);
        lastServoUseTime = millis();
        openServoPower();
        servoPushUp();
      }
      else if (obj_params.containsKey("servo_close")) // 舵机执行关闭操作
      {
        int servo_close = obj_params["servo_close"];
        Serial.printf("servo_close=%d\n", servo_close);
        lastServoUseTime = millis();
        openServoPower();
        servoPushDown();
      }
      else if (obj_params.containsKey("servo_hold_second")) // 舵机持续时间设置
      {
        int second = obj_params["servo_hold_second"];
        servo_hold_ms = second * 1000;
        writeConfig();
      }
      else if (obj_params.containsKey("servo_reset_angle")) // 舵机复位时需要转的角度
      {
        servo_reset_angle = obj_params["servo_reset_angle"];
        writeConfig();
      }
      else if (obj_params.containsKey("servo_open_angle")) // 舵机执行打开动作时需要转的角度
      {
        servo_open_angle = obj_params["servo_open_angle"];
        writeConfig();
      }
      else if (obj_params.containsKey("servo_close_angle")) // 舵机执行关闭动作时需要转的角度
      {
        servo_close_angle = obj_params["servo_close_angle"];
        writeConfig();
      }
    }
  }
}

void connectMqtt()
{
  lastConnectMQTTTime = millis();
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
  boolean result = mqttClient.connect(clientId, mqttUserName, mqttPassword);
  if (result)
  {
    Serial.println("mqtt connect success");
    // 订阅属性下发
    String down_property = String("$thing/down/property/R18OXXA2O5/");
    down_property.concat(clientId);
    Serial.printf("down_property=%s\n", down_property.c_str());
    boolean result_sub = mqttClient.subscribe(down_property.c_str());
    if (result_sub)
      Serial.println("mqtt subscribe success");
    else
      Serial.println("mqtt subscribe failure");
  }
  else
  {
    Serial.println("mqtt connect failure");
  }
}

void connectWifi()
{
  Serial.printf("WiFi connect:%s\n", ssid.c_str());
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(IO2, LOW);
    delay(250);
    digitalWrite(IO2, HIGH);
    delay(250);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.printf("IP  address:%s\r\n", WiFi.localIP().toString().c_str());
  Serial.printf("mac address:%s\r\n", WiFi.macAddress().c_str());
}

// 关闭舵机电源，防止舵机抖动
void closeServoPower()
{
  digitalWrite(IO0, LOW);
}

// 打开舵机电源，舵机开始工作
void openServoPower()
{
  servo.attach(IO2);
  digitalWrite(IO0, HIGH);
  Serial.println("openServoPower");
}

// 舵机重置，回到居中位置
void servoReset()
{
  servo.write(servo_reset_angle);
}

// 舵机转动
void servoPushUp()
{
  servo.write(servo_open_angle);
}

// 舵机转动
void servoPushDown()
{
  servo.write(servo_close_angle);
}

void autoReleaseServo()
{
  if (lastServoUseTime != 0)
  {
    unsigned long time = millis() - lastServoUseTime;
    if (time >= autoResetTime)
    {
      // 关灯开灯执行1s后舵机复位
      servoReset();
    }
    if (time >= autoReleaseTime)
    {
      // 关灯开灯执行5秒后释放舵机
      closeServoPower();
      servo.detach();
      lastServoUseTime = 0;
      closeLED();
    }
  }
  else
  {
    closeServoPower();
  }
}

//向udp工具发送消息
void sendCallBack(const char *buffer)
{
  Udp.beginPacket(Udp.remoteIP(), Udp.remotePort()); //配置远端ip地址和端口
  Udp.write(buffer);                                 //把数据写入发送缓冲区
  Udp.endPacket();                                   //发送数据
}

void smartConfig()
{
  WiFi.mode(WIFI_STA);
  Serial.println("\r\nWait for SmartConfig");
  delay(2000);
  WiFi.beginSmartConfig();

  while (1)
  {
    Serial.print(".");
    digitalWrite(IO2, LOW);
    delay(100);
    digitalWrite(IO2, HIGH);
    delay(100);
    digitalWrite(IO2, LOW);
    delay(100);
    digitalWrite(IO2, HIGH);
    delay(200);
    if (WiFi.smartConfigDone())
    {
      Serial.println("\r\nSmartConfig Success");
      Serial.printf("SSID:%s\r\n", WiFi.SSID().c_str());
      Serial.printf("PSW:%s\r\n", WiFi.psk().c_str());
      WiFi.setAutoConnect(true);
      break;
    }
  }

  Serial.printf("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(IO2, LOW);
    delay(250);
    digitalWrite(IO2, HIGH);
    delay(250);
    Serial.print(".");
  }
  ssid = WiFi.SSID();
  password = WiFi.psk();
  writeConfig();
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.printf("IP address:%s\r\n", WiFi.localIP().toString().c_str());
  Serial.printf("mac address:%s\r\n", WiFi.macAddress().c_str());
}

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  pinMode(IO0, OUTPUT); // IO0控制舵机MOS管，控制舵机电源和电源指示灯
  pinMode(IO2, OUTPUT); // IO2控制舵机信号，同时控制ESP-01s板载LED灯
  pinMode(IO3, INPUT);  // 按键

  btn.attachClick(handleClick);
  btn.attachDoubleClick(handleDoubleClick);
  btn.attachLongPressStop(handleLongClick);

  Serial.println("Formatting LittleFS filesystem");
  // LittleFS.format();
  Serial.println("Mount LittleFS");
  if (!LittleFS.begin())
  {
    Serial.println("LittleFS mount failed");
    return;
  }
  else
  {
    Serial.println("LittleFS mount success");
    readConfig();
  }

  int input = digitalRead(IO3);
  delay(20);
  int input2 = digitalRead(IO3);
  if (input == input2 && input == 0)
  {
    smartConfig();
  }
  else
  {
    if (ssid != NULL && ssid.length() > 0)
    {
      connectWifi();
    }
    else
    {
      smartConfig();
    }
  }

  servo.attach(IO2);
  servoReset();
  openServoPower();
  delay(1000);
  closeServoPower();
  servo.detach();

  Udp.begin(localUdpPort);
  Serial.printf("UDP Now listening at IP %s, UDP port %d\r\n", WiFi.localIP().toString().c_str(), localUdpPort);

  // ticker.attach_ms(100, checkAction);
  digitalWrite(IO0, HIGH); // 启动后默认关闭8266 LED

  ntpClient.begin();
  connectMqtt();
}

void loop()
{
  // put your main code here, to run repeatedly:
  btn.tick();
  syncServerTime();
  readUdp();
  boolean connected = mqttClient.loop();
  if (!connected)
  {
    unsigned long current_time = millis();
    if (current_time - lastConnectMQTTTime > 5 * 1000) //  最小时间间隔为5秒
    {
      Serial.println("mqttClient.loop() return false");
      connectMqtt();
    }
  }
  checkAction();
  checkLEDState();
  autoReleaseServo();
}

void syncServerTime()
{
  ntpClient.update();                   // 同步服务器时间
  epochTime = ntpClient.getEpochTime(); // getEpochTime()返回值单位为秒

  if (epochTime - lastPrintTime >= 60)
  {
    if (epochTime - timeOffset < timeOffset)
    {
      // 时间未同步成功前不打印
      return;
    }
    // 每隔60秒打印一次时间
    String timeStr = ntpClient.getFormattedTime();
    Serial.printf("Current Time:%s\n", timeStr.c_str());
    lastPrintTime = epochTime;
  }
}

void readUdp()
{
  int packetSize = Udp.parsePacket();
  if (packetSize)
  {
    Serial.printf("Received %d bytes from %s, port %d\n", packetSize, Udp.remoteIP().toString().c_str(), Udp.remotePort());
    int len = Udp.read(buffer, 255);
    if (len > 0)
    {
      buffer[len] = 0;
    }
    Serial.printf("UDP packet contents:%s\n", buffer);

    Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
    Udp.write("Receive Data:");
    Udp.write(buffer);
    Udp.endPacket();
  }
}

void checkAction()
{
  String str = buffer;
  if (str.length() == 0)
  {
    return;
  }
  if (str.equals("Action_LED_OFF")) // 关闭小夜灯功能
  {
    enableLED = 0;
    closeLED();
    sendCallBack("Switch has been turn off\n");
    memset(buffer, 0, sizeof(buffer));
  }
  else if (str.equals("Action_LED_ON")) // 开启小夜灯功能
  {
    enableLED = 1;
    openLED();
    sendCallBack("Switch has been turn off\n");
    memset(buffer, 0, sizeof(buffer));
  }
  else if (str.equals("Action_Servo_OFF")) // 舵机执行关闭动作
  {
    autoResetTime = 1000;   // 1s
    autoReleaseTime = 5000; // 5s
    lastServoUseTime = millis();
    openServoPower();
    servoPushDown();
    sendCallBack("Switch has been turn off\n");
    memset(buffer, 0, sizeof(buffer));
  }
  else if (str.equals("Action_Servo_ON")) // 舵机执行打开动作
  {
    autoResetTime = 1000;
    autoReleaseTime = 5000;
    lastServoUseTime = millis();
    openServoPower();
    servoPushUp();
    sendCallBack("Switch has been turn on\n");
    memset(buffer, 0, sizeof(buffer));
  }
  else if (str.startsWith("Action_Servo_Angle=")) // 舵机转到指定角度
  {
    autoResetTime = 5 * 60 * 1000;    // 5min
    autoReleaseTime = 10 * 60 * 1000; // 10min
    if (str.length() >= 19)
    {
      String s = str.substring(19);
      int value = s.toInt();
      Serial.printf("Servo_Angle=%d\n", value);
      lastServoUseTime = millis();
      openServoPower();
      servo.write(value);
    }
    memset(buffer, 0, sizeof(buffer));
  }
}

// 打开ESP-01上的LED灯
void openLED()
{
  if (servo.attached())
  {
    return;
  }
  digitalWrite(IO2, LOW);
}

// 关闭ESP-01上的LED灯
void closeLED()
{
  if (servo.attached())
  {
    return;
  }
  if (enableLED)
  {
    // 如果开启了夜灯，LED灯不关闭
    return;
  }
  digitalWrite(IO2, HIGH); // 关闭ESP-01s板载LED
}

void checkLEDState()
{
  if (servo.attached())
  {
    return;
  }
  if (enableLED)
  {
    openLED();
  }
  else
  {
    closeLED();
  }
}