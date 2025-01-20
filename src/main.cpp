
#include "Arduino.h"
#include "EEPROM.h"
#include "TinyStateMachine.h"

#define ALARM_TRIGGERED_ADDRESS 0
#define BUTTON_PIN 2
#define KICKSTAND_PIN 3
#define ALARM_PIN 4
#define RED_PIN 5
#define GREEN_PIN 6
#define BLUE_PIN 7

#define OFF 000, 000, 000
#define RED 255, 000, 000
#define GREEN 000, 255, 000

#define ALARM_BEEP_TIME 1000 // time on and off in ms.

TinyStateMachine tsm = TinyStateMachine(10, 20);

struct {
    bool alarm_triggered = false;
    unsigned long state_change_time = 0;
} state_data;


state_t START_STATE;
state_t WAIT_FOR_BUTTON_PRESS_STATE;
state_t WAIT_FOR_KICKSTAND_DOWN_STATE;
state_t WAIT_FOR_BUTTON_RELEASE_STATE;
state_t ALARM_ARMED_STATE;
state_t ALARM_TRIGGERED_STATE;
state_t WAIT_FOR_KICKSTAND_UP_STATE;


/**
 * FORWARD DECLARATIONS
 */
bool check_button(uint8_t pin);

void set_status_led(uint8_t r, uint8_t g, uint8_t b);


void setup() {

    // alarm relay should be pinout,
    pinMode(ALARM_PIN, OUTPUT);
    pinMode(KICKSTAND_PIN, INPUT_PULLUP);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // setup led pins
    pinMode(RED_PIN, OUTPUT);
    pinMode(GREEN_PIN, OUTPUT);
    pinMode(BLUE_PIN, OUTPUT);
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);

    state_data.state_change_time = millis();
    EEPROM.write(ALARM_TRIGGERED_ADDRESS, false);
    state_data.alarm_triggered = false;

    // START_STATE state. checks if, on last power off, the state had the alarm in the off state or not.
    START_STATE = tsm.add_state_enter([] {
        state_data.alarm_triggered = EEPROM.read(ALARM_TRIGGERED_ADDRESS);
        Serial.println("STATE START");
        set_status_led(GREEN); // green just for startup.
        analogWrite(LED_BUILTIN, 0);
    });

    // WAIT_FOR_BUTTON_PRESS_STATE state. waits for a button press and doesn't do anything.
    // this is the state that the state machine will be in the majority of the time.
    // the led is turned off when entering this state.
    WAIT_FOR_BUTTON_PRESS_STATE = tsm.add_state_enter([] {
        set_status_led(OFF);
        analogWrite(LED_BUILTIN, 255);
        Serial.println("STATE WAIT_FOR_BUTTON_PRESS_STATE");
    });


    // WAIT_FOR_KICKSTAND_DOWN_STATE state. called when button is pressed but kickstand isn't down yet.
    // need to turn on the status LED to GREEN for put-down-kickstand in this state.
    WAIT_FOR_KICKSTAND_DOWN_STATE = tsm.add_state_enter([] {
        set_status_led(GREEN);
        Serial.println("STATE WAIT_FOR_KICKSTAND_DOWN_STATE");
    });


    // WAIT_FOR_BUTTON_RELEASE_STATE state. After kickstand goes down, need to wait for the button to release to arm
    // the alarm.
    // set the alarm to RED for ARMED.
    WAIT_FOR_BUTTON_RELEASE_STATE = tsm.add_state_enter([] {
        set_status_led(RED);
        Serial.println("STATE WAIT_FOR_BUTTON_RELEASE_STATE");
    });


    // ALARM_ARMED_STATE state. Alarm is turned on and ready.
    // state doesn't do anything, but exits when the kickstand is up. LED is turned off when entering this state.
    // led is off in this state.
    ALARM_ARMED_STATE = tsm.add_state_enter([] {
        set_status_led(OFF);
        digitalWrite(ALARM_PIN, LOW);
        Serial.println("STATE ALARM_ARMED_STATE");
    });


    // ALARM_TRIGGERED_STATE state. Alarm has been triggered. Play sound on enter, and make it beep in the loop.
    // exit doesn't do anything, we only turn off the alarm when the button is pressed and the kickstand goes up.
    // Not in this state.
    ALARM_TRIGGERED_STATE = tsm.add_state_el(
            [] {
                digitalWrite(ALARM_PIN, HIGH);
                EEPROM.update(ALARM_TRIGGERED_ADDRESS, true);
                state_data.alarm_triggered = true;
                set_status_led(OFF); // turn off light so it doesn't drain battery.
                Serial.println("STATE ALARM_TRIGGERED_STATE");
            }, [] {
                auto time_since_start_ms = millis() - state_data.state_change_time;
                if ((time_since_start_ms / ALARM_BEEP_TIME) % 2 == 0) {
                    digitalWrite(ALARM_PIN, HIGH);
                } else {
                    digitalWrite(ALARM_PIN, LOW);
                }
            }
    );


    // WAIT_FOR_KICKSTAND_UP_STATE state. Alarm is still on, the trigger has to be pressed.
    WAIT_FOR_KICKSTAND_UP_STATE = tsm.add_state_ee(
            [] {
                Serial.println("STATE WAIT_FOR_KICKSTAND_UP_STATE");
                set_status_led(RED);
            }, [] {
                digitalWrite(ALARM_PIN, LOW);
                EEPROM.update(ALARM_TRIGGERED_ADDRESS, false);
                state_data.alarm_triggered = false;
            }
    );

    // every time we enter a state, we update the time when we entered the state.
    tsm.add_every_state_enter([] {
        state_data.state_change_time = millis();
        Serial.println(String("Entered new state at: ") + state_data.state_change_time + " ms");
    });

    // on startup, transition to alarm if the alarm was triggered on shutdown previously.
    tsm.add_transition(START_STATE, ALARM_TRIGGERED_STATE, [] {
        Serial.println("checking START to ALARM_TRIGGERED_STATE");
        return state_data.alarm_triggered;
    });

    // on startup, transition to wait for button press if alarm was not triggered on shutdown previously.
    tsm.add_transition(START_STATE, WAIT_FOR_BUTTON_PRESS_STATE, [] {
        return !state_data.alarm_triggered;
    });

    // go to kickstand down state if the button is pressed.
    // not checking if kickstand is up here. That way, if bike is ready, parked, and button is pressed, it'll go
    // directly into the armed state.
    tsm.add_transition(WAIT_FOR_BUTTON_PRESS_STATE, WAIT_FOR_KICKSTAND_DOWN_STATE, [] {
        return check_button(BUTTON_PIN);
    });

    // if the button is released while kickstand is still up, go back to waiting for button press.
    tsm.add_transition(WAIT_FOR_KICKSTAND_DOWN_STATE, WAIT_FOR_BUTTON_PRESS_STATE, [] {
        return !check_button(BUTTON_PIN);
    });

    // go to waiting for button release state if kickstand goes down after button is pressed.
    tsm.add_transition(WAIT_FOR_KICKSTAND_DOWN_STATE, WAIT_FOR_BUTTON_RELEASE_STATE, [] {
        return check_button(KICKSTAND_PIN);
    });


    // if kickstand goes up while waiting for the button to be released, we assume that the user is adjusting the bike
    // position. Go back to waiting for the kickstand to go down.
    tsm.add_transition(WAIT_FOR_BUTTON_RELEASE_STATE, WAIT_FOR_KICKSTAND_DOWN_STATE, [] {
        return !check_button(KICKSTAND_PIN);
    });

    // if button is released and kickstand is still down, we transition to the armed state. Alarm is now armed and dangerous.
    tsm.add_transition(WAIT_FOR_BUTTON_RELEASE_STATE, ALARM_ARMED_STATE, [] {
        return !check_button(BUTTON_PIN);
    });

    // if kickstand goes up while alarm is armed, transition into alarm triggered state
    tsm.add_transition(ALARM_ARMED_STATE, ALARM_TRIGGERED_STATE, [] {
        return !check_button(KICKSTAND_PIN);
    });

    // if button is pressed in alarm armed state, go back to waiting for button release.
    tsm.add_transition(ALARM_ARMED_STATE, WAIT_FOR_BUTTON_RELEASE_STATE, [] {
        return check_button(BUTTON_PIN);
    });

    // if button is pressed while alarm is on, wait for kickstand to go up. Only transition if the kickstand is down.
    // that way, if someone triggers the alarm, you have to put the kickstand back down before it can be silenced.
    tsm.add_transition(ALARM_TRIGGERED_STATE, WAIT_FOR_KICKSTAND_UP_STATE, [] {
        return check_button(BUTTON_PIN) && check_button(KICKSTAND_PIN);
    });

    // if the alarm is currently triggered, but time (2 min) has gone by since the alarm was triggered and kickstand
    // is down again, then turn off the alarm and go back to armed state.
    tsm.add_transition(ALARM_TRIGGERED_STATE, ALARM_ARMED_STATE, [] {
        auto current_time_ms = millis();
        auto delta_ms = current_time_ms - state_data.state_change_time;
        return check_button(KICKSTAND_PIN) && (delta_ms >= 120000); // 120 seconds = 2 min
    });

    // if kickstand goes up while button is pressed and alarm is on, go to waiting for kickstand down state.
    tsm.add_transition(WAIT_FOR_KICKSTAND_UP_STATE, WAIT_FOR_KICKSTAND_DOWN_STATE, [] {
        return !check_button(KICKSTAND_PIN);
    });

    // if button is released while alarm is on and kickstand is still down, go back to alarm state.
    tsm.add_transition(WAIT_FOR_KICKSTAND_UP_STATE, ALARM_TRIGGERED_STATE, [] {
        return !check_button(BUTTON_PIN);
    });


    tsm.startup();
}

void loop() {
    tsm.loop();
}


bool check_button(uint8_t pin) {

    /*
     * We use input pullup on pins. That means when there is a connection (closure on the pin), we read 0, and when
     * there is no connection (pin is open), we read 1.
     * Buttons are configured to be normally open, and when pressed (kickstand down, button pressed), complete the circuit.
     * That means they read 0 when open and 1 when closed. Therefore, the actual pin value is the opposite of what we get
     * from digitalRead()
     *
     * To make sure we're not reading floating values. The switches are always assumed to be open (button not pressed / kickstand up).
     *
     * To make sure we're not reading floating values, we check multiple times. Since we use input pullup, when the switch
     * is open we don't need to check multiple times, only when the switch is closed (otherwise, might get weird values).
     */

    const int num_checks = 20;
    int total_pressed = 0;

    for (int i = 0; i < num_checks; ++i) {
        total_pressed += !digitalRead(pin);
    }

    return total_pressed == num_checks;
}

void set_status_led(uint8_t r, uint8_t g, uint8_t b) {
    analogWrite(RED_PIN, r);
    analogWrite(GREEN_PIN, g);
    analogWrite(BLUE_PIN, b);
}
