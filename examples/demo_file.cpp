#include "demo_tasks.hpp"

#include <iostream>
#include <string>

namespace demo {

void schedule_file_ops(fiberuv::EventLoop &ev, const SharedMessageQueue &messages) {
  ev.spawn([messages]() {
    const std::string dir = "fiberuv_tmp";
    const std::string path = "fiberuv_tmp/demo.bin";

    fiberuv::io::create_directory(dir);
    fiberuv::io::create_file(path, true);

    auto writer = fiberuv::io::File::open_write(path, false, true, true);
    writer.write_at("offset-A", 0);
    writer.write_at("offset-B", 20);
    writer.seek(8);
    writer.write_some("stream-part");
    writer.close();

    auto reader = fiberuv::io::File::open_read(path);
    std::string head = reader.read_at(8, 0);
    std::string offset = reader.read_at(8, 20);
    reader.seek(8);
    std::string stream = reader.read_some(11);
    reader.close();

    std::cout << "[file] head=" << head << " offset=" << offset << " stream=" << stream << "\n";

    fiberuv::io::remove_file(path);
    fiberuv::io::remove_directory(dir);
    messages->send("file ops completed");
  });
}

} // namespace demo

