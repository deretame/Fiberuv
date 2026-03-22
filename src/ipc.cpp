#include "fiberuv/fiberuv.hpp"

#include <algorithm>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fiberuv::ipc {

namespace {

std::runtime_error uv_error(std::string_view where, int code) {
  return std::runtime_error(std::string(where) + ": " + uv_strerror(code));
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

struct FsResultState {
  std::shared_ptr<fpromise<int>> promise;
  uv_fs_t req{};
};

void on_fs_result(uv_fs_t *req) {
  std::unique_ptr<FsResultState> state(static_cast<FsResultState *>(req->data));
  const int result = static_cast<int>(req->result);
  uv_fs_req_cleanup(req);
  state->promise->set_value(result);
}

int fs_write_once(EventLoop &loop, uv_file fd, const std::uint8_t *data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  auto promise = std::make_shared<fpromise<int>>();
  auto future = promise->get_future();
  auto *state = new FsResultState();
  state->promise = promise;
  state->req.data = state;

  uv_buf_t buf = uv_buf_init(reinterpret_cast<char *>(const_cast<std::uint8_t *>(data)),
                             static_cast<unsigned int>(size));
  const int rc = uv_fs_write(loop.native_loop(), &state->req, fd, &buf, 1, -1, on_fs_result);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_write(start)", rc);
  }
  return future.get();
}

int fs_read_once(EventLoop &loop, uv_file fd, std::uint8_t *data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  auto promise = std::make_shared<fpromise<int>>();
  auto future = promise->get_future();
  auto *state = new FsResultState();
  state->promise = promise;
  state->req.data = state;

  uv_buf_t buf = uv_buf_init(reinterpret_cast<char *>(data), static_cast<unsigned int>(size));
  const int rc = uv_fs_read(loop.native_loop(), &state->req, fd, &buf, 1, -1, on_fs_result);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_read(start)", rc);
  }
  return future.get();
}

int fs_close_once(EventLoop &loop, uv_file fd) {
  auto promise = std::make_shared<fpromise<int>>();
  auto future = promise->get_future();
  auto *state = new FsResultState();
  state->promise = promise;
  state->req.data = state;

  const int rc = uv_fs_close(loop.native_loop(), &state->req, fd, on_fs_result);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_close(start)", rc);
  }
  return future.get();
}

void close_fd_best_effort(EventLoop &loop, uv_file fd) noexcept {
  if (fd < 0) {
    return;
  }
  try {
    invoke_on_loop(loop, [&]() {
      const int result = fs_close_once(loop, fd);
      (void)result;
    });
  } catch (...) {
  }
}

void close_fd_with_temp_loop(uv_file fd) noexcept {
  if (fd < 0) {
    return;
  }

  uv_loop_t loop{};
  if (uv_loop_init(&loop) != 0) {
    return;
  }

  try {
    auto promise = std::make_shared<fpromise<int>>();
    auto future = promise->get_future();
    auto *state = new FsResultState();
    state->promise = promise;
    state->req.data = state;

    const int rc = uv_fs_close(&loop, &state->req, fd, on_fs_result);
    if (rc < 0) {
      delete state;
    } else {
      while (uv_run(&loop, UV_RUN_DEFAULT) != 0) {
      }
      (void)future.get();
    }
  } catch (...) {
  }

  (void)uv_loop_close(&loop);
}

bool write_exact(EventLoop &loop, uv_file fd, const std::uint8_t *data, std::size_t size) {
  constexpr std::size_t kChunkBytes = 64 * 1024;
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t chunk = (std::min)(kChunkBytes, size - offset);
    const int wrote = invoke_on_loop(loop, [&]() { return fs_write_once(loop, fd, data + offset, chunk); });
    if (wrote > 0) {
      offset += static_cast<std::size_t>(wrote);
      continue;
    }
    if (wrote == 0 || wrote == UV_EPIPE || wrote == UV_ECONNRESET || wrote == UV_EOF) {
      return false;
    }
    if (wrote == UV_EINTR || wrote == UV_EAGAIN) {
      continue;
    }
    throw uv_error("uv_fs_write", wrote);
  }
  return true;
}

bool read_exact(EventLoop &loop, uv_file fd, std::uint8_t *data, std::size_t size,
                bool allow_eof_at_start) {
  constexpr std::size_t kChunkBytes = 64 * 1024;
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t chunk = (std::min)(kChunkBytes, size - offset);
    const int nread = invoke_on_loop(loop, [&]() { return fs_read_once(loop, fd, data + offset, chunk); });
    if (nread > 0) {
      offset += static_cast<std::size_t>(nread);
      continue;
    }
    if (nread == 0 || nread == UV_EOF) {
      if (allow_eof_at_start && offset == 0) {
        return false;
      }
      throw std::runtime_error("ipc recv truncated payload");
    }
    if (nread == UV_EINTR || nread == UV_EAGAIN) {
      continue;
    }
    throw uv_error("uv_fs_read", nread);
  }
  return true;
}

} // namespace

ProcessChannel::ProcessChannel(EventLoop &loop, int read_fd, int write_fd)
    : loop_(&loop), loop_lifetime_(loop.lifetime_token()), read_fd_(read_fd), write_fd_(write_fd) {}

ProcessChannel::~ProcessChannel() {
  close();
}

ProcessChannel::ProcessChannel(ProcessChannel &&other) noexcept
    : loop_(other.loop_), loop_lifetime_(std::move(other.loop_lifetime_)), read_fd_(other.read_fd_),
      write_fd_(other.write_fd_) {
  other.loop_ = nullptr;
  other.read_fd_ = -1;
  other.write_fd_ = -1;
}

ProcessChannel &ProcessChannel::operator=(ProcessChannel &&other) noexcept {
  if (this == std::addressof(other)) {
    return *this;
  }
  close();
  loop_ = other.loop_;
  loop_lifetime_ = std::move(other.loop_lifetime_);
  read_fd_ = other.read_fd_;
  write_fd_ = other.write_fd_;
  other.loop_ = nullptr;
  other.read_fd_ = -1;
  other.write_fd_ = -1;
  return *this;
}

std::pair<ProcessChannel, ProcessChannel> ProcessChannel::pair(EventLoop &loop) {
  uv_file ab[2] = {-1, -1}; // A -> B
  uv_file ba[2] = {-1, -1}; // B -> A

  const int rc_ab = uv_pipe(ab, 0, 0);
  if (rc_ab < 0) {
    throw uv_error("uv_pipe(A->B)", rc_ab);
  }

  const int rc_ba = uv_pipe(ba, 0, 0);
  if (rc_ba < 0) {
    close_fd_best_effort(loop, ab[0]);
    close_fd_best_effort(loop, ab[1]);
    throw uv_error("uv_pipe(B->A)", rc_ba);
  }

  ProcessChannel endpoint_a(loop, ba[0], ab[1]);
  ProcessChannel endpoint_b(loop, ab[0], ba[1]);
  return std::make_pair(std::move(endpoint_a), std::move(endpoint_b));
}

ProcessChannel ProcessChannel::adopt(EventLoop &loop, int read_fd, int write_fd) {
  if (read_fd < 0 || write_fd < 0) {
    throw std::runtime_error("ipc::ProcessChannel::adopt requires valid read/write fds");
  }
  return ProcessChannel(loop, read_fd, write_fd);
}

bool ProcessChannel::valid() const noexcept {
  return loop_ != nullptr && is_loop_alive(loop_lifetime_) && read_fd_ >= 0 && write_fd_ >= 0;
}

int ProcessChannel::native_read_fd() const noexcept {
  return read_fd_;
}

int ProcessChannel::native_write_fd() const noexcept {
  return write_fd_;
}

void ProcessChannel::close() {
  EventLoop *loop = loop_;
  const int read_fd = read_fd_;
  const int write_fd = write_fd_;

  loop_ = nullptr;
  read_fd_ = -1;
  write_fd_ = -1;

  if (loop == nullptr) {
    close_fd_with_temp_loop(read_fd);
    if (write_fd >= 0 && write_fd != read_fd) {
      close_fd_with_temp_loop(write_fd);
    }
    return;
  }

  if (is_loop_alive(loop_lifetime_)) {
    close_fd_best_effort(*loop, read_fd);
    if (write_fd >= 0 && write_fd != read_fd) {
      close_fd_best_effort(*loop, write_fd);
    }
  } else {
    close_fd_with_temp_loop(read_fd);
    if (write_fd >= 0 && write_fd != read_fd) {
      close_fd_with_temp_loop(write_fd);
    }
  }
}

bool ProcessChannel::send_stream(const std::vector<std::uint8_t> &bytes) {
  return send_blob(bytes.data(), bytes.size());
}

std::optional<std::vector<std::uint8_t>> ProcessChannel::recv_stream() {
  return recv_blob();
}

bool ProcessChannel::send_blob(const void *data, std::size_t size) {
  if (!is_loop_alive(loop_lifetime_) || loop_ == nullptr || read_fd_ < 0 || write_fd_ < 0) {
    throw std::runtime_error("invalid ipc process channel");
  }
  if (size > static_cast<std::size_t>((std::numeric_limits<std::uint64_t>::max)())) {
    throw std::runtime_error("ipc send_blob payload too large");
  }

  const std::uint64_t frame_size = static_cast<std::uint64_t>(size);
  const auto *frame_ptr = reinterpret_cast<const std::uint8_t *>(std::addressof(frame_size));
  if (!write_exact(*loop_, write_fd_, frame_ptr, sizeof(frame_size))) {
    return false;
  }

  if (size == 0) {
    return true;
  }
  const auto *payload = reinterpret_cast<const std::uint8_t *>(data);
  return write_exact(*loop_, write_fd_, payload, size);
}

std::optional<std::vector<std::uint8_t>> ProcessChannel::recv_blob() {
  if (!is_loop_alive(loop_lifetime_) || loop_ == nullptr || read_fd_ < 0 || write_fd_ < 0) {
    throw std::runtime_error("invalid ipc process channel");
  }

  std::uint64_t frame_size = 0;
  auto *frame_ptr = reinterpret_cast<std::uint8_t *>(std::addressof(frame_size));
  if (!read_exact(*loop_, read_fd_, frame_ptr, sizeof(frame_size), true)) {
    return std::nullopt;
  }

  if (frame_size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    throw std::runtime_error("ipc recv frame too large");
  }

  std::vector<std::uint8_t> payload(static_cast<std::size_t>(frame_size));
  if (!payload.empty()) {
    read_exact(*loop_, read_fd_, payload.data(), payload.size(), false);
  }
  return payload;
}

} // namespace fiberuv::ipc
