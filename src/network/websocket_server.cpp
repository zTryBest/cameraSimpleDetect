#include "src/network/websocket_server.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace camera_simple_detect::network {

WebSocketSession::WebSocketSession(websocket socket, WebSocketServer& server)
    : socket_(std::move(socket)), server_(server) {}

void WebSocketSession::Start() {
  socket_.set_option(
      boost::beast::websocket::stream_base::timeout::suggested(
          boost::beast::role_type::server));
  socket_.set_option(boost::beast::websocket::stream_base::decorator(
      [](boost::beast::websocket::response_type& response) {
        response.set(boost::beast::http::field::server,
                     std::string("cameraSimpleDetect"));
      }));

  // on_open: handshake success, register the session for broadcast.
  socket_.async_accept([
      self = shared_from_this()](boost::beast::error_code error) {
    if (error) {
      return;
    }
    self->server_.RegisterSession(self);
    self->DoRead();
  });
}

void WebSocketSession::Send(std::string message) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  socket_.text(true);
  socket_.async_write(
      boost::asio::buffer(std::move(message)),
      [self = shared_from_this()](boost::beast::error_code error,
                                  std::size_t bytes_transferred) {
        self->OnWrite(error, bytes_transferred);
      });
}

void WebSocketSession::DoRead() {
  socket_.async_read(buffer_,
                     [self = shared_from_this()](
                         boost::beast::error_code error,
                         std::size_t bytes_transferred) {
                       self->OnRead(error, bytes_transferred);
                     });
}

void WebSocketSession::OnRead(boost::beast::error_code error,
                              std::size_t /*bytes_transferred*/) {
  if (error == boost::beast::websocket::error::closed) {
    return;
  }

  if (error) {
    return;
  }

  // on_message: consume client messages and keep the connection alive.
  buffer_.consume(buffer_.size());
  DoRead();
}

void WebSocketSession::OnWrite(boost::beast::error_code error,
                               std::size_t /*bytes_transferred*/) {
  if (error) {
    return;
  }
}

WebSocketServer::WebSocketServer(boost::asio::io_context& io_context,
                                 const tcp::endpoint& endpoint)
    : io_context_(io_context), acceptor_(io_context) {
  boost::beast::error_code error;
  acceptor_.open(endpoint.protocol(), error);
  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), error);
  acceptor_.bind(endpoint, error);
  acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
}

void WebSocketServer::Start() {
  if (running_.exchange(true)) {
    return;
  }
  DoAccept();
}

void WebSocketServer::Stop() {
  running_.store(false);
  boost::beast::error_code error;
  acceptor_.close(error);
}

void WebSocketServer::BroadcastStatus(const std::string& status) {
  auto payload = BuildStatusJson(status, std::chrono::system_clock::now());

  std::lock_guard<std::mutex> lock(sessions_mutex_);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (auto session = it->lock()) {
      // WebSocket server broadcast: send status to every connected client.
      session->Send(payload);
      ++it;
    } else {
      it = sessions_.erase(it);
    }
  }
}

void WebSocketServer::RegisterSession(
    const std::shared_ptr<WebSocketSession>& session) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  sessions_.push_back(session);
}

void WebSocketServer::UnregisterExpiredSessions() {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  sessions_.erase(
      std::remove_if(sessions_.begin(), sessions_.end(),
                     [](const std::weak_ptr<WebSocketSession>& session) {
                       return session.expired();
                     }),
      sessions_.end());
}

void WebSocketServer::DoAccept() {
  acceptor_.async_accept([this](boost::beast::error_code error,
                                tcp::socket socket) {
    if (!error) {
      auto session = std::make_shared<WebSocketSession>(
          WebSocketSession::websocket(std::move(socket)), *this);
      session->Start();
    }

    if (running_) {
      DoAccept();
    }
  });
}

std::string BuildStatusJson(
    const std::string& status,
    std::chrono::system_clock::time_point timestamp) {
  auto time = std::chrono::system_clock::to_time_t(timestamp);
  std::tm tm_snapshot{};
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &time);
#else
  localtime_r(&time, &tm_snapshot);
#endif

  std::ostringstream stream;
  stream << std::put_time(&tm_snapshot, "%Y-%m-%dT%H:%M:%S%z");

  std::ostringstream json;
  json << '{' << "\"status\":\"" << status << "\",";
  json << "\"timestamp\":\"" << stream.str() << "\"";
  json << '}';
  return json.str();
}

}  // namespace camera_simple_detect::network
