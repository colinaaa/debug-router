// Copyright 2023 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef DEBUGROUTER_NATIVE_PROCESSOR_PROCESSOR_H_
#define DEBUGROUTER_NATIVE_PROCESSOR_PROCESSOR_H_

#include <memory>
#include <string>

#include "debug_router/native/processor/message_handler.h"
#include "debug_router/native/protocol/protocol.h"

namespace debugrouter {
namespace core {
class MessageTransceiverContext;
}
namespace processor {

extern const char *kDebugRouterErrorMessage;
extern const int kDebugRouterErrorCode;

class Processor {
 public:
  explicit Processor(std::unique_ptr<MessageHandler> message_handler);
  void Process(const std::string &message);
  void Process(const std::string &message,
               const std::shared_ptr<core::MessageTransceiverContext> &context);
  std::string WrapCustomizedMessage(const std::string &type, int session_id,
                                    const std::string &message, int mark,
                                    bool isObject = false);
  void FlushSessionList();
  void SetIsReconnect(bool is_reconnect);

 private:
  void registerDevice();
  void registerDevice(
      const std::shared_ptr<core::MessageTransceiverContext> &context);
  void joinRoom();
  void joinRoom(
      const std::shared_ptr<core::MessageTransceiverContext> &context);
  void reportError(const std::string &error);
  void sessionList();
  void sessionList(
      const std::shared_ptr<core::MessageTransceiverContext> &context);
  void changeRoomServer(const std::string &url, const std::string &room);
  void changeRoomServer(
      const std::string &url, const std::string &room,
      const std::shared_ptr<core::MessageTransceiverContext> &context);
  void openCard(const std::string &url);
  void processMessage(const std::string &type, int session_id,
                      const std::string &message);
  void HandleAppAction(
      const std::shared_ptr<protocol::RemoteDebugProtocolBodyData4Custom>
          custom_data,
      const std::shared_ptr<core::MessageTransceiverContext> &context);
  std::string wrapStopAtEntryMessage(const std::string &type,
                                     const std::string &message) const;

  debugrouter::protocol::RemoteDebugPrococolClientId client_id_;
  std::unique_ptr<MessageHandler> message_handler_;
  bool is_reconnect_;

  void process(const Json::Value &root,
               const std::shared_ptr<core::MessageTransceiverContext> &context);
};

}  // namespace processor
}  // namespace debugrouter

#endif  // DEBUGROUTER_NATIVE_PROCESSOR_PROCESSOR_H_
