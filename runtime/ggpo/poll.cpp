#include "types.h"
#include "poll.h"

bool
Poll::Pump(int timeout)
{
   if (timeout > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
   }
   bool finished = false;
   for (int i = 0; i < _loop_sinks.size(); i++) {
      PollSinkCb &cb = _loop_sinks[i];
      finished = !cb.sink->OnLoopPoll(cb.cookie) || finished;
   }
   return finished;
}
