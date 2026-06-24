// Copyright 2023 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "debug_router/native/socket/socket_server_type.h"
#ifdef _WIN32
#include <winsock2.h>

#include "debug_router/native/socket/win/socket_server_win.h"
#else
#include <unistd.h>

#include "debug_router/native/socket/posix/socket_server_posix.h"
#endif
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
  return Broadcast(message);
}

bool SocketServer::Send(const std::shared_ptr<UsbClient> &client,
                        const std::string &message) {
  if (!client) {
    LOGI("SocketServerApi Send: target client is null.");
    return false;
  }
  if (!HasActiveClient(client)) {
    LOGI("SocketServerApi Send: target client is not active.");
    return false;
  }
  return client->Send(message);
}

bool SocketServer::Broadcast(const std::string &message) {
  auto clients = ActiveClientsSnapshot();
  if (clients.empty()) {
    LOGI("SocketServerApi Broadcast: no active clients.");
    return false;
  }

  bool sent = false;
  for (const auto &client : clients) {
    if (client && client->Send(message)) {
      sent = true;
    }
  }
  return sent;
}

void SocketServer::AddPendingClient(const std::shared_ptr<UsbClient> &client) {
  if (!client) {
    return;
  }
  std::lock_guard<std::mutex> lock(clients_mutex_);
  pending_clients_.insert(client);
}

void SocketServer::HandleOnOpenStatus(std::shared_ptr<UsbClient> client,
                                      int32_t code, const std::string &reason) {
  if (!client) {
    return;
  }
  thread::DebugRouterExecutor::GetInstance().Post([=]() {
    if (!is_running_.load(std::memory_order_relaxed)) {
      LOGI("SocketServerApi OnOpen: server is not running.");
      client->Stop();
      return;
    }
    bool should_notify_connected = false;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      pending_clients_.erase(client);
      should_notify_connected = active_clients_.empty();
      active_clients_.insert(client);
    }
    LOGI("SocketServerApi OnOpen: active client connected.");
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
    if (!HasActiveClient(client)) {
      LOGI("SocketServerApi OnMessage: client is not active.");
      return;
    }
    if (auto listener = listener_.lock()) {
      listener->OnMessage(client, message);
    }
  });
}

void SocketServer::HandleOnCloseStatus(std::shared_ptr<UsbClient> client,
                                       ConnectionStatus status, int32_t code,
                                       const std::string &reason) {
  thread::DebugRouterExecutor::GetInstance().Post([=]() {
    bool should_notify_disconnected = false;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      pending_clients_.erase(client);
      size_t removed_count = active_clients_.erase(client);
      should_notify_disconnected =
          removed_count > 0 && active_clients_.empty();
    }
    LOGI("SocketServerApi HandleOnCloseStatus: close client for OnClose.");
    if (client) {
      client->Stop();
    }
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
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      pending_clients_.erase(client);
      size_t removed_count = active_clients_.erase(client);
      should_notify_error = removed_count > 0 && active_clients_.empty();
    }
    LOGI("SocketServerApi HandleOnErrorStatus: close client for OnError.");
    if (client) {
      client->Stop();
    }
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
  StopClients(DrainClients());
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

void SocketServer::CloseSocket(int socket_fd) {
  LOGI("SocketServer::CloseSocket fallback:" << socket_fd);
  if (socket_fd == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  if (closesocket(socket_fd) != 0) {
    LOGE("close socket error");
  }
#else
  if (close(socket_fd) != 0) {
    LOGE("close socket error");
  }
#endif
}

void SocketServer::Disconnect() {
  thread::DebugRouterExecutor::GetInstance().Post([=]() {
    LOGI("SocketServerApi Disconnect: stop all clients.");
    StopClients(DrainClients());
  });
}

SocketServer::~SocketServer() {
  LOGI("SocketServer::~SocketServer");
  StopClients(DrainClients());
  Close();
}

bool SocketServer::HasActiveClient(const std::shared_ptr<UsbClient> &client) {
  if (!client) {
    return false;
  }
  std::lock_guard<std::mutex> lock(clients_mutex_);
  return active_clients_.find(client) != active_clients_.end();
}

std::vector<std::shared_ptr<UsbClient>> SocketServer::ActiveClientsSnapshot() {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  return std::vector<std::shared_ptr<UsbClient>>(active_clients_.begin(),
                                                 active_clients_.end());
}

std::vector<std::shared_ptr<UsbClient>> SocketServer::DrainClients() {
  std::vector<std::shared_ptr<UsbClient>> clients;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients.reserve(pending_clients_.size() + active_clients_.size());
    clients.insert(clients.end(), pending_clients_.begin(),
                   pending_clients_.end());
    clients.insert(clients.end(), active_clients_.begin(),
                   active_clients_.end());
    pending_clients_.clear();
    active_clients_.clear();
  }
  return clients;
}

void SocketServer::StopClients(std::vector<std::shared_ptr<UsbClient>> clients) {
  for (const auto &client : clients) {
    if (client) {
      client->Stop();
    }
  }
}

#ifdef TESTING
size_t SocketServer::ActiveClientCountForTest() {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  return active_clients_.size();
}

size_t SocketServer::PendingClientCountForTest() {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  return pending_clients_.size();
}
#endif

}  // namespace socket_server
}  // namespace debugrouter
