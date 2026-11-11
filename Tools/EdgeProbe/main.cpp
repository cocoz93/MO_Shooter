// 비정상 상황 시험 도구 — 끊김·재접속·깨진 패킷·이상한 값·몰아 보내기를 서버에 일부러 일으키고 생존과 개수를 본다.
// - 왜: 봇과 ServerProbe 는 끝까지 정상으로 도는 판과 위조 입력만 지나가고, 접속이 비정상으로 끝나는 경로는 안 지나간다.
//
//   EdgeProbe --port=N --case=<이름> [--host=주소] [--seed=N] [--serverPid=N]
//     leave_one    1명이 들어왔다가 끊김(RST). 봇과 함께 돌려 남은 사람이 멀쩡한지 본다
//     leave_team   10명 중 한 팀 전원 끊김 → 남은 5명 상태 유지 → 전원 나감 → 새 접속은 새 방
//     rejoin_all   10명 동시에 끊김 → 곧바로 10명 다시 접속(같은 방)
//     reconnect    10명 찬 방에서 1명 끊고 0.5초 뒤 다시 접속 — 빈자리에 새 번호로 앉는지
//     reconnect_fast  같은 시험을 틈 없이 — 끊김과 인사가 같은 틱에 들어가는 경우
//     overflow     25명 → 방 셋, 방마다 팀이 고른지
//     postgame     경기 끝 경계에 몰아 접속 — 전원 Welcome 을 받는지(HelloRejected 경로)
//     maxsessions  정원보다 많이 접속(서버 --maxSessions=5 로 띄움) → 끊고 나면 다시 들어오는지
//     bad_packets  크기·종류·버전이 틀린 패킷, 1바이트씩 쪼개기, 무작위 바이트 — 지켜보는 사람이 멀쩡한지
//     ghost        인사 없는 접속으로 정원을 채움 → 새 사람이 못 들어오는 시간과 끊기는 시각을 기록
//     values       유한하지만 터무니없는 값·NaN·잘못된 번호 — 서버 생존과 남이 받는 값을 기록
//     view_abuse   터무니없는 시선값을 45초 보냄 — 같은 방 Lyra 가 버티는지 볼 때
//     flood        한 명이 입력을 쉬지 않고 몰아 보냄 — 서버 메모리와 남의 상태 수신을 기록
//     flood_10·flood_100  같은 시험을 초당 10·100MB 로
//
// 돌려주는 값 = 0 전부 통과 · 1 실패 있음 · 2 인자 오류. 「EDGE 관측」 줄은 판정이 아니라 사실 기록이다.
#include "GameProtocol.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")

using namespace mo;

namespace
{
	using Clock = std::chrono::steady_clock;

	int g_fails = 0;
	std::string g_host = "127.0.0.1";

	void Check(bool ok, const std::string& text)
	{
		std::printf("EDGE %s %s\n", ok ? "통과" : "실패", text.c_str());
		if (!ok) ++g_fails;
	}

	double SecondsSince(Clock::time_point t)
	{
		return std::chrono::duration<double>(Clock::now() - t).count();
	}

	// 봇의 CNetClient 는 정상 접속만 다루므로 끊는 방식과 반쪽 전송을 고를 수 있는 소켓을 따로 둔다
	class CRaw
	{
	public:
		~CRaw() { Close(); }

		bool Connect(int port)
		{
			_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (_socket == INVALID_SOCKET) return false;
			sockaddr_in address = {};
			address.sin_family = AF_INET;
			address.sin_port = htons((u_short)port);
			inet_pton(AF_INET, g_host.c_str(), &address.sin_addr);
			if (connect(_socket, (const sockaddr*)&address, sizeof(address)) != 0)
			{
				Close();
				closed = true;
				return false;
			}
			const BOOL noDelay = TRUE;
			setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
			u_long nonBlocking = 1;
			ioctlsocket(_socket, FIONBIO, &nonBlocking);
			return true;
		}

		void Close()
		{
			if (_socket != INVALID_SOCKET) closesocket(_socket);
			_socket = INVALID_SOCKET;
		}

		// RST 로 끊는다 — 프로세스가 죽거나 선이 끊긴 것에 가깝다
		void Abort()
		{
			if (_socket != INVALID_SOCKET)
			{
				const linger hard = { 1, 0 };
				setsockopt(_socket, SOL_SOCKET, SO_LINGER, (const char*)&hard, sizeof(hard));
			}
			Close();
		}

		// 반환 = 전부 보냈나. 다 못 보내면 false(몰아 보내기에서 씀)
		bool TrySend(const void* data, int size)
		{
			if (_socket == INVALID_SOCKET) return false;
			return send(_socket, static_cast<const char*>(data), size, 0) == size;
		}

		void SendAll(const void* data, int size)
		{
			const char* cursor = static_cast<const char*>(data);
			while (size > 0 && _socket != INVALID_SOCKET)
			{
				const int sent = send(_socket, cursor, size, 0);
				if (sent > 0)
				{
					cursor += sent;
					size -= sent;
				}
				else if (WSAGetLastError() == WSAEWOULDBLOCK) std::this_thread::yield();
				else
				{
					closed = true;
					return;
				}
			}
		}

		void Pump()
		{
			if (_socket == INVALID_SOCKET) return;
			char chunk[16384];
			for (;;)
			{
				const int received = recv(_socket, chunk, sizeof(chunk), 0);
				if (received > 0)
				{
					_buffer.insert(_buffer.end(), chunk, chunk + received);
					continue;
				}
				if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
				{
					closed = true;
					Close();
				}
				break;
			}
			size_t offset = 0;
			while (_buffer.size() - offset >= sizeof(wire::Header))
			{
				wire::Header header;
				std::memcpy(&header, _buffer.data() + offset, sizeof(header));
				if (header.size < sizeof(wire::Header))
				{
					++badFromServer;
					_buffer.clear();
					return;
				}
				if (_buffer.size() - offset < header.size) break;
				OnPacket(header, _buffer.data() + offset, (int)header.size);
				offset += header.size;
			}
			_buffer.erase(_buffer.begin(), _buffer.begin() + (std::ptrdiff_t)offset);
		}

		bool closed = false;
		bool welcomed = false;
		wire::Welcome welcome = {};
		wire::State state = {};
		wire::Match match = {};
		uint64_t states = 0;
		uint64_t badFromServer = 0;
		double maxStateGap = 0;
		uint32_t seq = 0;
		bool hasMatch = false;

		const wire::PlayerState* FindSelf() const
		{
			for (int i = 0; i < state.count && i < wire::kMaxPlayersInRoom; ++i)
			{
				if (state.players[i].id == welcome.playerId) return &state.players[i];
			}
			return nullptr;
		}

	private:
		void OnPacket(const wire::Header& header, const char* data, int size)
		{
			switch ((wire::Msg)header.type)
			{
			case wire::Msg::S2C_WELCOME:
				if (size == (int)sizeof(welcome))
				{
					std::memcpy(&welcome, data, sizeof(welcome));
					welcomed = true;
				}
				break;
			case wire::Msg::S2C_STATE:
				if (size <= (int)wire::kMaxStatePacketSize)		// 클립을 준 서버는 뒤에 애니 상태가 붙는다 — 앞부분만 본다
				{
					std::memcpy(&state, data, std::min((size_t)size, sizeof(state)));
					if (states > 0) maxStateGap = std::max(maxStateGap, SecondsSince(_lastState));
					_lastState = Clock::now();
					++states;
				}
				else ++badFromServer;
				break;
			case wire::Msg::S2C_MATCH:
				if (size == (int)sizeof(match))
				{
					std::memcpy(&match, data, sizeof(match));
					hasMatch = true;
				}
				break;
			default:
				break;
			}
		}

		SOCKET _socket = INVALID_SOCKET;
		std::vector<char> _buffer;
		Clock::time_point _lastState = Clock::now();
	};

	using RawPtr = std::unique_ptr<CRaw>;

	void SendHello(CRaw& c, uint32_t version = wire::kProtocolVersion)
	{
		wire::Hello hello = {};
		hello.h.size = (uint16_t)sizeof(hello);
		hello.h.type = (uint16_t)wire::Msg::C2S_HELLO;
		hello.protocolVersion = version;
		c.SendAll(&hello, (int)sizeof(hello));
	}

	wire::Input MakeInput(CRaw& c)
	{
		wire::Input input = {};
		input.h.size = (uint16_t)sizeof(input);
		input.h.type = (uint16_t)wire::Msg::C2S_INPUT;
		input.seq = ++c.seq;
		// 시작점은 쓰지 않는 입력(사격 없음)이라 0 으로 둔다
		return input;
	}

	RawPtr Join(int port, bool hello = true)
	{
		RawPtr c(new CRaw());
		if (c->Connect(port) && hello) SendHello(*c);
		return c;
	}

	// seconds 동안 전원 수신, keepInputs 인 사람은 60Hz 로 빈 입력
	void Run(std::vector<CRaw*> clients, double seconds, bool keepInputs = true)
	{
		const auto start = Clock::now();
		auto next = start;
		while (SecondsSince(start) < seconds)
		{
			if (keepInputs && Clock::now() >= next)
			{
				next += std::chrono::microseconds(16667);
				for (CRaw* c : clients)
				{
					if (c->closed || !c->welcomed) continue;
					wire::Input input = MakeInput(*c);
					c->SendAll(&input, (int)sizeof(input));
				}
			}
			for (CRaw* c : clients) c->Pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	std::vector<CRaw*> Ptrs(const std::vector<RawPtr>& clients)
	{
		std::vector<CRaw*> out;
		for (const RawPtr& c : clients) out.push_back(c.get());
		return out;
	}

	int CountWelcomed(const std::vector<RawPtr>& clients)
	{
		int n = 0;
		for (const RawPtr& c : clients) n += c->welcomed ? 1 : 0;
		return n;
	}

	int CountClosed(const std::vector<RawPtr>& clients)
	{
		int n = 0;
		for (const RawPtr& c : clients) n += c->closed ? 1 : 0;
		return n;
	}

	std::string Fmt(const char* format, ...)
	{
		char text[512];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		return text;
	}

	void JoinMany(std::vector<RawPtr>& clients, int port, int count)
	{
		for (int i = 0; i < count; ++i) clients.push_back(Join(port));
	}

	// ─── 끊김 ─────────────────────────────────────────
	void CaseLeaveOne(int port)
	{
		RawPtr a = Join(port);
		Run({ a.get() }, 8.0);
		Check(a->welcomed, "1명 입장");
		Check(!a->closed, "8초 동안 안 끊김");
		a->Abort();
		std::printf("EDGE 관측 끊은 뒤 남은 판은 봇 쪽 RESULT 와 기록 대조로 본다\n");
	}

	void CaseLeaveTeam(int port)
	{
		std::vector<RawPtr> clients;
		JoinMany(clients, port, 10);
		Run(Ptrs(clients), 5.0);
		Check(CountWelcomed(clients) == 10, Fmt("10명 입장 %d", CountWelcomed(clients)));
		const uint32_t firstRoom = clients[0]->welcome.roomId;

		std::vector<CRaw*> stay;
		int aborted = 0;
		for (RawPtr& c : clients)
		{
			if (c->welcome.team == wire::kTeamA)
			{
				c->Abort();
				++aborted;
			}
			else stay.push_back(c.get());
		}
		Check(aborted == 5, Fmt("한 팀 5명 끊음 %d", aborted));
		uint64_t statesBefore = stay.empty() ? 0 : stay[0]->states;
		Run(stay, 5.0);
		int closedStay = 0, wrongCount = 0;
		for (CRaw* c : stay)
		{
			closedStay += c->closed ? 1 : 0;
			wrongCount += c->state.count != 5 ? 1 : 0;
		}
		Check(closedStay == 0, Fmt("남은 팀 끊긴 수 %d", closedStay));
		Check(wrongCount == 0, Fmt("남은 팀이 받는 상태 인원이 5 — 어긋난 수 %d", wrongCount));
		Check(!stay.empty() && stay[0]->states > statesBefore + 200, Fmt("상태가 계속 옴 %llu → %llu", (unsigned long long)statesBefore, stay.empty() ? 0ull : (unsigned long long)stay[0]->states));

		for (CRaw* c : stay) c->Close();
		std::this_thread::sleep_for(std::chrono::seconds(5));	// 비고 3초 뒤 방이 닫힌다
		RawPtr next = Join(port);
		Run({ next.get() }, 1.0);
		Check(next->welcomed, "전원 나간 뒤 새 접속 입장");
		Check(next->welcome.roomId != firstRoom, Fmt("새 접속은 새 방 %u → %u", firstRoom, next->welcome.roomId));
	}

	void CaseRejoinAll(int port)
	{
		std::vector<RawPtr> first;
		JoinMany(first, port, 10);
		Run(Ptrs(first), 3.0);
		Check(CountWelcomed(first) == 10, Fmt("10명 입장 %d", CountWelcomed(first)));
		const uint32_t room = first[0]->welcome.roomId;
		for (RawPtr& c : first) c->Abort();

		std::vector<RawPtr> second;
		JoinMany(second, port, 10);	// 비고 3초 안에 다시 온다
		Run(Ptrs(second), 3.0);
		Check(CountWelcomed(second) == 10, Fmt("다시 10명 입장 %d", CountWelcomed(second)));
		int sameRoom = 0, seen10 = 0, firstId = 1000000;
		for (RawPtr& c : second)
		{
			sameRoom += c->welcome.roomId == room ? 1 : 0;
			seen10 += c->state.count == 10 ? 1 : 0;
			firstId = std::min(firstId, (int)c->welcome.playerId);
		}
		std::printf("EDGE 관측 같은 방 %d/10 · 새 번호 최소 %d\n", sameRoom, firstId);
		Check(seen10 == 10, Fmt("상태 인원 10 인 사람 %d/10 — 옛 접속이 안 남음", seen10));
		for (RawPtr& c : second) c->Abort();
	}

	// gapSeconds = 끊고 다시 붙기까지. 0 이면 끊김과 인사가 같은 틱에 게임 스레드로 들어갈 수 있다
	void CaseReconnect(int port, double gapSeconds)
	{
		std::vector<RawPtr> clients;
		JoinMany(clients, port, 10);
		Run(Ptrs(clients), 3.0);
		Check(CountWelcomed(clients) == 10, "10명 입장");
		const wire::Welcome old = clients[3]->welcome;
		clients[3]->Abort();
		if (gapSeconds > 0)
		{
			std::vector<CRaw*> others;
			for (int i = 0; i < 10; ++i)
			{
				if (i != 3) others.push_back(clients[i].get());
			}
			Run(others, gapSeconds);
		}
		clients[3] = Join(port);
		Run(Ptrs(clients), 2.0);
		Check(clients[3]->welcomed, "다시 접속 입장");
		{
			// 방 1 이 열려 있으면 이 사람은 11번째라 새 방으로 간다. 방 1 이 9명인데 새 방으로 가면 빈자리가 묶인 것이다
			RawPtr newcomer = Join(port);
			std::vector<CRaw*> all = Ptrs(clients);
			all.push_back(newcomer.get());
			Run(all, 1.0);
			std::printf("EDGE 관측 그 뒤 새 사람 방 %u · 첫 방 인원 %d\n", newcomer->welcome.roomId, clients[0]->state.count);
			newcomer->Abort();
		}
		std::printf("EDGE 관측 방 %u→%u 번호 %u→%u 팀 %u→%u · 같은 사람으로 돌아오는 처리는 없음\n", old.roomId, clients[3]->welcome.roomId, old.playerId, clients[3]->welcome.playerId, old.team, clients[3]->welcome.team);
		Check(clients[3]->welcome.roomId == old.roomId, "빈자리가 있는 방으로 감");
		Check(clients[3]->welcome.team == old.team, "빠진 팀으로 들어감");
		Check(clients[0]->state.count == 10, Fmt("인원 10 유지 %d", clients[0]->state.count));
		for (RawPtr& c : clients) c->Abort();
	}

	void CaseOverflow(int port)
	{
		std::vector<RawPtr> clients;
		JoinMany(clients, port, 25);
		Run(Ptrs(clients), 3.0);
		Check(CountWelcomed(clients) == 25, Fmt("25명 입장 %d", CountWelcomed(clients)));
		std::map<uint32_t, int> rooms, teamA;
		for (RawPtr& c : clients)
		{
			++rooms[c->welcome.roomId];
			teamA[c->welcome.roomId] += c->welcome.team == wire::kTeamA ? 1 : 0;
		}
		Check(rooms.size() == 3, Fmt("방 수 3 — %zu", rooms.size()));
		for (const auto& room : rooms)
		{
			std::printf("EDGE 관측 방 %u 인원 %d 팀A %d\n", room.first, room.second, teamA[room.first]);
			Check(room.second <= wire::kMaxPlayersInRoom, "방 인원이 한도 안");
			Check(std::abs(room.second - 2 * teamA[room.first]) <= 1, "팀 인원 차 1 이하");
		}
		for (RawPtr& c : clients) c->Abort();
	}

	// 경기 끝 틱을 받은 시간으로 맞춰, 그 앞뒤로 접속을 흩뿌린다(라우터가 닫힘을 알기 전의 틈을 노림)
	void CasePostgame(int port)
	{
		RawPtr first = Join(port);
		const auto deadline = Clock::now() + std::chrono::seconds(20);
		auto boundary = Clock::time_point::max();
		while (Clock::now() < deadline && boundary == Clock::time_point::max())
		{
			Run({ first.get() }, 0.02);
			if (first->hasMatch && first->match.phase == (uint8_t)wire::EMatchPhase::Playing && first->match.secondsLeft > 0)
			{
				boundary = Clock::now() + std::chrono::microseconds((int64_t)(first->match.secondsLeft * 1e6));
			}
		}
		Check(boundary != Clock::time_point::max(), "경기 시작을 봄");
		if (boundary == Clock::time_point::max()) return;	// 안 보면 아래 대기가 끝나지 않는다
		while (Clock::now() + std::chrono::milliseconds(120) < boundary) Run({ first.get() }, 0.005);
		std::vector<RawPtr> late;
		// 경기 끝 메시지가 올 때까지(늦어도 1초) 자리 한도 안에서 2ms 간격으로
		const auto spreadEnd = boundary + std::chrono::milliseconds(60);
		while (Clock::now() < spreadEnd && late.size() < 40)
		{
			first->Pump();
			for (RawPtr& c : late) c->Pump();
			if (Clock::now() + std::chrono::milliseconds(15) >= boundary) late.push_back(Join(port));
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		std::vector<CRaw*> all = Ptrs(late);
		all.push_back(first.get());
		Run(all, 2.0);
		std::map<uint32_t, int> rooms;
		for (RawPtr& c : late) ++rooms[c->welcome.roomId];
		std::string text;
		for (const auto& room : rooms) text += Fmt(" 방%u:%d", room.first, room.second);
		std::printf("EDGE 관측 경계 접속 %zu개%s\n", late.size(), text.c_str());
		Check(CountWelcomed(late) == (int)late.size(), Fmt("경계 접속 전원 Welcome %d/%zu", CountWelcomed(late), late.size()));
		for (RawPtr& c : late) c->Abort();
		first->Abort();
	}

	void CaseMaxSessions(int port)
	{
		std::vector<RawPtr> clients;
		JoinMany(clients, port, 8);
		Run(Ptrs(clients), 2.0);
		const int welcomed = CountWelcomed(clients);
		std::printf("EDGE 관측 8개 접속 시도 Welcome %d · 끊긴 것 %d\n", welcomed, CountClosed(clients));
		Check(welcomed == 5, Fmt("정원 5명만 입장 %d", welcomed));
		for (RawPtr& c : clients)
		{
			if (c->welcomed)
			{
				c->Abort();
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		RawPtr next = Join(port);
		std::vector<CRaw*> all = Ptrs(clients);
		all.push_back(next.get());
		Run(all, 2.0);
		std::printf("EDGE 관측 1명 나간 뒤 새 접속 Welcome %d 끊김 %d\n", next->welcomed ? 1 : 0, next->closed ? 1 : 0);
		Check(next->welcomed, "자리가 비면 다시 입장");
		for (RawPtr& c : clients) c->Abort();
	}

	// ─── 깨진 패킷 ─────────────────────────────────────
	struct RawPacket
	{
		wire::Header h;
		char body[1600];
	};

	void SendSized(CRaw& c, uint16_t size, uint16_t type, int bytesToSend)
	{
		RawPacket packet;
		std::memset(&packet, 0x41, sizeof(packet));
		packet.h.size = size;
		packet.h.type = type;
		c.SendAll(&packet, std::min(bytesToSend, (int)sizeof(packet)));
	}

	void CaseBadPackets(int port, uint32_t seed)
	{
		RawPtr watcher = Join(port);
		Run({ watcher.get() }, 1.0);
		Check(watcher->welcomed, "지켜보는 사람 입장");
		uint64_t expectBad = 0, expectNoRoom = 0;

		const auto single = [&](const char* name, bool hello, bool expectClosed, uint16_t size, uint16_t type, int bytes)
		{
			RawPtr c = Join(port, hello);
			if (hello) Run({ watcher.get(), c.get() }, 0.2);
			SendSized(*c, size, type, bytes);
			Run({ watcher.get(), c.get() }, 0.3);
			c->Pump();
			Check(c->closed == expectClosed, Fmt("%s — %s", name, expectClosed ? "서버가 끊음" : "안 끊고 버림"));
			c->Abort();
			Run({ watcher.get() }, 0.05);
		};

		// 코어 길이 검사(4 ≤ size ≤ 1458)에서 끊는 것
		single("크기 0", false, true, 0, 1, 64);
		single("크기 3", false, true, 3, 1, 64);
		single("크기 1459", false, true, 1459, 1, 1500);
		single("크기 65535", false, true, 65535, 1, 1500);
		// 앉기 전 — 라우터가 세고 버린다
		single("앉기 전 모르는 종류(크기 1458)", false, false, 1458, 999, 1458); ++expectNoRoom;
		single("앉기 전 입력", false, false, (uint16_t)sizeof(wire::Input), (uint16_t)wire::Msg::C2S_INPUT, sizeof(wire::Input)); ++expectNoRoom;
		single("인사 크기 틀림(머리말만)", false, false, 4, (uint16_t)wire::Msg::C2S_HELLO, 4); ++expectBad;
		single("인사 크기 틀림(12)", false, false, 12, (uint16_t)wire::Msg::C2S_HELLO, 12); ++expectBad;
		{
			RawPtr c = Join(port, false);
			SendHello(*c, 999);
			Run({ watcher.get(), c.get() }, 0.3);
			Check(!c->welcomed && !c->closed, "인사 버전 틀림 — 안 앉히고 안 끊음"); ++expectBad;
			c->Abort();
		}
		// 앉은 뒤 — 방이 세고 버린다
		single("앉은 뒤 인사 또", true, false, (uint16_t)sizeof(wire::Hello), (uint16_t)wire::Msg::C2S_HELLO, sizeof(wire::Hello)); ++expectBad;
		single("앉은 뒤 모르는 종류", true, false, 16, 999, 16); ++expectBad;
		single("앉은 뒤 서버용 종류", true, false, 16, (uint16_t)wire::Msg::S2C_STATE, 16); ++expectBad;
		single("앉은 뒤 입력 크기 틀림", true, false, 20, (uint16_t)wire::Msg::C2S_INPUT, 20); ++expectBad;
		single("앉은 뒤 자리 옮기기 크기 틀림", true, false, 20, (uint16_t)wire::Msg::C2S_DEBUG_PLACE, 20); ++expectBad;
		// 반쪽 — 나머지가 영영 안 온다
		{
			RawPtr c = Join(port, false);
			SendSized(*c, 100, (uint16_t)wire::Msg::C2S_HELLO, 50);
			Run({ watcher.get(), c.get() }, 0.3);
			Check(!c->closed, "반쪽 패킷 — 기다림");
			c->Abort();
		}
		// 1바이트씩 — 정상 패킷이어야 한다
		{
			RawPtr c = Join(port, false);
			wire::Hello hello = {};
			hello.h.size = (uint16_t)sizeof(hello);
			hello.h.type = (uint16_t)wire::Msg::C2S_HELLO;
			hello.protocolVersion = wire::kProtocolVersion;
			for (size_t i = 0; i < sizeof(hello); ++i)
			{
				c->SendAll(reinterpret_cast<const char*>(&hello) + i, 1);
				Run({ watcher.get(), c.get() }, 0.003, false);
			}
			Run({ watcher.get(), c.get() }, 0.2, false);
			Check(c->welcomed, "1바이트씩 보낸 인사 — 입장");
			for (int n = 0; n < 5; ++n)
			{
				wire::Input input = MakeInput(*c);
				for (size_t i = 0; i < sizeof(input); ++i)
				{
					c->SendAll(reinterpret_cast<const char*>(&input) + i, 1);
					Run({ watcher.get(), c.get() }, 0.001, false);
				}
			}
			Run({ watcher.get(), c.get() }, 0.3, false);
			const wire::PlayerState* self = c->FindSelf();
			Check(self && self->lastSeq == 5, Fmt("1바이트씩 보낸 입력 5개 반영 lastSeq=%u", self ? self->lastSeq : 0));
			c->Abort();
		}
		// 몰아서 — 입력 200개를 send 한 번에. 대기열이 32칸이라 넘친 만큼 버려야 한다
		{
			RawPtr c = Join(port);
			Run({ watcher.get(), c.get() }, 0.2);
			std::vector<wire::Input> inputs;
			for (int n = 0; n < 200; ++n) inputs.push_back(MakeInput(*c));
			c->SendAll(inputs.data(), (int)(inputs.size() * sizeof(wire::Input)));
			Run({ watcher.get(), c.get() }, 1.0, false);
			const wire::PlayerState* self = c->FindSelf();
			std::printf("EDGE 관측 입력 200개 한 번에 → lastSeq=%u\n", self ? self->lastSeq : 0);
			Check(!c->closed && self && self->lastSeq >= 32, "몰아 보낸 입력을 받아 처리");
			c->Abort();
		}
		// 무작위 바이트 — 접속마다 다른 내용
		std::mt19937 rng(seed);
		int randomClosed = 0;
		for (int i = 0; i < 30; ++i)
		{
			RawPtr c = Join(port, (i % 2) == 1);
			std::vector<char> junk(4096);
			for (char& b : junk) b = (char)(rng() & 0xFF);
			if (i % 2 == 1) Run({ watcher.get(), c.get() }, 0.05);
			c->SendAll(junk.data(), (int)junk.size());
			Run({ watcher.get(), c.get() }, 0.05);
			randomClosed += c->closed ? 1 : 0;
			c->Abort();
		}
		std::printf("EDGE 관측 무작위 4KB 접속 30개 중 서버가 끊은 것 %d(나머지는 내가 끊음)\n", randomClosed);

		Run({ watcher.get() }, 1.0);
		Check(!watcher->closed, "지켜보는 사람 안 끊김");
		Check(watcher->badFromServer == 0, "서버가 보낸 것은 멀쩡함");
		std::printf("EDGE 관측 지켜보는 사람 상태 %llu 받음 · 최대 간격 %.0fms · 나 포함 인원 %d\n", (unsigned long long)watcher->states, watcher->maxStateGap * 1000.0, watcher->state.count);
		std::printf("EDGE 기대 badPackets>=%llu packetsNoRoom>=%llu\n", (unsigned long long)expectBad, (unsigned long long)expectNoRoom);
		watcher->Abort();
	}

	// ─── 인사 없는 접속으로 정원 채우기 ─────────────────
	void CaseGhost(int port)
	{
		RawPtr watcher = Join(port);
		RawPtr idleSeated = Join(port);
		Run({ watcher.get(), idleSeated.get() }, 0.5);
		Check(watcher->welcomed && idleSeated->welcomed, "지켜보는 사람·입력 안 보내는 사람 입장");

		std::vector<RawPtr> ghosts;
		for (int i = 0; i < 70; ++i) ghosts.push_back(Join(port, false));
		Run({ watcher.get() }, 1.0);
		for (RawPtr& g : ghosts) g->Pump();
		std::printf("EDGE 관측 인사 없는 접속 70개 시도 → 바로 끊긴 것 %d(정원 64 기본값)\n", CountClosed(ghosts));

		RawPtr legit = Join(port);
		Run({ watcher.get(), legit.get() }, 2.0);
		std::printf("EDGE 관측 정원을 채운 동안 정상 접속 Welcome %d 끊김 %d\n", legit->welcomed ? 1 : 0, legit->closed ? 1 : 0);
		legit->Abort();

		const auto start = Clock::now();
		double firstGhostClosed = -1, allGhostClosed = -1, idleClosed = -1;
		RawPtr recovered;		// 인사 없는 접속이 다 끊긴 직후의 정상 접속 — 끝에 보는 after 는 60초 제한으로도 통과한다
		const int ghostsAtStart = CountClosed(ghosts);
		auto lastInput = start;
		while (SecondsSince(start) < 75.0)
		{
			if (Clock::now() >= lastInput)
			{
				lastInput += std::chrono::microseconds(16667);
				wire::Input input = MakeInput(*watcher);
				watcher->SendAll(&input, (int)sizeof(input));
			}
			watcher->Pump();
			idleSeated->Pump();
			for (RawPtr& g : ghosts) g->Pump();
			const int closed = CountClosed(ghosts);
			if (firstGhostClosed < 0 && closed > ghostsAtStart) firstGhostClosed = SecondsSince(start);
			if (allGhostClosed < 0 && closed == (int)ghosts.size())
			{
				allGhostClosed = SecondsSince(start);
				recovered = Join(port);
			}
			if (recovered) recovered->Pump();
			if (idleClosed < 0 && idleSeated->closed) idleClosed = SecondsSince(start);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		std::printf("EDGE 관측 인사 없는 접속이 끊긴 시각(시작 뒤 초) 처음 %.1f · 전부 %.1f · 입력 안 보내는 앉은 사람 %.1f\n", firstGhostClosed, allGhostClosed, idleClosed);
		Check(allGhostClosed > 0 && allGhostClosed < 15.0, "인사 없는 접속은 인사 제한 시간에 전부 끊김");
		Check(recovered && recovered->welcomed, "인사 없는 접속이 끊긴 직후 정상 접속 입장");
		Check(idleClosed < 0 || idleClosed > 30.0, "앉은 사람은 인사 제한에 안 걸림");
		Check(!watcher->closed, "입력을 보내는 사람은 안 끊김");

		RawPtr after = Join(port);
		Run({ watcher.get(), after.get() }, 1.0);
		Check(after->welcomed, "끊긴 뒤 정상 접속 입장");
	}

	// ─── 이상한 값 ─────────────────────────────────────
	void CaseValues(int port)
	{
		RawPtr a = Join(port);
		RawPtr b = Join(port);
		Run({ a.get(), b.get() }, 0.5);
		Check(a->welcomed && b->welcomed, "두 명 입장");
		Run({ a.get(), b.get() }, 2.5);	// 부활 직후 이동 막음이 지나가게

		const auto viewOfA = [&]() -> const wire::PlayerState*
		{
			for (int i = 0; i < b->state.count; ++i)
			{
				if (b->state.players[i].id == a->welcome.playerId) return &b->state.players[i];
			}
			return nullptr;
		};
		const float huge = 1e30f;
		const float inf = std::numeric_limits<float>::infinity();
		const float nan = std::numeric_limits<float>::quiet_NaN();
		struct Step { const char* name; float moveX, moveY, yaw, pitch, aimX; uint8_t buttons, weapon; };
		const Step steps[] = {
			{ "정상 끝값 yaw 180 pitch 89.9", 0, 0, 180, 89.9f, 0, 0, 0 },	// 범위 검사가 정상 입력을 막지 않는지 — 아래 값이 범위 밖이면 이 값이 남아야 한다
			{ "yaw 1e30 + 사격", 0, 0, huge, 0, 0, wire::BUTTON_FIRE, 0 },
			{ "pitch 1e30 + 사격", 0, 0, 0, huge, 0, wire::BUTTON_FIRE, 0 },
			{ "pitch 720", 0, 0, 0, 720, 0, 0, 0 },
			{ "시작점 1e30 + 사격", 0, 0, 0, 0, huge, wire::BUTTON_FIRE, 0 },
			{ "이동값 1e30", huge, -huge, 0, 0, 0, 0, 0 },
			{ "NaN", nan, nan, nan, nan, nan, 0, 0 },
			{ "무한대", inf, inf, inf, inf, inf, 0, 0 },
			{ "무기 255", 0, 0, 0, 0, 0, 0, 255 },
			{ "버튼 전부", 0, 0, 0, 0, 0, 255, 0 },
		};
		for (const Step& step : steps)
		{
			for (int i = 0; i < 90; ++i)
			{
				wire::Input input = MakeInput(*a);
				input.moveX = step.moveX;
				input.moveY = step.moveY;
				input.yaw = step.yaw;
				input.pitch = step.pitch;
				input.aimX = step.aimX;
				input.buttons = step.buttons;
				input.weapon = step.weapon;
				a->SendAll(&input, (int)sizeof(input));
				Run({ a.get(), b.get() }, 0.0167, false);
			}
			const wire::PlayerState* view = viewOfA();
			const bool finite = view && std::isfinite(view->x) && std::isfinite(view->y) && std::isfinite(view->z) && std::isfinite(view->yaw) && std::isfinite(view->pitch);
			std::printf("EDGE 관측 %s → 남이 받는 값 yaw=%g pitch=%g 위치=%.0f,%.0f,%.0f 속도xy=%.0f,%.0f\n", step.name, view ? view->yaw : 0.f, view ? view->pitch : 0.f, view ? view->x : 0.f, view ? view->y : 0.f, view ? view->z : 0.f, view ? view->vx : 0.f, view ? view->vy : 0.f);
			Check(finite, Fmt("%s — 남이 받는 값이 유한", step.name));
			Check(view && std::fabs(view->yaw) <= 360.f && std::fabs(view->pitch) <= 95.f, Fmt("%s — 남이 받는 시선이 범위 안", step.name));
			if (&step == &steps[0]) Check(view && view->yaw == 180.f && view->pitch == 89.9f, "정상 끝값이 그대로 전달");
			Check(!a->closed && !b->closed, Fmt("%s — 둘 다 접속 유지", step.name));
		}

		// 번호를 끝까지 올리면 그 뒤 정상 입력이 전부 버려진다 — 자기만 손해인지 본다
		wire::Input last = MakeInput(*a);
		last.seq = 0xFFFFFFFF;
		a->seq = 100;
		a->SendAll(&last, (int)sizeof(last));
		Run({ a.get(), b.get() }, 0.5);
		const wire::PlayerState* self = a->FindSelf();
		const uint32_t lastAfterMax = self ? self->lastSeq : 0;
		Run({ a.get(), b.get() }, 1.0);
		self = a->FindSelf();
		std::printf("EDGE 관측 seq 최댓값 뒤 lastSeq=%u → 정상 입력 1초 뒤 lastSeq=%u\n", lastAfterMax, self ? self->lastSeq : 0);
		Check(!b->closed && b->FindSelf() != nullptr, "다른 사람은 영향 없음");
		a->Abort();
		b->Abort();
	}

	// 터무니없는 시선값을 계속 보낸다. 서버는 범위를 안 보고 전달하므로, 받는 Lyra 가 버티는지는 Lyra 쪽 로그와 종료로 본다
	void CaseViewAbuse(int port, double seconds)
	{
		RawPtr a = Join(port);
		struct View { const char* name; float yaw, pitch; };
		const View views[] = {
			{ "yaw 1e30", 1e30f, 0 }, { "yaw -1e30", -1e30f, 0 }, { "pitch 1e30", 0, 1e30f }, { "pitch -1e30", 0, -1e30f },
			{ "pitch 720", 0, 720 }, { "yaw 3e38", 3e38f, 0 }, { "yaw 1e30 pitch 1e30", 1e30f, 1e30f },
		};
		const auto start = Clock::now();
		size_t index = 0;
		while (SecondsSince(start) < seconds && !a->closed)
		{
			const View& view = views[index++ % (sizeof(views) / sizeof(views[0]))];
			std::printf("EDGE 관측 %.0f초 %s\n", SecondsSince(start), view.name);
			for (int i = 0; i < 180 && !a->closed; ++i)
			{
				wire::Input input = MakeInput(*a);
				input.moveX = 0.5f;
				input.yaw = view.yaw;
				input.pitch = view.pitch;
				a->SendAll(&input, (int)sizeof(input));
				Run({ a.get() }, 0.0167, false);
			}
		}
		Check(a->welcomed && !a->closed, "보내는 쪽 접속 유지");
		a->Abort();
	}

	// ─── 몰아 보내기 ───────────────────────────────────
	double ServerMemoryMB(HANDLE process)
	{
		PROCESS_MEMORY_COUNTERS_EX counters = {};
		counters.cb = sizeof(counters);
		if (!process || !GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) return -1;
		return (double)counters.PrivateUsage / (1024.0 * 1024.0);
	}

	// mbPerSecond = 0 이면 소켓이 받는 만큼. 값을 주면 그 속도로 — 실제 회선으로 닿을 수 있는 양인지 가른다
	void CaseFlood(int port, int serverPid, double mbPerSecond)
	{
		const HANDLE process = serverPid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)serverPid) : nullptr;
		RawPtr watcher = Join(port);
		RawPtr flood = Join(port);
		Run({ watcher.get(), flood.get() }, 2.0);
		Check(watcher->welcomed && flood->welcomed, "두 명 입장");
		const double before = ServerMemoryMB(process);
		const uint64_t statesBefore = watcher->states;
		watcher->maxStateGap = 0;

		std::vector<wire::Input> burst(500);
		for (wire::Input& input : burst) input = MakeInput(*flood);
		const int burstBytes = (int)(burst.size() * sizeof(wire::Input));
		uint64_t sentBytes = 0;
		double peak = before;
		const double kStopAtMB = 4096.0;	// 이 PC 를 멈추지 않게
		bool stopped = false;
		const auto start = Clock::now();
		auto nextWatch = start;
		auto nextMemory = start;
		while (SecondsSince(start) < 10.0)
		{
			if (Clock::now() >= nextWatch)
			{
				nextWatch += std::chrono::microseconds(16667);
				wire::Input input = MakeInput(*watcher);
				watcher->SendAll(&input, (int)sizeof(input));
				watcher->Pump();
				flood->Pump();
			}
			if (Clock::now() >= nextMemory)
			{
				nextMemory += std::chrono::milliseconds(100);
				peak = std::max(peak, ServerMemoryMB(process));
				if (peak > kStopAtMB)
				{
					stopped = true;
					break;
				}
			}
			for (wire::Input& input : burst) input.seq = ++flood->seq;
			if (mbPerSecond > 0 && (double)sentBytes > mbPerSecond * 1024.0 * 1024.0 * SecondsSince(start))
			{
				std::this_thread::yield();
				continue;
			}
			if (flood->TrySend(burst.data(), burstBytes)) sentBytes += (uint64_t)burstBytes;
			else std::this_thread::yield();
		}
		const double floodSeconds = SecondsSince(start);
		const uint64_t statesDuring = watcher->states - statesBefore;
		const double gapDuring = watcher->maxStateGap;
		std::printf("EDGE 관측 %.1f초 동안 입력 약 %.0f MB 보냄 · 서버 메모리 %.0f → 최고 %.0f MB%s\n", floodSeconds, (double)sentBytes / (1024.0 * 1024.0), before, peak, stopped ? " (한도에서 멈춤)" : "");
		std::printf("EDGE 관측 지켜보는 사람 상태 %llu 받음(기대 약 %.0f) · 최대 간격 %.0fms · 끊김 %d · 몰아 보낸 쪽 끊김 %d\n", (unsigned long long)statesDuring, floodSeconds * 60.0, gapDuring * 1000.0, watcher->closed ? 1 : 0, flood->closed ? 1 : 0);
		flood->Abort();
		Run({ watcher.get() }, 3.0);
		const double after = ServerMemoryMB(process);
		std::printf("EDGE 관측 그친 뒤 3초 서버 메모리 %.0f MB\n", after);
		Check(!watcher->closed, "지켜보는 사람 안 끊김");
		Check(!stopped, Fmt("서버 메모리가 %.0f MB 안", kStopAtMB));
		Check(watcher->states > statesBefore, "상태가 계속 옴");
		watcher->Abort();
		if (process) CloseHandle(process);
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	int port = 7791, serverPid = 0;
	uint32_t seed = 20261102;
	std::string name;
	for (int i = 1; i < argc; ++i)
	{
		const std::string s = argv[i];
		if (s.rfind("--port=", 0) == 0) port = std::atoi(s.c_str() + 7);
		else if (s.rfind("--host=", 0) == 0) g_host = s.substr(7);
		else if (s.rfind("--case=", 0) == 0) name = s.substr(7);
		else if (s.rfind("--seed=", 0) == 0) seed = (uint32_t)std::atoi(s.c_str() + 7);
		else if (s.rfind("--serverPid=", 0) == 0) serverPid = std::atoi(s.c_str() + 12);
		else
		{
			std::printf("모르는 인자 %s\n", s.c_str());
			return 2;
		}
	}
	WSADATA data;
	WSAStartup(MAKEWORD(2, 2), &data);
	timeBeginPeriod(1);

	if (name == "leave_one") CaseLeaveOne(port);
	else if (name == "leave_team") CaseLeaveTeam(port);
	else if (name == "rejoin_all") CaseRejoinAll(port);
	else if (name == "reconnect") CaseReconnect(port, 0.5);
	else if (name == "reconnect_fast") CaseReconnect(port, 0.0);
	else if (name == "overflow") CaseOverflow(port);
	else if (name == "postgame") CasePostgame(port);
	else if (name == "maxsessions") CaseMaxSessions(port);
	else if (name == "bad_packets") CaseBadPackets(port, seed);
	else if (name == "ghost") CaseGhost(port);
	else if (name == "values") CaseValues(port);
	else if (name == "view_abuse") CaseViewAbuse(port, 45.0);
	else if (name == "flood") CaseFlood(port, serverPid, 0.0);
	else if (name == "flood_10") CaseFlood(port, serverPid, 10.0);
	else if (name == "flood_100") CaseFlood(port, serverPid, 100.0);
	else
	{
		std::printf("모르는 시험 %s\n", name.c_str());
		return 2;
	}

	timeEndPeriod(1);
	WSACleanup();
	std::printf("EDGE 실패 %d건\nRESULT %s\n", g_fails, g_fails == 0 ? "PASS" : "FAIL");
	return g_fails == 0 ? 0 : 1;
}
