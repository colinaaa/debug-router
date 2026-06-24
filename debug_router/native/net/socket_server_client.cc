// Copyright 2023 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "debug_router/native/net/socket_server_client.h"

#include "debug_router/native/log/logging.h"

namespace debugrouter {
namespace net {

SocketServerClientContext::SocketServerClientContext(
    std::shared_ptr<debugrouter::socket_server::UsbClient> socket_client)
    : socket_client_(socket_client) {}

const void *SocketServerClientContext::GetTypeId() const {
  return ContextTypeId();
}

const void *SocketServerClientContext::GetContextKey() const {
  return GetSocketClient().get();
}

const void *SocketServerClientContext::ContextTypeId() {
  static int type_id = 0;
  return &type_id;
}

std::shared_ptr<debugrouter::socket_server::UsbClient>
SocketServerClientContext::GetSocketClient() const {
  return socket_client_.lock();
}

class ConnectionListener
    : public debugrouter::socket_server::SocketServerConnectionListener {
 public:
  ConnectionListener(std::shared_ptr<core::MessageTransceiver> client)
      : client_(client) {}
  virtual ~ConnectionListener() = default;
  // LOGI error_code here.
  void OnInit(int32_t code, const std::string &info) override {
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
                       int32_t code, const std::string &info) override {
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

  void OnMessage(
      std::shared_ptr<debugrouter::socket_server::UsbClient> socket_client,
      const std::string &message) override {
    (void)socket_client;
    if (auto client = client_.lock()) {
      core::MessageTransceiverDelegate *delegate = client->delegate();
      if (delegate == nullptr) {
        LOGE("OnMessage: delegate == nullptr, client is already offline.");
        return;
      }
      delegate->OnMessage(
          message, client,
          std::make_shared<SocketServerClientContext>(socket_client));
    }
  }

  void OnClientClosed(
      std::shared_ptr<debugrouter::socket_server::UsbClient> socket_client)
      override {
    if (auto client = client_.lock()) {
      core::MessageTransceiverDelegate *delegate = client->delegate();
      if (delegate == nullptr) {
        LOGE("OnClientClosed: delegate == nullptr.");
        return;
      }
      delegate->OnContextClosed(
          client, std::make_shared<SocketServerClientContext>(socket_client));
    }
  }

 private:
  std::weak_ptr<core::MessageTransceiver> client_;
};

SocketServerClient::SocketServerClient() {}

void SocketServerClient::Init() {
  listener_ = std::make_shared<ConnectionListener>(shared_from_this());
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

void SocketServerClient::Send(
    const std::string &data,
    const std::shared_ptr<core::MessageTransceiverContext> &context) {
  if (!context) {
    Send(data);
    return;
  }
  auto socket_client = GetSocketClientFromContext(context);
  if (!socket_server_ || !socket_client) {
    LOGI("SocketServerClient::Send: socket context is no longer active.");
    return;
  }
  socket_server_->Send(socket_client, data);
}

std::shared_ptr<debugrouter::socket_server::UsbClient>
SocketServerClient::GetSocketClientFromContext(
    const std::shared_ptr<core::MessageTransceiverContext> &context) {
  if (!context ||
      context->GetTypeId() != SocketServerClientContext::ContextTypeId()) {
    return nullptr;
  }
  auto *socket_context = static_cast<SocketServerClientContext *>(context.get());
  return socket_context->GetSocketClient();
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

#ifdef TESTING
std::shared_ptr<debugrouter::socket_server::UsbClient>
SocketServerClient::GetSocketClientFromContextForTest(
    const std::shared_ptr<core::MessageTransceiverContext> &context) {
  return GetSocketClientFromContext(context);
}

void SocketServerClient::SetSocketServerForTest(
    const std::shared_ptr<debugrouter::socket_server::SocketServer>
        &socket_server) {
  socket_server_ = socket_server;
}
#endif

}  // namespace net
}  // namespace debugrouter
