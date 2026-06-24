// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "debug_router/native/net/socket_server_client.h"

#include "debug_router/native/socket/usb_client.h"
#include "gtest/gtest.h"

namespace debugrouter {
namespace net {
namespace {

class UnknownContext final : public core::MessageTransceiverContext {};

TEST(SocketServerClientTestSuite, SocketContextExposesLiveSocketClient) {
  auto socket_client =
      std::make_shared<socket_server::UsbClient>(socket_server::kInvalidSocket);
  auto context = std::make_shared<SocketServerClientContext>(socket_client);

  EXPECT_EQ(context->GetTypeId(), SocketServerClientContext::ContextTypeId());
  EXPECT_EQ(context->GetSocketClient(), socket_client);
}

TEST(SocketServerClientTestSuite, SocketContextDoesNotKeepClientAlive) {
  auto socket_client =
      std::make_shared<socket_server::UsbClient>(socket_server::kInvalidSocket);
  auto context = std::make_shared<SocketServerClientContext>(socket_client);

  socket_client.reset();

  EXPECT_EQ(context->GetSocketClient(), nullptr);
}

TEST(SocketServerClientTestSuite, ResolvesOnlySocketServerClientContext) {
  auto socket_client =
      std::make_shared<socket_server::UsbClient>(socket_server::kInvalidSocket);
  auto socket_context =
      std::make_shared<SocketServerClientContext>(socket_client);
  auto unknown_context = std::make_shared<UnknownContext>();
  SocketServerClient socket_server_client;

  EXPECT_EQ(socket_server_client.GetSocketClientFromContextForTest(
                socket_context),
            socket_client);
  EXPECT_EQ(socket_server_client.GetSocketClientFromContextForTest(
                unknown_context),
            nullptr);
}

}  // namespace
}  // namespace net
}  // namespace debugrouter
