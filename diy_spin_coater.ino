/**
 * v1.0
 *
 * DIY Spin Coater by Johannes Knotz
 * 
 * This basic firmware allows a stepper motor to act as a capable, repeatable 
 * spin coater by allowing the user to configure, save and run velocity 
 * ramps with minimal hard and software.
 *
 * Required software
 * - Arduino IDE 2
 * - FastAccelStepper library
 *
 * Standard profiles
 * - 8 profiles
 * - Default Init RPM: 500
 * - Default Spin RPM: 2000
 * - Default Duration: 30 s
 *
 * Button behavior:
 * - Short + / - : 5 RPM
 * - Hold + / -  : automatic repeat
 * - After 700 ms: 25 RPM per repeat
 * - After 2.0 s : 100 RPM per repeat
 *
 * Hardware:
 * ESP32
 * SH1106 128x64 I2C OLED
 * FastAccelStepper
 */

#include <Arduino.h>
#include "FastAccelStepper.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>

// ============================================================
// HARDWARE PIN DEFINITIONS
// ============================================================

#define PIN_SDA          22
#define PIN_SCL          21

#define BTN_SWITCH_PIN   32
#define BTN_PLUS_PIN     33
#define BTN_MINUS_PIN    25
#define BTN_STOP_PIN     26
#define BTN_START_PIN    27

#define PIN_STEP         14
#define PIN_DIR          12
#define PIN_ENABLE       13


// ============================================================
// CONFIGURATION
// ============================================================

#define MICROSTEPPING    1600

#define MAX_RPM          5000
#define MIN_RPM          0

#define MAX_TIME_S       180
#define MIN_TIME_S       5

#define ACCEL_RPM_S      2000

#define NUM_PROFILES     8

// Normal RPM change
#define RPM_STEP_SIZE    5

// Faster changes while holding + / -
#define RPM_FAST_STEP_SIZE   25
#define RPM_MAX_STEP_SIZE    100

// Button timing
#define BUTTON_DEBOUNCE_MS   60
#define HOLD_START_MS        700
#define HOLD_FAST_MS         2000
#define HOLD_REPEAT_MS       120


// ============================================================
// OBJECTS
// ============================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0,
  /* reset=*/ U8X8_PIN_NONE,
  /* clock=*/ PIN_SCL,
  /* data=*/ PIN_SDA
);

FastAccelStepperEngine engine;
FastAccelStepper *stepper = NULL;

Preferences preferences;


// ============================================================
// STATE MACHINE
// ============================================================

enum SystemState {
  STATE_HOME,
  STATE_INIT_SPIN,
  STATE_MAIN_SPIN
};

SystemState currentState = STATE_HOME;


// ============================================================
// PARAMETERS
// ============================================================

struct Params {
  int32_t initRpm;
  int32_t spinRpm;
  int32_t durationSec;
};

Params profiles[NUM_PROFILES];

int currentProfileIndex = 0;
int menuSelection = 0;


// ============================================================
// BUTTON STRUCTURE
// ============================================================

struct ButtonState {

  uint8_t pin;

  bool rawState;
  bool stableState;

  unsigned long lastRawChange;
  unsigned long pressStartTime;
  unsigned long lastRepeatTime;

  bool holdStarted;
};


// ============================================================
// BUTTON INSTANCES
// ============================================================

ButtonState buttonSwitch = {
  BTN_SWITCH_PIN,
  HIGH,
  HIGH,
  0,
  0,
  0,
  false
};

ButtonState buttonPlus = {
  BTN_PLUS_PIN,
  HIGH,
  HIGH,
  0,
  0,
  0,
  false
};

ButtonState buttonMinus = {
  BTN_MINUS_PIN,
  HIGH,
  HIGH,
  0,
  0,
  0,
  false
};

ButtonState buttonStop = {
  BTN_STOP_PIN,
  HIGH,
  HIGH,
  0,
  0,
  0,
  false
};

ButtonState buttonStart = {
  BTN_START_PIN,
  HIGH,
  HIGH,
  0,
  0,
  0,
  false
};


// ============================================================
// BUTTON EVENTS
// ============================================================

enum ButtonEvent {
  BUTTON_NONE,
  BUTTON_PRESSED,
  BUTTON_HELD
};


// ============================================================
// RUNTIME VARIABLES
// ============================================================

unsigned long spinStartTime = 0;

bool displayNeedsUpdate = true;


// ============================================================
// BUTTON UPDATE
// ============================================================

ButtonEvent updateButton(ButtonState &button) {

  unsigned long now = millis();

  bool reading = digitalRead(button.pin);

  // ----------------------------------------------------------
  // Raw input changed
  // ----------------------------------------------------------

  if (reading != button.rawState) {

    button.rawState = reading;
    button.lastRawChange = now;
  }

  // ----------------------------------------------------------
  // Wait for debounce
  // ----------------------------------------------------------

  if ((now - button.lastRawChange) < BUTTON_DEBOUNCE_MS) {
    return BUTTON_NONE;
  }

  // ----------------------------------------------------------
  // Stable state changed
  // ----------------------------------------------------------

  if (reading != button.stableState) {

    button.stableState = reading;

    // Button pressed
    if (button.stableState == LOW) {

      button.pressStartTime = now;
      button.lastRepeatTime = now;
      button.holdStarted = false;

      return BUTTON_PRESSED;
    }

    // Button released
    button.holdStarted = false;

    return BUTTON_NONE;
  }

  // ----------------------------------------------------------
  // Button is being held
  // ----------------------------------------------------------

  if (button.stableState == LOW) {

    unsigned long heldTime =
      now - button.pressStartTime;

    // Start repeating after HOLD_START_MS
    if (heldTime >= HOLD_START_MS) {

      // First repeat
      if (!button.holdStarted) {

        button.holdStarted = true;
        button.lastRepeatTime = now;

        return BUTTON_HELD;
      }

      // Subsequent repeats
      if ((now - button.lastRepeatTime) >= HOLD_REPEAT_MS) {

        button.lastRepeatTime = now;

        return BUTTON_HELD;
      }
    }
  }

  return BUTTON_NONE;
}


// ============================================================
// GET RPM STEP FOR A HELD BUTTON
// ============================================================

int32_t getRpmStepSize(ButtonState &button) {

  unsigned long heldTime =
    millis() - button.pressStartTime;

  if (heldTime >= HOLD_FAST_MS) {
    return (int32_t)RPM_MAX_STEP_SIZE;
  }

  if (heldTime >= HOLD_START_MS) {
    return (int32_t)RPM_FAST_STEP_SIZE;
  }

  return (int32_t)RPM_STEP_SIZE;
}


// ============================================================
// RPM -> STEP FREQUENCY
// ============================================================

int32_t rpmToHz(int32_t rpm) {

  if (rpm < 1) {
    return 0;
  }

  return (int32_t)(
    (float)rpm *
    (float)MICROSTEPPING /
    60.0f
  );
}


// ============================================================
// ACCELERATION -> STEPS/S2
// ============================================================

uint32_t accelToSteps(int32_t rpm_s) {

  return (uint32_t)(
    (float)rpm_s *
    (float)MICROSTEPPING /
    60.0f
  );
}


// ============================================================
// PREFERENCES KEY
// ============================================================

String getKey(const char *base, int index) {

  return String(base) + String(index);
}


// ============================================================
// SAVE PROFILE
// ============================================================

void saveProfile(int index) {

  preferences.putInt(
    getKey("init", index).c_str(),
    (int)profiles[index].initRpm
  );

  preferences.putInt(
    getKey("spin", index).c_str(),
    (int)profiles[index].spinRpm
  );

  preferences.putInt(
    getKey("time", index).c_str(),
    (int)profiles[index].durationSec
  );
}


// ============================================================
// CHANGE INIT RPM
// ============================================================

void changeInitRpm(int32_t amount) {

  Params &p = profiles[currentProfileIndex];

  int32_t newRpm = p.initRpm + amount;

  if (newRpm > (int32_t)MAX_RPM) {
    newRpm = (int32_t)MAX_RPM;
  }

  if (newRpm < (int32_t)MIN_RPM) {
    newRpm = (int32_t)MIN_RPM;
  }

  p.initRpm = newRpm;

  saveProfile(currentProfileIndex);

  displayNeedsUpdate = true;
}


// ============================================================
// CHANGE SPIN RPM
// ============================================================

void changeSpinRpm(int32_t amount) {

  Params &p = profiles[currentProfileIndex];

  int32_t newRpm = p.spinRpm + amount;

  if (newRpm > (int32_t)MAX_RPM) {
    newRpm = (int32_t)MAX_RPM;
  }

  if (newRpm < (int32_t)MIN_RPM) {
    newRpm = (int32_t)MIN_RPM;
  }

  p.spinRpm = newRpm;

  saveProfile(currentProfileIndex);

  displayNeedsUpdate = true;
}


// ============================================================
// CHANGE TIME
// ============================================================

void changeTime(int32_t amount) {

  Params &p = profiles[currentProfileIndex];

  int32_t newTime = p.durationSec + amount;

  if (newTime > (int32_t)MAX_TIME_S) {
    newTime = (int32_t)MAX_TIME_S;
  }

  if (newTime < (int32_t)MIN_TIME_S) {
    newTime = (int32_t)MIN_TIME_S;
  }

  p.durationSec = newTime;

  saveProfile(currentProfileIndex);

  displayNeedsUpdate = true;
}


// ============================================================
// CHANGE PROFILE
// ============================================================

void changeProfile(int direction) {

  // Save current profile before changing
  saveProfile(currentProfileIndex);

  if (direction > 0) {

    currentProfileIndex++;

    if (currentProfileIndex >= NUM_PROFILES) {
      currentProfileIndex = 0;
    }

  } else {

    currentProfileIndex--;

    if (currentProfileIndex < 0) {
      currentProfileIndex = NUM_PROFILES - 1;
    }
  }

  preferences.putInt(
    "lastProf",
    currentProfileIndex
  );

  displayNeedsUpdate = true;
}


// ============================================================
// DISPLAY
// ============================================================

void updateDisplay() {

  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_6x10_tf);

  Params &p =
    profiles[currentProfileIndex];

  char buf[32];

  // ----------------------------------------------------------
  // Init RPM
  // ----------------------------------------------------------

  if (menuSelection == 0) {
    u8g2.drawStr(0, 12, ">");
  }

  sprintf(
    buf,
    "Init: %ld RPM",
    (long)p.initRpm
  );

  u8g2.drawStr(10, 12, buf);


  // ----------------------------------------------------------
  // Spin RPM
  // ----------------------------------------------------------

  if (menuSelection == 1) {
    u8g2.drawStr(0, 24, ">");
  }

  sprintf(
    buf,
    "Spin: %ld RPM",
    (long)p.spinRpm
  );

  u8g2.drawStr(10, 24, buf);


  // ----------------------------------------------------------
  // Time
  // ----------------------------------------------------------

  if (menuSelection == 2) {
    u8g2.drawStr(0, 36, ">");
  }

  sprintf(
    buf,
    "Time: %ld s",
    (long)p.durationSec
  );

  u8g2.drawStr(10, 36, buf);


  // ----------------------------------------------------------
  // Profile
  // ----------------------------------------------------------

  if (menuSelection == 3) {
    u8g2.drawStr(0, 48, ">");
  }

  sprintf(
    buf,
    "Profile: #%d",
    currentProfileIndex + 1
  );

  u8g2.drawStr(10, 48, buf);


  // ----------------------------------------------------------
  // Status
  // ----------------------------------------------------------

  if (currentState == STATE_HOME) {

    u8g2.drawStr(
      0,
      58,
      "Status: READY"
    );

  } else if (currentState == STATE_INIT_SPIN) {

    u8g2.drawStr(
      0,
      58,
      "Status: DEPOSITING"
    );

  } else if (currentState == STATE_MAIN_SPIN) {

    u8g2.drawStr(
      0,
      58,
      "Status: SPINNING"
    );
  }


  // ----------------------------------------------------------
  // Profile indicator
  // ----------------------------------------------------------

  int blockWidth =
    128 / NUM_PROFILES;

  u8g2.drawFrame(
    0,
    60,
    128,
    4
  );

  u8g2.drawBox(
    currentProfileIndex * blockWidth,
    60,
    blockWidth,
    4
  );

  u8g2.sendBuffer();
}


// ============================================================
// START SPIN
// ============================================================

void startSpin() {

  if (stepper == NULL) {
    return;
  }

  if (currentState == STATE_HOME) {

    currentState = STATE_INIT_SPIN;

    displayNeedsUpdate = true;

    stepper->enableOutputs();

    stepper->setSpeedInHz(
      rpmToHz(
        profiles[currentProfileIndex].initRpm
      )
    );

    stepper->runForward();

  } else if (currentState == STATE_INIT_SPIN) {

    currentState = STATE_MAIN_SPIN;

    displayNeedsUpdate = true;

    spinStartTime = millis();

    stepper->setSpeedInHz(
      rpmToHz(
        profiles[currentProfileIndex].spinRpm
      )
    );

    stepper->applySpeedAcceleration();
  }
}


// ============================================================
// STOP SPIN
// ============================================================

void stopSpin() {

  if (stepper == NULL) {
    return;
  }

  if (currentState != STATE_HOME) {

    stepper->stopMove();

    while (stepper->isRunning()) {
      delay(10);
    }

    stepper->disableOutputs();

    currentState = STATE_HOME;

    displayNeedsUpdate = true;
  }
}


// ============================================================
// PROCESS SWITCH BUTTON
// ============================================================

void processSwitchButton() {

  if (currentState != STATE_HOME) {
    return;
  }

  menuSelection++;

  if (menuSelection >= 4) {
    menuSelection = 0;
  }

  displayNeedsUpdate = true;
}


// ============================================================
// PROCESS PLUS BUTTON
// ============================================================

void processPlusButton(ButtonEvent event) {

  if (currentState != STATE_HOME) {
    return;
  }

  if (
    event != BUTTON_PRESSED &&
    event != BUTTON_HELD
  ) {
    return;
  }

  Params &p =
    profiles[currentProfileIndex];

  // ----------------------------------------------------------
  // Init RPM
  // ----------------------------------------------------------

  if (menuSelection == 0) {

    int32_t step =
      getRpmStepSize(buttonPlus);

    changeInitRpm(step);
  }

  // ----------------------------------------------------------
  // Spin RPM
  // ----------------------------------------------------------

  else if (menuSelection == 1) {

    int32_t step =
      getRpmStepSize(buttonPlus);

    changeSpinRpm(step);
  }

  // ----------------------------------------------------------
  // Time
  // ----------------------------------------------------------

  else if (menuSelection == 2) {

    changeTime(5);
  }

  // ----------------------------------------------------------
  // Profile
  // ----------------------------------------------------------

  else if (menuSelection == 3) {

    // Only change profile once per physical press.
    if (event == BUTTON_PRESSED) {
      changeProfile(+1);
    }
  }
}


// ============================================================
// PROCESS MINUS BUTTON
// ============================================================

void processMinusButton(ButtonEvent event) {

  if (currentState != STATE_HOME) {
    return;
  }

  if (
    event != BUTTON_PRESSED &&
    event != BUTTON_HELD
  ) {
    return;
  }

  Params &p =
    profiles[currentProfileIndex];

  // ----------------------------------------------------------
  // Init RPM
  // ----------------------------------------------------------

  if (menuSelection == 0) {

    int32_t step =
      getRpmStepSize(buttonMinus);

    changeInitRpm(-step);
  }

  // ----------------------------------------------------------
  // Spin RPM
  // ----------------------------------------------------------

  else if (menuSelection == 1) {

    int32_t step =
      getRpmStepSize(buttonMinus);

    changeSpinRpm(-step);
  }

  // ----------------------------------------------------------
  // Time
  // ----------------------------------------------------------

  else if (menuSelection == 2) {

    changeTime(-5);
  }

  // ----------------------------------------------------------
  // Profile
  // ----------------------------------------------------------

  else if (menuSelection == 3) {

    // Only change profile once per physical press.
    if (event == BUTTON_PRESSED) {
      changeProfile(-1);
    }
  }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  // ----------------------------------------------------------
  // Display
  // ----------------------------------------------------------

  u8g2.begin();


  // ----------------------------------------------------------
  // Preferences
  // ----------------------------------------------------------

  preferences.begin(
    "spin-coater",
    false
  );

  currentProfileIndex =
    preferences.getInt(
      "lastProf",
      0
    );

  if (
    currentProfileIndex < 0 ||
    currentProfileIndex >= NUM_PROFILES
  ) {
    currentProfileIndex = 0;
  }


  // ----------------------------------------------------------
  // Load profiles
  //
  // All profiles use standard defaults if nothing has been
  // saved yet.
  // ----------------------------------------------------------

  for (int i = 0; i < NUM_PROFILES; i++) {

    profiles[i].initRpm =
      preferences.getInt(
        getKey("init", i).c_str(),
        500
      );

    profiles[i].spinRpm =
      preferences.getInt(
        getKey("spin", i).c_str(),
        2000
      );

    profiles[i].durationSec =
      preferences.getInt(
        getKey("time", i).c_str(),
        30
      );


    // Safety clamp loaded values

    if (profiles[i].initRpm < MIN_RPM) {
      profiles[i].initRpm = MIN_RPM;
    }

    if (profiles[i].initRpm > MAX_RPM) {
      profiles[i].initRpm = MAX_RPM;
    }

    if (profiles[i].spinRpm < MIN_RPM) {
      profiles[i].spinRpm = MIN_RPM;
    }

    if (profiles[i].spinRpm > MAX_RPM) {
      profiles[i].spinRpm = MAX_RPM;
    }

    if (profiles[i].durationSec < MIN_TIME_S) {
      profiles[i].durationSec = MIN_TIME_S;
    }

    if (profiles[i].durationSec > MAX_TIME_S) {
      profiles[i].durationSec = MAX_TIME_S;
    }
  }


  // ----------------------------------------------------------
  // Buttons
  // ----------------------------------------------------------

  pinMode(
    BTN_SWITCH_PIN,
    INPUT_PULLUP
  );

  pinMode(
    BTN_PLUS_PIN,
    INPUT_PULLUP
  );

  pinMode(
    BTN_MINUS_PIN,
    INPUT_PULLUP
  );

  pinMode(
    BTN_STOP_PIN,
    INPUT_PULLUP
  );

  pinMode(
    BTN_START_PIN,
    INPUT_PULLUP
  );


  // Initialize button states from actual hardware state

  unsigned long now = millis();

  buttonSwitch.rawState =
    digitalRead(BTN_SWITCH_PIN);

  buttonSwitch.stableState =
    buttonSwitch.rawState;

  buttonSwitch.lastRawChange =
    now;


  buttonPlus.rawState =
    digitalRead(BTN_PLUS_PIN);

  buttonPlus.stableState =
    buttonPlus.rawState;

  buttonPlus.lastRawChange =
    now;


  buttonMinus.rawState =
    digitalRead(BTN_MINUS_PIN);

  buttonMinus.stableState =
    buttonMinus.rawState;

  buttonMinus.lastRawChange =
    now;


  buttonStop.rawState =
    digitalRead(BTN_STOP_PIN);

  buttonStop.stableState =
    buttonStop.rawState;

  buttonStop.lastRawChange =
    now;


  buttonStart.rawState =
    digitalRead(BTN_START_PIN);

  buttonStart.stableState =
    buttonStart.rawState;

  buttonStart.lastRawChange =
    now;


  // ----------------------------------------------------------
  // Stepper
  // ----------------------------------------------------------

  engine.init();

  stepper =
    engine.stepperConnectToPin(PIN_STEP);

  if (stepper != NULL) {

    stepper->setDirectionPin(
      PIN_DIR,
      true,
      500
    );

    stepper->setEnablePin(
      PIN_ENABLE
    );

    stepper->setAutoEnable(false);

    stepper->setAcceleration(
      accelToSteps(ACCEL_RPM_S)
    );
  }


  // ----------------------------------------------------------
  // Initial display
  // ----------------------------------------------------------

  updateDisplay();

  displayNeedsUpdate = false;
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  unsigned long now = millis();


  // ==========================================================
  // READ BUTTONS
  // ==========================================================

  ButtonEvent switchEvent =
    updateButton(buttonSwitch);

  ButtonEvent plusEvent =
    updateButton(buttonPlus);

  ButtonEvent minusEvent =
    updateButton(buttonMinus);

  ButtonEvent stopEvent =
    updateButton(buttonStop);

  ButtonEvent startEvent =
    updateButton(buttonStart);


  // ==========================================================
  // START BUTTON
  // ==========================================================

  if (startEvent == BUTTON_PRESSED) {

    startSpin();
  }


  // ==========================================================
  // STOP BUTTON
  // ==========================================================

  if (stopEvent == BUTTON_PRESSED) {

    stopSpin();
  }


  // ==========================================================
  // SWITCH BUTTON
  // ==========================================================

  if (switchEvent == BUTTON_PRESSED) {

    processSwitchButton();
  }


  // ==========================================================
  // PLUS BUTTON
  // ==========================================================

  processPlusButton(plusEvent);


  // ==========================================================
  // MINUS BUTTON
  // ==========================================================

  processMinusButton(minusEvent);


  // ==========================================================
  // AUTOMATIC SPIN TIMER
  // ==========================================================

  if (currentState == STATE_MAIN_SPIN) {

    uint32_t targetMs =
      (uint32_t)
      profiles[currentProfileIndex].durationSec
      * 1000UL;

    if ((now - spinStartTime) >= targetMs) {

      stepper->stopMove();

      if (!stepper->isRunning()) {

        stepper->disableOutputs();

        currentState = STATE_HOME;

        displayNeedsUpdate = true;
      }
    }
  }


  // ==========================================================
  // DISPLAY UPDATE
  // ==========================================================

  if (displayNeedsUpdate) {

    updateDisplay();

    displayNeedsUpdate = false;
  }


  // Small yield so the ESP32 remains responsive
  delay(1);
}
