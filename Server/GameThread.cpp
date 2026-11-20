#include "GameThread.h"

#include "KillRecord.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace mo
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr int kEmptyCloseTicks = 3 * wire::kTickRate;		// 시험 클라가 접속을 바꿔 끼우는 사이에 닫히지 않게 비어 있는 채로 3초를 기다린다
		constexpr int kFinishedCloseTicks = 2 * wire::kTickRate;	// 경기가 끝나면 결과가 나갈 틈만 주고 닫는다

		double CurrentThreadCpuSeconds()
		{
			FILETIME creation, exit, kernel, user;
			if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0;
			const auto seconds = [](const FILETIME& t) { return (double)(((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) * 1e-7; };
			return seconds(kernel) + seconds(user);
		}
	}

	class CGameThread::CRoomHost final : public IRoomSender
	{
	public:
		CRoomHost(CGameThread& owner, uint32_t id, const RoomSettings& settings)
			: _owner(owner)
			, _id(id)
			, _room(*settings.map, *settings.params, settings.rules, settings.seed, settings.debug, *this, id, settings.rewind, settings.anim, settings.pickups)
			, _lockstepPlayers(settings.lockstepPlayers)
		{
			if (!settings.recordPath.empty()) _record = std::fopen(settings.recordPath.c_str(), "wb");
		}

		~CRoomHost() override
		{
			if (_record) std::fclose(_record);
		}

		uint32_t GetId() const { return _id; }
		const RoomStats& GetRoomStats() const { return _room.GetStats(); }

		void SendToSession(int64_t session, const void* data, int size) override
		{
			_owner.Send(session, data, size);
		}

		void OnKillRecorded(const wire::Kill& kill) override
		{
			WriteRecord(KillRecordLine(kill));
		}

		// 반환 = 앉았나
		bool OnHello(int64_t session)
		{
			if (_room.AddPlayer(session) == 0)
			{
				GameReport report;
				report.type = GameReport::Type::HelloRejected;
				report.roomId = _id;
				report.session = session;
				_owner.Report(report);
				return false;
			}
			_used = true;
			_playersPeak = std::max(_playersPeak, _room.GetPlayerCount());
			return true;
		}

		void OnLeave(int64_t session) { _room.RemovePlayer(session); }

		void OnPacket(int64_t session, const char* data, int size)
		{
			if (size < (int)sizeof(wire::Header))
			{
				++_badPackets;
				return;
			}
			wire::Header header;
			std::memcpy(&header, data, sizeof(header));
			switch ((wire::Msg)header.type)
			{
			case wire::Msg::C2S_INPUT:
			{
				wire::Input input;
				if (Read(data, size, input)) _room.OnInput(session, input);
				break;
			}
			case wire::Msg::C2S_DEBUG_PLACE:
			{
				wire::DebugPlace place;
				if (Read(data, size, place)) _room.OnDebugPlace(session, place);
				break;
			}
			default:
				++_badPackets;
				break;
			}
		}

		// 반환 = 이 방을 닫을 때가 됐나
		bool Tick()
		{
			if (!LockstepReady()) return false;
			_room.Tick();

			const wire::EMatchPhase phase = _room.GetPhase();
			if (!_closedToNew && (phase == wire::EMatchPhase::PostGame || phase == wire::EMatchPhase::Finished))
			{
				_closedToNew = true;
				GameReport report;
				report.type = GameReport::Type::ClosedToNew;
				report.roomId = _id;
				_owner.Report(report);
			}
			if (phase == wire::EMatchPhase::Finished)
			{
				if (!_resultWritten)
				{
					_resultWritten = true;
					WriteRecord(ResultRecordLines(_room.GetResult().packet));
				}
				if (_room.GetPlayerCount() == 0 || ++_finishedTicks >= kFinishedCloseTicks) return true;
			}
			_emptyTicks = _used && _room.GetPlayerCount() == 0 ? _emptyTicks + 1 : 0;
			return _emptyTicks >= kEmptyCloseTicks;
		}

		std::shared_ptr<RoomSummary> MakeSummary(int thread) const
		{
			std::shared_ptr<RoomSummary> summary = std::make_shared<RoomSummary>();
			summary->id = _id;
			summary->thread = thread;
			summary->stats = _room.GetStats();
			summary->badPackets = _badPackets;
			summary->playersPeak = _playersPeak;
			summary->finished = _room.GetPhase() == wire::EMatchPhase::Finished;
			if (summary->finished) summary->result = _room.GetResult().packet;
			summary->lockstepWaits = _lockstepWaits;
			summary->lockstepTimeouts = _lockstepTimeouts;
			return summary;
		}

	private:
		// 기다림 판(RoomSettings::lockstepPlayers) — 이 게임 스레드 틱에 방 틱을 돌려도 되나. 끝난 방 · 끈 판은 늘 돈다.
		// 봇이 받은 상태 틱이 늘 「직전 입력을 적용한 틱」이 되려면 서버가 그 다음 입력 없이 앞서 가면 안 된다.
		// 한도(시작 10초 · 틱마다 1초)는 봇이 멎어도 판이 안 멈추게 — 걸리면 lockstepTimeouts 로 세고 판정이 FAIL 로 본다
		bool LockstepReady()
		{
			if (_lockstepPlayers <= 0 || _room.GetPhase() == wire::EMatchPhase::Finished) return true;
			const bool seated = _lockstepStarted || (int)_room.GetPlayerCount() >= _lockstepPlayers;
			if (seated && _room.EveryPlayerHasInput())
			{
				_lockstepStarted = true;
				_lockstepWaited = 0;
				return true;
			}
			++_lockstepWaits;
			if (++_lockstepWaited < (_lockstepStarted ? kLockstepTickLimit : kLockstepStartLimit)) return false;
			++_lockstepTimeouts;
			_lockstepStarted = true;
			_lockstepWaited = 0;
			return true;
		}
		static constexpr int kLockstepStartLimit = 10 * wire::kTickRate;
		static constexpr int kLockstepTickLimit = wire::kTickRate;

		void WriteRecord(const std::string& text)
		{
			if (!_record) return;
			std::fwrite(text.data(), 1, text.size(), _record);
			std::fflush(_record);
		}

		template <class T>
		bool Read(const char* data, int size, T& out)
		{
			if (size != (int)sizeof(T))
			{
				++_badPackets;
				return false;
			}
			std::memcpy(&out, data, sizeof(T));
			return true;
		}

		CGameThread& _owner;
		const uint32_t _id;
		CRoom _room;
		FILE* _record = nullptr;
		uint64_t _badPackets = 0;
		size_t _playersPeak = 0;
		int _emptyTicks = 0;
		int _finishedTicks = 0;
		bool _used = false;
		bool _closedToNew = false;
		bool _resultWritten = false;
		const int _lockstepPlayers;
		bool _lockstepStarted = false;
		int _lockstepWaited = 0;
		uint64_t _lockstepWaits = 0;
		uint64_t _lockstepTimeouts = 0;
	};

	CGameThread::CGameThread(int index, NetIoModel& net)
		: _index(index)
		, _net(net)
	{
		_stats.workMicroseconds.reserve(600000);
	}

	CGameThread::~CGameThread() = default;

	void CGameThread::SetMeasure(Clock::time_point begin, Clock::time_point end)
	{
		_measure = Measure::Before;
		_measureBegin = begin;
		_measureEnd = end;
	}

	RoomStats CGameThread::SumRooms() const
	{
		RoomStats total;
		for (const std::unique_ptr<CRoomHost>& room : _rooms) AddRoomStats(total, room->GetRoomStats());
		return total;
	}

	void CGameThread::UpdateMeasure(Clock::time_point now)
	{
		if (_measure == Measure::Before && now >= _measureBegin) OpenMeasure();
		else if (_measure == Measure::During && now >= _measureEnd) CloseMeasure();
	}

	void CGameThread::OpenMeasure()
	{
		GameThreadStats fresh;
		fresh.roomsTotal = _stats.roomsTotal;
		fresh.roomsPeak = _stats.roomsPeak;
		fresh.workMicroseconds.swap(_stats.workMicroseconds);
		fresh.workMicroseconds.clear();
		_stats = std::move(fresh);
		_measureRoomsBegin = SumRooms();
		_measureCpuBegin = CurrentThreadCpuSeconds();
		_measure = Measure::During;
	}

	void CGameThread::CloseMeasure()
	{
		_stats.measured = true;
		_stats.measuredRooms = SumRooms();
		SubtractRoomStats(_stats.measuredRooms, _measureRoomsBegin);
		_stats.roomsAtEnd = _rooms.size();
		_stats.sessionsAtEnd = _sessionRoom.size();
		_stats.cpuSeconds = CurrentThreadCpuSeconds() - _measureCpuBegin;
		_measuredStats = _stats;
		_measure = Measure::After;
	}

	void CGameThread::Start()
	{
		_thread = std::thread([this] { Run(); });
	}

	void CGameThread::RequestStop()
	{
		_stop = true;
	}

	void CGameThread::Join()
	{
		if (_thread.joinable()) _thread.join();
	}

	void CGameThread::PostCreateRoom(uint32_t roomId, const RoomSettings& settings)
	{
		std::unique_ptr<CRoomHost> room(new CRoomHost(*this, roomId, settings));
		const std::lock_guard<std::mutex> lock(_inboxLock);
		_inboxRooms.push_back(std::move(room));
	}

	void CGameThread::PostHello(uint32_t roomId, int64_t session)
	{
		const std::lock_guard<std::mutex> lock(_inboxLock);
		_inboxCommands.push_back(Command{ Command::Type::Hello, roomId, session });
	}

	void CGameThread::PostLeave(uint32_t roomId, int64_t session)
	{
		const std::lock_guard<std::mutex> lock(_inboxLock);
		_inboxCommands.push_back(Command{ Command::Type::Leave, roomId, session });
	}

	void CGameThread::DrainReports(std::vector<GameReport>& out)
	{
		const std::lock_guard<std::mutex> lock(_reportLock);
		out.insert(out.end(), _reports.begin(), _reports.end());
		_reports.clear();
	}

	void CGameThread::Report(const GameReport& report)
	{
		const std::lock_guard<std::mutex> lock(_reportLock);
		_reports.push_back(report);
	}

	void CGameThread::Send(int64_t session, const void* data, int size)
	{
		const auto begin = Clock::now();
		_net.RequestSendRaw(_sendBatch, session, static_cast<const char*>(data), size);
		_stats.sendSumUs += std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
		_stats.sendBytes += (uint64_t)size;
		++_stats.sendPackets;
	}

	void CGameThread::ProcessInbox()
	{
		std::vector<std::unique_ptr<CRoomHost>> newRooms;
		{
			const std::lock_guard<std::mutex> lock(_inboxLock);
			newRooms.swap(_inboxRooms);
			_commands.swap(_inboxCommands);
		}
		for (std::unique_ptr<CRoomHost>& room : newRooms)
		{
			_roomById[room->GetId()] = room.get();
			_rooms.push_back(std::move(room));
			++_stats.roomsTotal;
		}
		_stats.roomsPeak = std::max(_stats.roomsPeak, _rooms.size());

		// 앉히기 → 입력 → 나가기 순서. 명령을 먼저 꺼냈으니 나간 접속의 마지막 입력은 이미 받은 큐에 들어와 있다(워커가 입력을 넣은 뒤에야 끊김 이벤트가 라우터로 간다)
		const auto findRoom = [this](const Command& command) -> CRoomHost*
		{
			// 못 찾으면 라우터가 방이 닫힌 줄 모르고 보낸 것 — 그 방의 접속은 라우터가 곧 끊는다
			const auto found = _roomById.find(command.roomId);
			return found == _roomById.end() ? nullptr : found->second;
		};
		for (const Command& command : _commands)
		{
			if (command.type != Command::Type::Hello) continue;
			CRoomHost* room = findRoom(command);
			if (room && room->OnHello(command.session)) _sessionRoom[command.session] = room;
		}
		ProcessPackets();
		for (const Command& command : _commands)
		{
			if (command.type != Command::Type::Leave) continue;
			CRoomHost* room = findRoom(command);
			if (!room) continue;
			room->OnLeave(command.session);
			_sessionRoom.erase(command.session);
		}
		_commands.clear();
	}

	void CGameThread::ProcessPackets()
	{
		_recvQueue.SwapOut(_recvEvents);
		while (!_recvEvents.empty())
		{
			NetworkEvent& e = _recvEvents.front();
			const auto seated = _sessionRoom.find(e.sessionId);
			if (seated != _sessionRoom.end()) seated->second->OnPacket(e.sessionId, e.pMsg->GetReadBufferPtr(), e.pMsg->GetDataSize());
			else ++_stats.packetsNoRoom;
			e.pMsg->SubRef();
			_recvEvents.pop();
		}
	}

	void CGameThread::TickRooms()
	{
		for (size_t i = 0; i < _rooms.size();)
		{
			++_stats.roomTicks;
			if (!_rooms[i]->Tick())
			{
				++i;
				continue;
			}
			GameReport report;
			report.type = GameReport::Type::Closed;
			report.roomId = _rooms[i]->GetId();
			report.summary = _rooms[i]->MakeSummary(_index);
			Report(report);
			++_stats.roomsClosedInMeasure;
			_roomById.erase(report.roomId);
			for (auto seated = _sessionRoom.begin(); seated != _sessionRoom.end();)
			{
				if (seated->second == _rooms[i].get()) seated = _sessionRoom.erase(seated);
				else ++seated;
			}
			_rooms[i] = std::move(_rooms.back());	// 순서는 뜻이 없다 — 방끼리는 안 만난다
			_rooms.pop_back();
		}
	}

	void CGameThread::FlushSends()
	{
		const auto begin = Clock::now();
		_net.FlushPendingSends(_sendBatch);
		const double us = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
		_stats.sendSumUs += us;
		_stats.flushSumUs += us;
	}

	void CGameThread::Run()
	{
		const auto tickDuration = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / wire::kTickRate));
		auto next = Clock::now() + tickDuration;
		auto previous = Clock::now();

		while (!_stop)
		{
			// 눈금이 1ms 라 잠은 앞까지만 자고 나머지는 양보하며 기다린다
			for (;;)
			{
				const auto now = Clock::now();
				if (now >= next) break;
				if (next - now > std::chrono::milliseconds(2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
				else std::this_thread::yield();
			}

			const auto begin = Clock::now();
			UpdateMeasure(begin);
			const double intervalMs = std::chrono::duration<double, std::milli>(begin - previous).count();
			previous = begin;
			_stats.intervalSumMs += intervalMs;
			_stats.intervalMaxMs = std::max(_stats.intervalMaxMs, intervalMs);
			++_stats.ticks;

			ProcessInbox();
			const auto inboxEnd = Clock::now();
			TickRooms();
			const auto roomsEnd = Clock::now();
			FlushSends();

			const auto end = Clock::now();
			_stats.inboxSumUs += std::chrono::duration<double, std::micro>(inboxEnd - begin).count();
			_stats.roomsSumUs += std::chrono::duration<double, std::micro>(roomsEnd - inboxEnd).count();
			_stats.workMicroseconds.push_back(std::chrono::duration<float, std::micro>(end - begin).count());
			next += tickDuration;
			if (end > next) ++_stats.lateTicks;	// 다음 틱 시각을 넘김 — 쉬지 않고 바로 이어서 돌아 따라잡는다
		}

		if (_measure == Measure::During) CloseMeasure();	// 구간이 끝나기 전에 멈춤

		// 남은 방의 숫자도 라우터가 합칠 수 있게 알린다
		for (const std::unique_ptr<CRoomHost>& room : _rooms)
		{
			GameReport report;
			report.type = GameReport::Type::Closed;
			report.roomId = room->GetId();
			report.summary = room->MakeSummary(_index);
			Report(report);
		}
		_sessionRoom.clear();
		_roomById.clear();
		_rooms.clear();
	}
}
