#include <ArduinoJson.h>
#include <Wire.h>
#include <MspFlash.h>

#define VERSION_STRING "1.4"

#define CURRENT P4_4
#define VIN_MON P4_5
#define V12_MON P3_6
#define SERIAL_SELECT P2_1
#define UPIVS_CLK P1_2
#define UPIVS_SDO P1_3
#define PIN_BOARD_TYPE_SENSE P1_5

// Both of these have signatures to make sure they're valid.
unsigned char *const default_disable = SEGMENT_B;
#define DEFAULT_DISABLE_SIGNATURE 0x55
int *const calib = (int *) SEGMENT_D;
#define CALIB_SIGNATURE 0x55AA

char __attribute__((section(".noinit"))) cmd_buffer[128];
unsigned char cmd_buffer_ptr = 0;

#define NUM_SENSE_CHANNELS 4
#define NUM_CHANNELS 8
static const uint8_t enables[8] = { P4_0, P4_1, P4_2, P4_7, P1_5, P3_3, P3_0, P2_0 };
static const int senses[4] = { P2_4, P2_2, P3_7, P4_6 };

unsigned char __attribute__((section(".noinit"))) numEnableChannels;

#define HOUSEKEEPING_PERIOD 1000
unsigned long housekeepingTime = 0;
typedef enum housekeepingState {
  output_ON = 0,
  output_V = 1,
  output_I = 2,
  output_T = 3
} housekeepingState_t;

housekeepingState_t housekeepingState = output_ON;

#define CURRENT_WAIT_PERIOD 100
#define CURRENT_PERIOD 5

unsigned long currentTime = 0;
unsigned char current_channel = 0;
unsigned char current_average_count = 0;

int current_measure[4] = { 0, 0, 0, 0};
unsigned int current_temp = 0;
unsigned int last_voltage = 0;

// It takes ~half a second to enter sleep mode. Wait double that.
#define UPIVS_WAIT_PERIOD 1000
// Wait a bit more after.
#define UPIVS_AFTER_WAIT_PERIOD 10
// After we wake up, we need to check for SDO low (data ready). Check quickly.
#define UPIVS_CHECK_PERIOD 1
// After that, the clock period is mainly set by the optocoupler, so set that
// to around 10 ms.
#define UPIVS_BIT_PERIOD 10
#define UPIVS_CLK_HIGH 1
#define UPIVS_CLK_LOW 0

unsigned long upivsTime = 0;
unsigned int upivsBitCount = 0;
unsigned int upivs_temp = 0;
unsigned int upivs_last_voltage = 0;
unsigned int upivs_waiting = 0;

unsigned int readVin() {
  return analogRead(VIN_MON);
}

unsigned int read12Vin() {
  return analogRead(V12_MON);
}

unsigned int readSense(unsigned int channel) {
  return analogRead(senses[channel]);
}

void enableXtal() {
}

// Our analog inputs are
// 0: P2_4, A4, OA1I0
// 1: P2_2, A2, OA0I1
// 2: P3_7, A7, OA1IA
// 3: P4_6, A15,OA1IB
// VIN: P4_5, A14
// OA0 output: P4_4, A13
// OA1 input: P2_3, A3
// OA1 output: P4_3, A12
// We want to hook up our op-amps as a difference amplifier.
// OA0: IN+ = A14 (OA0IB), IN- = OA0 output (internal)
// OA1: IN+ = A4/2/7/15, IN- = A3 (OA1I1)
// Then the output is on A13 (P4_4)
// OA0 is a unity-gain buffer
// OA1 is a general purpose op-amp
// OA0CTL0 = (OAP_3) | (OAPM_1) | (OAADC_3);
// OA0CTL1 = (OAFC_2);
// OA1CTL0 = (OAN_1) | (channel << 4) | (OAPM_1) | (OAADC_3);
// OA1CTL1 = (OAFC_0);
//
void readCurrent() {
  if (current_average_count == 64) {
	  unsigned char tmp;
	  current_temp += analogRead(CURRENT);
          current_temp = current_temp >> 1;
	  current_measure[current_channel] = (last_voltage << 5) + calib[current_channel] - current_temp;
	  current_temp = 0;
	  current_average_count = 0;

	  current_channel++;
	  current_channel = current_channel % 4;
	  tmp = OA1CTL0;
	  tmp &= (~OAP_3);
	  tmp |= (current_channel << 4);
	  OA1CTL0 = tmp;

	  currentTime = millis() + CURRENT_WAIT_PERIOD;
	  return;
  } else {
          if (!current_average_count) last_voltage = readVin();
	  current_temp += analogRead(CURRENT);
	  current_average_count++;
	  currentTime = millis() + CURRENT_PERIOD;
  }
}

char readTMP422(unsigned int ch) {
	unsigned int tmp;
	unsigned char err;

	Wire.beginTransmission(0x4C);
	Wire.write(ch);
	err = Wire.endTransmission();
	if (err) {
		Serial.print("{\"dbg\":\"I2C err\"}");
	}
	Wire.requestFrom(0x4C, 1);
	tmp = 0;
	if (Wire.available()) tmp = Wire.read();
	tmp -= 64;
	return tmp;
}

void doVinReadout() {
  if (digitalRead(UPIVS_CLK) == UPIVS_CLK_HIGH) {
    // Clock is high.
    if (upivsBitCount == 0) {
      // need to wakeup
      digitalWrite(UPIVS_CLK, UPIVS_CLK_LOW);
      upivsTime = millis() + UPIVS_CHECK_PERIOD;
      upivs_temp = 0;
      return;
    } else {
      if (digitalRead(UPIVS_SDO) == 1)
        upivs_temp = upivs_temp << 1;
      else
        upivs_temp = (upivs_temp << 1) | 0x1;
  
      if (upivsBitCount == 16) {
        upivs_last_voltage = upivs_temp;
      }
      if (upivsBitCount == 26) {
        // Leave clock high, let it go to sleep.
        upivsTime = millis() + UPIVS_WAIT_PERIOD;
        upivsBitCount = 0;
        return;
      }
      digitalWrite(UPIVS_CLK,UPIVS_CLK_LOW);
      upivsTime = millis() + UPIVS_BIT_PERIOD;
    }
  } else {
    if (upivsBitCount == 0) {
      if (digitalRead(UPIVS_SDO) == 1) {
      	if (!upivs_waiting) {
	   upivs_waiting = 1;
	   upivsTime = millis() + UPIVS_AFTER_WAIT_PERIOD;
	   return;
	} else {
	   upivs_waiting = 0;
	}
        // Ready!
        digitalWrite(UPIVS_CLK, UPIVS_CLK_HIGH);
        upivsTime = millis() + UPIVS_BIT_PERIOD;
        upivsBitCount++;
        return;
      }
      upivsTime = millis() + UPIVS_CHECK_PERIOD;
      return;
    }
    // Clock is low. Raise it.
    digitalWrite(UPIVS_CLK, UPIVS_CLK_HIGH);
    upivsTime = millis() + UPIVS_BIT_PERIOD;
    upivsBitCount++;
  }
}

void doHousekeeping() {
  unsigned int i;
  unsigned int count;
  int tmp;
  
  switch (housekeepingState) {
    case output_ON:
      Serial.print("{\"on\":[");
      count = 0;
      for (i=0;i<numEnableChannels;i++) {
        if (!digitalRead(enables[i])) {
          if (count) Serial.print(",");
          Serial.print(i);
          count++;
        }
      }
      Serial.println("]}");
      housekeepingState = output_V;
      break;
    case output_V:
      Serial.print("{\"v\":[");
      Serial.print(last_voltage);
      Serial.print(",");
      Serial.print(analogRead(128+11));
      Serial.print(",");
      Serial.print(upivs_last_voltage);
      if (numEnableChannels > 4) {
        Serial.print(",");
        Serial.print(read12Vin());        
      }
      Serial.println("]}");
      housekeepingState = output_I;
      break;
    case output_I:
      Serial.print("{\"i\":[");
      for (i=0;i<NUM_SENSE_CHANNELS;i++) {
        if (i) Serial.print(",");
        Serial.print(current_measure[i]);
      }
      Serial.println("]}");
      housekeepingState = output_T;
      break;
    case output_T:
      Serial.print("{\"t\":[");
      Serial.print(analogRead(128+10));
      for (i=0;i<3;i++) {
        Serial.print(",");
        tmp = readTMP422(i);
        Serial.print(tmp);
      }
      Serial.println("]}");
      housekeepingState = output_ON;
      break;
  }
  housekeepingTime = millis() + HOUSEKEEPING_PERIOD;
}

/////////////////////////////////////////
//               SETUP                 //
/////////////////////////////////////////
void setup()
{  
  unsigned char err=0;

  if (default_disable[63] != DEFAULT_DISABLE_SIGNATURE) {
    unsigned char tmp[64];
    memset(tmp, 0, sizeof(tmp));
    tmp[63] = DEFAULT_DISABLE_SIGNATURE;
    Flash.erase(default_disable);
    Flash.write(default_disable, (unsigned char *) tmp, 64);
    err++;
  }

  // What are we?
  pinMode(PIN_BOARD_TYPE_SENSE, INPUT_PULLUP);
  if (digitalRead(PIN_BOARD_TYPE_SENSE)) {
    numEnableChannels = 4;
  } else {
    numEnableChannels = 8;
  }

  // Add something here to grab data from info flash to determine if
  // we let all of them turn on.
  for (unsigned int i=0;i<numEnableChannels;i++) {
    if (default_disable[i]) {
      digitalWrite(enables[i], 1);
    } else {
      digitalWrite(enables[i], 0);
    }
    pinMode(enables[i], OUTPUT);
  }
  // Switch P2_1 to go to 15V_SW1_SENSE
  pinMode(SERIAL_SELECT, OUTPUT);
  digitalWrite(SERIAL_SELECT, 1);
  // Switch reference to 2.5V internal.
  analogReference(INTERNAL2V5);
  // Drive CLK low (P1.2) - it becomes high on the other side of the optocoupler.
  // We keep the ADS1244 in sleep, and wake it up.
  pinMode(UPIVS_CLK, OUTPUT);
  digitalWrite(UPIVS_CLK, UPIVS_CLK_HIGH);  
  // Set up the OA.
  // Disable the analog inputs on P2.2 (A2), P2.4 (A4), P3.7 (A7), P4.6 (A15), P2.3 (A3)
  // P4.5 (A14), P4.3 (A12), and P4.4 (A13), and P3.6 (A6).
  ADC10AE0 = (BIT2 | BIT3 | BIT4 | BIT6 | BIT7);
  ADC10AE1 = (BIT7 | BIT6 | BIT5 | BIT4);
  // OA0 is just a unity-gain follower on VIN_MON.
  OA0CTL1 = (OAFC_2);
  // Slow slew, output to external pin A12.
  // Also connect the negative input muxer to select input 1 as well.
  // This should be unimportant.
  OA0CTL0 = (OAN_3) | (OAP_3) | (OAPM_1) | (OAADC0) | (OAADC1);
  // OA1 is a general-purpose op-amp.
  OA1CTL1 = (OAFC_0);
  // start with channel 0.
  OA1CTL0 = (OAN_1) | (OAP_0) | (OAPM_1) | (OAADC0) | (OAADC1);
  if (calib[31] != CALIB_SIGNATURE) {
    int tmp[32];
    memset(tmp, 0, sizeof(tmp));
    tmp[31] = CALIB_SIGNATURE;
    Flash.erase((unsigned char *) calib);
    Flash.write((unsigned char *) calib, (unsigned char *) tmp, 64);
    err++;
  }

  
  // Now boot.
  Serial.begin(9600);
  if (err)
    Serial.print("{\"log\":\"initial boot - ");
  else
    Serial.print("{\"log\":\"reboot - ");
  Serial.print(numEnableChannels);
  Serial.println("ch\"}");

  Wire.begin();
  Wire.beginTransmission(0x4C);
  Wire.write(0x09);
  Wire.write(0x4);
  err = Wire.endTransmission();
  if (err) {
	  Serial.print("{\"dbg\":\"I2C err\"}");
  }
  currentTime = millis();
  housekeepingTime = millis() + HOUSEKEEPING_PERIOD;
  upivsBitCount = 26;
  
  pinMode(LED0, OUTPUT);
  digitalWrite(LED0, 0);
}

void loop()
{
  if ((long) (millis() - upivsTime) > 0)
    doVinReadout();

  if ((long) (millis() - currentTime) > 0)
    readCurrent();

  if ((long) (millis() - housekeepingTime) > 0)
    doHousekeeping();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      cmd_buffer[cmd_buffer_ptr] = 0;
      parseJsonInput();
      cmd_buffer_ptr = 0;
    } else {
      cmd_buffer[cmd_buffer_ptr] = c;
      if (cmd_buffer_ptr < 127) cmd_buffer_ptr++;
    }
  }
}

void parseJsonInput() {
  StaticJsonBuffer<64> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(cmd_buffer);
  if (!root.success()) {
    Serial.println("{\"dbg\":\"invalid JSON\"}");
    return;
  }
  if (root.containsKey("set")) {
    JsonArray& setArray = root["set"];
    unsigned char setMask = setArray[0];
    unsigned char setVal = setArray[1];
    unsigned int i;
    for (i=0;i<numEnableChannels;i++) {
      if (setMask & (1<<i)) {
        if (setVal & (1<<i))
          digitalWrite(enables[i], 0);
        else
          digitalWrite(enables[i], 1);
      }
    }
  }
  if (root.containsKey("disable")) {
    unsigned char tmp_default[64];
    JsonArray& disableArray = root["disable"];
    memcpy(tmp_default, default_disable, sizeof(tmp_default));
    tmp_default[63] = DEFAULT_DISABLE_SIGNATURE;
    for (size_t i=0;i<disableArray.size();i++) {
      unsigned int ch;
      ch = disableArray[i];
      if (ch<numEnableChannels) {
        tmp_default[ch] = 1;
      }
    }
    Flash.erase(SEGMENT_B);
    Flash.write(SEGMENT_B, tmp_default, sizeof(tmp_default));
  }
  if (root.containsKey("enable")) {
    unsigned char tmp_default[64];
    JsonArray& enableArray = root["enable"];
    memcpy(tmp_default, default_disable, sizeof(tmp_default));
    tmp_default[63] = DEFAULT_DISABLE_SIGNATURE;
    for (size_t i=0;i<enableArray.size();i++) {
      unsigned int ch;
      ch = enableArray[i];
      if (ch<numEnableChannels) {
        tmp_default[ch] = 0;
      }
    }
    Flash.erase(SEGMENT_B);
    Flash.write(SEGMENT_B, tmp_default, sizeof(tmp_default));
  }    
  
  if (root.containsKey("sn")) {
    Serial.print("{\"sn\":");
    Serial.print(calib[30]);
    Serial.println("}");
  }
  if (root.containsKey("fw")) {
    Serial.print("{\"fw\":\"");
    Serial.print(VERSION_STRING);
    Serial.println("\"}");
  }
  if (root.containsKey("dcocal")) {
    unsigned int idx = root["dcocal"];
    if (idx < 64) {
       unsigned char *infoa = (unsigned char *) 0x10C0;
       infoa += idx;
       Serial.print("{\"dcocal\":\"");
       Serial.print(*infoa);
       Serial.println("\"}");
    }
  }
  if (root.containsKey("passwd")) {
    unsigned int idx = root["passwd"];
    if (idx < 32) {
       unsigned char *pwd = (unsigned char *) 0xFFE0;
       pwd += idx;
       Serial.print("{\"passwd\":\"");
       Serial.print(*pwd);
       Serial.println("\"}");
    }
  }
       
  if (root.containsKey("calib")) {
    int tmp_calib[32];
    int val;
    unsigned char ch;
    unsigned int i;
    JsonArray& calibArray = root["calib"];
    for (i=0;i<32;i++) {
      tmp_calib[i] = calib[i];
    }
    ch = calibArray[0];
    val = calibArray[1];
    Serial.print("{\"log\":\"update cal");
    Serial.print(ch);
    Serial.print(" from ");
    Serial.print(calib[ch]);
    Serial.print(" to ");
    Serial.print(val);
    Serial.println("\"}");
    tmp_calib[ch] = val;
    Flash.erase(SEGMENT_D);
    Flash.write(SEGMENT_D, (unsigned char *) tmp_calib, 64);
  }
}
