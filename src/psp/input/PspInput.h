#pragma once
// PspInput — turns the PSP pad into the key/mouse events the engine already understands.
namespace PspInput
{
void init();                 // sceCtrl setup + HOME-button exit callback
void poll(bool inMenu);      // once per frame; pushes lwjgl Keyboard/Mouse events
bool exitRequested();        // HOME menu "Exit game" chosen
}
