// 매치 서버 — 방 여러 개. 플랜 = MyGit/MyDocs/claude-plans/mo-shooter/authoritative-server-plan.md
//
//   Server --map=<파일.mocl> [--port=7791] [--seed=20261102] [--duration=초] [--exitWhenEmpty] [--debug] [--record=파일]
//          [--warmup=초] [--matchSeconds=초] [--targetScore=N] [--fallKillZ=높이] [--gameThreads=1] [--maxSessions=64]
//          [--measureFrom=초 --measureSeconds=초] [--clips=파일] [--pads=파일]
//
// --debug 는 시험 클라가 위치·체력을 정할 수 있게 한다. 사람이 하는 판에서는 켜지 않는다.
// --warmup·--matchSeconds·--targetScore 는 시험에서 Lyra 값을 줄일 때만 쓴다.
// --record 는 처치와 경기 결과를 적는다 — 봇이 받아 적은 기록과 바이트로 비교한다. 방 2부터는 뒤에 .room<번호> 를 붙인다.
// --exitWhenEmpty 는 방을 한 번이라도 쓴 뒤 열린 방이 하나도 안 남으면 끝낸다.
// --measureSeconds 를 주면 시작 뒤 --measureFrom 초부터 그만큼만 재서 LOAD 줄로 낸다(부하 시험용).
// --clips 는 클립 파일(Tools/LyraPilot/make_clip_pack.py 가 Lyra 에서 뽑는다, 리포에 안 넣는다). 주면 몸 판정이 애니 자세를 쓰고 상태 패킷에 애니 상태가 붙는다.
//
// 이 파일이 라우터다: 접속·끊김·인사를 한 곳에서 받아 접속을 방에 앉힌다. 앉은 뒤 입력은 네트워크 워커가 그 방의 게임 스레드로 바로 넣는다.
// 로그인·로비는 없다 — 접속하면 자리가 남은 가장 앞 방에 앉고, 없으면 새 방을 만든다.
#include "GameProtocol.h"
#include "GameThread.h"
#include "KillRecord.h"
#include "Pickups.h"
#include "Room.h"

#include "NetIoModel.h"
#include "MonitorManager.h"
#include <Common/Logger.h>

#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "winmm.lib")

// ServerCore 가 실행 파일 쪽에 요구하는 종료 신호(IOCPServer.cpp 가 부른다)
std::atomic<bool> g_shutdownRequested{ false };
void SignalProcessShutdown() { g_shutdownRequested = true; }

namespace
{
	using Clock = std::chrono::steady_clock;

	constexpr int kHelloTimeoutSeconds = 5;		// 다리·봇은 접속 직후 바로 인사한다

	struct Args
	{
		std::string mapPath;
		int port = 7791;
		std::string recordPath;
		mo::MatchRules rules;
		uint32_t seed = 20261102;
		double durationSeconds = 0;
		bool exitWhenEmpty = false;
		bool debug = false;
		int testLockstep = 0;		// --testLockstep=N — 시험 전용 기다림 판(RoomSettings::lockstepPlayers)
		int gameThreads = 1;
		int maxSessions = 64;
		double measureFromSeconds = 0;
		double measureSeconds = 0;
		mo::RewindOptions rewind;
		std::string clipsPath;
		std::string padsPath;		// 받침(체력 · 무기, 컨텐츠 B · W-1) — 리포 밖 파일(Tools/LyraPilot/make_pads.py). 없으면 받침 없는 판
		mo::AnimOptions anim;
		mo::PickupOptions pickups;	// 받침 목록은 main 이 들고 있다 — 방은 가리키기만
	};

	bool ParseArgs(int argc, char** argv, Args& out)
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string s = argv[i];
			const auto value = [&s](const char* key, std::string& result)
			{
				const size_t n = std::strlen(key);
				if (s.compare(0, n, key) != 0) return false;
				result = s.substr(n);
				return true;
			};
			std::string v;
			if (value("--map=", v)) out.mapPath = v;
			else if (value("--port=", v)) out.port = std::atoi(v.c_str());
			else if (value("--clips=", v)) out.clipsPath = v;
			else if (value("--pads=", v)) out.padsPath = v;
			else if (value("--seed=", v)) out.seed = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
			else if (value("--duration=", v)) out.durationSeconds = std::atof(v.c_str());
			else if (value("--record=", v)) out.recordPath = v;
			else if (value("--warmup=", v)) out.rules.warmupSeconds = std::atof(v.c_str());
			else if (value("--matchSeconds=", v)) out.rules.bigMatchSeconds = out.rules.smallMatchSeconds = std::atof(v.c_str());
			else if (value("--targetScore=", v)) out.rules.bigMatchTargetScore = out.rules.smallMatchTargetScore = std::atoi(v.c_str());
			else if (value("--fallKillZ=", v)) out.rules.fallKillZ = std::atof(v.c_str());	// 재생 시험용 — Lyra 기록은 낙사 높이 아래까지 떨어진다(-27952)
			else if (value("--testLockstep=", v)) out.testLockstep = std::atoi(v.c_str());
			else if (value("--gameThreads=", v)) out.gameThreads = std::atoi(v.c_str());
			else if (value("--maxSessions=", v)) out.maxSessions = std::atoi(v.c_str());
			else if (value("--measureFrom=", v)) out.measureFromSeconds = std::atof(v.c_str());
			else if (value("--measureSeconds=", v)) out.measureSeconds = std::atof(v.c_str());
			else if (s == "--exitWhenEmpty") out.exitWhenEmpty = true;
			else if (s == "--debug") out.debug = true;
			else if (s == "--testNoPawnBlock") out.rules.pawnBlock = false;	// 시험 전용 — 사람끼리 막기를 끈다(MatchRules.h · 봇도 같이 줘야 한다)
			else if (s == "--testStartAllWeapons") out.rules.startAllWeapons = true;	// 시험 전용 — 세 무기로 부활(무기 줍기 전 동작, MatchRules.h · 봇도 같이 줘야 한다)
			else if (s == "--rewindShadow") out.rewind.shadow = true;
			else if (s == "--rewindLog") out.rewind.log = true;
			else if (s == "--rewindNoSpread") out.rewind.noSpread = true;
			else if (s == "--rewindBulletLog") out.rewind.bulletLog = true;
			else if (value("--rewindMontageShift=", v)) out.rewind.montageShiftTicks = (uint32_t)std::max(std::atoi(v.c_str()), 0);	// 일부러 틀림 전용(Room.h RewindOptions)
			else if (value("--rewindLimitTicks=", v)) out.rewind.limitTicks = (uint32_t)std::min(std::atoi(v.c_str()), (int)mo::kRewindLimitTicks);	// 시험 전용. 기록 칸이 상수만큼뿐이라 더 키우지는 못한다
			else
			{
				std::printf("[오류] 모르는 인자: %s\n", s.c_str());
				return false;
			}
		}
		if (out.gameThreads < 1 || out.maxSessions < 1)
		{
			std::printf("[오류] --gameThreads·--maxSessions 는 1 이상\n");
			return false;
		}
		return !out.mapPath.empty();
	}

	bool ReadFile(const std::string& path, std::vector<char>& data)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path.c_str(), "rb") != 0 || !file) return false;
		std::fseek(file, 0, SEEK_END);
		const long size = std::ftell(file);
		std::fseek(file, 0, SEEK_SET);
		data.resize((size_t)size);
		const size_t read = std::fread(data.data(), 1, data.size(), file);
		std::fclose(file);
		return read == data.size();
	}

	bool LoadMap(const std::string& path, mo::CMapCollision& map)
	{
		std::vector<char> data;
		if (!ReadFile(path, data)) return false;
		return map.LoadFromMemory(data.data(), data.size()) == mo::EMapLoad::Ok;
	}

	// 클립을 읽고, 방에서 쓸 초기화가 무기 묶음마다 되는지 시작할 때 확인한다. 넓은 공 = 세 묶음 중 가장 큰 것
	bool LoadClips(const std::string& path, double crouchMeshLiftZ, mo::CAnimClips& clips, double& outBoundRadius, double (&outBundleRadius)[(size_t)mo::EWeapon::Count])
	{
		std::vector<char> data;
		if (!ReadFile(path, data) || clips.LoadFromMemory(data.data(), data.size()) != mo::EClipLoad::Ok) return false;
		outBoundRadius = 0.0;
		for (size_t weapon = 0; weapon < (size_t)mo::EWeapon::Count; ++weapon)
		{
			const char* bundle = mo::AnimBundleOf((uint8_t)weapon);
			mo::CAnimPoseEvaluator evaluator;
			mo::CAnimState state;
			if (!evaluator.Init(clips, bundle) || !state.Init(clips, bundle)) return false;
			outBundleRadius[weapon] = evaluator.ComputeBoundRadius(crouchMeshLiftZ);		// 앉은 클립은 앉은 캡슐의 메시 올림으로도 잰다(D-4)
			outBoundRadius = std::max(outBoundRadius, outBundleRadius[weapon]);
		}
		return true;
	}

	double ProcessCpuSeconds()
	{
		FILETIME creation, exit, kernel, user;
		if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
		const auto seconds = [](const FILETIME& t) { return (double)(((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) * 1e-7; };
		return seconds(kernel) + seconds(user);
	}

	// 측정 구간 시작·끝에 읽는 프로세스 숫자
	struct NetSnapshot
	{
		int64_t sendBytes = 0;
		int64_t recvBytes = 0;
		int64_t sendCalls = 0;
		int64_t overflow = 0;
		int64_t recvPackets = 0;
		double cpuSeconds = 0;
	};

	class CMatchServer
	{
	public:
		CMatchServer(const Args& args, const mo::CMapCollision& map, const mo::MoveParams& params)
			: _args(args)
			, _map(map)
			, _params(params)
			, _net(args.port, args.maxSessions, ServerMode::GameServer, _monitor)
		{
			for (int i = 0; i < args.gameThreads; ++i)
			{
				_threads.emplace_back(new mo::CGameThread(i, _net));
				_threadRooms.push_back(0);
			}
		}

		bool Start()
		{
			if (!_net.Start()) return false;
			if (_args.measureSeconds > 0)
			{
				_measureBegin = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(_args.measureFromSeconds));
				_measureEnd = _measureBegin + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(_args.measureSeconds));
				for (const std::unique_ptr<mo::CGameThread>& thread : _threads) thread->SetMeasure(_measureBegin, _measureEnd);
			}
			for (const std::unique_ptr<mo::CGameThread>& thread : _threads) thread->Start();
			return true;
		}

		// 게임 스레드를 먼저 세운다 — 그래야 끊긴 네트워크에 보낼 일이 없다
		void Stop()
		{
			for (const std::unique_ptr<mo::CGameThread>& thread : _threads) thread->RequestStop();
			for (const std::unique_ptr<mo::CGameThread>& thread : _threads) thread->Join();
			DrainReports();
			_net.ShutdownServer();

			// 워커가 멈췄으니 큐에 남은 패킷의 버퍼를 돌려준다
			std::queue<NetworkEvent> left;
			for (const std::unique_ptr<mo::CGameThread>& thread : _threads)
			{
				thread->GetRecvQueue().SwapOut(left);
				ReleaseEvents(left);
			}
			_net.SwapOutNetworkEvents(left);
			ReleaseEvents(left);
		}

		// 라우터 루프가 부른다 — 측정 구간 경계를 지날 때 네트워크 숫자를 찍는다
		void UpdateMeasure(Clock::time_point now)
		{
			if (_args.measureSeconds <= 0) return;
			if (!_snapshotBeginTaken && now >= _measureBegin)
			{
				_snapshotBeginTaken = true;
				_snapshotBegin = TakeSnapshot();
			}
			if (_snapshotBeginTaken && !_snapshotEndTaken && now >= _measureEnd)
			{
				_snapshotEndTaken = true;
				_snapshotEnd = TakeSnapshot();
			}
		}

		bool HasMeasure() const { return _snapshotEndTaken; }
		const NetSnapshot& GetSnapshotBegin() const { return _snapshotBegin; }
		const NetSnapshot& GetSnapshotEnd() const { return _snapshotEnd; }

		// 접속·끊김·앉기 전 패킷을 처리하고, 게임 스레드의 알림을 처리한다
		void Route()
		{
			std::queue<NetworkEvent> events;
			_net.SwapOutNetworkEvents(events);
			while (!events.empty())
			{
				NetworkEvent& e = events.front();
				switch (e.type)
				{
				case NetworkEvent::Type::CONNECTED:
					++_connected;
					_helloDeadlines.push_back({ e.sessionId, Clock::now() + std::chrono::seconds(kHelloTimeoutSeconds) });
					break;
				case NetworkEvent::Type::DISCONNECTED:
					++_disconnected;
					OnDisconnected(e.sessionId);
					break;
				case NetworkEvent::Type::RECEIVED:
					OnPacket(e.sessionId, e.pMsg->GetReadBufferPtr(), e.pMsg->GetDataSize());
					e.pMsg->SubRef();
					break;
				}
				events.pop();
			}
			DrainReports();
			DisconnectWithoutHello();
		}

		bool IsDoneWithAllRooms() const { return _roomsCreated > 0 && _rooms.empty(); }
		const std::vector<std::unique_ptr<mo::CGameThread>>& GetThreads() const { return _threads; }
		const std::vector<std::shared_ptr<mo::RoomSummary>>& GetSummaries() const { return _summaries; }
		uint32_t GetRoomsCreated() const { return _roomsCreated; }
		uint64_t GetConnected() const { return _connected; }
		uint64_t GetDisconnected() const { return _disconnected; }
		uint64_t GetHellos() const { return _hellos; }
		uint64_t GetHelloRejected() const { return _helloRejected; }
		uint64_t GetHelloTimeouts() const { return _helloTimeouts; }
		uint64_t GetBadPackets() const { return _badPackets; }
		uint64_t GetPacketsNoRoom() const { return _packetsNoRoom; }
		size_t GetSessionsPeak() const { return _sessionsPeak; }

	private:
		NetSnapshot TakeSnapshot() const
		{
			NetSnapshot snapshot;
			snapshot.sendBytes = _monitor._sendBytes.Load();
			snapshot.recvBytes = _monitor._recvBytes.Load();
			snapshot.sendCalls = _monitor._wsaSendCalls.Load();
			snapshot.overflow = _monitor._sendQueueOverflow.Load();
			snapshot.recvPackets = _monitor._recvPackets.Load();
			snapshot.cpuSeconds = ProcessCpuSeconds();
			return snapshot;
		}

		static void ReleaseEvents(std::queue<NetworkEvent>& events)
		{
			for (; !events.empty(); events.pop())
			{
				if (events.front().pMsg) events.front().pMsg->SubRef();
			}
		}

		struct RoomSlot
		{
			int thread = 0;
			int players = 0;
			bool open = true;
			std::vector<int64_t> sessions;
		};

		uint32_t ChooseRoom()
		{
			for (const auto& room : _rooms)
			{
				if (room.second.open && room.second.players < mo::wire::kMaxPlayersInRoom) return room.first;
			}
			return CreateRoom();
		}

		uint32_t CreateRoom()
		{
			const uint32_t id = ++_roomsCreated;
			// 방 수가 가장 적은 스레드 — 방은 끝날 때까지 이 스레드를 떠나지 않는다
			const int thread = (int)(std::min_element(_threadRooms.begin(), _threadRooms.end()) - _threadRooms.begin());
			++_threadRooms[thread];

			RoomSlot slot;
			slot.thread = thread;
			_rooms.emplace(id, slot);

			mo::RoomSettings settings;
			settings.map = &_map;
			settings.params = &_params;
			settings.rules = _args.rules;
			settings.seed = _args.seed + (id - 1);		// 방마다 시작 자리 난수를 다르게 둔다. 탄 방향은 입력 번호로 정해져 이 값을 안 쓴다
			settings.debug = _args.debug;
			settings.lockstepPlayers = _args.testLockstep;
			settings.rewind = _args.rewind;
			settings.anim = _args.anim;
			settings.pickups = _args.pickups;
			if (!_args.recordPath.empty()) settings.recordPath = mo::RoomRecordPath(_args.recordPath, id);
			_threads[thread]->PostCreateRoom(id, settings);
			return id;
		}

		void Seat(int64_t session)
		{
			const uint32_t roomId = ChooseRoom();
			RoomSlot& room = _rooms.find(roomId)->second;
			++room.players;
			room.sessions.push_back(session);
			_sessionRoom[session] = roomId;
			_sessionsPeak = std::max(_sessionsPeak, _sessionRoom.size());
			// 받을 큐를 먼저 바꾼다 — 인사가 처리되면 Welcome 이 나가고 곧바로 입력이 돌아온다
			_net.SetSessionRecvQueue(session, &_threads[room.thread]->GetRecvQueue());
			_threads[room.thread]->PostHello(roomId, session);
		}

		// 방 자리 표에서 뺀다. 게임 스레드에 알릴지는 부르는 쪽이 정한다
		void Unseat(int64_t session, RoomSlot& room)
		{
			--room.players;
			room.sessions.erase(std::remove(room.sessions.begin(), room.sessions.end(), session), room.sessions.end());
			_sessionRoom.erase(session);
		}

		void OnDisconnected(int64_t session)
		{
			const auto seated = _sessionRoom.find(session);
			if (seated == _sessionRoom.end()) return;
			const uint32_t roomId = seated->second;
			RoomSlot& room = _rooms.find(roomId)->second;
			_threads[room.thread]->PostLeave(roomId, session);
			Unseat(session, room);
		}

		// 여기 오는 패킷은 앉기 전에 받은 것뿐이다
		void OnPacket(int64_t session, const char* data, int size)
		{
			if (size < (int)sizeof(mo::wire::Header))
			{
				++_badPackets;
				return;
			}
			mo::wire::Header header;
			std::memcpy(&header, data, sizeof(header));
			if ((mo::wire::Msg)header.type != mo::wire::Msg::C2S_HELLO)
			{
				++_packetsNoRoom;		// 인사 전 입력 — 게임 규칙 위반으로 세지 않는다
				return;
			}
			mo::wire::Hello hello;
			if (size != (int)sizeof(hello))
			{
				++_badPackets;
				return;
			}
			std::memcpy(&hello, data, sizeof(hello));
			if (hello.protocolVersion != mo::wire::kProtocolVersion || _sessionRoom.find(session) != _sessionRoom.end())
			{
				++_badPackets;
				return;
			}
			++_hellos;
			Seat(session);
		}

		// 인사 없는 접속이 정원을 채우지 못하게 한다. 제한 시간이 모두 같아 접속 순서가 곧 마감 순서다 — 맨 앞만 보면 된다
		void DisconnectWithoutHello()
		{
			if (_helloDeadlines.empty()) return;
			const Clock::time_point now = Clock::now();
			while (!_helloDeadlines.empty() && _helloDeadlines.front().second <= now)
			{
				const int64_t session = _helloDeadlines.front().first;
				_helloDeadlines.pop_front();
				// 이미 끊긴 번호면 코어가 false 를 돌려준다 — 번호는 다시 쓰이지 않는다
				if (_sessionRoom.find(session) == _sessionRoom.end() && _net.RequestDisconnectSession(session)) ++_helloTimeouts;
			}
		}

		void DrainReports()
		{
			_reports.clear();
			for (const std::unique_ptr<mo::CGameThread>& thread : _threads) thread->DrainReports(_reports);
			for (const mo::GameReport& report : _reports)
			{
				const auto found = _rooms.find(report.roomId);
				if (found == _rooms.end()) continue;
				RoomSlot& room = found->second;
				switch (report.type)
				{
				case mo::GameReport::Type::ClosedToNew:
					room.open = false;
					break;
				case mo::GameReport::Type::HelloRejected:
				{
					++_helloRejected;
					// 방을 닫지 않는다 — 나가기가 앉히기 뒤에 처리되어 잠깐 만원으로 거절한 것일 수 있다.
					// 경기 끝이면 같은 스레드의 ClosedToNew 가 이 알림보다 먼저 와서 이미 닫혀 있다
					// 그사이 끊겼으면 표에 없다 — 표에 그대로 이 방이면 다른 방으로
					const auto seated = _sessionRoom.find(report.session);
					if (seated != _sessionRoom.end() && seated->second == report.roomId)
					{
						Unseat(report.session, room);
						Seat(report.session);
					}
					break;
				}
				case mo::GameReport::Type::Closed:
					for (const int64_t session : room.sessions)
					{
						_sessionRoom.erase(session);
						_net.RequestDisconnectSession(session);
					}
					--_threadRooms[room.thread];
					_summaries.push_back(report.summary);
					_rooms.erase(found);
					break;
				}
			}
		}

		const Args& _args;
		const mo::CMapCollision& _map;
		const mo::MoveParams& _params;
		std::vector<std::unique_ptr<mo::CGameThread>> _threads;	// _net 보다 앞 — 워커가 넣는 받은 큐가 _net 보다 오래 살아야 한다
		CMonitorManager _monitor;
		NetIoModel _net;
		std::vector<int> _threadRooms;
		std::map<uint32_t, RoomSlot> _rooms;	// 번호 순서가 곧 앉히는 순서 — 앞 방부터 채운다
		std::unordered_map<int64_t, uint32_t> _sessionRoom;
		std::deque<std::pair<int64_t, Clock::time_point>> _helloDeadlines;	// 접속 순서
		std::vector<mo::GameReport> _reports;
		std::vector<std::shared_ptr<mo::RoomSummary>> _summaries;
		uint32_t _roomsCreated = 0;
		uint64_t _connected = 0;
		uint64_t _disconnected = 0;
		uint64_t _hellos = 0;
		uint64_t _helloRejected = 0;
		uint64_t _helloTimeouts = 0;
		uint64_t _badPackets = 0;
		uint64_t _packetsNoRoom = 0;
		size_t _sessionsPeak = 0;
		Clock::time_point _measureBegin;
		Clock::time_point _measureEnd;
		bool _snapshotBeginTaken = false;
		bool _snapshotEndTaken = false;
		NetSnapshot _snapshotBegin;
		NetSnapshot _snapshotEnd;
	};
}

int main(int argc, char** argv)
{
	shared::LoggerGuard loggerGuard;
	// --rewindLog 는 발사마다 줄을 낸다. 버퍼 없이 쓰면 그 틱이 수십 ms 길어진다(실측: 켠 판 workUs p99 43~58ms, 끈 판 0.26ms).
	// 그래서 이 옵션을 켠 판만 출력을 모았다가 끝에 쓴다. 8MB = 180초 판 기록(약 175KB)의 45배
	static char rewindLogBuffer[8 * 1024 * 1024];
	bool rewindLog = false;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--rewindLog") == 0 || std::strcmp(argv[i], "--rewindBulletLog") == 0) rewindLog = true;
	}
	if (rewindLog) std::setvbuf(stdout, rewindLogBuffer, _IOFBF, sizeof(rewindLogBuffer));
	else std::setvbuf(stdout, nullptr, _IONBF, 0);	// 러너가 파일로 받는다 — 줄마다 바로 보이게

	Args args;
	if (!ParseArgs(argc, argv, args))
	{
		std::printf("쓰는 법: Server --map=<파일.mocl> [--port=7791] [--seed=N] [--duration=초] [--exitWhenEmpty] [--debug] [--record=파일] [--warmup=초] [--matchSeconds=초] [--targetScore=N] [--fallKillZ=높이] [--gameThreads=N] [--maxSessions=N] [--clips=파일] [--pads=파일]\n");
		return 2;
	}

	mo::CMapCollision map;
	if (!LoadMap(args.mapPath, map))
	{
		std::printf("[오류] 맵을 읽지 못함: %s\n", args.mapPath.c_str());
		return 1;
	}

	const mo::MoveParams params = mo::MakeLyraMoveParams();
	mo::CAnimClips clips;		// 서버가 멈출 때까지 방들이 읽는다
	if (!args.clipsPath.empty())
	{
		double bundleRadius[(size_t)mo::EWeapon::Count] = {};
		if (!LoadClips(args.clipsPath, mo::CMoveSim(map, params).CrouchMeshLiftZ(), clips, args.anim.boundRadius, bundleRadius))
		{
			std::printf("[오류] 클립을 읽지 못함: %s\n", args.clipsPath.c_str());
			return 1;
		}
		args.anim.clips = &clips;
		std::printf("ANIMBOUND radius=%.3f meshZ=%.3f bones=%zu bodies=%zu %s=%.3f %s=%.3f %s=%.3f\n", args.anim.boundRadius, clips.MeshPlacement().p[2], clips.Bones().size(), clips.Bodies().size(),
			mo::AnimBundleOf(0), bundleRadius[0], mo::AnimBundleOf(1), bundleRadius[1], mo::AnimBundleOf(2), bundleRadius[2]);
	}

	// 받침 — 틀린 줄이 하나라도 있으면 시작하지 않는다(받침 하나 빠진 판은 Lyra 와 다른 맵)
	std::vector<mo::PickupPad> pads;		// 서버가 멈출 때까지 방들이 읽는다
	if (!args.padsPath.empty())
	{
		std::vector<char> data;
		if (!ReadFile(args.padsPath, data))
		{
			std::printf("[오류] 받침 파일을 읽지 못함: %s\n", args.padsPath.c_str());
			return 1;
		}
		size_t badLine = 0;
		const mo::EPadsLoad loaded = mo::ParsePickupPads(data.data(), data.size(), pads, badLine);
		if (loaded != mo::EPadsLoad::Ok)
		{
			std::printf("[오류] 받침 파일이 틀림: %s — %s (줄 %zu)\n", args.padsPath.c_str(), mo::PadsLoadName(loaded), badLine);
			return 1;
		}
	}
	size_t weaponPads = 0;
	for (const mo::PickupPad& pad : pads)
	{
		if (pad.kind != mo::EPadKind::Heal) ++weaponPads;
	}
	if (!pads.empty()) args.pickups.pads = &pads;
	std::printf("PADS count=%zu heal=%zu weapon=%zu file=%s\n", pads.size(), pads.size() - weaponPads, weaponPads, args.padsPath.empty() ? "-" : args.padsPath.c_str());
	for (size_t i = 0; i < pads.size(); ++i)
	{
		const mo::PickupPad& pad = pads[i];
		std::printf("PAD %zu kind=%s name=%s at=(%.3f,%.3f,%.3f) r=%.1f hh=%.1f heal=%g respawn=%g recheck=%g\n", i, mo::PadKindName(pad.kind), pad.name.c_str(),
			pad.location.x, pad.location.y, pad.location.z, pad.radius, pad.halfHeight, pad.heal, pad.respawnSeconds, pad.recheckSeconds);
	}

	CMatchServer server(args, map, params);
	if (!server.Start())
	{
		std::printf("[오류] 서버 시작 실패 (포트 %d)\n", args.port);
		return 1;
	}
	std::printf("[시작] 포트 %d 맵 %s 입출력 %s 틱 %dHz 시험 입력 %s 게임 스레드 %d 최대 접속 %d\n", args.port, args.mapPath.c_str(), kNetIoModelName, mo::wire::kTickRate, args.debug ? "켬" : "끔", args.gameThreads, args.maxSessions);

	timeBeginPeriod(1);	// 윈도 Sleep 기본 눈금 15.6ms 로는 60Hz 를 못 맞춘다

	const auto start = Clock::now();
	double routeSumUs = 0;
	while (!g_shutdownRequested)
	{
		const auto begin = Clock::now();
		server.Route();
		const auto end = Clock::now();
		routeSumUs += std::chrono::duration<double, std::micro>(end - begin).count();
		server.UpdateMeasure(end);

		if (args.exitWhenEmpty && server.IsDoneWithAllRooms()) break;
		if (args.durationSeconds > 0 && std::chrono::duration<double>(end - start).count() >= args.durationSeconds) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const double elapsedSeconds = std::chrono::duration<double>(Clock::now() - start).count();

	server.Stop();
	timeEndPeriod(1);

	// ─── 스레드별 ─────────────────────────────────────────
	const auto percentile = [](const std::vector<float>& sorted, double p) { return sorted.empty() ? 0.f : sorted[(size_t)((sorted.size() - 1) * p)]; };
	std::vector<float> allWork;
	double intervalSumMs = 0;
	double intervalMaxMs = 0;
	uint64_t intervalCount = 0;
	uint64_t lateTicks = 0;
	uint64_t packetsNoRoom = server.GetPacketsNoRoom();
	for (const std::unique_ptr<mo::CGameThread>& thread : server.GetThreads())
	{
		const mo::GameThreadStats& stats = thread->GetStats();
		std::vector<float> work = stats.workMicroseconds;
		std::sort(work.begin(), work.end());
		allWork.insert(allWork.end(), work.begin(), work.end());
		intervalSumMs += stats.intervalSumMs;
		intervalMaxMs = std::max(intervalMaxMs, stats.intervalMaxMs);
		intervalCount += stats.ticks;
		lateTicks += stats.lateTicks;
		packetsNoRoom += stats.packetsNoRoom;
		std::printf("THREAD %d rooms=%llu roomsPeak=%zu ticks=%llu roomTicks=%llu lateTicks=%llu intervalMaxMs=%.3f workUs p50=%.1f p99=%.1f max=%.1f sendAvgUs=%.1f\n",
			thread->GetIndex(), (unsigned long long)stats.roomsTotal, stats.roomsPeak, (unsigned long long)stats.ticks, (unsigned long long)stats.roomTicks, (unsigned long long)stats.lateTicks, stats.intervalMaxMs,
			percentile(work, 0.5), percentile(work, 0.99), percentile(work, 1.0), stats.ticks ? stats.sendSumUs / (double)stats.ticks : 0.0);
	}
	std::sort(allWork.begin(), allWork.end());

	// ─── 방별 · 합계 ───────────────────────────────────────
	std::vector<std::shared_ptr<mo::RoomSummary>> summaries = server.GetSummaries();
	std::sort(summaries.begin(), summaries.end(), [](const std::shared_ptr<mo::RoomSummary>& a, const std::shared_ptr<mo::RoomSummary>& b) { return a->id < b->id; });
	mo::RoomStats room;
	uint64_t badPackets = server.GetBadPackets();
	const mo::RoomSummary* firstFinished = nullptr;
	uint64_t lockstepWaits = 0, lockstepTimeouts = 0;
	for (const std::shared_ptr<mo::RoomSummary>& summary : summaries)
	{
		mo::AddRoomStats(room, summary->stats);
		badPackets += summary->badPackets;
		lockstepWaits += summary->lockstepWaits;
		lockstepTimeouts += summary->lockstepTimeouts;
		if (summary->finished && !firstFinished) firstFinished = summary.get();
		std::printf("ROOM id=%u thread=%d playersPeak=%zu ticks=%llu finished=%d score=%u:%u kills=%llu shots=%llu badPackets=%llu\n",
			summary->id, summary->thread, summary->playersPeak, (unsigned long long)summary->stats.ticks, summary->finished ? 1 : 0, summary->result.teamScoreA, summary->result.teamScoreB,
			(unsigned long long)summary->stats.kills, (unsigned long long)summary->stats.shots, (unsigned long long)summary->badPackets);
	}

	// ─── 부하 시험 — 측정 구간만 ─────────────────────────────
	if (args.measureSeconds > 0)
	{
		mo::RoomStats measured;
		std::vector<float> measuredWork;
		double worstThreadP99 = 0, intervalMax = 0, inboxUs = 0, roomsUs = 0, flushUs = 0, sendUs = 0, gameCpu = 0;
		uint64_t ticks = 0, late = 0, sendBytes = 0, sendPackets = 0, roomsClosed = 0;
		size_t roomsAtEnd = 0, sessionsAtEnd = 0;
		bool complete = server.HasMeasure();
		for (const std::unique_ptr<mo::CGameThread>& thread : server.GetThreads())
		{
			const mo::GameThreadStats& stats = thread->GetStats();
			if (!stats.measured) complete = false;
			mo::AddRoomStats(measured, stats.measuredRooms);
			std::vector<float> work = stats.workMicroseconds;
			std::sort(work.begin(), work.end());
			worstThreadP99 = std::max(worstThreadP99, (double)percentile(work, 0.99));
			measuredWork.insert(measuredWork.end(), work.begin(), work.end());
			intervalMax = std::max(intervalMax, stats.intervalMaxMs);
			ticks += stats.ticks;
			late += stats.lateTicks;
			inboxUs += stats.inboxSumUs;
			roomsUs += stats.roomsSumUs;
			flushUs += stats.flushSumUs;
			sendUs += stats.sendSumUs;
			gameCpu += stats.cpuSeconds;
			sendBytes += stats.sendBytes;
			sendPackets += stats.sendPackets;
			roomsClosed += stats.roomsClosedInMeasure;
			roomsAtEnd += stats.roomsAtEnd;
			sessionsAtEnd += stats.sessionsAtEnd;
		}
		std::sort(measuredWork.begin(), measuredWork.end());
		const double seconds = args.measureSeconds;
		const double perTick = ticks ? 1.0 / (double)ticks : 0.0;
		const NetSnapshot& a = server.GetSnapshotBegin();
		const NetSnapshot& b = server.GetSnapshotEnd();
		std::printf("LOAD complete=%d seconds=%.1f threads=%d rooms=%zu sessions=%zu roomsClosed=%llu tickP50Us=%.1f tickP99Us=%.1f worstThreadP99Us=%.1f tickMaxUs=%.1f lateTicks=%llu intervalMaxMs=%.3f"
			" inputsPerSec=%.1f shotsPerSec=%.1f bulletsPerSec=%.1f statesPerSec=%.1f sendBytesPerSec=%.0f sendPacketsPerSec=%.1f netSendBytesPerSec=%.0f netRecvBytesPerSec=%.0f sendCallsPerSec=%.1f netRecvPacketsPerSec=%.1f"
			" inboxAvgUs=%.1f roomsAvgUs=%.1f inputsAvgUs=%.1f shotsAvgUs=%.1f stateAvgUs=%.1f sendRawAvgUs=%.1f flushAvgUs=%.1f"
			" cpuProcessPercent=%.1f cpuGamePercent=%.1f overflow=%lld droppedFull=%llu starvedTicks=%llu idleMoves=%llu bullets=%llu\n",
			complete ? 1 : 0, seconds, args.gameThreads, roomsAtEnd, sessionsAtEnd, (unsigned long long)roomsClosed, percentile(measuredWork, 0.5), percentile(measuredWork, 0.99), worstThreadP99, percentile(measuredWork, 1.0), (unsigned long long)late, intervalMax,
			(double)measured.inputsApplied / seconds, (double)measured.shots / seconds, (double)measured.bullets / seconds, (double)measured.statesSent / seconds, (double)sendBytes / seconds, (double)sendPackets / seconds,
			(double)(b.sendBytes - a.sendBytes) / seconds, (double)(b.recvBytes - a.recvBytes) / seconds, (double)(b.sendCalls - a.sendCalls) / seconds, (double)(b.recvPackets - a.recvPackets) / seconds,
			inboxUs * perTick, roomsUs * perTick, (double)measured.timeInputsNs * 1e-3 * perTick, (double)measured.timeShotsNs * 1e-3 * perTick, (double)measured.timeStateNs * 1e-3 * perTick, (sendUs - flushUs) * perTick, flushUs * perTick,
			(b.cpuSeconds - a.cpuSeconds) / seconds * 100.0, gameCpu / seconds * 100.0, (long long)(b.overflow - a.overflow), (unsigned long long)measured.inputsDroppedFull, (unsigned long long)measured.starvedTicks, (unsigned long long)measured.idleMoves, (unsigned long long)measured.bullets);
	}

	std::printf("ROUTER roomsCreated=%u roomsReported=%zu sessionsPeak=%zu helloRejected=%llu helloTimeouts=%llu packetsNoRoom=%llu busyPercent=%.2f\n",
		server.GetRoomsCreated(), summaries.size(), server.GetSessionsPeak(), (unsigned long long)server.GetHelloRejected(), (unsigned long long)server.GetHelloTimeouts(), (unsigned long long)packetsNoRoom, elapsedSeconds > 0 ? routeSumUs / (elapsedSeconds * 1e4) : 0.0);
	std::printf("SERVER ticks=%llu intervalAvgMs=%.3f intervalMaxMs=%.3f lateTicks=%llu workUs p50=%.1f p99=%.1f max=%.1f\n",
		(unsigned long long)room.ticks, intervalCount ? intervalSumMs / (double)intervalCount : 0.0, intervalMaxMs, (unsigned long long)lateTicks, percentile(allWork, 0.5), percentile(allWork, 0.99), percentile(allWork, 1.0));
	std::printf("SERVER connected=%llu disconnected=%llu hellos=%llu badPackets=%llu\n",
		(unsigned long long)server.GetConnected(), (unsigned long long)server.GetDisconnected(), (unsigned long long)server.GetHellos(), (unsigned long long)badPackets);
	std::printf("SERVER inputs received=%llu applied=%llu whileDead=%llu droppedFull=%llu droppedOldSeq=%llu starvedTicks=%llu idleMoves=%llu debugPlaces=%llu statesSent=%llu\n",
		(unsigned long long)room.inputsReceived, (unsigned long long)room.inputsApplied, (unsigned long long)room.inputsWhileDead, (unsigned long long)room.inputsDroppedFull, (unsigned long long)room.inputsDroppedOldSeq,
		(unsigned long long)room.starvedTicks, (unsigned long long)room.idleMoves, (unsigned long long)room.debugPlaces, (unsigned long long)room.statesSent);
	std::printf("SERVER lockstep players=%d waits=%llu timeouts=%llu\n", args.testLockstep, (unsigned long long)lockstepWaits, (unsigned long long)lockstepTimeouts);
	std::printf("SERVER shots=%llu bullets=%llu hitMap=%llu hitBody=%llu hitImmune=%llu missed=%llu damageEvents=%llu\n",
		(unsigned long long)room.shots, (unsigned long long)room.bullets, (unsigned long long)room.bulletsHitMap, (unsigned long long)room.bulletsHitBody, (unsigned long long)room.bulletsHitImmune, (unsigned long long)room.bulletsMissed, (unsigned long long)room.damageEvents);
	std::printf("SERVER ads inputs=%llu shotsAimed=%llu (applied=%llu shots=%llu)\n",
		(unsigned long long)room.adsInputs, (unsigned long long)room.shotsAimed, (unsigned long long)room.inputsApplied, (unsigned long long)room.shots);
	std::printf("SERVER crouch presses=%llu crouchedInputs=%llu shotsCrouched=%llu (applied=%llu shots=%llu)\n",
		(unsigned long long)room.crouchPresses, (unsigned long long)room.crouchedInputs, (unsigned long long)room.shotsCrouched, (unsigned long long)room.inputsApplied, (unsigned long long)room.shots);
	// 사람끼리 막기(F-2) — 꺼진 판은 전부 0. 켜면 blockMoves = 산 채 굴린 입력(applied 중 몇), oddSimplex 는 늘 0 이어야 한다
	std::printf("SERVER pawn block=%d blockMoves=%llu others=%llu hits=%llu startPenetrating=%llu oddSimplex=%llu touchStart=%llu (applied=%llu)\n", args.rules.pawnBlock ? 1 : 0,
		(unsigned long long)room.pawnBlockMoves, (unsigned long long)room.pawnBlockOthers, (unsigned long long)room.pawnHits, (unsigned long long)room.pawnStartPenetrating,
		(unsigned long long)room.pawnOddSimplex, (unsigned long long)room.pawnTouchStart, (unsigned long long)room.inputsApplied);
	// 근접(E-3) — starts = hits + sameTeam + blocked + misses = 보낸 S2C_MELEE. 되감기 네 갈래 합 = starts
	std::printf("SERVER melee presses=%llu starts=%llu whileActive=%llu hits=%llu sameTeam=%llu blocked=%llu misses=%llu pushes=%llu hitImmune=%llu damageEvents=%llu kills=%llu targets=%llu poses=%llu poseSkipped=%llu handFallback=%llu firesBlocked=%llu reloadCancels=%llu rewind noView=%llu future=%llu clamped=%llu ok=%llu\n",
		(unsigned long long)room.meleePresses, (unsigned long long)room.meleeStarts, (unsigned long long)room.meleeWhileActive, (unsigned long long)room.meleeHits, (unsigned long long)room.meleeSameTeam, (unsigned long long)room.meleeBlocked, (unsigned long long)room.meleeMisses,
		(unsigned long long)room.meleePushes, (unsigned long long)room.meleeHitImmune, (unsigned long long)room.meleeDamageEvents, (unsigned long long)room.meleeKills, (unsigned long long)room.meleeTargets, (unsigned long long)room.meleePoses, (unsigned long long)room.meleePoseSkipped, (unsigned long long)room.meleeHandFallback,
		(unsigned long long)room.meleeFiresBlocked, (unsigned long long)room.meleeReloadCancels, (unsigned long long)room.meleeRewindNoView, (unsigned long long)room.meleeRewindFuture, (unsigned long long)room.meleeRewindClamped, (unsigned long long)room.meleeRewindOk);
	// 대시(D-2) — presses = started + noDirection + onCooldown + whileActive + dead(경기 끝 단계도 켠다 — L-3-b). 막기 · 끊기는 갈래마다(0 = 안 지나감인지 옆 수로 가린다). msgs = 보낸 S2C_DASH = started(D-3)
	std::printf("SERVER dash presses=%llu started=%llu noDirection=%llu onCooldown=%llu whileActive=%llu dead=%llu blocked fire=%llu reload=%llu melee=%llu jump=%llu ads=%llu cancel fire=%llu reload=%llu melee=%llu meleePush=%llu jump=%llu ads=%llu uncrouch=%llu liftoffs=%llu montage F=%llu B=%llu L=%llu R=%llu msgs=%llu (applied=%llu)\n",
		(unsigned long long)room.dashPresses, (unsigned long long)room.dashStarted, (unsigned long long)room.dashNoDirection, (unsigned long long)room.dashOnCooldown, (unsigned long long)room.dashWhileActive,
		(unsigned long long)room.dashDead,
		(unsigned long long)room.dashFiresBlocked, (unsigned long long)room.dashReloadsBlocked, (unsigned long long)room.dashMeleeBlocked, (unsigned long long)room.dashJumpsBlocked, (unsigned long long)room.dashAdsBlocked,
		(unsigned long long)room.dashFireCancels, (unsigned long long)room.dashReloadCancels, (unsigned long long)room.dashMeleeCancels, (unsigned long long)room.dashMeleePushRemoved, (unsigned long long)room.dashJumpCancels,
		(unsigned long long)room.dashAdsCancels, (unsigned long long)room.dashUncrouches, (unsigned long long)room.dashLiftoffs,
		(unsigned long long)room.dashMontage[0], (unsigned long long)room.dashMontage[1], (unsigned long long)room.dashMontage[2], (unsigned long long)room.dashMontage[3], (unsigned long long)room.dashMsgs, (unsigned long long)room.inputsApplied);
	// 수류탄(G-2) — presses = thrown + onCooldown + whileDash + dead(경기 끝 단계도 던진다 — L-3-b). 겨눔 갈래 넷의 합 = thrown. 터짐 timer + hit + alive(닫을 때 남은 수) = 생긴 수.
	// ownerLeft = 던진 사람이 나갈 때 살아 있던 수(남김 — L-3-d) · ownerGoneBody = 그 뒤 몸 맞음(안 터짐).
	// 폭발 과녁 targets = wall + ownerGone + immune + sameTeam + zero + damage(zero = 곡선 피해 0 — 450 안이면 0 보다 커서 0 이어야 한다)
	std::printf("SERVER grenade presses=%llu thrown=%llu onCooldown=%llu whileDash=%llu dead=%llu aim replaced=%llu noHit=%llu near45=%llu lookAt=%llu lookAtEnd=%llu listFull=%llu cancel melee=%llu meleePush=%llu reload=%llu bounces=%llu stops=%llu detonate timer=%llu hit=%llu alive=%llu ownerLeft=%llu ownerGoneBody=%llu targets=%llu wall=%llu ownerGone=%llu immune=%llu sameTeam=%llu zero=%llu damage=%llu self=%llu kills=%llu\n",
		(unsigned long long)room.grenadePresses, (unsigned long long)room.grenadeThrown, (unsigned long long)room.grenadeOnCooldown, (unsigned long long)room.grenadeWhileDash, (unsigned long long)room.grenadeDead,
		(unsigned long long)room.grenadeAimReplaced, (unsigned long long)room.grenadeAimNoHit, (unsigned long long)room.grenadeAimNear45, (unsigned long long)room.grenadeAimLookAt, (unsigned long long)room.grenadeAimLookAtEnd, (unsigned long long)room.grenadeListFull,
		(unsigned long long)room.grenadeMeleeCancels, (unsigned long long)room.grenadeMeleePushRemoved, (unsigned long long)room.grenadeReloadCancels, (unsigned long long)room.grenadeBounces, (unsigned long long)room.grenadeStops,
		(unsigned long long)room.grenadeDetonateTimer, (unsigned long long)room.grenadeDetonateHit, (unsigned long long)room.grenadeAliveAtClose,
		(unsigned long long)room.grenadeOwnerLeft, (unsigned long long)room.grenadeOwnerGoneBodyHits, (unsigned long long)room.grenadeTargets, (unsigned long long)room.grenadeWallHits, (unsigned long long)room.grenadeOwnerGoneTargets,
		(unsigned long long)room.grenadeImmune, (unsigned long long)room.grenadeSameTeam, (unsigned long long)room.grenadeZeroDamage, (unsigned long long)room.grenadeDamageEvents, (unsigned long long)room.grenadeSelfDamage, (unsigned long long)room.grenadeKills);
	// 수류탄 와이어(G-3) — throwMsgs = thrown(목록이 차 번호 0 인 것 포함) · boomMsgs = detonate timer + hit · stateBounced ≤ bounces
	std::printf("SERVER grenadeWire camBackClamped=%llu throwMsgs=%llu boomMsgs=%llu stateMsgs=%llu stateEntries=%llu stateBounced=%llu\n",
		(unsigned long long)room.grenadeCamBackClamped, (unsigned long long)room.grenadeThrowMsgs, (unsigned long long)room.grenadeBoomMsgs,
		(unsigned long long)room.grenadeStateMsgs, (unsigned long long)room.grenadeStateEntries, (unsigned long long)room.grenadeStateBounced);
	std::printf("REJECT badValue=%llu moveOver=%llu weaponId=%llu debugPlace=%llu aimFar=%llu swap=%llu aimWall=%llu droppedFull=%llu oldSeq=%llu badPackets=%llu | aimStale=%llu fireTooSoon=%llu fireEmpty=%llu seqGapFire=%llu\n",
		(unsigned long long)room.inputsRejectedBadValue, (unsigned long long)room.movesOverLength, (unsigned long long)room.weaponIdRejected, (unsigned long long)room.debugPlacesRejected, (unsigned long long)room.shotsAimReplaced,
		(unsigned long long)room.firesRejectedSwap, (unsigned long long)room.shotsAimPulledBack, (unsigned long long)room.inputsDroppedFull, (unsigned long long)room.inputsDroppedOldSeq, (unsigned long long)badPackets,
		(unsigned long long)room.shotsAimStaleAfterSpawn, (unsigned long long)room.firesTooSoon, (unsigned long long)room.firesEmpty, (unsigned long long)room.firesDroppedSeqGap);
	std::printf("SERVER rewind rewindNoView=%llu rewindFuture=%llu rewindClamped=%llu rewindOk=%llu rewindTicksSum=%llu hitsBoth=%llu hitsRewindOnly=%llu hitsNowOnly=%llu hitsNeither=%llu rewindOnlyBehindWall=%llu shots=%llu bullets=%llu\n",
		(unsigned long long)room.rewindNoView, (unsigned long long)room.rewindFuture, (unsigned long long)room.rewindClamped, (unsigned long long)room.rewindOk, (unsigned long long)room.rewindTicksSum,
		(unsigned long long)room.hitsBoth, (unsigned long long)room.hitsRewindOnly, (unsigned long long)room.hitsNowOnly, (unsigned long long)room.hitsNeither, (unsigned long long)room.rewindOnlyBehindWall, (unsigned long long)room.shots, (unsigned long long)room.bullets);
	if (args.anim.clips)
	{
		std::printf("SERVER anim updates=%llu poseTargets=%llu poseEvals=%llu poseReused=%llu poseSkipped=%llu stateBytesMax=%llu\n",
			(unsigned long long)room.animUpdates, (unsigned long long)room.poseTargets, (unsigned long long)room.poseEvals, (unsigned long long)room.poseReused, (unsigned long long)room.poseSkipped, (unsigned long long)room.stateBytesMax);
		std::printf("SERVER animBundle weaponChanges=%llu changes=%llu fails=%llu poseEvals %s=%llu %s=%llu %s=%llu\n",
			(unsigned long long)room.weaponChanges, (unsigned long long)room.animBundleChanges, (unsigned long long)room.animBundleFails,
			mo::AnimBundleOf(0), (unsigned long long)room.poseEvalsByWeapon[0], mo::AnimBundleOf(1), (unsigned long long)room.poseEvalsByWeapon[1], mo::AnimBundleOf(2), (unsigned long long)room.poseEvalsByWeapon[2]);
		// 사건 → 튼 몽타주(9단계 조각 4). 종류마다 사건 − 버림 = 튼 수(교체는 빼기 · 꺼내기 둘 다 그 수)여야 한다
		using mo::EAnimEvent;
		const auto ev = [&room](EAnimEvent kind) { return (unsigned long long)room.animEvents[(size_t)kind]; };
		const auto dropped = [&room](EAnimEvent kind) { return (unsigned long long)room.animEventsDropped[(size_t)kind]; };
		const auto starts = [&room](uint8_t kind) { return (unsigned long long)room.montageStarts[kind]; };
		std::printf("SERVER animMontage events spawn=%llu swap=%llu fire=%llu reload=%llu dropped spawn=%llu swap=%llu fire=%llu reload=%llu starts spawn=%llu unequip=%llu equip=%llu fire=%llu reload=%llu fails=%llu reloadCancels=%llu montageDropped=%llu\n",
			ev(EAnimEvent::Spawn), ev(EAnimEvent::Swap), ev(EAnimEvent::Fire), ev(EAnimEvent::Reload),
			dropped(EAnimEvent::Spawn), dropped(EAnimEvent::Swap), dropped(EAnimEvent::Fire), dropped(EAnimEvent::Reload),
			starts(mo::MONTAGE_SPAWN), starts(mo::MONTAGE_UNEQUIP), starts(mo::MONTAGE_EQUIP), starts(mo::MONTAGE_FIRE), starts(mo::MONTAGE_RELOAD),
			(unsigned long long)room.montageFails, (unsigned long long)room.reloadCancels, (unsigned long long)room.montageDrops);
		// 근접 몽타주(E-3) — 사건 − 버림 = 튼 수. 반응은 맞은 쪽에 쌓여 같은 틱에 죽으면 버린다
		std::printf("SERVER animMelee events melee=%llu hitBack=%llu hitFront=%llu dropped melee=%llu hitBack=%llu hitFront=%llu starts melee=%llu hitBack=%llu hitFront=%llu reloadCancels=%llu\n",
			ev(EAnimEvent::Melee), ev(EAnimEvent::HitBack), ev(EAnimEvent::HitFront), dropped(EAnimEvent::Melee), dropped(EAnimEvent::HitBack), dropped(EAnimEvent::HitFront),
			starts(mo::MONTAGE_MELEE), starts(mo::MONTAGE_HIT_BACK), starts(mo::MONTAGE_HIT_FRONT), (unsigned long long)room.meleeReloadMontageCancels);
		// 대시 몽타주(D-4) — 방향마다 사건 − 버림 = 튼 수. 사건 = SERVER dash 의 montage 칸(켠 방향)
		std::printf("SERVER animDash events forward=%llu backward=%llu left=%llu right=%llu dropped forward=%llu backward=%llu left=%llu right=%llu starts forward=%llu backward=%llu left=%llu right=%llu\n",
			ev(EAnimEvent::DashForward), ev(EAnimEvent::DashBackward), ev(EAnimEvent::DashLeft), ev(EAnimEvent::DashRight),
			dropped(EAnimEvent::DashForward), dropped(EAnimEvent::DashBackward), dropped(EAnimEvent::DashLeft), dropped(EAnimEvent::DashRight),
			starts(mo::MONTAGE_DASH_FORWARD), starts(mo::MONTAGE_DASH_BACKWARD), starts(mo::MONTAGE_DASH_LEFT), starts(mo::MONTAGE_DASH_RIGHT));
		// 수류탄 몽타주(G-4) — 사건 − 버림 = 튼 수. 사건 = SERVER grenade 의 thrown. afterFire = 같은 입력 쏘기를 던짐 앞에 끼운 수 · reloadCancels = 던짐이 먼저 끈 재장전 칸
		std::printf("SERVER animGrenade events=%llu dropped=%llu starts=%llu afterFire=%llu reloadCancels=%llu\n",
			ev(EAnimEvent::Grenade), dropped(EAnimEvent::Grenade), starts(mo::MONTAGE_GRENADE), (unsigned long long)room.grenadeAnimAfterFire,
			(unsigned long long)room.grenadeReloadMontageCancels);
	}
	std::printf("SERVER pickups=%llu fromRecheck=%llu fullHealth=%llu padRespawns=%llu padRechecks=%llu\n", (unsigned long long)room.pickups, (unsigned long long)room.pickupsFromRecheck,
		(unsigned long long)room.pickupsFullHealth, (unsigned long long)room.padRespawns, (unsigned long long)room.padRechecks);
	std::printf("SERVER weaponPickups new=%llu noSwitch=%llu ammo=%llu full=%llu noSlot=%llu postGame=%llu fromRecheck=%llu switchRejected=%llu debugAmmoSets=%llu debugKills=%llu\n",
		(unsigned long long)room.weaponPickupsNew, (unsigned long long)room.weaponPickupsNoSwitch, (unsigned long long)room.weaponPickupsAmmo, (unsigned long long)room.weaponPickupsFull,
		(unsigned long long)room.weaponPickupsNoSlot, (unsigned long long)room.weaponPickupsPostGame, (unsigned long long)room.weaponPickupsFromRecheck, (unsigned long long)room.weaponSwitchRejected,
		(unsigned long long)room.debugAmmoSets, (unsigned long long)room.debugKills);
	std::printf("SERVER kills=%llu assists=%llu falls=%llu spawns=%llu spawnsFromFallback=%llu phaseChanges=%llu finished=%d\n",
		(unsigned long long)room.kills, (unsigned long long)room.assists, (unsigned long long)room.falls, (unsigned long long)room.spawns, (unsigned long long)room.spawnsFromFallback, (unsigned long long)room.phaseChanges, firstFinished ? 1 : 0);
	if (firstFinished)
	{
		const mo::wire::Result& result = firstFinished->result;
		std::printf("TEAMSCORE a=%u b=%u target=%u winner=%u resultTick=%u\n", result.teamScoreA, result.teamScoreB, result.targetScore, result.winningTeam, result.tick);
		for (int i = 0; i < result.count; ++i)
		{
			std::printf("SCORE id=%u team=%u kills=%u deaths=%u assists=%u\n", result.players[i].id, result.players[i].team, result.players[i].kills, result.players[i].deaths, result.players[i].assists);
		}
	}
	return 0;
}
