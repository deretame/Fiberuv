# Fiberuv

`Fiberuv` is a small event-loop library built with `Boost.Fiber + libuv`.

It includes:

- Fiber-based task scheduling on top of `libuv`
- File I/O (`read_file`, `write_file`)
- Network I/O (`TcpServer`, `TcpClient::request`)
- UDP I/O (`UdpSocket::bind`, `UdpSocket::send_to`, `UdpSocket::recv_from`)
- Unified high-level I/O API (`fiberuv::io::Socket`, `fiberuv::io::File`)
- A simple blocking-task interface (`run_blocking`) backed by worker threads
- Structured concurrency with `TaskScope` (`spawn/go`, cancellation token, RAII auto-join on scope exit)
- A cross-thread message queue (`MessageQueue<T>`) for fiber/thread send and receive
- A cross-process IPC channel (`ipc::ProcessChannel`) built on `libuv` pipes
- Timeout-capable FD wait helpers (`io::wait_*_for`)
- EventLoop unhandled-exception reporting hook
- Periodic non-blocking timers (`every(...)`, `loop_every(...)`) with `std::chrono` input

Common blocking receive APIs also provide timeout variants:

- `TcpConnection::read_for(...)`
- `TcpClient::connect_for(...)`
- `TcpServer::accept_for(...)`
- `UdpSocket::recv_from_for(...)`
- `io::Socket::connect_for(...)`, `io::Socket::accept_for(...)`, `io::Socket::recv_for(...)`, `io::Socket::recv_from_for(...)`
- `MessageQueue<T>::recv_for(...)`
- `PeriodicTask periodic = fiberuv::loop_every(200ms, fn);` (`periodic.stop()` to cancel)

`fiberuv::io` unified style supports:

- File system helpers: `create_directory`, `remove_directory`, `create_file`, `remove_file`
- File access: `File::read_at`, `File::write_at`, `File::read_some`, `File::write_some`, `seek/tell`
- Network sockets: one-style API for TCP/UDP via `io::Socket` (`bind`, `connect`, `send/recv`, `send_to/recv_from`)

## Dependency policy

This project does **not** use system-provided `libuv` or `Boost` binaries.

`CMake` downloads dependency **source code** directly and builds locally:

- `libuv` from official source tarball
- `Boost` CMake source package (building `fiber` + `context`)

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFIBERUV_BUILD_EXAMPLES=ON
cmake --build build -j
```

## Run demo

```bash
./build/fiberuv_demo
```

Expected output includes file I/O, blocking task result, and TCP request/response logs.

## Minimal usage

```cpp
#include "fiberuv/fiberuv.hpp"

fiberuv::run([] {
    auto& ev = fiberuv::loop();
    auto udp = fiberuv::io::Socket::udp(ev);
    udp.bind({"127.0.0.1", 0});
    udp.send_to({"127.0.0.1", 19090}, "ping");

    auto file = fiberuv::io::File::open_write(ev, "a.txt");
    file.write_some("hello\n");
    file.close();

    auto value = ev.run_blocking([] { return 42; });
    (void)value;

    fiberuv::stop_loop();
});
```

## libpq fd-wait demo (optional)

Fetch and build `libpq` source into `third_party/`:

```bash
./scripts/fetch_libpq_source.sh
./scripts/build_libpq.sh
```

Start PostgreSQL for local testing (example with podman):

```bash
podman run -d \
  --name my-postgres \
  -e POSTGRES_PASSWORD=mysecretpassword \
  -e POSTGRES_HOST_AUTH_METHOD=trust \
  -p 5432:5432 \
  docker.io/library/postgres
```

Build and run the demo:

```bash
cmake -S . -B build -DFIBERUV_BUILD_LIBPQ_EXAMPLE=ON
cmake --build build -j
./build/fiberuv_libpq_demo
```

The demo uses `fiberuv::io::wait_readable/wait_writable` on `PQsocket()` without adding libpq as a hard dependency of `Fiberuv`.

## Cross-process IPC payload rules

`ipc::ProcessChannel` supports:

- Typed payloads via `send<T>/recv<T>` for deep-copy-safe types
- Byte stream payloads via `send_stream/recv_stream` using `std::vector<std::uint8_t>`

For safety, user-defined struct/class types are **not** accepted by default.
You must explicitly opt in:

```cpp
struct MyPacket {
  std::uint32_t id;
  std::uint64_t ts;
};

template <>
struct fiberuv::IpcDeepCopyable<MyPacket, void> : std::true_type {};
```

This opt-in should only be used after confirming all members are cross-process deep-copy safe (no raw pointers/owned process-local resources).
