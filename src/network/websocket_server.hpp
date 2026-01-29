#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace camera_simple_detect::network {

class WebSocketServer;

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
 public:
  using tcp = boost::asio::ip::tcp;
  using websocket = boost::beast::websocket::stream<tcp::socket>;

  WebSocketSession(websocket socket, WebSocketServer& server);

  void Start();
  void Send(std::string message);

 private:
  void DoRead();
  void OnRead(boost::beast::error_code error, std::size_t bytes_transferred);
  void OnWrite(boost::beast::error_code error, std::size_t bytes_transferred);

  websocket socket_;
  WebSocketServer& server_;
  boost::beast::flat_buffer buffer_;
  std::mutex write_mutex_;
};

class WebSocketServer {
 public:
  using tcp = boost::asio::ip::tcp;

  WebSocketServer(boost::asio::io_context& io_context,
                  const tcp::endpoint& endpoint);

  void Start();
  void Stop();
  void BroadcastStatus(const std::string& status);

  void RegisterSession(const std::shared_ptr<WebSocketSession>& session);
  void UnregisterExpiredSessions();

 private:
  void DoAccept();

  boost::asio::io_context& io_context_;
  tcp::acceptor acceptor_;
  std::atomic<bool> running_{false};
  std::mutex sessions_mutex_;
  std::vector<std::weak_ptr<WebSocketSession>> sessions_;
};

std::string BuildStatusJson(const std::string& status,
                            std::chrono::system_clock::time_point timestamp);

}  // namespace camera_simple_detect::network
