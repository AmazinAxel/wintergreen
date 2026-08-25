#include "Loop.h"

namespace wintergreen {

void run_loop_iteration(Application& app, DrawBuffer& buf, IInputSource& input, IRuntime& runtime) {
  const ButtonState buttons = input.poll_buttons();
  app.update(buttons, runtime.frame_time_ms(), buf, runtime);
  runtime.wait_next_frame();
}

}  // namespace wintergreen
