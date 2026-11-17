#pragma once

#include "GameProtocol.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Room.h"

#include "NetIoModel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// 게임 스레드 하나 — 고정 배정된 방들을 60Hz 로 돌린다. 방 안 상태는 이 스레드만 만진다.
// - 왜: 플랜 확정값 = 게임 스레드 N개에 방을 고정 배정. 앉히기·방 만들기는 라우터(main.cpp) 한 곳이 맡고, 받은 입력은 네트워크 워커가 이 스레드의 큐로 바로 넣는다.
// - 송신은 이 스레드의 묶음(CSendBatch)에 쌓아 틱 끝에 넘긴다. 접속 하나는 방 하나, 방 하나는 이 스레드 하나라 스레드끼리 같은 접속에 보내는 일이 없다.
namespace mo
{
	struct RoomSettings
	{
		const CMapCollision* map = nullptr;
		const MoveParams* params = nullptr;
		MatchRules rules;
		uint32_t seed = 0;
		bool debug = false;
		std::string recordPath;		// 비어 있으면 안 적는다
		RewindOptions rewind;
		AnimOptions anim;
		PickupOptions pickups;
	};

	struct RoomSummary
	{
		uint32_t id = 0;
		int thread = 0;
		RoomStats stats;
		uint64_t badPackets = 0;
		size_t playersPeak = 0;
		bool finished = false;
		wire::Result result = wire::Result();
	};

	struct GameReport
	{
		enum class Type : uint8_t
		{
			HelloRejected,	// 방이 받지 못함 — 라우터가 다시 앉힌다(방을 닫는 것은 ClosedToNew 만)
			ClosedToNew,	// 경기 끝 단계에 들어가 새 접속을 못 받음
			Closed,			// 방이 닫힘 — 라우터가 남은 접속을 끊는다
		};
		Type type = Type::Closed;
		uint32_t roomId = 0;
		int64_t session = 0;
		std::shared_ptr<RoomSummary> summary;	// Closed 일 때
	};

	// RoomStats 는 전부 uint64_t 다
	inline void AddRoomStats(RoomStats& total, const RoomStats& add)
	{
		static_assert(sizeof(RoomStats) % sizeof(uint64_t) == 0, "RoomStats 는 uint64_t 만");
		const uint64_t stateBytesMax = std::max(total.stateBytesMax, add.stateBytesMax);		// 최대는 더하지 않는다
		uint64_t* to = reinterpret_cast<uint64_t*>(&total);
		const uint64_t* from = reinterpret_cast<const uint64_t*>(&add);
		for (size_t i = 0; i < sizeof(RoomStats) / sizeof(uint64_t); ++i) to[i] += from[i];
		total.stateBytesMax = stateBytesMax;
	}

	inline void SubtractRoomStats(RoomStats& total, const RoomStats& sub)
	{
		const uint64_t stateBytesMax = total.stateBytesMax;		// 최대는 빼지 않는다
		uint64_t* to = reinterpret_cast<uint64_t*>(&total);
		const uint64_t* from = reinterpret_cast<const uint64_t*>(&sub);
		for (size_t i = 0; i < sizeof(RoomStats) / sizeof(uint64_t); ++i) to[i] -= from[i];
		total.stateBytesMax = stateBytesMax;
	}

	// 측정 구간이 있으면 틱 숫자는 그 구간 것만 남는다
	struct GameThreadStats
	{
		uint64_t ticks = 0;
		uint64_t lateTicks = 0;
		uint64_t roomTicks = 0;
		uint64_t roomsTotal = 0;
		uint64_t packetsNoRoom = 0;		// 방이 막 닫혔거나 인사 처리 전 — 게임 규칙 위반으로 세지 않는다
		size_t roomsPeak = 0;
		double intervalSumMs = 0;
		double intervalMaxMs = 0;
		double sendSumUs = 0;			// 쌓기 + 넘기기
		double flushSumUs = 0;			// 넘기기만
		double inboxSumUs = 0;
		double roomsSumUs = 0;
		uint64_t sendBytes = 0;
		uint64_t sendPackets = 0;
		std::vector<float> workMicroseconds;

		// 측정 구간이 닫힐 때 적는다
		bool measured = false;
		RoomStats measuredRooms;		// 구간 동안 늘어난 양
		uint64_t roomsClosedInMeasure = 0;	// 0 이어야 measuredRooms 가 온전하다
		size_t roomsAtEnd = 0;
		size_t sessionsAtEnd = 0;
		double cpuSeconds = 0;
	};

	class CGameThread
	{
	public:
		CGameThread(int index, NetIoModel& net);
		~CGameThread();

		CGameThread(const CGameThread&) = delete;
		CGameThread& operator=(const CGameThread&) = delete;

		// Start 전에. begin 부터 end 까지만 잰다 — 접속이 다 붙기 전 숫자를 빼려고
		void SetMeasure(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end);

		void Start();
		void RequestStop();
		void Join();					// 멈출 때 남은 방도 Closed 로 알린다

		// 라우터 스레드에서 부른다
		void PostCreateRoom(uint32_t roomId, const RoomSettings& settings);
		void PostHello(uint32_t roomId, int64_t session);
		void PostLeave(uint32_t roomId, int64_t session);
		void DrainReports(std::vector<GameReport>& out);

		ThreadSafeQueue<NetworkEvent>& GetRecvQueue() { return _recvQueue; }	// 네트워크 워커가 넣고 이 스레드가 뺀다

		int GetIndex() const { return _index; }
		const GameThreadStats& GetStats() const { return _measure == Measure::After ? _measuredStats : _stats; }		// Join 뒤에만

	private:
		class CRoomHost;

		struct Command
		{
			enum class Type : uint8_t { Hello, Leave };
			Type type;
			uint32_t roomId;
			int64_t session;
		};

		void Run();
		void ProcessInbox();
		void ProcessPackets();
		void TickRooms();
		void FlushSends();
		void Send(int64_t session, const void* data, int size);
		void Report(const GameReport& report);
		void UpdateMeasure(std::chrono::steady_clock::time_point now);
		void OpenMeasure();
		void CloseMeasure();
		RoomStats SumRooms() const;

		const int _index;
		NetIoModel& _net;
		std::thread _thread;
		std::atomic<bool> _stop{ false };

		std::mutex _inboxLock;
		std::vector<std::unique_ptr<CRoomHost>> _inboxRooms;
		std::vector<Command> _inboxCommands;

		ThreadSafeQueue<NetworkEvent> _recvQueue;

		std::mutex _reportLock;
		std::vector<GameReport> _reports;

		// 여기부터 이 스레드만 만진다
		std::vector<Command> _commands;
		std::queue<NetworkEvent> _recvEvents;
		std::vector<std::unique_ptr<CRoomHost>> _rooms;
		std::unordered_map<uint32_t, CRoomHost*> _roomById;
		std::unordered_map<int64_t, CRoomHost*> _sessionRoom;
		CSendBatch _sendBatch;
		GameThreadStats _stats;

		enum class Measure : uint8_t { Off, Before, During, After };
		Measure _measure = Measure::Off;
		std::chrono::steady_clock::time_point _measureBegin;
		std::chrono::steady_clock::time_point _measureEnd;
		RoomStats _measureRoomsBegin;
		double _measureCpuBegin = 0;
		GameThreadStats _measuredStats;
	};
}
