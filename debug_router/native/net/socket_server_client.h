// Copyright 2023 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef DEBUGROUTER_NATIVE_NET_SOCKET_SERVER_CLIENT_H_
#define DEBUGROUTER_NATIVE_NET_SOCKET_SERVER_CLIENT_H_

#include "debug_router/native/core/message_transceiver.h"
#include "debug_router/native/socket/socket_server_api.h"

namespace debugrouter {
namespace net {
class SocketServerClientContext : public core::MessageTransceiverContext {
 public:
  explicit SocketServerClientContext(
      std::shared_ptr<debugrouter::socket_server::UsbClient> socket_client);
  const void *GetTypeId() const override;
  const void *GetContextKey() const override;
  std::shared_ptr<debugrouter::socket_server::UsbClient> GetSocketClient()
      const;
  static const void *ContextTypeId();

 private:
  std::weak_ptr<debugrouter::socket_server::UsbClient> socket_client_;
};

class SocketServerClient : public core::MessageTransceiver {
 public:
  SocketServerClient();
  virtual ~SocketServerClient() = default;
  void Init() override;
  bool Connect(const std::string &url) override;
  void Disconnect() override;
  void Send(const std::string &data) override;
  void Send(const std::string &data,
            const std::shared_ptr<core::MessageTransceiverContext> &context)
      override;
  core::ConnectionType GetType() override;
  void HandleReceivedMessage(const std::string &message) override;

  void StartServer() override;
  void StopServer() override;

#ifdef TESTING
  std::shared_ptr<debugrouter::socket_server::UsbClient>
  GetSocketClientFromContextForTest(
      const std::shared_ptr<core::MessageTransceiverContext> &context);
  void SetSocketServerForTest(
      const std::shared_ptr<debugrouter::socket_server::SocketServer>
          &socket_server);
#endif

 private:
  std::shared_ptr<debugrouter::socket_server::UsbClient>
  GetSocketClientFromContext(
      const std::shared_ptr<core::MessageTransceiverContext> &context);

  std::shared_ptr<debugrouter::socket_server::SocketServer> socket_server_;
  std::shared_ptr<debugrouter::socket_server::SocketServerConnectionListener>
      listener_;
};

}  // namespace net
}  // namespace debugrouter

#endif  // DEBUGROUTER_NATIVE_NET_SOCKET_SERVER_CLIENT_H_
