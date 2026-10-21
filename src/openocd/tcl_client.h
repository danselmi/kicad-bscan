// SPDX-License-Identifier: GPL-3.0-only
// Client for OpenOCD's Tcl RPC server (default port 6666).
//
// Protocol: the client sends a command terminated by 0x1a, OpenOCD answers
// with the command's result text terminated by 0x1a. The server does not say
// whether the command failed, so every command is wrapped in Tcl 'catch' and
// the reply carries "<code>:<text>".

#pragma once

#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace openocd
{

enum class ErrorKind
{
    NotConnected,   // no connection (OpenOCD not running, wrong host/port)
    Timeout,        // no answer in time; the connection is closed
    Command,        // OpenOCD ran the command and reported an error
    Protocol,       // unexpected reply format
};

struct Error
{
    ErrorKind   kind;
    std::string message;
};

const char* ToString( ErrorKind aKind );

template <typename T>
using Result = std::expected<T, Error>;

class TclClient
{
public:
    struct Config
    {
        std::string host      = "127.0.0.1";
        int         port      = 6666;
        int         timeoutMs = 3000;
    };

    explicit TclClient( Config aConfig );
    ~TclClient();

    TclClient( const TclClient& ) = delete;
    TclClient& operator=( const TclClient& ) = delete;

    // Runs a Tcl command and returns its result text.
    // aCommand must not contain unbalanced braces (it is placed inside {...}).
    // Connects on first use and reconnects after a failure.
    Result<std::string> Run( const std::string& aCommand );

    void Disconnect();

    // True if OpenOCD has the command (e.g. "jtag scan"), via 'usage {...}'.
    // Cached until the connection is closed (another OpenOCD may answer
    // after a reconnect).
    Result<bool> HasCommand( const std::string& aCommand );

private:
    Result<void>        connect();
    Result<std::string> exchange( const std::string& aLine );

    Config     m_config;
    std::mutex m_mutex;

    struct Socket;
    std::unique_ptr<Socket> m_socket;

    std::map<std::string, bool> m_commands;   // HasCommand() cache, per connection
};

} // namespace openocd
