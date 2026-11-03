#pragma once
#include <cstdint>
#include <filesystem>

#include "core/input.h"

// Test runs driven without a person: presses from a script or a file, frames saved on request. Off unless
// XBE_PAD_SCRIPT or XBE_TEST_DIR is set.
//
// XBE_PAD_SCRIPT file: lines of "frame BUTTONS [frames held]", frames counted in presents, held 8 by default, #
// comments. XBE_TEST_DIR folder, read each frame: a file "cmd" holding "BUTTONS [frames held]" is pressed once from
// the frame it is read (3 by default) and removed; a file "snap" saves the next presented frame as snap_<n>.bmp in
// the folder (Direct3D 9) and is removed. BUTTONS joined by +: UP DOWN LEFT RIGHT START BACK A B X Y BLACK WHITE LT
// RT L3 R3, LS LSD LSL LSR (the left stick up, down, left, right), RS RSD RSL RSR, KEY:<name or #vk> (a key, for
// the game's own key reads through testdrive::KeyHeld).
//
// XBE_LOCKSTEP set: one vertical blank for each present, and the guest's clocks move 1/60 s a present, so a slow
// renderer reaches the same game frames as a fast one. A load that presents nothing gets the real clock after 2 s.
namespace testdrive {

bool On();
bool Lockstep();
uint64_t ClockNs();                 // the guest's time since boot, kept by presents under XBE_LOCKSTEP
void Pad(int port, input::Pad& p);  // input::Poll, before GameInfo::pad: controller 1 gets the scripted presses
bool KeyHeld(int vk);               // a scripted key, for code that reads the keyboard itself
void Presented();                   // each present: counts frames, reads the folder
bool SnapDue();                     // a frame is to be saved: the renderer then calls Snap
void SnapAs(const char* name);      // the next presented frame goes to <name>.bmp in XBE_TEST_DIR
// top-down rows of 32 bit BGRA (or BGRX), written as a .bmp in XBE_TEST_DIR
void Snap(uint32_t width, uint32_t height, const uint8_t* rows, uint32_t pitch);
uint64_t Frames();

}  // namespace testdrive
