#pragma once

#include "core.hpp"
#include "io.hpp"
#include "tb.hpp"

#include <atomic>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <event2/event.h>

namespace buxtehude
{

class Server;
class ClientHandle;

class Client
{
public:
    Client() = default;
    Client(const Client& other) = delete;
    Client(Client&& other) = delete;
    Client(const ClientPreferences& preferences);
    ~Client();

    tb::error<ConnectError> IPConnect(std::string_view hostname, uint16_t port);
    tb::error<ConnectError> UnixConnect(std::string_view path);
    void InternalConnect(Server& server);

    // Applicable to all types of Client
    tb::error<WriteError> Write(const Message& msg);
    tb::error<WriteError> Handshake();
    tb::error<WriteError> SetAvailable(std::string_view type, bool available);

    void AddHandler(std::string_view type, Handler&& h);
    void EraseHandler(const std::string& type);
    void ClearHandlers();

    void Run();
    void Close(); // Run must be called before Close()

    ClientPreferences preferences;
private: // Only for INTERNAL clients
    friend ClientHandle;
    // Called by the Server ClientHandle when it sends a message
    void Receive(const Message& msg);
private:
    // Only for socket-based connections
    tb::error<AllocError> SetupEvents();
    void Read();
    void Listen();

    void HandleMessage(const Message& msg);

    ConnectionType conn_type;

    int client_socket = -1;
    Stream stream;
    Server* server_ptr = nullptr;

    std::unordered_map<std::string, Handler> handlers;
    std::queue<Message> ingress, egress;

    std::thread current_thread;
    std::atomic<bool> run = false;
    bool setup = false;

    // Libevent internals
    UEventBase ebase;
    UEvent read_event, interrupt_event;

    EventCallbackData callback_data;
};

}
