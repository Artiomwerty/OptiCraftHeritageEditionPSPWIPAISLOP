#include "platform/Input.h"

#include "psp/input/PspInput.h"

PlatformTextInputSnapshot platformTextInputSnapshot(int port)
{
    (void)port;
    PlatformTextInputSnapshot s;
    s.connected = true;
    unsigned h = 0, pr = 0;
    PspInput::textSnapshot(h, pr);
    s.held = h;
    s.pressed = pr;
    return s;
}

PlatformGamepadSnapshot platformGamepadSnapshot(int port)
{
    (void)port;
    return {};
}

PlatformGamepadSnapshot platformRawGamepadSnapshot(int port)
{
    (void)port;
    return {};
}

int platformMenuPad()
{
    return 0;
}

bool platformMenuPointerActive()
{
    return true;
}

bool platformMenuCursorVisible()
{
    return true;
}

void platformSetMenuCursor(int x, int y)
{
    (void)x;
    (void)y;
}

const PlatformKeyboardHints& platformKeyboardHints()
{
    static const PlatformKeyboardHints hints = {
        {"X:type  []:del  /\\:shift  Sel:space  Start:ok  O:close", nullptr, nullptr}, 1};
    return hints;
}

const char* platformInputDebugLine()
{
    return "";
}
