#include <Arduino.h>
#include "HX711_MP.h"


//  10 calibration points
//  all user defined.
HX711_MP scale(10);


//  adjust pins to your setup.
uint8_t dataPin = 2;
uint8_t clockPin = 3;


float current_reading;
float max_reading;
unsigned long test_start_time; // Timestamp when test started

const byte escape_character = 'x'; //This is the ASCII character we look for to break reporting
boolean pause_mode = true; //This is set to true if user presses x
boolean json_mode = false; //Toggle between CSV and JSON output formats


// Function declarations
void outputCSV(float current, float peak);
void outputJSON(float current, float peak);
void outputReading(float current, float peak);
void displayMenu();
void handleMenuInput();
void measurementMode();


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
  scale.setCalibrate(0, 12697, 0);
  scale.setCalibrate(1, 81470, 0.994);
  scale.setCalibrate(2, 420690, 4.940);
  scale.setCalibrate(3, 609031, 7.362);
  scale.setCalibrate(4, 875551, 10.490);
  scale.setCalibrate(5, 1086675, 12.684);
  scale.setCalibrate(6, 1245437, 14.723);
  scale.setCalibrate(7, 1564460, 18.404);
  scale.setCalibrate(8, 1855791, 21.717);
  scale.setCalibrate(9, 2202545, 25.766);

  for (uint32_t raw = 0; raw <= 7000; raw += 20)
  {
    Serial.print(raw);
    Serial.print("\t");
    Serial.println(scale.testCalibration(raw));
  }
  delay(5000);
  
  test_start_time = millis(); // Initialize test start time
}

void loop() {
  if (pause_mode) {
    displayMenu();
    handleMenuInput();
    pause_mode = false;
  } else {
    measurementMode();
  }
}


// Output data in CSV format
void outputCSV(float current, float peak) {
  Serial.print(current);
  Serial.print(",");
  Serial.println(peak);
}


// Output data in JSON format for visualization apps
void outputJSON(float current, float peak) {
  float test_time_seconds = (millis() - test_start_time) / 1000.0;
  Serial.print("{\"timestamp\":");
  Serial.print(test_time_seconds, 3);
  Serial.print(",\"current\":");
  Serial.print(current, 3);
  Serial.print(",\"peak\":");
  Serial.print(peak, 3);
  Serial.println("}");
}


// Route output to appropriate format
void outputReading(float current, float peak) {
  if (json_mode) {
    outputJSON(current, peak);
  } else {
    outputCSV(current, peak);
  }
}


// Display the pause menu
void displayMenu() {
  Serial.print("Measurement Paused. Peak: ");
  Serial.print(max_reading);
  Serial.println(" kN");
  Serial.println();
  Serial.println("--------");
  Serial.println("r) Resume measurement");
  Serial.println("x) Start new test (reset peak and timestamp)");
  Serial.print("j) Toggle output format (current: ");
  Serial.print(json_mode ? "JSON" : "CSV");
  Serial.println(")");
  Serial.println("c) Enter calibration mode (future feature)");
}


// Handle user input from the menu
void handleMenuInput() {
  while (1) {
    if (Serial.available()) {
      char incoming = Serial.read();
      
      if (incoming == 'x') {
        max_reading = 0; // Clear max values
        test_start_time = millis(); // Reset test timestamp
        Serial.println("Starting new test...");
        break;
      }

      if (incoming == 'j') {
        json_mode = !json_mode; // Toggle JSON mode
        Serial.print("Output format changed to: ");
        Serial.println(json_mode ? "JSON" : "CSV");
        break;
      }

      if (incoming == 'c') {
        //calibrationMode();
        Serial.println("Calibration mode not yet implemented.");
        break;
      }

      if (incoming == 'r') {
        Serial.println("Resuming measurements...");
        break;
      }
    }
  }
}


// Continuous measurement mode
void measurementMode() {
  long startTime = millis();

  // Take measurement (5 samples averaged)
  current_reading = scale.get_units(5);

  // Update peak reading
  if (current_reading >= max_reading) {
    max_reading = current_reading;
  }

  // Output the reading
  outputReading(current_reading, max_reading);

  // Wait for report period to complete or check for escape character
  while (1) {
    if (Serial.available()) {
      char incoming = Serial.read();
      if (incoming == escape_character) {
        pause_mode = true;
        break;
      }
    }

    if ((millis() - startTime) >= 500) break;
  }
}

