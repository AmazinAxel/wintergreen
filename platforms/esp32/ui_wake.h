#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Lets input end the UI loop's frame wait early.
//
// The loop sleeps out a 25 ms frame (Esp32Runtime::wait_next_frame), so a press
// landing just after a frame began sat for up to the whole frame before the
// screen even looked at it — ~12 ms on average, on every press, ahead of the
// draw and the waveform. Input sources give this semaphore on an edge and the
// wait returns at once. It costs nothing when nothing is pressed: the loop
// still wakes at its usual cadence, no more often.
//
// A binary semaphore rather than a task notification: IDF drivers the UI task
// calls into may block on its notification slot, and a stray give there would
// wake them, not us.
inline SemaphoreHandle_t g_ui_wake = nullptr;

inline void ui_wake_init() {
  if (!g_ui_wake)
    g_ui_wake = xSemaphoreCreateBinary();
}

inline void ui_wake() {
  if (g_ui_wake)
    xSemaphoreGive(g_ui_wake);
}
