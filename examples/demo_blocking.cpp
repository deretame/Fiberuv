#include "demo_tasks.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace demo {

void schedule_blocking_tasks(fiberuv::EventLoop &ev, const SharedMessageQueue &messages) {
  ev.spawn([messages]() {
    int value = Fiberuv::loop().run_blocking([]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      return 21 * 2;
    });
    std::cout << "[blocking] result=" << value << "\n";
    messages->send("blocking task completed");
  });

  // Cross-thread send: run in EventLoop worker thread.
  ev.spawn(
      [messages]() { Fiberuv::loop().run_blocking([messages]() { messages->send("from run_blocking worker thread"); }); });

  // Cross-thread send: run in a standalone std::thread.
  ev.spawn([messages]() {
    std::thread producer([messages]() { messages->send("from std::thread producer"); });
    producer.join();
  });
}

} // namespace demo

