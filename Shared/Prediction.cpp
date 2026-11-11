#include "Prediction.h"

#include <cmath>

namespace mo
{
	namespace
	{
		constexpr float kTick = 1.f / (float)wire::kTickRate;

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

	void CSelfPrediction::AddInput(wire::Input& input)
	{
		input.seq = ++_seq;
		Slot& slot = _ring[input.seq % kRingSize];
		slot.input = input;
		slot.seq = input.seq;
		slot.valid = false;
		_lastWeaponEvents = WeaponEvents();
		if (_waitingSpawn) return;
		Equip(input);
		ApplyMove(input);
		ApplyWeapon(input);
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
		_weapon = 0;
		for (size_t i = 0; i < _weapons.size(); ++i) _weapons[i].Init((EWeapon)i);
		_fireGate = CWeaponFireGate();

		// 서버는 이 번호 뒤의 입력을 새 자리에서 쓴다 — 여기도 똑같이 다시 돌린다
		for (uint32_t seq = lastSeq + 1; seq <= _seq; ++seq)
		{
			Slot& slot = _ring[seq % kRingSize];
			if (slot.seq != seq) continue;
			Equip(slot.input);
			ApplyMove(slot.input);
			ApplyWeapon(slot.input);
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
		if (result.posDiff <= tolerance && result.velDiff <= tolerance && serverFalling == slotFalling) return result;

		result.corrected = true;
		Slot& slot = _ring[server.lastSeq % kRingSize];
		_move = slot.move;
		_move.location = serverLocation;
		_move.velocity = serverVelocity;
		_move.mode = serverFalling ? EMoveMode::Falling : EMoveMode::Walking;
		_jumpHeld = slot.jumpHeld;
		_lockInputs = slot.lockInputs;
		_sim.RefreshFloor(_move);
		slot.move = _move;
		for (uint32_t seq = server.lastSeq + 1; seq <= _seq; ++seq)
		{
			Slot& later = _ring[seq % kRingSize];
			if (later.seq != seq) continue;
			ApplyMove(later.input);
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

	// Room.cpp ApplyInput 과 같은 순서
	void CSelfPrediction::ApplyMove(const wire::Input& input)
	{
		const bool jump = (input.buttons & wire::BUTTON_JUMP) != 0;
		if (jump && !_jumpHeld) _sim.PressJump(_move);
		else if (!jump && _jumpHeld) _sim.ReleaseJump(_move);
		_jumpHeld = jump;

		Vec3 moveInput(input.moveX, input.moveY, 0);
		const double moveLength = Length(moveInput);
		if (moveLength > 1.0) moveInput = moveInput * (1.0 / moveLength);
		if (_lockInputs > 0)
		{
			--_lockInputs;
			moveInput = Vec3();
		}
		if (_debugMoveScale != 1.0) moveInput = moveInput * _debugMoveScale;
		_move.maxSpeed = _maxWalkSpeed;
		_sim.Tick(_move, moveInput, kTick);
	}

	void CSelfPrediction::ApplyWeapon(const wire::Input& input)
	{
		WeaponMotion motion;
		motion.speed = (float)Length(_move.velocity);
		motion.falling = _move.mode == EMoveMode::Falling;
		WeaponInput weaponInput;
		weaponInput.fireHeld = (input.buttons & wire::BUTTON_FIRE) != 0;
		weaponInput.reloadPressed = (input.buttons & wire::BUTTON_RELOAD) != 0;
		// 서버와 같은 순서(Room.cpp ApplyInput)
		_fireGate.NextInput();
		if (weaponInput.fireHeld && _fireGate.IsBlocked((EWeapon)_weapon, _weapons[_weapon].IsFireActive())) weaponInput.fireHeld = false;
		_lastWeaponEvents = _weapons[_weapon].Tick(kTick, weaponInput, motion);
		if (_lastWeaponEvents.fired) _fireGate.OnFired((EWeapon)_weapon, _weapons[_weapon].GetDef(), kTick);
	}

	void CSelfPrediction::RememberMove(Slot& slot) const
	{
		slot.move = _move;
		slot.jumpHeld = _jumpHeld;
		slot.lockInputs = _lockInputs;
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
