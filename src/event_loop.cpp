#include "fiberuv/fiberuv.hpp"

#include <algorithm>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <utility>

namespace fiberuv {

struct PeriodicTask::Impl {
  EventLoop *loop{nullptr};
  loop_lifetime_token loop_lifetime;
  uv_timer_t timer{};

  std::function<void()> fn;
  std::shared_ptr<Impl> keepalive;

  std::atomic<bool> active{false};
  std::atomic<bool> stopping{false};
  bool initialized{false};
};

namespace {

std::runtime_error uv_error(const char *where, int code) {
  return std::runtime_error(std::string(where) + ": " + uv_strerror(code));
}

thread_local EventLoop *tls_current_loop = nullptr;

class CurrentLoopGuard {
public:
  explicit CurrentLoopGuard(EventLoop *loop) : previous_(tls_current_loop) {
    tls_current_loop = loop;
  }

  ~CurrentLoopGuard() {
    tls_current_loop = previous_;
  }

private:
  EventLoop *previous_{nullptr};
};

struct SleepState {
  std::shared_ptr<fpromise<void>> promise;
  uv_timer_t timer{};
};

void on_sleep_timer_closed(uv_handle_t *handle) {
  auto *state = static_cast<SleepState *>(handle->data);
  delete state;
}

void on_sleep_timer(uv_timer_t *timer) {
  auto *state = static_cast<SleepState *>(timer->data);
  state->promise->set_value();
  uv_timer_stop(timer);
  uv_close(reinterpret_cast<uv_handle_t *>(timer), on_sleep_timer_closed);
}

void on_periodic_timer_closed(uv_handle_t *handle) {
  auto *impl = static_cast<PeriodicTask::Impl *>(handle->data);
  impl->active.store(false, std::memory_order_release);
  impl->stopping.store(false, std::memory_order_release);
  impl->keepalive.reset();
}

void stop_periodic_on_loop(const std::shared_ptr<PeriodicTask::Impl> &impl) {
  if (!impl || !impl->initialized) {
    return;
  }

  if (impl->stopping.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  uv_handle_t *handle = reinterpret_cast<uv_handle_t *>(&impl->timer);
  if (!uv_is_closing(handle)) {
    uv_timer_stop(&impl->timer);
    uv_close(handle, on_periodic_timer_closed);
  } else {
    impl->active.store(false, std::memory_order_release);
    impl->stopping.store(false, std::memory_order_release);
    impl->keepalive.reset();
  }
}

void on_periodic_timer(uv_timer_t *timer) {
  auto *impl = static_cast<PeriodicTask::Impl *>(timer->data);
  if (!impl->active.load(std::memory_order_acquire) ||
      impl->stopping.load(std::memory_order_acquire)) {
    return;
  }
  try {
    impl->loop->spawn([fn = impl->fn]() mutable { fn(); });
  } catch (...) {
  }
}

void start_periodic_on_loop(const std::shared_ptr<PeriodicTask::Impl> &impl,
                            std::chrono::milliseconds interval) {
  if (!impl || impl->loop == nullptr) {
    throw std::runtime_error("invalid periodic task state");
  }
  if (!is_loop_alive(impl->loop_lifetime)) {
    throw std::runtime_error("event loop is not alive");
  }

  if (interval.count() < 0) {
    interval = std::chrono::milliseconds(0);
  }
  const uint64_t repeat = static_cast<uint64_t>(interval.count());

  impl->timer.data = impl.get();
  impl->keepalive = impl;

  const int init_rc = uv_timer_init(impl->loop->native_loop(), &impl->timer);
  if (init_rc < 0) {
    impl->keepalive.reset();
    throw uv_error("uv_timer_init(periodic)", init_rc);
  }

  const int start_rc = uv_timer_start(&impl->timer, on_periodic_timer, repeat, repeat);
  if (start_rc < 0) {
    uv_close(reinterpret_cast<uv_handle_t *>(&impl->timer), on_periodic_timer_closed);
    throw uv_error("uv_timer_start(periodic)", start_rc);
  }

  impl->initialized = true;
  impl->active.store(true, std::memory_order_release);
}

} // namespace

EventLoop *try_get_current_loop() noexcept {
  return tls_current_loop;
}

EventLoop &get_current_loop() {
  if (tls_current_loop == nullptr) {
    throw std::runtime_error("No running EventLoop on current thread.");
  }
  return *tls_current_loop;
}

EventLoop &loop() {
  return get_current_loop();
}

void stop_loop() {
  get_current_loop().stop();
}

void sleep_for(std::chrono::milliseconds delay) {
  auto &current = get_current_loop();
  auto promise = std::make_shared<fpromise<void>>();
  auto future = promise->get_future();

  auto *state = new SleepState();
  state->promise = promise;
  state->timer.data = state;

  const int init_rc = uv_timer_init(current.native_loop(), &state->timer);
  if (init_rc < 0) {
    delete state;
    throw uv_error("uv_timer_init", init_rc);
  }

  const uint64_t timeout = delay.count() < 0 ? 0 : static_cast<uint64_t>(delay.count());
  const int start_rc = uv_timer_start(&state->timer, on_sleep_timer, timeout, 0);
  if (start_rc < 0) {
    uv_close(reinterpret_cast<uv_handle_t *>(&state->timer), on_sleep_timer_closed);
    throw uv_error("uv_timer_start", start_rc);
  }

  future.get();
}

PeriodicTask::PeriodicTask(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

PeriodicTask::~PeriodicTask() {
  stop();
}

bool PeriodicTask::active() const noexcept {
  auto impl = impl_;
  if (!impl) {
    return false;
  }
  return impl->active.load(std::memory_order_acquire);
}

void PeriodicTask::stop() noexcept {
  auto impl = impl_;
  if (!impl) {
    return;
  }

  if (!is_loop_alive(impl->loop_lifetime)) {
    impl->active.store(false, std::memory_order_release);
    impl->stopping.store(false, std::memory_order_release);
    impl->keepalive.reset();
    return;
  }

  if (try_get_current_loop() == impl->loop) {
    stop_periodic_on_loop(impl);
    return;
  }

  auto weak = std::weak_ptr<Impl>(impl);
  try {
    dispatch_to_loop(*impl->loop, [weak]() {
      if (auto s = weak.lock()) {
        stop_periodic_on_loop(s);
      }
    });
  } catch (...) {
  }
}

PeriodicTask every(EventLoop &loop, std::chrono::milliseconds interval, std::function<void()> fn) {
  if (!fn) {
    throw std::runtime_error("every() requires a valid callback");
  }
  if (interval.count() < 0) {
    interval = std::chrono::milliseconds(0);
  }

  auto impl = std::make_shared<PeriodicTask::Impl>();
  impl->loop = &loop;
  impl->loop_lifetime = loop.lifetime_token();
  impl->fn = std::move(fn);

  if (try_get_current_loop() == &loop) {
    start_periodic_on_loop(impl, interval);
    return PeriodicTask(std::move(impl));
  }

  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
  dispatch_to_loop(loop, [impl, interval, promise]() mutable {
    try {
      start_periodic_on_loop(impl, interval);
      promise->set_value();
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  });
  future.get();
  return PeriodicTask(std::move(impl));
}

EventLoop::EventLoop(std::size_t blocking_threads) {
  const int init_rc = uv_loop_init(&loop_);
  if (init_rc != 0) {
    throw uv_error("uv_loop_init", init_rc);
  }

  async_.data = this;
  const int async_rc = uv_async_init(&loop_, &async_, [](uv_async_t *handle) {
    auto *self = static_cast<EventLoop *>(handle->data);
    self->drain_loop_tasks();
  });
  if (async_rc != 0) {
    uv_loop_close(&loop_);
    throw uv_error("uv_async_init", async_rc);
  }

  if (blocking_threads == 0) {
    const unsigned int hw = std::thread::hardware_concurrency();
    blocking_threads = std::max<std::size_t>(1, static_cast<std::size_t>(hw == 0 ? 4 : hw));
  }
  start_workers(blocking_threads);

  set_unhandled_exception_handler([](std::exception_ptr ex) {
    if (!ex) {
      return;
    }
    try {
      std::rethrow_exception(ex);
    } catch (const std::exception &err) {
      std::fprintf(stderr, "[fiberuv] unhandled exception in spawned fiber: %s\n", err.what());
    } catch (...) {
      std::fprintf(stderr, "[fiberuv] unhandled non-std exception in spawned fiber\n");
    }
  });
}

EventLoop::~EventLoop() {
  if (lifetime_flag_) {
    lifetime_flag_->store(false, std::memory_order_release);
  }

  stop();
  stop_workers();

  if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&async_))) {
    uv_close(reinterpret_cast<uv_handle_t *>(&async_), nullptr);
  }

  uv_walk(
      &loop_,
      [](uv_handle_t *handle, void *) {
        if (!uv_is_closing(handle)) {
          uv_close(handle, nullptr);
        }
      },
      nullptr);

  while (uv_run(&loop_, UV_RUN_DEFAULT) != 0) {
  }
  drain_loop_tasks();

  const int close_rc = uv_loop_close(&loop_);
  if (close_rc != 0) {
    // If close fails at teardown there is not much left to recover here.
  }
}

void EventLoop::run() {
  CurrentLoopGuard loop_guard(this);
  fibers::use_scheduling_algorithm<fround_robin>();

  while (true) {
    if (stopping_.load(std::memory_order_acquire)) {
      break;
    }

    uv_run(&loop_, UV_RUN_NOWAIT);
    drain_loop_tasks();
    this_fiber::yield();

    const bool idle = !uv_loop_alive(&loop_) &&
                      active_fibers_.load(std::memory_order_acquire) == 0 &&
                      pending_blocking_.load(std::memory_order_acquire) == 0 && loop_tasks_empty();
    if (idle) {
      break;
    }
  }
}

void EventLoop::stop() {
  stopping_.store(true, std::memory_order_release);
  uv_stop(&loop_);
  uv_async_send(&async_);
}

void EventLoop::set_unhandled_exception_handler(std::function<void(std::exception_ptr)> handler) {
  std::lock_guard<std::mutex> lock(unhandled_exception_mutex_);
  unhandled_exception_handler_ = std::move(handler);
}

std::size_t EventLoop::unhandled_exception_count() const noexcept {
  return unhandled_exception_count_.load(std::memory_order_acquire);
}

std::exception_ptr EventLoop::take_last_unhandled_exception() {
  std::lock_guard<std::mutex> lock(unhandled_exception_mutex_);
  auto out = last_unhandled_exception_;
  last_unhandled_exception_ = nullptr;
  return out;
}

void EventLoop::post_to_loop(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(loop_tasks_mutex_);
    loop_tasks_.push(std::move(fn));
  }
  uv_async_send(&async_);
}

void EventLoop::drain_loop_tasks() {
  while (true) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(loop_tasks_mutex_);
      if (loop_tasks_.empty()) {
        break;
      }
      fn = std::move(loop_tasks_.front());
      loop_tasks_.pop();
    }
    fn();
  }
}

bool EventLoop::loop_tasks_empty() const {
  std::lock_guard<std::mutex> lock(loop_tasks_mutex_);
  return loop_tasks_.empty();
}

void EventLoop::report_unhandled_exception(std::exception_ptr ex) noexcept {
  std::function<void(std::exception_ptr)> handler;
  {
    std::lock_guard<std::mutex> lock(unhandled_exception_mutex_);
    ++unhandled_exception_count_;
    last_unhandled_exception_ = ex;
    handler = unhandled_exception_handler_;
  }

  if (!handler) {
    return;
  }
  try {
    handler(ex);
  } catch (...) {
  }
}

void EventLoop::start_workers(std::size_t worker_count) {
  workers_.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this]() {
      while (true) {
        std::function<void()> job;
        {
          std::unique_lock<std::mutex> lock(blocking_mutex_);
          blocking_cv_.wait(lock, [this] { return workers_stopping_ || !blocking_jobs_.empty(); });

          if (workers_stopping_ && blocking_jobs_.empty()) {
            return;
          }

          job = std::move(blocking_jobs_.front());
          blocking_jobs_.pop_front();
        }

        job();
      }
    });
  }
}

void EventLoop::stop_workers() {
  {
    std::lock_guard<std::mutex> lock(blocking_mutex_);
    workers_stopping_ = true;
  }
  blocking_cv_.notify_all();

  for (std::thread &t : workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
  workers_.clear();
}

} // namespace fiberuv
