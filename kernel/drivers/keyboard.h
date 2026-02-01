#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

// Special key codes (values > 127 so they don't conflict with ASCII)
#define KEY_UP        0x80
#define KEY_DOWN      0x81
#define KEY_LEFT      0x82
#define KEY_RIGHT     0x83
#define KEY_HOME      0x84
#define KEY_END       0x85
#define KEY_PGUP      0x86
#define KEY_PGDN      0x87
#define KEY_DELETE    0x88
#define KEY_INSERT    0x89
#define KEY_F1        0x8A
#define KEY_F2        0x8B
#define KEY_F3        0x8C
#define KEY_F4        0x8D
#define KEY_F5        0x8E
#define KEY_F6        0x8F
#define KEY_F7        0x90
#define KEY_F8        0x91
#define KEY_F9        0x92
#define KEY_F10       0x93
#define KEY_F11       0x94
#define KEY_F12       0x95

// Modifier flags
#define MOD_SHIFT     0x01
#define MOD_CTRL      0x02
#define MOD_ALT       0x04

// Key event structure
struct key_event {
    uint8_t key;        // ASCII char or special key code
    uint8_t modifiers;  // MOD_SHIFT, MOD_CTRL, MOD_ALT
    uint8_t pressed;    // 1 = pressed, 0 = released
    uint8_t scancode;   // Raw scancode
};

// Initialize keyboard (called by IDT init)
void keyboard_init(void);

// Handle raw scancode (called by IRQ handler)
void keyboard_handle_scancode(uint8_t scancode);

// Check if a key event is available
int keyboard_has_event(void);

// Get next key event (blocks if none available)
struct key_event keyboard_get_event(void);

// Get next key event (returns 0 if none available)
int keyboard_poll_event(struct key_event *event);

// Convenience: get next printable character (blocks)
// Returns the ASCII char, or 0 for non-printable keys
// Fills modifiers if not NULL
char keyboard_getchar(uint8_t *modifiers);

#endif
