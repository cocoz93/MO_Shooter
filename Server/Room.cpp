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

		// 시선 허용 범위 — 다리는 yaw ±180·pitch ±90 안에서 보내고, 봇은 조준 오차로 각각 1도까지 넘는다
		constexpr float kMaxAbsYawDegrees = 360.f;
		constexpr float kMaxAbsPitchDegrees = 95.f;

		// 사격 시작점 허용 범위 — Lyra 기록의 최대(내 위치에서 92.5cm, 수평 60.6cm)에 이동 한 틱(10cm)과 여유를 더한 값
		// 조준 카메라를 넣어 시선 ±89도를 훑은 판(C-5 `c5sweep`)의 최대는 100.1cm(조준 · 시선 −10~0도) → 한 틱 더해 110.1, 그대로 둔다
		constexpr double kAimMaxDistance = 120.0;
		// 수평만 따로 묶지 않는다 — 72도쯤 내려다보면 정직한 값도 수평 91.8cm 까지 나온다(실측 d6). 간격이 수평으로 눕기 때문
		constexpr double kAimFallbackHeight = 60.0;		// 범위 밖일 때 쓰는 시작점 — 내 위치에서 이만큼 위
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

	ERewindKind ClassifyRewind(uint32_t nowTick, uint32_t viewTick, float viewAlpha, uint32_t limitTicks)
	{
		if (viewTick == 0) return ERewindKind::NoView;
		if (viewTick > nowTick || (viewTick == nowTick && viewAlpha > 0.f)) return ERewindKind::Future;
		if (nowTick - viewTick > limitTicks) return ERewindKind::Clamped;
		return ERewindKind::Ok;
	}

	CRoom::CRoom(const CMapCollision& map, const MoveParams& params, const MatchRules& rules, uint32_t seed, bool allowDebug, IRoomSender& sender, uint32_t roomId, const RewindOptions& rewind, const AnimOptions& anim)
		: _map(map), _params(params), _rules(rules), _allowDebug(allowDebug), _sender(sender), _roomId(roomId), _rewind(rewind), _anim(anim), _moveSim(map, _params), _spawnRng(seed + 1)
	{
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
			for (const std::unique_ptr<Player>& player : _players)
			{
				player->kills = player->deaths = 0;
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
		player.jumpHeld = false;
		player.ads.Reset();
		player.aimCamera.Reset();
		player.yaw = slot.point.yawDegrees;
		player.pitch = 0;
		player.weapon = 0;
		if (_animReady)
		{
			// 부활하면 처음 무기(권총)를 든다 — 자세도 그 묶음으로 처음부터, 부활 몽타주를 튼다
			player.anim.Init(*_anim.clips, AnimBundleOf(player.weapon));
			player.animWeapon = player.weapon;
			DropAnimEvents(player);
			PushAnimEvent(player, EAnimEvent::Spawn, player.weapon);
		}
		for (size_t i = 0; i < player.weapons.size(); ++i)
		{
			player.weapons[i].Init((EWeapon)i);		// 부활하면 탄을 다시 채운다 — 무기 줍기가 5단계라 처음 든 셋 그대로
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
		player.jumpHeld = false;
		player.hp = place.hp;
		player.dead = false;
		player.immuneUntilTick = 0;
		player.moveLockInputs = 0;
		player.lastSeq = place.seq;
		if (_animReady)
		{
			// 순간이동 + 멈춤 = 판 시작과 같다 — 자세를 든 무기 묶음으로 처음부터(경로 봇이 무기를 든 뒤 옮기면 서기 시계가 옮긴 틱에서 시작).
			// 이 틱에 앞서 쌓인 사건(무기 들기의 꺼내기 등)도 버린다 — 몽타주 없는 서기에서 시작
			player.anim.Init(*_anim.clips, AnimBundleOf(player.weapon));
			player.animWeapon = player.weapon;
			DropAnimEvents(player);
		}
	}

	void CRoom::ApplyInput(Player& player, const wire::Input& input)
	{
		player.lastSeq = input.seq;
		if (player.dead)
		{
			++_stats.inputsWhileDead;
			return;
		}
		++_stats.inputsApplied;
		player.fireGate.NextInput();

		if (input.weapon >= (uint8_t)EWeapon::Count)
		{
			++_stats.weaponIdRejected;
		}
		else if (input.weapon != player.weapon)
		{
			player.weapon = input.weapon;
			player.weapons[player.weapon].Equip();
			++_stats.weaponChanges;		// 자세 묶음은 이 틱의 UpdateAnimStates 가 따라 바꾼다
			PushAnimEvent(player, EAnimEvent::Swap, player.weapon);
		}

		const bool jump = (input.buttons & wire::BUTTON_JUMP) != 0;
		if (jump && !player.jumpHeld) _moveSim.PressJump(player.move);
		else if (!jump && player.jumpHeld) _moveSim.ReleaseJump(player.move);
		player.jumpHeld = jump;

		Vec3 moveInput(input.moveX, input.moveY, 0);
		const double moveLength = Length(moveInput);
		if (moveLength > 1.0)
		{
			if (moveLength > 1.001) ++_stats.movesOverLength;
			moveInput = moveInput * (1.0 / moveLength);
		}
		if (player.moveLockInputs > 0)
		{
			--player.moveLockInputs;	// GA_SpawnEffect 의 이동 입력 막음 — 점프·사격은 안 막힌다
			moveInput = Vec3();
		}
		player.moveX = (int8_t)std::lround(moveInput.x * 127.0);
		player.moveY = (int8_t)std::lround(moveInput.y * 127.0);
		// 조준 — 이 입력의 버튼이 이 입력의 걸음부터 먹는다(D61). 이동 막음 · 경기 끝에도 버튼을 따른다(예측이 단계를 몰라 맞춰 둔다, D61)
		player.ads.Apply((input.buttons & wire::BUTTON_ADS) != 0);
		if (player.ads.active) ++_stats.adsInputs;
		player.move.maxSpeed = AdsWalkSpeed(_params.maxWalkSpeed, player.ads.active);
		const Vec3 locationBeforeMove = player.move.location;
		_moveSim.Tick(player.move, moveInput, kTickSeconds);

		player.yaw = input.yaw;
		player.pitch = input.pitch;

		if (player.move.location.z < _rules.fallKillZ)
		{
			++_stats.falls;
			KillPlayer(player, nullptr, player.weapon, false);
			return;
		}

		WeaponMotion motion;
		motion.speed = (float)Length(player.move.velocity);
		motion.falling = player.move.mode == EMoveMode::Falling;
		motion.aimingAlpha = player.aimCamera.GetAimingAlpha();		// 앞 입력의 카메라 갱신 결과 — Lyra 무기 틱은 카메라 갱신보다 먼저 돈다(C-1 실측)
		WeaponInput weaponInput;
		weaponInput.fireHeld = (input.buttons & wire::BUTTON_FIRE) != 0;
		weaponInput.reloadPressed = (input.buttons & wire::BUTTON_RELOAD) != 0;
		if (_phase == wire::EMatchPhase::PostGame)
		{
			if (weaponInput.fireHeld) ++_stats.firesBlockedPostGame;
			weaponInput = WeaponInput();	// GE_BlockAbilityInput
		}
		CWeaponCycle& cycle = player.weapons[player.weapon];
		if (weaponInput.fireHeld && player.fireGate.IsBlocked((EWeapon)player.weapon, cycle.IsFireActive()))
		{
			++_stats.firesRejectedSwap;
			weaponInput.fireHeld = false;
		}
		const WeaponEvents events = cycle.Tick(kTickSeconds, weaponInput, motion);
		player.aimCamera.Tick(player.ads.active, kTickSeconds);		// 이 입력의 조준으로 카메라를 굴린다 — 다음 입력의 퍼짐이 읽는다
		if (weaponInput.fireHeld && !events.fired)
		{
			if (cycle.GetMagazine() == 0) ++_stats.firesEmpty;
			else ++_stats.firesTooSoon;
		}
		if (events.fired)
		{
			if (motion.aimingAlpha > 0.f) ++_stats.shotsAimed;
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
			start = location + Vec3(0, 0, kAimFallbackHeight);
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
			if (player.jumpHeld) idle.buttons |= wire::BUTTON_JUMP;
			if (player.ads.held) idle.buttons |= wire::BUTTON_ADS;		// 끊긴 틱에 조준이 풀렸다 다시 켜지지 않게
			ApplyInput(player, idle);
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

		wire::Kill kill = wire::Kill();
		kill.h.size = (uint16_t)sizeof(kill);
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
		SendToAll(&kill, (int)sizeof(kill));
		_sender.OnKillRecorded(kill);

		if (scored)
		{
			const uint16_t score = attacker->team == wire::kTeamA ? _teamScoreA : _teamScoreB;
			if (score >= _targetScore && _targetScore > 0) HandleVictory(attacker->team);
		}
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
		out.pitch = std::min(std::max(NormalizeAxis(player.pitch), -90.f), 90.f);
		if (out.falling)
		{
			out.groundDistance = kGroundTraceDistance;
			Vec3 end = move.location;
			end.z -= kGroundTraceDistance + _params.capsuleHalfHeight;
			RayHit hit;
			if (_map.Raycast(move.location, end, MAP_BLOCK_MOVE, hit)) out.groundDistance = std::max((float)hit.distance - _params.capsuleHalfHeight, 0.f);
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
		_animCodecs[from.animWeapon].FromWire(JudgeAnim(from.anim, _rewind.montageShiftTicks, shiftedFrom), from.pitch, input);
		evaluator.EvaluateLocal(input, _localFrom.data());
		const BoneTransform* local = _localFrom.data();
		if (alpha > 0.f)
		{
			const RewindSample& to = owner.rewindHistory[(tick + 1) % size];
			AnimPoseInput nextInput;
			_animCodecs[to.animWeapon].FromWire(JudgeAnim(to.anim, _rewind.montageShiftTicks, shiftedTo), to.pitch, nextInput);
			_poseEvaluators[to.animWeapon].EvaluateLocal(nextInput, _localTo.data());
			evaluator.BlendLocal(_localFrom.data(), _localTo.data(), alpha, _localBlend.data());
			local = _localBlend.data();
		}
		evaluator.BodiesFromLocal(local, cached.bodies.data());
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
				std::printf("REWIND seq=%u shooter=%u target=%u now=%u viewTick=%u alpha=%.4f x=%.3f y=%.3f z=%.3f yaw=%.3f animWeapon=%u montages=%u ads=%u\n", shot.seq, shot.shooter, player->id, _tick, bodyTick, bodyAlpha, body.center.x, body.center.y, body.center.z, body.yawDegrees, (unsigned)poseSample.animWeapon, (unsigned)poseMontages, (unsigned)poseAds);
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
			out.weapon = player->weapon;
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
