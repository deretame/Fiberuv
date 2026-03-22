#include "fiberuv/fiberuv.hpp"

#include <bit>
#include <cstring>
#include <deque>
#include <memory>
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

bool make_sockaddr(const std::string &host, int port, sockaddr_storage &out) {
  sockaddr_in addr4{};
  if (uv_ip4_addr(host.c_str(), port, &addr4) == 0) {
    std::memcpy(&out, &addr4, sizeof(addr4));
    return true;
  }

  sockaddr_in6 addr6{};
  if (uv_ip6_addr(host.c_str(), port, &addr6) == 0) {
    std::memcpy(&out, &addr6, sizeof(addr6));
    return true;
  }

  return false;
}

UdpPacket packet_from_sockaddr(const sockaddr *addr, std::string data) {
  UdpPacket packet;
  packet.data = std::move(data);
  if (addr == nullptr) {
    return packet;
  }

  char ip[INET6_ADDRSTRLEN] = {0};

  auto from_network_u16 = [](std::uint16_t value) -> int {
    if constexpr (std::endian::native == std::endian::little) {
      return static_cast<int>(static_cast<std::uint16_t>((value >> 8) | (value << 8)));
    } else {
      return static_cast<int>(value);
    }
  };

  if (addr->sa_family == AF_INET) {
    auto *v4 = reinterpret_cast<const sockaddr_in *>(addr);
    if (uv_ip4_name(v4, ip, sizeof(ip)) != 0) {
      throw std::runtime_error("uv_ip4_name failed");
    }
    packet.host = ip;
    packet.port = from_network_u16(v4->sin_port);
    return packet;
  }

  if (addr->sa_family == AF_INET6) {
    auto *v6 = reinterpret_cast<const sockaddr_in6 *>(addr);
    if (uv_ip6_name(v6, ip, sizeof(ip)) != 0) {
      throw std::runtime_error("uv_ip6_name failed");
    }
    packet.host = ip;
    packet.port = from_network_u16(v6->sin6_port);
    return packet;
  }

  throw std::runtime_error("Unsupported sockaddr family for UDP packet");
}

struct UdpState {
  EventLoop *loop{nullptr};
  loop_lifetime_token loop_lifetime;
  uv_udp_t handle{};

  bool initialized{false};
  bool bound{false};
  bool closing{false};
  bool closed{false};

  struct PendingRecv {
    std::shared_ptr<fpromise<UdpPacket>> promise;
  };
  PendingRecv *pending_recv{nullptr};
  std::deque<UdpPacket> buffered_packets;

  std::vector<std::shared_ptr<fpromise<void>>> close_waiters;
  std::shared_ptr<UdpState> keepalive;
};

void finish_recv_success(UdpState *state, UdpPacket packet) {
  if (state->pending_recv == nullptr) {
    return;
  }

  auto *pending = state->pending_recv;
  state->pending_recv = nullptr;
  pending->promise->set_value(std::move(packet));
  delete pending;
}

void finish_recv_error(UdpState *state, std::exception_ptr ex) {
  if (state->pending_recv == nullptr) {
    return;
  }

  auto *pending = state->pending_recv;
  state->pending_recv = nullptr;
  pending->promise->set_exception(ex);
  delete pending;
}

void on_udp_closed(uv_handle_t *handle) {
  auto *state = static_cast<UdpState *>(handle->data);
  state->closed = true;
  state->closing = false;

  if (state->pending_recv != nullptr) {
    state->pending_recv->promise->set_exception(
        std::make_exception_ptr(std::runtime_error("udp socket closed")));
    delete state->pending_recv;
    state->pending_recv = nullptr;
  }

  std::vector<std::shared_ptr<fpromise<void>>> waiters;
  waiters.swap(state->close_waiters);
  for (const auto &waiter : waiters) {
    waiter->set_value();
  }

  state->keepalive.reset();
}

void start_udp_close(const std::shared_ptr<UdpState> &state,
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
    if (state->pending_recv != nullptr) {
      state->pending_recv->promise->set_exception(
          std::make_exception_ptr(std::runtime_error("udp socket closed")));
      delete state->pending_recv;
      state->pending_recv = nullptr;
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

  uv_handle_t *handle = reinterpret_cast<uv_handle_t *>(&state->handle);
  if (!uv_is_closing(handle)) {
    uv_close(handle, on_udp_closed);
  }
}

std::shared_ptr<UdpState> create_udp_socket(EventLoop &loop) {
  auto state = std::make_shared<UdpState>();
  state->loop = &loop;
  state->loop_lifetime = loop.lifetime_token();

  const int init_rc = uv_udp_init(loop.native_loop(), &state->handle);
  if (init_rc < 0) {
    throw uv_error("uv_udp_init", init_rc);
  }

  state->initialized = true;
  state->handle.data = state.get();
  state->keepalive = state;
  return state;
}

void on_udp_alloc(uv_handle_t *, std::size_t suggested_size, uv_buf_t *buf) {
  char *memory = new char[suggested_size];
  *buf = uv_buf_init(memory, static_cast<unsigned int>(suggested_size));
}

void on_udp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const sockaddr *addr,
                 unsigned) {
  auto *state = static_cast<UdpState *>(handle->data);

  if (nread == 0 && addr == nullptr) {
    delete[] buf->base;
    return;
  }

  uv_udp_recv_stop(handle);

  if (nread < 0) {
    finish_recv_error(
        state, std::make_exception_ptr(uv_error("uv_udp_recv_start", static_cast<int>(nread))));
    delete[] buf->base;
    return;
  }

  try {
    const auto size = static_cast<std::size_t>(nread);
    std::string data(buf->base, size);
    UdpPacket packet = packet_from_sockaddr(addr, std::move(data));
    if (state->pending_recv != nullptr) {
      finish_recv_success(state, std::move(packet));
    } else {
      state->buffered_packets.emplace_back(std::move(packet));
    }
  } catch (...) {
    finish_recv_error(state, std::current_exception());
  }

  delete[] buf->base;
}

struct UdpSendState {
  uv_udp_send_t req{};
  std::shared_ptr<UdpState> socket;
  std::shared_ptr<fpromise<void>> promise;
  std::string payload;
  sockaddr_storage addr{};
};

void on_udp_send(uv_udp_send_t *req, int status) {
  std::unique_ptr<UdpSendState> state(static_cast<UdpSendState *>(req->data));
  if (status < 0) {
    state->promise->set_exception(std::make_exception_ptr(uv_error("uv_udp_send", status)));
    return;
  }
  state->promise->set_value();
}

} // namespace

struct UdpSocket::Impl {
  explicit Impl(std::shared_ptr<UdpState> in_state) : state(std::move(in_state)) {}
  std::shared_ptr<UdpState> state;
};

UdpSocket::UdpSocket(EventLoop &loop) : impl_(std::make_shared<Impl>(create_udp_socket(loop))) {}

UdpSocket::~UdpSocket() {
  try {
    close();
  } catch (...) {
  }
}

void UdpSocket::bind(const std::string &host, int port) {
  if (!impl_ || !impl_->state || !impl_->state->initialized || impl_->state->closed) {
    throw std::runtime_error("invalid udp socket");
  }
  ensure_loop_alive(impl_->state->loop_lifetime);
  if (impl_->state->bound) {
    return;
  }

  sockaddr_storage addr{};
  if (!make_sockaddr(host, port, addr)) {
    throw std::runtime_error("invalid udp bind address");
  }

  const int rc = uv_udp_bind(&impl_->state->handle, reinterpret_cast<const sockaddr *>(&addr), 0);
  if (rc < 0) {
    throw uv_error("uv_udp_bind", rc);
  }
  impl_->state->bound = true;
}

void UdpSocket::send_to(const std::string &host, int port, const std::string &payload) {
  if (!impl_ || !impl_->state || !impl_->state->initialized || impl_->state->closed) {
    throw std::runtime_error("invalid udp socket");
  }
  ensure_loop_alive(impl_->state->loop_lifetime);
  if (payload.empty()) {
    return;
  }

  sockaddr_storage addr{};
  if (!make_sockaddr(host, port, addr)) {
    throw std::runtime_error("invalid udp destination address");
  }

  auto promise = std::make_shared<fpromise<void>>();
  auto future = promise->get_future();

  auto *send_state = new UdpSendState();
  send_state->socket = impl_->state;
  send_state->promise = promise;
  send_state->payload = payload;
  send_state->addr = addr;
  send_state->req.data = send_state;

  uv_buf_t buf = uv_buf_init(send_state->payload.data(),
                             static_cast<unsigned int>(send_state->payload.size()));
  const int rc = uv_udp_send(&send_state->req, &impl_->state->handle, &buf, 1,
                             reinterpret_cast<const sockaddr *>(&send_state->addr), on_udp_send);
  if (rc < 0) {
    delete send_state;
    throw uv_error("uv_udp_send(start)", rc);
  }

  future.get();
}

UdpPacket UdpSocket::recv_from() {
  if (!impl_ || !impl_->state || !impl_->state->initialized || impl_->state->closed) {
    throw std::runtime_error("invalid udp socket");
  }
  ensure_loop_alive(impl_->state->loop_lifetime);
  if (!impl_->state->buffered_packets.empty()) {
    UdpPacket out = std::move(impl_->state->buffered_packets.front());
    impl_->state->buffered_packets.pop_front();
    return out;
  }
  if (impl_->state->pending_recv != nullptr) {
    throw std::runtime_error("another udp recv is already pending");
  }

  auto promise = std::make_shared<fpromise<UdpPacket>>();
  auto future = promise->get_future();
  auto *pending = new UdpState::PendingRecv();
  pending->promise = promise;
  impl_->state->pending_recv = pending;

  const int rc = uv_udp_recv_start(&impl_->state->handle, on_udp_alloc, on_udp_recv);
  if (rc < 0) {
    impl_->state->pending_recv = nullptr;
    delete pending;
    throw uv_error("uv_udp_recv_start", rc);
  }

  return future.get();
}

std::optional<UdpPacket> UdpSocket::recv_from_for(std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state || !impl_->state->initialized || impl_->state->closed) {
    throw std::runtime_error("invalid udp socket");
  }
  ensure_loop_alive(impl_->state->loop_lifetime);
  if (!impl_->state->buffered_packets.empty()) {
    UdpPacket out = std::move(impl_->state->buffered_packets.front());
    impl_->state->buffered_packets.pop_front();
    return out;
  }
  if (impl_->state->pending_recv != nullptr) {
    throw std::runtime_error("another udp recv is already pending");
  }

  if (timeout.count() < 0) {
    timeout = std::chrono::milliseconds(0);
  }

  auto promise = std::make_shared<fpromise<UdpPacket>>();
  auto future = promise->get_future();
  auto *pending = new UdpState::PendingRecv();
  pending->promise = promise;
  impl_->state->pending_recv = pending;

  const int rc = uv_udp_recv_start(&impl_->state->handle, on_udp_alloc, on_udp_recv);
  if (rc < 0) {
    impl_->state->pending_recv = nullptr;
    delete pending;
    throw uv_error("uv_udp_recv_start", rc);
  }

  if (future.wait_for(timeout) == fibers::future_status::ready) {
    return future.get();
  }

  if (impl_->state->pending_recv == pending) {
    uv_udp_recv_stop(&impl_->state->handle);
    impl_->state->pending_recv = nullptr;
    delete pending;
    return std::nullopt;
  }

  if (future.wait_for(std::chrono::milliseconds(0)) == fibers::future_status::ready) {
    return future.get();
  }

  return std::nullopt;
}

void UdpSocket::close() {
  if (!impl_ || !impl_->state) {
    return;
  }
  if (!is_loop_alive(impl_->state->loop_lifetime)) {
    impl_->state->closed = true;
    impl_->state->closing = false;
    if (impl_->state->pending_recv != nullptr) {
      impl_->state->pending_recv->promise->set_exception(
          std::make_exception_ptr(std::runtime_error("udp socket closed")));
      delete impl_->state->pending_recv;
      impl_->state->pending_recv = nullptr;
    }
    return;
  }

  auto promise = std::make_shared<fpromise<void>>();
  auto future = promise->get_future();
  start_udp_close(impl_->state, promise);
  future.get();
}

bool UdpSocket::valid() const noexcept {
  return impl_ && impl_->state && impl_->state->initialized && !impl_->state->closed &&
         is_loop_alive(impl_->state->loop_lifetime);
}

} // namespace fiberuv
