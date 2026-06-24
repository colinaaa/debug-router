// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "debug_router/native/socket/socket_server_api.h"

#ifndef _WIN32

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "debug_router/native/core/debug_router_core.h"
#include "debug_router/native/core/util.h"
#include "debug_router/native/socket/usb_client.h"
#include "gtest/gtest.h"

namespace debugrouter {
namespace socket_server {
namespace {

class RecordingListener final : public SocketServerConnectionListener {
 public:
  void OnInit(int32_t code, const std::string &info) override {}

  void OnStatusChanged(ConnectionStatus status, int32_t code,
                       const std::string &info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    statuses_.push_back(status);
    condition_.notify_all();
  }

  void OnMessage(std::shared_ptr<UsbClient> client,
                 const std::string &message) override {
    std::lock_guard<std::mutex> lock(mutex_);
    message_clients_.push_back(client);
    messages_.push_back(message);
    condition_.notify_all();
  }

  void OnClientClosed(std::shared_ptr<UsbClient> client) override {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_clients_.push_back(client);
    condition_.notify_all();
  }

  bool WaitForStatusCount(size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(1),
                               [&]() { return statuses_.size() >= count; });
  }

  size_t StatusCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return statuses_.size();
  }

  ConnectionStatus StatusAt(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    return statuses_[index];
  }

  bool WaitForMessageCount(size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(1),
                               [&]() { return messages_.size() >= count; });
  }

  std::shared_ptr<UsbClient> MessageClientAt(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_clients_[index].lock();
  }

  std::string MessageAt(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_[index];
  }

  bool WaitForClosedClientCount(size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(
        lock, std::chrono::seconds(1),
        [&]() { return closed_clients_.size() >= count; });
  }

  std::shared_ptr<UsbClient> ClosedClientAt(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_clients_[index].lock();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<ConnectionStatus> statuses_;
  std::vector<std::weak_ptr<UsbClient>> message_clients_;
  std::vector<std::string> messages_;
  std::vector<std::weak_ptr<UsbClient>> closed_clients_;
};

class TestSocketServer : public SocketServer {
 public:
  using SocketServer::SocketServer;

  void AddPendingClientForTest(const std::shared_ptr<UsbClient> &client) {
    AddPendingClient(client);
  }

  size_t ActiveClientCount() { return ActiveClientCountForTest(); }

  size_t PendingClientCount() { return PendingClientCountForTest(); }

 private:
  void Start() override {}
  int GetErrorMessage() override { return 0; }
  void CloseSocket(int socket_fd) override {}
};

std::shared_ptr<UsbClient> MakeUsbClient(std::vector<int> *peer_sockets) {
  int sockets[2] = {-1, -1};
  EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  peer_sockets->push_back(sockets[1]);
  return std::make_shared<UsbClient>(sockets[0]);
}

void ClosePeer(int *peer_socket) {
  if (*peer_socket != -1) {
    shutdown(*peer_socket, SHUT_RDWR);
    close(*peer_socket);
    *peer_socket = -1;
  }
}

bool WriteExact(int socket_fd, const char *data, size_t size) {
  size_t written = 0;
  while (written < size) {
    ssize_t result = send(socket_fd, data + written, size - written, 0);
    if (result <= 0) {
      return false;
    }
    written += static_cast<size_t>(result);
  }
  return true;
}

std::string EncodeIncomingFrame(const std::string &message) {
  std::string frame;
  frame.resize(kFrameHeaderLen + kPayloadSizeLen + message.size());
  char *buffer = &frame[0];
  char word[4];
  util::IntToCharArray(kFrameProtocolVersion, word);
  memcpy(buffer, word, 4);
  util::IntToCharArray(kPTFrameTypeTextMessage, word);
  memcpy(buffer + 4, word, 4);
  util::IntToCharArray(kFrameDefaultTag, word);
  memcpy(buffer + 8, word, 4);
  util::IntToCharArray(static_cast<uint32_t>(message.size() + 4), word);
  memcpy(buffer + 12, word, 4);
  util::IntToCharArray(static_cast<uint32_t>(message.size()), word);
  memcpy(buffer + 16, word, 4);
  memcpy(buffer + 20, message.data(), message.size());
  return frame;
}

bool WriteFramedMessage(int socket_fd, const std::string &message) {
  std::string frame = EncodeIncomingFrame(message);
  return WriteExact(socket_fd, frame.data(), frame.size());
}

bool ReadExactWithTimeout(int socket_fd, char *buffer, size_t size,
                          std::chrono::milliseconds timeout) {
  size_t received = 0;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (received < size) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_fd, &read_set);
    timeval tv;
    tv.tv_sec = static_cast<long>(remaining.count() / 1000000);
    tv.tv_usec = static_cast<long>(remaining.count() % 1000000);
    int ready = select(socket_fd + 1, &read_set, nullptr, nullptr, &tv);
    if (ready <= 0) {
      return false;
    }
    ssize_t result = recv(socket_fd, buffer + received, size - received, 0);
    if (result <= 0) {
      return false;
    }
    received += static_cast<size_t>(result);
  }
  return true;
}

bool ReadFramedMessage(int socket_fd, std::string *message,
                       std::chrono::milliseconds timeout =
                           std::chrono::milliseconds(1000)) {
  char header[kFrameHeaderLen];
  if (!ReadExactWithTimeout(socket_fd, header, kFrameHeaderLen, timeout)) {
    return false;
  }
  char payload_size[kPayloadSizeLen];
  if (!ReadExactWithTimeout(socket_fd, payload_size, kPayloadSizeLen,
                            timeout)) {
    return false;
  }
  uint32_t payload_size_int =
      util::DecodePayloadSize(payload_size, kPayloadSizeLen);
  message->resize(payload_size_int);
  if (payload_size_int == 0) {
    return true;
  }
  return ReadExactWithTimeout(socket_fd, &(*message)[0], payload_size_int,
                              timeout);
}

bool WaitUntil(const std::function<bool()> &predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

TEST(SocketServerApiTestSuite,
     UsbClientsBroadcastTargetAndSurvivePeerCloseWithFrames) {
  core::DebugRouterCore::GetInstance();

  std::vector<int> peer_sockets;
  auto listener = std::make_shared<RecordingListener>();
  auto server = std::make_shared<TestSocketServer>(listener);
  server->StartServer();
  auto client_listener = std::make_shared<ClientListener>(server);
  auto first_client = MakeUsbClient(&peer_sockets);
  auto second_client = MakeUsbClient(&peer_sockets);
  first_client->Init();
  second_client->Init();
  server->AddPendingClientForTest(first_client);
  server->AddPendingClientForTest(second_client);
  first_client->StartUp(client_listener);
  second_client->StartUp(client_listener);

  ASSERT_TRUE(WriteFramedMessage(peer_sockets[0], "from first"));
  ASSERT_TRUE(listener->WaitForStatusCount(1));
  ASSERT_TRUE(listener->WaitForMessageCount(1));
  EXPECT_EQ(listener->MessageClientAt(0), first_client);
  EXPECT_EQ(listener->MessageAt(0), "from first");

  ASSERT_TRUE(WriteFramedMessage(peer_sockets[1], "from second"));
  ASSERT_TRUE(listener->WaitForMessageCount(2));
  EXPECT_TRUE(WaitUntil([&]() {
    return server->ActiveClientCount() == 2U &&
           server->PendingClientCount() == 0U;
  }));
  EXPECT_EQ(listener->StatusCount(), 1U);
  EXPECT_EQ(listener->MessageClientAt(1), second_client);
  EXPECT_EQ(listener->MessageAt(1), "from second");

  first_client->SetConnectStatus(USBConnectStatus::CONNECTED);
  second_client->SetConnectStatus(USBConnectStatus::CONNECTED);
  ASSERT_TRUE(server->Broadcast("broadcast"));
  std::string first_message;
  std::string second_message;
  ASSERT_TRUE(ReadFramedMessage(peer_sockets[0], &first_message));
  ASSERT_TRUE(ReadFramedMessage(peer_sockets[1], &second_message));
  EXPECT_EQ(first_message, "broadcast");
  EXPECT_EQ(second_message, "broadcast");

  first_client->SetConnectStatus(USBConnectStatus::CONNECTED);
  second_client->SetConnectStatus(USBConnectStatus::CONNECTED);
  ASSERT_TRUE(server->Send(first_client, "targeted"));
  ASSERT_TRUE(ReadFramedMessage(peer_sockets[0], &first_message));
  EXPECT_EQ(first_message, "targeted");
  EXPECT_FALSE(ReadFramedMessage(peer_sockets[1], &second_message,
                                 std::chrono::milliseconds(100)));

  ClosePeer(&peer_sockets[0]);
  ASSERT_TRUE(listener->WaitForClosedClientCount(1));
  EXPECT_EQ(listener->ClosedClientAt(0), first_client);
  EXPECT_TRUE(WaitUntil([&]() { return server->ActiveClientCount() == 1U; }));
  EXPECT_EQ(listener->StatusCount(), 1U);

  second_client->SetConnectStatus(USBConnectStatus::CONNECTED);
  ASSERT_TRUE(server->Broadcast("after close"));
  ASSERT_TRUE(ReadFramedMessage(peer_sockets[1], &second_message));
  EXPECT_EQ(second_message, "after close");

  ClosePeer(&peer_sockets[1]);
  ASSERT_TRUE(listener->WaitForClosedClientCount(2));
  ASSERT_TRUE(listener->WaitForStatusCount(2));
  EXPECT_EQ(listener->StatusAt(1), ConnectionStatus::kError);
  EXPECT_TRUE(WaitUntil([&]() { return server->ActiveClientCount() == 0U; }));

  server->StopServer();
  for (int &peer_socket : peer_sockets) {
    ClosePeer(&peer_socket);
  }
}

TEST(SocketServerApiTestSuite,
     TracksMultipleActiveClientsWithAggregateLifecycleNotifications) {
  core::DebugRouterCore::GetInstance();

  std::vector<int> peer_sockets;
  auto listener = std::make_shared<RecordingListener>();
  auto server = std::make_shared<TestSocketServer>(listener);
  server->StartServer();
  auto first_client = MakeUsbClient(&peer_sockets);
  auto second_client = MakeUsbClient(&peer_sockets);

  server->AddPendingClientForTest(first_client);
  server->AddPendingClientForTest(second_client);
  EXPECT_EQ(server->PendingClientCount(), 2U);

  server->HandleOnOpenStatus(first_client, ConnectionStatus::kConnected,
                             "first connected");
  ASSERT_TRUE(listener->WaitForStatusCount(1));
  EXPECT_EQ(listener->StatusAt(0), ConnectionStatus::kConnected);
  EXPECT_TRUE(WaitUntil([&]() {
    return server->ActiveClientCount() == 1U &&
           server->PendingClientCount() == 1U;
  }));

  server->HandleOnOpenStatus(second_client, ConnectionStatus::kConnected,
                             "second connected");
  EXPECT_TRUE(WaitUntil([&]() {
    return server->ActiveClientCount() == 2U &&
           server->PendingClientCount() == 0U;
  }));
  EXPECT_EQ(listener->StatusCount(), 1U);

  EXPECT_TRUE(server->Broadcast("broadcast"));
  EXPECT_TRUE(server->Send(first_client, "targeted"));

  server->HandleOnMessageStatus(second_client, "from second");
  ASSERT_TRUE(listener->WaitForMessageCount(1));
  EXPECT_EQ(listener->MessageClientAt(0), second_client);
  EXPECT_EQ(listener->MessageAt(0), "from second");

  server->HandleOnCloseStatus(first_client, ConnectionStatus::kDisconnected, 0,
                              "first closed");
  ASSERT_TRUE(listener->WaitForClosedClientCount(1));
  EXPECT_EQ(listener->ClosedClientAt(0), first_client);
  EXPECT_TRUE(WaitUntil([&]() { return server->ActiveClientCount() == 1U; }));
  EXPECT_EQ(listener->StatusCount(), 1U);
  EXPECT_FALSE(server->Send(first_client, "closed client"));

  server->HandleOnCloseStatus(second_client, ConnectionStatus::kDisconnected, 0,
                              "second closed");
  ASSERT_TRUE(listener->WaitForClosedClientCount(2));
  EXPECT_EQ(listener->ClosedClientAt(1), second_client);
  ASSERT_TRUE(listener->WaitForStatusCount(2));
  EXPECT_EQ(listener->StatusAt(1), ConnectionStatus::kDisconnected);
  EXPECT_TRUE(WaitUntil([&]() { return server->ActiveClientCount() == 0U; }));

  server->StopServer();
  for (int peer_socket : peer_sockets) {
    close(peer_socket);
  }
}

}  // namespace
}  // namespace socket_server
}  // namespace debugrouter

#endif  // _WIN32
