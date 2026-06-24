// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "debug_router/native/core/message_transceiver.h"

#include "gtest/gtest.h"

namespace debugrouter {
namespace core {
namespace {

class TestMessageContext : public MessageTransceiverContext {};

class TestTransceiver final : public MessageTransceiver {
 public:
  using MessageTransceiver::Send;

  bool Connect(const std::string &url) override { return false; }
  void Disconnect() override {}
  void Send(const std::string &data) override { last_sent_ = data; }
  ConnectionType GetType() override { return ConnectionType::kUsb; }
  void StartServer() override {}
  void StopServer() override {}

  std::string last_sent_;
};

class ContextAwareDelegate : public MessageTransceiverDelegate {
 public:
  void OnOpen(const std::shared_ptr<MessageTransceiver> &transceiver) override {
  }
  void OnClosed(
      const std::shared_ptr<MessageTransceiver> &transceiver) override {}
  void OnFailure(const std::shared_ptr<MessageTransceiver> &transceiver,
                 const std::string &error_message, int error_code) override {}
  void OnMessage(const std::string &message,
                 const std::shared_ptr<MessageTransceiver> &transceiver)
      override {
    legacy_message = message;
  }
  void OnMessage(const std::string &message,
                 const std::shared_ptr<MessageTransceiver> &transceiver,
                 const std::shared_ptr<MessageTransceiverContext> &context)
      override {
    context_message = message;
    received_context = context;
  }
  void OnInit(const std::shared_ptr<MessageTransceiver> &transceiver,
              int32_t code, const std::string &info) override {}

  std::string legacy_message;
  std::string context_message;
  std::shared_ptr<MessageTransceiverContext> received_context;
};

class LegacyDelegate : public MessageTransceiverDelegate {
 public:
  void OnOpen(const std::shared_ptr<MessageTransceiver> &transceiver) override {
  }
  void OnClosed(
      const std::shared_ptr<MessageTransceiver> &transceiver) override {}
  void OnFailure(const std::shared_ptr<MessageTransceiver> &transceiver,
                 const std::string &error_message, int error_code) override {}
  void OnMessage(const std::string &message,
                 const std::shared_ptr<MessageTransceiver> &transceiver)
      override {
    legacy_message = message;
  }
  void OnInit(const std::shared_ptr<MessageTransceiver> &transceiver,
              int32_t code, const std::string &info) override {}

  std::string legacy_message;
};

TEST(MessageTransceiverTestSuite, PassesContextToContextAwareDelegate) {
  auto transceiver = std::make_shared<TestTransceiver>();
  ContextAwareDelegate delegate;
  transceiver->SetDelegate(&delegate);
  auto context = std::make_shared<TestMessageContext>();

  transceiver->HandleReceivedMessage("context message", context);

  EXPECT_EQ(delegate.context_message, "context message");
  EXPECT_EQ(delegate.received_context, context);
  EXPECT_TRUE(delegate.legacy_message.empty());
}

TEST(MessageTransceiverTestSuite, FallsBackToLegacyDelegateWithoutContext) {
  auto transceiver = std::make_shared<TestTransceiver>();
  LegacyDelegate delegate;
  transceiver->SetDelegate(&delegate);
  auto context = std::make_shared<TestMessageContext>();

  transceiver->HandleReceivedMessage("legacy message", context);

  EXPECT_EQ(delegate.legacy_message, "legacy message");
}

TEST(MessageTransceiverTestSuite, SendWithContextUsesLegacySendByDefault) {
  auto transceiver = std::make_shared<TestTransceiver>();
  auto context = std::make_shared<TestMessageContext>();

  transceiver->Send("payload", context);

  EXPECT_EQ(transceiver->last_sent_, "payload");
}

}  // namespace
}  // namespace core
}  // namespace debugrouter
