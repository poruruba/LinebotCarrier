#include <M5StickC.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "Audio.h"

#define I2S_VOLUME    7 // 0...21

const char *wifi_ssid = "【WiFiアクセスポイントのSSID】";
const char *wifi_password = "【WiFiアクセスポイントのパスワード】";

const char* mqtt_server = "【MQTTサーバのホスト名】"; // MQTTサーバのホスト名
const uint16_t mqtt_port = 1883; // MQTTサーバのポート番号(TCP接続)

const String base_url("【立ち上げたNode.jsサーバのURL】");

#define SAMPLING_RATE (8192)       // サンプリングレート(44100, 22050, 16384, more...)
#define BUFFER_LEN    (1024)        // バッファサイズ
#define SAMPLEING_SEC (4)           // 最大サンプリング時間(秒)
#define STORAGE_LEN   (SAMPLING_RATE * SAMPLEING_SEC)      // 本体保存容量
#define MQTT_CLIENT_NAME  "AtomEcho" // MQTTサーバ接続時のクライアント名
#define MQTT_TOPIC_TO_ATOM "linebot_to_atom"
#define MQTT_BUFFER_SIZE  255 // MQTT送受信のバッファサイズ
#define PRESSED_DURATION  500

bool isRecording = false;               // 録音状態
int recPos = 0;                     // 録音の長さ
uint8_t soundBuffer[BUFFER_LEN];    // DMA転送バッファ
uint8_t soundStorage[STORAGE_LEN];  // サウンドデータ保存領域
uint8_t temp_buffer[1024];

bool isReceived = false;  // LINEメッセージの受信状態
bool btnPressed = false;  // ボタンの押下状態
unsigned long start_tim = 0; // ボタンを押下した時の時間

#define I2S_BCLK      19
#define I2S_LRC       33
#define I2S_DOUT      22
#define I2S_DIN       23
Audio audio;

#define NEOPIXEL_PIN 27
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

WiFiClient espClient;
PubSubClient client(espClient);

const int subscribe_capacity = JSON_OBJECT_SIZE(2);
StaticJsonDocument<subscribe_capacity> json_subscribe;

void wifi_connect(const char *ssid, const char *password);
long doHttpPostFile(const char *p_url, const unsigned char *p_data, unsigned long data_len, const char *p_content_type, const char *p_bin_name, const char *p_bin_filename, 
    const char *p_param_name, const char *p_param_data, uint8_t *p_buffer, unsigned long *p_len);
void i2sRecord();
void i2sPlayUrl(const char *url);
void i2sPlaySpeech(const char *text);

// void audio_info(const char* str){
//   Serial.println(str);
// }

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Mqtt Received: "); Serial.println(topic);
  DeserializationError err = deserializeJson(json_subscribe, payload, length);
  if( err ){
    Serial.println("Deserialize error");
    Serial.println(err.c_str());
    return;
  }

  // LINEメッセージの通知
  const char* message = json_subscribe["message"];
  Serial.println(message);
  isReceived = true;

  pixels.setPixelColor(0, pixels.Color(100, 0, 0));
  pixels.show();
}

void setup() {
  M5.begin(false, false, true);
  Serial.begin(9600);
  pixels.begin();

  pixels.setPixelColor(0, pixels.Color(0, 0, 0));
  pixels.show();

  wifi_connect(wifi_ssid, wifi_password);

  // バッファサイズの変更
  client.setBufferSize(MQTT_BUFFER_SIZE);
  // MQTTコールバック関数の設定
  client.setCallback(mqtt_callback);
  // MQTTブローカに接続
  client.setServer(mqtt_server, mqtt_port);
}

void loop() {
  client.loop();

  // MQTT未接続の場合、再接続
  while(!client.connected() ){
    static bool led = true;
    Serial.println("Mqtt Reconnecting");

    pixels.setPixelColor(0, pixels.Color(0, 0, led ? 100 : 0));
    pixels.show();
    led = !led;

    if( client.connect(MQTT_CLIENT_NAME) ){
      // MQTT Subscribe
      client.subscribe(MQTT_TOPIC_TO_ATOM);
      Serial.println("Mqtt Connected and Subscribing");

      pixels.setPixelColor(0, pixels.Color(0, 0, 0));
      pixels.show();

      break;
    }

    delay(1000);
  }
  
  M5.update();
  audio.loop();

  if( M5.BtnB.isPressed() && !btnPressed ) {
    start_tim = millis();
    btnPressed = true;
  }else
  if( btnPressed && M5.BtnB.isPressed() && !isRecording ){
    unsigned long end_tim = millis();
    if( end_tim - start_tim > PRESSED_DURATION ){
      if( audio.isRunning() )
        audio.stopSong();

      // 録音スタート
      Serial.println("Record Start");
      i2sRecord();
    }
  } else if ( !M5.BtnB.isPressed() && btnPressed ) {
    unsigned long end_tim = millis();
    btnPressed = false;
    if( end_tim - start_tim <= PRESSED_DURATION ){
      // LINEメッセージの再生
      Serial.println("Clicked");
      if( isReceived ){
        pixels.setPixelColor(0, pixels.Color(0, 0, 0));
        pixels.show();

        i2sPlayUrl((base_url + "/message.mp3").c_str());
      }
    }else{
      if( isRecording ){
        // 録音ストップ
        isRecording = false;
        delay(1000); // 録音終了まで待つ
        Serial.println("Record Stop");
      }
    }
  }
}

void wifi_connect(const char *ssid, const char *password){
  bool led = true;

  Serial.println("");
  Serial.print("WiFi Connenting");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    pixels.setPixelColor(0, pixels.Color(0, 0, led ? 100 : 0));
    pixels.show();
    led = !led;

    Serial.print(".");
    delay(1000);
  }

  pixels.setPixelColor(0, pixels.Color(0, 0, 0));
  pixels.show();

  Serial.println("");
  Serial.print("Connected : ");
  Serial.println(WiFi.localIP());
}

// 録音用タスク
void i2sRecordTask(void* arg){
  // 初期化
  recPos = 0;
  memset(soundStorage, 0, sizeof(soundStorage));

  vTaskDelay(100);
 
  // 録音処理
  while (isRecording) {
    size_t transBytes;
 
    // I2Sからデータ取得
    i2s_read(I2S_NUM_0, (char*)soundBuffer, BUFFER_LEN, &transBytes, (100 / portTICK_RATE_MS));
 
    // int16_t(12bit精度)をuint8_tに変換
    for (int i = 0 ; i < transBytes ; i += 2 ) {
      if ( recPos < STORAGE_LEN ) {
        int16_t* val = (int16_t*)&soundBuffer[i];
        soundStorage[recPos] = ( *val + 32768 ) / 256;
        recPos++;
        if( recPos >= sizeof(soundStorage) ){
          isRecording = false;
          break;
        }
      }
    }
//    Serial.printf("transBytes=%d, recPos=%d\n", transBytes, recPos);
    vTaskDelay(1 / portTICK_RATE_MS);
  }

  i2s_driver_uninstall(I2S_NUM_0);

  pixels.setPixelColor(0, pixels.Color(0, 0, 0));
  pixels.show();

  if( recPos > 0 ){
    unsigned long len = sizeof(temp_buffer);
    int ret = doHttpPostFile((base_url + "/linebot-carrier-wav2text").c_str(), soundStorage, recPos, "application/octet-stream", 
                              "upfile", "test.bin", NULL, NULL, temp_buffer, &len);
    if( ret != 0 ){
      Serial.println("/linebot-carrier-wav2text: Error");
    }else{
      Serial.println((char*)temp_buffer);
    }
  }
 
  // タスク削除
  vTaskDelete(NULL);
}

void i2sRecord(){
  isRecording = true;

  pixels.setPixelColor(0, pixels.Color(0, 100, 0));
  pixels.show();

  i2s_driver_uninstall(I2S_NUM_0);
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
      .sample_rate = SAMPLING_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // is fixed at 12bit, stereo, MSB
      .channel_format = I2S_CHANNEL_FMT_ALL_RIGHT,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 6,
      .dma_buf_len = 60,
  };

  esp_err_t err = ESP_OK;

  err += i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_pin_config_t tx_pin_config;

  tx_pin_config.bck_io_num = I2S_BCLK;
  tx_pin_config.ws_io_num = I2S_LRC;
  tx_pin_config.data_out_num = I2S_DOUT;
  tx_pin_config.data_in_num = I2S_DIN;

  //Serial.println("Init i2s_set_pin");
  err += i2s_set_pin(I2S_NUM_0, &tx_pin_config);
  //Serial.println("Init i2s_set_clk");
  err += i2s_set_clk(I2S_NUM_0, SAMPLING_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  // 録音開始
  xTaskCreatePinnedToCore(i2sRecordTask, "i2sRecordTask", 4096, NULL, 1, NULL, 1);    
}

void i2sPlayUrl(const char *url){
  if( audio.isRunning() )
    audio.stopSong();

  audio.setup();
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_DIN);
  audio.setVolume(I2S_VOLUME); // 0...21

  audio.connecttohost(url);
}

void i2sPlaySpeech(const char *text){
  if( audio.isRunning() )
    audio.stopSong();

  audio.setup();
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_DIN);
  audio.setVolume(I2S_VOLUME); // 0...21

  audio.connecttospeech(text, "jp");
}

typedef struct{
    char scheme;
    char host[32];
    unsigned short port;
    const char *p_path;
} URL_INFO;

URL_INFO url_parse(const char *p_url){
  URL_INFO url_info;
  const char *p_ptr = p_url;

  url_info.scheme = -1;
  if( strncmp(p_ptr, "http://", 7 ) == 0 ){
    url_info.scheme = 0;
    p_ptr += 7;
  }else if( strncmp( p_ptr, "https://", 8 ) == 0 ){
    url_info.scheme = 1;
    p_ptr += 8;
  }

  const char *p_host = p_ptr;
  memset( url_info.host, '\0', sizeof(url_info.host));
  url_info.port = 80; 
  while( *p_ptr != '\0' ){
    if( *p_ptr == ':' ){
      strncat(url_info.host, p_host, p_ptr - p_host);
      p_ptr++;
      url_info.port = atoi(p_ptr);
      while( isdigit(*p_ptr) ) p_ptr++;
      break;
    }else if( *p_ptr == '/' ){
      strncat(url_info.host, p_ptr, p_ptr - p_host);
      break;
    }
    p_ptr++;
  }

  url_info.p_path = p_ptr;

  return url_info;
}

#define MUTIPART_BOUNDARY  "f9492bf35a2126aca30c9ca8525"

#define PAYLOAD_PARAM_HEADER_FORMAT ""\
                     "--%s\r\n"\
                     "Content-Disposition: form-data; name=\"%s\"\r\n"\
                     "\r\n"

#define PAYLOAD_FILE_HEADER_FORMAT ""\
                     "--%s\r\n"\
                     "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"\
                     "Content-Type: %s\r\n"\
                     "Content-Transfer-Encoding: binary\r\n"\
                     "\r\n"

#define MULTIPART_CONTENT_TYPE_FORMAT "multipart/form-data; boundary=%s"
#define MULTIPART_DELIMITER "\r\n"
#define PAYLOAD_FOOTER_FORMAT "--%s--\r\n"

long doHttpPostFile(const char *p_url, const unsigned char *p_data, unsigned long data_len, const char *p_content_type, const char *p_bin_name, const char *p_bin_filename, 
    const char *p_param_name, const char *p_param_data, uint8_t *p_buffer, unsigned long *p_len){

  static WiFiClient espClient;
  static WiFiClientSecure espClientSecure;

  Serial.print("[HTTP] POST begin...\n");

  URL_INFO url_info = url_parse(p_url);
  WiFiClient client = espClient;
  if( url_info.scheme == 1 )
    client = espClientSecure;

  char contentType[120];
  sprintf(contentType, MULTIPART_CONTENT_TYPE_FORMAT, MUTIPART_BOUNDARY);

  char payloadParamHeader[120] = {0};
  if( p_param_name != NULL && p_param_data != NULL ){
    sprintf(payloadParamHeader,
                PAYLOAD_PARAM_HEADER_FORMAT,
                MUTIPART_BOUNDARY,
                p_param_name);
  }

  char payloadHeader[220] = {0};
  sprintf(payloadHeader,
              PAYLOAD_FILE_HEADER_FORMAT,
              MUTIPART_BOUNDARY,
              p_bin_name, p_bin_filename, p_content_type);

  char payloadFooter[70] = {0};
  sprintf(payloadFooter, PAYLOAD_FOOTER_FORMAT, MUTIPART_BOUNDARY);

  long contentLength = strlen(payloadHeader) + data_len + strlen(MULTIPART_DELIMITER) + strlen(payloadFooter);
  if( p_param_name != NULL && p_param_data != NULL )
    contentLength += strlen(payloadParamHeader) + strlen(p_param_data) + strlen(MULTIPART_DELIMITER);

  Serial.printf("[HTTP] request Content-Length=%ld\n", contentLength);

  Serial.print("[HTTP] POST...\n");
  if (!client.connect(url_info.host, url_info.port)){
    Serial.println("can not connect");
    return -3;
  }

  client.printf("POST %s HTTP/1.1\n", url_info.p_path);
  client.print(F("Host: "));client.println(url_info.host);
  client.println(F("Accept: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Type: "));client.println(contentType);
  client.print(F("Content-Length: "));client.println(contentLength);        
  client.println();
  client.print(payloadHeader);
  client.flush();

  client.write(p_data, data_len);
  client.flush();
  client.print(MULTIPART_DELIMITER);
  if( p_param_name != NULL && p_param_data != NULL ){
    client.print(payloadParamHeader);
    client.print(p_param_data);
    client.print(MULTIPART_DELIMITER);
  }
  client.print(payloadFooter);
  client.flush();

  bool trimed = false;
  long length = -1;
  long received = 0;

  while(client.connected() || client.available()) {
    int size = client.available();
    if( size <= 0 ){
      delay(1);
      continue;
    }

    if( !trimed ){
      String line = client.readStringUntil('\n');
      line.toLowerCase();
      if( line.startsWith("content-length:") ){
        length = atoi(&(line.c_str()[15]));
        Serial.printf("[HTTP] response Content-Length=%ld\n", length);
      }else if( line == "\r" ){
        trimed = true;
      }
    }else if( trimed ){
      if( received + size > *p_len ){
        client.stop();
        Serial.println("receive buffer over");
        return -1;
      }
      int len = client.readBytes(&p_buffer[received], size);
      received += len;
      if( length >= 0 && received >= length )
        break;
    }
  }

  client.stop();

  if( !trimed )
    return -2;

  *p_len = received;

  return 0;
}
