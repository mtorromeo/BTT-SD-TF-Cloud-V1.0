#include <SD.h>
#include <SDFS.h>
#include <ESP8266WiFi.h>
#include <ESPWebDAV.h>
#include <string>

#ifdef DBG_ENABLED
#define DBG_PRINT(...)         \
  {                            \
    Serial.print(__VA_ARGS__); \
  }
#define DBG_PRINTLN(...)         \
  {                              \
    Serial.println(__VA_ARGS__); \
  }
#else
#define DBG_PRINT(...) \
  {                    \
  }
#define DBG_PRINTLN(...) \
  {                      \
  }
#endif

// LED is connected to GPIO2 on this board
#define INIT_LED        \
  {                     \
    pinMode(2, OUTPUT); \
  }
#define LED_ON            \
  {                       \
    digitalWrite(2, LOW); \
  }
#define LED_OFF            \
  {                        \
    digitalWrite(2, HIGH); \
  }

#define INI_MAX_LEN 50

#define HOSTNAME "WiFi-SD-Card-3DPrinter"
#define SERVER_PORT 80
#define SPI_BLOCKOUT_PERIOD 20000UL

#define SD_CS 4
#define MISO 12
#define MOSI 13
#define SCLK 14
#define CS_SENSE 5

class ESPWebDAVExt : public ESPWebDAV
{
public:
  void sendError(const String &code, const String &message)
  {
    send(code, "text/plain", message);
  }

  bool isClientWaiting()
  {
    return server->hasClient();
  }
};

char ssid[INI_MAX_LEN];
char password[INI_MAX_LEN];
char ss[INI_MAX_LEN];

WiFiServer server(SERVER_PORT);

ESPWebDAVExt dav;
bool initFailed = false;

volatile unsigned long spiBlockoutTime = 0;
bool weHaveBus = false;

File file;

void blink()
{
  LED_ON;
  delay(100);
  LED_OFF;
  delay(400);
}

void errorBlink()
{
  for (int i = 0; i < 100; i++)
  {
    LED_ON;
    delay(50);
    LED_OFF;
    delay(50);
  }
}

void takeBusControl()
{
  weHaveBus = true;
  LED_ON;
  pinMode(MISO, SPECIAL);
  pinMode(MOSI, SPECIAL);
  pinMode(SCLK, SPECIAL);
  pinMode(SD_CS, OUTPUT);
}

void relenquishBusControl()
{
  pinMode(MISO, INPUT);
  pinMode(MOSI, INPUT);
  pinMode(SCLK, INPUT);
  pinMode(SD_CS, INPUT);
  LED_OFF;
  weHaveBus = false;
}

bool ReadLine(File *file, char *str, size_t size)
{
  char ch;
  bool rtn = false;
  size_t n = 0;
  while (true)
  {
    // check for EOF
    if (!file->available())
    {
      break;
    }
    file->read((uint8_t *)&ch, 1);
    rtn = true;
    // Delete CR
    if (ch == '\r')
    {
      continue;
    }
    if (ch == '\n')
    {
      break;
    }
    if ((n + 1) >= size)
    {
      // string too long
      rtn = false;
      n--;
      break;
    }
    str[n++] = ch;
  }
  str[n] = '\0';
  return rtn;
}

void DivideStr(char *str, char *s1, char *s2, char sym)
{
  int i, n1, n2;
  i = -1;
  n1 = 0;
  n2 = 0;
  bool found_sep = false;
  // strip leading white space at start of string
  bool strip_white = true;
  while (str[i + 1] != 0)
  {
    i++;
    if (str[i] == sym && !found_sep)
    {
      found_sep = true;
      // strip leading white space after the separator
      strip_white = true;
      continue;
    }
    if (strip_white)
    {
      if (str[i] == ' ')
      {
        continue;
      }
      else
      {
        strip_white = false;
      }
    }
    if (found_sep)
    {
      s2[n2] = str[i];
      n2++;
    }
    else
    {
      s1[n1] = str[i];
      n1++;
    }
  }

  // backtrack strings to remove tail whitespace
  while (n1 - 1 > 0 && s1[n1 - 1] == ' ')
  {
    n1--;
  }
  while (n2 - 1 > 0 && s2[n2 - 1] == ' ')
  {
    n2--;
  }

  s1[n1] = 0;
  s2[n2] = 0;
}

void setup()
{
  Serial.begin(115200);

  String str1, str2;
  char s1[INI_MAX_LEN], s2[INI_MAX_LEN];
  // ----- GPIO -------
  // Detect when other master uses SPI bus
  pinMode(CS_SENSE, INPUT);
  // attachInterrupt(CS_SENSE, []() {
  //     if (!weHaveBus)
  //       spiBlockoutTime = millis() + SPI_BLOCKOUT_PERIOD;
  //   }, FALLING);

  INIT_LED;
  blink();

  // wait for other master to assert SPI bus first
  delay(SPI_BLOCKOUT_PERIOD);
  if (!SD.begin(SD_CS))
  {
    Serial.println("begin SD failed");
    return;
  }
  DBG_PRINT("SD begin OK...");
  file = SD.open("SETUP.INI", FILE_READ);
  if (!file)
  {
    Serial.println("open file failed");
    return;
  }
  DBG_PRINT("SD open OK...");

  bool found_ssid = false;
  bool found_pass = false;
  while (ReadLine(&file, ss, INI_MAX_LEN))
  {
    Serial.print("ReadLine(file) = ");
    Serial.println(ss);

    DivideStr(ss, s1, s2, '=');
    str1 = String(s1);
    str2 = String(s2);
    str1.toUpperCase();

    DBG_PRINT("str1 = ");
    DBG_PRINTLN(str1);
    DBG_PRINT("str2 = ");
    DBG_PRINTLN(str2);

    if (str1 == "SSID")
    {
      str2.toCharArray(ssid, str2.length() + 1);
      found_ssid = true;
    }
    else if (str1 == "PASSWORD")
    {
      str2.toCharArray(password, str2.length() + 1);
      found_pass = true;
    }
    // if (str1 == "HOSTNAME")
    // {
    //   str2.toCharArray(HOSTNAME, s2.length() + 1);
    // }

    if (found_ssid && found_pass)
    {
      break;
    }
  }

  file.close();

  DBG_PRINT("Connecting to \"");
  DBG_PRINT(ssid);
  DBG_PRINT("\" using password \"");
  DBG_PRINT(password);
  DBG_PRINT("\"");

  // ----- WIFI -------
  // Set hostname first
  WiFi.hostname(HOSTNAME);
  // Reduce startup surge current
  WiFi.setAutoConnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setPhyMode(WIFI_PHY_MODE_11N);
  WiFi.begin(ssid, password);

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED)
  {
    blink();
    DBG_PRINT(".");
  }
  DBG_PRINTLN("");

  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  DBG_PRINT("RSSI: ");
  DBG_PRINTLN(WiFi.RSSI());
  DBG_PRINT("Mode: ");
  DBG_PRINTLN(WiFi.getPhyMode());

  // ----- SD Card and Server -------
  // Check to see if other master is using the SPI bus
  while (millis() < spiBlockoutTime)
    blink();

  takeBusControl();

  // start the SD DAV server
  if (!SD.begin(SD_CS))
  {
    DBG_PRINT("ERROR: ");
    DBG_PRINTLN("Failed to initialize SD Card");
    // indicate error on LED
    errorBlink();
    initFailed = true;
  }
  else
  {
    server.begin();
    dav.begin(&server, &SDFS);
    blink();
  }

  relenquishBusControl();
  DBG_PRINTLN("WebDAV server started");
}

void loop()
{
  if (millis() < spiBlockoutTime)
    blink();

  // do it only if there is a need to read FS
  if (server.hasClient())
  {
    if (initFailed)
    {
      DBG_PRINTLN("Failed to initialize SD Card");
      return dav.sendError("500 Internal Server Error", "Failed to initialize SD Card");
    }

    // has other master been using the bus in last few seconds
    if (millis() < spiBlockoutTime)
    {
      DBG_PRINTLN("Marlin is reading from SD card");
      return dav.sendError("503 Service Unavailable", "Marlin is reading from SD card");
    }

    // a client is waiting and FS is ready and other SPI master is not using the bus
    takeBusControl();
    dav.handleClient();
    relenquishBusControl();
  }
}
