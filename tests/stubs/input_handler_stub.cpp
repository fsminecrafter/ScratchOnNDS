// tests/stubs/input_handler_stub.cpp

#include <string>

class InputHandler {
public:
    static int getTouchX() {
        return 0;
    }

    static int getTouchY() {
        return 0;
    }

    static bool isTouching() {
        return false;
    }

    static int getMicLoudness() {
        return 0;
    }

    static bool isKeyDown(const std::string&) {
        return false;
    }

    static bool isButtonDown(const std::string&) {
        return false;
    }

    static bool isButtonHeld(const std::string&) {
        return false;
    }

    static bool isButtonReleased(const std::string&) {
        return false;
    }
};
