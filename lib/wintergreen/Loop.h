#pragma once

#include "wintergreen/Application.h"
#include "wintergreen/Input.h"
#include "wintergreen/Runtime.h"
#include "wintergreen/display/DrawBuffer.h"

namespace wintergreen {

void run_loop(Application& app, DrawBuffer& buf, IInputSource& input, IRuntime& runtime);
void run_loop_iteration(Application& app, DrawBuffer& buf, IInputSource& input, IRuntime& runtime);

}  // namespace wintergreen
