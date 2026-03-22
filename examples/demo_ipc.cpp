#include "demo_tasks.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace demo {

void schedule_ipc_tasks(
    fiberuv::EventLoop &ev,
    std::pair<fiberuv::ipc::ProcessChannel, fiberuv::ipc::ProcessChannel> ipc_pair,
    const SharedMessageQueue &messages, fiberuv::fpromise<void> &ipc_done_promise) {
  ev.spawn([sender = std::move(ipc_pair.first), messages]() mutable {
    const IpcDemoPayload payload{7, 98.5};
    if (!sender.send(payload)) {
      messages->send("ipc sender closed");
      return;
    }
    const std::vector<std::uint8_t> payload_bytes = {
        static_cast<std::uint8_t>('i'), static_cast<std::uint8_t>('p'),
        static_cast<std::uint8_t>('c'), static_cast<std::uint8_t>('-'),
        static_cast<std::uint8_t>('b'), static_cast<std::uint8_t>('y'),
        static_cast<std::uint8_t>('t'), static_cast<std::uint8_t>('e'),
        static_cast<std::uint8_t>('s')};
    if (!sender.send_stream(payload_bytes)) {
      messages->send("ipc sender closed");
      return;
    }
    messages->send("ipc sender done");
  });

  ev.spawn([receiver = std::move(ipc_pair.second), messages, &ipc_done_promise]() mutable {
    try {
      auto payload = receiver.recv<IpcDemoPayload>();
      if (payload) {
        std::cout << "[ipc typed] id=" << payload->id << " score=" << payload->score << "\n";
        messages->send("ipc recv typed ok");
      }

      auto stream = receiver.recv_stream();
      if (stream) {
        std::string text(stream->begin(), stream->end());
        std::cout << "[ipc stream] " << text << "\n";
        messages->send("ipc recv stream ok");
      }
    } catch (const std::exception &ex) {
      std::cout << "[ipc err] " << ex.what() << "\n";
      messages->send("ipc recv failed");
    }
    ipc_done_promise.set_value();
  });
}

void schedule_consumer(fiberuv::EventLoop &ev, const SharedMessageQueue &messages,
                       fiberuv::fpromise<void> &consumer_done_promise) {
  ev.spawn([messages, &consumer_done_promise]() {
    while (true) {
      auto msg = messages->recv_for(std::chrono::seconds(5));
      if (!msg) {
        if (messages->closed()) {
          break;
        }
        continue;
      }
      std::cout << "[msg] " << *msg << "\n";
    }
    consumer_done_promise.set_value();
  });
}

} // namespace demo
