// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "debug_router/native/core/message_transceiver.h"
#include "debug_router/native/processor/message_handler.h"
#include "debug_router/native/processor/processor.h"
#include "debug_router/native/protocol/protocol.h"
#include "gtest/gtest.h"
#include "json/reader.h"
#include "json/value.h"

namespace debugrouter {
namespace {

class RecordingContext final : public core::MessageTransceiverContext {
 public:
  void Send(const std::string &data) override { sent_messages.push_back(data); }

  std::vector<std::string> sent_messages;
};

class RecordingTransceiver final : public core::MessageTransceiver {
 public:
  bool Connect(const std::string &url) override { return true; }
  void Disconnect() override {}
  void Send(const std::string &data) override { sent_messages.push_back(data); }
  core::ConnectionType GetType() override { return core::ConnectionType::kUsb; }
  void StartServer() override {}
  void StopServer() override {}

  std::vector<std::string> sent_messages;
};

class RecordingDelegate final : public core::MessageTransceiverDelegate {
 public:
  void OnOpen(
      const std::shared_ptr<core::MessageTransceiver> &transceiver) override {}
  void OnClosed(
      const std::shared_ptr<core::MessageTransceiver> &transceiver) override {}
  void OnFailure(const std::shared_ptr<core::MessageTransceiver> &transceiver,
                 const std::string &error_message, int error_code) override {}
  void OnMessage(const std::string &message,
                 const std::shared_ptr<core::MessageTransceiver> &transceiver,
                 const std::shared_ptr<core::MessageTransceiverContext>
                     &context) override {
    received_messages.push_back(message);
    received_contexts.push_back(context);
  }
  void OnInit(const std::shared_ptr<core::MessageTransceiver> &transceiver,
              int32_t code, const std::string &info) override {}

  std::vector<std::string> received_messages;
  std::vector<std::shared_ptr<core::MessageTransceiverContext>>
      received_contexts;
};

struct SentMessage {
  std::string message;
  std::shared_ptr<core::MessageTransceiverContext> context;
};

class RecordingMessageHandler final : public processor::MessageHandler {
 public:
  std::string GetRoomId() override { return room_id; }

  std::unordered_map<std::string, std::string> GetClientInfo() override {
    return client_info;
  }

  std::unordered_map<int, std::string> GetSessionList() override {
    return session_list;
  }

  void OnMessage(const std::string &type, int session_id,
                 const std::string &message) override {
    std::lock_guard<std::mutex> lock(mutex);
    received_types.push_back(type);
    received_session_ids.push_back(session_id);
    received_messages.push_back(message);
  }

  void SendMessage(const std::string &message,
                   const std::shared_ptr<core::MessageTransceiverContext>
                       &context) override {
    std::lock_guard<std::mutex> lock(mutex);
    sent_messages.push_back({message, context});
  }

  void OpenCard(const std::string &url) override {
    std::lock_guard<std::mutex> lock(mutex);
    opened_cards.push_back(url);
  }

  std::string HandleAppAction(const std::string &method,
                              const std::string &params) override {
    std::lock_guard<std::mutex> lock(mutex);
    app_methods.push_back(method);
    app_params.push_back(params);
    return app_result;
  }

  void ChangeRoomServer(const std::string &url,
                        const std::string &room) override {
    std::lock_guard<std::mutex> lock(mutex);
    changed_server_url = url;
    changed_room_id = room;
  }

  void ReportError(const std::string &error) override {
    std::lock_guard<std::mutex> lock(mutex);
    reported_errors.push_back(error);
  }

  void ClearSentMessages() {
    std::lock_guard<std::mutex> lock(mutex);
    sent_messages.clear();
  }

  std::vector<SentMessage> CopySentMessages() {
    std::lock_guard<std::mutex> lock(mutex);
    return sent_messages;
  }

  std::mutex mutex;
  std::string room_id = "room-1";
  std::unordered_map<std::string, std::string> client_info = {
      {"App", "unit-test-app"}, {"osType", "test"}};
  std::unordered_map<int, std::string> session_list = {
      {3, R"({"type":"card","url":"lynx://card/3"})"},
      {8, R"({"type":"view","url":"lynx://view/8"})"}};
  std::string app_result = R"({"code":0,"message":"pong"})";
  std::vector<SentMessage> sent_messages;
  std::vector<std::string> received_types;
  std::vector<int> received_session_ids;
  std::vector<std::string> received_messages;
  std::vector<std::string> opened_cards;
  std::vector<std::string> app_methods;
  std::vector<std::string> app_params;
  std::vector<std::string> reported_errors;
  std::string changed_server_url;
  std::string changed_room_id;
};

bool ParseJson(const std::string &message, Json::Value *root) {
  Json::Reader reader;
  return reader.parse(message, *root);
}

std::string Stringify(const Json::Value &root) { return root.toStyledString(); }

std::string BuildInitializeMessage(int client_id) {
  return protocol::RemoteDebugProtocol::Stringify(
      protocol::RemoteDebugProtocol::CreateProtocolBody4Init(client_id));
}

std::string BuildRegisteredMessage() {
  return protocol::RemoteDebugProtocol::Stringify(
      protocol::RemoteDebugProtocol::CreateProtocolBody4Registerd());
}

std::string BuildRoomJoinedMessage(const std::string &room_id, int client_id) {
  return protocol::RemoteDebugProtocol::Stringify(
      protocol::RemoteDebugProtocol::CreateProtocolBody4RoomJoined(
          room_id.c_str(), client_id));
}

std::string BuildChangeRoomServerMessage(int client_id,
                                         const std::string &room_id,
                                         const std::string &url) {
  return protocol::RemoteDebugProtocol::Stringify(
      protocol::RemoteDebugProtocol::CreateProtocolBody4ChangeRoomServer(
          client_id, room_id, url));
}

std::string BuildListSessionMessage(int sender, int client_id) {
  Json::Value root(Json::objectValue);
  root[protocol::kKeyEvent] = protocol::kRemoteDebugServerEvent4Custom;
  root[protocol::kKeyData][protocol::kKeyType] =
      protocol::kRemoteDebugProtocolBodyData4Custom4ListSession;
  root[protocol::kKeyData][protocol::kKeySender] = sender;
  root[protocol::kKeyData][protocol::kKeyData][protocol::kKeyClientId] =
      client_id;
  return Stringify(root);
}

std::string BuildAppMessage(int sender, int client_id, int request_id,
                            const std::string &method,
                            const std::string &payload) {
  Json::Value root(Json::objectValue);
  root[protocol::kKeyEvent] = protocol::kRemoteDebugServerEvent4Custom;
  root[protocol::kKeyData][protocol::kKeyType] =
      protocol::kRemoteDebugProtocolBodyData4Custom4MessageHandler;
  root[protocol::kKeyData][protocol::kKeySender] = sender;
  Json::Value &data = root[protocol::kKeyData][protocol::kKeyData];
  data[protocol::kKeyClientId] = client_id;
  data[protocol::kKeyMessage][protocol::kKeyId] = request_id;
  data[protocol::kKeyMessage][protocol::kKeyMethod] = method;
  data[protocol::kKeyMessage][protocol::kKeyParams]["payload"] = payload;
  return Stringify(root);
}

processor::Processor CreateProcessor(RecordingMessageHandler **handler_raw) {
  auto handler = std::make_unique<RecordingMessageHandler>();
  *handler_raw = handler.get();
  return processor::Processor(std::move(handler));
}

void InitializeProcessor(processor::Processor *processor,
                         RecordingMessageHandler *handler, int client_id) {
  processor->Process(BuildInitializeMessage(client_id));
  handler->ClearSentMessages();
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

TEST(MessageTransceiverMultiplexTest, SendWithContextUsesContextOnly) {
  auto transceiver = std::make_shared<RecordingTransceiver>();
  std::shared_ptr<core::MessageTransceiver> base = transceiver;
  auto context = std::make_shared<RecordingContext>();

  base->Send("targeted-message", context);

  EXPECT_TRUE(transceiver->sent_messages.empty());
  ASSERT_EQ(context->sent_messages.size(), 1u);
  EXPECT_EQ(context->sent_messages[0], "targeted-message");
}

TEST(MessageTransceiverMultiplexTest,
     ConcurrentSendWithContextsRoutesEachMessageToOwnContext) {
  constexpr size_t kContextCount = 16;
  auto transceiver = std::make_shared<RecordingTransceiver>();
  std::shared_ptr<core::MessageTransceiver> base = transceiver;
  std::vector<std::shared_ptr<RecordingContext>> contexts;
  contexts.reserve(kContextCount);
  for (size_t i = 0; i < kContextCount; ++i) {
    contexts.push_back(std::make_shared<RecordingContext>());
  }

  RunInParallel(kContextCount, [&](size_t index) {
    base->Send("targeted-message-" + std::to_string(index), contexts[index]);
  });

  EXPECT_TRUE(transceiver->sent_messages.empty());
  for (size_t i = 0; i < kContextCount; ++i) {
    ASSERT_EQ(contexts[i]->sent_messages.size(), 1u);
    EXPECT_EQ(contexts[i]->sent_messages[0],
              "targeted-message-" + std::to_string(i));
  }
}

TEST(MessageTransceiverMultiplexTest,
     SendWithoutContextFallsBackToTransceiver) {
  auto transceiver = std::make_shared<RecordingTransceiver>();
  std::shared_ptr<core::MessageTransceiver> base = transceiver;

  base->Send("broadcast-message", nullptr);

  ASSERT_EQ(transceiver->sent_messages.size(), 1u);
  EXPECT_EQ(transceiver->sent_messages[0], "broadcast-message");
}

TEST(MessageTransceiverMultiplexTest,
     HandleReceivedMessageForwardsNullContext) {
  auto transceiver = std::make_shared<RecordingTransceiver>();
  RecordingDelegate delegate;
  transceiver->SetDelegate(&delegate);

  transceiver->HandleReceivedMessage("incoming-message");

  ASSERT_EQ(delegate.received_messages.size(), 1u);
  EXPECT_EQ(delegate.received_messages[0], "incoming-message");
  ASSERT_EQ(delegate.received_contexts.size(), 1u);
  EXPECT_EQ(delegate.received_contexts[0], nullptr);
}

TEST(ProcessorMultiplexTest, InitializeResponseKeepsRequestContext) {
  RecordingMessageHandler *handler = nullptr;
  auto processor = CreateProcessor(&handler);
  auto context = std::make_shared<RecordingContext>();

  processor.Process(BuildInitializeMessage(7), context);

  ASSERT_EQ(handler->sent_messages.size(), 1u);
  EXPECT_EQ(handler->sent_messages[0].context, context);
  Json::Value response;
  ASSERT_TRUE(ParseJson(handler->sent_messages[0].message, &response));
  EXPECT_EQ(response[protocol::kKeyEvent].asString(),
            protocol::kRemoteDebugServerEvent4Register);
  EXPECT_EQ(response[protocol::kKeyData][protocol::kKeyId].asInt(), 7);
  EXPECT_EQ(response[protocol::kKeyData][protocol::kKeyInfo]["App"].asString(),
            "unit-test-app");
}

TEST(ProcessorMultiplexTest, RegisteredResponseKeepsRequestContext) {
  RecordingMessageHandler *handler = nullptr;
  auto processor = CreateProcessor(&handler);
  InitializeProcessor(&processor, handler, 11);
  auto context = std::make_shared<RecordingContext>();

  processor.Process(BuildRegisteredMessage(), context);

  ASSERT_EQ(handler->sent_messages.size(), 1u);
  EXPECT_EQ(handler->sent_messages[0].context, context);
  Json::Value response;
  ASSERT_TRUE(ParseJson(handler->sent_messages[0].message, &response));
  EXPECT_EQ(response[protocol::kKeyEvent].asString(),
            protocol::kRemoteDebugServerEvent4JoinRoom);
  EXPECT_EQ(response[protocol::kKeyData].asString(), "room-1");
}

TEST(ProcessorMultiplexTest, RoomJoinedSessionListKeepsRequestContext) {
  RecordingMessageHandler *handler = nullptr;
  auto processor = CreateProcessor(&handler);
  InitializeProcessor(&processor, handler, 17);
  auto context = std::make_shared<RecordingContext>();

  processor.Process(BuildRoomJoinedMessage("room-1", 17), context);

  ASSERT_EQ(handler->sent_messages.size(), 1u);
  EXPECT_EQ(handler->sent_messages[0].context, context);
  Json::Value response;
  ASSERT_TRUE(ParseJson(handler->sent_messages[0].message, &response));
  EXPECT_EQ(response[protocol::kKeyEvent].asString(),
            protocol::kRemoteDebugServerEvent4Custom);
  EXPECT_EQ(response[protocol::kKeyData][protocol::kKeyType].asString(),
            protocol::kRemoteDebugProtocolBodyData4Custom4SessionList);
  EXPECT_EQ(response[protocol::kKeyData][protocol::kKeyData].size(), 2u);
}

TEST(ProcessorMultiplexTest, ExplicitListSessionKeepsRequestContext) {
  RecordingMessageHandler *handler = nullptr;
  auto processor = CreateProcessor(&handler);
  InitializeProcessor(&processor, handler, 23);
  auto context = std::make_shared<RecordingContext>();

  processor.Process(BuildListSessionMessage(1001, 23), context);

  ASSERT_EQ(handler->sent_messages.size(), 1u);
  EXPECT_EQ(handler->sent_messages[0].context, context);
  Json::Value response;
  ASSERT_TRUE(ParseJson(handler->sent_messages[0].message, &response));
  EXPECT_EQ(response[protocol::kKeyData][protocol::kKeyType].asString(),
            protocol::kRemoteDebugProtocolBodyData4Custom4SessionList);
  EXPECT_TRUE(handler->received_messages.empty());
}

TEST(ProcessorMultiplexTest, AppActionResponseKeepsRequestContext) {
  RecordingMessageHandler *handler = nullptr;
  auto processor = CreateProcessor(&handler);
  InitializeProcessor(&processor, handler, 29);
  auto context = std::make_shared<RecordingContext>();

  processor.Process(BuildAppMessage(2001, 29, 404, "App.ping", "hello"),
                    context);

  ASSERT_EQ(handler->sent_messages.size(), 1u);
  EXPECT_EQ(handler->sent_messages[0].context, context);
  ASSERT_EQ(handler->app_methods.size(), 1u);
  EXPECT_EQ(handler->app_methods[0], "App.ping");
  Json::Value params;
  ASSERT_TRUE(ParseJson(handler->app_params[0], &params));
  EXPECT_EQ(params["payload"].asString(), "hello");

  Json::Value response;
  ASSERT_TRUE(ParseJson(handler->sent_messages[0].message, &response));
  EXPECT_EQ(response[protocol::kKeyData][protocol::kKeyType].asString(),
            protocol::kRemoteDebugProtocolBodyData4Custom4MessageHandler);
  EXPECT_EQ(response[protocol::kKeyData][protocol::kKeySender].asInt(), 2001);
  EXPECT_EQ(
      response[protocol::kKeyData][protocol::kKeyData][protocol::kKeyClientId]
          .asInt(),
      29);

  Json::Value app_message;
  ASSERT_TRUE(ParseJson(
      response[protocol::kKeyData][protocol::kKeyData][protocol::kKeyMessage]
          .asString(),
      &app_message));
  EXPECT_EQ(app_message[protocol::kKeyId].asInt(), 404);
  EXPECT_EQ(app_message[protocol::kKeyMethod].asString(), "App.ping");
  Json::Value result;
  ASSERT_TRUE(ParseJson(app_message[protocol::kKeyResult].asString(), &result));
  EXPECT_EQ(result[protocol::kKeyCode].asInt(), 0);
  EXPECT_EQ(result[protocol::kKeyMessage].asString(), "pong");
}

TEST(ProcessorMultiplexTest, ConcurrentAppActionsKeepEachRequestContext) {
  constexpr size_t kRequestCount = 16;
  constexpr int kClientId = 41;
  constexpr int kRequestIdBase = 1000;
  RecordingMessageHandler *handler = nullptr;
  auto processor = CreateProcessor(&handler);
  InitializeProcessor(&processor, handler, kClientId);

  std::vector<std::shared_ptr<core::MessageTransceiverContext>> contexts;
  contexts.reserve(kRequestCount);
  for (size_t i = 0; i < kRequestCount; ++i) {
    contexts.push_back(std::make_shared<RecordingContext>());
  }

  RunInParallel(kRequestCount, [&](size_t index) {
    processor.Process(
        BuildAppMessage(2000 + static_cast<int>(index), kClientId,
                        kRequestIdBase + static_cast<int>(index), "App.ping",
                        "payload-" + std::to_string(index)),
        contexts[index]);
  });

  auto sent_messages = handler->CopySentMessages();
  ASSERT_EQ(sent_messages.size(), kRequestCount);
  std::vector<bool> seen(kRequestCount, false);
  for (const auto &sent_message : sent_messages) {
    Json::Value response;
    ASSERT_TRUE(ParseJson(sent_message.message, &response));
    ASSERT_EQ(response[protocol::kKeyData][protocol::kKeyType].asString(),
              protocol::kRemoteDebugProtocolBodyData4Custom4MessageHandler);

    Json::Value app_message;
    ASSERT_TRUE(ParseJson(
        response[protocol::kKeyData][protocol::kKeyData][protocol::kKeyMessage]
            .asString(),
        &app_message));
    const int request_id = app_message[protocol::kKeyId].asInt();
    const int index = request_id - kRequestIdBase;
    ASSERT_GE(index, 0);
    ASSERT_LT(index, static_cast<int>(kRequestCount));
    EXPECT_EQ(sent_message.context, contexts[index]);
    EXPECT_FALSE(seen[index]);
    seen[index] = true;
  }

  EXPECT_TRUE(std::all_of(seen.begin(), seen.end(),
                          [](bool has_response) { return has_response; }));
}

TEST(ProcessorMultiplexTest, ChangeRoomServerAckKeepsRequestContext) {
  RecordingMessageHandler *handler = nullptr;
  auto processor = CreateProcessor(&handler);
  InitializeProcessor(&processor, handler, 31);
  auto context = std::make_shared<RecordingContext>();

  processor.Process(
      BuildChangeRoomServerMessage(31, "room-2", "ws://127.0.0.1:9000"),
      context);

  ASSERT_EQ(handler->sent_messages.size(), 1u);
  EXPECT_EQ(handler->sent_messages[0].context, context);
  Json::Value response;
  ASSERT_TRUE(ParseJson(handler->sent_messages[0].message, &response));
  EXPECT_EQ(response[protocol::kKeyEvent].asString(),
            protocol::kRemoteDebugServerEvent4ChangeRoomServerAck);
  EXPECT_EQ(response[protocol::kKeyData].asInt(), 31);
  EXPECT_EQ(handler->changed_server_url, "ws://127.0.0.1:9000");
  EXPECT_EQ(handler->changed_room_id, "room-2");
}

TEST(ProcessorMultiplexTest, LegacyProcessWithoutContextStillSendsNullContext) {
  RecordingMessageHandler *handler = nullptr;
  auto processor = CreateProcessor(&handler);

  processor.Process(BuildInitializeMessage(37));

  ASSERT_EQ(handler->sent_messages.size(), 1u);
  EXPECT_EQ(handler->sent_messages[0].context, nullptr);
}

}  // namespace
}  // namespace debugrouter
