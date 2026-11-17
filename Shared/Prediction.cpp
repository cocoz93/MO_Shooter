#include "Prediction.h"

#include <cmath>

namespace mo
{
	namespace
	{
		constexpr float kTick = 1.f / (float)wire::kTickRate;

		bool SameForce(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

		double Length(const Vec3& v)
		{
			return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		}
	}

	CSelfPrediction::CSelfPrediction(CMoveSim& sim, const MatchRules& rules, float maxWalkSpeed)
		: _sim(sim), _rules(rules), _maxWalkSpeed(maxWalkSpeed)
	{
		for (size_t i = 0; i < _weapons.size(); ++i) _weapons[i].Init((EWeapon)i);
	}

	void CSelfPrediction::AddInput(wire::Input& input, const Vec3* meleePush)
	{
		input.seq = ++_seq;
		Slot& slot = _ring[input.seq % kRingSize];
		slot.input = input;
		slot.seq = input.seq;
		slot.valid = false;
		slot.meleePush = false;
		slot.meleePushForce = Vec3();
		slot.meleeStarted = false;
		slot.meleeServer = false;
		_lastWeaponEvents = WeaponEvents();
		if (_waitingSpawn) return;
		Equip(input);
		StepMelee(slot);
		if (meleePush)
		{
			if (slot.meleeStarted)
			{
				slot.meleePush = true;
				slot.meleePushForce = *meleePush;
				++_meleeCounters.pushesGiven;
			}
			else ++_meleeCounters.pushesIgnored;
		}
		ApplyMove(slot);
		ApplyWeapon(slot);
		RememberMove(slot);
		RememberWeapon(slot);
	}

	void CSelfPrediction::OnSpawn(double x, double y, double z, uint32_t lastSeq)
	{
		_waitingSpawn = false;
		_compareFromSeq = lastSeq;
		_move = MoveState();
		_move.location = Vec3(x, y, z);
		_move.maxSpeed = _maxWalkSpeed;
		_sim.RefreshFloor(_move);
		_jumpHeld = false;
		_lockInputs = SpawnMoveLockInputs(_rules);
		_ads.Reset();
		_aimCamera.Reset();
		_weapon = 0;
		for (size_t i = 0; i < _weapons.size(); ++i) _weapons[i].Init((EWeapon)i);
		_fireGate = CWeaponFireGate();
		_melee.Reset();		// 죽음이 능력을 끈다 — 새 몸은 꺼진 채 시작한다
		RememberMove(_spawnBase);
		_meleeAckedSeq = lastSeq;

		// 서버는 이 번호 뒤의 입력을 새 자리에서 쓴다 — 여기도 똑같이 다시 돌린다.
		// 앞 목숨 자리에서 정한 근접 밀기는 버린다 — 새 자리 판정은 서버 결과(OnMeleeResult)가 채운다
		for (uint32_t seq = lastSeq + 1; seq <= _seq; ++seq)
		{
			Slot& slot = _ring[seq % kRingSize];
			if (slot.seq != seq) continue;
			slot.meleePush = false;
			slot.meleePushForce = Vec3();
			slot.meleeServer = false;
			Equip(slot.input);
			StepMelee(slot);
			ApplyMove(slot);
			ApplyWeapon(slot);
			RememberMove(slot);
			RememberWeapon(slot);
		}
	}

	const CSelfPrediction::Slot* CSelfPrediction::FindComparable(const wire::PlayerState& server) const
	{
		if (server.flags & wire::STATE_DEAD) return nullptr;
		if (server.lastSeq <= _compareFromSeq) return nullptr;	// 시작 자리에 막 섰고 그 뒤 입력이 아직 안 돌았다
		const Slot& slot = _ring[server.lastSeq % kRingSize];
		if (slot.seq != server.lastSeq || !slot.valid) return nullptr;
		return &slot;
	}

	CSelfPrediction::ReconcileResult CSelfPrediction::Reconcile(const wire::PlayerState& server, double tolerance)
	{
		ReconcileResult result;
		if (!_waitingSpawn && !(server.flags & wire::STATE_DEAD)) AckMelee(server.lastSeq);
		const Slot* found = FindComparable(server);
		if (!found || _waitingSpawn) return result;
		result.compared = true;

		const Vec3 serverLocation(server.x, server.y, server.z);
		const Vec3 serverVelocity(server.vx, server.vy, server.vz);
		// 슬롯은 double 이고 와이어는 float 라 float 끼리 맞춘다
		const Vec3 slotLocation((float)found->move.location.x, (float)found->move.location.y, (float)found->move.location.z);
		const Vec3 slotVelocity((float)found->move.velocity.x, (float)found->move.velocity.y, (float)found->move.velocity.z);
		result.posDiff = Length(serverLocation - slotLocation);
		result.velDiff = Length(serverVelocity - slotVelocity);
		const bool serverFalling = (server.flags & wire::STATE_FALLING) != 0;
		const bool slotFalling = found->move.mode == EMoveMode::Falling;
		const bool serverCrouched = (server.flags & wire::STATE_CROUCHED) != 0;
		if (result.posDiff <= tolerance && result.velDiff <= tolerance && serverFalling == slotFalling && serverCrouched == found->move.crouched) return result;

		result.corrected = true;
		Slot& slot = _ring[server.lastSeq % kRingSize];
		_move = slot.move;
		_move.location = serverLocation;
		_move.velocity = serverVelocity;
		// 서버 값으로 덮은 속도는 밀기의 덧붙이기 전 속도로 옮기지 않는다 — ClientAdjustPosition 이 LastUpdateVelocity 도 같이 덮는 것과 같다(원본 11382).
		// 밀기 상태(rootForce)는 이 슬롯의 내 값 그대로다 — 밀기는 서버 근접 결과(OnMeleeResult)가 이 비교 앞에 서버 값으로 고쳐 다시 돌려 두므로 와이어에 따로 싣지 않는다(E-3, D74)
		_move.lastUpdateVelocity = _move.velocity;
		_move.mode = serverFalling ? EMoveMode::Falling : EMoveMode::Walking;
		// 앉음은 캡슐 높이라 위치와 같이 서버 값으로. 앉으려 함(wantsToCrouch)은 와이어에 없다 — 앉음이 달랐을 때만 앉음과 같게 둔다(제 판단:
		// 같은 입력을 돈 두 쪽이 앉음만 다르면 앉기 키를 받은 때의 땅 · 공중이 달랐던 것이고, 서버는 그 바람대로 앉거나 섰다고 본다)
		if (_move.crouched != serverCrouched) _move.wantsToCrouch = serverCrouched;
		_move.crouched = serverCrouched;
		_jumpHeld = slot.jumpHeld;
		_lockInputs = slot.lockInputs;
		_ads = slot.ads;
		_sim.RefreshFloor(_move);
		slot.move = _move;
		for (uint32_t seq = server.lastSeq + 1; seq <= _seq; ++seq)
		{
			Slot& later = _ring[seq % kRingSize];
			if (later.seq != seq) continue;
			ApplyMove(later);
			RememberMove(later);
			later.valid = true;
		}
		return result;
	}

	void CSelfPrediction::Equip(const wire::Input& input)
	{
		if (input.weapon < (uint8_t)EWeapon::Count && input.weapon != _weapon)
		{
			_weapon = input.weapon;
			_weapons[_weapon].Equip();
		}
	}

	void CSelfPrediction::OnMeleeResult(uint32_t seq, bool push, const Vec3& force)
	{
		Slot& slot = _ring[seq % kRingSize];
		if (_waitingSpawn || slot.seq != seq || seq <= _compareFromSeq || !slot.valid)
		{
			++_meleeCounters.resultsNoSlot;
			return;
		}
		++_meleeCounters.results;
		slot.meleeServer = true;
		if (!slot.meleeStarted) ++_meleeCounters.startMismatches;
		const Vec3 serverForce = push ? force : Vec3();
		if (slot.meleePush == push && SameForce(slot.meleePushForce, serverForce)) return;
		if (slot.meleePush != push) ++_meleeCounters.pushHitFixes;
		else ++_meleeCounters.pushForceFixes;
		slot.meleePush = push;
		slot.meleePushForce = serverForce;
		RerunMovesFrom(seq);
	}

	void CSelfPrediction::AckMelee(uint32_t lastSeq)
	{
		if (_waitingSpawn) return;		// 부활을 기다리는 동안은 칸이 없다 — OnSpawn 이 본 번호를 부활 번호로 옮긴다
		if (lastSeq > _seq) lastSeq = _seq;
		uint32_t rerunFrom = 0;
		for (uint32_t seq = _meleeAckedSeq + 1; seq <= lastSeq; ++seq)
		{
			Slot& slot = _ring[seq % kRingSize];
			if (slot.seq != seq || !slot.meleeStarted || slot.meleeServer) continue;
			++_meleeCounters.unconfirmed;
			if (!slot.meleePush) continue;
			slot.meleePush = false;
			slot.meleePushForce = Vec3();
			if (rerunFrom == 0) rerunFrom = seq;
		}
		if (lastSeq > _meleeAckedSeq) _meleeAckedSeq = lastSeq;
		if (rerunFrom != 0) RerunMovesFrom(rerunFrom);
	}

	bool CSelfPrediction::RerunMovesFrom(uint32_t seq)
	{
		const Slot* base = nullptr;
		if (seq == _compareFromSeq + 1) base = &_spawnBase;
		else
		{
			const Slot& before = _ring[(seq - 1) % kRingSize];
			if (before.seq == seq - 1 && before.valid) base = &before;
		}
		if (!base)
		{
			++_meleeCounters.rerunsMissed;
			return false;
		}
		// Reconcile 의 다시 돌리기와 같은 값을 잇는다 — 걸음 상태 · 점프 키 · 이동 막음 · 조준
		_move = base->move;
		_jumpHeld = base->jumpHeld;
		_lockInputs = base->lockInputs;
		_ads = base->ads;
		for (uint32_t at = seq; at <= _seq; ++at)
		{
			Slot& later = _ring[at % kRingSize];
			if (later.seq != at) continue;
			ApplyMove(later);
			RememberMove(later);
			later.valid = true;
		}
		return true;
	}

	// 근접 능력 — 서버 ApplyInput 과 같은 자리(무기 교체 뒤 · 걸음 앞). 경기 끝 단계는 모른다(서버만 막는다 — 결과가 안 와 AckMelee 가 지운다)
	void CSelfPrediction::StepMelee(Slot& slot)
	{
		_melee.NextInput();
		slot.meleeStarted = (slot.input.buttons & wire::BUTTON_MELEE) != 0 && _melee.TryActivate(GetWeaponDef((EWeapon)_weapon), kTick);
		if (slot.meleeStarted) ++_meleeCounters.started;
	}

	// Room.cpp ApplyInput 과 같은 순서
	void CSelfPrediction::ApplyMove(const Slot& slot)
	{
		const wire::Input& input = slot.input;
		const bool jump = (input.buttons & wire::BUTTON_JUMP) != 0;
		if (jump && !_jumpHeld)
		{
			_sim.RequestUnCrouch(_move);		// 점프 전 일어서기(LyraGameplayAbility_Jump.cpp:55)
			_sim.PressJump(_move);
		}
		else if (!jump && _jumpHeld) _sim.ReleaseJump(_move);
		_jumpHeld = jump;
		if ((input.buttons & wire::BUTTON_CROUCH) != 0) _sim.ToggleCrouch(_move);		// 이동 막음 중에도(서버와 같다)

		Vec3 moveInput(input.moveX, input.moveY, 0);
		const double moveLength = Length(moveInput);
		if (moveLength > 1.0) moveInput = moveInput * (1.0 / moveLength);
		if (_lockInputs > 0)
		{
			--_lockInputs;
			moveInput = Vec3();
		}
		if (_debugMoveScale != 1.0) moveInput = moveInput * _debugMoveScale;
		_ads.Apply((input.buttons & wire::BUTTON_ADS) != 0);
		_move.maxSpeed = AdsWalkSpeed(_maxWalkSpeed, _ads.active);
		// 근접 밀기는 걸음 바로 앞 — Lyra 는 능력이 켜지는 입력 처리에서 넣고 그 프레임 이동이 민다(E-2-1). 서버도 이 자리에서 넣는다(E-3)
		if (slot.meleePush) _sim.ApplyRootForce(_move, slot.meleePushForce, kMeleePushDuration);
		_sim.Tick(_move, moveInput, kTick);
	}

	void CSelfPrediction::ApplyWeapon(const Slot& slot)
	{
		const wire::Input& input = slot.input;
		WeaponMotion motion;
		motion.speed = (float)Length(_move.velocity);
		motion.falling = _move.mode == EMoveMode::Falling;
		motion.crouching = _move.crouched;
		motion.aimingAlpha = _aimCamera.GetAimingAlpha();
		WeaponInput weaponInput;
		weaponInput.fireHeld = (input.buttons & wire::BUTTON_FIRE) != 0;
		weaponInput.reloadPressed = (input.buttons & wire::BUTTON_RELOAD) != 0;
		weaponInput.meleeActive = _melee.IsActive();
		weaponInput.meleeStarted = slot.meleeStarted;
		// 서버와 같은 순서(Room.cpp ApplyInput)
		_fireGate.NextInput();
		if (weaponInput.fireHeld && _fireGate.IsBlocked((EWeapon)_weapon, _weapons[_weapon].IsFireActive())) weaponInput.fireHeld = false;
		_lastWeaponEvents = _weapons[_weapon].Tick(kTick, weaponInput, motion);
		_aimCamera.Tick(_ads.active, kTick);
		if (_lastWeaponEvents.fired) _fireGate.OnFired((EWeapon)_weapon, _weapons[_weapon].GetDef(), kTick);
	}

	void CSelfPrediction::RememberMove(Slot& slot) const
	{
		slot.move = _move;
		slot.jumpHeld = _jumpHeld;
		slot.lockInputs = _lockInputs;
		slot.ads = _ads;
	}

	void CSelfPrediction::RememberWeapon(Slot& slot) const
	{
		slot.valid = true;
		slot.magazine = (int16_t)_weapons[_weapon].GetMagazine();
		slot.spare = (int16_t)_weapons[_weapon].GetSpare();
		slot.weapon = _weapon;
		slot.reloading = _weapons[_weapon].IsReloading();
	}
}
