// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "debug_router/native/processor/processor.h"

#include <unordered_map>
#include <vector>

#include "debug_router/native/protocol/protocol.h"
#include "gtest/gtest.h"
#include "json/reader.h"

namespace debugrouter {
namespace processor {
namespace {

class RecordingMessageHandler final : public MessageHandler {
 public:
  std::string GetRoomId() override { return "room"; }

  std::unordered_map<std::string, std::string> GetClientInfo() override {
    return {{"app", "debug-router-test"}};
  }

  std::unordered_map<int, std::string> GetSessionList() override { return {}; }

  void OnMessage(const std::string &type, int session_id,
                 const std::string &message) override {
    received_messages.push_back(type + ":" + std::to_string(session_id) +
                                ":" + message);
  }

  void SendMessage(const std::string &message) override {
    sent_messages.push_back(message);
  }

  void OpenCard(const std::string &url) override {}

  std::string HandleAppAction(const std::string &method,
                              const std::string &params) override {
    return "{}";
  }

  void ChangeRoomServer(const std::string &url,
                        const std::string &room) override {}

  void ReportError(const std::string &error) override {}

  std::vector<std::string> sent_messages;
  std::vector<std::string> received_messages;
};

std::string InitMessage(protocol::RemoteDebugPrococolClientId client_id) {
  return protocol::RemoteDebugProtocol::Stringify(
      protocol::RemoteDebugProtocol::CreateProtocolBody4Init(client_id));
}

Json::Value ParseJson(const std::string &message) {
  Json::Value root;
  Json::Reader reader;
  EXPECT_TRUE(reader.parse(message, root));
  return root;
}

TEST(ProcessorTestSuite, ExplicitContextsKeepClientIdsIndependent) {
  auto *handler = new RecordingMessageHandler();
  RecordingMessageHandler *handler_ptr = handler;
  Processor processor{std::unique_ptr<MessageHandler>(handler)};
  Processor::ClientProtocolContext first_context;
  Processor::ClientProtocolContext second_context;

  processor.Process(InitMessage(101), first_context);
  processor.Process(InitMessage(202), second_context);

  ASSERT_EQ(handler_ptr->sent_messages.size(), 2U);
  EXPECT_EQ(ParseJson(handler_ptr->sent_messages[0])[protocol::kKeyData]
                [protocol::kKeyId]
                    .asUInt(),
            101U);
  EXPECT_EQ(ParseJson(handler_ptr->sent_messages[1])[protocol::kKeyData]
                [protocol::kKeyId]
                    .asUInt(),
            202U);

  std::string first_wrapped = processor.WrapCustomizedMessage(
      protocol::kRemoteDebugProtocolBodyData4CDP, 1, "first", -1, false,
      first_context);
  std::string second_wrapped = processor.WrapCustomizedMessage(
      protocol::kRemoteDebugProtocolBodyData4CDP, 2, "second", -1, false,
      second_context);

  Json::Value first = ParseJson(first_wrapped);
  Json::Value second = ParseJson(second_wrapped);
  EXPECT_EQ(first[protocol::kKeyData][protocol::kKeySender].asUInt(), 101U);
  EXPECT_EQ(first[protocol::kKeyData][protocol::kKeyData]
                 [protocol::kKeyClientId]
                     .asUInt(),
            101U);
  EXPECT_EQ(second[protocol::kKeyData][protocol::kKeySender].asUInt(), 202U);
  EXPECT_EQ(second[protocol::kKeyData][protocol::kKeyData]
                  [protocol::kKeyClientId]
                      .asUInt(),
            202U);
}

TEST(ProcessorTestSuite, LegacyApiStillUsesDefaultContext) {
  auto *handler = new RecordingMessageHandler();
  RecordingMessageHandler *handler_ptr = handler;
  Processor processor{std::unique_ptr<MessageHandler>(handler)};

  processor.Process(InitMessage(303));

  ASSERT_EQ(handler_ptr->sent_messages.size(), 1U);
  EXPECT_EQ(ParseJson(handler_ptr->sent_messages[0])[protocol::kKeyData]
                [protocol::kKeyId]
                    .asUInt(),
            303U);
  std::string wrapped = processor.WrapCustomizedMessage(
      protocol::kRemoteDebugProtocolBodyData4CDP, 3, "legacy", -1, false);
  Json::Value root = ParseJson(wrapped);
  EXPECT_EQ(root[protocol::kKeyData][protocol::kKeySender].asUInt(), 303U);
}

}  // namespace
}  // namespace processor
}  // namespace debugrouter
