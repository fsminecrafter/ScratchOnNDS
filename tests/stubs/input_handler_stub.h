// tests/stubs/input_handler_stub.h

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

bool input_is_key_pressed(int key);
bool input_is_key_held(int key);

int input_get_touch_x();
int input_get_touch_y();

void input_update();

#ifdef __cplusplus
}
#endif
