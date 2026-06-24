// Copyright 2024 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "debug_router/native/core/debug_router_core.h"
#include "debug_router/native/core/debug_router_global_handler.h"
#include "debug_router/native/core/debug_router_session_handler.h"
#include "debug_router/native/protocol/protocol.h"
#include "gtest/gtest.h"
#include "json/reader.h"

namespace debugrouter {
namespace core {

class TestGlobalHandler : public DebugRouterGlobalHandler {
 public:
  virtual ~TestGlobalHandler() = default;
  void OpenCard(const std::string &url) override {}
  void OnMessage(const std::string &message, const std::string &type) override {
  }
};

class TestSessionHandler : public DebugRouterSessionHandler {
 public:
  virtual ~TestSessionHandler() = default;
  void OnSessionCreate(int session_id, const std::string &url) override {}
  void OnSessionDestroy(int session_id) override {}
  void OnMessage(const std::string &message, const std::string &type,
                 int session_id) override {}
};

class BlockingMessageHandler final : public DebugRouterMessageHandler {
 public:
  std::string Handle(std::string params) override {
    std::unique_lock<std::mutex> lock(mutex_);
    int order = ++entered_count_;
    entered_cv_.notify_all();
    release_cv_.wait(lock, [&]() { return release_count_ >= order; });
    return "{\"ok\":true}";
  }

  std::string GetName() const override { return "BlockingMethod"; }

  void WaitForEnteredCount(int count) {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_cv_.wait(lock, [&]() { return entered_count_ >= count; });
  }

  void ReleaseNext() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++release_count_;
    }
    release_cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable entered_cv_;
  std::condition_variable release_cv_;
  int entered_count_ = 0;
  int release_count_ = 0;
};

class TestNativeSlot final : public NativeSlot {
 public:
  TestNativeSlot() : NativeSlot("test", "test://slot") {}
  void OnMessage(const std::string &message,
                 const std::string &type) override {}
};

class TestMessageContext final : public MessageTransceiverContext {};

class ContextRecordingTransceiver final : public MessageTransceiver {
 public:
  bool Connect(const std::string &url) override { return false; }
  void Disconnect() override {}
  void Send(const std::string &data) override {
    std::lock_guard<std::mutex> lock(send_mutex);
    legacy_sent = data;
  }
  void Send(const std::string &data,
            const std::shared_ptr<MessageTransceiverContext> &context)
      override {
    std::lock_guard<std::mutex> lock(send_mutex);
    context_sent = data;
    sent_context = context;
    context_sends.push_back({data, context});
  }
  ConnectionType GetType() override { return ConnectionType::kUsb; }
  void StartServer() override {}
  void StopServer() override {}

  std::string legacy_sent;
  std::string context_sent;
  std::shared_ptr<MessageTransceiverContext> sent_context;
  std::vector<
      std::pair<std::string, std::shared_ptr<MessageTransceiverContext>>>
      context_sends;
  std::mutex send_mutex;
};

class DebugRouterCoreConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    core_ = &DebugRouterCore::GetInstance();
    ClearSlots();
    ClearGlobalHandlers();
    ClearSessionHandlers();
    ClearMessageHandlers();
    core_->ClearProcessorContexts();
  }

  void TearDown() override {
    ClearSlots();
    ClearGlobalHandlers();
    ClearSessionHandlers();
    ClearMessageHandlers();
    core_->ClearProcessorContexts();
  }

  void ClearSlots() {
    std::unique_lock lock(core_->slots_mutex_);
    core_->slots_.clear();
    core_->max_session_id_ = 0;
  }

  void ClearGlobalHandlers() {
    std::unique_lock lock(core_->global_handler_mutex_);
    core_->global_handler_map_.clear();
  }

  size_t GetGlobalHandlerCount() {
    std::shared_lock lock(core_->global_handler_mutex_);
    return core_->global_handler_map_.size();
  }

  void ClearSessionHandlers() {
    std::unique_lock lock(core_->session_handler_mutex_);
    core_->session_handler_map_.clear();
  }

  void ClearMessageHandlers() {
    std::unique_lock lock(core_->message_handler_mutex_);
    core_->message_handlers_.clear();
  }

  size_t GetMessageHandlerCount() {
    std::shared_lock lock(core_->message_handler_mutex_);
    return core_->message_handlers_.size();
  }

  size_t GetSessionHandlerCount() {
    std::shared_lock lock(core_->session_handler_mutex_);
    return core_->session_handler_map_.size();
  }

  std::shared_ptr<MessageTransceiver> GetCurrentTransceiver() {
    return core_->current_transceiver_;
  }

  void SetCurrentTransceiver(
      const std::shared_ptr<MessageTransceiver> &transceiver) {
    core_->current_transceiver_ = transceiver;
  }

  ConnectionState GetCurrentConnectionState() {
    return core_->connection_state_.load(std::memory_order_relaxed);
  }

  void SetCurrentConnectionState(ConnectionState state) {
    core_->connection_state_.store(state, std::memory_order_relaxed);
  }

  DebugRouterCore *core_;
};

std::string InitMessage(protocol::RemoteDebugPrococolClientId client_id) {
  return protocol::RemoteDebugProtocol::Stringify(
      protocol::RemoteDebugProtocol::CreateProtocolBody4Init(client_id));
}

std::string AppActionMessage(protocol::RemoteDebugPrococolClientId client_id,
                             int32_t message_id) {
  Json::Value root(Json::objectValue);
  root[protocol::kKeyEvent] = protocol::kRemoteDebugServerEvent4Custom;
  root[protocol::kKeyData][protocol::kKeyType] =
      protocol::kRemoteDebugProtocolBodyData4Custom4MessageHandler;
  root[protocol::kKeyData][protocol::kKeySender] = client_id;
  root[protocol::kKeyData][protocol::kKeyData][protocol::kKeyClientId] =
      client_id;
  root[protocol::kKeyData][protocol::kKeyData][protocol::kKeyMessage]
      [protocol::kKeyMethod] = "BlockingMethod";
  root[protocol::kKeyData][protocol::kKeyData][protocol::kKeyMessage]
      [protocol::kKeyId] = message_id;
  root[protocol::kKeyData][protocol::kKeyData][protocol::kKeyMessage]
      [protocol::kKeyParams]["request_id"] = message_id;
  return root.toStyledString();
}

TEST_F(DebugRouterCoreConcurrencyTest, SendUsesTransceiverContextWhenProvided) {
  auto previous_transceiver = GetCurrentTransceiver();
  ConnectionState previous_state = GetCurrentConnectionState();
  auto transceiver = std::make_shared<ContextRecordingTransceiver>();
  auto context = std::make_shared<TestMessageContext>();
  SetCurrentTransceiver(transceiver);
  SetCurrentConnectionState(CONNECTED);

  core_->Send("context payload", context);

  SetCurrentTransceiver(previous_transceiver);
  SetCurrentConnectionState(previous_state);
  EXPECT_EQ(transceiver->context_sent, "context payload");
  EXPECT_EQ(transceiver->sent_context, context);
  EXPECT_TRUE(transceiver->legacy_sent.empty());
}

TEST_F(DebugRouterCoreConcurrencyTest, OnMessageUsesPerTransceiverContextState) {
  auto previous_transceiver = GetCurrentTransceiver();
  ConnectionState previous_state = GetCurrentConnectionState();
  auto transceiver = std::make_shared<ContextRecordingTransceiver>();
  auto first_context = std::make_shared<TestMessageContext>();
  auto second_context = std::make_shared<TestMessageContext>();
  SetCurrentTransceiver(transceiver);
  SetCurrentConnectionState(CONNECTED);

  core_->OnMessage(InitMessage(401), transceiver, first_context);
  core_->OnMessage(InitMessage(402), transceiver, second_context);

  EXPECT_EQ(core_->TransceiverContextCountForTest(), 2U);
  EXPECT_EQ(core_->GetProcessorContextForTest(first_context).client_id, 401U);
  EXPECT_EQ(core_->GetProcessorContextForTest(second_context).client_id, 402U);

  core_->OnClosed(transceiver);
  EXPECT_EQ(core_->TransceiverContextCountForTest(), 0U);
  SetCurrentTransceiver(previous_transceiver);
  SetCurrentConnectionState(previous_state);
}

TEST_F(DebugRouterCoreConcurrencyTest, SendDataBroadcastsPerProcessorContext) {
  auto previous_transceiver = GetCurrentTransceiver();
  ConnectionState previous_state = GetCurrentConnectionState();
  auto transceiver = std::make_shared<ContextRecordingTransceiver>();
  auto first_context = std::make_shared<TestMessageContext>();
  auto second_context = std::make_shared<TestMessageContext>();
  SetCurrentTransceiver(transceiver);
  SetCurrentConnectionState(CONNECTED);
  core_->GetProcessorContextForTest(first_context).client_id = 501;
  core_->GetProcessorContextForTest(second_context).client_id = 502;

  core_->SendData("payload", protocol::kRemoteDebugProtocolBodyData4CDP, 7, -1,
                  false);

  ASSERT_EQ(transceiver->context_sends.size(), 2U);
  std::unordered_map<std::shared_ptr<MessageTransceiverContext>, uint32_t>
      sent_client_ids;
  for (const auto &send : transceiver->context_sends) {
    Json::Value root;
    Json::Reader reader;
    ASSERT_TRUE(reader.parse(send.first, root));
    sent_client_ids[send.second] =
        root[protocol::kKeyData][protocol::kKeySender].asUInt();
  }
  EXPECT_EQ(sent_client_ids[first_context], 501U);
  EXPECT_EQ(sent_client_ids[second_context], 502U);

  core_->OnClosed(transceiver);
  SetCurrentTransceiver(previous_transceiver);
  SetCurrentConnectionState(previous_state);
}

TEST_F(DebugRouterCoreConcurrencyTest, ContextCloseRemovesProcessorContext) {
  auto previous_transceiver = GetCurrentTransceiver();
  ConnectionState previous_state = GetCurrentConnectionState();
  auto transceiver = std::make_shared<ContextRecordingTransceiver>();
  auto first_context = std::make_shared<TestMessageContext>();
  auto second_context = std::make_shared<TestMessageContext>();
  SetCurrentTransceiver(transceiver);
  SetCurrentConnectionState(CONNECTED);
  core_->GetProcessorContextForTest(first_context).client_id = 701;
  core_->GetProcessorContextForTest(second_context).client_id = 702;

  core_->OnContextClosed(transceiver, first_context);
  EXPECT_EQ(core_->TransceiverContextCountForTest(), 1U);
  core_->SendData("payload", protocol::kRemoteDebugProtocolBodyData4CDP, 7, -1,
                  false);

  ASSERT_EQ(transceiver->context_sends.size(), 1U);
  EXPECT_EQ(transceiver->context_sends[0].second, second_context);
  Json::Value root;
  Json::Reader reader;
  ASSERT_TRUE(reader.parse(transceiver->context_sends[0].first, root));
  EXPECT_EQ(root[protocol::kKeyData][protocol::kKeySender].asUInt(), 702U);

  core_->OnClosed(transceiver);
  SetCurrentTransceiver(previous_transceiver);
  SetCurrentConnectionState(previous_state);
}

TEST_F(DebugRouterCoreConcurrencyTest,
       DisconnectMarksDisconnectedAndClearsContexts) {
  auto previous_transceiver = GetCurrentTransceiver();
  ConnectionState previous_state = GetCurrentConnectionState();
  auto transceiver = std::make_shared<ContextRecordingTransceiver>();
  auto context = std::make_shared<TestMessageContext>();
  SetCurrentTransceiver(transceiver);
  SetCurrentConnectionState(CONNECTED);
  core_->GetProcessorContextForTest(context).client_id = 704;
  ASSERT_EQ(core_->TransceiverContextCountForTest(), 1U);

  core_->Disconnect();

  EXPECT_EQ(GetCurrentConnectionState(), DISCONNECTED);
  EXPECT_EQ(GetCurrentTransceiver(), nullptr);
  EXPECT_EQ(core_->TransceiverContextCountForTest(), 0U);

  SetCurrentTransceiver(previous_transceiver);
  SetCurrentConnectionState(previous_state);
}

TEST_F(DebugRouterCoreConcurrencyTest, ConcurrentSendDataAndContextUpdates) {
  auto previous_transceiver = GetCurrentTransceiver();
  ConnectionState previous_state = GetCurrentConnectionState();
  auto transceiver = std::make_shared<ContextRecordingTransceiver>();
  auto context = std::make_shared<TestMessageContext>();
  SetCurrentTransceiver(transceiver);
  SetCurrentConnectionState(CONNECTED);

  std::atomic<bool> start(false);
  std::thread updater([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (uint32_t i = 0; i < 32; ++i) {
      core_->OnMessage(InitMessage(800 + i), transceiver, context);
    }
  });
  std::thread sender([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (uint32_t i = 0; i < 32; ++i) {
      core_->SendData("payload", protocol::kRemoteDebugProtocolBodyData4CDP, 7,
                      -1, false);
    }
  });

  start.store(true, std::memory_order_release);
  updater.join();
  sender.join();

  EXPECT_EQ(core_->TransceiverContextCountForTest(), 1U);
  EXPECT_GE(core_->GetProcessorContextForTest(context).client_id, 800U);

  core_->OnClosed(transceiver);
  SetCurrentTransceiver(previous_transceiver);
  SetCurrentConnectionState(previous_state);
}

TEST_F(DebugRouterCoreConcurrencyTest,
       ConcurrentRepliesUseThreadLocalMessageContext) {
  auto previous_transceiver = GetCurrentTransceiver();
  ConnectionState previous_state = GetCurrentConnectionState();
  auto transceiver = std::make_shared<ContextRecordingTransceiver>();
  auto first_context = std::make_shared<TestMessageContext>();
  auto second_context = std::make_shared<TestMessageContext>();
  BlockingMessageHandler handler;
  core_->AddMessageHandler(&handler);
  SetCurrentTransceiver(transceiver);
  SetCurrentConnectionState(CONNECTED);
  core_->GetProcessorContextForTest(first_context).client_id = 901;
  core_->GetProcessorContextForTest(second_context).client_id = 902;

  std::thread first([&]() {
    core_->OnMessage(AppActionMessage(901, 1), transceiver, first_context);
  });
  handler.WaitForEnteredCount(1);
  std::thread second([&]() {
    core_->OnMessage(AppActionMessage(902, 2), transceiver, second_context);
  });
  handler.WaitForEnteredCount(2);

  handler.ReleaseNext();
  first.join();
  handler.ReleaseNext();
  second.join();

  ASSERT_EQ(transceiver->context_sends.size(), 2U);
  std::unordered_map<uint32_t, std::shared_ptr<MessageTransceiverContext>>
      sent_contexts_by_sender;
  for (const auto &send : transceiver->context_sends) {
    Json::Value root;
    Json::Reader reader;
    ASSERT_TRUE(reader.parse(send.first, root));
    sent_contexts_by_sender[root[protocol::kKeyData][protocol::kKeySender]
                                .asUInt()] = send.second;
  }
  EXPECT_EQ(sent_contexts_by_sender[901], first_context);
  EXPECT_EQ(sent_contexts_by_sender[902], second_context);

  core_->OnClosed(transceiver);
  SetCurrentTransceiver(previous_transceiver);
  SetCurrentConnectionState(previous_state);
}

TEST_F(DebugRouterCoreConcurrencyTest, MissingAppActionDoesNotMutateHandlers) {
  auto previous_transceiver = GetCurrentTransceiver();
  ConnectionState previous_state = GetCurrentConnectionState();
  auto transceiver = std::make_shared<ContextRecordingTransceiver>();
  auto context = std::make_shared<TestMessageContext>();
  SetCurrentTransceiver(transceiver);
  SetCurrentConnectionState(CONNECTED);
  core_->GetProcessorContextForTest(context).client_id = 903;

  core_->OnMessage(AppActionMessage(903, 1), transceiver, context);

  EXPECT_EQ(GetMessageHandlerCount(), 0U);
  ASSERT_EQ(transceiver->context_sends.size(), 1U);
  EXPECT_EQ(transceiver->context_sends[0].second, context);

  core_->OnClosed(transceiver);
  SetCurrentTransceiver(previous_transceiver);
  SetCurrentConnectionState(previous_state);
}

TEST_F(DebugRouterCoreConcurrencyTest,
       PlugBroadcastsSessionListPerProcessorContext) {
  auto previous_transceiver = GetCurrentTransceiver();
  ConnectionState previous_state = GetCurrentConnectionState();
  auto transceiver = std::make_shared<ContextRecordingTransceiver>();
  auto first_context = std::make_shared<TestMessageContext>();
  auto second_context = std::make_shared<TestMessageContext>();
  SetCurrentTransceiver(transceiver);
  SetCurrentConnectionState(CONNECTED);
  core_->GetProcessorContextForTest(first_context).client_id = 601;
  core_->GetProcessorContextForTest(second_context).client_id = 602;

  core_->Plug(std::make_shared<TestNativeSlot>());

  ASSERT_EQ(transceiver->context_sends.size(), 2U);
  std::unordered_map<std::shared_ptr<MessageTransceiverContext>, uint32_t>
      sent_client_ids;
  for (const auto &send : transceiver->context_sends) {
    Json::Value root;
    Json::Reader reader;
    ASSERT_TRUE(reader.parse(send.first, root));
    sent_client_ids[send.second] =
        root[protocol::kKeyData][protocol::kKeySender].asUInt();
  }
  EXPECT_EQ(sent_client_ids[first_context], 601U);
  EXPECT_EQ(sent_client_ids[second_context], 602U);

  core_->OnClosed(transceiver);
  SetCurrentTransceiver(previous_transceiver);
  SetCurrentConnectionState(previous_state);
}

TEST_F(DebugRouterCoreConcurrencyTest, ConcurrentAddSameGlobalHandler) {
  TestGlobalHandler handler;
  std::vector<std::thread> threads;
  std::vector<int> returned_ids;
  std::mutex ids_mutex;
  const size_t kNumThreads = 10;

  for (size_t i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, this]() {
      int id = core_->AddGlobalHandler(&handler);
      std::lock_guard<std::mutex> lock(ids_mutex);
      returned_ids.push_back(id);
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(returned_ids.size(), kNumThreads);

  int first_id = returned_ids[0];
  for (int id : returned_ids) {
    EXPECT_EQ(id, first_id);
  }

  EXPECT_EQ(GetGlobalHandlerCount(), static_cast<size_t>(1));
}

TEST_F(DebugRouterCoreConcurrencyTest, ConcurrentAddDifferentGlobalHandlers) {
  std::vector<std::unique_ptr<TestGlobalHandler>> handlers;
  for (int i = 0; i < 10; ++i) {
    handlers.push_back(std::make_unique<TestGlobalHandler>());
  }

  std::vector<std::thread> threads;
  const size_t kNumThreads = 10;

  for (size_t i = 0; i < kNumThreads; ++i) {
    threads.emplace_back(
        [&, i, this]() { core_->AddGlobalHandler(handlers[i].get()); });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(GetGlobalHandlerCount(), kNumThreads);
}

TEST_F(DebugRouterCoreConcurrencyTest, ConcurrentRemoveGlobalHandler) {
  TestGlobalHandler handler;

  int handler_id = core_->AddGlobalHandler(&handler);

  std::vector<std::thread> threads;
  std::atomic<int> success_count(0);
  const size_t kNumThreads = 10;

  for (size_t i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, handler_id, this]() {
      if (core_->RemoveGlobalHandler(handler_id)) {
        success_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(std::memory_order_relaxed), 1);

  EXPECT_EQ(GetGlobalHandlerCount(), static_cast<size_t>(0));
}

TEST_F(DebugRouterCoreConcurrencyTest, ConcurrentAddAndRemoveGlobalHandlers) {
  std::vector<std::unique_ptr<TestGlobalHandler>> handlers;
  for (int i = 0; i < 5; ++i) {
    handlers.push_back(std::make_unique<TestGlobalHandler>());
  }

  std::vector<int> handler_ids;
  for (auto &h : handlers) {
    handler_ids.push_back(core_->AddGlobalHandler(h.get()));
  }

  std::vector<std::thread> threads;
  const int kNumThreads = 20;

  for (size_t i = 0; i < kNumThreads; ++i) {
    if (i % 2 == 0) {
      threads.emplace_back([&, i, this]() {
        int idx = i % handlers.size();
        core_->AddGlobalHandler(handlers[idx].get());
      });
    } else {
      threads.emplace_back([&, i, this]() {
        int idx = i % handler_ids.size();
        core_->RemoveGlobalHandler(handler_ids[idx]);
      });
    }
  }

  for (auto &t : threads) {
    t.join();
  }

  // Lightweight assertions to ensure final handler count is within reasonable
  // bounds
  size_t final_count = GetGlobalHandlerCount();
  // At least 0 (always true, but explicitly written as documentation)
  EXPECT_GE(final_count, static_cast<size_t>(0));
  // At most: initial 5 + 10 adds = 15 (different threads might add same
  // handler, but no duplicate counting) In reality, since same handler is not
  // added repeatedly, at most 5
  EXPECT_LE(final_count,
            static_cast<size_t>(handlers.size() + kNumThreads / 2));
}

TEST_F(DebugRouterCoreConcurrencyTest, ConcurrentAddSameSessionHandler) {
  TestSessionHandler handler;
  std::vector<std::thread> threads;
  std::vector<int> returned_ids;
  std::mutex ids_mutex;
  const size_t kNumThreads = 10;

  for (size_t i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, this]() {
      int id = core_->AddSessionHandler(&handler);
      std::lock_guard<std::mutex> lock(ids_mutex);
      returned_ids.push_back(id);
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(returned_ids.size(), kNumThreads);

  int first_id = returned_ids[0];
  for (int id : returned_ids) {
    EXPECT_EQ(id, first_id);
  }

  EXPECT_EQ(GetSessionHandlerCount(), static_cast<size_t>(1));
}

TEST_F(DebugRouterCoreConcurrencyTest, ConcurrentAddDifferentSessionHandlers) {
  std::vector<std::unique_ptr<TestSessionHandler>> handlers;
  for (int i = 0; i < 10; ++i) {
    handlers.push_back(std::make_unique<TestSessionHandler>());
  }

  std::vector<std::thread> threads;
  const size_t kNumThreads = 10;

  for (size_t i = 0; i < kNumThreads; ++i) {
    threads.emplace_back(
        [&, i, this]() { core_->AddSessionHandler(handlers[i].get()); });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(GetSessionHandlerCount(), kNumThreads);
}

TEST_F(DebugRouterCoreConcurrencyTest, ConcurrentRemoveSessionHandler) {
  TestSessionHandler handler;

  int handler_id = core_->AddSessionHandler(&handler);

  std::vector<std::thread> threads;
  std::atomic<int> success_count(0);
  const size_t kNumThreads = 10;

  for (size_t i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, handler_id, this]() {
      if (core_->RemoveSessionHandler(handler_id)) {
        success_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(std::memory_order_relaxed), 1);

  EXPECT_EQ(GetSessionHandlerCount(), static_cast<size_t>(0));
}

TEST_F(DebugRouterCoreConcurrencyTest, ConcurrentAddAndRemoveSessionHandlers) {
  std::vector<std::unique_ptr<TestSessionHandler>> handlers;
  for (int i = 0; i < 5; ++i) {
    handlers.push_back(std::make_unique<TestSessionHandler>());
  }

  std::vector<int> handler_ids;
  for (auto &h : handlers) {
    handler_ids.push_back(core_->AddSessionHandler(h.get()));
  }

  std::vector<std::thread> threads;
  const int kNumThreads = 20;

  for (size_t i = 0; i < kNumThreads; ++i) {
    if (i % 2 == 0) {
      threads.emplace_back([&, i, this]() {
        int idx = i % handlers.size();
        core_->AddSessionHandler(handlers[idx].get());
      });
    } else {
      threads.emplace_back([&, i, this]() {
        int idx = i % handler_ids.size();
        core_->RemoveSessionHandler(handler_ids[idx]);
      });
    }
  }

  for (auto &t : threads) {
    t.join();
  }

  // Lightweight assertions to ensure final handler count is within reasonable
  // bounds
  size_t final_count = GetSessionHandlerCount();
  // At least 0 (always true, but explicitly written as documentation)
  EXPECT_GE(final_count, static_cast<size_t>(0));
  // At most: initial 5 + 10 adds = 15 (different threads might add same
  // handler, but no duplicate counting) In reality, since same handler is not
  // added repeatedly, at most 5
  EXPECT_LE(final_count,
            static_cast<size_t>(handlers.size() + kNumThreads / 2));
}

}  // namespace core
}  // namespace debugrouter
