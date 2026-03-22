#include "fiberuv/fiberuv.hpp"

#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace fiberuv::io {

namespace {

std::runtime_error uv_error(std::string_view where, int code) {
  return std::runtime_error(std::string(where) + ": " + uv_strerror(code));
}

void ensure_loop_alive(const loop_lifetime_token &token) {
  if (!is_loop_alive(token)) {
    throw std::runtime_error("event loop is not alive");
  }
}

template <typename Fn> auto invoke_on_loop(EventLoop &loop, Fn &&fn) -> decltype(fn()) {
  using Result = decltype(fn());
  if (try_get_current_loop() == &loop) {
    if constexpr (std::is_void_v<Result>) {
      fn();
      return;
    } else {
      return fn();
    }
  }

  auto promise = std::make_shared<std::promise<Result>>();
  auto future = promise->get_future();

  dispatch_to_loop(loop, [promise, fn = std::forward<Fn>(fn)]() mutable {
    try {
      if constexpr (std::is_void_v<Result>) {
        fn();
        promise->set_value();
      } else {
        promise->set_value(fn());
      }
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  });

  if constexpr (std::is_void_v<Result>) {
    future.get();
    return;
  } else {
    return future.get();
  }
}

struct FsOpenState {
  std::shared_ptr<fpromise<int>> promise;
  uv_fs_t req{};
};

void on_fs_open(uv_fs_t *req) {
  std::unique_ptr<FsOpenState> state(static_cast<FsOpenState *>(req->data));
  const int result = static_cast<int>(req->result);
  uv_fs_req_cleanup(req);
  if (result < 0) {
    state->promise->set_exception(std::make_exception_ptr(uv_error("uv_fs_open", result)));
    return;
  }
  state->promise->set_value(result);
}

int fs_open(EventLoop &loop, const std::string &path, int flags, int mode) {
  auto promise = std::make_shared<fpromise<int>>();
  auto future = promise->get_future();
  auto *state = new FsOpenState();
  state->promise = promise;
  state->req.data = state;

  const int rc = uv_fs_open(loop.native_loop(), &state->req, path.c_str(), flags, mode, on_fs_open);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_open(start)", rc);
  }

  return future.get();
}

struct FsCloseState {
  std::shared_ptr<fpromise<void>> promise;
  uv_fs_t req{};
};

void on_fs_close(uv_fs_t *req) {
  std::unique_ptr<FsCloseState> state(static_cast<FsCloseState *>(req->data));
  const int result = static_cast<int>(req->result);
  uv_fs_req_cleanup(req);
  if (result < 0) {
    state->promise->set_exception(std::make_exception_ptr(uv_error("uv_fs_close", result)));
    return;
  }
  state->promise->set_value();
}

void fs_close(EventLoop &loop, uv_file file) {
  auto promise = std::make_shared<fpromise<void>>();
  auto future = promise->get_future();
  auto *state = new FsCloseState();
  state->promise = promise;
  state->req.data = state;

  const int rc = uv_fs_close(loop.native_loop(), &state->req, file, on_fs_close);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_close(start)", rc);
  }

  future.get();
}

struct FsReadState {
  EventLoop *loop{nullptr};
  std::shared_ptr<fpromise<std::string>> promise;
  uv_fs_t req{};
  uv_file file{-1};
  std::uint64_t offset{0};
  std::vector<char> buffer;
};

void on_fs_read(uv_fs_t *req) {
  std::unique_ptr<FsReadState> state(static_cast<FsReadState *>(req->data));
  const ssize_t result = req->result;
  uv_fs_req_cleanup(req);

  if (result < 0) {
    state->promise->set_exception(
        std::make_exception_ptr(uv_error("uv_fs_read", static_cast<int>(result))));
    return;
  }

  state->promise->set_value(std::string(state->buffer.data(), static_cast<std::size_t>(result)));
}

std::string fs_read_at(EventLoop &loop, uv_file file, std::size_t size, std::uint64_t offset) {
  auto promise = std::make_shared<fpromise<std::string>>();
  auto future = promise->get_future();
  auto *state = new FsReadState();
  state->loop = &loop;
  state->promise = promise;
  state->file = file;
  state->offset = offset;
  state->buffer.resize(size);
  state->req.data = state;

  uv_buf_t buf = uv_buf_init(state->buffer.data(), static_cast<unsigned int>(state->buffer.size()));
  const int rc = uv_fs_read(loop.native_loop(), &state->req, file, &buf, 1,
                            static_cast<int64_t>(offset), on_fs_read);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_read(start)", rc);
  }

  return future.get();
}

struct FsWriteState {
  EventLoop *loop{nullptr};
  std::shared_ptr<fpromise<std::size_t>> promise;
  uv_fs_t req{};
  uv_file file{-1};
  std::uint64_t base_offset{0};
  std::string data;
  std::size_t written{0};
};

void on_fs_write(uv_fs_t *req) {
  auto *state = static_cast<FsWriteState *>(req->data);
  const ssize_t result = req->result;
  uv_fs_req_cleanup(req);

  if (result < 0) {
    std::unique_ptr<FsWriteState> cleanup(state);
    cleanup->promise->set_exception(
        std::make_exception_ptr(uv_error("uv_fs_write", static_cast<int>(result))));
    return;
  }

  state->written += static_cast<std::size_t>(result);
  if (state->written >= state->data.size()) {
    std::unique_ptr<FsWriteState> cleanup(state);
    cleanup->promise->set_value(cleanup->written);
    return;
  }

  char *start = state->data.data() + state->written;
  const std::size_t remaining = state->data.size() - state->written;
  uv_buf_t buf = uv_buf_init(start, static_cast<unsigned int>(remaining));
  const int rc =
      uv_fs_write(state->loop->native_loop(), &state->req, state->file, &buf, 1,
                  static_cast<int64_t>(state->base_offset + state->written), on_fs_write);
  if (rc < 0) {
    std::unique_ptr<FsWriteState> cleanup(state);
    cleanup->promise->set_exception(std::make_exception_ptr(uv_error("uv_fs_write(start)", rc)));
  }
}

std::size_t fs_write_at(EventLoop &loop, uv_file file, std::string_view data,
                        std::uint64_t offset) {
  if (data.empty()) {
    return 0;
  }

  auto promise = std::make_shared<fpromise<std::size_t>>();
  auto future = promise->get_future();
  auto *state = new FsWriteState();
  state->loop = &loop;
  state->promise = promise;
  state->file = file;
  state->base_offset = offset;
  state->data = std::string(data);
  state->req.data = state;

  uv_buf_t buf = uv_buf_init(state->data.data(), static_cast<unsigned int>(state->data.size()));
  const int rc = uv_fs_write(loop.native_loop(), &state->req, file, &buf, 1,
                             static_cast<int64_t>(offset), on_fs_write);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_write(start)", rc);
  }

  return future.get();
}

struct FsSimpleState {
  std::shared_ptr<fpromise<int>> promise;
  uv_fs_t req{};
};

void on_fs_simple(uv_fs_t *req) {
  std::unique_ptr<FsSimpleState> state(static_cast<FsSimpleState *>(req->data));
  const int result = static_cast<int>(req->result);
  uv_fs_req_cleanup(req);
  state->promise->set_value(result);
}

int fs_mkdir_raw(EventLoop &loop, const std::string &path, int mode) {
  auto promise = std::make_shared<fpromise<int>>();
  auto future = promise->get_future();
  auto *state = new FsSimpleState();
  state->promise = promise;
  state->req.data = state;

  const int rc = uv_fs_mkdir(loop.native_loop(), &state->req, path.c_str(), mode, on_fs_simple);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_mkdir(start)", rc);
  }
  return future.get();
}

int fs_rmdir_raw(EventLoop &loop, const std::string &path) {
  auto promise = std::make_shared<fpromise<int>>();
  auto future = promise->get_future();
  auto *state = new FsSimpleState();
  state->promise = promise;
  state->req.data = state;

  const int rc = uv_fs_rmdir(loop.native_loop(), &state->req, path.c_str(), on_fs_simple);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_rmdir(start)", rc);
  }
  return future.get();
}

int fs_unlink_raw(EventLoop &loop, const std::string &path) {
  auto promise = std::make_shared<fpromise<int>>();
  auto future = promise->get_future();
  auto *state = new FsSimpleState();
  state->promise = promise;
  state->req.data = state;

  const int rc = uv_fs_unlink(loop.native_loop(), &state->req, path.c_str(), on_fs_simple);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_unlink(start)", rc);
  }
  return future.get();
}

struct PollWaitState {
  std::shared_ptr<fpromise<void>> promise;
  uv_poll_t poll{};
  bool completed{false};
};

struct PollWaitForState {
  std::shared_ptr<fpromise<bool>> promise;
  uv_poll_t poll{};
  uv_timer_t timer{};
  bool completed{false};
  int pending_close{0};
};

void on_poll_closed(uv_handle_t *handle) {
  auto *state = static_cast<PollWaitState *>(handle->data);
  delete state;
}

void on_poll_wait_for_closed(uv_handle_t *handle) {
  auto *state = static_cast<PollWaitForState *>(handle->data);
  if (state->pending_close > 0) {
    --state->pending_close;
  }
  if (state->pending_close == 0) {
    delete state;
  }
}

void close_poll_wait_for_handles(PollWaitForState *state) {
  state->pending_close = 0;
  if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->poll))) {
    ++state->pending_close;
    uv_close(reinterpret_cast<uv_handle_t *>(&state->poll), on_poll_wait_for_closed);
  }
  if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->timer))) {
    ++state->pending_close;
    uv_close(reinterpret_cast<uv_handle_t *>(&state->timer), on_poll_wait_for_closed);
  }
  if (state->pending_close == 0) {
    delete state;
  }
}

void finish_poll_wait_for(PollWaitForState *state, std::optional<bool> value, std::exception_ptr ex) {
  if (state->completed) {
    return;
  }
  state->completed = true;

  if (ex) {
    state->promise->set_exception(ex);
  } else {
    state->promise->set_value(value.value_or(false));
  }

  uv_poll_stop(&state->poll);
  uv_timer_stop(&state->timer);
  close_poll_wait_for_handles(state);
}

void on_poll_event_for(uv_poll_t *poll, int status, int) {
  auto *state = static_cast<PollWaitForState *>(poll->data);
  if (status < 0) {
    finish_poll_wait_for(state, std::nullopt,
                         std::make_exception_ptr(uv_error("uv_poll_start", status)));
    return;
  }
  finish_poll_wait_for(state, true, nullptr);
}

void on_poll_timeout_for(uv_timer_t *timer) {
  auto *state = static_cast<PollWaitForState *>(timer->data);
  finish_poll_wait_for(state, false, nullptr);
}

void on_poll_event(uv_poll_t *poll, int status, int) {
  auto *state = static_cast<PollWaitState *>(poll->data);
  if (state->completed) {
    return;
  }
  state->completed = true;

  if (status < 0) {
    state->promise->set_exception(std::make_exception_ptr(uv_error("uv_poll_start", status)));
  } else {
    state->promise->set_value();
  }

  uv_poll_stop(poll);
  uv_close(reinterpret_cast<uv_handle_t *>(poll), on_poll_closed);
}

} // namespace

struct Socket::Impl {
  EventLoop *loop{nullptr};
  loop_lifetime_token loop_lifetime;
  SocketType type{SocketType::tcp};
  bool closed{false};
  bool listening{false};
  std::optional<Endpoint> local;
  std::optional<Endpoint> peer;
  std::optional<TcpServer> tcp_server;
  std::optional<TcpConnection> tcp_conn;
  std::optional<UdpSocket> udp;
};

Socket::Socket(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Socket Socket::tcp(EventLoop &loop) {
  auto impl = std::make_shared<Impl>();
  impl->loop = &loop;
  impl->loop_lifetime = loop.lifetime_token();
  impl->type = SocketType::tcp;
  return Socket(std::move(impl));
}

Socket Socket::udp(EventLoop &loop) {
  auto impl = std::make_shared<Impl>();
  impl->loop = &loop;
  impl->loop_lifetime = loop.lifetime_token();
  impl->type = SocketType::udp;
  impl->udp.emplace(loop);
  return Socket(std::move(impl));
}

void Socket::bind(const Endpoint &local) {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  invoke_on_loop(*impl_->loop, [impl = impl_, local]() {
    if (impl->closed) {
      throw std::runtime_error("socket is closed");
    }

    if (impl->type == SocketType::tcp) {
      impl->local = local;
      return;
    }

    if (!impl->udp) {
      impl->udp.emplace(*impl->loop);
    }
    impl->udp->bind(local.host, local.port);
    impl->local = local;
  });
}

void Socket::listen(int backlog) {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  invoke_on_loop(*impl_->loop, [impl = impl_, backlog]() {
    if (impl->type != SocketType::tcp) {
      throw std::runtime_error("listen is only valid for TCP sockets");
    }
    if (!impl->local) {
      throw std::runtime_error("bind() must be called before listen()");
    }
    if (!impl->tcp_server) {
      impl->tcp_server.emplace(*impl->loop);
    }
    impl->tcp_server->listen(impl->local->host, impl->local->port, backlog);
    impl->listening = true;
  });
}

Socket Socket::accept() {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  return invoke_on_loop(*impl_->loop, [impl = impl_]() {
    if (impl->type != SocketType::tcp || !impl->listening || !impl->tcp_server) {
      throw std::runtime_error("accept() requires a listening TCP socket");
    }

    auto conn = impl->tcp_server->accept();
    auto child = std::make_shared<Impl>();
    child->loop = impl->loop;
    child->loop_lifetime = impl->loop_lifetime;
    child->type = SocketType::tcp;
    child->tcp_conn = std::move(conn);
    return Socket(std::move(child));
  });
}

std::optional<Socket> Socket::accept_for(std::chrono::milliseconds timeout) {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);
  if (timeout.count() < 0) {
    timeout = std::chrono::milliseconds(0);
  }

  return invoke_on_loop(*impl_->loop, [impl = impl_, timeout]() -> std::optional<Socket> {
    if (impl->type != SocketType::tcp || !impl->listening || !impl->tcp_server) {
      throw std::runtime_error("accept_for() requires a listening TCP socket");
    }

    auto conn = impl->tcp_server->accept_for(timeout);
    if (!conn) {
      return std::nullopt;
    }

    auto child = std::make_shared<Impl>();
    child->loop = impl->loop;
    child->loop_lifetime = impl->loop_lifetime;
    child->type = SocketType::tcp;
    child->tcp_conn = std::move(*conn);
    return std::optional<Socket>(Socket(std::move(child)));
  });
}

void Socket::connect(const Endpoint &remote) {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  invoke_on_loop(*impl_->loop, [impl = impl_, remote]() {
    if (impl->closed) {
      throw std::runtime_error("socket is closed");
    }

    if (impl->type == SocketType::tcp) {
      TcpClient client(*impl->loop);
      auto conn = client.connect(remote.host, remote.port);
      impl->tcp_conn = std::move(conn);
      impl->peer = remote;
      return;
    }

    if (!impl->udp) {
      impl->udp.emplace(*impl->loop);
    }
    impl->peer = remote;
  });
}

bool Socket::connect_for(const Endpoint &remote, std::chrono::milliseconds timeout) {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);
  if (timeout.count() < 0) {
    timeout = std::chrono::milliseconds(0);
  }

  return invoke_on_loop(*impl_->loop, [impl = impl_, remote, timeout]() {
    if (impl->closed) {
      throw std::runtime_error("socket is closed");
    }

    if (impl->type == SocketType::tcp) {
      TcpClient client(*impl->loop);
      auto conn = client.connect_for(remote.host, remote.port, timeout);
      if (!conn) {
        return false;
      }
      impl->tcp_conn = std::move(*conn);
      impl->peer = remote;
      return true;
    }

    if (!impl->udp) {
      impl->udp.emplace(*impl->loop);
    }
    impl->peer = remote;
    return true;
  });
}

void Socket::send(std::string_view data) {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  invoke_on_loop(*impl_->loop, [impl = impl_, data]() {
    if (impl->type == SocketType::tcp) {
      if (!impl->tcp_conn) {
        throw std::runtime_error("TCP socket is not connected");
      }
      impl->tcp_conn->write(std::string(data));
      return;
    }

    if (!impl->udp || !impl->peer) {
      throw std::runtime_error("UDP socket is not connected");
    }
    impl->udp->send_to(impl->peer->host, impl->peer->port, std::string(data));
  });
}

std::string Socket::recv() {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  return invoke_on_loop(*impl_->loop, [impl = impl_]() -> std::string {
    if (impl->type == SocketType::tcp) {
      if (!impl->tcp_conn) {
        throw std::runtime_error("TCP socket is not connected");
      }
      return impl->tcp_conn->read();
    }

    if (!impl->udp) {
      throw std::runtime_error("UDP socket is not initialized");
    }
    auto packet = impl->udp->recv_from();
    impl->peer = Endpoint{packet.host, packet.port};
    return packet.data;
  });
}

std::optional<std::string> Socket::recv_for(std::chrono::milliseconds timeout) {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);
  if (timeout.count() < 0) {
    timeout = std::chrono::milliseconds(0);
  }

  return invoke_on_loop(*impl_->loop, [impl = impl_, timeout]() -> std::optional<std::string> {
    if (impl->type == SocketType::tcp) {
      if (!impl->tcp_conn) {
        throw std::runtime_error("TCP socket is not connected");
      }
      return impl->tcp_conn->read_for(timeout);
    }

    if (!impl->udp) {
      throw std::runtime_error("UDP socket is not initialized");
    }
    auto packet = impl->udp->recv_from_for(timeout);
    if (!packet) {
      return std::nullopt;
    }
    impl->peer = Endpoint{packet->host, packet->port};
    return std::optional<std::string>(std::move(packet->data));
  });
}

void Socket::send_to(const Endpoint &remote, std::string_view data) {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  invoke_on_loop(*impl_->loop, [impl = impl_, remote, data]() {
    if (impl->type != SocketType::udp) {
      throw std::runtime_error("send_to() is only valid for UDP sockets");
    }
    if (!impl->udp) {
      impl->udp.emplace(*impl->loop);
    }
    impl->udp->send_to(remote.host, remote.port, std::string(data));
  });
}

Datagram Socket::recv_from() {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  return invoke_on_loop(*impl_->loop, [impl = impl_]() -> Datagram {
    if (impl->type != SocketType::udp || !impl->udp) {
      throw std::runtime_error("recv_from() is only valid for UDP sockets");
    }
    auto packet = impl->udp->recv_from();
    return Datagram{std::move(packet.data), Endpoint{std::move(packet.host), packet.port}};
  });
}

std::optional<Datagram> Socket::recv_from_for(std::chrono::milliseconds timeout) {
  if (!impl_) {
    throw std::runtime_error("invalid socket");
  }
  ensure_loop_alive(impl_->loop_lifetime);
  if (timeout.count() < 0) {
    timeout = std::chrono::milliseconds(0);
  }

  return invoke_on_loop(*impl_->loop, [impl = impl_, timeout]() -> std::optional<Datagram> {
    if (impl->type != SocketType::udp || !impl->udp) {
      throw std::runtime_error("recv_from_for() is only valid for UDP sockets");
    }
    auto packet = impl->udp->recv_from_for(timeout);
    if (!packet) {
      return std::nullopt;
    }
    return std::optional<Datagram>(
        Datagram{std::move(packet->data), Endpoint{std::move(packet->host), packet->port}});
  });
}

void Socket::close() {
  if (!impl_) {
    return;
  }
  if (!is_loop_alive(impl_->loop_lifetime)) {
    impl_->closed = true;
    impl_->listening = false;
    impl_->tcp_conn.reset();
    impl_->tcp_server.reset();
    impl_->udp.reset();
    return;
  }

  invoke_on_loop(*impl_->loop, [impl = impl_]() {
    if (impl->closed) {
      return;
    }
    if (impl->tcp_conn) {
      impl->tcp_conn->close();
      impl->tcp_conn.reset();
    }
    if (impl->tcp_server) {
      impl->tcp_server->close();
      impl->tcp_server.reset();
    }
    if (impl->udp) {
      impl->udp->close();
      impl->udp.reset();
    }
    impl->closed = true;
    impl->listening = false;
  });
}

bool Socket::valid() const noexcept {
  if (!impl_ || impl_->closed) {
    return false;
  }
  if (!is_loop_alive(impl_->loop_lifetime)) {
    return false;
  }
  if (impl_->type == SocketType::tcp) {
    return (impl_->tcp_conn && impl_->tcp_conn->valid()) || impl_->listening;
  }
  return impl_->udp && impl_->udp->valid();
}

SocketType Socket::type() const noexcept {
  if (!impl_) {
    return SocketType::tcp;
  }
  return impl_->type;
}

struct File::Impl {
  EventLoop *loop{nullptr};
  loop_lifetime_token loop_lifetime;
  uv_file fd{-1};
  std::uint64_t cursor{0};
  mutable std::mutex mutex;
  bool closed{false};
};

File::File(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

File File::open(EventLoop &loop, const std::string &path, int flags, int mode) {
  const uv_file fd = invoke_on_loop(loop, [&]() { return fs_open(loop, path, flags, mode); });
  auto impl = std::make_shared<Impl>();
  impl->loop = &loop;
  impl->loop_lifetime = loop.lifetime_token();
  impl->fd = fd;
  return File(std::move(impl));
}

File File::open_read(EventLoop &loop, const std::string &path) {
  return open(loop, path, UV_FS_O_RDONLY, 0);
}

File File::open_write(EventLoop &loop, const std::string &path, bool append, bool truncate,
                      bool create) {
  int flags = UV_FS_O_WRONLY;
  if (append) {
    flags |= UV_FS_O_APPEND;
  }
  if (truncate) {
    flags |= UV_FS_O_TRUNC;
  }
  if (create) {
    flags |= UV_FS_O_CREAT;
  }
  return open(loop, path, flags, 0644);
}

std::string File::read_at(std::size_t size, std::uint64_t offset) {
  if (!impl_ || impl_->closed) {
    throw std::runtime_error("invalid file");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  return invoke_on_loop(*impl_->loop, [impl = impl_, size, offset]() {
    return fs_read_at(*impl->loop, impl->fd, size, offset);
  });
}

std::size_t File::write_at(std::string_view data, std::uint64_t offset) {
  if (!impl_ || impl_->closed) {
    throw std::runtime_error("invalid file");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  return invoke_on_loop(*impl_->loop, [impl = impl_, data, offset]() {
    return fs_write_at(*impl->loop, impl->fd, data, offset);
  });
}

std::string File::read_some(std::size_t size) {
  if (!impl_ || impl_->closed) {
    throw std::runtime_error("invalid file");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  std::uint64_t offset = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    offset = impl_->cursor;
  }

  std::string out = read_at(size, offset);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cursor += out.size();
  }
  return out;
}

std::size_t File::write_some(std::string_view data) {
  if (!impl_ || impl_->closed) {
    throw std::runtime_error("invalid file");
  }
  ensure_loop_alive(impl_->loop_lifetime);

  std::uint64_t offset = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    offset = impl_->cursor;
  }

  const auto wrote = write_at(data, offset);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cursor += wrote;
  }
  return wrote;
}

void File::seek(std::uint64_t offset) {
  if (!impl_ || impl_->closed) {
    throw std::runtime_error("invalid file");
  }
  ensure_loop_alive(impl_->loop_lifetime);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->cursor = offset;
}

std::uint64_t File::tell() const {
  if (!impl_ || impl_->closed) {
    throw std::runtime_error("invalid file");
  }
  ensure_loop_alive(impl_->loop_lifetime);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->cursor;
}

void File::close() {
  if (!impl_ || impl_->closed) {
    return;
  }
  if (!is_loop_alive(impl_->loop_lifetime)) {
    impl_->fd = -1;
    impl_->closed = true;
    return;
  }

  invoke_on_loop(*impl_->loop, [impl = impl_]() {
    if (impl->closed) {
      return;
    }
    if (impl->fd >= 0) {
      fs_close(*impl->loop, impl->fd);
      impl->fd = -1;
    }
    impl->closed = true;
  });
}

bool File::valid() const noexcept {
  return impl_ && !impl_->closed && impl_->fd >= 0 && is_loop_alive(impl_->loop_lifetime);
}

bool create_directory(EventLoop &loop, const std::string &path, int mode) {
  return invoke_on_loop(loop, [&]() {
    const int result = fs_mkdir_raw(loop, path, mode);
    if (result >= 0) {
      return true;
    }
    if (result == UV_EEXIST) {
      return false;
    }
    throw uv_error("uv_fs_mkdir", result);
  });
}

bool remove_directory(EventLoop &loop, const std::string &path) {
  return invoke_on_loop(loop, [&]() {
    const int result = fs_rmdir_raw(loop, path);
    if (result >= 0) {
      return true;
    }
    if (result == UV_ENOENT) {
      return false;
    }
    throw uv_error("uv_fs_rmdir", result);
  });
}

bool create_file(EventLoop &loop, const std::string &path, bool truncate) {
  return invoke_on_loop(loop, [&]() {
    int flags = UV_FS_O_WRONLY | UV_FS_O_CREAT;
    if (truncate) {
      flags |= UV_FS_O_TRUNC;
    }
    const uv_file fd = fs_open(loop, path, flags, 0644);
    fs_close(loop, fd);
    return true;
  });
}

bool remove_file(EventLoop &loop, const std::string &path) {
  return invoke_on_loop(loop, [&]() {
    const int result = fs_unlink_raw(loop, path);
    if (result >= 0) {
      return true;
    }
    if (result == UV_ENOENT) {
      return false;
    }
    throw uv_error("uv_fs_unlink", result);
  });
}

void wait_fd(EventLoop &loop, uv_os_sock_t fd, int events) {
  invoke_on_loop(loop, [&]() {
    auto promise = std::make_shared<fpromise<void>>();
    auto future = promise->get_future();
    auto *state = new PollWaitState();
    state->promise = promise;
    state->poll.data = state;

    const int init_rc = uv_poll_init_socket(loop.native_loop(), &state->poll, fd);
    if (init_rc < 0) {
      delete state;
      throw uv_error("uv_poll_init_socket", init_rc);
    }

    const int start_rc = uv_poll_start(&state->poll, events, on_poll_event);
    if (start_rc < 0) {
      uv_close(reinterpret_cast<uv_handle_t *>(&state->poll), on_poll_closed);
      throw uv_error("uv_poll_start", start_rc);
    }

    future.get();
  });
}

void wait_readable(EventLoop &loop, uv_os_sock_t fd) {
  wait_fd(loop, fd, UV_READABLE);
}

void wait_writable(EventLoop &loop, uv_os_sock_t fd) {
  wait_fd(loop, fd, UV_WRITABLE);
}

bool wait_fd_for(EventLoop &loop, uv_os_sock_t fd, int events, std::chrono::milliseconds timeout) {
  return invoke_on_loop(loop, [&]() {
    auto promise = std::make_shared<fpromise<bool>>();
    auto future = promise->get_future();
    auto *state = new PollWaitForState();
    state->promise = promise;
    state->poll.data = state;
    state->timer.data = state;

    const int init_poll_rc = uv_poll_init_socket(loop.native_loop(), &state->poll, fd);
    if (init_poll_rc < 0) {
      delete state;
      throw uv_error("uv_poll_init_socket", init_poll_rc);
    }

    const int init_timer_rc = uv_timer_init(loop.native_loop(), &state->timer);
    if (init_timer_rc < 0) {
      close_poll_wait_for_handles(state);
      throw uv_error("uv_timer_init", init_timer_rc);
    }

    const int start_poll_rc = uv_poll_start(&state->poll, events, on_poll_event_for);
    if (start_poll_rc < 0) {
      close_poll_wait_for_handles(state);
      throw uv_error("uv_poll_start", start_poll_rc);
    }

    const uint64_t timeout_ms =
        timeout.count() < 0 ? 0 : static_cast<uint64_t>(timeout.count());
    const int start_timer_rc = uv_timer_start(&state->timer, on_poll_timeout_for, timeout_ms, 0);
    if (start_timer_rc < 0) {
      uv_poll_stop(&state->poll);
      close_poll_wait_for_handles(state);
      throw uv_error("uv_timer_start", start_timer_rc);
    }

    return future.get();
  });
}

bool wait_readable_for(EventLoop &loop, uv_os_sock_t fd, std::chrono::milliseconds timeout) {
  return wait_fd_for(loop, fd, UV_READABLE, timeout);
}

bool wait_writable_for(EventLoop &loop, uv_os_sock_t fd, std::chrono::milliseconds timeout) {
  return wait_fd_for(loop, fd, UV_WRITABLE, timeout);
}

} // namespace fiberuv::io
