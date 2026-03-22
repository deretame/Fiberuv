#include "fiberuv/fiberuv.hpp"

#include <algorithm>
#include <deque>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace fiberuv {

namespace {

std::runtime_error uv_error(std::string_view where, int code) {
  return std::runtime_error(std::string(where) + ": " + uv_strerror(code));
}

void ensure_loop_alive(const loop_lifetime_token &token) {
  if (!is_loop_alive(token)) {
    throw std::runtime_error("event loop is not alive");
  }
}

struct ConnectionState {
  EventLoop *loop{nullptr};
  loop_lifetime_token loop_lifetime;
  uv_tcp_t socket{};

  bool initialized{false};
  bool closing{false};
  bool closed{false};
  bool eof{false};

  struct PendingRead {
    std::shared_ptr<fpromise<std::string>> promise;
  };
  PendingRead *pending_read{nullptr};
  std::deque<std::string> buffered_reads;

  std::vector<std::shared_ptr<fpromise<void>>> close_waiters;
  std::shared_ptr<ConnectionState> keepalive;
};

void on_connection_closed(uv_handle_t *handle) {
  auto *state = static_cast<ConnectionState *>(handle->data);
  state->closed = true;
  state->closing = false;

  if (state->pending_read != nullptr) {
    state->pending_read->promise->set_exception(
        std::make_exception_ptr(std::runtime_error("connection closed")));
    delete state->pending_read;
    state->pending_read = nullptr;
  }

  std::vector<std::shared_ptr<fpromise<void>>> waiters;
  waiters.swap(state->close_waiters);
  for (const auto &promise : waiters) {
    promise->set_value();
  }

  state->keepalive.reset();
}

void start_close(const std::shared_ptr<ConnectionState> &state,
                 const std::shared_ptr<fpromise<void>> &promise) {
  if (!state || !state->initialized || state->closed) {
    if (promise) {
      promise->set_value();
    }
    return;
  }

  if (promise) {
    state->close_waiters.push_back(promise);
  }

  if (!is_loop_alive(state->loop_lifetime)) {
    state->closed = true;
    state->closing = false;
    if (state->pending_read != nullptr) {
      state->pending_read->promise->set_exception(
          std::make_exception_ptr(std::runtime_error("connection closed")));
      delete state->pending_read;
      state->pending_read = nullptr;
    }
    std::vector<std::shared_ptr<fpromise<void>>> waiters;
    waiters.swap(state->close_waiters);
    for (const auto &waiter : waiters) {
      waiter->set_value();
    }
    state->keepalive.reset();
    return;
  }

  if (state->closing) {
    return;
  }
  state->closing = true;

  uv_handle_t *handle = reinterpret_cast<uv_handle_t *>(&state->socket);
  if (!uv_is_closing(handle)) {
    uv_close(handle, on_connection_closed);
  }
}

std::shared_ptr<ConnectionState> create_connection(EventLoop *loop,
                                                   const loop_lifetime_token &loop_lifetime) {
  auto state = std::make_shared<ConnectionState>();
  state->loop = loop;
  state->loop_lifetime = loop_lifetime;

  const int init_rc = uv_tcp_init(loop->native_loop(), &state->socket);
  if (init_rc < 0) {
    throw uv_error("uv_tcp_init", init_rc);
  }

  state->initialized = true;
  state->socket.data = state.get();
  state->keepalive = state;
  return state;
}

void on_alloc_read(uv_handle_t *, std::size_t suggested_size, uv_buf_t *buf) {
  char *memory = new char[suggested_size];
  *buf = uv_buf_init(memory, static_cast<unsigned int>(suggested_size));
}

void finish_read_success(ConnectionState *state, std::string out) {
  if (state->pending_read == nullptr) {
    return;
  }

  auto *pending = state->pending_read;
  state->pending_read = nullptr;
  pending->promise->set_value(std::move(out));
  delete pending;
}

void finish_read_error(ConnectionState *state, std::exception_ptr ex) {
  if (state->pending_read == nullptr) {
    return;
  }

  auto *pending = state->pending_read;
  state->pending_read = nullptr;
  pending->promise->set_exception(ex);
  delete pending;
}

void on_connection_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  auto *state = static_cast<ConnectionState *>(stream->data);

  if (nread > 0) {
    uv_read_stop(stream);
    std::string out(buf->base, static_cast<std::size_t>(nread));
    if (state->pending_read != nullptr) {
      finish_read_success(state, std::move(out));
    } else {
      state->buffered_reads.emplace_back(std::move(out));
    }
  } else if (nread == UV_EOF) {
    state->eof = true;
    uv_read_stop(stream);
    if (state->pending_read != nullptr) {
      finish_read_success(state, "");
    }
  } else if (nread < 0) {
    uv_read_stop(stream);
    finish_read_error(
        state, std::make_exception_ptr(uv_error("uv_read_start(read)", static_cast<int>(nread))));
  }

  delete[] buf->base;
}

std::string connection_read(const std::shared_ptr<ConnectionState> &state) {
  if (!state || !state->initialized || state->closed) {
    throw std::runtime_error("connection is closed");
  }
  ensure_loop_alive(state->loop_lifetime);
  if (!state->buffered_reads.empty()) {
    std::string out = std::move(state->buffered_reads.front());
    state->buffered_reads.pop_front();
    return out;
  }
  if (state->eof) {
    return {};
  }
  if (state->pending_read != nullptr) {
    throw std::runtime_error("another read is already pending");
  }

  auto promise = std::make_shared<fpromise<std::string>>();
  auto future = promise->get_future();
  auto *pending = new ConnectionState::PendingRead();
  pending->promise = promise;
  state->pending_read = pending;

  const int rc = uv_read_start(reinterpret_cast<uv_stream_t *>(&state->socket), on_alloc_read,
                               on_connection_read);
  if (rc < 0) {
    state->pending_read = nullptr;
    delete pending;
    throw uv_error("uv_read_start", rc);
  }

  return future.get();
}

std::optional<std::string> connection_read_for(const std::shared_ptr<ConnectionState> &state,
                                               std::chrono::milliseconds timeout) {
  if (!state || !state->initialized || state->closed) {
    throw std::runtime_error("connection is closed");
  }
  ensure_loop_alive(state->loop_lifetime);
  if (!state->buffered_reads.empty()) {
    std::string out = std::move(state->buffered_reads.front());
    state->buffered_reads.pop_front();
    return out;
  }
  if (state->eof) {
    return std::string{};
  }
  if (state->pending_read != nullptr) {
    throw std::runtime_error("another read is already pending");
  }

  if (timeout.count() < 0) {
    timeout = std::chrono::milliseconds(0);
  }

  auto promise = std::make_shared<fpromise<std::string>>();
  auto future = promise->get_future();
  auto *pending = new ConnectionState::PendingRead();
  pending->promise = promise;
  state->pending_read = pending;

  const int rc = uv_read_start(reinterpret_cast<uv_stream_t *>(&state->socket), on_alloc_read,
                               on_connection_read);
  if (rc < 0) {
    state->pending_read = nullptr;
    delete pending;
    throw uv_error("uv_read_start", rc);
  }

  if (future.wait_for(timeout) == fibers::future_status::ready) {
    return future.get();
  }

  if (state->pending_read == pending) {
    uv_read_stop(reinterpret_cast<uv_stream_t *>(&state->socket));
    state->pending_read = nullptr;
    delete pending;
    return std::nullopt;
  }

  if (future.wait_for(std::chrono::milliseconds(0)) == fibers::future_status::ready) {
    return future.get();
  }

  return std::nullopt;
}

struct WriteRequestState {
  uv_write_t req{};
  std::shared_ptr<ConnectionState> connection;
  std::shared_ptr<fpromise<void>> promise;
  std::string payload;
};

void on_write_done(uv_write_t *req, int status) {
  std::unique_ptr<WriteRequestState> state(static_cast<WriteRequestState *>(req->data));
  if (status < 0) {
    state->promise->set_exception(std::make_exception_ptr(uv_error("uv_write", status)));
    return;
  }
  state->promise->set_value();
}

void connection_write(const std::shared_ptr<ConnectionState> &state, const std::string &payload) {
  if (!state || !state->initialized || state->closed) {
    throw std::runtime_error("connection is closed");
  }
  ensure_loop_alive(state->loop_lifetime);
  if (payload.empty()) {
    return;
  }

  auto promise = std::make_shared<fpromise<void>>();
  auto future = promise->get_future();
  auto *write_state = new WriteRequestState();
  write_state->connection = state;
  write_state->promise = promise;
  write_state->payload = payload;
  write_state->req.data = write_state;

  uv_buf_t write_buf = uv_buf_init(write_state->payload.data(),
                                   static_cast<unsigned int>(write_state->payload.size()));
  const int rc = uv_write(&write_state->req, reinterpret_cast<uv_stream_t *>(&state->socket),
                          &write_buf, 1, on_write_done);
  if (rc < 0) {
    delete write_state;
    throw uv_error("uv_write(start)", rc);
  }

  future.get();
}

struct ConnectRequestState {
  EventLoop *loop{nullptr};
  loop_lifetime_token loop_lifetime;
  std::shared_ptr<fpromise<std::shared_ptr<ConnectionState>>> promise;

  uv_getaddrinfo_t resolver_req{};
  uv_connect_t connect_req{};
  std::shared_ptr<ConnectionState> connection;
  std::atomic<bool> timed_out{false};
  std::atomic<bool> finished{false};
};

std::exception_ptr connect_timeout_exception() {
  return std::make_exception_ptr(std::runtime_error("tcp connect timeout"));
}

bool begin_connect_finish(ConnectRequestState *state) {
  bool expected = false;
  return state->finished.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
}

void finish_connect_success(ConnectRequestState *state) {
  if (!begin_connect_finish(state)) {
    return;
  }
  if (state->timed_out.load(std::memory_order_acquire)) {
    if (state->connection) {
      start_close(state->connection, nullptr);
    }
    try {
      state->promise->set_exception(connect_timeout_exception());
    } catch (...) {
    }
    delete state;
    return;
  }
  state->promise->set_value(state->connection);
  delete state;
}

void finish_connect_error(ConnectRequestState *state, std::exception_ptr ex) {
  if (!begin_connect_finish(state)) {
    return;
  }
  if (state->connection) {
    start_close(state->connection, nullptr);
  }
  state->promise->set_exception(ex);
  delete state;
}

void on_connected(uv_connect_t *req, int status) {
  auto *state = static_cast<ConnectRequestState *>(req->data);
  if (state->timed_out.load(std::memory_order_acquire)) {
    finish_connect_error(state, connect_timeout_exception());
    return;
  }
  if (status < 0) {
    finish_connect_error(state, std::make_exception_ptr(uv_error("uv_tcp_connect", status)));
    return;
  }
  finish_connect_success(state);
}

void on_resolved(uv_getaddrinfo_t *req, int status, addrinfo *res) {
  auto *state = static_cast<ConnectRequestState *>(req->data);
  if (state->timed_out.load(std::memory_order_acquire)) {
    if (res != nullptr) {
      uv_freeaddrinfo(res);
    }
    finish_connect_error(state, connect_timeout_exception());
    return;
  }
  if (status < 0 || res == nullptr) {
    if (res != nullptr) {
      uv_freeaddrinfo(res);
    }
    finish_connect_error(state, std::make_exception_ptr(uv_error("uv_getaddrinfo", status)));
    return;
  }

  try {
    state->connection = create_connection(state->loop, state->loop_lifetime);
  } catch (...) {
    uv_freeaddrinfo(res);
    finish_connect_error(state, std::current_exception());
    return;
  }

  if (state->timed_out.load(std::memory_order_acquire)) {
    uv_freeaddrinfo(res);
    finish_connect_error(state, connect_timeout_exception());
    return;
  }

  state->connect_req.data = state;
  const int rc =
      uv_tcp_connect(&state->connect_req, &state->connection->socket, res->ai_addr, on_connected);
  uv_freeaddrinfo(res);

  if (rc < 0) {
    finish_connect_error(state, std::make_exception_ptr(uv_error("uv_tcp_connect(start)", rc)));
  }
}

} // namespace

struct TcpConnection::Impl {
  explicit Impl(std::shared_ptr<ConnectionState> in_state) : state(std::move(in_state)) {}

  std::shared_ptr<ConnectionState> state;
};

TcpConnection::TcpConnection(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

TcpConnection::~TcpConnection() {
  try {
    close();
  } catch (...) {
  }
}

std::string TcpConnection::read() {
  if (!impl_) {
    throw std::runtime_error("invalid connection");
  }
  return connection_read(impl_->state);
}

std::optional<std::string> TcpConnection::read_for(std::chrono::milliseconds timeout) {
  if (!impl_) {
    throw std::runtime_error("invalid connection");
  }
  return connection_read_for(impl_->state, timeout);
}

void TcpConnection::write(const std::string &payload) {
  if (!impl_) {
    throw std::runtime_error("invalid connection");
  }
  connection_write(impl_->state, payload);
}

void TcpConnection::close() {
  if (!impl_) {
    return;
  }

  auto promise = std::make_shared<fpromise<void>>();
  auto future = promise->get_future();
  start_close(impl_->state, promise);
  future.get();
}

bool TcpConnection::valid() const noexcept {
  return impl_ && impl_->state && impl_->state->initialized && !impl_->state->closed &&
         is_loop_alive(impl_->state->loop_lifetime);
}

TcpClient::TcpClient(EventLoop &loop) : loop_(&loop), loop_lifetime_(loop.lifetime_token()) {}

TcpConnection TcpClient::connect(const std::string &host, int port) {
  ensure_loop_alive(loop_lifetime_);
  auto promise = std::make_shared<fpromise<std::shared_ptr<ConnectionState>>>();
  auto future = promise->get_future();
  auto *state = new ConnectRequestState();
  state->loop = loop_;
  state->loop_lifetime = loop_lifetime_;
  state->promise = promise;

  state->resolver_req.data = state;
  const std::string port_text = std::to_string(port);
  const int rc = uv_getaddrinfo(loop_->native_loop(), &state->resolver_req, on_resolved,
                                host.c_str(), port_text.c_str(), nullptr);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_getaddrinfo(start)", rc);
  }

  auto connection = future.get();
  return TcpConnection(std::make_shared<TcpConnection::Impl>(std::move(connection)));
}

std::optional<TcpConnection> TcpClient::connect_for(const std::string &host, int port,
                                                    std::chrono::milliseconds timeout) {
  ensure_loop_alive(loop_lifetime_);
  if (timeout.count() < 0) {
    timeout = std::chrono::milliseconds(0);
  }

  auto promise = std::make_shared<fpromise<std::shared_ptr<ConnectionState>>>();
  auto future = promise->get_future();
  auto *state = new ConnectRequestState();
  state->loop = loop_;
  state->loop_lifetime = loop_lifetime_;
  state->promise = promise;

  state->resolver_req.data = state;
  const std::string port_text = std::to_string(port);
  const int rc = uv_getaddrinfo(loop_->native_loop(), &state->resolver_req, on_resolved,
                                host.c_str(), port_text.c_str(), nullptr);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_getaddrinfo(start)", rc);
  }

  if (future.wait_for(timeout) == fibers::future_status::ready) {
    auto connection = future.get();
    return TcpConnection(std::make_shared<TcpConnection::Impl>(std::move(connection)));
  }

  if (future.wait_for(std::chrono::milliseconds(0)) == fibers::future_status::ready) {
    auto connection = future.get();
    return TcpConnection(std::make_shared<TcpConnection::Impl>(std::move(connection)));
  }

  state->timed_out.store(true, std::memory_order_release);
  if (state->connection) {
    start_close(state->connection, nullptr);
  }
  (void)uv_cancel(reinterpret_cast<uv_req_t *>(&state->resolver_req));
  if (state->connect_req.data != nullptr) {
    (void)uv_cancel(reinterpret_cast<uv_req_t *>(&state->connect_req));
  }
  return std::nullopt;
}

std::string TcpClient::request(const std::string &host, int port, const std::string &payload) {
  ensure_loop_alive(loop_lifetime_);
  TcpConnection connection = connect(host, port);
  if (!payload.empty()) {
    connection.write(payload);
  }
  std::string response = connection.read();
  connection.close();
  return response;
}

struct TcpServer::Impl : std::enable_shared_from_this<TcpServer::Impl> {
  explicit Impl(EventLoop &in_loop) : loop(&in_loop), loop_lifetime(in_loop.lifetime_token()) {}

  EventLoop *loop{nullptr};
  loop_lifetime_token loop_lifetime;
  uv_tcp_t server{};
  bool server_initialized{false};
  bool listening{false};
  bool closing{false};
  bool server_closed{false};

  std::queue<std::shared_ptr<ConnectionState>> accepted_connections;
  std::deque<std::shared_ptr<fpromise<std::shared_ptr<ConnectionState>>>> accept_waiters;
  std::shared_ptr<Impl> keepalive;

  static void fail_accept_waiters(Impl *self, const std::exception_ptr &ex) {
    while (!self->accept_waiters.empty()) {
      auto promise = self->accept_waiters.front();
      self->accept_waiters.pop_front();
      promise->set_exception(ex);
    }
  }

  static void dispatch_accepted(Impl *self, std::shared_ptr<ConnectionState> connection) {
    if (!self->accept_waiters.empty()) {
      auto promise = self->accept_waiters.front();
      self->accept_waiters.pop_front();
      promise->set_value(std::move(connection));
      return;
    }
    self->accepted_connections.push(std::move(connection));
  }

  static void on_server_closed(uv_handle_t *handle) {
    auto *self = static_cast<Impl *>(handle->data);
    self->server_closed = true;

    auto ex = std::make_exception_ptr(std::runtime_error("server is closed"));
    fail_accept_waiters(self, ex);

    if (self->closing) {
      self->keepalive.reset();
    }
  }

  static void on_connection(uv_stream_t *server_stream, int status) {
    auto *self = static_cast<Impl *>(server_stream->data);
    if (status < 0 || self->closing) {
      return;
    }

    std::shared_ptr<ConnectionState> connection;
    try {
      connection = create_connection(self->loop, self->loop_lifetime);
    } catch (...) {
      return;
    }

    const int accept_rc =
        uv_accept(server_stream, reinterpret_cast<uv_stream_t *>(&connection->socket));
    if (accept_rc < 0) {
      start_close(connection, nullptr);
      return;
    }

    dispatch_accepted(self, std::move(connection));
  }

  void begin_close() {
    if (closing) {
      return;
    }
    closing = true;

    auto ex = std::make_exception_ptr(std::runtime_error("server is closed"));
    fail_accept_waiters(this, ex);

    while (!accepted_connections.empty()) {
      start_close(accepted_connections.front(), nullptr);
      accepted_connections.pop();
    }

    if (server_initialized && is_loop_alive(loop_lifetime)) {
      uv_handle_t *handle = reinterpret_cast<uv_handle_t *>(&server);
      if (!uv_is_closing(handle)) {
        uv_close(handle, on_server_closed);
      } else {
        server_closed = true;
        keepalive.reset();
      }
    } else {
      server_closed = true;
      keepalive.reset();
    }
  }
};

TcpServer::TcpServer(EventLoop &loop) : impl_(std::make_shared<Impl>(loop)) {}

TcpServer::~TcpServer() {
  close();
}

void TcpServer::listen(const std::string &host, int port, int backlog) {
  if (!impl_) {
    throw std::runtime_error("TcpServer is not initialized");
  }
  ensure_loop_alive(impl_->loop_lifetime);
  if (impl_->listening) {
    return;
  }

  const int init_rc = uv_tcp_init(impl_->loop->native_loop(), &impl_->server);
  if (init_rc < 0) {
    throw uv_error("uv_tcp_init(server)", init_rc);
  }
  impl_->server_initialized = true;
  impl_->server.data = impl_.get();

  sockaddr_in addr4{};
  const int addr_rc = uv_ip4_addr(host.c_str(), port, &addr4);
  if (addr_rc != 0) {
    throw uv_error("uv_ip4_addr", addr_rc);
  }

  const int bind_rc = uv_tcp_bind(&impl_->server, reinterpret_cast<const sockaddr *>(&addr4), 0);
  if (bind_rc < 0) {
    throw uv_error("uv_tcp_bind", bind_rc);
  }

  const int listen_rc =
      uv_listen(reinterpret_cast<uv_stream_t *>(&impl_->server), backlog, Impl::on_connection);
  if (listen_rc < 0) {
    throw uv_error("uv_listen", listen_rc);
  }

  impl_->listening = true;
  impl_->keepalive = impl_->shared_from_this();
}

TcpConnection TcpServer::accept() {
  if (!impl_) {
    throw std::runtime_error("TcpServer is not initialized");
  }
  ensure_loop_alive(impl_->loop_lifetime);
  if (!impl_->listening) {
    throw std::runtime_error("TcpServer is not listening");
  }
  if (impl_->closing) {
    throw std::runtime_error("TcpServer is closed");
  }

  if (!impl_->accepted_connections.empty()) {
    auto connection = impl_->accepted_connections.front();
    impl_->accepted_connections.pop();
    return TcpConnection(std::make_shared<TcpConnection::Impl>(std::move(connection)));
  }

  auto promise = std::make_shared<fpromise<std::shared_ptr<ConnectionState>>>();
  auto future = promise->get_future();
  impl_->accept_waiters.push_back(promise);
  auto connection = future.get();
  return TcpConnection(std::make_shared<TcpConnection::Impl>(std::move(connection)));
}

std::optional<TcpConnection> TcpServer::accept_for(std::chrono::milliseconds timeout) {
  if (!impl_) {
    throw std::runtime_error("TcpServer is not initialized");
  }
  ensure_loop_alive(impl_->loop_lifetime);
  if (!impl_->listening) {
    throw std::runtime_error("TcpServer is not listening");
  }
  if (impl_->closing) {
    throw std::runtime_error("TcpServer is closed");
  }

  if (!impl_->accepted_connections.empty()) {
    auto connection = impl_->accepted_connections.front();
    impl_->accepted_connections.pop();
    return TcpConnection(std::make_shared<TcpConnection::Impl>(std::move(connection)));
  }

  if (timeout.count() < 0) {
    timeout = std::chrono::milliseconds(0);
  }

  auto promise = std::make_shared<fpromise<std::shared_ptr<ConnectionState>>>();
  auto future = promise->get_future();
  impl_->accept_waiters.push_back(promise);

  if (future.wait_for(timeout) == fibers::future_status::ready) {
    auto connection = future.get();
    return TcpConnection(std::make_shared<TcpConnection::Impl>(std::move(connection)));
  }

  const auto it = std::find(impl_->accept_waiters.begin(), impl_->accept_waiters.end(), promise);
  if (it != impl_->accept_waiters.end()) {
    impl_->accept_waiters.erase(it);
    return std::nullopt;
  }

  if (future.wait_for(std::chrono::milliseconds(0)) == fibers::future_status::ready) {
    auto connection = future.get();
    return TcpConnection(std::make_shared<TcpConnection::Impl>(std::move(connection)));
  }

  return std::nullopt;
}

void TcpServer::close() {
  if (!impl_) {
    return;
  }
  impl_->begin_close();
  impl_.reset();
}

} // namespace fiberuv
