// SPDX-License-Identifier: BSD-2-Clause
// author: Max Kellermann <max.kellermann@gmail.com>

#pragma once

#include "system/Error.hxx"

#ifdef _WIN32
#include <winsock2.h>
typedef DWORD socket_error_t;
#else
#include <cerrno>
typedef int socket_error_t;
#endif

[[gnu::pure]]
static inline socket_error_t
GetSocketError() noexcept
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

constexpr bool
IsSocketErrorInProgress(socket_error_t code) noexcept
{
#ifdef _WIN32
	return code == WSAEINPROGRESS;
#else
	return code == EINPROGRESS;
#endif
}

constexpr bool
IsSocketErrorWouldBlock(socket_error_t code) noexcept
{
#ifdef _WIN32
	return code == WSAEWOULDBLOCK;
#else
	return code == EWOULDBLOCK;
#endif
}

constexpr bool
IsSocketErrorConnectWouldBlock(socket_error_t code) noexcept
{
#if defined(_WIN32) || defined(__linux__)
	/* on Windows, WSAEINPROGRESS is for blocking sockets and
	   WSAEWOULDBLOCK for non-blocking sockets */
	/* on Linux, EAGAIN==EWOULDBLOCK is for local sockets and
	   EINPROGRESS is for all other sockets */
	return IsSocketErrorInProgress(code) || IsSocketErrorWouldBlock(code);
#else
	/* on all other operating systems, there's just EINPROGRESS */
	return IsSocketErrorInProgress(code);
#endif
}

constexpr bool
IsSocketErrorSendWouldBlock(socket_error_t code) noexcept
{
#ifdef _WIN32
	/* on Windows, WSAEINPROGRESS is for blocking sockets and
	   WSAEWOULDBLOCK for non-blocking sockets */
	return IsSocketErrorInProgress(code) || IsSocketErrorWouldBlock(code);
#else
	/* on all other operating systems, there's just EAGAIN==EWOULDBLOCK */
	return IsSocketErrorWouldBlock(code);
#endif
}

constexpr bool
IsSocketErrorReceiveWouldBlock(socket_error_t code) noexcept
{
#ifdef _WIN32
	/* on Windows, WSAEINPROGRESS is for blocking sockets and
	   WSAEWOULDBLOCK for non-blocking sockets */
	return IsSocketErrorInProgress(code) || IsSocketErrorWouldBlock(code);
#else
	/* on all other operating systems, there's just
	   EAGAIN==EWOULDBLOCK */
	return IsSocketErrorWouldBlock(code);
#endif
}

constexpr bool
IsSocketErrorAcceptWouldBlock(socket_error_t code) noexcept
{
#ifdef _WIN32
	/* on Windows, WSAEINPROGRESS is for blocking sockets and
	   WSAEWOULDBLOCK for non-blocking sockets */
	return IsSocketErrorInProgress(code) || IsSocketErrorWouldBlock(code);
#else
	/* on all other operating systems, there's just
	   EAGAIN==EWOULDBLOCK */
	return IsSocketErrorWouldBlock(code);
#endif
}

constexpr bool
IsSocketErrorInterrupted(socket_error_t code) noexcept
{
#ifdef _WIN32
	return code == WSAEINTR;
#else
	return code == EINTR;
#endif
}

constexpr bool
IsSocketErrorClosed(socket_error_t code) noexcept
{
#ifdef _WIN32
	return code == WSAECONNRESET;
#else
	return code == EPIPE || code == ECONNRESET;
#endif
}

constexpr bool
IsSocketErrorTimeout(socket_error_t code) noexcept
{
#ifdef _WIN32
	return code == WSAETIMEDOUT;
#else
	return code == ETIMEDOUT;
#endif
}

/**
 * Helper class that formats a socket error message into a
 * human-readable string.  On Windows, a buffer is necessary for this,
 * and this class hosts the buffer.
 */
class SocketErrorMessage {
#ifdef _WIN32
	static constexpr unsigned msg_size = 256;
	char msg[msg_size];
#else
	const char *const msg;
#endif

public:
	explicit SocketErrorMessage(socket_error_t code=GetSocketError()) noexcept;

	operator const char *() const {
		return msg;
	}
};

/**
 * Returns the error_category to be used to wrap socket errors.
 */
[[gnu::const]]
static inline const std::error_category &
SocketErrorCategory() noexcept
{
#ifdef _WIN32
	return LastErrorCategory();
#else
	return ErrnoCategory();
#endif
}

[[gnu::pure]]
static inline bool
IsSocketError(const std::system_error &e) noexcept
{
	return e.code().category() == SocketErrorCategory();
}

[[gnu::pure]]
static inline bool
IsSocketError(const std::system_error &e, socket_error_t code) noexcept
{
	return IsSocketError(e) && static_cast<socket_error_t>(e.code().value()) == code;
}

[[gnu::pure]]
static inline bool
IsSocketErrorReceiveWouldBlock(const std::system_error &e) noexcept
{
	return IsSocketError(e) &&
		IsSocketErrorReceiveWouldBlock(e.code().value());
}

[[gnu::pure]]
static inline auto
MakeSocketError(socket_error_t code, const char *msg) noexcept
{
#ifdef _WIN32
	return MakeLastError(code, msg);
#else
	return MakeErrno(code, msg);
#endif
}

[[gnu::pure]]
static inline auto
MakeSocketError(const char *msg) noexcept
{
	return MakeSocketError(GetSocketError(), msg);
}
