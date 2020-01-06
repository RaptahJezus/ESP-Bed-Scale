#include <Arduino.h>
#include <Wire.h>
#include "HX711.h"
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include "config.h"

//HX711 definitions
#define DOUT 12
#define CLK 13
#define calibration_factor 	-12850
#define NUM_SAMPLES 		3
#define SAMPLE_PERIOD 		100
#define RESULT_WIDTH 		5
#define RESOLUTION 			1
#define PRECISION 			0.1

//MQTT topics
#define TOPIC_BASE "ESP/BedScale"
#define TOPIC_MASS TOPIC_BASE "/mass"
#define TOPIC_CMD  TOPIC_BASE "/cmd"
#define TOPIC_AVAIL TOPIC_BASE "/available"

//IO Definitions
int statusLED = 5;
int mqttPostLED = 4;
int tareButton = 14;

//WiFi Settings
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

//MQTT settings
const char* mqtt_server = MQTT_SERVER_IP;
const char* mqtt_clientID = MQTT_CLIENT_ID;
const char* mqtt_username = MQTT_USER;
const char* mqtt_password = MQTT_PASSWORD;

//Variables to publish availability
static const unsigned long AVAIL_PUB_INTERVAL = 1500;
static unsigned long lastAvailPubTime = 0;

//Initiliaze scale
HX711 scale;

//Init WiFi and MQTT
WiFiClient bedScale;
PubSubClient client(bedScale);

//Init OTA functionality
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

void init_WiFi()
{
	delay(10);

	Serial.println();
	Serial.print("Connecting to ");
	Serial.println(ssid);

	WiFi.hostname("ESP-BedScale");
	WiFi.begin(ssid, password);

	while (WiFi.status() != WL_CONNECTED)
	{
		delay(100);
		digitalWrite(statusLED, !digitalRead(statusLED));
	}

	Serial.println("");
	Serial.println("WiFi connected.");
	Serial.print("IP Address: ");
	Serial.println(WiFi.localIP());
}


void MQTT_Reconnect()
{
	int delayCount = 0;

	while (!client.connected())
	{
		Serial.println("Attempting MQTT connection...");


		if(client.connect(mqtt_clientID, mqtt_username, mqtt_password))
		{
			Serial.println("Connected");
			client.subscribe("ESP/BedScale/cmd");
			digitalWrite(statusLED, HIGH);
		}

		else
		{
			Serial.print("failed, rc=");
			Serial.print(client.state());
			Serial.println("Trying again in 5 seconds");

			for(delayCount = 0; delayCount <= 25; delayCount++ )
			{
				digitalWrite(statusLED, !digitalRead(statusLED));
				delay(200);
			}
		}

	}
}


void callback(String topic, byte* message, unsigned int length)
{
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;

  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println();


  if(topic=="ESP/BedScale/cmd")
  {
      if(messageTemp == "tare")
      {
        Serial.println("Taring scale");
        scale.tare();
      }
    }
}


void init_OTA()
{
	MDNS.begin("ESP-BedScale");
	httpUpdater.setup(&httpServer);
	httpServer.begin();
	MDNS.addService("http","tcp",80);
	Serial.println("Http Server Ready!");


	ArduinoOTA.setPort(8266);
	ArduinoOTA.setHostname("OTA-BedScale");

	ArduinoOTA.onStart([]()
	{
		String type;
		if (ArduinoOTA.getCommand() == U_FLASH)
		{
			type = "sketch";
		}
		else // U_SPIFFS
		{
			type = "filesystem";
		}

// NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
		Serial.println("Start updating " + type);
	});


	ArduinoOTA.onEnd([]()
	{
		Serial.println("\nEnd");
	});

	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
	{
		Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
	});

	ArduinoOTA.onError([](ota_error_t error)
	{
		Serial.printf("Error[%u]: ", error);
		if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
		else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
		else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
		else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
		else if (error == OTA_END_ERROR) Serial.println("End Failed");
	});

	ArduinoOTA.begin();
}


void init_MQTT()
{
	client.setServer(mqtt_server, 1883);
	client.setCallback(callback);
}


void init_Scale()
{
	scale.begin(DOUT,CLK);
	scale.set_scale();
	scale.tare();
	scale.set_scale(calibration_factor);
}

void setup() {
  Serial.begin(9600);

  //Init functions
  init_WiFi();
  init_OTA();
  init_MQTT();
  init_Scale();

  //Configure Outputs
  pinMode(statusLED, OUTPUT);
  pinMode(mqttPostLED, OUTPUT);

}

void loop() {

	static uint32_t timer = 0;
	static uint32_t avail_timer = 0;
	static uint8_t samples = 0;
	static float avg = 0;
	static float oldAvg = 0;

	if(!digitalRead(tareButton))
	{
		Serial.println("Taring");
		scale.tare();
	}


	//Take load sample
	if(millis() - timer >= SAMPLE_PERIOD)
	{
		avg += scale.get_units();
		samples++;


		if(samples >= NUM_SAMPLES)
		{
			//Create buffer
			char result[RESULT_WIDTH + 1];

			//Average results
			avg /= samples;

			//Transmit only if changed
			if(abs(avg - oldAvg) > PRECISION)
			{
				if(avg <= 0.5)
				{
					avg = 0;
				}
				//COnvert float to string
				dtostrf(avg, RESULT_WIDTH, RESOLUTION, result);

				//Serial.println(result);
				client.publish(TOPIC_MASS, result);
				digitalWrite(mqttPostLED, HIGH);
				delay(100);
				digitalWrite(mqttPostLED, LOW);
				oldAvg = avg;
			}

			Serial.println(avg);

			avg = 0;
			samples = 0;
		}

		timer = millis();
	}


	//MQTT Loop
	if(!client.connected())
	{
		MQTT_Reconnect();
	}
	client.loop();

	httpServer.handleClient();
	ArduinoOTA.handle();


	if(millis() - avail_timer >= 1500)
	{
		client.publish(TOPIC_AVAIL, "online");
		avail_timer = millis();
	}


	//Serial.println("HelloWorld");


		//Serial.println(scale.read_average());
		//Serial.print("Reading: ");
		//Serial.print(scale.get_units(), 1);
		//Serial.println(" lbs");





}