// tests/stubs/input_handler_stub.cpp

#include <cstdint>

extern "C" {

bool input_is_key_pressed(int) {
    return false;
}

bool input_is_key_held(int) {
    return false;
}

int input_get_touch_x() {
    return 0;
}

int input_get_touch_y() {
    return 0;
}

void input_update() {
}

}
