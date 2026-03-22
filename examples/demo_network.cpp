#include "demo_tasks.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace demo {

void schedule_tcp_server(fiberuv::EventLoop &ev, fiberuv::io::Socket &tcp_server,
                         const SharedMessageQueue &messages,
                         fiberuv::fpromise<void> &tcp_server_done_promise) {
  ev.spawn([&tcp_server, messages, &tcp_server_done_promise]() {
    tcp_server.bind({"127.0.0.1", 18080});
    tcp_server.listen();
    std::cout << "[tcp server] listen 127.0.0.1:18080\n";
    messages->send("server ready");
    try {
      auto conn = tcp_server.accept_for(std::chrono::seconds(2));
      if (!conn) {
        messages->send("tcp server accept timeout");
        tcp_server_done_promise.set_value();
        return;
      }
      messages->send("tcp server accepted");
      auto request = conn->recv_for(std::chrono::seconds(2));
      if (!request) {
        messages->send("tcp server recv timeout");
        conn->close();
        tcp_server_done_promise.set_value();
        return;
      }
      messages->send("tcp server recv ok");
      conn->send(std::string("echo: ") + *request);
      conn->close();
      messages->send("tcp server replied");
    } catch (const std::exception &ex) {
      std::cout << "[tcp server err] " << ex.what() << "\n";
      messages->send("tcp server failed");
    }
    tcp_server_done_promise.set_value();
  });
}

void schedule_udp_server(fiberuv::EventLoop &ev, fiberuv::io::Socket &udp_server,
                         const SharedMessageQueue &messages) {
  ev.spawn([&udp_server, messages]() {
    udp_server.bind({"127.0.0.1", 19090});
    messages->send("udp server ready");
    auto request = udp_server.recv_from_for(std::chrono::seconds(2));
    if (!request) {
      messages->send("udp server recv timeout");
      return;
    }
    udp_server.send_to(request->peer, std::string("udp echo: ") + request->data);
    messages->send("udp server replied");
  });
}

void schedule_client_task(fiberuv::EventLoop &ev, const SharedMessageQueue &messages,
                          fiberuv::fpromise<void> &done_promise) {
  ev.spawn([messages, &done_promise]() {
    Fiberuv::sleep_for(std::chrono::milliseconds(250));
    messages->send("client task start");

    try {
      auto tcp_client = fiberuv::io::Socket::tcp(Fiberuv::loop());
      messages->send("tcp client connect begin");
      if (!tcp_client.connect_for({"127.0.0.1", 18080}, std::chrono::seconds(2))) {
        throw std::runtime_error("tcp client connect timeout");
      }
      messages->send("tcp client connect ok");
      tcp_client.send("ping");
      messages->send("tcp client send ok");
      auto response = tcp_client.recv_for(std::chrono::seconds(2));
      if (!response) {
        throw std::runtime_error("tcp client recv timeout");
      }
      std::cout << "[tcp client] " << *response << "\n";
      tcp_client.close();
      messages->send("client done");

      auto udp_client = fiberuv::io::Socket::udp(Fiberuv::loop());
      udp_client.bind({"127.0.0.1", 0});
      udp_client.send_to({"127.0.0.1", 19090}, "ping-udp");
      auto udp_response = udp_client.recv_from_for(std::chrono::seconds(2));
      if (!udp_response) {
        throw std::runtime_error("udp client recv timeout");
      }
      std::cout << "[udp client] " << udp_response->data << "\n";
      udp_client.close();

      messages->send("udp client done");
    } catch (const std::exception &ex) {
      std::cout << "[client err] " << ex.what() << "\n";
      messages->send("client failed");
    }

    done_promise.set_value();
  });
}

} // namespace demo
