/*
SensnodeTX v3.0-dev
Written by Artur Wronowski <artur.wronowski@digi-led.pl>
Works with optiboot too.
Need Arduino 1.0 do compile
*/

// libs for I2C and 1Wire
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
// lisb for SHT21 and BMP085
#include <SHT21.h>
#include <BMP085.h>
// lib for RFM12B from <jeelabs.org>
#include <JeeLib.h>
// avr sleep instructions
#include <avr/sleep.h>

/*************************************************************/

// structure of data
typedef struct {
	int nodeid;
	int light;
	float humi;
	float temp;
	float pressure;
	byte lobat		:1;
	int battvol;
	byte fet		:1;
} Payload;
Payload measure;

static byte NODEID = 2;       // ID this node
static byte NODEGROUP = 212;

// Input/Output definition
// Analog
#define LDRPin            0
#define BatteryVolPin     1
#define CustomA3          3

// Digital
#define CustomD3          3
#define CustomD4          4
#define MOSFET            5
#define RTC_INT           6
#define CustomD7          7
#define ONEWIRE_DATA      8
#define ACT_LED           9 // or custom use

// Settings
#define MEASURE_PERIOD    300 //300
#define RETRY_PERIOD      10
#define RETRY_ACK         5 // how many times try get ack
#define ACK_TIME          10 // time for recive ack packet (in milisec)
#define REPORT_EVERY      6 //5
#define SMOOTH            3

//#define ObwAnem 0.25434 // meters

// Used devices and buses
#define LDR               1
#define ONEWIRE           0 // use 1wire bus
#define I2C               1 // use i2c bus
#define DEBUG             0 // debug mode - serial
#define LED_ON            1 // use act led - transmission
#define SOLAR             0 // use solar to charge batteries

#define RADIO_SYNC_MODE   2

#define rf12_sleep(x)

/************************************************************/

ISR(WDT_vect) { Sleepy::watchdogEvent(); }

enum { MEASURE, REPORT, TASKS };

static word schedbuf[TASKS];
Scheduler scheduler (schedbuf, TASKS);

Port ldr (1);
Port batvol (3);

byte count = 0;
static byte reportCount;

#if ONEWIRE
    OneWire oneWire(ONEWIRE_DATA);
    DallasTemperature sensors(&oneWire);
#endif

void setup()
{
    rf12_initialize(NODEID, RF12_433MHZ, NODEGROUP);

#if I2C
    Wire.begin();
    BMP085.getCalData();
#endif

#if DEBUG
    Serial.begin(115200);
#endif

#if ONEWIRE
    sensors.begin();
#endif

  rf12_sleep(RF12_SLEEP);

  reportCount = REPORT_EVERY;
  scheduler.timer(MEASURE, 0);
  
  pinMode(5, OUTPUT);
  digitalWrite(5, LOW);
  batvol.digiWrite2(0);
  ldr.digiWrite2(0);
  
}


static int smoothedAverage(int prev, int next, byte firstTime =0) {
    if (firstTime)
        return next;
    return ((SMOOTH - 1) * prev + next + SMOOTH / 2) / SMOOTH;
}

void loop()
{
  #if DEBUG
     Serial.print('.');
     delay(2);	
  #endif
  
  switch (scheduler.pollWaiting()) {
    
    case MEASURE:
        scheduler.timer(MEASURE, MEASURE_PERIOD);
        doMeasure();

        if (++reportCount >= REPORT_EVERY) {
          reportCount = 0;
          scheduler.timer(REPORT, 0);
        }
        break;
    case REPORT:
        if (DEBUG) {
          transmissionRS();
          doReport();
        }
        else
        {
		if (LED_ON) {
			activityLed(1);
			}
          doReport();
		if (LED_ON) {
			activityLed(0);
			}
        }
        break;
  }
}


static void doReportACK()
{
  for (byte i = 0; i < RETRY_ACK; ++i) {
    rf12_sleep(RF12_WAKEUP);
    while (!rf12_canSend())
      rf12_recvDone();
    rf12_sendStart(RF12_HDR_ACK, &measure, sizeof measure);
    rf12_sendWait(RADIO_SYNC_MODE);
    byte acked = waitForACK();
    rf12_sleep(RF12_SLEEP);
    
    if (acked) {
      #if DEBUG
        Serial.print("ACK! ");
        Serial.println((int) i);
        Serial.println("-------");
        delay(2);
      #endif
      scheduler.timer(MEASURE, MEASURE_PERIOD);
      return;
    }
    Sleepy::loseSomeTime(RETRY_PERIOD * 100);
  }
  scheduler.timer(MEASURE, MEASURE_PERIOD);
  #if DEBUG
    Serial.print("no ACK! ");
    Serial.println("-------");
    delay(2);
  #endif
}


static void doReport()
{
  rf12_sleep(RF12_WAKEUP);
  while (!rf12_canSend())
    rf12_recvDone();
  rf12_sendStart(0, &measure, sizeof measure, RADIO_SYNC_MODE);
  rf12_sleep(RF12_SLEEP);
}

static byte waitForACK() {
  MilliTimer t;
  while (!t.poll(ACK_TIME)) {
    if (rf12_recvDone() && rf12_crc == 0 && rf12_hdr == (RF12_HDR_DST | RF12_HDR_CTL | NODEID))
      return 1;
     set_sleep_mode(SLEEP_MODE_IDLE);
     sleep_mode();
  }
  return 0;
}

static void transmissionRS()
{
  activityLed(1);
  Serial.println(' ');
  Serial.print("LIGHT ");
  Serial.println(measure.light);
  Serial.print("HUMI ");
  Serial.println(measure.humi);
  Serial.print("TEMP ");
  Serial.println(measure.temp);
  Serial.print("PRES ");
  Serial.println(measure.pressure);
  Serial.print("LOBAT " );
  Serial.println(measure.lobat, DEC);
  Serial.print("BATVOL ");
  Serial.println(measure.battvol);
  activityLed(0);
}


static void activityLed (byte on) {
  pinMode(ACT_LED, OUTPUT);
  digitalWrite(ACT_LED, on);
  delay(150);
}


static void mosfetControl(byte on) {
  pinMode(MOSFET, OUTPUT);
  digitalWrite(MOSFET, on);
  delay(100);
}

static byte pinState(int pin)
{
  return digitalRead(pin) ? 1 : 0;
}

static void doMeasure() {
  count++;
  byte firstTime = measure.humi == 0;
#if LDR
  if ((count % 2) == 0) {
     measure.light = ldr.anaRead();
  }
#endif
#if I2C
  SHT21.readSensor();
  measure.humi = SHT21.humi;
  measure.temp = SHT21.temp;
  delay(32);
  BMP085.readSensor();
  measure.pressure = (BMP085.press*10) + 16;
#endif
#if ONEWIRE
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  measure.temp = tempC;
#endif
  measure.battvol = map(batvol.anaRead(), 0, 1023, 0, 660);
  measure.nodeid = NODEID;
  measure.lobat = rf12_lowbat();
}

