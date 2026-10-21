// SPDX-License-Identifier: GPL-3.0-only
#include "openocd/tcl_client.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t BAD_SOCKET = INVALID_SOCKET;
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t BAD_SOCKET = -1;
#endif

namespace openocd
{

static constexpr char TERMINATOR = '\x1a';

// Upper bound for a reply; a boundary register of a big FPGA is a few
// thousand bits, i.e. a few kB of hex. OpenOCD itself limits lines to 4 MB.
static constexpr size_t MAX_REPLY = 8 * 1024 * 1024;


const char* ToString( ErrorKind aKind )
{
    switch( aKind )
    {
    case ErrorKind::NotConnected: return "not connected";
    case ErrorKind::Timeout:      return "timeout";
    case ErrorKind::Command:      return "command failed";
    case ErrorKind::Protocol:     return "protocol error";
    }

    return "?";
}


// --- platform helpers ---------------------------------------------------------

#ifdef _WIN32
struct WinsockInit
{
    WinsockInit()
    {
        WSADATA d;
        WSAStartup( MAKEWORD( 2, 2 ), &d );
    }
    ~WinsockInit() { WSACleanup(); }
};

static void closeSocket( socket_t s ) { closesocket( s ); }
static int  lastError() { return WSAGetLastError(); }
static bool wouldBlock( int e ) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }

static void setNonBlocking( socket_t s )
{
    u_long on = 1;
    ioctlsocket( s, FIONBIO, &on );
}

static int pollOne( socket_t s, short events, int timeoutMs )
{
    WSAPOLLFD p{ s, events, 0 };
    return WSAPoll( &p, 1, timeoutMs ) > 0 ? p.revents : 0;
}
#else
static void closeSocket( socket_t s ) { ::close( s ); }
static int  lastError() { return errno; }
static bool wouldBlock( int e ) { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS; }

static void setNonBlocking( socket_t s )
{
    int flags = fcntl( s, F_GETFL, 0 );
    fcntl( s, F_SETFL, flags | O_NONBLOCK );
}

static int pollOne( socket_t s, short events, int timeoutMs )
{
    pollfd p{ s, events, 0 };
    return ::poll( &p, 1, timeoutMs ) > 0 ? p.revents : 0;
}
#endif


struct TclClient::Socket
{
#ifdef _WIN32
    WinsockInit winsock;
#endif
    socket_t    fd = BAD_SOCKET;
    std::string pending;   // bytes received after a terminator (should stay empty)

    ~Socket() { close(); }

    void close()
    {
        if( fd != BAD_SOCKET )
            closeSocket( fd );

        fd = BAD_SOCKET;
        pending.clear();
    }
};


TclClient::TclClient( Config aConfig ) :
        m_config( std::move( aConfig ) ),
        m_socket( std::make_unique<Socket>() )
{
}


TclClient::~TclClient() = default;


void TclClient::Disconnect()
{
    std::lock_guard lock( m_mutex );
    m_socket->close();
}


Result<void> TclClient::connect()
{
    if( m_socket->fd != BAD_SOCKET )
        return {};

    m_commands.clear();   // may be another OpenOCD now

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo*   res  = nullptr;
    std::string port = std::to_string( m_config.port );

    if( int rv = getaddrinfo( m_config.host.c_str(), port.c_str(), &hints, &res ); rv != 0 )
    {
        return std::unexpected( Error{ ErrorKind::NotConnected,
                                       "cannot resolve " + m_config.host + ": "
                                               + gai_strerror( rv ) } );
    }

    std::string lastMsg = "no address";

    for( addrinfo* ai = res; ai; ai = ai->ai_next )
    {
        socket_t s = ::socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );

        if( s == BAD_SOCKET )
            continue;

        setNonBlocking( s );

        int rv = ::connect( s, ai->ai_addr, static_cast<int>( ai->ai_addrlen ) );

        if( rv != 0 && wouldBlock( lastError() ) )
        {
            int ev = pollOne( s, POLLOUT, m_config.timeoutMs );
            int err = 0;
            socklen_t len = sizeof( err );
            getsockopt( s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>( &err ), &len );
            rv = ( ev & POLLOUT ) && err == 0 ? 0 : -1;

            if( rv != 0 )
                lastMsg = err ? std::strerror( err ) : "connect timeout";
        }
        else if( rv != 0 )
        {
            lastMsg = std::strerror( lastError() );
        }

        if( rv == 0 )
        {
            int one = 1;
            setsockopt( s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>( &one ),
                        sizeof( one ) );
            m_socket->fd = s;
            break;
        }

        closeSocket( s );
    }

    freeaddrinfo( res );

    if( m_socket->fd == BAD_SOCKET )
    {
        return std::unexpected( Error{ ErrorKind::NotConnected,
                                       m_config.host + ":" + port + ": " + lastMsg } );
    }

    return {};
}


Result<std::string> TclClient::exchange( const std::string& aLine )
{
    using clock   = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds( m_config.timeoutMs );

    auto remainingMs = [&]
    {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - clock::now() );
        return static_cast<int>( std::max<long long>( 0, ms.count() ) );
    };

    auto fail = [&]( ErrorKind aKind, const std::string& aMsg ) -> Result<std::string>
    {
        // After a timeout or error the stream position is unknown: start over.
        m_socket->close();
        return std::unexpected( Error{ aKind, aMsg } );
    };

    // --- send ---
    std::string out = aLine + TERMINATOR;
    size_t      sent = 0;

    while( sent < out.size() )
    {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;   // no SIGPIPE if OpenOCD went away
#else
        constexpr int flags = 0;
#endif
        auto n = ::send( m_socket->fd, out.data() + sent, static_cast<int>( out.size() - sent ),
                         flags );

        if( n > 0 )
        {
            sent += static_cast<size_t>( n );
            continue;
        }

        if( n < 0 && wouldBlock( lastError() ) )
        {
            if( !( pollOne( m_socket->fd, POLLOUT, remainingMs() ) & POLLOUT ) )
                return fail( ErrorKind::Timeout, "send timeout" );

            continue;
        }

        return fail( ErrorKind::NotConnected, "connection lost (send)" );
    }

    // --- receive until terminator ---
    std::string reply = std::move( m_socket->pending );
    m_socket->pending.clear();

    for( ;; )
    {
        if( size_t pos = reply.find( TERMINATOR ); pos != std::string::npos )
        {
            m_socket->pending = reply.substr( pos + 1 );
            reply.resize( pos );
            return reply;
        }

        if( reply.size() > MAX_REPLY )
            return fail( ErrorKind::Protocol, "reply too long" );

        int ev = pollOne( m_socket->fd, POLLIN, remainingMs() );

        if( !( ev & ( POLLIN | POLLHUP | POLLERR ) ) )
            return fail( ErrorKind::Timeout, "no reply from OpenOCD" );

        char buf[16384];
        auto n = ::recv( m_socket->fd, buf, sizeof( buf ), 0 );

        if( n > 0 )
            reply.append( buf, static_cast<size_t>( n ) );
        else if( n < 0 && wouldBlock( lastError() ) )
            continue;
        else
            return fail( ErrorKind::NotConnected, "connection closed by OpenOCD" );
    }
}


Result<bool> TclClient::HasCommand( const std::string& aCommand )
{
    {
        std::lock_guard lock( m_mutex );

        if( m_socket->fd != BAD_SOCKET )
            if( auto it = m_commands.find( aCommand ); it != m_commands.end() )
                return it->second;
    }

    // 'usage' prints nothing for unknown commands. (Calling an unknown
    // subcommand would print the usage of the whole command group instead
    // of "invalid command", so that cannot be used.)
    auto text = Run( "usage {" + aCommand + "}" );

    if( !text )
        return std::unexpected( text.error() );

    bool has = text->find( aCommand ) != std::string::npos;

    std::lock_guard lock( m_mutex );
    m_commands[aCommand] = has;
    return has;
}


Result<std::string> TclClient::Run( const std::string& aCommand )
{
    std::lock_guard lock( m_mutex );

    if( auto r = connect(); !r )
        return std::unexpected( r.error() );

    // "<code>:<result>", code 0 = ok, 1 = error (Tcl catch return codes)
    std::string line = "format \"%d:%s\" [catch {" + aCommand + "} _bscan_r] $_bscan_r";

    auto reply = exchange( line );

    if( !reply )
        return reply;

    size_t colon = reply->find( ':' );

    if( colon == std::string::npos || colon == 0 )
        return std::unexpected( Error{ ErrorKind::Protocol, "unexpected reply: " + *reply } );

    std::string code = reply->substr( 0, colon );
    std::string text = reply->substr( colon + 1 );

    if( code == "0" )
        return text;

    // Strip trailing newlines of OpenOCD error messages
    while( !text.empty() && ( text.back() == '\n' || text.back() == '\r' ) )
        text.pop_back();

    return std::unexpected( Error{ ErrorKind::Command, text } );
}

} // namespace openocd
