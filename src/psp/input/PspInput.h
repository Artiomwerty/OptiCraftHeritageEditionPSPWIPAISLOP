#pragma once
// PspInput — turns the PSP pad into the key/mouse events the engine already understands.
namespace PspInput
{
void init();                 // sceCtrl setup + HOME-button exit callback
void poll(bool inMenu);      // once per frame; pushes lwjgl Keyboard/Mouse events
bool exitRequested();
// Pad state in PLATFORM_TEXT_* bits, for the on-screen keyboard.
void textSnapshot(unsigned &held, unsigned &pressed);
void warpCursor(int x, int y); // move the menu cursor (screen pixels, top-left origin)
// True while a menu is open; x/y are the cursor position in screen pixels (top-left origin).
bool menuCursor(int &x, int &y);        // HOME menu "Exit game" chosen
}
