#pragma once

#include "core.hpp"
#include "io.hpp"
#include "tb.hpp"

#include <ctime>

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <event2/event.h>
#include <event2/listener.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

namespace buxtehude
{

class Client;

class ClientHandle
{
public:
    ClientHandle(Client& iclient, std::string_view teamname);
    ClientHandle(ConnectionType conn_type, FILE* ptr, uint32_t max_msg_len);

    ClientHandle(const ClientHandle&) = delete;
    ClientHandle& operator=(const ClientHandle&) = delete;

    ClientHandle(ClientHandle&& other) noexcept = default;
    ClientHandle& operator=(ClientHandle&& other) noexcept = default;
    ~ClientHandle() = default;

    // Applicable to all types of ClientHandle
    tb::error<WriteError> Handshake();
    tb::error<WriteError> Write(const Message& m);
    void Error(std::string_view errstr);
    void Disconnect(std::string_view reason="Disconnected by server");
    void Disconnect_NoWrite();

    bool Available(std::string_view type);

    // Try to read a message from the socket - only for INTERNET/UNIX
    tb::result<Message, ReadError> Read();

    Stream stream; // Only for UNIX/INTERNET
    std::time_t last_error = 0;

    std::vector<std::string> unavailable;
    UEvent read_event, write_event;
    Client* client_ptr = nullptr; // Only for INTERNAL connections

    ConnectionType conn_type;
    ClientPreferences preferences;

    int socket = -1;

    bool handshaken = false;
    bool connected = false;
};

class Server
{
public:
    Server() = default;
    Server(const Server& other) = delete;
    ~Server();

    tb::error<ListenError> UnixServer(std::string_view path="buxtehude_unix");
    tb::error<ListenError> IPServer(uint16_t port=DEFAULT_PORT);
    tb::error<AllocError> InternalServer();

    void Close();

    uint32_t max_msg_length = DEFAULT_MAX_MESSAGE_LENGTH;
private: // For INTERNAL connections only.
    friend Client;
    void Internal_AddClient(Client& cl);
    void Internal_RemoveClient(Client& cl);
    void Internal_ReceiveFrom(Client& cl, const Message& msg);
private:
    using HandleIter = std::vector<ClientHandle>::iterator;

    void Run();
    void Serve(HandleIter client_handle);
    void HandleMessage(ClientHandle& client_handle, Message&& msg);
    void Broadcast_NoLock(const Message& msg);

    // Only if listening sockets are opened
    tb::error<AllocError> SetupEvents();
    void Listen();
    void AddConnection(int fd, sa_family_t addr_type);

    // Retrieving clients
    HandleIter GetClientBySocket(int fd);
    HandleIter GetClientByPointer(Client* ptr);
    HandleIter GetFirstAvailable(std::string_view team, std::string_view type,
        const ClientHandle& exclude);

    std::vector<ClientHandle> clients;
    std::vector<std::pair<Client*, Message>> internal_messages;
    std::mutex clients_mutex, internal_mutex;

    std::thread current_thread;
    bool started = false;

    // File descriptors for listening sockets
    int unix_server = -1;
    int ip_server = -1;

    std::string unix_path;

    // Libevent internals
    UEventBase ebase;
    UEvconnListener ip_listener, unix_listener;
    UEvent interrupt_event, read_internal_event;

    EventCallbackData callback_data;
};

}
