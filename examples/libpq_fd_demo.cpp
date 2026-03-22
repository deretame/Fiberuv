#include "fiberuv/fiberuv.hpp"

#include <libpq-fe.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::runtime_error pq_error(const char *where, const char *message) {
  return std::runtime_error(std::string(where) + ": " +
                            (message ? message : "unknown libpq error"));
}

void wait_poll_fd(PGconn *conn, PostgresPollingStatusType status) {
  const int fd = PQsocket(conn);
  if (fd < 0) {
    throw std::runtime_error("PQsocket returned invalid fd");
  }

  switch (status) {
  case PGRES_POLLING_READING:
    fiberuv::io::wait_readable(static_cast<uv_os_sock_t>(fd));
    break;
  case PGRES_POLLING_WRITING:
    fiberuv::io::wait_writable(static_cast<uv_os_sock_t>(fd));
    break;
  case PGRES_POLLING_ACTIVE:
    fiberuv::this_fiber::yield();
    break;
  default:
    break;
  }
}

std::unique_ptr<PGconn, decltype(&PQfinish)> connect_nonblocking(const std::string &conninfo) {
  PGconn *raw = PQconnectStart(conninfo.c_str());
  if (raw == nullptr) {
    throw std::runtime_error("PQconnectStart returned null");
  }

  std::unique_ptr<PGconn, decltype(&PQfinish)> conn(raw, PQfinish);
  if (PQsetnonblocking(conn.get(), 1) != 0) {
    throw pq_error("PQsetnonblocking", PQerrorMessage(conn.get()));
  }

  while (true) {
    const PostgresPollingStatusType status = PQconnectPoll(conn.get());
    if (status == PGRES_POLLING_OK) {
      return conn;
    }
    if (status == PGRES_POLLING_FAILED) {
      throw pq_error("PQconnectPoll", PQerrorMessage(conn.get()));
    }
    wait_poll_fd(conn.get(), status);
  }
}

std::string query_scalar(PGconn *conn, const std::string &sql) {
  if (PQsendQuery(conn, sql.c_str()) == 0) {
    throw pq_error("PQsendQuery", PQerrorMessage(conn));
  }

  const int fd = PQsocket(conn);
  if (fd < 0) {
    throw std::runtime_error("PQsocket returned invalid fd");
  }

  while (true) {
    const int flush_status = PQflush(conn);
    if (flush_status == -1) {
      throw pq_error("PQflush", PQerrorMessage(conn));
    }
    if (flush_status == 1) {
      fiberuv::io::wait_writable(static_cast<uv_os_sock_t>(fd));
      continue;
    }

    if (PQconsumeInput(conn) == 0) {
      throw pq_error("PQconsumeInput", PQerrorMessage(conn));
    }
    if (!PQisBusy(conn)) {
      break;
    }
    fiberuv::io::wait_readable(static_cast<uv_os_sock_t>(fd));
  }

  std::string value;
  while (PGresult *res = PQgetResult(conn)) {
    std::unique_ptr<PGresult, decltype(&PQclear)> result(res, PQclear);
    if (PQresultStatus(result.get()) == PGRES_TUPLES_OK && PQntuples(result.get()) > 0 &&
        PQnfields(result.get()) > 0) {
      value = PQgetvalue(result.get(), 0, 0);
    }
  }

  return value;
}

void execute_nonquery(PGconn *conn, const std::string &sql) {
  if (PQsendQuery(conn, sql.c_str()) == 0) {
    throw pq_error("PQsendQuery", PQerrorMessage(conn));
  }

  const int fd = PQsocket(conn);
  if (fd < 0) {
    throw std::runtime_error("PQsocket returned invalid fd");
  }

  while (true) {
    const int flush_status = PQflush(conn);
    if (flush_status == -1) {
      throw pq_error("PQflush", PQerrorMessage(conn));
    }
    if (flush_status == 1) {
      fiberuv::io::wait_writable(static_cast<uv_os_sock_t>(fd));
      continue;
    }

    if (PQconsumeInput(conn) == 0) {
      throw pq_error("PQconsumeInput", PQerrorMessage(conn));
    }
    if (!PQisBusy(conn)) {
      break;
    }
    fiberuv::io::wait_readable(static_cast<uv_os_sock_t>(fd));
  }

  while (PGresult *res = PQgetResult(conn)) {
    std::unique_ptr<PGresult, decltype(&PQclear)> result(res, PQclear);
    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
      throw pq_error("execute_nonquery", PQresultErrorMessage(result.get()));
    }
  }
}

std::vector<std::vector<std::string>> query_rows(PGconn *conn, const std::string &sql) {
  if (PQsendQuery(conn, sql.c_str()) == 0) {
    throw pq_error("PQsendQuery", PQerrorMessage(conn));
  }

  const int fd = PQsocket(conn);
  if (fd < 0) {
    throw std::runtime_error("PQsocket returned invalid fd");
  }

  while (true) {
    const int flush_status = PQflush(conn);
    if (flush_status == -1) {
      throw pq_error("PQflush", PQerrorMessage(conn));
    }
    if (flush_status == 1) {
      fiberuv::io::wait_writable(static_cast<uv_os_sock_t>(fd));
      continue;
    }

    if (PQconsumeInput(conn) == 0) {
      throw pq_error("PQconsumeInput", PQerrorMessage(conn));
    }
    if (!PQisBusy(conn)) {
      break;
    }
    fiberuv::io::wait_readable(static_cast<uv_os_sock_t>(fd));
  }

  std::vector<std::vector<std::string>> rows;
  while (PGresult *res = PQgetResult(conn)) {
    std::unique_ptr<PGresult, decltype(&PQclear)> result(res, PQclear);
    const ExecStatusType status = PQresultStatus(result.get());
    if (status == PGRES_TUPLES_OK) {
      const int tuple_count = PQntuples(result.get());
      const int field_count = PQnfields(result.get());
      for (int i = 0; i < tuple_count; ++i) {
        std::vector<std::string> row;
        row.reserve(static_cast<std::size_t>(field_count));
        for (int j = 0; j < field_count; ++j) {
          row.emplace_back(PQgetvalue(result.get(), i, j));
        }
        rows.emplace_back(std::move(row));
      }
      continue;
    }

    if (status != PGRES_COMMAND_OK) {
      throw pq_error("query_rows", PQresultErrorMessage(result.get()));
    }
  }

  return rows;
}

} // namespace

int main() {
  const std::string conninfo =
      "host=127.0.0.1 port=5432 user=postgres dbname=postgres password=mysecretpassword";

  fiberuv::run([&]() {
    auto conn = connect_nonblocking(conninfo);
    std::string server_version = query_scalar(conn.get(), "select version();");
    std::cout << "[libpq] " << server_version << "\n";

    execute_nonquery(conn.get(), "create table if not exists fiberuv_users ("
                                 "id serial primary key, "
                                 "name text not null, "
                                 "age int not null)");
    execute_nonquery(conn.get(), "truncate table fiberuv_users");
    execute_nonquery(conn.get(), "insert into fiberuv_users(name, age) values "
                                 "('alice', 18), ('bob', 20), ('carol', 22)");

    auto rows = query_rows(conn.get(), "select id, name, age from fiberuv_users order by id");
    for (const auto &row : rows) {
      if (row.size() >= 3) {
        std::cout << "[row] id=" << row[0] << " name=" << row[1] << " age=" << row[2] << "\n";
      }
    }

    fiberuv::stop_loop();
  });

  return 0;
}
