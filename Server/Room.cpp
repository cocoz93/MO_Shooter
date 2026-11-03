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

		Vec3 AimDirection(float yawDegrees, float pitchDegrees)
		{
			const double yaw = yawDegrees * kPi / 180.0;
			const double pitch = pitchDegrees * kPi / 180.0;
			return Vec3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
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

	CRoom::CRoom(const CMapCollision& map, const MoveParams& params, const MatchRules& rules, uint32_t seed, bool allowDebug, IRoomSender& sender, uint32_t roomId, const RewindOptions& rewind)
		: _map(map), _params(params), _rules(rules), _allowDebug(allowDebug), _sender(sender), _roomId(roomId), _rewind(rewind), _moveSim(map, _params), _rng(seed), _spawnRng(seed + 1)
	{
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
		if (!IsFiniteInput(input) || !IsViewInRange(input) || !IsViewAlphaInRange(input))
		{
			++_stats.inputsRejectedBadValue;
			return;
		}
		QueuedInput item;
		item.input = input;
		item.place = wire::DebugPlace();
		item.isPlace = false;
		PushQueue(*player, item, input.seq);
	}

	void CRoom::OnDebugPlace(int64_t session, const wire::DebugPlace& place)
	{
		if (!_allowDebug)
		{
			++_stats.debugPlacesRejected;
			return;
		}
		Player* player = Find(session);
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
		player.yaw = slot.point.yawDegrees;
		player.pitch = 0;
		player.weapon = 0;
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
		player.move.maxSpeed = _params.maxWalkSpeed;
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
		if (weaponInput.fireHeld && !events.fired)
		{
			if (cycle.GetMagazine() == 0) ++_stats.firesEmpty;
			else ++_stats.firesTooSoon;
		}
		if (events.fired)
		{
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
			ApplyInput(player, idle);
			--_stats.inputsApplied;
		}
	}

	// ─── 사격 ──────────────────────────────────────────────
	void CRoom::KillPlayer(Player& target, Player* attacker, uint8_t weapon, bool weakSpot)
	{
		target.dead = true;
		target.hp = 0;
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
			sample.alive = !player->dead;
		}
	}

	// Lyra 가 남의 캐릭터를 그리는 식과 같다(MOBridgeSubsystem::TickRemotes). false = 그 시점에 몸이 없었다(죽었거나 아직 안 들어옴)
	bool CRoom::GetRewoundBody(const Player& player, uint32_t viewTick, float viewAlpha, BodyPlacement& out) const
	{
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
		return true;
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
		for (const std::unique_ptr<Player>& player : _players)
		{
			if (player->id == shot.shooter)
			{
				shooter = player.get();
				continue;
			}
			if (player->dead) continue;		// 지금 죽어 있으면 그때 살아 있었어도 안 맞는다
			BodyPlacement now;
			now.center = player->move.location;
			now.yawDegrees = player->yaw;
			now.id = player->id;
			if (_rewind.shadow) _bodiesNow.push_back(now);
			BodyPlacement body = now;
			if (useHistory && !GetRewoundBody(*player, bodyTick, bodyAlpha, body)) continue;
			if (_rewind.log)
			{
				std::printf("REWIND seq=%u shooter=%u target=%u now=%u viewTick=%u alpha=%.4f x=%.3f y=%.3f z=%.3f yaw=%.3f\n", shot.seq, shot.shooter, player->id, _tick, bodyTick, bodyAlpha, body.center.x, body.center.y, body.center.z, body.yawDegrees);
			}
			_bodies.push_back(body);
			_bodyOwners.push_back(player.get());
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

		for (int i = 0; i < packet.count; ++i)
		{
			Vec3 dir = SpreadDirection(shot.aimDir, shot.spreadAngle, shot.spreadMultiplier, def.spreadExponent, _rng);
			if (_rewind.noSpread) dir = shot.aimDir;
			const Vec3 end = shot.start + dir * (double)def.maxDamageRange;
			BulletResult result;
			ResolveBullet(_map, _bodies.data(), _bodies.size(), shot.start, end, def.sweepRadius, result);

			++_stats.bullets;
			if (_rewind.shadow)
			{
				BulletResult now;
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
			out.weapon = player->weapon;
			out.team = player->team;
			out.moveX = player->dead ? (int8_t)0 : player->moveX;
			out.moveY = player->dead ? (int8_t)0 : player->moveY;
		}
		const int size = (int)(offsetof(wire::State, players) + sizeof(wire::PlayerState) * packet.count);
		packet.h.size = (uint16_t)size;
		packet.h.type = (uint16_t)wire::Msg::S2C_STATE;
		SendToAll(&packet, size);
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
