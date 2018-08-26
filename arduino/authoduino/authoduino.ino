/*
   Authoduino is a modified version of the Doorduino code, as employed
   by Revspace and other hackerspaces. It differs in that instead of
   opening doors, it authorises a user to a certain resource. A door
   could also be a resource, in a way, but what makes Authoduino different
   is that it retains its state. Where the "door open"-state of Doorduino
   is a blocking state (it is implemented using the delay() function) and
   expires at some point, Authoduino retains the access or no access state,
   granted there is a frequent ping from the managing device.

   The protocol spoken by Authoduino is slightly different (but equally simple):
   It says "INIT" on init rather than "RESET", and responds to characters
   "E" & "D" (enabled & disabled) rather than characters "A" & "N" (access & no access).
   It lacks the challenge function, because it is not needed for my intended application.

   The application I wrote Authoduino for is managing access to an EV (electric vehicle)
   charging outlet. This system is located outside, meaning the device can be opened
   and hardware be examined. Storing iButton secrets in this reachable piece of hardware
   would greatly weaken the security of Revspace access control.
   The system authorises users solely on the id of their iButton, hence this code lacks
   the challenge function present in Doorduino.

   Like Doorduino, Authoduino outputs the id of a presented iButton to serial.
   If the managing device on the other end of the serial line decides to grant
   access to the resource, it sends "E", in my case enabling the charging function.
   If the user want to stop the charging operation and have their charging cable unlocked,
   they should present the same iButton again, causing the managing device to send "D".

   The managing device should also ping (char "P") at a regular interval. Not receiving pings or
   other commands for a defined period makes Authoduino go into abandoned mode, disabling
   access to the charging outlet in the process. The only way to return from this state is
   explicitly telling Authoduino to go into enabled or disabled state. Only a ping won't do.
*/

#include <OneWire.h> // Make sure the 1-Wire driver is in your library

// Board layout (Doorduino hardware)
#define PIN_LED_GREEN 13 // Green led in the iButton reader socket
#define PIN_LED_RED   12 // Red led in the iButton reader socket
#define PIN_RELAY     11 // Controls the relay used to close or cut SAE J1772 CP-pin connection between charging station and EV
#define PIN_1WIRE     10 // Pin used for 1-Wire communication bus (iButtons)
#define PIN_BUTTON     9 // Auxiliary button input, used on some Doorduino installs

// Led colors
#define OFF    0 // Undefined state (or off/broken)
#define GREEN  1 // Charging available
#define RED    2 // Stuff b0rked
#define YELLOW (GREEN | RED) // Yellow means ready for auth (same as Doorduino)

// Timing related definitions
#define INIT_DELAY               500 // Delay after all setup operations
#define LED_BLINK_INTERVAL      2000 // Be cool, blink to people/robots/ghosts/other things passing by
#define LED_BLINK_TIME           120 // Each fancy led blink last this much miliseconds
#define BUTTON_INTERVAL_TIME    3000 // After an iButton has been succesfully read, no new read operation for this much miliseconds
#define ABANDONED_TIME         30000 // Max interval between serial messages before Arduino considers the managing device down
#define ABANDONED_MSG_INTERVAL  5000 // The interval between each message in a bottle send over serial, when in the ABANDONED state

OneWire ds(PIN_1WIRE); // Dallas 1-Wire bus driver

enum Command {
  NONE,    // No command received
  ENABLE,  // Manager says user/customer is authorized
  DISABLE, // Manager says user/customer not authorized or wants their authorisation removed
  PING     // Sign of life
};

static enum State {
  ABANDONED, // Arduino has not had contact with the managing device for too long, or is yet to receive the first message
  ENABLED,  // Charging is available to the user/customer
  DISABLED // No charging possible, ready for a new customer
} state;  // State the system is in

// These values are in miliseconds since start of program. Ulongs are 32 bits on arduino, which gets you
// up to 4.294.967.295 msec or just over 49 and a half days. So values that affect state need some guards for millis() overflowing back to zero
static unsigned long lastblink = 0; // Last time we did a nice blinky on the led
static unsigned long lastread = 0;  // Last time an iButton was read
static unsigned long lastcmd = 0;   // Last time a sign of life was received from the managing device (affects device state!)
static unsigned long lastmsg = 0;   // Last time we have send a message to the managing device (currently used to limit the abandoned message over serial rate)

void setup () {
  led(YELLOW); // Led test (if the reader is green or red during power-up, the other color is broken)

  Serial.begin(115200);   // Same baudrate as Doorduino
  Serial.println("INIT"); // Doorduino says "RESET", Authoduino says "INIT"
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED,   OUTPUT);
  pinMode(PIN_RELAY,     OUTPUT);
  pinMode(PIN_BUTTON,    INPUT); // Unused at this moment. TODO: Make a car explode on push, set of a horn orchestra etc.

  state = ABANDONED; // When system initializes, nothing is possible until managing device has shown a sign of life

  delay(INIT_DELAY); // To make the led test effective (init is very fast)
}

void loop () {
  // Overflow protections regarding timing. Since millis() overflows back to zero
  // somewhere in the 49th day, some guards are required to prevent nutty behaviour
  if (millis() < lastcmd) {
    lastcmd = 0; // Overflow detected, reset lastcmd
  }
  // The other timing-related values are not affecting state. If anything, a weird led flicker is kinda interesting so yeah whatever

  // Check for messages from the managing device
  Command command = readSerial();

  // If any command was received, the managing device is still operational
  if (command != NONE) {
    lastcmd = millis();
  }

  // Commands that affect state are processed here
  switch (command) {
    case ENABLE: {
        state = ENABLED;
        break;
      }
    case DISABLE: {
        state = DISABLED;
        break;
      }
  }

  // If there has been no input from the managing device for some time, we consider ourselves abandoned.
  // First compare loops backwards to near max value first msecs of program operation, second compare is to catch that
  if (millis() - ABANDONED_TIME > lastcmd && millis() > ABANDONED_TIME) {
    state = ABANDONED;
  }

  // Read the 1-Wire bus, see if there's an iButton
  if (state != ABANDONED) { // We only care about iButtons if there's a managing device present
    // First compare loops backwards to near max value first msecs of program operation, hence the second compare
    if (millis() - BUTTON_INTERVAL_TIME > lastread && millis() > BUTTON_INTERVAL_TIME) {
      if (readButton()) {
        lastread = millis(); // A button was successfully read
      }
    }
  }

  // Allow or not allow charging, depending on what input has been given before
  switch (state) {
    case ENABLED: {
        digitalWrite(PIN_RELAY, HIGH); // By enabling the relay, the SAE J1772 CP-pin is enabled, making charging possible
        led(GREEN);
        break;
      }
    case DISABLED: {
        digitalWrite(PIN_RELAY, LOW); // By disabling the relay, the SAE J1772 CP-pin connection is cut
        led(YELLOW);
        break;
      }
    case ABANDONED: {
        digitalWrite(PIN_RELAY, LOW); // By disabling the relay, the SAE J1772 CP-pin connection is cut
        led(RED);
        if (millis() - ABANDONED_MSG_INTERVAL > lastmsg && millis() > ABANDONED_MSG_INTERVAL) {
          Serial.println("ABANDONED"); // I hope that someone gets my...
          lastmsg = millis();
        }
        break;
      }
  }
}

void led (byte color) {
  if (millis() - LED_BLINK_INTERVAL > lastblink && millis() > LED_BLINK_INTERVAL) {
    digitalWrite(PIN_LED_GREEN, 0);
    digitalWrite(PIN_LED_RED,   0);
    lastblink = millis();
  }

  if (millis() - LED_BLINK_TIME > lastblink) {
    digitalWrite(PIN_LED_GREEN, color & GREEN);
    digitalWrite(PIN_LED_RED,   color & RED);
  }
}

bool readButton() {
  byte id[8]; // The id of the iButton will be written into this variable by ds.search()

  if (ds.reset()) { // After a reset pulse, if one or more slave devices are on the bus, reset returns nonzero
    ds.reset_search(); // 1-Wire bus can have multiple slave devices. Reset next use of search() to start/first position
    if (ds.search(id)) { // Do a search until something is found, write the ID into id (stateful; next call returns next device). Returns nonzero if a new ID was written to id
      if (OneWire::crc8(id, 7) != id[7]) {
        return false; // If checksum fails, return
      }
      led(OFF);
      Serial.print("<");
      for (byte i = 0; i < 8; i++) { // Dump the found ID to serial out
        if (id[i] < 16) Serial.print("0");
        Serial.print(id[i], HEX);
      }
      Serial.println(">");
      return true;
    }
  }

  return false;
}

Command readSerial() {
  while (Serial.available()) { // Process all received data
    char c = Serial.read(); // one char at the time

    if (c == 'E') { // Enable charging (grant access)
      Serial.println("ENABLED");
      return ENABLE;
    }
    else if (c == 'D') { // Disable charging (no longer access)
      Serial.println("DISABLED");
      return DISABLE;
    }
    else if (c == 'P') { // Yo still there bro?
      Serial.println("PONG");
      return PING;
    }
  }

  return NONE;
}

