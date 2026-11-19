#include "Room.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace mo
{
	namespace
	{
		constexpr float kTickSeconds = 1.f / (float)wire::kTickRate;
		constexpr double kPi = 3.14159265358979323846;

		// 빠른 칸 한 칸을 로그용 수로 — 빈 칸 -1
		int SlotNumber(uint8_t weapon) { return weapon == kNoWeapon ? -1 : (int)weapon; }

		// 시선 허용 범위 — 다리는 yaw ±180·pitch ±90 안에서 보내고, 봇은 조준 오차로 각각 1도까지 넘는다
		constexpr float kMaxAbsYawDegrees = 360.f;
		constexpr float kMaxAbsPitchDegrees = 95.f;

		// 사격 시작점 허용 범위 — Lyra 기록의 최대(내 위치에서 92.5cm, 수평 60.6cm)에 이동 한 틱(10cm)과 여유를 더한 값
		// 조준 카메라를 넣어 시선 ±89도를 훑은 판(C-5 `c5sweep`)의 최대는 100.1cm(조준 · 시선 −10~0도) → 한 틱 더해 110.1, 그대로 둔다
		// 앉은 직후 0.25초는 117.8cm(D-6-a `d6asweep2`) — 앉는 순간 캡슐 중심은 25 내려가는데 Lyra 카메라는 받침점을 그대로 두고 0.2초에 걸쳐 앉은 눈높이로 내린다
		// (LyraCameraMode.cpp GetPivotLocation · LyraCameraMode_ThirdPerson.cpp UpdateCrouchOffset). 그래서 앉은 직후 거리는 「서서 잰 거리 + 25」를 못 넘는다 →
		// 100.1 + 25 = 125.1, 한 틱 더해 135.1 → 140(D66). 앉은 뒤 0.25초가 지나면 89.7 로 서 있을 때보다 작다
		constexpr double kAimMaxDistance = 140.0;
		// 수평만 따로 묶지 않는다 — 72도쯤 내려다보면 정직한 값도 수평 91.8cm 까지 나온다(실측 d6). 간격이 수평으로 눕기 때문
		constexpr double kAimFallbackHeight = 60.0;		// 범위 밖일 때 쓰는 시작점 — 내 위치에서 이만큼 위
		// 앉았으면 눈이 내려간 만큼 같이 내린다 — Lyra 눈높이 서기 80 · 앉음 32(0-3 실행 기록, 실측). 앉은 위치는 이미 캡슐 중심이라 눈 아래 20 이라는 거리가 서기와 같다.
		// 앉아 숨은 채 위조한 시작점으로 선 눈높이에서 쏘지 못하게(제 판단 — 60 자체는 처음부터의 값이고 근거 기록이 없다)
		constexpr double kAimFallbackCrouchDrop = 80.0 - 32.0;
		constexpr int kAimStaleInputs = 30;				// 부활 뒤 이만큼은 부활 소식을 듣기 전에 보낸 입력일 수 있다(0.5초치)

		bool IsAimInRange(const Vec3& aim, const Vec3& location)
		{
			const Vec3 d = aim - location;
			return d.x * d.x + d.y * d.y + d.z * d.z <= kAimMaxDistance * kAimMaxDistance;
		}

		bool IsFiniteInput(const wire::Input& input)
		{
			return std::isfinite(input.moveX) && std::isfinite(input.moveY) && std::isfinite(input.yaw) && std::isfinite(input.pitch)
				&& std::isfinite(input.aimX) && std::isfinite(input.aimY) && std::isfinite(input.aimZ);
		}

		// 0~1 밖이면 보간이 몸을 아무 데나 둘 수 있다 — 시점을 안 믿는 대신 입력째 버린다. NaN 도 이 비교에서 걸린다
		bool IsViewAlphaInRange(const wire::Input& input)
		{
			return input.viewAlpha >= 0.f && input.viewAlpha <= 1.f;
		}

		float NormalizeAxis(float degrees)
		{
			float d = std::fmod(degrees, 360.f);
			if (d > 180.f) d -= 360.f;
			else if (d < -180.f) d += 360.f;
			return d;
		}

		// 남에게 그대로 전해지는 값이라 범위 밖이면 입력째 버린다
		bool IsViewInRange(const wire::Input& input)
		{
			return std::fabs(input.yaw) <= kMaxAbsYawDegrees && std::fabs(input.pitch) <= kMaxAbsPitchDegrees;
		}

		double Length(const Vec3& v)
		{
			return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		}

		// 선분에서 점까지 거리의 제곱
		double SegmentPointDistanceSquared(const Vec3& start, const Vec3& end, const Vec3& point)
		{
			const Vec3 segment = end - start;
			const Vec3 toPoint = point - start;
			const double lengthSquared = segment.x * segment.x + segment.y * segment.y + segment.z * segment.z;
			const double along = toPoint.x * segment.x + toPoint.y * segment.y + toPoint.z * segment.z;
			const double t = lengthSquared > 0.0 ? std::min(std::max(along / lengthSquared, 0.0), 1.0) : 0.0;
			const Vec3 gap = toPoint - segment * t;
			return gap.x * gap.x + gap.y * gap.y + gap.z * gap.z;
		}

		// 일부러 틀림 전용 --rewindMontageShift(Room.h RewindOptions) — 판정이 푸는 몽타주 시각만 민다. 0 이면 기록 그대로를 돌려준다.
		//   기록은 안 고친다 — SendState 가 같은 칸(rewindHistory[_tick].anim)을 보내므로, 기록을 밀면 화면도 같이 밀려 틀림이 안 드러난다
		const wire::AnimStateWire& JudgeAnim(const wire::AnimStateWire& recorded, uint32_t shiftTicks, wire::AnimStateWire& scratch)
		{
			if (shiftTicks == 0) return recorded;
			scratch = recorded;
			for (int m = 0; m < (int)scratch.montageCount; ++m)
			{
				// 1틱 = 1/60초 = 와이어 시각 100칸(1/6000초). 0xFFFF 는 「안 함」 표시라 그 앞에서 멈춘다. 끝을 넘으면 자세 쪽이 몽타주 길이에서 멈춘다
				const uint32_t shifted = (uint32_t)scratch.montages[m].time + 100u * shiftTicks;
				scratch.montages[m].time = (uint16_t)std::min<uint32_t>(shifted, 0xFFFEu);
			}
			return scratch;
		}

		wire::EShotHit ToWireHit(EBulletHit hit)
		{
			switch (hit)
			{
			case EBulletHit::None: return wire::EShotHit::None;
			case EBulletHit::Map: return wire::EShotHit::Map;
			case EBulletHit::Body: return wire::EShotHit::Body;
			}
			return wire::EShotHit::None;
		}
	}

	static_assert(wire::kWeaponMelee == (uint8_t)EWeapon::Count, "근접 무기 번호 = 무기 셋 뒤");
	static_assert(wire::MELEE_NONE == (uint8_t)EMeleeHit::None && wire::MELEE_SAME_TEAM == (uint8_t)EMeleeHit::SameTeam
		&& wire::MELEE_BLOCKED == (uint8_t)EMeleeHit::Blocked && wire::MELEE_HIT == (uint8_t)EMeleeHit::Hit, "S2C_MELEE 결과 = EMeleeHit 차례");
	// 대시 사건 · 몽타주 종류 = 기준 + EDashDir(StartDash · PlayAnimEvents 가 더해서 쓴다)
	static_assert((uint8_t)EAnimEvent::DashBackward - (uint8_t)EAnimEvent::DashForward == (uint8_t)EDashDir::Backward
		&& (uint8_t)EAnimEvent::DashLeft - (uint8_t)EAnimEvent::DashForward == (uint8_t)EDashDir::Left
		&& (uint8_t)EAnimEvent::DashRight - (uint8_t)EAnimEvent::DashForward == (uint8_t)EDashDir::Right
		&& (uint8_t)EDashDir::Forward == 0, "대시 사건 차례 = EDashDir");
	static_assert(MONTAGE_DASH_BACKWARD - MONTAGE_DASH_FORWARD == (int)EDashDir::Backward && MONTAGE_DASH_LEFT - MONTAGE_DASH_FORWARD == (int)EDashDir::Left
		&& MONTAGE_DASH_RIGHT - MONTAGE_DASH_FORWARD == (int)EDashDir::Right, "대시 몽타주 종류 차례 = EDashDir");

	ERewindKind ClassifyRewind(uint32_t nowTick, uint32_t viewTick, float viewAlpha, uint32_t limitTicks)
	{
		if (viewTick == 0) return ERewindKind::NoView;
		if (viewTick > nowTick || (viewTick == nowTick && viewAlpha > 0.f)) return ERewindKind::Future;
		if (nowTick - viewTick > limitTicks) return ERewindKind::Clamped;
		return ERewindKind::Ok;
	}

	CRoom::CRoom(const CMapCollision& map, const MoveParams& params, const MatchRules& rules, uint32_t seed, bool allowDebug, IRoomSender& sender, uint32_t roomId, const RewindOptions& rewind, const AnimOptions& anim,
		const PickupOptions& pickups)
		: _map(map), _params(params), _rules(rules), _allowDebug(allowDebug), _sender(sender), _roomId(roomId), _rewind(rewind), _anim(anim), _pickups(pickups), _moveSim(map, _params), _spawnRng(seed + 1)
	{
		_padStates.resize(PadCount());
		if (_anim.clips)
		{
			// main 이 시작할 때 같은 초기화로 확인했으므로 여기서 실패하지 않는다
			_animReady = true;
			for (size_t weapon = 0; weapon < _poseEvaluators.size(); ++weapon)
			{
				const char* bundle = AnimBundleOf((uint8_t)weapon);
				_animReady = _animReady && _poseEvaluators[weapon].Init(*_anim.clips, bundle) && _animCodecs[weapon].Init(*_anim.clips, bundle);
				_animBundleIndex[weapon] = _animCodecs[weapon].BundleIndex();
			}
			if (_animReady)
			{
				// 뼈대는 묶음끼리 같다(make_clip_pack.py 한 몸 검사) — 뼈 수는 어느 묶음 것이든 같다
				_localFrom.resize(_poseEvaluators[0].BoneCount());
				_localTo.resize(_poseEvaluators[0].BoneCount());
				_localBlend.resize(_poseEvaluators[0].BoneCount());
			}
		}

		size_t count = 0;
		const SpawnPoint* starts = GetExpanseStarts(count);
		for (size_t i = 0; i < count; ++i)
		{
			StartSlot slot;
			slot.point = starts[i];
			_starts.push_back(slot);
		}
	}

	CRoom::Player* CRoom::Find(int64_t session)
	{
		for (const std::unique_ptr<Player>& player : _players)
		{
			if (player->session == session) return player.get();
		}
		return nullptr;
	}

	const CRoom::Player* CRoom::Find(int64_t session) const
	{
		for (const std::unique_ptr<Player>& player : _players)
		{
			if (player->session == session) return player.get();
		}
		return nullptr;
	}

	CRoom::Player* CRoom::FindById(uint32_t id)
	{
		for (const std::unique_ptr<Player>& player : _players)
		{
			if (player->id == id) return player.get();
		}
		return nullptr;
	}

	// ULyraTeamCreationComponent::GetLeastPopulatedTeamID — 적은 쪽, 같으면 작은 번호
	uint8_t CRoom::ChooseTeam() const
	{
		int a = 0, b = 0;
		for (const std::unique_ptr<Player>& player : _players)
		{
			if (player->team == wire::kTeamA) ++a;
			else ++b;
		}
		return a <= b ? wire::kTeamA : wire::kTeamB;
	}

	uint32_t CRoom::AddPlayer(int64_t session)
	{
		if ((int)_players.size() >= wire::kMaxPlayersInRoom) return 0;
		if (_phase == wire::EMatchPhase::PostGame || _phase == wire::EMatchPhase::Finished) return 0;

		std::unique_ptr<Player> player(new Player());
		player->id = _nextPlayerId++;
		player->session = session;
		player->team = ChooseTeam();

		wire::Welcome welcome = wire::Welcome();
		welcome.h.size = (uint16_t)sizeof(wire::Welcome);
		welcome.h.type = (uint16_t)wire::Msg::S2C_WELCOME;
		welcome.playerId = player->id;
		welcome.tick = _tick;
		welcome.team = player->team;
		welcome.debugAllowed = _allowDebug ? 1 : 0;
		welcome.roomId = _roomId;
		_sender.SendToSession(session, &welcome, (int)sizeof(welcome));		// 환영이 먼저 — 클라가 자기 번호로 시작 자리를 알아본다

		const uint32_t id = player->id;
		Player& added = *player;
		_players.push_back(std::move(player));
		if (_animReady)
		{
			added.anim.Init(*_anim.clips, AnimBundleOf(added.weapon));
			added.animWeapon = added.weapon;
		}
		added.padOverlap.assign(PadCount(), 0);
		for (size_t i = 0; i < PadCount(); ++i) SendPad(i, wire::PICKUP_SYNC, nullptr, 0.f, &added);	// 늦게 들어와도 꺼진 받침 · 남은 대기를 안다
		SpawnPlayer(added);

		if (_phase == wire::EMatchPhase::Waiting)
		{
			EnterPhase(wire::EMatchPhase::Warmup);	// Lyra 는 경험이 준비되면 혼자 시작한다 — 여기서는 첫 사람이 들어온 때
		}
		else
		{
			SendMatch();
		}
		return id;
	}

	void CRoom::RemovePlayer(int64_t session)
	{
		const Player* player = Find(session);
		if (!player) return;
		// 측정 전용 — 나간 뒤 도착한 탄은 그 몸이 없어 뒤 벽에 맞는다. 봇 판정 대조가 이 줄 뒤의 탄을 DEADSKIP 처럼 뺀다(C-5:
		// 판 끝에 봇이 한꺼번에 끊으면 중계가 쏜 쪽 마지막 입력을 흔들림만큼 붙든 사이 맞은 쪽이 먼저 나간다)
		if (_rewind.bulletLog) std::printf("LEAVE id=%u tick=%u\n", player->id, _tick);
		// 도움 기록에서 뺀다 — AssistProcessor 는 나간 사람의 PlayerState 가 비어 건너뛴다(AssistProcessor.cpp:49)
		for (const std::unique_ptr<Player>& other : _players)
		{
			uint32_t* const end = other->damagedBy.data() + other->damagedByCount;
			other->damagedByCount = (int)(std::remove(other->damagedBy.data(), end, player->id) - other->damagedBy.data());
		}
		for (StartSlot& slot : _starts)
		{
			if (slot.claimedBy == player->id) slot.claimedBy = 0;
		}
		_players.erase(std::remove_if(_players.begin(), _players.end(), [session](const std::unique_ptr<Player>& p) { return p->session == session; }), _players.end());
	}

	bool CRoom::PushQueue(Player& player, const QueuedInput& item, uint32_t seq)
	{
		if (seq <= player.lastQueuedSeq)
		{
			++_stats.inputsDroppedOldSeq;
			return false;
		}
		if (player.queueCount >= kInputQueueSize)
		{
			++_stats.inputsDroppedFull;
			return false;
		}
		player.queue[(player.queueHead + player.queueCount) % kInputQueueSize] = item;
		++player.queueCount;
		player.lastQueuedSeq = seq;
		return true;
	}

	void CRoom::OnInput(int64_t session, const wire::Input& input)
	{
		Player* player = Find(session);
		if (!player) return;
		++_stats.inputsReceived;
		// 탄 방향이 입력 번호로 정해지므로 번호를 건너뛰면 유리한 값을 고를 수 있다. 정상 클라는 1씩 늘리고 TCP 라 안 건너뛴다(D15)
		const bool skippedSeq = input.seq != player->lastReceivedSeq + 1;
		player->lastReceivedSeq = input.seq;
		if (!IsFiniteInput(input) || !IsViewInRange(input) || !IsViewAlphaInRange(input))
		{
			++_stats.inputsRejectedBadValue;
			return;
		}
		QueuedInput item;
		item.input = input;
		if (skippedSeq && (item.input.buttons & wire::BUTTON_FIRE))
		{
			// 이동은 그대로 쓰고 그 사격만 버린다
			item.input.buttons = (uint8_t)(item.input.buttons & ~wire::BUTTON_FIRE);
			++_stats.firesDroppedSeqGap;
		}
		item.place = wire::DebugPlace();
		item.isPlace = false;
		PushQueue(*player, item, input.seq);
	}

	void CRoom::OnDebugPlace(int64_t session, const wire::DebugPlace& place)
	{
		Player* player = Find(session);
		if (player) player->lastReceivedSeq = place.seq;		// 시험용 자리 옮기기도 입력과 같은 번호 줄을 쓴다(거절돼도 번호는 지나갔다)
		if (!_allowDebug)
		{
			++_stats.debugPlacesRejected;
			return;
		}
		if (!player) return;
		QueuedInput item;
		item.input = wire::Input();
		item.place = place;
		item.isPlace = true;
		PushQueue(*player, item, place.seq);
	}

	// ─── 경기 단계 ─────────────────────────────────────────
	void CRoom::EnterPhase(wire::EMatchPhase phase)
	{
		_phase = phase;
		++_stats.phaseChanges;
		switch (phase)
		{
		case wire::EMatchPhase::Waiting:
			break;
		case wire::EMatchPhase::Warmup:
			_phaseEndTick = _tick + SecondsToTicks(_rules.warmupSeconds);
			break;
		case wire::EMatchPhase::Playing:
		{
			// B_TeamDeathMatchScoring GameStarted — 캐릭터 수로 목표·시간을 정하고 모두 시작 자리로 돌린다
			const bool big = (int)_players.size() >= _rules.bigMatchPlayers;
			_targetScore = (uint16_t)(big ? _rules.bigMatchTargetScore : _rules.smallMatchTargetScore);
			_phaseEndTick = _tick + SecondsToTicks(big ? _rules.bigMatchSeconds : _rules.smallMatchSeconds);
			_teamScoreA = _teamScoreB = 0;
			ResetPads();	// 받침도 새로 생긴다 — 0-3 실행 기록 pads1(경기 시작 초기화가 큰 받침 대기를 끊었다). 사람을 다시 세우기 전에
			for (const std::unique_ptr<Player>& player : _players)
			{
				player->kills = player->deaths = player->assists = 0;	// 도움 기록(damagedBy)은 그대로 — AssistProcessor 는 죽을 때만 지운다
				SpawnPlayer(*player);
			}
			break;
		}
		case wire::EMatchPhase::PostGame:
			_phaseEndTick = _tick + SecondsToTicks(_rules.postGameSeconds);
			break;
		case wire::EMatchPhase::Finished:
			BuildResult();
			SendToAll(&_result.packet, _result.packetSize);
			break;
		}
		SendMatch();
	}

	void CRoom::TickPhase()
	{
		switch (_phase)
		{
		case wire::EMatchPhase::Waiting:
		case wire::EMatchPhase::Finished:
			return;
		case wire::EMatchPhase::Warmup:
			if (_tick >= _phaseEndTick) EnterPhase(wire::EMatchPhase::Playing);
			break;
		case wire::EMatchPhase::Playing:
			if (_tick >= _phaseEndTick)
			{
				// 시간 끝 — 같으면 무승부, 아니면 많은 쪽
				if (_teamScoreA == _teamScoreB) HandleVictory(wire::kTeamNone);
				else HandleVictory(_teamScoreA >= _teamScoreB ? wire::kTeamA : wire::kTeamB);
			}
			break;
		case wire::EMatchPhase::PostGame:
			if (_tick >= _phaseEndTick) EnterPhase(wire::EMatchPhase::Finished);
			break;
		}
		if (_phase != wire::EMatchPhase::Finished && _tick % wire::kTickRate == 0) SendMatch();
	}

	void CRoom::HandleVictory(uint8_t team)
	{
		if (_phase != wire::EMatchPhase::Playing) return;
		_winningTeam = team;
		EnterPhase(wire::EMatchPhase::PostGame);
	}

	void CRoom::SendMatch()
	{
		wire::Match match = wire::Match();
		match.h.size = (uint16_t)sizeof(match);
		match.h.type = (uint16_t)wire::Msg::S2C_MATCH;
		match.tick = _tick;
		match.secondsLeft = _phaseEndTick > _tick ? (float)(_phaseEndTick - _tick) / (float)wire::kTickRate : 0.f;
		match.targetScore = _targetScore;
		match.teamScoreA = _teamScoreA;
		match.teamScoreB = _teamScoreB;
		match.phase = (uint8_t)_phase;
		match.winningTeam = _winningTeam;
		SendToAll(&match, (int)sizeof(match));
	}

	void CRoom::BuildResult()
	{
		wire::Result& packet = _result.packet;
		packet = wire::Result();
		packet.tick = _tick;
		packet.targetScore = _targetScore;
		packet.teamScoreA = _teamScoreA;
		packet.teamScoreB = _teamScoreB;
		packet.winningTeam = _winningTeam;
		for (const std::unique_ptr<Player>& player : _players)
		{
			wire::PlayerResult& out = packet.players[packet.count++];
			out.id = player->id;
			out.kills = player->kills;
			out.deaths = player->deaths;
			out.assists = player->assists;
			out.team = player->team;
		}
		_result.packetSize = (int)(offsetof(wire::Result, players) + sizeof(wire::PlayerResult) * packet.count);
		packet.h.size = (uint16_t)_result.packetSize;
		packet.h.type = (uint16_t)wire::Msg::S2C_RESULT;
	}

	// ─── 시작 자리 ─────────────────────────────────────────
	// ALyraPlayerStart::GetLocationOccupancy 의 줄임 — 서버에는 플레이어끼리 충돌이 없어 캡슐이 겹치면 찬 것으로 본다
	bool CRoom::IsStartOccupied(const StartSlot& slot) const
	{
		const double radius = _params.capsuleRadius * 2.0;
		const double height = _params.capsuleHalfHeight * 2.0;
		for (const std::unique_ptr<Player>& player : _players)
		{
			if (player->dead) continue;
			const Vec3 d = player->move.location - slot.point.location;
			if (d.x * d.x + d.y * d.y < radius * radius && std::fabs(d.z) < height) return true;
		}
		return false;
	}

	// UTDM_PlayerSpawningManagmentComponent::OnChoosePlayerStart — 어느 적 하나에서든 가장 먼 자리. 찜 안 된 자리 우선.
	// 적이 없으면 ULyraPlayerSpawningManagerComponent::GetFirstRandomUnoccupiedPlayerStart
	int CRoom::ChooseStart(const Player& player)
	{
		int best = -1, fallback = -1;
		double bestDistance = 0, fallbackDistance = 0;
		for (const std::unique_ptr<Player>& other : _players)
		{
			if (other->team == player.team || other->dead) continue;
			for (size_t i = 0; i < _starts.size(); ++i)
			{
				const double distance = Length(_starts[i].point.location - other->move.location);
				if (_starts[i].claimedBy != 0)
				{
					if (fallback < 0 || distance > fallbackDistance)
					{
						fallback = (int)i;
						fallbackDistance = distance;
					}
				}
				else if (best < 0 || distance > bestDistance)
				{
					best = (int)i;
					bestDistance = distance;
				}
			}
		}
		if (best >= 0) return best;
		++_stats.spawnsFromFallback;
		if (fallback >= 0) return fallback;

		std::vector<int> empty, occupied;
		for (size_t i = 0; i < _starts.size(); ++i)
		{
			(IsStartOccupied(_starts[i]) ? occupied : empty).push_back((int)i);
		}
		const std::vector<int>& list = empty.empty() ? occupied : empty;
		const int pick = (int)(_spawnRng.NextFloat() * (float)list.size());
		return list[(size_t)std::min(pick, (int)list.size() - 1)];
	}

	void CRoom::SpawnPlayer(Player& player)
	{
		for (StartSlot& slot : _starts)
		{
			if (slot.claimedBy == player.id) slot.claimedBy = 0;
		}
		const int startIndex = ChooseStart(player);
		StartSlot& slot = _starts[(size_t)startIndex];
		if (slot.claimedBy == 0) slot.claimedBy = player.id;	// TryClaim

		player.dead = false;
		player.hp = (float)_rules.maxHealth;
		player.move = MoveState();
		player.move.location = slot.point.location;
		player.move.maxSpeed = _params.maxWalkSpeed;
		_moveSim.RefreshFloor(player.move);
		player.jump.Reset();
		player.ads.Reset();
		player.aimCamera.Reset();
		player.melee.Reset();		// 죽음이 능력을 끈다 — 새 몸은 꺼진 채
		player.meleePushId = 0;
		player.dash.Reset();		// 쿨다운도 — Lyra 쿨다운 효과는 1.5초라 부활 대기(respawnSeconds)보다 먼저 끝난다
		player.dash.RememberInput(Vec3(), slot.point.yawDegrees);	// 새 몸의 「앞 입력」 = 이동 입력 없음 · 놓인 요 — 첫 입력에 누르면 still_yaw0 · still_yaw90(D-0)과 같은 갈래
		player.yaw = slot.point.yawDegrees;
		player.pitch = 0;
		// 가방 — Lyra 는 죽은 몸이 치워질 때 비우고 새 몸에 권총만 준다(W-0 실측). 죽은 동안은 줍기 · 바꾸기가 없어 부활 때 비워도 보이는 것이 같다(제 판단)
		player.loadout.Reset(_rules.startAllWeapons, player.weapons);
		player.weapon = player.loadout.Held();
		if (_animReady)
		{
			// 부활하면 처음 무기(권총)를 든다 — 자세도 그 묶음으로 처음부터, 부활 몽타주를 튼다
			player.anim.Init(*_anim.clips, AnimBundleOf(player.weapon));
			player.animWeapon = player.weapon;
			DropAnimEvents(player);
			PushAnimEvent(player, EAnimEvent::Spawn, player.weapon);
		}
		player.immuneUntilTick = _tick + SecondsToTicks(_rules.spawnImmunitySeconds);
		player.moveLockInputs = SpawnMoveLockInputs(_rules);
		player.fireGate = CWeaponFireGate();
		player.starvedInRow = 0;
		++_stats.spawns;

		wire::Spawn spawn = wire::Spawn();
		spawn.h.size = (uint16_t)sizeof(spawn);
		spawn.h.type = (uint16_t)wire::Msg::S2C_SPAWN;
		spawn.tick = _tick;
		spawn.player = player.id;
		spawn.lastSeq = player.lastSeq;
		spawn.x = player.move.location.x;
		spawn.y = player.move.location.y;
		spawn.z = player.move.location.z;
		spawn.yaw = player.yaw;
		spawn.startIndex = (uint8_t)startIndex;
		SendToAll(&spawn, (int)sizeof(spawn));
		UpdatePadOverlaps(player);	// 새 몸이 받침 안에 서면 겹침 시작이다
	}

	void CRoom::TickRespawns()
	{
		for (const std::unique_ptr<Player>& player : _players)
		{
			if (player->dead && _tick >= player->respawnTick) SpawnPlayer(*player);
		}
	}

	// ALyraPlayerStart::CheckUnclaimed — 1초마다. 찜한 사람이 살아 있고 자리를 떠났으면 푼다
	void CRoom::TickClaims()
	{
		if (_tick % wire::kTickRate != 0) return;
		for (StartSlot& slot : _starts)
		{
			if (slot.claimedBy == 0) continue;
			const Player* claimer = FindById(slot.claimedBy);
			if (!claimer)
			{
				slot.claimedBy = 0;
				continue;
			}
			if (!claimer->dead && !IsStartOccupied(slot)) slot.claimedBy = 0;
		}
	}

	bool CRoom::IsImmune(const Player& player) const
	{
		// GE_PregameLobby · GE_SpawnIn · GE_DamageImmunity_FromGameMode
		if (_phase != wire::EMatchPhase::Playing) return true;
		return _tick < player.immuneUntilTick;
	}

	// ─── 체력 받침(컨텐츠 B, ALyraWeaponSpawner + B_AbilitySpawner) ───────────
	// 두 캡슐 모두 서 있다(회전 없음) — 축 선분 사이 거리가 반지름 합보다 작으면 겹침. 받침 캡슐은 반높이 = 반지름(80)이라 공이다.
	// 경계(딱 닿음)는 엔진 겹침 판정과 다를 수 있다 — 엔진 쪽 여유 값은 옮기지 않았다(원본 확인 안 함)
	bool CRoom::OverlapsPad(const MoveState& move, const PickupPad& pad) const
	{
		const double radius = _params.capsuleRadius;
		const double segment = std::max(0.0, (double)_moveSim.HalfHeightOf(move) - radius);
		const double padSegment = std::max(0.0, pad.halfHeight - pad.radius);
		const double dx = move.location.x - pad.location.x;
		const double dy = move.location.y - pad.location.y;
		const double gapZ = std::max(0.0, std::fabs(move.location.z - pad.location.z) - segment - padSegment);
		const double reach = radius + pad.radius;
		return dx * dx + dy * dy + gapZ * gapZ < reach * reach;
	}

	// OnOverlapBegin — 겹침이 새로 시작된 받침만 집기를 시도한다. 이미 서 있던 사람은 다시 켜진 뒤 다시 검사(TickPads)에서만 본다
	void CRoom::UpdatePadOverlaps(Player& player)
	{
		for (size_t i = 0; i < player.padOverlap.size(); ++i)
		{
			const bool now = !player.dead && OverlapsPad(player.move, (*_pickups.pads)[i]);
			const bool began = now && player.padOverlap[i] == 0;
			player.padOverlap[i] = now ? 1 : 0;
			if (began) TryPickup(i, player, false);
		}
	}

	// AttemptPickUpWeapon + B_AbilitySpawner GiveWeapon — 켜져 있고 체력이 최대보다 낮을 때만(Less_DoubleDouble) 회복하고 꺼진다.
	// 대기 틱(실측 cprobe_pads2.log, 60프레임/초): 집은 프레임 → 다시 켜짐 = 큰 것 1499 · 작은 것 599 = 정한 시간의 틱 − 1. 이동 중(fromTimer = false)
	// 건 타이머는 이 틱을 한 칸으로 센다 — 엔진 타이머가 이 프레임 시간을 이 프레임 끝에 먼저 빼서로 짐작(엔진 소스 확인 안 함).
	// 다시 검사 안에서(fromTimer = true) 건 타이머는 다음 틱부터 센다(다시 검사 +15 = 0.25초 × 60 이 − 1 없이 맞은 것과 같은 까닭)
	bool CRoom::TryPickup(size_t padIndex, Player& player, bool fromTimer)
	{
		const PadState& state = _padStates[padIndex];
		if (!state.available || player.dead) return false;
		const PickupPad& pad = (*_pickups.pads)[padIndex];
		if (pad.kind != EPadKind::Heal) return TryWeaponPickup(padIndex, player, fromTimer);
		const float maxHealth = (float)_rules.maxHealth;
		if (!(player.hp < maxHealth))
		{
			++_stats.pickupsFullHealth;
			if (_allowDebug) std::printf("PADEV tick=%u room=%u pad=%zu kind=full player=%u hp=%.1f\n", _tick, _roomId, padIndex, player.id, player.hp);
			return false;
		}
		const float before = player.hp;
		player.hp = std::min(player.hp + (float)pad.heal, maxHealth);	// LyraHealthSet — 회복도 최대에서 잘린다
		TakePad(padIndex, fromTimer);
		++_stats.pickups;
		if (fromTimer) ++_stats.pickupsFromRecheck;
		if (_allowDebug) std::printf("PADEV tick=%u room=%u pad=%zu kind=%s player=%u hp=%.1f>%.1f ready=%u\n", _tick, _roomId, padIndex, fromTimer ? "takeRecheck" : "take", player.id, before, player.hp, state.readyTick);
		SendPad(padIndex, fromTimer ? wire::PICKUP_TAKE_RECHECK : wire::PICKUP_TAKE, &player, player.hp - before, nullptr);
		return true;
	}

	void CRoom::TakePad(size_t padIndex, bool fromTimer)
	{
		PadState& state = _padStates[padIndex];
		state.available = false;
		state.readyTick = _tick + SecondsToTicks((*_pickups.pads)[padIndex].respawnSeconds) - (fromTimer ? 0u : 1u);
	}

	// B_WeaponSpawner GiveWeapon(weapon-pickup-plan.md 「Lyra」 표 · W-0 실측) — 경기 끝 단계면 실패, 그 밖은 가방 Give 결과대로.
	// 실패(예비 가득 · 경기 끝 · 빈 칸 없음)는 받침이 안 꺼지고 소식도 안 보낸다(체력 가득과 같은 길). 무기를 바꿨으면 바꾸기 몽타주를 튼다(키로 바꿀 때와 같은 사건)
	bool CRoom::TryWeaponPickup(size_t padIndex, Player& player, bool fromTimer)
	{
		const PickupPad& pad = (*_pickups.pads)[padIndex];
		EWeapon weapon = EWeapon::Pistol;
		switch (pad.kind)
		{
		case EPadKind::Pistol: weapon = EWeapon::Pistol; break;
		case EPadKind::Rifle: weapon = EWeapon::Rifle; break;
		case EPadKind::Shotgun: weapon = EWeapon::Shotgun; break;
		case EPadKind::Heal: return false;	// 부르는 쪽이 갈랐다
		}
		if (_phase == wire::EMatchPhase::PostGame)
		{
			++_stats.weaponPickupsPostGame;
			if (_allowDebug) std::printf("PADEV tick=%u room=%u pad=%zu kind=full give=postGame player=%u weapon=%u\n", _tick, _roomId, padIndex, player.id, (unsigned)weapon);
			return false;
		}
		const uint8_t heldBefore = player.weapon;
		const EGiveResult result = player.loadout.Give(weapon, player.weapons);
		switch (result)
		{
		case EGiveResult::NewEquipped: ++_stats.weaponPickupsNew; break;
		case EGiveResult::NewNotEquipped: ++_stats.weaponPickupsNoSwitch; break;
		case EGiveResult::Ammo: ++_stats.weaponPickupsAmmo; break;
		case EGiveResult::Full: ++_stats.weaponPickupsFull; break;
		case EGiveResult::NoSlot: ++_stats.weaponPickupsNoSlot; break;
		}
		const CWeaponCycle& cycle = player.weapons[(size_t)weapon];
		if (!IsGiveTaken(result))
		{
			if (_allowDebug) std::printf("PADEV tick=%u room=%u pad=%zu kind=full give=%s player=%u weapon=%u mag=%d spare=%d\n", _tick, _roomId, padIndex, GiveResultName(result), player.id, (unsigned)weapon, cycle.GetMagazine(), cycle.GetSpare());
			return false;
		}
		player.weapon = player.loadout.Held();
		if (player.weapon != heldBefore)
		{
			++_stats.weaponChanges;
			PushAnimEvent(player, EAnimEvent::Swap, player.weapon);
		}
		TakePad(padIndex, fromTimer);
		if (fromTimer) ++_stats.weaponPickupsFromRecheck;
		if (_allowDebug)
		{
			std::printf("PADEV tick=%u room=%u pad=%zu kind=%s give=%s player=%u weapon=%u mag=%d spare=%d held=%u slots=%d,%d,%d ready=%u\n", _tick, _roomId, padIndex,
				fromTimer ? "takeRecheck" : "take", GiveResultName(result), player.id, (unsigned)weapon, cycle.GetMagazine(), cycle.GetSpare(), (unsigned)player.weapon,
				SlotNumber(player.loadout.SlotWeapon(0)), SlotNumber(player.loadout.SlotWeapon(1)), SlotNumber(player.loadout.SlotWeapon(2)), _padStates[padIndex].readyTick);
		}
		SendPad(padIndex, fromTimer ? wire::PICKUP_TAKE_RECHECK : wire::PICKUP_TAKE, &player, 0.f, nullptr, (uint8_t)result, (uint8_t)weapon);
		return true;
	}

	// 받침 타이머 — 이동을 다 돌린 뒤. ResetCoolDown(켜짐 · 다시 검사 타이머) · CheckForExistingOverlaps(겹친 사람마다 집기 시도, 하나가 집으면 꺼져 나머지는 안 봄)
	void CRoom::TickPads()
	{
		for (size_t i = 0; i < _padStates.size(); ++i)
		{
			PadState& state = _padStates[i];
			if (!state.available && _tick >= state.readyTick)
			{
				state.available = true;
				const double recheck = (*_pickups.pads)[i].recheckSeconds;
				state.recheckPending = recheck > 0;	// 엔진 SetTimer 는 0 이하 간격이면 타이머를 지운다
				state.recheckTick = _tick + SecondsToTicks(recheck);
				++_stats.padRespawns;
				if (_allowDebug) std::printf("PADEV tick=%u room=%u pad=%zu kind=ready recheck=%u\n", _tick, _roomId, i, state.recheckTick);
				SendPad(i, wire::PICKUP_READY, nullptr, 0.f, nullptr);
				continue;
			}
			if (state.recheckPending && _tick >= state.recheckTick)
			{
				state.recheckPending = false;
				++_stats.padRechecks;
				if (_allowDebug) std::printf("PADEV tick=%u room=%u pad=%zu kind=recheck available=%d\n", _tick, _roomId, i, state.available ? 1 : 0);
				for (const std::unique_ptr<Player>& player : _players)
				{
					if (!state.available) break;
					if (player->padOverlap[i] != 0) TryPickup(i, *player, true);
				}
			}
		}
	}

	void CRoom::ResetPads()
	{
		for (PadState& state : _padStates) state = PadState();
		for (const std::unique_ptr<Player>& player : _players) std::fill(player->padOverlap.begin(), player->padOverlap.end(), (uint8_t)0);
		for (size_t i = 0; i < PadCount(); ++i) SendPad(i, wire::PICKUP_SYNC, nullptr, 0.f, nullptr);	// 경기 시작 — 꺼져 있던 받침도 켜진다
	}

	// S2C_PICKUP — only 가 있으면 그 사람에게만(들어올 때 현황), 없으면 방 전원
	void CRoom::SendPad(size_t padIndex, wire::EPickupReason reason, const Player* picker, float heal, const Player* only, uint8_t give, uint8_t weapon)
	{
		const PickupPad& pad = (*_pickups.pads)[padIndex];
		const PadState& state = _padStates[padIndex];
		wire::Pickup msg = wire::Pickup();
		msg.h.size = (uint16_t)sizeof(msg);
		msg.h.type = (uint16_t)wire::Msg::S2C_PICKUP;
		msg.tick = _tick;
		msg.readyTick = state.available ? 0 : state.readyTick;
		msg.cooldownTicks = SecondsToTicks(pad.respawnSeconds);
		msg.picker = picker ? picker->id : 0;
		msg.heal = heal;
		msg.hpAfter = picker ? picker->hp : 0.f;
		msg.x = (float)pad.location.x;
		msg.y = (float)pad.location.y;
		msg.z = (float)pad.location.z;
		msg.pad = (uint8_t)padIndex;
		msg.padCount = (uint8_t)PadCount();
		msg.available = state.available ? 1 : 0;
		msg.reason = reason;
		msg.kind = (uint8_t)pad.kind;
		msg.give = give;
		msg.seq = picker ? picker->lastSeq : 0;
		if (picker && give != wire::kPickupGiveNone && weapon < (uint8_t)EWeapon::Count)
		{
			const CWeaponCycle& cycle = picker->weapons[weapon];
			msg.weapon = weapon;
			msg.slot = (uint8_t)std::max(0, picker->loadout.SlotOf(weapon));
			msg.magazine = (int16_t)cycle.GetMagazine();
			msg.spare = (int16_t)cycle.GetSpare();
		}
		if (_allowDebug && reason == wire::PICKUP_SYNC) std::printf("PADEV tick=%u room=%u pad=%zu kind=sync to=%u available=%d ready=%u\n", _tick, _roomId, padIndex, only ? only->id : 0u, msg.available, msg.readyTick);
		if (only) _sender.SendToSession(only->session, &msg, (int)sizeof(msg));
		else SendToAll(&msg, (int)sizeof(msg));
	}

	// ─── 입력 ──────────────────────────────────────────────
	// 시험용 — 사망·부활 대기·무적·이동 막음을 모두 지운다
	void CRoom::ApplyPlace(Player& player, const wire::DebugPlace& place)
	{
		++_stats.debugPlaces;
		player.move = MoveState();
		player.move.location = Vec3(place.x, place.y, place.z);
		player.move.velocity = Vec3(place.vx, place.vy, place.vz);
		player.move.mode = place.falling ? EMoveMode::Falling : EMoveMode::Walking;
		player.move.maxSpeed = _params.maxWalkSpeed;
		_moveSim.RefreshFloor(player.move);
		player.jump.Reset();
		player.meleePushId = 0;		// 힘 묶음도 새로 만들었다(번호도 처음부터) — 앞 근접의 밀기 번호는 없는 힘을 가리킨다
		player.hp = place.hp;
		player.dead = false;
		player.immuneUntilTick = 0;
		player.moveLockInputs = 0;
		player.lastSeq = place.seq;
		UpdatePadOverlaps(player);	// 순간이동(SetActorLocation)도 겹침을 다시 본다 — 0-3 pads2 는 이렇게 올라서서 그 프레임에 집었다
		if (_animReady)
		{
			// 순간이동 + 멈춤 = 판 시작과 같다 — 자세를 든 무기 묶음으로 처음부터(경로 봇이 무기를 든 뒤 옮기면 서기 시계가 옮긴 틱에서 시작).
			// 이 틱에 앞서 쌓인 사건(무기 들기의 꺼내기 등)도 버린다 — 몽타주 없는 서기에서 시작
			player.anim.Init(*_anim.clips, AnimBundleOf(player.weapon));
			player.animWeapon = player.weapon;
			DropAnimEvents(player);
		}
		if ((place.flags & wire::DEBUG_PLACE_SET_AMMO) && place.ammoWeapon < (uint8_t)EWeapon::Count)
		{
			++_stats.debugAmmoSets;
			player.weapons[place.ammoWeapon].SetAmmoForCheck(place.magazine, place.spare);
		}
		if (place.flags & wire::DEBUG_PLACE_KILL)
		{
			++_stats.debugKills;
			KillPlayer(player, nullptr, player.weapon, false);
		}
	}

	void CRoom::LogLoadouts()
	{
		for (const std::unique_ptr<Player>& player : _players)
		{
			char text[160];
			int at = std::snprintf(text, sizeof(text), "slots=[");
			for (size_t slot = 0; slot < kQuickSlotCount; ++slot)
			{
				const uint8_t weapon = player->loadout.SlotWeapon(slot);
				const char* gap = slot + 1 < kQuickSlotCount ? " " : "";
				if (weapon == kNoWeapon) at += std::snprintf(text + at, sizeof(text) - at, "-%s", gap);
				else at += std::snprintf(text + at, sizeof(text) - at, "%s:%d/%d%s", GetWeaponDef((EWeapon)weapon).name, player->weapons[weapon].GetMagazine(), player->weapons[weapon].GetSpare(), gap);
			}
			std::snprintf(text + at, sizeof(text) - at, "] active=%u", (unsigned)player->loadout.ActiveSlot());
			if (player->loggedLoadout == text) continue;
			player->loggedLoadout = text;
			std::printf("INVEV tick=%u room=%u player=%u dead=%d %s\n", _tick, _roomId, player->id, player->dead ? 1 : 0, text);
		}
	}

	void CRoom::ApplyInput(Player& player, const wire::Input& input, const bool serverMade)
	{
		player.lastSeq = input.seq;
		if (player.dead)
		{
			++_stats.inputsWhileDead;
			if (!serverMade && (input.buttons & wire::BUTTON_DASH) != 0) CountDash(player.dash.TryActivate(input.pitch, input.yaw, true, _phase == wire::EMatchPhase::PostGame).result);
			return;
		}
		++_stats.inputsApplied;
		// 서버가 만든 빈 입력(serverMade — 입력이 늦어 TickPlayerInputs 가 채움)은 걸음 물리만 굴린다. 입력 수로 세는 시계(발사 문 · 근접 · 조준 받기 ·
		// 무기 순환 · 조준 카메라)는 클라 예측이 모르는 입력에 돌리면 서버만 앞서 간다(D84 걸음 잠금 · D87 — 쉬기 봇 판 서버가 한 발 더 쏨, 탄창 6 · 예측 7).
		// 언리얼 CharacterMovement 의 ForcePositionUpdate 도 이동만 굴린다
		if (!serverMade) player.fireGate.NextInput();

		// 무기 바꾸기 — 앞 입력과 번호가 달라졌을 때만(Loadout.h). 빈 입력은 안 본다 — 빈 입력의 번호(지금 든 무기)가 「앞 입력 번호」가 되면
		// 줍기로 바뀐 무기 뒤에 오는 클라의 옛 번호가 「달라짐」이 되어 무기가 되돌아간다
		switch (serverMade ? ESwitchResult::Same : player.loadout.OnInputWeapon(input.weapon, player.weapons))
		{
		case ESwitchResult::Invalid:
			++_stats.weaponIdRejected;
			break;
		case ESwitchResult::NotOwned:
			++_stats.weaponSwitchRejected;
			if (_allowDebug) std::printf("INVEV tick=%u room=%u player=%u reject weapon=%u\n", _tick, _roomId, player.id, (unsigned)input.weapon);	// 어느 입력이 거부됐나 — 수만으로는 출처를 못 가른다
			break;
		case ESwitchResult::Switched:
			player.weapon = player.loadout.Held();
			++_stats.weaponChanges;		// 자세 묶음은 이 틱의 UpdateAnimStates 가 따라 바꾼다
			PushAnimEvent(player, EAnimEvent::Swap, player.weapon);
			break;
		case ESwitchResult::Same:
		case ESwitchResult::AlreadyHeld:
			break;
		}

		// 대시(D-2) — 무기 바꾸기 뒤 · 점프 앞. 켜면 그 자리에서 근접(밀기 힘까지) · 점프를 끊고 일어서기 · 힘을 넣는다(StartDash).
		// 조준 · 사격 · 재장전의 막기 · 끊기는 각자의 자리(ads.Apply · 무기 순환)가 대시 상태를 본다. 근접 입력 수도 여기서 센다 — 끊을지를 이번 입력 값으로 본다
		if (!serverMade)	// 빈 입력은 대시 · 근접 시계를 안 센다(D87)
		{
			player.melee.NextInput();
			player.dash.NextInput();
			if ((input.buttons & wire::BUTTON_DASH) != 0)
			{
				const DashStart dash = player.dash.TryActivate(input.pitch, input.yaw, false, _phase == wire::EMatchPhase::PostGame);
				CountDash(dash.result);
				if (dash.result == EDashResult::Started) StartDash(player, dash, input.seq);
			}
		}

		const bool jump = (input.buttons & wire::BUTTON_JUMP) != 0;
		switch (player.jump.Apply(jump, player.dash.BlocksPresses()))
		{
		case EJumpStep::Press:
			_moveSim.RequestUnCrouch(player.move);		// Lyra 점프 능력은 점프 전에 일어서기를 부른다(LyraGameplayAbility_Jump.cpp:55) — 이동 대조(MoveCheck)와 같은 차례
			_moveSim.PressJump(player.move);
			break;
		case EJumpStep::Release:
			_moveSim.ReleaseJump(player.move);
			break;
		case EJumpStep::Blocked:
			++_stats.dashJumpsBlocked;
			break;
		case EJumpStep::None:
			break;
		}
		// 앉기 키 — 바람만 바꾸고 실제로 앉고 서는 것은 이 입력의 이동 앞뒤 갱신이다(D-2). 점프 뒤에 넣는다(MoveCheck 와 같은 차례).
		// 이동 막음 · 경기 끝에도 받는다 — Lyra 앉기는 능력이 아니라 키에 바로 묶인 함수라(LyraHeroComponent.cpp:451) 이동 금지 태그 · 능력 입력 막음을 안 거친다
		if ((input.buttons & wire::BUTTON_CROUCH) != 0)
		{
			_moveSim.ToggleCrouch(player.move);
			++_stats.crouchPresses;
		}
		// 근접(E-3) — 무기 교체 뒤(켜는 무기의 간격 · 몽타주) · 걸음 앞(밀기가 이 걸음부터). 예측(CSelfPrediction::StepMelee)도 같은 자리
		const bool meleeStarted = serverMade ? false : TryMelee(player, input);	// 빈 입력은 근접 입력 수(melee.NextInput — 위 대시 자리)도 안 센다(D87)

		Vec3 moveInput(input.moveX, input.moveY, 0);
		const double moveLength = Length(moveInput);
		if (moveLength > 1.0)
		{
			if (moveLength > 1.001) ++_stats.movesOverLength;
			moveInput = moveInput * (1.0 / moveLength);
		}
		if (player.moveLockInputs > 0)
		{
			// GA_SpawnEffect 의 이동 입력 막음 — 점프·사격은 안 막힌다
			// 서버가 만든 빈 입력은 세지 않는다 — 클라가 모르는 입력이라 세면 서버만 먼저 풀린다(D84 · 쉬기 봇 판 막음 어긋남 500 → 0)
			if (!serverMade) --player.moveLockInputs;
			moveInput = Vec3();
		}
		player.moveX = (int8_t)std::lround(moveInput.x * 127.0);
		player.moveY = (int8_t)std::lround(moveInput.y * 127.0);
		// 조준 — 이 입력의 버튼이 이 입력의 걸음부터 먹는다(D61). 이동 막음 · 경기 끝에도 버튼을 따른다(예측이 단계를 몰라 맞춰 둔다, D61)
		if (!serverMade)	// 빈 입력은 조준 상태를 그대로 둔다 — Apply 는 첫 입력에 granted 를 켜므로 부활 직후 빈 입력이 서버만 먼저 켠다(D87)
		{
			const bool adsHeld = (input.buttons & wire::BUTTON_ADS) != 0;
			const bool adsWasActive = player.ads.active;
			const bool adsWasHeld = player.ads.held;
			player.ads.Apply(adsHeld, player.dash.BlocksPresses());		// 대시가 켜지는 입력에 끄고, 막힌 누름은 떼었다 다시 눌러야 켜진다(D-2)
			if (player.dash.JustStarted() && adsWasActive) ++_stats.dashAdsCancels;
			else if (adsHeld && !adsWasHeld && player.dash.BlocksPresses()) ++_stats.dashAdsBlocked;
			if (player.ads.active) ++_stats.adsInputs;
		}
		player.move.maxSpeed = AdsWalkSpeed(_params.maxWalkSpeed, player.ads.active);
		const Vec3 locationBeforeMove = player.move.location;
		// 사람끼리 막기(F-2) — 이 입력을 굴리는 순간의 남 자리(이 틱에 먼저 굴린 사람은 굴린 뒤 자리). Lyra 서버도 받은 이동을 하나씩 바로 굴린다
		uint64_t pawnHitsBefore = 0, pawnStartBefore = 0, pawnOddBefore = 0, pawnTouchBefore = 0;
		if (_rules.pawnBlock)
		{
			const MoveCounters& countBefore = _moveSim.GetCounters();
			pawnHitsBefore = countBefore.pawnHits;
			pawnStartBefore = countBefore.pawnStartPenetrating;
			pawnOddBefore = countBefore.pawnOddSimplex;
			pawnTouchBefore = countBefore.pawnTouchStart;
			_pawnScratch.clear();
			for (const std::unique_ptr<Player>& other : _players)
			{
				if (other.get() == &player || other->dead) continue;
				PawnCapsule capsule;
				capsule.center = other->move.location;
				capsule.radius = _params.capsuleRadius;
				capsule.halfHeight = _moveSim.HalfHeightOf(other->move);
				capsule.yaw = other->yaw;
				_pawnScratch.push_back(capsule);
			}
			_moveSim.SetPawns(_pawnScratch.data(), _pawnScratch.size());
			++_stats.pawnBlockMoves;
			_stats.pawnBlockOthers += _pawnScratch.size();
		}
		const uint64_t liftoffsBefore = _moveSim.GetCounters().rootForceLiftoffs;
		_moveSim.Tick(player.move, moveInput, kTickSeconds);
		_stats.dashLiftoffs += _moveSim.GetCounters().rootForceLiftoffs - liftoffsBefore;	// 위아래를 더하는 힘은 대시뿐이다(근접 밀기는 위아래 무시)
		player.dash.RememberInput(moveInput, input.yaw);	// 다음 입력의 「앞 입력」 — 이 걸음이 가져간 이동(막음 뒤) · 이 입력의 요. 빈 입력도 걸음을 굴렸으니 넣는다(Dash.h)
		if (_rules.pawnBlock)
		{
			_moveSim.SetPawns(nullptr, 0);
			const MoveCounters& countAfter = _moveSim.GetCounters();
			_stats.pawnHits += countAfter.pawnHits - pawnHitsBefore;
			_stats.pawnStartPenetrating += countAfter.pawnStartPenetrating - pawnStartBefore;
			_stats.pawnOddSimplex += countAfter.pawnOddSimplex - pawnOddBefore;
			_stats.pawnTouchStart += countAfter.pawnTouchStart - pawnTouchBefore;
		}
		if (player.move.crouched) ++_stats.crouchedInputs;
		UpdatePadOverlaps(player);	// 이동이 끝난 자리로 — Lyra 는 움직이는 중 겹침이 시작되면 그 이동 안에서 집는다

		player.yaw = input.yaw;
		player.pitch = input.pitch;

		if (player.move.location.z < _rules.fallKillZ)
		{
			++_stats.falls;
			KillPlayer(player, nullptr, player.weapon, false);
			return;
		}
		if (serverMade) return;		// 빈 입력은 여기까지 — 무기 순환 · 조준 카메라는 안 돌린다(D87)

		WeaponMotion motion;
		motion.speed = (float)Length(player.move.velocity);
		motion.falling = player.move.mode == EMoveMode::Falling;
		motion.crouching = player.move.crouched;		// 이동 뒤 앉음(IsCrouching) — 속도 · 낙하와 같은 이동 뒤 값. Lyra 무기 기록 weaponrec_crouch1 과 대조해 같다(11-18, 한 프레임 앞 값이면 배율이 0.03~0.05 어긋난다)
		motion.aimingAlpha = player.aimCamera.GetAimingAlpha();		// 앞 입력의 카메라 갱신 결과 — Lyra 무기 틱은 카메라 갱신보다 먼저 돈다(C-1 실측)
		WeaponInput weaponInput;
		weaponInput.fireHeld = (input.buttons & wire::BUTTON_FIRE) != 0;
		weaponInput.reloadPressed = (input.buttons & wire::BUTTON_RELOAD) != 0;
		weaponInput.meleeStarted = meleeStarted;
		if (_phase == wire::EMatchPhase::PostGame)
		{
			if (weaponInput.fireHeld) ++_stats.firesBlockedPostGame;
			weaponInput = WeaponInput();	// GE_BlockAbilityInput
		}
		// 근접이 켜져 있으면 발사 · 재장전 입력 · 자동 재장전이 막힌다(근접의 막기 태그). 발사는 간격 검사 · 빈 탄 집계 앞에서 뺀다 — 정직한 누름이 거절로 안 세지게
		weaponInput.meleeActive = player.melee.IsActive();
		if (weaponInput.fireHeld && weaponInput.meleeActive)
		{
			++_stats.meleeFiresBlocked;
			weaponInput.fireHeld = false;
		}
		// 대시(D-2) — 앞 입력부터 켜져 있으면 쥔 사격을 뺀다(근접과 같은 까닭). 켜는 입력의 쥔 사격은 무기 순환이 먼저 쏘고 끊는다(Dash.h BlocksHeldFire)
		weaponInput.dashActive = player.dash.BlocksHeldFire();
		weaponInput.dashStarted = player.dash.JustStarted();
		if (weaponInput.fireHeld && weaponInput.dashActive)
		{
			++_stats.dashFiresBlocked;
			weaponInput.fireHeld = false;
		}
		if (weaponInput.reloadPressed && player.dash.BlocksPresses()) ++_stats.dashReloadsBlocked;	// 순환이 막는다 — 여기선 세기만
		CWeaponCycle& cycle = player.weapons[player.weapon];
		if (weaponInput.fireHeld && player.fireGate.IsBlocked((EWeapon)player.weapon, cycle.IsFireActive()))
		{
			++_stats.firesRejectedSwap;
			weaponInput.fireHeld = false;
		}
		const WeaponEvents events = cycle.Tick(kTickSeconds, weaponInput, motion);
		if (events.reloadCancelledByMelee) ++_stats.meleeReloadCancels;
		if (events.reloadCancelledByDash) ++_stats.dashReloadCancels;
		if (events.fireCancelledByDash) ++_stats.dashFireCancels;
		player.aimCamera.Tick(player.ads.active, kTickSeconds);		// 이 입력의 조준으로 카메라를 굴린다 — 다음 입력의 퍼짐이 읽는다
		if (weaponInput.fireHeld && !events.fired)
		{
			if (cycle.GetMagazine() == 0) ++_stats.firesEmpty;
			else ++_stats.firesTooSoon;
		}
		if (events.fired)
		{
			if (motion.aimingAlpha > 0.f) ++_stats.shotsAimed;
			if (motion.crouching) ++_stats.shotsCrouched;
			PushAnimEvent(player, EAnimEvent::Fire, player.weapon);
			player.fireGate.OnFired((EWeapon)player.weapon, cycle.GetDef(), kTickSeconds);
			PendingShot shot;
			shot.shooter = player.id;
			shot.weapon = player.weapon;
			shot.start = ResolveAimStart(player, locationBeforeMove, Vec3(input.aimX, input.aimY, input.aimZ));
			shot.aimDir = AimDirection(input.yaw, input.pitch);
			shot.spreadAngle = events.spreadAngle;
			shot.spreadMultiplier = events.spreadMultiplier;
			shot.viewTick = input.viewTick;
			shot.viewAlpha = input.viewAlpha;
			shot.seq = input.seq;
			_pendingShots.push_back(shot);
		}
		if (events.reloadStarted) PushAnimEvent(player, EAnimEvent::Reload, player.weapon);		// 손으로 · 탄창이 비어 스스로 둘 다. 무기 순환 차례(쏘기 → 재장전) 그대로
	}

	// 대시 누름 결과 — 갈래마다 따로 센다(합 = dashPresses)
	void CRoom::CountDash(EDashResult result)
	{
		switch (result)
		{
		case EDashResult::NotPressed: return;		// TryActivate 는 안 낸다 — 누른 입력에만 부른다
		case EDashResult::Started: ++_stats.dashStarted; break;
		case EDashResult::NoDirection: ++_stats.dashNoDirection; break;
		case EDashResult::WhileActive: ++_stats.dashWhileActive; break;
		case EDashResult::OnCooldown: ++_stats.dashOnCooldown; break;
		case EDashResult::Dead: ++_stats.dashDead; break;
		case EDashResult::PostGame: ++_stats.dashPostGame; break;
		}
		++_stats.dashPresses;
	}

	// 대시가 켜진 입력 — 끊기(Ability.Type.Action) → 일어서기 → 힘. DashCheck 이동 대조(D-1)와 같은 차례: 근접 밀기 지우기 → 일어서기 → 대시 힘.
	// 소식(S2C_DASH)은 입력 처리 안에서 보내 이 틱 상태보다 먼저 간다 — 켠 사람 예측은 상태의 확인(AckDash)보다 소식을 먼저 받아야 「서버는 안 켰다」로 안 읽는다
	void CRoom::StartDash(Player& player, const DashStart& dash, uint32_t seq)
	{
		if (player.melee.Cut())		// 근접 작업이 끝나며 밀기를 지운다(OnDestroy → RemoveRootMotionSourceByID). 피해는 켜진 입력에 이미 들어갔다(D-0 melee_dash 100)
		{
			++_stats.dashMeleeCancels;
			if (player.meleePushId != 0)
			{
				_moveSim.RemoveRootForce(player.move, player.meleePushId);
				++_stats.dashMeleePushRemoved;
			}
		}
		player.meleePushId = 0;
		if (player.jump.Cut())		// 점프 능력이 끝나며 StopJumping
		{
			_moveSim.ReleaseJump(player.move);
			++_stats.dashJumpCancels;
		}
		// 앉아 있으면 일어선다(그래프 bIsCrouched → UnCrouch). 같은 입력의 앉기 키는 이 뒤에 받아 앉기가 이긴다(D-0 crouch_same)
		if (player.move.crouched)
		{
			_moveSim.RequestUnCrouch(player.move);
			++_stats.dashUncrouches;
		}
		_moveSim.ApplyRootForce(player.move, dash.force, kDashForceDuration, DashForceSettings());
		++_stats.dashMontage[(size_t)dash.dir];
		PushAnimEvent(player, (EAnimEvent)((uint8_t)EAnimEvent::DashForward + (uint8_t)dash.dir), player.weapon);

		wire::Dash msg = wire::Dash();
		msg.h.size = (uint16_t)sizeof(msg);
		msg.h.type = (uint16_t)wire::Msg::S2C_DASH;
		msg.tick = _tick;
		msg.player = player.id;
		msg.seq = seq;
		msg.forceX = dash.force.x;
		msg.forceY = dash.force.y;
		msg.forceZ = dash.force.z;
		msg.dir = (uint8_t)dash.dir;
		SendToAll(&msg, (int)sizeof(msg));
		++_stats.dashMsgs;
	}

	// 근접(GA_Melee, 컨텐츠 E-3) — 켜지는 입력에 판정 · 피해 · 밀기 · 몽타주를 다 한다. 걸음 앞에서 불러 밀기가 이 입력 걸음부터 먹는다(E-2-1 · D73).
	// 손 · 몸 앞 = 앞 프레임(능력은 그 프레임 자세 평가 전에 켜진다, e1g1). 상대 몸 = 사격과 같은 되감기(D8 · D30) —
	// 사격은 틱 끝(기록 뒤)에 판정하지만 근접은 입력 처리 안이라 이 틱 기록이 아직 없다. 마지막으로 적은 틱(_tick − 1)이 「지금」이다(D74).
	// 반환 = 이 입력에 근접 능력이 켜졌나
	bool CRoom::TryMelee(Player& player, const wire::Input& input)
	{
		if ((input.buttons & wire::BUTTON_MELEE) == 0) return false;
		++_stats.meleePresses;
		if (_phase == wire::EMatchPhase::PostGame)
		{
			++_stats.meleeBlockedPostGame;		// GE_BlockAbilityInput — 쏘기와 같다
			return false;
		}
		// 대시가 켜져 있으면(켠 입력 포함) 막힌다 — 같은 프레임에 근접과 대시를 누르면 Lyra 는 둘 다 누름 능력이라 그 프레임에 눌린 차례대로 켠다
		// (LyraAbilitySystemComponent.cpp:250-283 InputPressedSpecHandles). 입력 하나에는 그 차례가 없어 대시 먼저로 둔다(제 판단 · 기록 없음 — 근접이 먼저면 피해가 들어간 뒤 끊긴다)
		if (player.dash.BlocksPresses())
		{
			++_stats.dashMeleeBlocked;
			return false;
		}
		if (!player.melee.TryActivate(GetWeaponDef((EWeapon)player.weapon), kTickSeconds))
		{
			++_stats.meleeWhileActive;
			return false;
		}
		++_stats.meleeStarts;
		player.meleePushId = 0;		// 맞아야 밀기를 넣는다 — 앞 근접의 번호를 안 남긴다
		PushAnimEvent(player, EAnimEvent::Melee, player.weapon);		// 공격자 몽타주는 맞음 · 같은 팀 · 벽 · 빗나감 모두 돈다(0-3)

		const Vec3 forward = AimDirection(player.yaw, 0.f);		// 앞 입력의 요 — player.yaw 는 이 입력 걸음 뒤에 바뀐다
		Vec3 hand;
		const bool handFromPose = MeleeHandOf(player, hand);
		if (!handFromPose) ++_stats.meleeHandFallback;

		const uint32_t nowTick = _tick - 1;
		uint32_t bodyTick = nowTick;
		float bodyAlpha = 0.f;
		switch (ClassifyRewind(nowTick, input.viewTick, input.viewAlpha, _rewind.limitTicks))
		{
		case ERewindKind::NoView:
			++_stats.meleeRewindNoView;
			break;
		case ERewindKind::Future:
			++_stats.meleeRewindFuture;
			break;
		case ERewindKind::Clamped:
			++_stats.meleeRewindClamped;
			bodyTick = nowTick - _rewind.limitTicks;
			break;
		case ERewindKind::Ok:
			++_stats.meleeRewindOk;
			bodyTick = input.viewTick;
			bodyAlpha = input.viewAlpha;
			break;
		}

		// 대상 = 지금 살아 있는 남(사격과 같다 — 지금 죽어 있으면 그때 살아 있었어도 안 맞는다). 캡슐 · 메시 몸체 둘 다 본다(e1g1)
		constexpr double kSqrt3 = 1.7320508075688772;
		const double reach = _anim.boundRadius + kMeleeRadius * kSqrt3;		// 자세를 구할 몸 — PrepareBodyPoses 와 같은 어림
		const Vec3 sweepEnd = hand + forward * kMeleeDistance;
		_meleeTargets.clear();
		_meleeOwners.clear();
		for (const std::unique_ptr<Player>& other : _players)
		{
			if (other.get() == &player || other->dead) continue;
			MeleeTarget target;
			float poseAlpha = 0.f;
			if (!GetRewoundBody(*other, bodyTick, bodyAlpha, target.body, poseAlpha)) continue;
			// 캡슐 높이 = 그 틱 앉음. 다음 틱과 섞어 자리가 사이에 와도 높이는 앞 틱 것이다(제 판단 — 앉는 두 틱 사이만 다르다)
			const RewindSample& sample = other->rewindHistory[bodyTick % other->rewindHistory.size()];
			target.capsuleRadius = _params.capsuleRadius;
			target.capsuleHalfHeight = sample.crouched ? _params.capsuleHalfHeight - _moveSim.CrouchMeshLiftZ() : _params.capsuleHalfHeight;
			target.sameTeam = other->team == player.team;
			if (_animReady)
			{
				if (SegmentPointDistanceSquared(hand, sweepEnd, target.body.center) > reach * reach)
				{
					target.body.skip = true;
					++_stats.meleePoseSkipped;
				}
				else
				{
					target.body.pose = GetPoseAt(*other, bodyTick, poseAlpha, false);
					target.body.shapes = _anim.clips->Bodies().data();
					target.body.shapeCount = (uint32_t)_anim.clips->Bodies().size();
					++_stats.meleePoses;
				}
			}
			_meleeTargets.push_back(target);
			_meleeOwners.push_back(other.get());
		}
		_stats.meleeTargets += _meleeTargets.size();

		MeleeResult result;
		ResolveMelee(_map, _meleeTargets.data(), _meleeTargets.size(), hand, forward, result);
		wire::Melee msg = wire::Melee();
		msg.h.size = (uint16_t)sizeof(msg);
		msg.h.type = (uint16_t)wire::Msg::S2C_MELEE;
		msg.tick = _tick;
		msg.attacker = player.id;
		msg.seq = input.seq;
		msg.result = (uint8_t)result.kind;
		msg.weapon = player.weapon;
		Player* const target = result.kind == EMeleeHit::None ? nullptr : _meleeOwners[result.targetIndex];
		if (target)
		{
			msg.target = target->id;
			msg.distance = (float)result.distance;
		}
		switch (result.kind)
		{
		case EMeleeHit::None:
			++_stats.meleeMisses;
			break;
		case EMeleeHit::SameTeam:
			++_stats.meleeSameTeam;		// 피해 · 밀기 · 반응 없음(0-3 같은 팀 칸)
			break;
		case EMeleeHit::Blocked:
			++_stats.meleeBlocked;
			break;
		case EMeleeHit::Hit:
		{
			++_stats.meleeHits;
			// 밀기 — 와이어에 싣는 float 값 그대로 넣는다. 공격자 화면이 같은 값으로 다시 돌려 위치가 같게(줄인 차 = 힘의 float 반올림만큼)
			const Vec3 exact = MeleePushForce(forward, result.distance);
			msg.forceX = (float)exact.x;
			msg.forceY = (float)exact.y;
			player.meleePushId = _moveSim.ApplyRootForce(player.move, Vec3(msg.forceX, msg.forceY, 0.0), kMeleePushDuration);
			++_stats.meleePushes;
			// 맞은 쪽 반응 — 둘 중 무작위(0-3 · D58). 입력 번호 · 맞은 사람으로 정해 같은 입력이면 같은 쪽
			CSpreadRng react(SpreadSeed(input.seq, target->id));
			PushAnimEvent(*target, react.NextFloat() < 0.5f ? EAnimEvent::HitBack : EAnimEvent::HitFront, target->weapon);
			const bool immune = IsImmune(*target);
			if (immune) ++_stats.meleeHitImmune;
			msg.damage = immune ? 0.f : kMeleeDamage;		// 거리 · 부위 보정 없음(0-3)
			break;
		}
		}
		if (_allowDebug)
		{
			// 시험판(ServerProbe --melee)이 Lyra 연습장 기록의 손 · 맞은 거리와 견준다
			std::printf("MELEEEV tick=%u room=%u attacker=%u seq=%u weapon=%u hand=%.4f,%.4f,%.4f fromPose=%d fwd=%.6f,%.6f,%.6f result=%u target=%u dist=%.4f capsule=%d force=%.4f,%.4f damage=%.1f\n",
				_tick, _roomId, player.id, input.seq, (unsigned)player.weapon, hand.x, hand.y, hand.z, handFromPose ? 1 : 0, forward.x, forward.y, forward.z,
				(unsigned)msg.result, msg.target, result.distance, result.capsule ? 1 : 0, msg.forceX, msg.forceY, msg.damage);
		}
		SendToAll(&msg, (int)sizeof(msg));		// 근접 → 피해 → 처치 차례로 받게 먼저 보낸다

		if (target && msg.damage > 0.f)
		{
			target->hp = std::max(target->hp - msg.damage, 0.f);
			NoteDamage(*target, player.id);
			wire::Damage damage = wire::Damage();
			damage.h.size = (uint16_t)sizeof(wire::Damage);
			damage.h.type = (uint16_t)wire::Msg::S2C_DAMAGE;
			damage.tick = _tick;
			damage.attacker = player.id;
			damage.target = target->id;
			damage.amount = msg.damage;
			damage.hpAfter = target->hp;
			damage.weapon = wire::kWeaponMelee;
			damage.weakSpot = 0;
			SendToAll(&damage, (int)sizeof(damage));
			++_stats.damageEvents;
			++_stats.meleeDamageEvents;
			if (target->hp <= 0.f)
			{
				++_stats.meleeKills;
				KillPlayer(*target, &player, wire::kWeaponMelee, false);
			}
		}
		return true;
	}

	// 공격자 손(hand_r) — 마지막으로 적은 틱의 자세로 몸 기준 손 자리를 구해, 걸음 앞 자리 · 앞 입력의 요로 놓는다.
	// 애니가 없거나 그 틱 기록이 없으면(부활 첫 입력 등) false — 권총 기본 자세 손(앉았으면 앉은 기본 자세 손)으로 놓는다
	bool CRoom::MeleeHandOf(const Player& player, Vec3& out)
	{
		double local[3];
		bool fromPose = false;
		const uint32_t lastTick = _tick - 1;
		const RewindSample& last = player.rewindHistory[lastTick % player.rewindHistory.size()];
		if (_animReady && last.tick == lastTick && last.alive)
		{
			AnimPoseInput input;
			_animCodecs[last.animWeapon].FromWire(last.anim, last.pitch, last.crouched, input);
			CAnimPoseEvaluator& evaluator = _poseEvaluators[last.animWeapon];
			evaluator.EvaluateLocal(input, _localFrom.data());
			evaluator.HandRightFromLocal(_localFrom.data(), local, last.crouched ? _moveSim.CrouchMeshLiftZ() : 0.0);
			fromPose = true;
		}
		else
		{
			MeleeRestHandLocal(player.move.crouched, _moveSim.CrouchMeshLiftZ(), local);
		}
		out = MeleeHandWorld(player.move.location, player.yaw, local);
		return fromPose;
	}

	// 사격 시작점은 클라가 보내지만 믿지는 않는다. 쏜 사실 자체는 안 물리고(탄·연사는 같은 입력이면 클라와 같아야 한다) 시작점만 바꾼다 —
	// 위조해도 내 몸 근처에서 나가니 이득이 없다.
	// - 범위는 움직이기 전·후 둘 다 본다: 클라는 움직이기 전 자리로 계산해 보내는데 낙하 중엔 한 틱에 67cm 까지 움직인다
	// - 벽에 붙으면 정직한 시작점도 벽 너머일 수 있어(Lyra 는 카메라 쪽 점에서 쏜다) 거절하지 않고 벽 앞으로 당긴다
	Vec3 CRoom::ResolveAimStart(const Player& player, const Vec3& locationBeforeMove, const Vec3& aim)
	{
		const Vec3& location = player.move.location;
		Vec3 start = aim;
		if (!IsAimInRange(aim, locationBeforeMove) && !IsAimInRange(aim, location))
		{
			start = location + Vec3(0, 0, player.move.crouched ? kAimFallbackHeight - kAimFallbackCrouchDrop : kAimFallbackHeight);
			if ((int)player.fireGate.GetInputIndex() <= kAimStaleInputs) ++_stats.shotsAimStaleAfterSpawn;
			else ++_stats.shotsAimReplaced;
		}
		RayHit wall;
		if (_map.Raycast(location, start, MAP_BLOCK_WEAPON, wall))
		{
			const Vec3 toStart = start - location;
			const double keep = std::max(wall.distance - 1.0, 0.0) / std::max(Length(toStart), 1e-6);
			start = location + toStart * keep;
			++_stats.shotsAimPulledBack;
		}
		return start;
	}

	void CRoom::TickPlayerInputs(Player& player)
	{
		player.tokens = std::min(player.tokens + 1, kMaxInputTokens);

		bool applied = false;
		while (player.queueCount > 0)
		{
			const QueuedInput& item = player.queue[player.queueHead];
			if (!item.isPlace && player.tokens <= 0) break;
			if (item.isPlace)
			{
				ApplyPlace(player, item.place);
			}
			else
			{
				--player.tokens;
				ApplyInput(player, item.input);
				applied = true;
			}
			player.queueHead = (player.queueHead + 1) % kInputQueueSize;
			--player.queueCount;
		}

		if (applied)
		{
			player.starvedInRow = 0;
			return;
		}
		if (player.lastSeq == 0 || player.dead) return;	// 입력을 한 번도 안 보냈거나 죽어 있으면 그대로 둔다
		++_stats.starvedTicks;
		if (++player.starvedInRow > kIdleMoveAfterTicks && player.tokens > 0)
		{
			// 입력을 끊어 공중에 머무는 일이 없게 — 쌓인 한도를 쓰게 해 다시 밀린 입력으로 빨라지는 것도 줄인다
			--player.tokens;
			++_stats.idleMoves;
			wire::Input idle = wire::Input();
			idle.yaw = player.yaw;
			idle.pitch = player.pitch;
			idle.weapon = player.weapon;
			idle.seq = player.lastSeq;
			if (player.jump.held) idle.buttons |= wire::BUTTON_JUMP;
			if (player.ads.held) idle.buttons |= wire::BUTTON_ADS;		// 끊긴 틱에 조준이 풀렸다 다시 켜지지 않게 — D87 뒤로는 빈 입력이 ads.Apply 를 안 불러 조준 상태가 그대로 남는다. 이 줄은 버튼 값만 맞춰 둔다
			// 앉기는 누른 입력 하나에만 켜는 버튼이라 채우지 않는다 — 앉음은 이동 상태(wantsToCrouch · crouched)에 남는다
			ApplyInput(player, idle, true);
			--_stats.inputsApplied;
		}
	}

	// ─── 사격 ──────────────────────────────────────────────
	void CRoom::KillPlayer(Player& target, Player* attacker, uint8_t weapon, bool weakSpot)
	{
		target.dead = true;
		target.hp = 0;
		target.ads.Reset();		// Lyra 죽음 능력이 다른 능력을 끈다 — 카메라는 새 몸에서 다시 시작한다(SpawnPlayer)
		target.respawnTick = _tick + SecondsToTicks(_rules.respawnSeconds);
		target.move.velocity = Vec3();
		std::fill(target.padOverlap.begin(), target.padOverlap.end(), (uint8_t)0);	// 죽으면 충돌이 꺼져 겹침이 끝난다
		++target.deaths;

		// B_ShooterGameScoring_Base — 죽은 쪽은 늘 +1, 다른 팀이 죽였을 때만 처치와 팀 점수 +1
		const bool scored = attacker && attacker->team != target.team;
		if (scored)
		{
			++attacker->kills;
			uint16_t& score = attacker->team == wire::kTeamA ? _teamScoreA : _teamScoreB;
			++score;
		}
		if (attacker) ++_stats.kills;

		// AssistProcessor::OnEliminationMessage — 피해를 준 사람 중 처치자를 뺀 전원이 +1, 문턱 없음. 떨어져 죽으면 처치자가 없어 전원이 받는다.
		// 그 사람 기록은 지운다(AssistProcessor.cpp:40-71)
		wire::Kill kill = wire::Kill();
		for (int i = 0; i < target.damagedByCount; ++i)
		{
			if (attacker && target.damagedBy[i] == attacker->id) continue;
			Player* const assister = FindById(target.damagedBy[i]);
			if (!assister) continue;
			++assister->assists;
			++_stats.assists;
			if (kill.assistCount < wire::kMaxPlayersInRoom - 1) kill.assisters[kill.assistCount++] = assister->id;
		}
		target.damagedByCount = 0;

		const int killSize = (int)(offsetof(wire::Kill, assisters) + sizeof(uint32_t) * kill.assistCount);	// 도움 번호는 수만큼만
		kill.h.size = (uint16_t)killSize;
		kill.h.type = (uint16_t)wire::Msg::S2C_KILL;
		kill.tick = _tick;
		kill.attacker = attacker ? attacker->id : 0;
		kill.target = target.id;
		kill.teamScoreA = _teamScoreA;
		kill.teamScoreB = _teamScoreB;
		kill.attackerKills = attacker ? attacker->kills : 0;
		kill.targetDeaths = target.deaths;
		kill.weapon = weapon;
		kill.weakSpot = weakSpot ? 1 : 0;
		SendToAll(&kill, killSize);
		_sender.OnKillRecorded(kill);

		if (scored)
		{
			const uint16_t score = attacker->team == wire::kTeamA ? _teamScoreA : _teamScoreB;
			if (score >= _targetScore && _targetScore > 0) HandleVictory(attacker->team);
		}
	}

	// AssistProcessor::OnDamageMessage — 때린 사람별로 쌓는다. 자기 피해는 뺀다(AssistProcessor.cpp:25). 피해량은 도움 수에 안 쓰이므로 번호만 든다
	void CRoom::NoteDamage(Player& target, uint32_t attackerId)
	{
		if (attackerId == target.id) return;
		const uint32_t* const begin = target.damagedBy.data();
		const uint32_t* const end = begin + target.damagedByCount;
		if (std::find(begin, end, attackerId) != end) return;
		if (target.damagedByCount < (int)target.damagedBy.size()) target.damagedBy[target.damagedByCount++] = attackerId;	// 나간 사람은 RemovePlayer 가 빼므로 방 인원 − 1 을 넘지 않는다
	}

	// 틱마다 입력을 모두 적용한 뒤, 사격 판정 앞에서 적는다 — 상태 패킷으로 나가는 자리와 같은 틱 값이어야 화면 자리와 맞는다
	void CRoom::RecordRewindSamples()
	{
		for (const std::unique_ptr<Player>& player : _players)
		{
			RewindSample& sample = player->rewindHistory[_tick % player->rewindHistory.size()];
			sample.tick = _tick;
			sample.location = player->move.location;
			sample.yaw = player->yaw;
			sample.pitch = player->pitch;
			sample.alive = !player->dead;
			sample.crouched = player->move.crouched;
			if (_animReady)
			{
				player->anim.ToWire(sample.anim);
				sample.animWeapon = player->animWeapon;
			}
		}
	}

	// 언리얼 쪽 입력과 같은 값을 만든다 — MOBSimpleAnimInstance.cpp 의 Pitch · bAccelerating · GroundDistance · PlantDrop
	void CRoom::MakeMoveInput(const Player& player, AnimMoveInput& out) const
	{
		constexpr double kAccelerationNearlyZero = 1e-8;	// FMath::IsNearlyZero 기본 허용치
		constexpr float kGroundTraceDistance = 100000.f;	// ULyraCharacterMovementComponent::GetGroundInfo

		const MoveState& move = player.move;
		const double yaw = player.yaw * kPi / 180.0;
		const double cosYaw = std::cos(yaw);
		const double sinYaw = std::sin(yaw);
		out.deltaSeconds = kTickSeconds;
		out.velocity[0] = cosYaw * move.velocity.x + sinYaw * move.velocity.y;
		out.velocity[1] = -sinYaw * move.velocity.x + cosYaw * move.velocity.y;
		out.velocity[2] = move.velocity.z;
		out.acceleration[0] = cosYaw * move.acceleration.x + sinYaw * move.acceleration.y;
		out.acceleration[1] = -sinYaw * move.acceleration.x + cosYaw * move.acceleration.y;
		out.acceleration[2] = move.acceleration.z;
		out.accelerating = move.acceleration.x * move.acceleration.x + move.acceleration.y * move.acceleration.y > kAccelerationNearlyZero;
		out.falling = move.mode == EMoveMode::Falling;
		out.ads = player.ads.active;		// 이 틱 입력이 켠 조준 — Lyra 애니는 능력이 바뀐 뒤 첫 갱신에 본다(D61)
		out.crouched = move.crouched;		// 이 틱 이동이 끝낸 앉음(bIsCrouched) — Lyra 애니는 바뀐 프레임에 본다(D62 8 · D63 3)
		out.pitch = std::min(std::max(NormalizeAxis(player.pitch), -90.f), 90.f);
		if (out.falling)
		{
			// 지금 캡슐 반높이 — Lyra GetGroundInfo 는 GetUnscaledCapsuleHalfHeight(앉은 채 떨어지면 65)로 잰다(LyraCharacterMovementComponent.cpp:121)
			const float halfHeight = _moveSim.HalfHeightOf(move);
			out.groundDistance = kGroundTraceDistance;
			Vec3 end = move.location;
			end.z -= kGroundTraceDistance + halfHeight;
			RayHit hit;
			if (_map.Raycast(move.location, end, MAP_BLOCK_MOVE, hit)) out.groundDistance = std::max((float)hit.distance - halfHeight, 0.f);
		}
		else if (move.floor.blockingHit)
		{
			out.hasFloorGap = true;
			out.plantGap = _anim.clips->MeshPlacement().p[2] + _params.capsuleHalfHeight + move.floor.GetDistanceToFloor();
		}
	}

	// 입력을 다 적용한 뒤, 기록 앞에서 사람마다 한 번 — 입력이 몇 개 왔든 시간은 틱만큼 흐른다
	void CRoom::UpdateAnimStates()
	{
		if (!_animReady) return;
		for (const std::unique_ptr<Player>& player : _players)
		{
			// 무기를 바꾼 틱 — 그 틱 입력을 다 적용한 뒤 마지막 무기로 한 번 바꾼다. 패킷의 weapon 과 이 틱 상태의 묶음이 늘 같게 죽은 사람도 맞춘다
			if (player->animWeapon != player->weapon)
			{
				if (player->anim.ChangeBundle(*_anim.clips, AnimBundleOf(player->weapon)))
				{
					player->animWeapon = player->weapon;
					++_stats.animBundleChanges;
				}
				else
				{
					++_stats.animBundleFails;
				}
			}
			if (player->dead)
			{
				DropAnimEvents(*player);		// 이 틱 입력에서 떨어져 죽었다 — 몸이 없으니 틀지 않는다
				continue;
			}
			PlayAnimEvents(*player);
			player->anim.SetFiring(player->weapons[player->weapon].IsFireActive());		// Lyra IsFiring 태그 = 쏘기 능력이 켜진 동안(소총 · 권총 8프레임이 기록과 같다)
			AnimMoveInput input;
			MakeMoveInput(*player, input);
			player->anim.Update(input);
			++_stats.animUpdates;
		}
	}

	void CRoom::PushAnimEvent(Player& player, EAnimEvent kind, uint8_t weapon)
	{
		if (!_animReady) return;
		++_stats.animEvents[(size_t)kind];
		if (player.animEventCount == (int)player.animEvents.size())
		{
			++_stats.animEventsDropped[(size_t)kind];
			return;
		}
		player.animEvents[(size_t)player.animEventCount++] = { kind, weapon };
	}

	void CRoom::DropAnimEvents(Player& player)
	{
		for (int i = 0; i < player.animEventCount; ++i) ++_stats.animEventsDropped[(size_t)player.animEvents[(size_t)i].kind];
		player.animEventCount = 0;
	}

	// 이번 틱 사건을 생긴 차례대로 몽타주로 튼다. 앞 틱 갱신 뒤 · 이번 갱신 앞이라 새 칸은 시각 0 · 비중 0 으로 생겨 이번 갱신부터 움직인다(CAnimState::PlayMontage).
	// Lyra 는 사건 프레임의 갱신 뒤에 틀고 다음 프레임 갱신 앞에 무기 층을 바꾼다 — 서버는 입력 틱에 묶음을 바꾸므로(D46) 이 자리가 「튼 뒤 첫 갱신 = 새 층 첫 갱신」을 Lyra 와 같게 둔다.
	// 속도: 쏘기 · 재장전 = 무기 표(autoRate · reloadRate), 빼기 · 꺼내기 · 부활 = 1.0(9-1 기록 세 무기 다, 실측). 몽타주는 사건 무기의 묶음에서 찾는다(대조 도구 AnimCheck 와 같다)
	void CRoom::PlayAnimEvents(Player& player)
	{
		constexpr float kAnimEventRate = 1.f;
		CAnimState& anim = player.anim;
		const int dropsBefore = anim.MontageDrops();
		const auto play = [this, &anim](uint8_t kind, float rate, int bundle)
		{
			if (anim.PlayMontage(kind, rate, bundle)) ++_stats.montageStarts[kind];
			else ++_stats.montageFails;
		};
		for (int i = 0; i < player.animEventCount; ++i)
		{
			const Player::AnimEvent& event = player.animEvents[(size_t)i];
			const int bundle = _animBundleIndex[event.weapon];
			const WeaponDef& def = GetWeaponDef((EWeapon)event.weapon);
			switch (event.kind)
			{
			case EAnimEvent::Spawn:
				play(MONTAGE_SPAWN, kAnimEventRate, bundle);
				break;
			case EAnimEvent::Swap:
				// Lyra 는 앞 무기를 넣는 빼기를 틀고 같은 프레임에 꺼내기가 같은 무리로 끈다 — 빼기는 비중 0 인 채 빠진다(9-1 기록)
				play(MONTAGE_UNEQUIP, kAnimEventRate, bundle);
				play(MONTAGE_EQUIP, kAnimEventRate, bundle);
				break;
			case EAnimEvent::Fire:
			{
				bool cancelledReload = false;
				if (anim.PlayFireMontage(def.autoRate, bundle, cancelledReload)) ++_stats.montageStarts[MONTAGE_FIRE];
				else ++_stats.montageFails;
				if (cancelledReload) ++_stats.reloadCancels;
				break;
			}
			case EAnimEvent::Reload:
				play(MONTAGE_RELOAD, def.reloadRate, bundle);
				break;
			case EAnimEvent::Melee:
			{
				// 근접 능력은 재장전 능력을 끊는다(TagRelationships_ShooterHero) — 쏘기와 같은 길로 재장전 칸을 먼저 끈다. 속도 1.0(15-b 기록 세 무기, 실측)
				bool cancelledReload = false;
				if (anim.PlayMontageCancellingReload(MONTAGE_MELEE, kAnimEventRate, bundle, cancelledReload)) ++_stats.montageStarts[MONTAGE_MELEE];
				else ++_stats.montageFails;
				if (cancelledReload) ++_stats.meleeReloadMontageCancels;
				break;
			}
			case EAnimEvent::HitBack:
				play(MONTAGE_HIT_BACK, kAnimEventRate, bundle);		// 덧붙임 무리라 쏘기 · 재장전 몽타주를 안 끈다(클립 묶음 자산 값). 속도 1.0(15-b, 실측)
				break;
			case EAnimEvent::HitFront:
				play(MONTAGE_HIT_FRONT, kAnimEventRate, bundle);
				break;
			case EAnimEvent::DashForward:
			case EAnimEvent::DashBackward:
			case EAnimEvent::DashLeft:
			case EAnimEvent::DashRight:
				// 대시 · 재장전 · 근접이 같은 무리(DefaultGroup, 기록의 자산 표 animcmp_dash_rifle_montages.csv)라 같은 무리 규칙이 대시의 넣기 섞기로 끈다 — 쏘기 · 근접처럼 재장전 칸을 먼저 끄지 않는다.
				// Lyra 기록(D-4-b rl_dash): 재장전 비중이 대시 넣기의 거울(1 → 0.844 → 0.500 → 0.157 → 0)로 빠졌다. 먼저 끄면 재장전 빼기 곡선이 남는다(StopMontage 는 시간만 줄인다). 속도 1.0(같은 기록, 실측)
				play((uint8_t)(MONTAGE_DASH_FORWARD + ((uint8_t)event.kind - (uint8_t)EAnimEvent::DashForward)), kAnimEventRate, bundle);
				break;
			case EAnimEvent::Count:
				break;
			}
		}
		player.animEventCount = 0;
		_stats.montageDrops += (uint64_t)(anim.MontageDrops() - dropsBefore);
	}

	// Lyra 가 남의 캐릭터를 그리는 식과 같다(MOBridgeSubsystem::TickRemotes). false = 그 시점에 몸이 없었다(죽었거나 아직 안 들어옴)
	bool CRoom::GetRewoundBody(const Player& player, uint32_t viewTick, float viewAlpha, BodyPlacement& out, float& outAlpha) const
	{
		outAlpha = 0.f;
		const size_t size = player.rewindHistory.size();
		const RewindSample& from = player.rewindHistory[viewTick % size];
		if (from.tick != viewTick || !from.alive) return false;
		out.center = from.location;
		out.yawDegrees = from.yaw;
		out.id = player.id;
		if (viewAlpha <= 0.f) return true;
		const RewindSample& to = player.rewindHistory[(viewTick + 1) % size];
		if (to.tick != viewTick + 1 || Length(to.location - from.location) > wire::kRemoteTeleportDistance) return true;
		out.center = from.location + (to.location - from.location) * (double)viewAlpha;
		out.yawDegrees = from.yaw + viewAlpha * NormalizeAxis(to.yaw - from.yaw);
		outAlpha = viewAlpha;
		return true;
	}

	// 탄이 닿을 수 있는 몸만 자세를 구한다. 안 닿는 몸은 skip 표시로 판정에서 뺀다
	void CRoom::PrepareBodyPoses(std::vector<BodyPlacement>& bodies, const std::vector<BodyPoseRef>& refs, const Vec3& start, const Vec3& end, double sweepRadius)
	{
		// 구로 쏠 때 부위는 탄 반지름만큼 부푼다. 상자는 면을 세 축으로 밀어 모서리가 √3 배까지 나가므로 그 몫으로 잡는다
		constexpr double kSqrt3 = 1.7320508075688772;
		const double reach = _anim.boundRadius + sweepRadius * kSqrt3;
		for (size_t i = 0; i < bodies.size(); ++i)
		{
			BodyPlacement& body = bodies[i];
			if (SegmentPointDistanceSquared(start, end, body.center) > reach * reach)
			{
				body.skip = true;
				++_stats.poseSkipped;
				continue;
			}
			body.skip = false;
			body.pose = GetPoseAt(*refs[i].owner, refs[i].tick, refs[i].alpha);
			body.shapes = _anim.clips->Bodies().data();
			body.shapeCount = (uint32_t)_anim.clips->Bodies().size();
		}
	}

	// 판정 자세 = 그 틱의 상태로 만든 자세. alpha > 0 이면 다음 틱 자세와 뼈별로 섞는다 — 화면과 같은 식(D32)
	const BodyPose* CRoom::GetPoseAt(const Player& owner, uint32_t tick, float alpha, bool countStats)
	{
		for (size_t i = 0; i < _poseCacheUsed; ++i)
		{
			const CachedPose& cached = _poseCache[i];
			if (cached.owner == owner.id && cached.tick == tick && cached.alpha == alpha)
			{
				if (countStats) ++_stats.poseReused;
				return cached.bodies.data();
			}
		}

		if (_poseCacheUsed == _poseCache.size())
		{
			_poseCache.emplace_back();
			_poseCache.back().bodies.resize(_poseEvaluators[0].BodyCount());
		}
		CachedPose& cached = _poseCache[_poseCacheUsed++];
		cached.owner = owner.id;
		cached.tick = tick;
		cached.alpha = alpha;

		const size_t size = owner.rewindHistory.size();
		// 틱마다 그 틱 상태를 만든 묶음으로 푼다. 무기를 바꾼 틱 사이도 두 묶음 자세를 뼈별로 섞는다 — 뼈대가 같아 섞기 · 부위 계산은 어느 평가기로 해도 같다
		const RewindSample& from = owner.rewindHistory[tick % size];
		CAnimPoseEvaluator& evaluator = _poseEvaluators[from.animWeapon];
		AnimPoseInput input;
		wire::AnimStateWire shiftedFrom, shiftedTo;	// 일부러 틀림 때만 쓴다(JudgeAnim)
		_animCodecs[from.animWeapon].FromWire(JudgeAnim(from.anim, _rewind.montageShiftTicks, shiftedFrom), from.pitch, from.crouched, input);		// 앉음은 되감기 기록에서(D62 8)
		evaluator.EvaluateLocal(input, _localFrom.data());
		const BoneTransform* local = _localFrom.data();
		// 앉은 메시 올림(D62 7) — 다음 틱과 섞으면 자리(캡슐 중심, GetRewoundBody)와 같은 비율로 섞는다. 땅에서 앉은 틱은 중심이 25 내려가고 올림이 25 늘어 섞는 동안에도 메시 세상 자리가 그대로다
		double meshLift = from.crouched ? _moveSim.CrouchMeshLiftZ() : 0.0;
		if (alpha > 0.f)
		{
			const RewindSample& to = owner.rewindHistory[(tick + 1) % size];
			AnimPoseInput nextInput;
			_animCodecs[to.animWeapon].FromWire(JudgeAnim(to.anim, _rewind.montageShiftTicks, shiftedTo), to.pitch, to.crouched, nextInput);
			_poseEvaluators[to.animWeapon].EvaluateLocal(nextInput, _localTo.data());
			evaluator.BlendLocal(_localFrom.data(), _localTo.data(), alpha, _localBlend.data());
			local = _localBlend.data();
			meshLift += ((to.crouched ? _moveSim.CrouchMeshLiftZ() : 0.0) - meshLift) * (double)alpha;
		}
		evaluator.BodiesFromLocal(local, cached.bodies.data(), meshLift);
		if (countStats)
		{
			++_stats.poseEvals;
			++_stats.poseEvalsByWeapon[from.animWeapon];
		}
		return cached.bodies.data();
	}

	void CRoom::ResolveShot(const PendingShot& shot)
	{
		// 그 시점 = 정상이면 보던 시점, 한도 초과면 한도 시점. 시점 없음·미래는 현재 자리
		const ERewindKind rewindKind = ClassifyRewind(_tick, shot.viewTick, shot.viewAlpha, _rewind.limitTicks);
		bool useHistory = false;
		uint32_t bodyTick = 0;
		float bodyAlpha = 0.f;
		switch (rewindKind)
		{
		case ERewindKind::NoView:
		case ERewindKind::Future:
			break;
		case ERewindKind::Clamped:
			useHistory = true;
			bodyTick = _tick - _rewind.limitTicks;
			break;
		case ERewindKind::Ok:
			useHistory = true;
			bodyTick = shot.viewTick;
			bodyAlpha = shot.viewAlpha;
			break;
		}

		Player* shooter = nullptr;
		_bodies.clear();
		_bodyOwners.clear();
		_bodiesNow.clear();
		_bodyPoseRefs.clear();
		_bodyNowRefs.clear();
		for (const std::unique_ptr<Player>& player : _players)
		{
			if (player->id == shot.shooter)
			{
				shooter = player.get();
				continue;
			}
			if (player->dead)		// 지금 죽어 있으면 그때 살아 있었어도 안 맞는다
			{
				if (_rewind.bulletLog) std::printf("DEADSKIP seq=%u shooter=%u target=%u\n", shot.seq, shot.shooter, player->id);
				continue;
			}
			BodyPlacement now;
			now.center = player->move.location;
			now.yawDegrees = player->yaw;
			now.id = player->id;
			if (_rewind.shadow)
			{
				_bodiesNow.push_back(now);
				if (_animReady) _bodyNowRefs.push_back({ player.get(), _tick, 0.f });
			}
			BodyPlacement body = now;
			float poseAlpha = 0.f;
			if (useHistory && !GetRewoundBody(*player, bodyTick, bodyAlpha, body, poseAlpha)) continue;
			if (_rewind.log)
			{
				// animWeapon = 아래 자세를 푼 묶음(GetPoseAt 이 읽는 그 틱 기록) — 화면 대조를 무기별로 나눠 센다
				// montages = 그 자세에 든 몽타주 칸 수(그 틱 기록, 다음 틱과 섞으면 둘 중 큰 쪽) — 「상대가 몽타주 중」인 쏨을 따로 센다(9-5)
				const size_t historySize = player->rewindHistory.size();
				const uint32_t poseTick = useHistory ? bodyTick : _tick;
				const RewindSample& poseSample = player->rewindHistory[poseTick % historySize];
				uint8_t poseMontages = poseSample.anim.montageCount;
				if (poseAlpha > 0.f) poseMontages = std::max(poseMontages, player->rewindHistory[(poseTick + 1) % historySize].anim.montageCount);
				// ads = 그 자세의 조준 묶음 비중 0~255(같은 식으로 다음 틱과 섞으면 큰 쪽) — 「상대가 조준 중」인 쏨을 따로 센다(C-5)
				uint8_t poseAds = poseSample.anim.adsBlend;
				if (poseAlpha > 0.f) poseAds = std::max(poseAds, player->rewindHistory[(poseTick + 1) % historySize].anim.adsBlend);
				// crouch = 그 자세의 앉기 묶음 비중 0~255(같은 식) — 「상대가 앉음」인 쏨을 따로 센다(D-6)
				uint8_t poseCrouch = poseSample.anim.crouchBlend;
				if (poseAlpha > 0.f) poseCrouch = std::max(poseCrouch, player->rewindHistory[(poseTick + 1) % historySize].anim.crouchBlend);
				// react = 그 자세에 맞은 쪽 반응 몽타주가 들었나(다음 틱과 섞으면 둘 중 하나라도) — 「상대가 반응 중」인 쏨을 따로 센다(E-3 17-d)
				const bool poseReact = HasHitReactMontage(poseSample.anim) || (poseAlpha > 0.f && HasHitReactMontage(player->rewindHistory[(poseTick + 1) % historySize].anim));
				std::printf("REWIND seq=%u shooter=%u target=%u now=%u viewTick=%u alpha=%.4f x=%.3f y=%.3f z=%.3f yaw=%.3f animWeapon=%u montages=%u ads=%u crouch=%u react=%u\n", shot.seq, shot.shooter, player->id, _tick, bodyTick, bodyAlpha, body.center.x, body.center.y, body.center.z, body.yawDegrees, (unsigned)poseSample.animWeapon, (unsigned)poseMontages, (unsigned)poseAds, (unsigned)poseCrouch, poseReact ? 1u : 0u);
				if (_animReady)
				{
					// 화면 MOB_VIEWP 줄과 (seq, 상대, 부위) 로 잇는다. 기록용이라 비용 집계에 넣지 않는다
					const BodyPose* pose = GetPoseAt(*player, useHistory ? bodyTick : _tick, poseAlpha, false);
					const std::vector<ClipBody>& shapes = _anim.clips->Bodies();
					const std::vector<ClipBone>& bones = _anim.clips->Bones();
					for (size_t part = 0; part < shapes.size(); ++part)
					{
						const ClipBody& shape = shapes[part];
						const BodyPose& at = pose[part];
						std::printf("REWINDP seq=%u shooter=%u target=%u bone=%s kind=%d weak=%d p=%.4f,%.4f,%.4f x=%.5f,%.5f,%.5f y=%.5f,%.5f,%.5f z=%.5f,%.5f,%.5f s=%.4f,%.4f,%.4f\n", shot.seq, shot.shooter, player->id, bones[(size_t)shape.bone].name.c_str(), (int)shape.kind, shape.weakSpot ? 1 : 0,
							at.center[0], at.center[1], at.center[2], at.axisX[0], at.axisX[1], at.axisX[2], at.axisY[0], at.axisY[1], at.axisY[2], at.axisZ[0], at.axisZ[1], at.axisZ[2], shape.a, shape.b, shape.c);
					}
				}
			}
			_bodies.push_back(body);
			_bodyOwners.push_back(player.get());
			if (_animReady) _bodyPoseRefs.push_back({ player.get(), useHistory ? bodyTick : _tick, poseAlpha });
		}
		if (!shooter) return;	// 쏜 틱에 나감

		switch (rewindKind)
		{
		case ERewindKind::NoView: ++_stats.rewindNoView; break;
		case ERewindKind::Future: ++_stats.rewindFuture; break;
		case ERewindKind::Clamped: ++_stats.rewindClamped; break;
		case ERewindKind::Ok: ++_stats.rewindOk; break;
		}
		if (useHistory) _stats.rewindTicksSum += _tick - bodyTick;

		const WeaponDef& def = GetWeaponDef((EWeapon)shot.weapon);
		if (_rewind.log)
		{
			std::printf("AIM seq=%u shooter=%u weapon=%u start=%.2f,%.2f,%.2f dir=%.5f,%.5f,%.5f spread=%.3f mult=%.3f\n", shot.seq, shot.shooter, (unsigned)shot.weapon, shot.start.x, shot.start.y, shot.start.z, shot.aimDir.x, shot.aimDir.y, shot.aimDir.z, shot.spreadAngle, shot.spreadMultiplier);
		}
		wire::Shot packet = wire::Shot();
		packet.tick = _tick;
		packet.shooter = shot.shooter;
		packet.startX = (float)shot.start.x;
		packet.startY = (float)shot.start.y;
		packet.startZ = (float)shot.start.z;
		packet.weapon = shot.weapon;
		packet.count = (uint8_t)std::min(def.bulletsPerCartridge, (int32_t)wire::kMaxBulletsInShot);
		if (_animReady) _stats.poseTargets += (uint64_t)packet.count * (_bodies.size() + _bodiesNow.size());

		for (int i = 0; i < packet.count; ++i)
		{
			CSpreadRng bulletRng(SpreadSeed(shot.seq, (uint32_t)i));
			Vec3 dir = SpreadDirection(shot.aimDir, shot.spreadAngle, shot.spreadMultiplier, def.spreadExponent, bulletRng);
			if (_rewind.noSpread) dir = shot.aimDir;
			if (_rewind.log)
			{
				std::printf("SPREAD seq=%u shooter=%u i=%d spread=%.6f mult=%.6f aim=%.7f,%.7f,%.7f dir=%.7f,%.7f,%.7f\n", shot.seq, shot.shooter, i, shot.spreadAngle, shot.spreadMultiplier, shot.aimDir.x, shot.aimDir.y, shot.aimDir.z, dir.x, dir.y, dir.z);
			}
			const Vec3 end = shot.start + dir * (double)def.maxDamageRange;
			BulletResult result;
			if (_animReady) PrepareBodyPoses(_bodies, _bodyPoseRefs, shot.start, end, def.sweepRadius);
			ResolveBullet(_map, _bodies.data(), _bodies.size(), shot.start, end, def.sweepRadius, result);
			if (_rewind.bulletLog)
			{
				const bool hitBody = result.kind == EBulletHit::Body;
				std::printf("BULLET seq=%u shooter=%u i=%d kind=%d target=%u weak=%d rewind=%d\n", shot.seq, shot.shooter, i, (int)result.kind, hitBody ? _bodies[result.bodyIndex].id : 0u, hitBody && result.weakSpot ? 1 : 0, (int)rewindKind);
			}

			++_stats.bullets;
			if (_rewind.shadow)
			{
				BulletResult now;
				if (_animReady) PrepareBodyPoses(_bodiesNow, _bodyNowRefs, shot.start, end, def.sweepRadius);
				ResolveBullet(_map, _bodiesNow.data(), _bodiesNow.size(), shot.start, end, def.sweepRadius, now);
				const bool hitAtView = result.kind == EBulletHit::Body;
				const bool hitNow = now.kind == EBulletHit::Body;
				if (hitAtView && hitNow) ++_stats.hitsBoth;
				else if (hitAtView) ++_stats.hitsRewindOnly;
				else if (hitNow) ++_stats.hitsNowOnly;
				else ++_stats.hitsNeither;
				if (hitAtView && !hitNow)
				{
					RayHit wall;
					if (_map.Raycast(shot.start, _bodyOwners[result.bodyIndex]->move.location, MAP_BLOCK_WEAPON, wall)) ++_stats.rewindOnlyBehindWall;
				}
			}
			wire::ShotBullet& bullet = packet.bullets[i];
			bullet.hit = (uint8_t)ToWireHit(result.kind);
			const Vec3 point = result.kind == EBulletHit::None ? end : result.point;
			bullet.x = (float)point.x;
			bullet.y = (float)point.y;
			bullet.z = (float)point.z;

			switch (result.kind)
			{
			case EBulletHit::None:
				++_stats.bulletsMissed;
				break;
			case EBulletHit::Map:
				++_stats.bulletsHitMap;
				break;
			case EBulletHit::Body:
			{
				++_stats.bulletsHitBody;
				Player& target = *_bodyOwners[result.bodyIndex];
				const bool sameTeam = target.team == shooter->team;
				const bool immune = IsImmune(target);
				if (immune) ++_stats.bulletsHitImmune;
				const float amount = immune ? 0.f : ComputeDamage(def, result.distance, result.weakSpot, sameTeam);
				bullet.target = target.id;
				bullet.weakSpot = result.weakSpot ? 1 : 0;
				bullet.damage = amount;
				if (amount > 0 && !target.dead)		// 샷건 한 발 안에서 이미 쓰러졌을 수 있다
				{
					target.hp = std::max(target.hp - amount, 0.f);
					NoteDamage(target, shooter->id);		// LyraHealthSet 은 0보다 큰 피해만 알린다(LyraHealthSet.cpp:131) — 같은 팀 · 무적은 amount 0 이라 안 쌓인다
					wire::Damage damage = wire::Damage();
					damage.h.size = (uint16_t)sizeof(wire::Damage);
					damage.h.type = (uint16_t)wire::Msg::S2C_DAMAGE;
					damage.tick = _tick;
					damage.attacker = shooter->id;
					damage.target = target.id;
					damage.amount = amount;
					damage.hpAfter = target.hp;
					damage.weapon = shot.weapon;
					damage.weakSpot = bullet.weakSpot;
					SendToAll(&damage, (int)sizeof(damage));
					++_stats.damageEvents;
					if (target.hp <= 0) KillPlayer(target, shooter, shot.weapon, result.weakSpot);
				}
				break;
			}
			}
		}

		const int size = (int)(offsetof(wire::Shot, bullets) + sizeof(wire::ShotBullet) * packet.count);
		packet.h.size = (uint16_t)size;
		packet.h.type = (uint16_t)wire::Msg::S2C_SHOT;
		SendToAll(&packet, size);
		++_stats.shots;
	}

	void CRoom::ResolveShots()
	{
		_poseCacheUsed = 0;
		for (const PendingShot& shot : _pendingShots)
		{
			ResolveShot(shot);
		}
		_pendingShots.clear();
	}

	void CRoom::SendState()
	{
		if (_players.empty()) return;

		wire::State packet;
		packet.tick = _tick;
		packet.count = 0;
		for (const std::unique_ptr<Player>& player : _players)
		{
			wire::PlayerState& out = packet.players[packet.count++];
			const CWeaponCycle& cycle = player->weapons[player->weapon];
			out.id = player->id;
			out.lastSeq = player->lastSeq;
			out.x = (float)player->move.location.x;
			out.y = (float)player->move.location.y;
			out.z = (float)player->move.location.z;
			out.vx = (float)player->move.velocity.x;
			out.vy = (float)player->move.velocity.y;
			out.vz = (float)player->move.velocity.z;
			out.yaw = player->yaw;
			out.pitch = player->pitch;
			out.hp = player->hp;
			out.magazine = (int16_t)cycle.GetMagazine();
			out.spare = (int16_t)cycle.GetSpare();
			out.flags = 0;
			if (player->move.mode == EMoveMode::Falling) out.flags |= wire::STATE_FALLING;
			if (cycle.IsReloading()) out.flags |= wire::STATE_RELOADING;
			if (player->dead) out.flags |= wire::STATE_DEAD;
			if (IsImmune(*player)) out.flags |= wire::STATE_IMMUNE;
			if (player->moveLockInputs > 0) out.flags |= wire::STATE_MOVE_LOCKED;
			if (player->ads.active) out.flags |= wire::STATE_ADS;
			if (player->move.crouched) out.flags |= wire::STATE_CROUCHED;
			out.weapon = player->weapon;
			out.quickSlots = player->loadout.PackSlots();
			out.team = player->team;
			out.moveX = player->dead ? (int8_t)0 : player->moveX;
			out.moveY = player->dead ? (int8_t)0 : player->moveY;
		}
		const int headSize = (int)(offsetof(wire::State, players) + sizeof(wire::PlayerState) * packet.count);
		int size = headSize;
		const auto animOf = [this](const Player& player) -> const wire::AnimStateWire& { return player.rewindHistory[_tick % player.rewindHistory.size()].anim; };
		if (_animReady)
		{
			for (const std::unique_ptr<Player>& player : _players) size += (int)wire::AnimTailSize(animOf(*player));
		}
		packet.h.size = (uint16_t)size;
		packet.h.type = (uint16_t)wire::Msg::S2C_STATE;
		if (_animReady)
		{
			// 꼬리 = 사람마다 앞부분과 같은 차례로 — 앞부분만 읽는 클라는 count 까지만 보므로 깨지지 않는다
			char bytes[wire::kMaxStatePacketSize];
			std::memcpy(bytes, &packet, (size_t)headSize);
			size_t at = (size_t)headSize;
			for (const std::unique_ptr<Player>& player : _players) at += wire::WriteAnimTail(animOf(*player), bytes + at);
			SendToAll(bytes, size);
			_stats.stateBytesMax = std::max(_stats.stateBytesMax, (uint64_t)size);
		}
		else
		{
			SendToAll(&packet, size);
		}
		_stats.statesSent += packet.count;
	}

	void CRoom::SendToAll(const void* data, int size)
	{
		for (const std::unique_ptr<Player>& player : _players)
		{
			_sender.SendToSession(player->session, data, size);
		}
	}

	void CRoom::Tick()
	{
		++_tick;
		++_stats.ticks;
		TickPhase();
		if (_phase == wire::EMatchPhase::Finished) return;
		TickRespawns();
		TickClaims();
		using Clock = std::chrono::steady_clock;
		const Clock::time_point begin = Clock::now();
		for (const std::unique_ptr<Player>& player : _players)
		{
			TickPlayerInputs(*player);
		}
		TickPads();	// Lyra 도 이동(겹침 시작 집기) 뒤에 타이머(다시 켜짐 · 다시 검사)를 돌린다
		if (_allowDebug) LogLoadouts();		// 부활 · 입력 · 받침이 다 돈 뒤 — 이 틱에 바뀐 가방을 한 줄로
		UpdateAnimStates();
		RecordRewindSamples();
		const Clock::time_point inputsEnd = Clock::now();
		ResolveShots();
		const Clock::time_point shotsEnd = Clock::now();
		SendState();
		const Clock::time_point stateEnd = Clock::now();
		_stats.timeInputsNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(inputsEnd - begin).count();
		_stats.timeShotsNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(shotsEnd - inputsEnd).count();
		_stats.timeStateNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(stateEnd - shotsEnd).count();
	}
}
