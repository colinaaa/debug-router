// Copyright 2023 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "debug_router/native/socket/socket_server_type.h"
#ifdef _WIN32
#include "debug_router/native/socket/win/socket_server_win.h"
#else
#include "debug_router/native/socket/posix/socket_server_posix.h"
#endif

#include <algorithm>

#include "debug_router/native/core/util.h"
#include "debug_router/native/thread/debug_router_executor.h"

namespace debugrouter {
namespace socket_server {

std::shared_ptr<SocketServer> SocketServer::CreateSocketServer(
    const std::shared_ptr<SocketServerConnectionListener> &listener) {
#ifdef _WIN32
  return std::make_shared<SocketServerWin>(listener);
#else
  return std::make_shared<SocketServerPosix>(listener);
#endif
}

SocketServer::SocketServer(
    const std::shared_ptr<SocketServerConnectionListener> &listener)
    : listener_(listener) {}

bool SocketServer::Send(const std::string &message) {
  std::vector<std::shared_ptr<UsbClient>> clients;
  {
    std::lock_guard<std::mutex> lock(clients_lock_);
    clients = usb_clients_;
  }
  if (clients.empty()) {
    LOGI("SocketServerApi Send: clients is empty.");
    return false;
  }
  bool sent = false;
  for (const auto &client : clients) {
    if (client) {
      sent = client->Send(message) || sent;
    }
  }
  return sent;
}

void SocketServer::HandleOnOpenStatus(std::shared_ptr<UsbClient> client,
                                      int32_t code, const std::string &reason) {
  thread::DebugRouterExecutor::GetInstance().Post([=]() {
    bool should_notify_connected = false;
    {
      std::lock_guard<std::mutex> lock(clients_lock_);
      auto it = std::find(usb_clients_.begin(), usb_clients_.end(), client);
      if (it == usb_clients_.end()) {
        should_notify_connected = usb_clients_.empty();
        usb_clients_.push_back(client);
      }
      LOGI("SocketServerApi OnOpen: active client count:"
           << usb_clients_.size());
    }
    if (should_notify_connected) {
      if (auto listener = listener_.lock()) {
        listener->OnStatusChanged(kConnected, code, reason);
      }
    }
  });
}

void SocketServer::HandleOnMessageStatus(std::shared_ptr<UsbClient> client,
                                         const std::string &message) {
  thread::DebugRouterExecutor::GetInstance().Post([=]() {
    bool is_active_client = false;
    {
      std::lock_guard<std::mutex> lock(clients_lock_);
      is_active_client = std::find(usb_clients_.begin(), usb_clients_.end(),
                                   client) != usb_clients_.end();
    }
    if (!is_active_client) {
      LOGI("SocketServerApi OnMessage: client is not active.");
      return;
    }
    if (auto listener = listener_.lock()) {
      listener->OnMessage(message);
    }
  });
}

void SocketServer::HandleOnCloseStatus(std::shared_ptr<UsbClient> client,
                                       ConnectionStatus status, int32_t code,
                                       const std::string &reason) {
  thread::DebugRouterExecutor::GetInstance().Post([=]() {
    bool should_notify_disconnected = false;
    bool is_active_client = false;
    {
      std::lock_guard<std::mutex> lock(clients_lock_);
      auto it = std::find(usb_clients_.begin(), usb_clients_.end(), client);
      if (it != usb_clients_.end()) {
        is_active_client = true;
        usb_clients_.erase(it);
        should_notify_disconnected = usb_clients_.empty();
      }
      LOGI("SocketServerApi OnClose: active client count:"
           << usb_clients_.size());
    }
    if (!is_active_client) {
      LOGI("SocketServerApi OnClose: client is not active.");
      client->Stop();
      return;
    }
    LOGI("SocketServerApi HandleOnCloseStatus: close client for OnClose.");
    client->Stop();
    if (should_notify_disconnected) {
      if (auto listener = listener_.lock()) {
        listener->OnStatusChanged(status, code, reason);
      }
    }
  });
}

void SocketServer::HandleOnErrorStatus(std::shared_ptr<UsbClient> client,
                                       ConnectionStatus status, int32_t code,
                                       const std::string &reason) {
  thread::DebugRouterExecutor::GetInstance().Post([=]() {
    bool should_notify_error = false;
    bool is_active_client = false;
    {
      std::lock_guard<std::mutex> lock(clients_lock_);
      auto it = std::find(usb_clients_.begin(), usb_clients_.end(), client);
      if (it != usb_clients_.end()) {
        is_active_client = true;
        usb_clients_.erase(it);
        should_notify_error = usb_clients_.empty();
      }
      LOGI("SocketServerApi OnError: active client count:"
           << usb_clients_.size());
    }
    if (!is_active_client) {
      LOGI("SocketServerApi OnError: client is not active.");
      client->Stop();
      return;
    }
    LOGI("SocketServerApi HandleOnErrorStatus: close client for OnError.");
    client->Stop();
    if (should_notify_error) {
      if (auto listener = listener_.lock()) {
        listener->OnStatusChanged(status, code, reason);
      }
    }
  });
}

void SocketServer::NotifyInit(int32_t code, const std::string &info) {
  thread::DebugRouterExecutor::GetInstance().Post([=]() {
    if (auto listener = listener_.lock()) {
      listener->OnInit(code, info);
    }
  });
}

void SocketServer::setEnableServer(bool enable) {
  LOGI("SocketServer::setEnableServer:" << enable);
  // notify only when transition from false to true
  if (!is_running_.exchange(enable, std::memory_order_relaxed) && enable) {
    running_condition_.notify_one();
  }
}

void SocketServer::StartServer() { setEnableServer(true); }

void SocketServer::StopServer() {
  setEnableServer(false);
  // Close socket if it's valid
  if (socket_fd_ != kInvalidSocket) {
#ifdef _WIN32
    shutdown(socket_fd_, SD_BOTH);
#else
    shutdown(socket_fd_, SHUT_RDWR);
#endif
  }

  Close();
  std::vector<std::shared_ptr<UsbClient>> clients;
  {
    std::lock_guard<std::mutex> lock(clients_lock_);
    clients = usb_clients_;
  }
  for (const auto &client : clients) {
    if (client) {
      client->Stop();
    }
  }
}

void SocketServer::ThreadFunc(std::shared_ptr<SocketServer> socket_server) {
  int count = 0;
  while (true) {
    {
      std::unique_lock lock(socket_server->running_mutex_);
      socket_server->running_condition_.wait(lock, [=]() {
        return socket_server->is_running_.load(std::memory_order_relaxed) ==
               true;
      });
    }
    LOGI("Init start:" << count);
    socket_server->Start();
    count++;
  }
}

void SocketServer::Init() {
  std::thread listen_thread(ThreadFunc, shared_from_this());
  listen_thread.detach();
}

// close server socket
void SocketServer::Close() {
  LOGI("SocketServer::Close server socket_fd_:" << socket_fd_);
  CloseSocket(socket_fd_);
  socket_fd_ = kInvalidSocket;
}

void SocketServer::Disconnect() {
  thread::DebugRouterExecutor::GetInstance().Post([=]() {
    std::vector<std::shared_ptr<UsbClient>> clients;
    {
      std::lock_guard<std::mutex> lock(clients_lock_);
      clients = usb_clients_;
    }
    if (!clients.empty()) {
      LOGI("SocketServerApi Disconnect: stop all clients.");
      for (const auto &client : clients) {
        if (client) {
          client->Stop();
        }
      }
    }
  });
}

SocketServer::~SocketServer() {
  LOGI("SocketServer::~SocketServer");
  std::vector<std::shared_ptr<UsbClient>> clients;
  {
    std::lock_guard<std::mutex> lock(clients_lock_);
    clients.swap(usb_clients_);
  }
  for (const auto &client : clients) {
    if (client) {
      client->Stop();
    }
  }
  Close();
}

}  // namespace socket_server
}  // namespace debugrouter
