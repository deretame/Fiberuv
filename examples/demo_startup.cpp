#include "demo_tasks.hpp"

#include <chrono>
#include <functional>

namespace demo {

void run_startup_scope(fiberuv::EventLoop &ev, const SharedMessageQueue &messages) {
  (void)ev;
  messages->send("scope startup task 1");
  messages->send("scope startup task 2");
}

fiberuv::PeriodicTask start_periodic(const SharedMessageQueue &messages) {
  std::function<void()> tick = [messages]() { messages->send("periodic tick"); };
  return fiberuv::every(std::chrono::milliseconds(100), std::move(tick));
}

} // namespace demo
