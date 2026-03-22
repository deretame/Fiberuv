#include "demo_tasks.hpp"

#include <memory>

int main() {
  fiberuv::run([]() {
    auto &ev = Fiberuv::loop();
    fiberuv::io::Socket tcp_server = fiberuv::io::Socket::tcp(ev);
    fiberuv::io::Socket udp_server = fiberuv::io::Socket::udp(ev);
    auto messages = std::make_shared<demo::MessageQueue>(ev);
    auto ipc_pair = fiberuv::ipc::ProcessChannel::pair(ev);

    fiberuv::fpromise<void> done_promise;
    auto done = done_promise.get_future();
    fiberuv::fpromise<void> consumer_done_promise;
    auto consumer_done = consumer_done_promise.get_future();
    fiberuv::fpromise<void> tcp_server_done_promise;
    auto tcp_server_done = tcp_server_done_promise.get_future();
    fiberuv::fpromise<void> ipc_done_promise;
    auto ipc_done = ipc_done_promise.get_future();

    demo::run_startup_scope(ev, messages);
    auto periodic = demo::start_periodic(messages);
    demo::schedule_file_ops(ev, messages);
    demo::schedule_blocking_tasks(ev, messages);
    demo::schedule_tcp_server(ev, tcp_server, messages, tcp_server_done_promise);
    demo::schedule_udp_server(ev, udp_server, messages);
    demo::schedule_ipc_tasks(ev, std::move(ipc_pair), messages, ipc_done_promise);
    demo::schedule_consumer(ev, messages, consumer_done_promise);
    demo::schedule_client_task(ev, messages, done_promise);

    done.get();
    tcp_server_done.get();
    ipc_done.get();
    periodic.stop();
    messages->close();
    consumer_done.get();
    udp_server.close();
    tcp_server.close();
    Fiberuv::stop_loop();
  });
  return 0;
}

