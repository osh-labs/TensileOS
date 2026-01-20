#include <Arduino.h>
#include "HX711_MP.h"


//  10 calibration points
//  all user defined.
HX711_MP scale(10);


//  adjust pins to your setup.
uint8_t dataPin = 16;
uint8_t clockPin = 17;


float current_reading;
float max_reading;

const byte escape_character = 'x'; //This is the ASCII character we look for to break reporting
boolean pause_mode = true; //This is set to true if user presses x



void setup() {
  Serial.begin(115200);
  // Serial.println(__FILE__);
  // Serial.print("HX711_MP_LIB_VERSION: ");
  // Serial.println(HX711_MP_LIB_VERSION);
  // Serial.println();

  scale.begin(dataPin, clockPin);

  //  Calibration
  //  adjust the data to your measurements
  //  setCalibrate(index, rawRead, weight);
  scale.setCalibrate(0, 1000, -10000);
  scale.setCalibrate(1, 1300, 0);
  scale.setCalibrate(2, 2000, 20000);
  scale.setCalibrate(3, 4000, 30000);
  scale.setCalibrate(4, 5000, 40000);
  scale.setCalibrate(5, 5200, 50000);
  scale.setCalibrate(6, 6000, 60000);
  scale.setCalibrate(7, 6500, 70000);
  scale.setCalibrate(8, 6750, 80000);
  scale.setCalibrate(9, 6900, 90000);

  for (uint32_t raw = 0; raw <= 7000; raw += 20)
  {
    Serial.print(raw);
    Serial.print("\t");
    Serial.println(scale.testCalibration(raw));
  }
  delay(5000);
}

void loop() {
  long startTime = millis();

  //  continuous scale 4x per second
  current_reading = scale.get_units(5);

  if (current_reading >= max_reading) {
    max_reading = current_reading;
  }

  Serial.print(current_reading);
  Serial.print(",");
  Serial.println(max_reading);
  

  //Hang out until the end of this report period
  while (1)
  {
    //If we see escape char then drop to setup menu
    if (Serial.available())
    {
      char incoming = Serial.read();

      if (incoming == escape_character) 
      {
        pause_mode = true;  //For Trigger Character Feature
        break; //So we can enter the setup menu
      }
    }

    if ((millis() - startTime) >= 100) break;
  }

  if (pause_mode == true) {
    Serial.print("Measurement Paused. Peak: ");
    Serial.print(max_reading);
    Serial.println(" kN");
    Serial.println();
    Serial.println("--------");
    Serial.println("r) Resume measurement");
    Serial.println("x) Clear peak reading and resume measurement");
    Serial.println("c) Enter calibration mode (future feature)");

    while (1)
    {
      if (Serial.available())
      {
        char incoming = Serial.read();
        if (incoming == 'x') {
          max_reading = 0; // Clear max values
          break;
        }

        if (incoming == 'c') {
          //calibrationMode();
          break;
        }

        if (incoming == 'r') break;
      }
    }
    pause_mode = false;
  }
}

