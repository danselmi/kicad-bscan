// SPDX-License-Identifier: GPL-3.0-only
#include "kicad/ipc_client.h"

#include <cstdlib>
#include <filesystem>

#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>

#include <common/envelope.pb.h>

namespace kicad
{

using kiapi::common::ApiRequest;
using kiapi::common::ApiResponse;
using kiapi::common::ApiStatusCode;


const char* ToString( ErrorKind aKind )
{
    switch( aKind )
    {
    case ErrorKind::NotConnected:  return "not connected";
    case ErrorKind::Timeout:       return "timeout";
    case ErrorKind::Busy:          return "busy";
    case ErrorKind::NotReady:      return "not ready";
    case ErrorKind::TokenMismatch: return "token mismatch";
    case ErrorKind::Unhandled:     return "unhandled";
    case ErrorKind::BadRequest:    return "bad request";
    case ErrorKind::Unimplemented: return "unimplemented";
    case ErrorKind::Protocol:      return "protocol error";
    case ErrorKind::Other:         return "error";
    }

    return "?";
}


struct Client::Socket
{
    nng_socket sock = NNG_SOCKET_INITIALIZER;
    bool       open = false;
};


static std::string getEnv( const char* aName )
{
    const char* v = std::getenv( aName );
    return v ? std::string( v ) : std::string();
}


static std::string defaultSocketUrl()
{
    // Same location KiCad uses: <temp dir>/kicad/api.sock (KiCad appends its PID
    // if the default name is taken, which a hand-started client cannot know).
    std::filesystem::path tmp;

#ifdef _WIN32
    for( const char* var : { "TMP", "TEMP", "USERPROFILE" } )
    {
        if( std::string v = getEnv( var ); !v.empty() )
        {
            tmp = v;
            break;
        }
    }
#else
    for( const char* var : { "TMPDIR", "TMP", "TEMP" } )
    {
        if( std::string v = getEnv( var ); !v.empty() )
        {
            tmp = v;
            break;
        }
    }

    if( tmp.empty() )
        tmp = "/tmp";
#endif

    return "ipc://" + ( tmp / "kicad" / "api.sock" ).string();
}


Client::Config Client::ConfigFromEnvironment( const std::string& aClientName )
{
    Config cfg;
    cfg.socketUrl  = getEnv( "KICAD_API_SOCKET" );
    cfg.token      = getEnv( "KICAD_API_TOKEN" );
    cfg.clientName = aClientName;

    if( cfg.socketUrl.empty() )
        cfg.socketUrl = defaultSocketUrl();
    else if( cfg.socketUrl.find( "://" ) == std::string::npos )
        cfg.socketUrl = "ipc://" + cfg.socketUrl;   // KiCad passes a full URL; be lenient

    return cfg;
}


Client::Client( Config aConfig ) :
        m_config( std::move( aConfig ) ),
        m_socket( std::make_unique<Socket>() )
{
}


Client::~Client()
{
    close();
}


void Client::close()
{
    if( m_socket->open )
    {
        nng_close( m_socket->sock );
        m_socket->open = false;
    }
}


Result<void> Client::ensureOpen()
{
    if( m_socket->open )
        return {};

    if( int rv = nng_req0_open( &m_socket->sock ); rv != 0 )
        return std::unexpected( Error{ ErrorKind::Other,
                                       std::string( "nng_req0_open: " ) + nng_strerror( rv ) } );

    nng_socket_set_ms( m_socket->sock, NNG_OPT_SENDTIMEO, m_config.timeoutMs );
    nng_socket_set_ms( m_socket->sock, NNG_OPT_RECVTIMEO, m_config.timeoutMs );

    // Non-blocking dial: NNG keeps retrying in the background, so the client
    // connects as soon as KiCad's socket appears (and reconnects after a drop).
    if( int rv = nng_dial( m_socket->sock, m_config.socketUrl.c_str(), nullptr, NNG_FLAG_NONBLOCK );
        rv != 0 )
    {
        nng_close( m_socket->sock );
        return std::unexpected( Error{ ErrorKind::NotConnected,
                                       "dial " + m_config.socketUrl + ": " + nng_strerror( rv ) } );
    }

    m_socket->open = true;
    return {};
}


static Error statusToError( const kiapi::common::ApiResponseStatus& aStatus )
{
    ErrorKind kind = ErrorKind::Other;

    switch( aStatus.status() )
    {
    case ApiStatusCode::AS_TIMEOUT:        kind = ErrorKind::Timeout;       break;
    case ApiStatusCode::AS_BAD_REQUEST:    kind = ErrorKind::BadRequest;    break;
    case ApiStatusCode::AS_NOT_READY:      kind = ErrorKind::NotReady;      break;
    case ApiStatusCode::AS_UNHANDLED:      kind = ErrorKind::Unhandled;     break;
    case ApiStatusCode::AS_TOKEN_MISMATCH: kind = ErrorKind::TokenMismatch; break;
    case ApiStatusCode::AS_BUSY:           kind = ErrorKind::Busy;          break;
    case ApiStatusCode::AS_UNIMPLEMENTED:  kind = ErrorKind::Unimplemented; break;
    default:                               kind = ErrorKind::Other;         break;
    }

    return Error{ kind, aStatus.error_message() };
}


Result<void> Client::call( const google::protobuf::Message& aRequest,
                           google::protobuf::Message&       aResponse )
{
    std::lock_guard lock( m_mutex );

    if( auto r = ensureOpen(); !r )
        return r;

    ApiRequest envelope;
    envelope.mutable_header()->set_kicad_token( m_config.token );
    envelope.mutable_header()->set_client_name( m_config.clientName );
    if( !envelope.mutable_message()->PackFrom( aRequest ) )
        return std::unexpected( Error{ ErrorKind::Protocol, "could not pack request" } );

    std::string out;

    if( !envelope.SerializeToString( &out ) )
        return std::unexpected( Error{ ErrorKind::Protocol, "could not serialise request" } );

    if( int rv = nng_send( m_socket->sock, out.data(), out.size(), 0 ); rv != 0 )
    {
        // A send timeout on a REQ socket means no peer is connected.
        ErrorKind kind = ( rv == NNG_ETIMEDOUT ) ? ErrorKind::NotConnected : ErrorKind::Other;
        return std::unexpected( Error{ kind, std::string( "send: " ) + nng_strerror( rv ) } );
    }

    char*  buf = nullptr;
    size_t len = 0;

    if( int rv = nng_recv( m_socket->sock, &buf, &len, NNG_FLAG_ALLOC ); rv != 0 )
    {
        ErrorKind kind = ( rv == NNG_ETIMEDOUT ) ? ErrorKind::Timeout : ErrorKind::Other;
        return std::unexpected( Error{ kind, std::string( "receive: " ) + nng_strerror( rv ) } );
    }

    ApiResponse reply;
    bool        parsed = reply.ParseFromArray( buf, static_cast<int>( len ) );
    nng_free( buf, len );

    if( !parsed )
        return std::unexpected( Error{ ErrorKind::Protocol, "could not parse reply" } );

    if( reply.status().status() != ApiStatusCode::AS_OK )
        return std::unexpected( statusToError( reply.status() ) );

    if( !reply.message().UnpackTo( &aResponse ) )
    {
        return std::unexpected( Error{ ErrorKind::Protocol,
                                       "unexpected reply type " + reply.message().type_url()
                                               + ", expected "
                                               + std::string( aResponse.GetTypeName() ) } );
    }

    return {};
}

} // namespace kicad
