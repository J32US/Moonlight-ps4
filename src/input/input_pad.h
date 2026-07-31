#pragma once

#include <stdbool.h>

// Initialize scePad and announce the controller to Moonlight.
int input_init(void);
void input_shutdown(void);

// Read the pad and send state. Returns true if the user requests quit
// (OPTIONS + TOUCHPAD held ~1 s, or the external flag).
bool input_poll(void);

// Request quit from another thread (connectionTerminated, etc.).
void input_request_quit(void);
bool input_should_quit(void);

// Reset quit/combo/controller announcement (before entering menu or stream).
void input_reset(void);

/* Mark current pad state as "already seen" (no edges). Call when
 * entering the menu after OPTIONS+TOUCHPAD to avoid instant re-launch. */
void input_menu_absorb(void);

/* Wait until launch buttons are released (OPTIONS/X/O). timeout_ms. */
void input_menu_wait_release(unsigned mask, int timeout_ms);

// --- Menu mode (UI): read pad without sending anything to Moonlight ---
#define MENU_BTN_UP       0x0001u
#define MENU_BTN_DOWN     0x0002u
#define MENU_BTN_LEFT     0x0004u
#define MENU_BTN_RIGHT    0x0008u
#define MENU_BTN_CROSS    0x0010u
#define MENU_BTN_CIRCLE   0x0020u
#define MENU_BTN_SQUARE   0x0040u
#define MENU_BTN_TRIANGLE 0x0080u
#define MENU_BTN_L1       0x0100u
#define MENU_BTN_R1       0x0200u
#define MENU_BTN_OPTIONS  0x0400u

// pressed: edges this frame (with D-pad auto-repeat); held: state.
// Returns 0 if pad was read, <0 if no controller.
int input_menu_poll(unsigned *pressed, unsigned *held);
