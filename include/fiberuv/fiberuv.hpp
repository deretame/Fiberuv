#pragma once

#include <uv.h>
#include <boost/fiber/algo/round_robin.hpp>
#include <boost/fiber/channel_op_status.hpp>
#include <boost/fiber/condition_variable.hpp>
#include <boost/fiber/fiber.hpp>
#include <boost/fiber/future.hpp>
#include <boost/fiber/mutex.hpp>
#include <boost/fiber/operations.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace fiberuv {

namespace fibers = boost::fibers;
namespace this_fiber = boost::this_fiber;

template <typename T> using fpromise = fibers::promise<T>;

template <typename T> using ffuture = fibers::future<T>;

using ffiber = fibers::fiber;
using fround_robin = fibers::algo::round_robin;
using fchannel_status = fibers::channel_op_status;
using loop_lifetime_token = std::weak_ptr<std::atomic<bool>>;

inline bool is_loop_alive(const loop_lifetime_token &token) noexcept {
  auto flag = token.lock();
  if (!flag) {
    return false;
  }
  return flag->load(std::memory_order_acquire);
}

template <typename T>
concept QueueMessageType = !std::is_reference_v<T> && std::is_move_constructible_v<std::decay_t<T>>;

// IPC deep-copy policy:
// - Built-in arithmetic/enum/byte-like types are enabled by default.
// - User-defined classes/structs are disabled by default and must opt in by
//   specializing IpcDeepCopyable<T> = std::true_type after verifying every
//   member is also deep-copy safe for cross-process transfer.
template <typename T, typename = void> struct IpcDeepCopyable : std::false_type {};

template <typename T>
struct IpcDeepCopyable<T, std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>>>
    : std::true_type {};

template <> struct IpcDeepCopyable<std::byte, void> : std::true_type {};
template <> struct IpcDeepCopyable<std::uint8_t, void> : std::true_type {};
template <> struct IpcDeepCopyable<std::int8_t, void> : std::true_type {};

template <typename T, std::size_t N>
struct IpcDeepCopyable<T[N], void>
    : std::bool_constant<IpcDeepCopyable<std::remove_cv_t<T>>::value> {};

template <typename T>
concept IpcTrivialPayload =
    !std::is_reference_v<T> && !std::is_pointer_v<std::remove_cv_t<T>> &&
    std::is_trivially_copyable_v<std::remove_cv_t<T>> &&
    std::is_standard_layout_v<std::remove_cv_t<T>> && IpcDeepCopyable<std::remove_cv_t<T>>::value;

class EventLoop;
void dispatch_to_loop(EventLoop &loop, std::function<void()> fn);
EventLoop &loop();
EventLoop *try_get_current_loop() noexcept;

template <typename T>
  requires QueueMessageType<T>
class MessageQueue {
public:
  using value_type = std::decay_t<T>;

  explicit MessageQueue(EventLoop &) {}
  ~MessageQueue() {
    close();
  }

  bool send(const value_type &message) {
    return send_impl(message);
  }

  bool send(value_type &&message) {
    return send_impl(std::move(message));
  }

  template <typename... Args> bool emplace(Args &&...args) {
    std::optional<value_type> payload(std::in_place, std::forward<Args>(args)...);
    return send_payload(std::move(payload));
  }

  std::optional<value_type> recv() {
    std::shared_ptr<fpromise<std::optional<value_type>>> promise;
    ffuture<std::optional<value_type>> future;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!queue_.empty()) {
        return pop_front_unlocked();
      }
      if (closed_) {
        return std::nullopt;
      }
      promise = std::make_shared<fpromise<std::optional<value_type>>>();
      future = promise->get_future();
      fiber_waiters_.push_back(promise);
    }
    return future.get();
  }

  template <typename Rep, typename Period>
  std::optional<value_type> recv_for(std::chrono::duration<Rep, Period> timeout) {
    std::shared_ptr<fpromise<std::optional<value_type>>> promise;
    ffuture<std::optional<value_type>> future;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!queue_.empty()) {
        return pop_front_unlocked();
      }
      if (closed_) {
        return std::nullopt;
      }
      promise = std::make_shared<fpromise<std::optional<value_type>>>();
      future = promise->get_future();
      fiber_waiters_.push_back(promise);
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() < 0) {
      ms = std::chrono::milliseconds(0);
    }

    if (future.wait_for(ms) == fibers::future_status::ready) {
      return future.get();
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = std::find(fiber_waiters_.begin(), fiber_waiters_.end(), promise);
      if (it != fiber_waiters_.end()) {
        fiber_waiters_.erase(it);
        return std::nullopt;
      }
    }

    if (future.wait_for(std::chrono::milliseconds(0)) == fibers::future_status::ready) {
      return future.get();
    }

    return std::nullopt;
  }

  std::optional<value_type> recv_blocking() {
    std::unique_lock<std::mutex> lock(mutex_);
    recv_cv_.wait(lock, [this]() { return closed_ || !queue_.empty(); });
    if (!queue_.empty()) {
      return pop_front_unlocked();
    }
    return std::nullopt;
  }

  template <typename Rep, typename Period>
  std::optional<value_type> recv_blocking_for(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    recv_cv_.wait_for(lock, timeout, [this]() { return closed_ || !queue_.empty(); });
    if (!queue_.empty()) {
      return pop_front_unlocked();
    }
    return std::nullopt;
  }

  bool try_recv(value_type &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  std::optional<value_type> try_recv() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    return pop_front_unlocked();
  }

  void close() noexcept {
    std::deque<std::shared_ptr<fpromise<std::optional<value_type>>>> waiters;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
      waiters.swap(fiber_waiters_);
    }

    recv_cv_.notify_all();
    for (const auto &waiter : waiters) {
      // boost::fibers future/promise synchronization is thread-safe.
      waiter->set_value(std::nullopt);
    }
  }

  bool closed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

private:
  std::optional<value_type> pop_front_unlocked() {
    value_type out = std::move(queue_.front());
    queue_.pop_front();
    return std::optional<value_type>(std::move(out));
  }

  template <typename U> bool send_impl(U &&message) {
    // Build payload first. If construction throws, we don't consume a waiter.
    std::optional<value_type> payload(std::in_place, std::forward<U>(message));
    return send_payload(std::move(payload));
  }

  bool send_payload(std::optional<value_type> payload) {
    std::shared_ptr<fpromise<std::optional<value_type>>> waiter;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      if (!fiber_waiters_.empty()) {
        waiter = std::move(fiber_waiters_.front());
        fiber_waiters_.pop_front();
      } else {
        queue_.push_back(std::move(*payload));
        recv_cv_.notify_one();
        return true;
      }
    }

    // Setting promise from another thread is supported by boost::fibers.
    waiter->set_value(std::move(payload));
    return true;
  }

  mutable std::mutex mutex_;
  std::condition_variable recv_cv_;
  std::deque<value_type> queue_;
  bool closed_{false};
  std::deque<std::shared_ptr<fpromise<std::optional<value_type>>>> fiber_waiters_;
};

class EventLoop {
public:
  explicit EventLoop(std::size_t blocking_threads = 0);
  ~EventLoop();

  EventLoop(const EventLoop &) = delete;
  EventLoop &operator=(const EventLoop &) = delete;

  uv_loop_t *native_loop() noexcept {
    return &loop_;
  }
  const uv_loop_t *native_loop() const noexcept {
    return &loop_;
  }

  loop_lifetime_token lifetime_token() const noexcept {
    return lifetime_flag_;
  }

  void run();
  void stop();
  void set_unhandled_exception_handler(std::function<void(std::exception_ptr)> handler);
  std::size_t unhandled_exception_count() const noexcept;
  std::exception_ptr take_last_unhandled_exception();
  template <typename Fn> void dispatch(Fn &&fn) {
    using Task = std::decay_t<Fn>;
    auto task = std::make_shared<Task>(std::forward<Fn>(fn));
    post_to_loop([task]() { (*task)(); });
  }

  template <typename Fn> void spawn(Fn &&fn) {
    using Task = std::decay_t<Fn>;
    auto task = std::make_shared<Task>(std::forward<Fn>(fn));

    active_fibers_.fetch_add(1, std::memory_order_release);
    ffiber([this, task]() mutable {
      try {
        (*task)();
      } catch (...) {
        report_unhandled_exception(std::current_exception());
      }
      active_fibers_.fetch_sub(1, std::memory_order_release);
      uv_async_send(&async_);
    }).detach();

    uv_async_send(&async_);
  }

  template <typename Fn, typename... Args> void go(Fn &&fn, Args &&...args) {
    spawn([f = std::forward<Fn>(fn), ... as = std::forward<Args>(args)]() mutable {
      std::invoke(std::move(f), std::move(as)...);
    });
  }

  template <typename Fn> auto run_blocking(Fn &&fn) -> std::invoke_result_t<Fn> {
    using Result = std::invoke_result_t<Fn>;
    using Task = std::decay_t<Fn>;
    static_assert(!std::is_reference_v<Result>,
                  "run_blocking does not support reference return types.");

    auto promise = std::make_shared<fpromise<Result>>();
    auto future = promise->get_future();
    auto task = std::make_shared<Task>(std::forward<Fn>(fn));

    pending_blocking_.fetch_add(1, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(blocking_mutex_);
      blocking_jobs_.emplace_back([this, promise, task]() {
        try {
          if constexpr (std::is_void_v<Result>) {
            (*task)();
            post_to_loop([promise]() { promise->set_value(); });
          } else {
            Result out = (*task)();
            post_to_loop(
                [promise, out = std::move(out)]() mutable { promise->set_value(std::move(out)); });
          }
        } catch (...) {
          auto ex = std::current_exception();
          post_to_loop([promise, ex]() { promise->set_exception(ex); });
        }

        pending_blocking_.fetch_sub(1, std::memory_order_release);
        uv_async_send(&async_);
      });
    }
    blocking_cv_.notify_one();

    if constexpr (std::is_void_v<Result>) {
      future.get();
    } else {
      return future.get();
    }
  }

  std::string read_file(const std::string &path);
  void write_file(const std::string &path, const std::string &data, bool append = false);

private:
  friend class TcpConnection;
  friend class TcpClient;
  friend class TcpServer;

  void post_to_loop(std::function<void()> fn);
  void drain_loop_tasks();
  bool loop_tasks_empty() const;
  void report_unhandled_exception(std::exception_ptr ex) noexcept;

  void start_workers(std::size_t worker_count);
  void stop_workers();

  uv_loop_t loop_{};
  uv_async_t async_{};
  std::shared_ptr<std::atomic<bool>> lifetime_flag_{std::make_shared<std::atomic<bool>>(true)};

  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> active_fibers_{0};
  std::atomic<std::size_t> pending_blocking_{0};

  mutable std::mutex loop_tasks_mutex_;
  std::queue<std::function<void()>> loop_tasks_;

  std::mutex blocking_mutex_;
  std::condition_variable blocking_cv_;
  std::deque<std::function<void()>> blocking_jobs_;
  bool workers_stopping_{false};
  std::vector<std::thread> workers_;

  mutable std::mutex unhandled_exception_mutex_;
  std::function<void(std::exception_ptr)> unhandled_exception_handler_;
  std::exception_ptr last_unhandled_exception_;
  std::atomic<std::size_t> unhandled_exception_count_{0};
};

class TaskScope {
  struct SharedState;

public:
  class Token {
  public:
    bool stop_requested() const noexcept;

  private:
    explicit Token(std::weak_ptr<SharedState> state) : state_(std::move(state)) {}
    std::weak_ptr<SharedState> state_;
    friend class TaskScope;
  };

  explicit TaskScope(EventLoop &loop);
  TaskScope();
  ~TaskScope();

  TaskScope(const TaskScope &) = delete;
  TaskScope &operator=(const TaskScope &) = delete;
  TaskScope(TaskScope &&) noexcept = default;
  TaskScope &operator=(TaskScope &&) noexcept = default;

  template <typename Fn> void spawn(Fn &&fn) {
    using Task = std::decay_t<Fn>;
    auto state = state_;
    if (!state || state->loop == nullptr) {
      throw std::runtime_error("TaskScope is not initialized");
    }

    auto task = std::make_shared<Task>(std::forward<Fn>(fn));
    state->active.fetch_add(1, std::memory_order_acq_rel);

    state->loop->spawn([state, task]() mutable {
      try {
        if constexpr (std::is_invocable_v<Task &, Token>) {
          std::invoke(*task, Token(std::weak_ptr<SharedState>(state)));
        } else if constexpr (std::is_invocable_v<Task &>) {
          std::invoke(*task);
        } else {
          static_assert(std::is_invocable_v<Task &, Token> || std::is_invocable_v<Task &>,
                        "TaskScope::spawn expects callable as fn() or fn(TaskScope::Token).");
        }
      } catch (...) {
        std::lock_guard<fibers::mutex> lock(state->mutex);
        if (!state->first_exception) {
          state->first_exception = std::current_exception();
        }
        state->cancel_requested.store(true, std::memory_order_release);
      }

      if (state->active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        state->cv.notify_all();
        state->std_cv.notify_all();
      }
    });
  }

  template <typename Fn, typename... Args> void go(Fn &&fn, Args &&...args) {
    spawn([f = std::forward<Fn>(fn), ... as = std::forward<Args>(args)](Token token) mutable {
      using FnType = decltype(f);
      if constexpr (std::is_invocable_v<FnType, Token, decltype(as)...>) {
        std::invoke(std::move(f), token, std::move(as)...);
      } else if constexpr (std::is_invocable_v<FnType, decltype(as)...>) {
        std::invoke(std::move(f), std::move(as)...);
      } else {
        static_assert(std::is_invocable_v<FnType, Token, decltype(as)...> ||
                          std::is_invocable_v<FnType, decltype(as)...>,
                      "TaskScope::go expects callable as fn(Token, args...) or fn(args...).");
      }
    });
  }

  void request_cancel() noexcept;
  bool cancel_requested() const noexcept;
  Token token() const noexcept;

  void join();

  template <typename Rep, typename Period>
  bool join_for(std::chrono::duration<Rep, Period> timeout) {
    auto state = state_;
    if (!state) {
      return true;
    }

    if (!is_loop_alive(state->loop_lifetime)) {
      return state->active.load(std::memory_order_acquire) == 0;
    }

    bool done = false;
    if (try_get_current_loop() == state->loop) {
      std::unique_lock<fibers::mutex> lock(state->mutex);
      done = state->cv.wait_for(lock, timeout, [&state]() {
        return state->active.load(std::memory_order_acquire) == 0;
      });
    } else {
      std::unique_lock<std::mutex> lock(state->std_mutex);
      done = state->std_cv.wait_for(lock, timeout, [&state]() {
        return state->active.load(std::memory_order_acquire) == 0 ||
               !is_loop_alive(state->loop_lifetime);
      });
      if (!done) {
        return false;
      }
      if (!is_loop_alive(state->loop_lifetime) &&
          state->active.load(std::memory_order_acquire) > 0) {
        return false;
      }
      done = true;
    }
    if (!done) {
      return false;
    }
    std::exception_ptr ex;
    {
      std::lock_guard<fibers::mutex> lock(state->mutex);
      ex = state->first_exception;
    }
    if (ex) {
      std::rethrow_exception(ex);
    }
    return true;
  }

private:
  struct SharedState {
    explicit SharedState(EventLoop *in_loop) : loop(in_loop), loop_lifetime(in_loop->lifetime_token()) {}

    EventLoop *loop{nullptr};
    loop_lifetime_token loop_lifetime;
    std::atomic<bool> cancel_requested{false};
    fibers::mutex mutex;
    fibers::condition_variable cv;
    std::mutex std_mutex;
    std::condition_variable std_cv;
    std::atomic<std::size_t> active{0};
    std::exception_ptr first_exception;
  };

  std::shared_ptr<SharedState> state_;
};

inline void dispatch_to_loop(EventLoop &loop, std::function<void()> fn) {
  loop.dispatch([fn = std::move(fn)]() mutable { fn(); });
}

inline TaskScope::TaskScope(EventLoop &loop) : state_(std::make_shared<SharedState>(&loop)) {}

inline TaskScope::TaskScope() : TaskScope(::fiberuv::loop()) {}

inline TaskScope::~TaskScope() {
  try {
    join();
  } catch (...) {
  }
}

inline bool TaskScope::Token::stop_requested() const noexcept {
  auto state = state_.lock();
  if (!state) {
    return true;
  }
  return state->cancel_requested.load(std::memory_order_acquire);
}

inline void TaskScope::request_cancel() noexcept {
  auto state = state_;
  if (!state) {
    return;
  }
  state->cancel_requested.store(true, std::memory_order_release);
}

inline bool TaskScope::cancel_requested() const noexcept {
  auto state = state_;
  if (!state) {
    return true;
  }
  return state->cancel_requested.load(std::memory_order_acquire);
}

inline TaskScope::Token TaskScope::token() const noexcept {
  return Token(state_);
}

inline void TaskScope::join() {
  auto state = state_;
  if (!state) {
    return;
  }

  if (!is_loop_alive(state->loop_lifetime)) {
    return;
  }

  if (try_get_current_loop() == state->loop) {
    std::unique_lock<fibers::mutex> lock(state->mutex);
    state->cv.wait(lock, [&state]() { return state->active.load(std::memory_order_acquire) == 0; });
  } else {
    std::unique_lock<std::mutex> lock(state->std_mutex);
    state->std_cv.wait(lock, [&state]() {
      return state->active.load(std::memory_order_acquire) == 0 ||
             !is_loop_alive(state->loop_lifetime);
    });
    if (!is_loop_alive(state->loop_lifetime) &&
        state->active.load(std::memory_order_acquire) > 0) {
      return;
    }
  }

  std::exception_ptr ex;
  {
    std::lock_guard<fibers::mutex> lock(state->mutex);
    ex = state->first_exception;
  }

  if (ex) {
    std::rethrow_exception(ex);
  }
}

class PeriodicTask {
public:
  struct Impl;

  PeriodicTask() = default;
  ~PeriodicTask();

  PeriodicTask(const PeriodicTask &) = delete;
  PeriodicTask &operator=(const PeriodicTask &) = delete;
  PeriodicTask(PeriodicTask &&) noexcept = default;
  PeriodicTask &operator=(PeriodicTask &&) noexcept = default;

  bool active() const noexcept;
  void stop() noexcept;

private:
  explicit PeriodicTask(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;

  friend PeriodicTask every(EventLoop &, std::chrono::milliseconds, std::function<void()>);
};

PeriodicTask every(EventLoop &loop, std::chrono::milliseconds interval, std::function<void()> fn);
inline PeriodicTask every(std::chrono::milliseconds interval, std::function<void()> fn) {
  return every(::fiberuv::loop(), interval, std::move(fn));
}

template <typename Rep, typename Period, typename Fn>
PeriodicTask every(EventLoop &loop, std::chrono::duration<Rep, Period> interval, Fn &&fn) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
  if (ms.count() < 0) {
    ms = std::chrono::milliseconds(0);
  }
  using Task = std::decay_t<Fn>;
  auto task = std::make_shared<Task>(std::forward<Fn>(fn));
  return every(loop, ms, [task]() mutable { (*task)(); });
}

template <typename Rep, typename Period, typename Fn>
PeriodicTask every(std::chrono::duration<Rep, Period> interval, Fn &&fn) {
  return every(::fiberuv::loop(), interval, std::forward<Fn>(fn));
}

template <typename Rep, typename Period, typename Fn>
PeriodicTask loop_every(EventLoop &loop, std::chrono::duration<Rep, Period> interval, Fn &&fn) {
  return every(loop, interval, std::forward<Fn>(fn));
}

template <typename Rep, typename Period, typename Fn>
PeriodicTask loop_every(std::chrono::duration<Rep, Period> interval, Fn &&fn) {
  return every(::fiberuv::loop(), interval, std::forward<Fn>(fn));
}

class TcpConnection {
public:
  TcpConnection() = default;
  ~TcpConnection();

  TcpConnection(const TcpConnection &) = default;
  TcpConnection &operator=(const TcpConnection &) = default;
  TcpConnection(TcpConnection &&) noexcept = default;
  TcpConnection &operator=(TcpConnection &&) noexcept = default;

  std::string read();
  std::optional<std::string> read_for(std::chrono::milliseconds timeout);
  template <typename Rep, typename Period>
  std::optional<std::string> read_for(std::chrono::duration<Rep, Period> timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() < 0) {
      ms = std::chrono::milliseconds(0);
    }
    return read_for(ms);
  }
  void write(const std::string &payload);
  void close();
  bool valid() const noexcept;

private:
  struct Impl;
  explicit TcpConnection(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;

  friend class TcpClient;
  friend class TcpServer;
};

class TcpClient {
public:
  explicit TcpClient(EventLoop &loop);
  TcpConnection connect(const std::string &host, int port);
  std::optional<TcpConnection> connect_for(const std::string &host, int port,
                                           std::chrono::milliseconds timeout);
  template <typename Rep, typename Period>
  std::optional<TcpConnection> connect_for(const std::string &host, int port,
                                           std::chrono::duration<Rep, Period> timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() < 0) {
      ms = std::chrono::milliseconds(0);
    }
    return connect_for(host, port, ms);
  }
  std::string request(const std::string &host, int port, const std::string &payload);

private:
  EventLoop *loop_{nullptr};
  loop_lifetime_token loop_lifetime_;
};

class TcpServer {
public:
  explicit TcpServer(EventLoop &loop);
  ~TcpServer();

  void listen(const std::string &host, int port, int backlog = 128);
  TcpConnection accept();
  std::optional<TcpConnection> accept_for(std::chrono::milliseconds timeout);
  template <typename Rep, typename Period>
  std::optional<TcpConnection> accept_for(std::chrono::duration<Rep, Period> timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() < 0) {
      ms = std::chrono::milliseconds(0);
    }
    return accept_for(ms);
  }
  void close();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

struct UdpPacket {
  std::string data;
  std::string host;
  int port{0};
};

class UdpSocket {
public:
  explicit UdpSocket(EventLoop &loop);
  ~UdpSocket();

  UdpSocket(const UdpSocket &) = default;
  UdpSocket &operator=(const UdpSocket &) = default;
  UdpSocket(UdpSocket &&) noexcept = default;
  UdpSocket &operator=(UdpSocket &&) noexcept = default;

  void bind(const std::string &host, int port);
  void send_to(const std::string &host, int port, const std::string &payload);
  UdpPacket recv_from();
  std::optional<UdpPacket> recv_from_for(std::chrono::milliseconds timeout);
  template <typename Rep, typename Period>
  std::optional<UdpPacket> recv_from_for(std::chrono::duration<Rep, Period> timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() < 0) {
      ms = std::chrono::milliseconds(0);
    }
    return recv_from_for(ms);
  }
  void close();
  bool valid() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

namespace io {

struct Endpoint {
  std::string host;
  int port{0};
};

struct Datagram {
  std::string data;
  Endpoint peer;
};

enum class SocketType {
  tcp,
  udp,
};

class Socket {
public:
  static Socket tcp(EventLoop &loop);
  static Socket udp(EventLoop &loop);
  static Socket tcp() {
    return tcp(::fiberuv::loop());
  }
  static Socket udp() {
    return udp(::fiberuv::loop());
  }

  void bind(const Endpoint &local);
  void listen(int backlog = 128);
  Socket accept();
  std::optional<Socket> accept_for(std::chrono::milliseconds timeout);
  template <typename Rep, typename Period>
  std::optional<Socket> accept_for(std::chrono::duration<Rep, Period> timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() < 0) {
      ms = std::chrono::milliseconds(0);
    }
    return accept_for(ms);
  }
  void connect(const Endpoint &remote);
  bool connect_for(const Endpoint &remote, std::chrono::milliseconds timeout);
  template <typename Rep, typename Period>
  bool connect_for(const Endpoint &remote, std::chrono::duration<Rep, Period> timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() < 0) {
      ms = std::chrono::milliseconds(0);
    }
    return connect_for(remote, ms);
  }

  void send(std::string_view data);
  std::string recv();
  std::optional<std::string> recv_for(std::chrono::milliseconds timeout);
  template <typename Rep, typename Period>
  std::optional<std::string> recv_for(std::chrono::duration<Rep, Period> timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() < 0) {
      ms = std::chrono::milliseconds(0);
    }
    return recv_for(ms);
  }

  void send_to(const Endpoint &remote, std::string_view data);
  Datagram recv_from();
  std::optional<Datagram> recv_from_for(std::chrono::milliseconds timeout);
  template <typename Rep, typename Period>
  std::optional<Datagram> recv_from_for(std::chrono::duration<Rep, Period> timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    if (ms.count() < 0) {
      ms = std::chrono::milliseconds(0);
    }
    return recv_from_for(ms);
  }

  void close();
  bool valid() const noexcept;
  SocketType type() const noexcept;

private:
  struct Impl;
  explicit Socket(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

class File {
public:
  static File open(EventLoop &loop, const std::string &path, int flags, int mode = 0644);
  static File open(const std::string &path, int flags, int mode = 0644) {
    return open(::fiberuv::loop(), path, flags, mode);
  }
  static File open_read(EventLoop &loop, const std::string &path);
  static File open_read(const std::string &path) {
    return open_read(::fiberuv::loop(), path);
  }
  static File open_write(EventLoop &loop, const std::string &path, bool append = false,
                         bool truncate = true, bool create = true);
  static File open_write(const std::string &path, bool append = false, bool truncate = true,
                         bool create = true) {
    return open_write(::fiberuv::loop(), path, append, truncate, create);
  }

  std::string read_at(std::size_t size, std::uint64_t offset);
  std::size_t write_at(std::string_view data, std::uint64_t offset);

  std::string read_some(std::size_t size = 4096);
  std::size_t write_some(std::string_view data);

  void seek(std::uint64_t offset);
  std::uint64_t tell() const;

  void close();
  bool valid() const noexcept;

private:
  struct Impl;
  explicit File(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

bool create_directory(EventLoop &loop, const std::string &path, int mode = 0755);
inline bool create_directory(const std::string &path, int mode = 0755) {
  return create_directory(::fiberuv::loop(), path, mode);
}
bool remove_directory(EventLoop &loop, const std::string &path);
inline bool remove_directory(const std::string &path) {
  return remove_directory(::fiberuv::loop(), path);
}
bool create_file(EventLoop &loop, const std::string &path, bool truncate = true);
inline bool create_file(const std::string &path, bool truncate = true) {
  return create_file(::fiberuv::loop(), path, truncate);
}
bool remove_file(EventLoop &loop, const std::string &path);
inline bool remove_file(const std::string &path) {
  return remove_file(::fiberuv::loop(), path);
}

void wait_fd(EventLoop &loop, uv_os_sock_t fd, int events);
inline void wait_fd(uv_os_sock_t fd, int events) {
  wait_fd(::fiberuv::loop(), fd, events);
}

void wait_readable(EventLoop &loop, uv_os_sock_t fd);
inline void wait_readable(uv_os_sock_t fd) {
  wait_readable(::fiberuv::loop(), fd);
}

void wait_writable(EventLoop &loop, uv_os_sock_t fd);
inline void wait_writable(uv_os_sock_t fd) {
  wait_writable(::fiberuv::loop(), fd);
}

bool wait_fd_for(EventLoop &loop, uv_os_sock_t fd, int events, std::chrono::milliseconds timeout);
template <typename Rep, typename Period>
bool wait_fd_for(EventLoop &loop, uv_os_sock_t fd, int events, std::chrono::duration<Rep, Period> timeout);
template <typename Rep, typename Period>
inline bool wait_fd_for(uv_os_sock_t fd, int events, std::chrono::duration<Rep, Period> timeout) {
  return wait_fd_for(::fiberuv::loop(), fd, events, timeout);
}

bool wait_readable_for(EventLoop &loop, uv_os_sock_t fd, std::chrono::milliseconds timeout);
template <typename Rep, typename Period>
bool wait_readable_for(EventLoop &loop, uv_os_sock_t fd, std::chrono::duration<Rep, Period> timeout);
template <typename Rep, typename Period>
inline bool wait_readable_for(uv_os_sock_t fd, std::chrono::duration<Rep, Period> timeout) {
  return wait_readable_for(::fiberuv::loop(), fd, timeout);
}

bool wait_writable_for(EventLoop &loop, uv_os_sock_t fd, std::chrono::milliseconds timeout);
template <typename Rep, typename Period>
bool wait_writable_for(EventLoop &loop, uv_os_sock_t fd, std::chrono::duration<Rep, Period> timeout);
template <typename Rep, typename Period>
inline bool wait_writable_for(uv_os_sock_t fd, std::chrono::duration<Rep, Period> timeout) {
  return wait_writable_for(::fiberuv::loop(), fd, timeout);
}

} // namespace io

namespace ipc {

class ProcessChannel {
public:
  ProcessChannel() = default;
  ~ProcessChannel();

  ProcessChannel(const ProcessChannel &) = delete;
  ProcessChannel &operator=(const ProcessChannel &) = delete;
  ProcessChannel(ProcessChannel &&other) noexcept;
  ProcessChannel &operator=(ProcessChannel &&other) noexcept;

  static std::pair<ProcessChannel, ProcessChannel> pair(EventLoop &loop);
  static std::pair<ProcessChannel, ProcessChannel> pair() {
    return pair(::fiberuv::loop());
  }
  static ProcessChannel adopt(EventLoop &loop, int read_fd, int write_fd);

  bool valid() const noexcept;
  int native_read_fd() const noexcept;
  int native_write_fd() const noexcept;
  void close();

  bool send_stream(const std::vector<std::uint8_t> &bytes);
  std::optional<std::vector<std::uint8_t>> recv_stream();

  template <IpcTrivialPayload T> bool send(const T &value) {
    return send_blob(std::addressof(value), sizeof(T));
  }

  template <IpcTrivialPayload T> std::optional<T> recv() {
    auto frame = recv_blob();
    if (!frame) {
      return std::nullopt;
    }
    if (frame->size() != sizeof(T)) {
      throw std::runtime_error("ipc recv<T>: payload size mismatch");
    }

    T out{};
    std::memcpy(std::addressof(out), frame->data(), sizeof(T));
    return out;
  }

private:
  explicit ProcessChannel(EventLoop &loop, int read_fd, int write_fd);
  bool send_blob(const void *data, std::size_t size);
  std::optional<std::vector<std::uint8_t>> recv_blob();

  EventLoop *loop_{nullptr};
  loop_lifetime_token loop_lifetime_;
  int read_fd_{-1};
  int write_fd_{-1};
};

} // namespace ipc

EventLoop *try_get_current_loop() noexcept;
EventLoop &get_current_loop();
EventLoop &loop();
template <typename Fn, typename... Args> void go(Fn &&fn, Args &&...args) {
  loop().go(std::forward<Fn>(fn), std::forward<Args>(args)...);
}
void stop_loop();
void sleep_for(std::chrono::milliseconds delay);

template <typename Rep, typename Period> void sleep_for(std::chrono::duration<Rep, Period> delay) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delay);
  if (ms.count() < 0) {
    ms = std::chrono::milliseconds(0);
  }
  sleep_for(ms);
}

template <typename Fn> void run(Fn &&fn, std::size_t blocking_threads = 0) {
  EventLoop event_loop(blocking_threads);
  event_loop.dispatch(
      [&event_loop, fn = std::forward<Fn>(fn)]() mutable { event_loop.spawn(std::move(fn)); });
  event_loop.run();
}

namespace io {

template <typename Rep, typename Period>
bool wait_fd_for(EventLoop &loop, uv_os_sock_t fd, int events,
                 std::chrono::duration<Rep, Period> timeout) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
  if (ms.count() < 0) {
    ms = std::chrono::milliseconds(0);
  }
  return wait_fd_for(loop, fd, events, ms);
}

template <typename Rep, typename Period>
bool wait_readable_for(EventLoop &loop, uv_os_sock_t fd, std::chrono::duration<Rep, Period> timeout) {
  return wait_fd_for(loop, fd, UV_READABLE, timeout);
}

template <typename Rep, typename Period>
bool wait_writable_for(EventLoop &loop, uv_os_sock_t fd, std::chrono::duration<Rep, Period> timeout) {
  return wait_fd_for(loop, fd, UV_WRITABLE, timeout);
}

} // namespace io

} // namespace fiberuv

namespace Fiberuv = fiberuv;
