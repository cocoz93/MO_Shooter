// TCP relay that delays both directions, with optional jitter. Order is kept (a later chunk never overtakes an earlier one).
// Why: every other test runs on loopback at zero delay; prediction and correction only show their edges with a real round trip.
//
//   LagRelay --listen=N --target=N --delayMs=N [--jitterMs=N] [--seconds=N] [--seed=N]
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

namespace
{
	using Clock = std::chrono::steady_clock;

	struct Chunk
	{
		Clock::time_point due;
		std::vector<char> bytes;
		size_t sent = 0;
	};

	struct Direction
	{
		SOCKET from = INVALID_SOCKET;
		SOCKET to = INVALID_SOCKET;
		std::deque<Chunk> queue;
		Clock::time_point lastDue = Clock::now();
		bool fromClosed = false;
		uint64_t bytes = 0;
	};

	struct Link
	{
		Direction up, down;
		bool dead = false;
	};

	void Configure(SOCKET s)
	{
		const BOOL noDelay = TRUE;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
		u_long nonBlocking = 1;
		ioctlsocket(s, FIONBIO, &nonBlocking);
	}

	void Close(SOCKET& s)
	{
		if (s != INVALID_SOCKET) closesocket(s);
		s = INVALID_SOCKET;
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	int listenPort = 0, targetPort = 0, delayMs = 0, jitterMs = 0;
	double seconds = 0;
	uint32_t seed = 1;
	for (int i = 1; i < argc; ++i)
	{
		const std::string s = argv[i];
		if (s.rfind("--listen=", 0) == 0) listenPort = std::atoi(s.c_str() + 9);
		else if (s.rfind("--target=", 0) == 0) targetPort = std::atoi(s.c_str() + 9);
		else if (s.rfind("--delayMs=", 0) == 0) delayMs = std::atoi(s.c_str() + 10);
		else if (s.rfind("--jitterMs=", 0) == 0) jitterMs = std::atoi(s.c_str() + 11);
		else if (s.rfind("--seconds=", 0) == 0) seconds = std::atof(s.c_str() + 10);
		else if (s.rfind("--seed=", 0) == 0) seed = (uint32_t)std::atoi(s.c_str() + 7);
		else
		{
			std::printf("unknown argument %s\n", s.c_str());
			return 2;
		}
	}
	if (listenPort == 0 || targetPort == 0)
	{
		std::printf("--listen and --target are required\n");
		return 2;
	}

	WSADATA data;
	WSAStartup(MAKEWORD(2, 2), &data);
	timeBeginPeriod(1);

	SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons((u_short)listenPort);
	inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
	if (bind(listener, (const sockaddr*)&address, sizeof(address)) != 0 || listen(listener, 64) != 0)
	{
		std::printf("listen failed %d\n", WSAGetLastError());
		return 1;
	}
	u_long nonBlocking = 1;
	ioctlsocket(listener, FIONBIO, &nonBlocking);

	std::mt19937 rng(seed);
	std::uniform_int_distribution<int> jitter(0, std::max(0, jitterMs));
	std::vector<std::unique_ptr<Link>> links;
	uint64_t accepted = 0, upBytes = 0, downBytes = 0;
	double worstLateMs = 0;
	const auto start = Clock::now();

	const auto pump = [&](Direction& d, Link& link)
	{
		char chunk[65536];
		while (!d.fromClosed)
		{
			const int received = recv(d.from, chunk, sizeof(chunk), 0);
			if (received > 0)
			{
				Chunk c;
				c.due = std::max(d.lastDue, Clock::now() + std::chrono::milliseconds(delayMs + jitter(rng)));
				d.lastDue = c.due;
				c.bytes.assign(chunk, chunk + received);
				d.queue.push_back(std::move(c));
				d.bytes += (uint64_t)received;
				continue;
			}
			if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) d.fromClosed = true;
			break;
		}
		const auto now = Clock::now();
		while (!d.queue.empty() && d.queue.front().due <= now)
		{
			Chunk& c = d.queue.front();
			const int sent = send(d.to, c.bytes.data() + c.sent, (int)(c.bytes.size() - c.sent), 0);
			if (sent > 0)
			{
				c.sent += (size_t)sent;
				if (c.sent == c.bytes.size())
				{
					worstLateMs = std::max(worstLateMs, std::chrono::duration<double, std::milli>(Clock::now() - c.due).count());
					d.queue.pop_front();
				}
				continue;
			}
			if (WSAGetLastError() != WSAEWOULDBLOCK) link.dead = true;
			break;
		}
		// Hold the close back until the bytes ahead of it have gone out, so a close never overtakes data
		if (d.fromClosed && d.queue.empty()) link.dead = true;
	};

	while (seconds <= 0 || std::chrono::duration<double>(Clock::now() - start).count() < seconds)
	{
		const SOCKET client = accept(listener, nullptr, nullptr);
		if (client != INVALID_SOCKET)
		{
			SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			sockaddr_in target = {};
			target.sin_family = AF_INET;
			target.sin_port = htons((u_short)targetPort);
			inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
			if (connect(server, (const sockaddr*)&target, sizeof(target)) == 0)
			{
				Configure(client);
				Configure(server);
				std::unique_ptr<Link> link(new Link());
				link->up.from = client;
				link->up.to = server;
				link->down.from = server;
				link->down.to = client;
				links.push_back(std::move(link));
				++accepted;
			}
			else
			{
				closesocket(server);
				closesocket(client);
			}
		}
		for (std::unique_ptr<Link>& link : links)
		{
			pump(link->up, *link);
			pump(link->down, *link);
		}
		for (size_t i = 0; i < links.size();)
		{
			if (links[i]->dead)
			{
				upBytes += links[i]->up.bytes;
				downBytes += links[i]->down.bytes;
				SOCKET a = links[i]->up.from, b = links[i]->up.to;
				Close(a);
				Close(b);
				links.erase(links.begin() + (std::ptrdiff_t)i);
				continue;
			}
			++i;
		}
		if (accepted > 0 && links.empty() && seconds <= 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	for (std::unique_ptr<Link>& link : links)
	{
		upBytes += link->up.bytes;
		downBytes += link->down.bytes;
	}
	std::printf("RELAY delayMs=%d jitterMs=%d accepted=%llu upBytes=%llu downBytes=%llu worstLateMs=%.1f\n", delayMs, jitterMs, (unsigned long long)accepted, (unsigned long long)upBytes, (unsigned long long)downBytes, worstLateMs);
	timeEndPeriod(1);
	WSACleanup();
	return 0;
}
