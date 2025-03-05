#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>

#include <cstdio>

#include <event2/event.h>
#include <event2/listener.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include "validate.hpp"

namespace buxtehude
{

constexpr std::string_view MSG_ALL        = "$$all";
constexpr std::string_view MSG_AVAILABLE  = "$$available";
constexpr std::string_view MSG_DISCONNECT = "$$disconnect";
constexpr std::string_view MSG_ERROR      = "$$error";
constexpr std::string_view MSG_HANDSHAKE  = "$$handshake";
constexpr std::string_view MSG_INFO       = "$$info";
constexpr std::string_view MSG_SERVER     = "$$server";
constexpr std::string_view MSG_SUBSCRIBE  = "$$subscribe";
constexpr std::string_view MSG_YOU        = "$$you";

constexpr uint32_t MAX_MESSAGE_LENGTH = 1024 * 32;
constexpr uint16_t DEFAULT_PORT = 1637;

constexpr uint8_t CURRENT_VERSION        = 0;
constexpr uint8_t MIN_COMPATIBLE_VERSION = 0;

using nlohmann::json;

class Client;

enum AddressType { UNIX, INTERNET, INTERNAL };
enum MessageFormat { JSON = 0, MSGPACK = 1 };

enum EventType { NEW_CONNECTION, READ_READY, TIMEOUT, INTERRUPT };

enum LogLevel { DEBUG = 0, INFO = 1, WARNING = 2, SEVERE = 3 };

template<auto Deleter>
struct LibeventDeleter
{
    constexpr void operator()(auto* ev) { Deleter(ev); }
};

template<typename T>
T make(typename T::element_type* ptr) { return T { ptr }; }

using UEvent = std::unique_ptr<event, LibeventDeleter<event_free>>;
using UEventBase = std::unique_ptr<event_base,
                                   LibeventDeleter<event_base_free>>;
using UEvconnListener = std::unique_ptr<evconnlistener,
                                        LibeventDeleter<evconnlistener_free>>;

struct EventCallbackData
{
    sockaddr address;
    event_base* ebase;
    int fd;
    EventType type;
    int addr_len;
};

struct Message
{
    std::string dest, src, type;
    json content;
    bool only_first = false;

    std::string Serialise(MessageFormat f) const;
    static Message Deserialise(MessageFormat f, std::string_view data);
    static bool WriteToStream(FILE* stream, const Message& m, MessageFormat f);
};

void to_json(json& j, const Message& msg);
void from_json(const json& j, Message& msg);

struct ClientPreferences
{
    MessageFormat format = JSON;
};

using Handler = std::function<void(Client&, const Message&)>;
using LogCallback = void (*)(LogLevel, std::string_view);
using SignalHandler = void (*)(int);

// Must be called to initialise libevent, logging and SIGPIPE handling
void Initialise(LogCallback cb = nullptr, SignalHandler sh = nullptr);

constinit inline LogCallback logger = nullptr;

using nlohmann::json;

inline const ValidationPair VERSION_CHECK = {
    "/version"_json_pointer,
    predicates::GreaterEq<MIN_COMPATIBLE_VERSION>
};

inline const ValidationSeries VALIDATE_HANDSHAKE_SERVERSIDE = {
    { "/teamname"_json_pointer, predicates::NotEmpty },
    { "/format"_json_pointer, predicates::Matches({ JSON, MSGPACK }) },
    VERSION_CHECK
};

inline const ValidationSeries VALIDATE_HANDSHAKE_CLIENTSIDE = {
    VERSION_CHECK
};

inline const ValidationSeries VALIDATE_AVAILABLE = {
    { "/type"_json_pointer, predicates::NotEmpty },
    { "/available"_json_pointer, predicates::IsBool }
};

inline const ValidationSeries VALIDATE_SERVER_MESSAGE = {
    { ""_json_pointer, predicates::NotEmpty }
};

namespace callbacks {

constexpr timeval DEFAULT_TIMEOUT = { 60, 0 };

void ConnectionCallback(evconnlistener* listener, evutil_socket_t fd,
                        sockaddr* addr, int addr_len, void* data);

void ReadCallback(evutil_socket_t fd, short what, void* data);

void LoopInterruptCallback(evutil_socket_t fd, short what, void* data);

}

}
