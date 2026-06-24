// Copyright 2023 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "debug_router/native/core/message_transceiver.h"

namespace debugrouter {
namespace core {
MessageTransceiver::MessageTransceiver() {}

const void *MessageTransceiverContext::GetTypeId() const { return nullptr; }

const void *MessageTransceiverContext::GetContextKey() const { return this; }

void MessageTransceiverDelegate::OnMessage(
    const std::string &message,
    const std::shared_ptr<MessageTransceiver> &transceiver,
    const std::shared_ptr<MessageTransceiverContext> &context) {
  (void)context;
  OnMessage(message, transceiver);
}

void MessageTransceiverDelegate::OnContextClosed(
    const std::shared_ptr<MessageTransceiver> &transceiver,
    const std::shared_ptr<MessageTransceiverContext> &context) {
  (void)transceiver;
  (void)context;
}

void MessageTransceiver::Send(
    const std::string &data,
    const std::shared_ptr<MessageTransceiverContext> &context) {
  (void)context;
  Send(data);
}

void MessageTransceiver::HandleReceivedMessage(const std::string &message) {
  HandleReceivedMessage(message, nullptr);
}

void MessageTransceiver::HandleReceivedMessage(
    const std::string &message,
    const std::shared_ptr<MessageTransceiverContext> &context) {
  if (delegate_) {
    delegate_->OnMessage(message, shared_from_this(), context);
  }
}

void MessageTransceiver::SetDelegate(MessageTransceiverDelegate *delegate) {
  delegate_ = delegate;
}

MessageTransceiverDelegate *MessageTransceiver::delegate() { return delegate_; }

}  // namespace core
}  // namespace debugrouter
