// SPDX-License-Identifier: GPL-3.0-only
// Minimal C++ client for the KiCad IPC API (KiCad >= 10.0.6).
//
// KiCad runs an NNG REP socket (ipc://<tmp>/kicad/api.sock). Every request is
// an ApiRequest envelope carrying the command as google.protobuf.Any; every
// reply is an ApiResponse envelope with a status and the response as Any.

#pragma once

#include <expected>
#include <memory>
#include <mutex>
#include <string>

#include <google/protobuf/message.h>

namespace kicad
{

enum class ErrorKind
{
    NotConnected,   // socket could not be opened / KiCad not running
    Timeout,        // no reply in time (KiCad blocked, or gone)
    Busy,           // AS_BUSY: KiCad is in an interactive operation, try later
    NotReady,       // AS_NOT_READY: KiCad has just started
    TokenMismatch,  // AS_TOKEN_MISMATCH: a different KiCad instance answers
    Unhandled,      // AS_UNHANDLED: no handler, e.g. wrong document / editor closed
    BadRequest,     // AS_BAD_REQUEST
    Unimplemented,  // AS_UNIMPLEMENTED
    Protocol,       // malformed reply, wrong response type, ...
    Other
};

struct Error
{
    ErrorKind   kind;
    std::string message;
};

const char* ToString( ErrorKind aKind );

template <typename T>
using Result = std::expected<T, Error>;

class Client
{
public:
    struct Config
    {
        std::string socketUrl;    // e.g. "ipc:///tmp/kicad/api.sock"
        std::string token;        // KICAD_API_TOKEN, may be empty
        std::string clientName;   // shown in KiCad's API log
        int         timeoutMs = 2000;
    };

    // Socket and token from KICAD_API_SOCKET / KICAD_API_TOKEN (set by KiCad
    // when it launches a plugin). Falls back to the default socket path so the
    // program can also be started by hand while KiCad is running.
    static Config ConfigFromEnvironment( const std::string& aClientName );

    explicit Client( Config aConfig );
    ~Client();

    Client( const Client& ) = delete;
    Client& operator=( const Client& ) = delete;

    const Config& GetConfig() const { return m_config; }

    // Sends aRequest and unpacks the reply into Resp.
    // Thread-safe, but calls are serialised (one outstanding request).
    template <typename Resp>
    Result<Resp> Call( const google::protobuf::Message& aRequest )
    {
        Resp resp;

        if( auto r = call( aRequest, resp ); !r )
            return std::unexpected( r.error() );

        return resp;
    }

private:
    Result<void> call( const google::protobuf::Message& aRequest,
                       google::protobuf::Message&       aResponse );
    Result<void> ensureOpen();
    void         close();

    Config     m_config;
    std::mutex m_mutex;

    struct Socket;
    std::unique_ptr<Socket> m_socket;
};

} // namespace kicad
