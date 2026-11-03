// ServerCore 새 함수와 기존 함수 경로 시험. 인자 = 포트. 접속 최대 1 이라 다시 접속하면 같은 자리를 다시 쓴다.
#include "NetIoModel.h"
#include "MonitorManager.h"
#include <Common/Logger.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
using ClientSocket = SOCKET;
static void CloseClient(ClientSocket s) { closesocket(s); }
static void SetTimeout(ClientSocket s, int ms) { DWORD v = (DWORD)ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof(v)); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using ClientSocket = int;
static void CloseClient(ClientSocket s) { close(s); }
static void SetTimeout(ClientSocket s, int ms) { timeval v{ ms / 1000, (ms % 1000) * 1000 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &v, sizeof(v)); }
#endif

void SignalProcessShutdown() {}

namespace
{
	int g_fail = 0;
	void Check(bool ok, const char* what)
	{
		std::printf("%s %s\n", ok ? "맞음" : "틀림", what);
		if (!ok) ++g_fail;
	}

	ClientSocket Connect(int port)
	{
		for (int i = 0; i < 100; ++i)
		{
			ClientSocket s = socket(AF_INET, SOCK_STREAM, 0);
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons((uint16_t)port);
			inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
			if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) return s;
			CloseClient(s);
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		std::printf("접속 실패\n");
		std::exit(2);
	}

	void SendPacket(ClientSocket s, uint16_t value)
	{
		const uint16_t p[3] = { 6, 77, value };
		send(s, (const char*)p, sizeof(p), 0);
	}

	// 반환 = 받은 값, 시간 안에 못 받으면 -1
	int RecvPacket(ClientSocket s, int ms)
	{
		SetTimeout(s, ms);
		uint16_t p[3] = {};
		int got = 0;
		while (got < (int)sizeof(p))
		{
			const int n = recv(s, (char*)p + got, (int)sizeof(p) - got, 0);
			if (n <= 0) return -1;
			got += n;
		}
		return p[2];
	}

	template <class Pop>
	bool WaitEvent(Pop pop, NetworkEvent::Type type, int64_t* session, int* value, int ms)
	{
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		while (std::chrono::steady_clock::now() < end)
		{
			NetworkEvent e(NetworkEvent::Type::CONNECTED, 0);
			if (!pop(e))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			if (value && e.pMsg)
			{
				uint16_t p[3];
				std::memcpy(p, e.pMsg->GetReadBufferPtr(), sizeof(p));
				*value = p[2];
			}
			if (e.pMsg) e.pMsg->SubRef();
			if (e.type != type) continue;
			if (session) *session = e.sessionId;
			return true;
		}
		return false;
	}
}

int main(int argc, char** argv)
{
	shared::LoggerGuard logger;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const int port = argc > 1 ? std::atoi(argv[1]) : 7970;
	std::printf("입출력 %s\n", kNetIoModelName);

	ThreadSafeQueue<NetworkEvent> myQueue;
	CMonitorManager monitor;
	NetIoModel net(port, 1, ServerMode::GameServer, monitor);
	if (!net.Start()) { std::printf("서버 시작 실패\n"); return 2; }

	const auto popShared = [&net](NetworkEvent& e) { return net.PopNetworkEvent(e); };
	const auto popMine = [&myQueue](NetworkEvent& e) { return myQueue.TryPop(e); };
	const char reply[6] = { 6, 0, 77, 0, 42, 0 };
	int64_t session = 0;
	int value = 0;

	ClientSocket client = Connect(port);
	Check(WaitEvent(popShared, NetworkEvent::Type::CONNECTED, &session, nullptr, 2000), "접속 이벤트 = 공용 큐");

	// 기존 경로 — 지정 없으면 공용 큐, 묶음 인자 없는 송신
	SendPacket(client, 11);
	Check(WaitEvent(popShared, NetworkEvent::Type::RECEIVED, nullptr, &value, 2000) && value == 11, "기존: 받은 패킷 = 공용 큐");
	net.RequestSendRaw(session, reply, sizeof(reply));
	Check(RecvPacket(client, 150) == -1, "기존: 넘기기 전에는 안 나감");
	net.FlushPendingSends();
	Check(RecvPacket(client, 2000) == 42, "기존: RequestSendRaw + FlushPendingSends 도착");

	SendPacket(client, 12);
	{
		NetworkEvent e(NetworkEvent::Type::CONNECTED, 0);
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!net.PopNetworkEvent(e) && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		Check(e.type == NetworkEvent::Type::RECEIVED && e.pMsg, "기존: 메시지 송신용 패킷 받음");
		if (e.pMsg)
		{
			net.RequestSendMsg(session, e.pMsg, SendFlush::Deferred);	// 받은 버퍼를 그대로 돌려보낸다(소유권 넘김)
			net.FlushPendingSends();
			Check(RecvPacket(client, 2000) == 12, "기존: RequestSendMsg(Deferred) + FlushPendingSends 도착");
		}
	}

	// 받을 큐 지정
	Check(net.SetSessionRecvQueue(session, &myQueue), "받을 큐 지정 성공");
	SendPacket(client, 21);
	Check(WaitEvent(popMine, NetworkEvent::Type::RECEIVED, nullptr, &value, 2000) && value == 21, "지정: 받은 패킷 = 지정한 큐");
	Check(net.GetEventQueueSize() == 0, "지정: 공용 큐에는 안 감");

	// 묶음 — 기본 묶음을 넘겨도 안 나가고 자기 묶음을 넘겨야 나간다
	CSendBatch batch;
	net.RequestSendRaw(batch, session, reply, sizeof(reply));
	net.FlushPendingSends();
	Check(RecvPacket(client, 150) == -1, "묶음: 기본 묶음을 넘겨도 안 나감");
	net.FlushPendingSends(batch);
	Check(RecvPacket(client, 2000) == 42, "묶음: 자기 묶음을 넘기면 도착");

	// 표식이 풀렸나 — 다시 쌓으면 또 나간다
	net.RequestSendRaw(batch, session, reply, sizeof(reply));
	net.FlushPendingSends(batch);
	Check(RecvPacket(client, 2000) == 42, "묶음: 두 번째도 도착");

	// nullptr 이면 공용 큐로 되돌림
	Check(net.SetSessionRecvQueue(session, nullptr), "되돌리기 성공");
	SendPacket(client, 31);
	Check(WaitEvent(popShared, NetworkEvent::Type::RECEIVED, nullptr, &value, 2000) && value == 31, "되돌림: 받은 패킷 = 공용 큐");

	// 끊김은 지정해 둬도 공용 큐, 받은 패킷이 먼저. 자리를 다시 쓰면 지정이 풀린다
	const int64_t oldSession = session;
	Check(net.SetSessionRecvQueue(session, &myQueue), "다시 지정 성공");
	SendPacket(client, 41);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	CloseClient(client);
	Check(WaitEvent(popShared, NetworkEvent::Type::DISCONNECTED, nullptr, nullptr, 3000), "끊김 이벤트 = 공용 큐");
	Check(WaitEvent(popMine, NetworkEvent::Type::RECEIVED, nullptr, &value, 10) && value == 41, "끊김 이벤트를 받았을 때 마지막 패킷은 이미 지정한 큐에 있음");
	Check(!net.SetSessionRecvQueue(oldSession, &myQueue), "끊긴 번호로 지정하면 실패");

	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	client = Connect(port);
	Check(WaitEvent(popShared, NetworkEvent::Type::CONNECTED, &session, nullptr, 2000), "다시 접속");
	Check(session != oldSession, "번호가 새것");
	SendPacket(client, 51);
	Check(WaitEvent(popShared, NetworkEvent::Type::RECEIVED, nullptr, &value, 2000) && value == 51, "자리 재사용: 지정이 풀려 공용 큐로 감");

	CloseClient(client);
	net.ShutdownServer();
	std::printf("CORE 틀림 %d건\nRESULT %s\n", g_fail, g_fail == 0 ? "PASS" : "FAIL");
	return g_fail == 0 ? 0 : 1;
}
