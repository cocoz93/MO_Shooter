#pragma once

#include "GameProtocol.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

// 콘솔 클라용 TCP 한 줄기. 받기는 막지 않고 Poll 에서 패킷 단위로 넘긴다.
namespace mo
{
	class CNetClient
	{
	public:
		CNetClient()
		{
			WSADATA data;
			WSAStartup(MAKEWORD(2, 2), &data);
		}

		~CNetClient()
		{
			Close();
			WSACleanup();
		}

		CNetClient(const CNetClient&) = delete;
		CNetClient& operator=(const CNetClient&) = delete;

		bool Connect(const char* host, int port)
		{
			_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (_socket == INVALID_SOCKET) return false;
			sockaddr_in address = {};
			address.sin_family = AF_INET;
			address.sin_port = htons((u_short)port);
			inet_pton(AF_INET, host, &address.sin_addr);
			if (connect(_socket, (const sockaddr*)&address, sizeof(address)) != 0)
			{
				Close();
				return false;
			}
			const BOOL noDelay = TRUE;
			setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
			u_long nonBlocking = 1;
			ioctlsocket(_socket, FIONBIO, &nonBlocking);
			_buffer.clear();
			return true;
		}

		void Close()
		{
			if (_socket != INVALID_SOCKET)
			{
				closesocket(_socket);
				_socket = INVALID_SOCKET;
			}
		}

		bool IsOpen() const { return _socket != INVALID_SOCKET; }

		bool Send(const void* data, int size)
		{
			const char* cursor = static_cast<const char*>(data);
			int left = size;
			while (left > 0)
			{
				const int sent = send(_socket, cursor, left, 0);
				if (sent > 0)
				{
					cursor += sent;
					left -= sent;
					continue;
				}
				if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
				std::this_thread::yield();
			}
			return true;
		}

		template <class T>
		bool SendPacket(T& packet, wire::Msg type)
		{
			packet.h.size = (uint16_t)sizeof(T);
			packet.h.type = (uint16_t)type;
			return Send(&packet, (int)sizeof(T));
		}

		// 반환 false = 끊김. handler(const wire::Header&, const char* data, int size)
		template <class Handler>
		bool Poll(Handler&& handler)
		{
			char chunk[8192];
			for (;;)
			{
				const int received = recv(_socket, chunk, sizeof(chunk), 0);
				if (received > 0)
				{
					_buffer.insert(_buffer.end(), chunk, chunk + received);
					continue;
				}
				if (received == 0) return false;
				if (WSAGetLastError() == WSAEWOULDBLOCK) break;
				return false;
			}

			size_t offset = 0;
			while (_buffer.size() - offset >= sizeof(wire::Header))
			{
				wire::Header header;
				std::memcpy(&header, _buffer.data() + offset, sizeof(header));
				if (header.size < sizeof(wire::Header)) return false;
				if (_buffer.size() - offset < header.size) break;
				handler(header, _buffer.data() + offset, (int)header.size);
				offset += header.size;
			}
			_buffer.erase(_buffer.begin(), _buffer.begin() + (std::ptrdiff_t)offset);
			return true;
		}

	private:
		SOCKET _socket = INVALID_SOCKET;
		std::vector<char> _buffer;
	};
}
