// Copyright 2023 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "debug_router/native/net/socket_server_client.h"

#include "debug_router/native/log/logging.h"

namespace debugrouter {
namespace net {

class SocketServerClientContext : public core::MessageTransceiverContext {
 public:
  SocketServerClientContext(
      std::shared_ptr<debugrouter::socket_server::SocketServer> socket_server,
      std::shared_ptr<debugrouter::socket_server::UsbClient> client)
      : socket_server_(socket_server), client_(client) {}

  void Send(const std::string &data) override {
    auto socket_server = socket_server_.lock();
    auto client = client_.lock();
    if (!socket_server || !client) {
      LOGI("SocketServerClientContext::Send target is gone.");
      return;
    }
    socket_server->Send(data, client);
  }

 private:
  std::weak_ptr<debugrouter::socket_server::SocketServer> socket_server_;
  std::weak_ptr<debugrouter::socket_server::UsbClient> client_;
};

class ConnectionListener
    : public debugrouter::socket_server::SocketServerConnectionListener {
 public:
  ConnectionListener(std::shared_ptr<SocketServerClient> client)
      : client_(client) {}
  virtual ~ConnectionListener() = default;
  // LOGI error_code here.
  void OnInit(int32_t code, const std::string &info) {
    LOGI("OnInit: code :" << code << ", info:" << info);
    if (auto client = client_.lock()) {
      core::MessageTransceiverDelegate *delegate = client->delegate();
      if (delegate == nullptr) {
        LOGE("OnInit: delegate == nullptr");
        return;
      }
      delegate->OnInit(client, code, info);
    }
  }

  void OnStatusChanged(debugrouter::socket_server::ConnectionStatus status,
                       int32_t code, const std::string &info) {
    if (auto client = client_.lock()) {
      core::MessageTransceiverDelegate *delegate = client->delegate();
      if (delegate == nullptr) {
        LOGE(
            "OnStatusChanged: delegate == nullptr, client is already offline.");
        return;
      }
      if (status == debugrouter::socket_server::kConnected) {
        LOGI("OnOpen: code :" << code << ", info:" << info);
        delegate->OnOpen(client);
      } else if (status == debugrouter::socket_server::kDisconnected) {
        LOGI("OnClose: code :" << code << ", info:" << info);
        delegate->OnClosed(client);
      } else if (status == debugrouter::socket_server::kError) {
        LOGI("OnError: code :" << code << ", info:" << info);
        delegate->OnFailure(client, info, code);
      }
    }
  }

  void OnMessage(std::shared_ptr<debugrouter::socket_server::UsbClient> source,
                 const std::string &message) {
    if (auto client = client_.lock()) {
      core::MessageTransceiverDelegate *delegate = client->delegate();
      if (delegate == nullptr) {
        LOGE("OnMessage: delegate == nullptr, client is already offline.");
        return;
      }
      delegate->OnMessage(message, client, client->CreateContext(source));
    }
  }

 private:
  std::weak_ptr<SocketServerClient> client_;
};

SocketServerClient::SocketServerClient() {}

void SocketServerClient::Init() {
  listener_ = std::make_shared<ConnectionListener>(
      std::static_pointer_cast<SocketServerClient>(shared_from_this()));
  socket_server_ = socket_server::SocketServer::CreateSocketServer(listener_);
  socket_server_->Init();
}

bool SocketServerClient::Connect(const std::string &url) { return false; }

void SocketServerClient::Disconnect() { socket_server_->Disconnect(); }

core::ConnectionType SocketServerClient::GetType() {
  return core::ConnectionType::kUsb;
}

void SocketServerClient::Send(const std::string &data) {
  socket_server_->Send(data);
}

std::shared_ptr<core::MessageTransceiverContext>
SocketServerClient::CreateContext(
    const std::shared_ptr<socket_server::UsbClient> &client) {
  return std::make_shared<SocketServerClientContext>(socket_server_, client);
}

void SocketServerClient::HandleReceivedMessage(const std::string &message) {
  // empty
}

void SocketServerClient::StartServer() {
  if (socket_server_) {
    socket_server_->StartServer();
  }
}

void SocketServerClient::StopServer() {
  if (socket_server_) {
    socket_server_->StopServer();
  }
}

}  // namespace net
}  // namespace debugrouter
