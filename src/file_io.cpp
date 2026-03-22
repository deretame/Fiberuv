#include "fiberuv/fiberuv.hpp"

#include <array>
#include <stdexcept>
#include <string_view>

namespace fiberuv {

namespace {

std::runtime_error uv_error(std::string_view where, int code) {
  return std::runtime_error(std::string(where) + ": " + uv_strerror(code));
}

struct ReadFileState {
  EventLoop *loop{nullptr};
  std::shared_ptr<fpromise<std::string>> promise;

  uv_fs_t open_req{};
  uv_fs_t read_req{};
  uv_fs_t close_req{};

  uv_file file{-1};
  std::size_t offset{0};
  std::array<char, 64 * 1024> chunk{};
  std::string out;
  std::exception_ptr error;
};

struct WriteFileState {
  EventLoop *loop{nullptr};
  std::shared_ptr<fpromise<void>> promise;

  uv_fs_t open_req{};
  uv_fs_t write_req{};
  uv_fs_t close_req{};

  uv_file file{-1};
  std::string data;
  std::size_t offset{0};
  bool append{false};
  std::exception_ptr error;
};

void finalize_read(ReadFileState *state) {
  if (state->error) {
    state->promise->set_exception(state->error);
  } else {
    state->promise->set_value(std::move(state->out));
  }
  delete state;
}

void close_read_file(ReadFileState *state);

void on_read_close(uv_fs_t *req) {
  auto *state = static_cast<ReadFileState *>(req->data);
  if (req->result < 0 && !state->error) {
    state->error =
        std::make_exception_ptr(uv_error("uv_fs_close(read)", static_cast<int>(req->result)));
  }
  uv_fs_req_cleanup(req);
  finalize_read(state);
}

void close_read_file(ReadFileState *state) {
  if (state->file < 0) {
    finalize_read(state);
    return;
  }

  state->close_req.data = state;
  const int rc =
      uv_fs_close(state->loop->native_loop(), &state->close_req, state->file, on_read_close);
  if (rc < 0) {
    if (!state->error) {
      state->error = std::make_exception_ptr(uv_error("uv_fs_close(read)", rc));
    }
    finalize_read(state);
  }
}

void read_next_chunk(ReadFileState *state);

void on_file_read(uv_fs_t *req) {
  auto *state = static_cast<ReadFileState *>(req->data);
  const ssize_t nread = req->result;
  uv_fs_req_cleanup(req);

  if (nread > 0) {
    state->out.append(state->chunk.data(), static_cast<std::size_t>(nread));
    state->offset += static_cast<std::size_t>(nread);
    read_next_chunk(state);
    return;
  }

  if (nread == 0) {
    close_read_file(state);
    return;
  }

  state->error = std::make_exception_ptr(uv_error("uv_fs_read", static_cast<int>(nread)));
  close_read_file(state);
}

void read_next_chunk(ReadFileState *state) {
  uv_buf_t buf = uv_buf_init(state->chunk.data(), static_cast<unsigned int>(state->chunk.size()));
  state->read_req.data = state;
  const int rc = uv_fs_read(state->loop->native_loop(), &state->read_req, state->file, &buf, 1,
                            static_cast<int64_t>(state->offset), on_file_read);
  if (rc < 0) {
    state->error = std::make_exception_ptr(uv_error("uv_fs_read(start)", rc));
    close_read_file(state);
  }
}

void on_file_open_for_read(uv_fs_t *req) {
  auto *state = static_cast<ReadFileState *>(req->data);
  const int result = static_cast<int>(req->result);
  uv_fs_req_cleanup(req);

  if (result < 0) {
    state->error = std::make_exception_ptr(uv_error("uv_fs_open(read)", result));
    close_read_file(state);
    return;
  }

  state->file = result;
  read_next_chunk(state);
}

void finalize_write(WriteFileState *state) {
  if (state->error) {
    state->promise->set_exception(state->error);
  } else {
    state->promise->set_value();
  }
  delete state;
}

void close_write_file(WriteFileState *state);

void on_write_close(uv_fs_t *req) {
  auto *state = static_cast<WriteFileState *>(req->data);
  if (req->result < 0 && !state->error) {
    state->error =
        std::make_exception_ptr(uv_error("uv_fs_close(write)", static_cast<int>(req->result)));
  }
  uv_fs_req_cleanup(req);
  finalize_write(state);
}

void close_write_file(WriteFileState *state) {
  if (state->file < 0) {
    finalize_write(state);
    return;
  }

  state->close_req.data = state;
  const int rc =
      uv_fs_close(state->loop->native_loop(), &state->close_req, state->file, on_write_close);
  if (rc < 0) {
    if (!state->error) {
      state->error = std::make_exception_ptr(uv_error("uv_fs_close(write)", rc));
    }
    finalize_write(state);
  }
}

void write_next_chunk(WriteFileState *state);

void on_file_write(uv_fs_t *req) {
  auto *state = static_cast<WriteFileState *>(req->data);
  const ssize_t wrote = req->result;
  uv_fs_req_cleanup(req);

  if (wrote > 0) {
    state->offset += static_cast<std::size_t>(wrote);
    write_next_chunk(state);
    return;
  }

  state->error = std::make_exception_ptr(uv_error("uv_fs_write", static_cast<int>(wrote)));
  close_write_file(state);
}

void write_next_chunk(WriteFileState *state) {
  if (state->offset >= state->data.size()) {
    close_write_file(state);
    return;
  }

  char *start = state->data.data() + state->offset;
  const std::size_t remaining = state->data.size() - state->offset;
  uv_buf_t buf = uv_buf_init(start, static_cast<unsigned int>(remaining));
  state->write_req.data = state;

  const int rc =
      uv_fs_write(state->loop->native_loop(), &state->write_req, state->file, &buf, 1,
                  state->append ? -1 : static_cast<int64_t>(state->offset), on_file_write);
  if (rc < 0) {
    state->error = std::make_exception_ptr(uv_error("uv_fs_write(start)", rc));
    close_write_file(state);
  }
}

void on_file_open_for_write(uv_fs_t *req) {
  auto *state = static_cast<WriteFileState *>(req->data);
  const int result = static_cast<int>(req->result);
  uv_fs_req_cleanup(req);

  if (result < 0) {
    state->error = std::make_exception_ptr(uv_error("uv_fs_open(write)", result));
    close_write_file(state);
    return;
  }

  state->file = result;
  write_next_chunk(state);
}

} // namespace

std::string EventLoop::read_file(const std::string &path) {
  auto promise = std::make_shared<fpromise<std::string>>();
  auto future = promise->get_future();
  auto *state = new ReadFileState();
  state->loop = this;
  state->promise = promise;

  state->open_req.data = state;
  const int rc = uv_fs_open(native_loop(), &state->open_req, path.c_str(), UV_FS_O_RDONLY, 0,
                            on_file_open_for_read);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_open(read/start)", rc);
  }

  return future.get();
}

void EventLoop::write_file(const std::string &path, const std::string &data, bool append) {
  auto promise = std::make_shared<fpromise<void>>();
  auto future = promise->get_future();
  auto *state = new WriteFileState();
  state->loop = this;
  state->promise = promise;
  state->data = data;
  state->append = append;

  int flags = UV_FS_O_WRONLY | UV_FS_O_CREAT;
  if (append) {
    flags |= UV_FS_O_APPEND;
  } else {
    flags |= UV_FS_O_TRUNC;
  }

  state->open_req.data = state;
  const int rc = uv_fs_open(native_loop(), &state->open_req, path.c_str(), flags, 0644,
                            on_file_open_for_write);
  if (rc < 0) {
    delete state;
    throw uv_error("uv_fs_open(write/start)", rc);
  }

  future.get();
}

} // namespace fiberuv
