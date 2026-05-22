// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "debug_router/native/core/debug_router_core.h"
#include "debug_router/native/socket/socket_server_api.h"
#include "debug_router/native/socket/socket_server_type.h"
#include "debug_router/native/socket/usb_client.h"
#include "debug_router/native/thread/debug_router_executor.h"
#include "gtest/gtest.h"

namespace debugrouter {
namespace socket_server {
namespace {

struct StatusEvent {
  ConnectionStatus status;
  int32_t code;
  std::string info;
};

class RecordingSocketServerListener final
    : public SocketServerConnectionListener {
 public:
  void OnInit(int32_t code, const std::string &info) override {
    init_codes.push_back(code);
    init_infos.push_back(info);
  }

  void OnStatusChanged(ConnectionStatus status, int32_t code,
                       const std::string &info) override {
    status_events.push_back({status, code, info});
  }

  void OnMessage(std::shared_ptr<UsbClient> client,
                 const std::string &message) override {
    message_clients.push_back(client);
    messages.push_back(message);
  }

  std::vector<int32_t> init_codes;
  std::vector<std::string> init_infos;
  std::vector<StatusEvent> status_events;
  std::vector<std::string> messages;
  std::vector<std::shared_ptr<UsbClient>> message_clients;
};

class TestSocketServer final : public SocketServer {
 public:
  explicit TestSocketServer(
      const std::shared_ptr<SocketServerConnectionListener> &listener)
      : SocketServer(listener) {}

  size_t ActiveClientCount() {
    std::lock_guard<std::mutex> lock(clients_lock_);
    return usb_clients_.size();
  }

 private:
  void Start() override {}
  int GetErrorMessage() override { return 0; }
  void CloseSocket(int socket_fd) override {}
};

std::shared_ptr<UsbClient> MakeClient() {
  return std::make_shared<UsbClient>(kInvalidSocket);
}

std::vector<std::shared_ptr<UsbClient>> MakeClients(size_t count) {
  std::vector<std::shared_ptr<UsbClient>> clients;
  clients.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    clients.push_back(MakeClient());
  }
  return clients;
}

template <typename Callback>
void RunInParallel(size_t count, Callback callback) {
  std::vector<std::thread> threads;
  threads.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    threads.emplace_back([&, i]() { callback(i); });
  }
  for (auto &thread : threads) {
    thread.join();
  }
}

void DrainDebugRouterExecutor() {
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;

  thread::DebugRouterExecutor::GetInstance().Post([&]() {
    std::lock_guard<std::mutex> lock(mutex);
    done = true;
    condition.notify_one();
  });

  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2),
                                 [&]() { return done; }));
}

class SocketServerMultiplexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    core::DebugRouterCore::GetInstance();
    listener_ = std::make_shared<RecordingSocketServerListener>();
    server_ = std::shared_ptr<TestSocketServer>(new TestSocketServer(listener_),
                                                [](TestSocketServer *) {});
  }

  std::shared_ptr<RecordingSocketServerListener> listener_;
  std::shared_ptr<TestSocketServer> server_;
};

TEST_F(SocketServerMultiplexTest, NotifiesConnectedOnlyForFirstActiveClient) {
  auto first = MakeClient();
  auto second = MakeClient();

  server_->HandleOnOpenStatus(first, 0, "first");
  server_->HandleOnOpenStatus(second, 0, "second");
  DrainDebugRouterExecutor();

  EXPECT_EQ(server_->ActiveClientCount(), 2u);
  ASSERT_EQ(listener_->status_events.size(), 1u);
  EXPECT_EQ(listener_->status_events[0].status, kConnected);
  EXPECT_EQ(listener_->status_events[0].code, 0);
  EXPECT_EQ(listener_->status_events[0].info, "first");
}

TEST_F(SocketServerMultiplexTest, KeepsConnectionOpenUntilLastClientCloses) {
  auto first = MakeClient();
  auto second = MakeClient();
  server_->HandleOnOpenStatus(first, 0, "first");
  server_->HandleOnOpenStatus(second, 0, "second");
  DrainDebugRouterExecutor();

  server_->HandleOnCloseStatus(first, kDisconnected, 1000, "first closed");
  DrainDebugRouterExecutor();

  EXPECT_EQ(server_->ActiveClientCount(), 1u);
  ASSERT_EQ(listener_->status_events.size(), 1u);
  EXPECT_EQ(listener_->status_events[0].status, kConnected);

  server_->HandleOnCloseStatus(second, kDisconnected, 1001, "second closed");
  DrainDebugRouterExecutor();

  EXPECT_EQ(server_->ActiveClientCount(), 0u);
  ASSERT_EQ(listener_->status_events.size(), 2u);
  EXPECT_EQ(listener_->status_events[1].status, kDisconnected);
  EXPECT_EQ(listener_->status_events[1].code, 1001);
  EXPECT_EQ(listener_->status_events[1].info, "second closed");
}

TEST_F(SocketServerMultiplexTest, ForwardsMessagesOnlyFromActiveClients) {
  auto active = MakeClient();
  auto inactive = MakeClient();
  server_->HandleOnOpenStatus(active, 0, "active");
  DrainDebugRouterExecutor();

  server_->HandleOnMessageStatus(inactive, "ignored-message");
  server_->HandleOnMessageStatus(active, "active-message");
  DrainDebugRouterExecutor();

  ASSERT_EQ(listener_->messages.size(), 1u);
  EXPECT_EQ(listener_->messages[0], "active-message");
  ASSERT_EQ(listener_->message_clients.size(), 1u);
  EXPECT_EQ(listener_->message_clients[0], active);
}

TEST_F(SocketServerMultiplexTest,
       HandlesConcurrentOpenMessageAndCloseRequests) {
  constexpr size_t kClientCount = 16;
  auto clients = MakeClients(kClientCount);

  RunInParallel(kClientCount, [&](size_t index) {
    server_->HandleOnOpenStatus(clients[index], 0,
                                "open-" + std::to_string(index));
  });
  DrainDebugRouterExecutor();

  EXPECT_EQ(server_->ActiveClientCount(), kClientCount);
  ASSERT_EQ(listener_->status_events.size(), 1u);
  EXPECT_EQ(listener_->status_events[0].status, kConnected);

  RunInParallel(kClientCount, [&](size_t index) {
    server_->HandleOnMessageStatus(clients[index],
                                   "message-" + std::to_string(index));
  });
  server_->HandleOnMessageStatus(MakeClient(), "ignored-message");
  DrainDebugRouterExecutor();

  std::vector<std::string> expected_messages;
  expected_messages.reserve(kClientCount);
  for (size_t i = 0; i < kClientCount; ++i) {
    expected_messages.push_back("message-" + std::to_string(i));
  }
  std::sort(expected_messages.begin(), expected_messages.end());
  std::sort(listener_->messages.begin(), listener_->messages.end());
  EXPECT_EQ(listener_->messages, expected_messages);

  RunInParallel(kClientCount, [&](size_t index) {
    server_->HandleOnCloseStatus(clients[index], kDisconnected,
                                 static_cast<int32_t>(1000 + index),
                                 "close-" + std::to_string(index));
  });
  DrainDebugRouterExecutor();

  EXPECT_EQ(server_->ActiveClientCount(), 0u);
  ASSERT_EQ(listener_->status_events.size(), 2u);
  EXPECT_EQ(listener_->status_events[1].status, kDisconnected);
}

TEST_F(SocketServerMultiplexTest,
       ErrorOnOneClientDoesNotNotifyUntilLastClientIsGone) {
  auto first = MakeClient();
  auto second = MakeClient();
  server_->HandleOnOpenStatus(first, 0, "first");
  server_->HandleOnOpenStatus(second, 0, "second");
  DrainDebugRouterExecutor();

  server_->HandleOnErrorStatus(first, kError, -10, "first error");
  DrainDebugRouterExecutor();

  EXPECT_EQ(server_->ActiveClientCount(), 1u);
  ASSERT_EQ(listener_->status_events.size(), 1u);
  EXPECT_EQ(listener_->status_events[0].status, kConnected);

  server_->HandleOnErrorStatus(second, kError, -11, "second error");
  DrainDebugRouterExecutor();

  EXPECT_EQ(server_->ActiveClientCount(), 0u);
  ASSERT_EQ(listener_->status_events.size(), 2u);
  EXPECT_EQ(listener_->status_events[1].status, kError);
  EXPECT_EQ(listener_->status_events[1].code, -11);
  EXPECT_EQ(listener_->status_events[1].info, "second error");
}

TEST_F(SocketServerMultiplexTest,
       SendFailsWithoutClientsAndSucceedsWithClients) {
  EXPECT_FALSE(server_->Send("no-client-message"));

  server_->HandleOnOpenStatus(MakeClient(), 0, "connected");
  DrainDebugRouterExecutor();

  EXPECT_TRUE(server_->Send("broadcast-message"));
}

}  // namespace
}  // namespace socket_server
}  // namespace debugrouter
