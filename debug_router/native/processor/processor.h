// Copyright 2023 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef DEBUGROUTER_NATIVE_PROCESSOR_PROCESSOR_H_
#define DEBUGROUTER_NATIVE_PROCESSOR_PROCESSOR_H_

#include <string>

#include "debug_router/native/processor/message_handler.h"
#include "debug_router/native/protocol/protocol.h"

namespace debugrouter {
namespace processor {

extern const char *kDebugRouterErrorMessage;
extern const int kDebugRouterErrorCode;

class Processor {
 public:
  struct ClientProtocolContext {
    debugrouter::protocol::RemoteDebugPrococolClientId client_id = 0;
    bool is_reconnect = false;
  };

  explicit Processor(std::unique_ptr<MessageHandler> message_handler);
  void Process(const std::string &message);
  void Process(const std::string &message, ClientProtocolContext &context);
  std::string WrapCustomizedMessage(const std::string &type, int session_id,
                                    const std::string &message, int mark,
                                    bool isObject = false);
  std::string WrapCustomizedMessage(const std::string &type, int session_id,
                                    const std::string &message, int mark,
                                    bool isObject,
                                    const ClientProtocolContext &context);
  void FlushSessionList();
  void FlushSessionList(const ClientProtocolContext &context);
  void SetIsReconnect(bool is_reconnect);

 private:
  void registerDevice(const ClientProtocolContext &context);
  void joinRoom();
  void reportError(const std::string &error);
  void sessionList(const ClientProtocolContext &context);
  void changeRoomServer(const std::string &url, const std::string &room,
                        const ClientProtocolContext &context);
  void openCard(const std::string &url);
  void processMessage(const std::string &type, int session_id,
                      const std::string &message);
  void HandleAppAction(
      const std::shared_ptr<protocol::RemoteDebugProtocolBodyData4Custom>
          custom_data,
      const ClientProtocolContext &context);
  std::string wrapStopAtEntryMessage(const std::string &type,
                                     const std::string &message,
                                     const ClientProtocolContext &context) const;

  std::unique_ptr<MessageHandler> message_handler_;
  ClientProtocolContext default_context_;

  void process(const Json::Value &root, ClientProtocolContext &context);
};

}  // namespace processor
}  // namespace debugrouter

#endif  // DEBUGROUTER_NATIVE_PROCESSOR_PROCESSOR_H_
