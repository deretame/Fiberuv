#pragma once

#include "fiberuv/fiberuv.hpp"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace demo {

using MessageQueue = fiberuv::MessageQueue<std::string>;
using SharedMessageQueue = std::shared_ptr<MessageQueue>;

struct IpcDemoPayload {
  int id{0};
  double score{0};
};

static_assert(std::is_trivially_copyable_v<IpcDemoPayload>);

void run_startup_scope(fiberuv::EventLoop &ev, const SharedMessageQueue &messages);
fiberuv::PeriodicTask start_periodic(const SharedMessageQueue &messages);
void schedule_file_ops(fiberuv::EventLoop &ev, const SharedMessageQueue &messages);
void schedule_blocking_tasks(fiberuv::EventLoop &ev, const SharedMessageQueue &messages);

void schedule_tcp_server(fiberuv::EventLoop &ev, fiberuv::io::Socket &tcp_server,
                         const SharedMessageQueue &messages,
                         fiberuv::fpromise<void> &tcp_server_done_promise);
void schedule_udp_server(fiberuv::EventLoop &ev, fiberuv::io::Socket &udp_server,
                         const SharedMessageQueue &messages);
void schedule_client_task(fiberuv::EventLoop &ev, const SharedMessageQueue &messages,
                          fiberuv::fpromise<void> &done_promise);

void schedule_ipc_tasks(
    fiberuv::EventLoop &ev,
    std::pair<fiberuv::ipc::ProcessChannel, fiberuv::ipc::ProcessChannel> ipc_pair,
    const SharedMessageQueue &messages, fiberuv::fpromise<void> &ipc_done_promise);
void schedule_consumer(fiberuv::EventLoop &ev, const SharedMessageQueue &messages,
                       fiberuv::fpromise<void> &consumer_done_promise);

} // namespace demo

template <> struct fiberuv::IpcDeepCopyable<demo::IpcDemoPayload, void> : std::true_type {};

