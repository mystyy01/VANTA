#include "keyboard.h"

// Circular buffer for key events
#define KEY_BUFFER_SIZE 64
static struct key_event key_buffer[KEY_BUFFER_SIZE];
static volatile int key_read_idx = 0;
static volatile int key_write_idx = 0;

// Modifier state
static volatile uint8_t mod_state = 0;
static volatile int extended = 0;

// Scancode tables
static const char scancode_lower[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '#', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // F1-F10
    0, 0,  // Num lock, Scroll lock
    0, 0, 0, '-',  // Home, Up, PgUp, -
    0, 0, 0, '+',  // Left, 5, Right, +
    0, 0, 0, 0, 0, // End, Down, PgDn, Ins, Del
    0, 0, 0,
    0, 0,  // F11, F12
};

static const char scancode_upper[128] = {
    0, 27, '!', '"', 0x9C, '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '@', '~',
    0, '~', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    0, 0, 0, '-',
    0, 0, 0, '+',
    0, 0, 0, 0, 0,
    0, 0, 0,
    0, 0,
};

// Scancodes
#define SC_LSHIFT     0x2A
#define SC_RSHIFT     0x36
#define SC_LCTRL      0x1D
#define SC_LALT       0x38
#define SC_EXTENDED   0xE0
#define SC_BACKSPACE  0x0E
#define SC_ENTER      0x1C
#define SC_TAB        0x0F
#define SC_SPACE      0x39
#define SC_ESCAPE     0x01

// Extended scancodes (after 0xE0)
#define SC_EXT_UP     0x48
#define SC_EXT_DOWN   0x50
#define SC_EXT_LEFT   0x4B
#define SC_EXT_RIGHT  0x4D
#define SC_EXT_HOME   0x47
#define SC_EXT_END    0x4F
#define SC_EXT_PGUP   0x49
#define SC_EXT_PGDN   0x51
#define SC_EXT_INSERT 0x52
#define SC_EXT_DELETE 0x53
#define SC_EXT_RCTRL  0x1D
#define SC_EXT_RALT   0x38

void keyboard_init(void) {
    key_read_idx = 0;
    key_write_idx = 0;
    mod_state = 0;
    extended = 0;
}

// Called by IRQ handler in isr.c
void keyboard_handle_scancode(uint8_t scancode) {
    // Handle extended prefix
    if (scancode == SC_EXTENDED) {
        extended = 1;
        return;
    }

    int released = scancode & 0x80;
    uint8_t code = scancode & 0x7F;

    struct key_event event;
    event.scancode = scancode;
    event.pressed = !released;
    event.modifiers = mod_state;
    event.key = 0;

    // Handle modifier keys
    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        if (released) mod_state &= ~MOD_SHIFT;
        else mod_state |= MOD_SHIFT;
        extended = 0;
        return;  // Don't queue modifier-only events
    }
    if (code == SC_LCTRL || (extended && code == SC_EXT_RCTRL)) {
        if (released) mod_state &= ~MOD_CTRL;
        else mod_state |= MOD_CTRL;
        extended = 0;
        return;
    }
    if (code == SC_LALT || (extended && code == SC_EXT_RALT)) {
        if (released) mod_state &= ~MOD_ALT;
        else mod_state |= MOD_ALT;
        extended = 0;
        return;
    }

    // Only queue key presses, not releases (for now)
    if (released) {
        extended = 0;
        return;
    }

    // Handle extended keys
    if (extended) {
        extended = 0;
        switch (code) {
            case SC_EXT_UP:     event.key = KEY_UP; break;
            case SC_EXT_DOWN:   event.key = KEY_DOWN; break;
            case SC_EXT_LEFT:   event.key = KEY_LEFT; break;
            case SC_EXT_RIGHT:  event.key = KEY_RIGHT; break;
            case SC_EXT_HOME:   event.key = KEY_HOME; break;
            case SC_EXT_END:    event.key = KEY_END; break;
            case SC_EXT_PGUP:   event.key = KEY_PGUP; break;
            case SC_EXT_PGDN:   event.key = KEY_PGDN; break;
            case SC_EXT_INSERT: event.key = KEY_INSERT; break;
            case SC_EXT_DELETE: event.key = KEY_DELETE; break;
            default: return;  // Unknown extended key
        }
    } else {
        // Regular key - get ASCII
        if (mod_state & MOD_SHIFT) {
            event.key = scancode_upper[code];
        } else {
            event.key = scancode_lower[code];
        }

        // ISO 102nd key (UK layout: \ and | next to left Shift)
        if (code == 0x56) {
            event.key = (mod_state & MOD_SHIFT) ? '|' : '\\';
        }

        // Handle special cases
        if (event.key == 0) {
            // Check for function keys
            if (code >= 0x3B && code <= 0x44) {
                event.key = KEY_F1 + (code - 0x3B);
            } else if (code == 0x57) {
                event.key = KEY_F11;
            } else if (code == 0x58) {
                event.key = KEY_F12;
            } else {
                return;  // Unknown key
            }
        }
    }

    // Add to buffer (if not full)
    int next_write = (key_write_idx + 1) % KEY_BUFFER_SIZE;
    if (next_write != key_read_idx) {
        key_buffer[key_write_idx] = event;
        key_write_idx = next_write;
    }
}

int keyboard_has_event(void) {
    return key_read_idx != key_write_idx;
}

struct key_event keyboard_get_event(void) {
    // Wait for event
    while (!keyboard_has_event()) {
        __asm__ volatile ("hlt");
    }

    struct key_event event = key_buffer[key_read_idx];
    key_read_idx = (key_read_idx + 1) % KEY_BUFFER_SIZE;
    return event;
}

int keyboard_poll_event(struct key_event *event) {
    if (!keyboard_has_event()) {
        return 0;
    }

    *event = key_buffer[key_read_idx];
    key_read_idx = (key_read_idx + 1) % KEY_BUFFER_SIZE;
    return 1;
}

char keyboard_getchar(uint8_t *modifiers) {
    while (1) {
        struct key_event event = keyboard_get_event();

        if (modifiers) {
            *modifiers = event.modifiers;
        }

        // Return printable characters
        if (event.key >= 0x20 && event.key < 0x7F) {
            return event.key;
        }

        // Also return common control chars
        if (event.key == '\n' || event.key == '\b' || event.key == '\t' || event.key == 27) {
            return event.key;
        }

        // For special keys, return 0 but set modifiers so caller knows something happened
        if (event.key >= 0x80) {
            return 0;
        }
    }
}
