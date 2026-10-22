#pragma once
#include <windows.h>
#include <string>

// Only scheme 2 is supported: Compatible SDK / negotiated 4M.
bool makcu_select_scheme(int scheme);
int makcu_get_scheme();
std::string makcu_get_info_json();
std::string makcu_get_schemes_json();

#define MAKCU_BTN_LEFT      0x01
#define MAKCU_BTN_RIGHT     0x02
#define MAKCU_BTN_MIDDLE    0x04
#define MAKCU_BTN_SIDE1     0x08
#define MAKCU_BTN_SIDE2     0x10

#define MAKCU_MOD_CTRL      0x01
#define MAKCU_MOD_SHIFT     0x02
#define MAKCU_MOD_ALT       0x04
#define MAKCU_MOD_GUI       0x08

bool makcu_init(const char* portName, int baudRate = 115200);
void makcu_close();
bool makcu_is_connected();

bool makcu_autodetect(char* outPort, int* outBaud);

int  makcu_button_pressed(unsigned char btn);
bool makcu_is_left_down();
bool makcu_is_right_down();
bool makcu_is_middle_down();
bool makcu_is_side1_down();
bool makcu_is_side2_down();

// True means the entire serial command was written, not that USB motion occurred.
bool makcu_mouse_move(int dx, int dy);
void makcu_mouse_button(unsigned char button, int state);
void makcu_mouse_left(int state);
void makcu_mouse_right(int state);
void makcu_mouse_middle(int state);
void makcu_mouse_wheel(char wheel);
void makcu_mouse_all(unsigned char buttons, short dx, short dy, char wheel);

void makcu_key_down(unsigned char vk_key);
void makcu_key_up(unsigned char vk_key);
void makcu_modifier_down(unsigned char modifier);
void makcu_modifier_up(unsigned char modifier);
void makcu_release_all();

const char* makcu_get_last_error();
