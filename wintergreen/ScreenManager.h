#pragma once

#include <cstdio>

#include "display/DrawBuffer.h"
#include "screens/IScreen.h"

namespace wintergreen {

// Stack-based screen manager. Screens are pushed/popped; only the top screen
// is active (receives start/stop and update calls)

class ScreenManager {
 public:
  static constexpr int kMaxDepth = 8;

  // Push a new screen. Pauses the current top, starts the new one.
  // The new screen draws into buf; caller handles the actual refresh.
  void push(IScreen* screen, DrawBuffer& buf, IRuntime& runtime) {
    if (!screen || depth_ >= kMaxDepth)
      return;
    if (depth_ > 0)
      stack_[depth_ - 1]->pause();
    stack_[depth_++] = screen;
    screen->start(buf, runtime);
  }

  // Swap the top screen for another, leaving the rest of the stack untouched.
  //
  // Not pop()+push(): pop() resumes whatever is underneath, which for
  // MainMenu -> Reader means HomeScreen rebuilding its carousel, reloading the
  // book index and decoding a cover — all of it thrown away one line later when
  // the push pauses it again. It also avoids pausing a screen that was never
  // resumed.
  void replace(IScreen* screen, DrawBuffer& buf, IRuntime& runtime) {
    if (!screen)
      return;
    if (depth_ == 0) {
      push(screen, buf, runtime);
      return;
    }
    stack_[depth_ - 1]->stop();
    stack_[depth_ - 1] = screen;
    screen->start(buf, runtime);
  }

  // Pop the top screen(s). Stops all removed screens, then resumes the new top.
  void pop(int count, DrawBuffer& buf, IRuntime& runtime) {
    if (count <= 0 || depth_ == 0)
      return;
    // Never pop the last screen: an empty stack means top() is null and the app
    // silently stops processing input, which looks exactly like a freeze.
    if (count > depth_ - 1)
      count = depth_ - 1;
    if (count == 0)
      return;

    // Stop all screens being removed from the stack.
    for (int i = depth_ - 1; i >= depth_ - count; --i)
      stack_[i]->stop();
    depth_ -= count;
    if (depth_ > 0) {
      stack_[depth_ - 1]->resume(buf, runtime);
    }
  }

  void pop(DrawBuffer& buf, IRuntime& runtime) {
    pop(1, buf, runtime);
  }

  IScreen* top() const {
    return depth_ > 0 ? stack_[depth_ - 1] : nullptr;
  }
  bool contains(const IScreen* screen) const {
    for (int i = 0; i < depth_; ++i)
      if (stack_[i] == screen)
        return true;
    return false;
  }
  int depth() const {
    return depth_;
  }
  bool empty() const {
    return depth_ == 0;
  }

 private:
  IScreen* stack_[kMaxDepth] = {};
  int depth_ = 0;
};

}  // namespace wintergreen
