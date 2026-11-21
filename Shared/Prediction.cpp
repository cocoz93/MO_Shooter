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

		// 칸의 켜는 차례에서 key 의 자리(없으면 3 — 셋 다 지난 뒤)
		int AbilityPos(const CSelfPrediction::Slot& slot, uint8_t key)
		{
			for (int k = 0; k < slot.abilityCount; ++k)
			{
				if (slot.abilityOrder[k] == key) return k;
			}
			return 3;
		}

		// 서버가 켰다는 능력이 칸 차례에 없으면(내 입력엔 그 누름이 없었다 — 서버의 앞 입력이 달랐던 갈래, DashCheck predictFix serverStart · spawnBase)
		// 판 13 고정 차례(대시 → 수류탄 → 근접) 자리에 끼운다 — 서버가 어느 차례로 켰는지 모르니 차례 없는 입력과 같은 자리
		void AddServerAbility(CSelfPrediction::Slot& slot, uint8_t key)
		{
			if (AbilityPos(slot, key) < 3 || slot.abilityCount >= 3) return;
			const auto rank = [](uint8_t k) { return k == wire::PRESS_DASH ? 0 : (k == wire::PRESS_GRENADE ? 1 : 2); };
			int at = 0;
			while (at < slot.abilityCount && rank(slot.abilityOrder[at]) < rank(key)) ++at;
			for (int k = slot.abilityCount; k > at; --k) slot.abilityOrder[k] = slot.abilityOrder[k - 1];
			slot.abilityOrder[at] = key;
			++slot.abilityCount;
		}

		// key 차례에 대시가 누름을 막나 — 대시가 그보다 먼저면 능력 자리 뒤 값(이 입력에 켰으면 막음), 아니면 켜기 앞 값
		bool DashBlocksAt(const CSelfPrediction::Slot& slot, uint8_t key)
		{
			return AbilityPos(slot, wire::PRESS_DASH) < AbilityPos(slot, key) ? slot.dashBlocks : slot.dashBlocksBefore;
		}

		// key 차례에 근접이 켜져 있나 — 이 입력 앞부터 켜진 값에서 그보다 먼저 켠 능력을 차례로 탄다(근접 켬 → 켜짐 · 대시 켬 · 던짐 → 끊김).
		// 다시 고르기는 근접을 다시 안 고르므로 칸의 켬 · 던짐 값을 그대로 읽는다(StepAbilities 의 차례 고리와 같은 결과)
		bool MeleeActiveAt(const CSelfPrediction::Slot& slot, uint8_t key)
		{
			bool active = slot.meleeWasActive;
			for (int k = 0; k < slot.abilityCount && slot.abilityOrder[k] != key; ++k)
			{
				switch (slot.abilityOrder[k])
				{
				case wire::PRESS_MELEE:
					if (slot.meleeStarted) active = true;
					break;
				case wire::PRESS_DASH:
					if (slot.dashStarted) active = false;
					break;
				case wire::PRESS_GRENADE:
					if (slot.grenadeThrown && !slot.grenadeDead) active = false;
					break;
				default:
					break;
				}
			}
			return active;
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
		slot.meleeWasActive = false;
		slot.dashStarted = false;
		slot.dashDir = 0;
		slot.dashForce = Vec3();
		slot.dashBlocks = false;
		slot.dashBlocksFire = false;
		slot.dashCutMelee = false;
		slot.dashServer = false;
		slot.dashDenied = false;
		slot.grenadeThrown = false;
		slot.grenadeCutMelee = false;
		slot.grenadeDead = _waitingSpawn;
		slot.grenadeTicks = true;
		slot.grenadeServer = false;
		slot.grenadeDenied = false;
		slot.dashBlocksBefore = false;
		// 켜는 차례는 입력으로 정해진다(서버 ApplyInput 과 같은 함수) — 부활 대기 칸도 채워 다시 고르기가 남은 값을 안 읽게
		slot.abilityCount = (uint8_t)wire::AbilityOrder(input.buttons, input.pressOrder, slot.abilityOrder, slot.reloadBeforeGrenade);
		if ((input.buttons & wire::BUTTON_GRENADE) != 0) ++_grenadeCounters.presses;
		_lastWeaponEvents = WeaponEvents();
		if (_waitingSpawn)
		{
			// 서버 죽은 갈래(Room.cpp ApplyInput) — 수류탄 쿨다운은 죽은 입력에도 준다(G-0-b2). 누름은 죽음으로 안 켜진다
			_grenade.NextInput();
			if ((input.buttons & wire::BUTTON_GRENADE) != 0) (void)_grenade.TryActivate(true, false);
			slot.grenadeAfter = _grenade;
			return;
		}
		Equip(input);
		StepAbilities(slot, true);
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
		RememberDash(slot, ApplyMove(slot));
		ApplyWeapon(slot, _melee.IsActive());
		RememberMove(slot);
		RememberWeapon(slot, _melee.IsActive());
	}

	void CSelfPrediction::OnSpawn(double x, double y, double z, uint32_t lastSeq, float yaw)
	{
		_waitingSpawn = false;
		_compareFromSeq = lastSeq;
		_placeActive = false;		// 이 번호 칸은 부활 칸이다(죽음 옮기기 번호와 같을 수 있다 — DEBUG_PLACE_KILL 뒤 입력 없이 부활)
		_move = MoveState();
		_move.location = Vec3(x, y, z);
		_move.maxSpeed = _maxWalkSpeed;
		_sim.RefreshFloor(_move);
		_jump.Reset();
		_meleePushId = 0;
		_lockInputs = SpawnMoveLockInputs(_rules);
		_ads.Reset();
		_aimCamera.Reset();
		_loadout.Reset(_rules.startAllWeapons, _weapons);		// 서버 SpawnPlayer 와 같다 — [권총, 빈, 빈](시험 옵션이면 세 무기)
		_weapon = _loadout.Held();
		_fireGate = CWeaponFireGate();
		_melee.Reset();		// 죽음이 능력을 끈다 — 새 몸은 꺼진 채 시작한다
		_dash.Reset();		// 서버 SpawnPlayer 와 같다 — 쿨다운도 지우고, 새 몸의 「앞 입력」 = 이동 없음 · 놓인 요
		_dash.RememberInput(Vec3(), yaw);
		RememberMove(_spawnBase);
		_spawnBase.dashAfter = _dash;
		SaveWeapon(_spawnWeapon, lastSeq, false);
		_meleeAckedSeq = lastSeq;
		// 죽은 줄 모르고 켠 대시 — 서버는 죽은 몸 입력이라 안 켰고(소식 없음), 확인은 부활 대기 중이라 AckDash 가 안 봤다. 아래에서 확인 번호를 옮기며 버리므로
		// 그 몫만 센다(행동은 그대로 — 새 몸이 다시 돈다). 수류탄 unconfirmedDead 와 같은 몫(G-3-c 봇 판에서 드러남)
		for (uint32_t seq = _dashAckedSeq + 1; seq <= lastSeq && seq <= _seq; ++seq)
		{
			const Slot& slot = _ring[seq % kRingSize];
			if (slot.seq == seq && slot.dashStarted && !slot.dashServer) ++_dashCounters.unconfirmedDead;
		}
		_dashAckedSeq = lastSeq;
		// 수류탄은 부활이 안 지운다(서버 SpawnPlayer 도 안 건드림, G-0-b2) — 부활 번호까지는 서버가 (죽은 몸으로) 돌렸으니 소식으로 정하고,
		// 그 칸 상태에서 뒤 입력을 새 몸으로 다시 돈다(아래 StepAbilities). 걸음은 아래 고리가 어차피 다시 돈다
		bool grenadeCutChanged = false;
		(void)MarkGrenadeAcked(lastSeq, true, grenadeCutChanged);		// 부활 번호까지는 서버가 죽은 몸으로 돌렸다
		const Slot& grenadeBase = _ring[lastSeq % kRingSize];
		if (grenadeBase.seq == lastSeq) _grenade = grenadeBase.grenadeAfter;

		// 서버는 이 번호 뒤의 입력을 새 자리에서 쓴다 — 여기도 똑같이 다시 돌린다.
		// 앞 목숨 자리에서 정한 근접 밀기는 버린다 — 새 자리 판정은 서버 결과(OnMeleeResult)가 채운다
		for (uint32_t seq = lastSeq + 1; seq <= _seq; ++seq)
		{
			Slot& slot = _ring[seq % kRingSize];
			if (slot.seq != seq) continue;
			slot.meleePush = false;
			slot.meleePushForce = Vec3();
			slot.meleeServer = false;
			slot.dashServer = false;		// 앞 목숨에서 받은 대시 소식 · 확인도 버린다 — 새 몸에서 다시 고른다
			slot.dashDenied = false;
			slot.grenadeDead = false;		// 서버는 부활 번호 뒤 입력을 새 몸으로 쓴다 — 대기 중 죽은 갈래로 센 칸을 산 갈래로 다시 고른다
			Equip(slot.input);
			const bool dashStartedBefore = slot.dashStarted;
			const bool grenadeThrownBefore = slot.grenadeThrown;
			StepAbilities(slot, false);
			if (slot.dashStarted != dashStartedBefore) ++(slot.dashStarted ? _dashCounters.respawnGained : _dashCounters.respawnLost);
			if (slot.grenadeThrown != grenadeThrownBefore) ++(slot.grenadeThrown ? _grenadeCounters.respawnThrown : _grenadeCounters.respawnLost);
			RememberDash(slot, ApplyMove(slot));
			ApplyWeapon(slot, _melee.IsActive());
			RememberMove(slot);
			RememberWeapon(slot, _melee.IsActive());
		}
	}

	uint32_t CSelfPrediction::OnPlace(double x, double y, double z, uint8_t ammoWeapon, int16_t magazine, int16_t spare)
	{
		const uint32_t seq = ++_seq;
		Slot& slot = _ring[seq % kRingSize];
		slot = Slot();
		slot.seq = seq;
		// 서버 ApplyPlace 와 같은 차례 — 걸음 새로(멈춤 · 걷기) · 바닥 다시 찾기 · 점프 키 · 이동 막음 지움 · 죽음 지움, 그 뒤 탄 정하기
		_waitingSpawn = false;
		_compareFromSeq = seq;
		_move = MoveState();
		_move.location = Vec3(x, y, z);
		_move.mode = EMoveMode::Walking;
		_move.maxSpeed = _maxWalkSpeed;
		_sim.RefreshFloor(_move);
		_jump.Reset();
		_meleePushId = 0;		// 서버 ApplyPlace 와 같다 — 힘 묶음을 새로 만들었다. 대시 능력은 서버도 안 건드린다(켜짐 · 쿨다운 · 앞 입력 그대로)
		_lockInputs = 0;
		// 옮기기 앞 번호의 근접 결과는 부활처럼 닫는다 — 그 밀기를 고쳐 옮기기 앞 걸음부터 다시 돌리면 옮긴 자리를 덮는다(OnMeleeResult 도 이 번호 앞은 안 받는다)
		_meleeAckedSeq = seq;
		_dashAckedSeq = seq;		// 대시 소식도 같다(OnDashResult)
		RememberMove(_spawnBase);
		_spawnBase.dashAfter = _dash;
		slot.dashAfter = _dash;
		slot.grenadeTicks = false;		// 서버 ApplyPlace 는 수류탄 입력 수를 안 센다 — 사슬만 잇는다
		slot.grenadeAfter = _grenade;
		// 서버는 겹침(받침 집기)을 탄 정하기 앞에 본다 — 이 번호로 집은 소식의 앞 칸은 탄 정하기 앞 무기 쪽, 집은 뒤 탄 정하기를 다시 건다(OnPickup)
		const bool meleeActive = _melee.IsActive();
		SaveWeapon(_placeWeapon, seq, meleeActive);
		_placeActive = true;
		_placeAmmoWeapon = ammoWeapon;
		_placeMagazine = magazine;
		_placeSpare = spare;
		if (ammoWeapon < (uint8_t)EWeapon::Count) _weapons[ammoWeapon].SetAmmoForCheck(magazine, spare);
		SaveWeapon(_spawnWeapon, seq, meleeActive);		// 다음 입력이 이어 받는 칸 = 탄 정하기 뒤
		RememberWeapon(slot, meleeActive);
		slot.valid = false;		// 이 번호의 서버 상태는 견주지 않는다(FindComparable 이 _compareFromSeq 로도 거른다)
		return seq;
	}

	const CSelfPrediction::Slot* CSelfPrediction::FindComparable(const wire::PlayerState& server) const
	{
		if (server.flags & wire::STATE_DEAD) return nullptr;
		if (server.lastSeq <= _compareFromSeq) return nullptr;	// 시작 자리에 막 섰고 그 뒤 입력이 아직 안 돌았다
		const Slot& slot = _ring[server.lastSeq % kRingSize];
		if (slot.seq != server.lastSeq || !slot.valid) return nullptr;
		return &slot;
	}

	bool CSelfPrediction::RerunUnconfirmed(uint32_t confirmedSeq)
	{
		if (_waitingSpawn) return false;
		const uint32_t from = std::max(confirmedSeq, _compareFromSeq) + 1;	// 부활 앞 입력은 옛 몸이라 안 돌린다(부활 칸이 앞 칸)
		if (from > _seq) return false;
		return RerunMovesFrom(from);
	}

	CSelfPrediction::ReconcileResult CSelfPrediction::Reconcile(const wire::PlayerState& server, double tolerance)
	{
		ReconcileResult result;
		if (!_waitingSpawn && !(server.flags & wire::STATE_DEAD))
		{
			AckMelee(server.lastSeq);
			AckDash(server.lastSeq);
		}
		AckGrenade(server.lastSeq, (server.flags & wire::STATE_DEAD) != 0);		// 부활 대기 · 죽음 중에도 — 서버는 죽은 입력에도 수류탄 쿨다운을 센다(대시 확인 뒤 — 대시 막기가 고쳐진 칸으로 고른다)
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
		_jump = slot.jump;
		_meleePushId = slot.meleePushId;
		_lockInputs = slot.lockInputs;
		_ads = slot.ads;
		_sim.RefreshFloor(_move);
		slot.move = _move;
		for (uint32_t seq = server.lastSeq + 1; seq <= _seq; ++seq)
		{
			Slot& later = _ring[seq % kRingSize];
			if (later.seq != seq) continue;
			(void)ApplyMove(later);		// 걸음이 쓴 이동은 이 입력을 처음 돌릴 때(AddInput) 정한 값과 같다 — 이동 막음 수가 칸에서 이어진다
			RememberMove(later);
			later.valid = true;
		}
		return result;
	}

	void CSelfPrediction::Equip(const wire::Input& input)
	{
		// 서버 ApplyInput 과 같은 함수 — 앞 입력과 번호가 달라졌고 가진 무기일 때만 바꾼다(Loadout.h)
		if (_loadout.OnInputWeapon(input.weapon, _weapons) == ESwitchResult::Switched) _weapon = _loadout.Held();
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

	void CSelfPrediction::OnPickup(uint32_t seq, bool midInput, uint8_t weapon, uint8_t give, uint8_t slotIndex, int16_t magazine, int16_t spare)
	{
		++_pickupCounters.received;
		const bool atSpawn = seq == _compareFromSeq;
		if (atSpawn) midInput = false;		// 부활 · 자리 옮기기 안에서 집음 — 입력이 아니다
		const uint32_t baseSeq = midInput ? seq - 1 : seq;
		const bool atPlace = atSpawn && _placeActive;		// 자리 옮기기 안에서 집음 — 앞 칸은 탄 정하기 앞(OnPlace)
		const WeaponSnap& base = atPlace ? _placeWeapon : baseSeq == _compareFromSeq ? _spawnWeapon : _weaponSnaps[baseSeq % kWeaponSnapCount];
		Slot& at = _ring[seq % kRingSize];
		const WeaponSnap& atSnap = _weaponSnaps[seq % kWeaponSnapCount];
		const bool atKnown = !atSpawn && at.seq == seq && atSnap.seq == seq;
		if (_waitingSpawn || seq < _compareFromSeq || seq > _seq || base.seq != baseSeq || (!atSpawn && !atKnown) || weapon >= (uint8_t)EWeapon::Count || slotIndex >= kQuickSlotCount)
		{
			++_pickupCounters.noSlot;
			return;
		}
		++_pickupCounters.rerun;
		// 걸음 쪽 · 마지막 무기 결과는 그대로 둔다 — 다시 도는 것은 무기 쪽뿐이고, 걸음 값은 입력마다 그 슬롯 값을 빌려 쓴다
		const MoveState move = _move;
		const AdsAbility ads = _ads;
		const WeaponEvents events = _lastWeaponEvents;
		const WeaponSnap start = base;
		const bool atMelee = atKnown ? atSnap.meleeActive : false;		// RememberWeapon 이 이 칸을 덮기 전에 읽는다
		// 집기 직전 — 입력 중이면 앞 입력 뒤 칸 + 이 입력의 교체(AddInput 처럼 걸음 앞이지만 무기 쪽만 보므로 순서는 같다)
		const auto beforeGive = [&]()
		{
			LoadWeapon(start);
			if (midInput)
			{
				_move = at.move;		// 무기 순환이 읽는 속도 · 공중 · 앉음 = 이 입력 걸음 뒤
				_ads = at.ads;
				Equip(at.input);
			}
		};
		beforeGive();
		const EGiveResult result = _loadout.Give((EWeapon)weapon, _weapons);
		if ((uint8_t)result != give || _loadout.SlotOf(weapon) != (int)slotIndex)
		{
			++_pickupCounters.resultDiffs;
			beforeGive();
			const bool isNew = give == (uint8_t)EGiveResult::NewEquipped || give == (uint8_t)EGiveResult::NewNotEquipped;
			if (isNew) _weapons[weapon].Init((EWeapon)weapon);
			_loadout.ForceGive(weapon, slotIndex, give == (uint8_t)EGiveResult::NewEquipped);
		}
		_weapon = _loadout.Held();
		CWeaponCycle& cycle = _weapons[weapon];
		if (cycle.GetMagazine() != magazine || cycle.GetSpare() != spare)
		{
			++_pickupCounters.ammoFixes;
			cycle.SetAmmoForCheck(magazine, spare);
		}
		if (midInput) ApplyWeapon(at, atMelee);		// 서버처럼 집은 뒤 이 입력의 무기 순환
		if (atPlace)
		{
			SaveWeapon(_placeWeapon, seq, start.meleeActive);		// 같은 번호 소식이 하나 더 와도(받침 둘에 겹침) 집은 뒤에서 잇는다
			if (_placeAmmoWeapon < (uint8_t)EWeapon::Count) _weapons[_placeAmmoWeapon].SetAmmoForCheck(_placeMagazine, _placeSpare);	// 서버 ApplyPlace 차례 — 집기 뒤 탄 정하기
		}
		if (atSpawn) SaveWeapon(_spawnWeapon, seq, start.meleeActive);		// 부활 칸은 늘 꺼짐 · 자리 옮기기 칸은 옮길 때 값(OnPlace)
		else RememberWeapon(at, atMelee);
		for (uint32_t next = seq + 1; next <= _seq; ++next)
		{
			Slot& later = _ring[next % kRingSize];
			const WeaponSnap& old = _weaponSnaps[next % kWeaponSnapCount];
			if (later.seq != next || old.seq != next) continue;
			const bool meleeActive = old.meleeActive;
			if (later.meleeStarted) ++_pickupCounters.meleeInWindow;
			_move = later.move;		// 무기 순환이 읽는 속도 · 공중 · 앉음 = 이 입력 걸음 뒤(AddInput 과 같은 자리)
			_ads = later.ads;
			Equip(later.input);
			ApplyWeapon(later, meleeActive);
			RememberWeapon(later, meleeActive);
		}
		_move = move;
		_ads = ads;
		_lastWeaponEvents = events;
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

	bool CSelfPrediction::WillMeleeStart(const wire::Input& input) const
	{
		if ((input.buttons & wire::BUTTON_MELEE) == 0 || _waitingSpawn) return false;
		// StepAbilities 의 능력 자리를 사본으로 같은 차례(AbilityOrder)대로 — 근접 차례에 대시가 켜져 있으면 막히고,
		// 도는 근접은 그보다 먼저 던진 수류탄 · 켠 대시가 끊어야 켜진다(대시가 끊었으면 대시가 막는다)
		CDashAbility dash = _dash;
		dash.NextInput();
		CGrenadeAbility grenade = _grenade;
		grenade.NextInput();
		bool meleeFree = _melee.WillActivateNextInput();
		uint8_t order[3];
		bool reloadFirst = false;
		const int abilities = wire::AbilityOrder(input.buttons, input.pressOrder, order, reloadFirst);
		for (int k = 0; k < abilities; ++k)
		{
			switch (order[k])
			{
			case wire::PRESS_DASH:
				if (dash.TryActivate(input.pitch, input.yaw, false).result == EDashResult::Started) meleeFree = true;
				break;
			case wire::PRESS_GRENADE:
				if (grenade.TryActivate(false, dash.BlocksPresses()) == EGrenadeResult::Thrown) meleeFree = true;
				break;
			case wire::PRESS_MELEE:
				return !dash.BlocksPresses() && meleeFree;
			default:
				break;
			}
		}
		return false;
	}

	void CSelfPrediction::OnDashResult(uint32_t seq, uint8_t dir, const Vec3& force)
	{
		Slot& slot = _ring[seq % kRingSize];
		if (_waitingSpawn || slot.seq != seq || seq <= _compareFromSeq || !slot.valid)
		{
			++_dashCounters.resultsNoSlot;
			return;
		}
		++_dashCounters.results;
		slot.dashServer = true;
		slot.dashDenied = false;
		if (slot.dashStarted && slot.dashDir == dir && SameForce(slot.dashForce, force)) return;
		if (!slot.dashStarted) ++_dashCounters.startMismatches;
		else ++_dashCounters.forceFixes;
		slot.dashDir = dir;
		slot.dashForce = force;
		if (ReplayDash(seq)) RerunMovesFrom(seq);
	}

	void CSelfPrediction::AckDash(uint32_t lastSeq)
	{
		if (_waitingSpawn) return;		// AckMelee 와 같다 — OnSpawn 이 본 번호를 부활 번호로 옮긴다
		if (lastSeq > _seq) lastSeq = _seq;
		uint32_t replayFrom = 0;
		for (uint32_t seq = _dashAckedSeq + 1; seq <= lastSeq; ++seq)
		{
			Slot& slot = _ring[seq % kRingSize];
			if (slot.seq != seq || slot.dashServer) continue;
			slot.dashDenied = true;		// 서버가 돌린 입력인데 켬 소식이 없다 = 서버는 안 켰다. 다시 고를 때 이 칸에서는 안 켠다
			if (!slot.dashStarted) continue;
			++_dashCounters.unconfirmed;
			if (replayFrom == 0) replayFrom = seq;
		}
		if (lastSeq > _dashAckedSeq) _dashAckedSeq = lastSeq;
		if (replayFrom != 0 && ReplayDash(replayFrom)) RerunMovesFrom(replayFrom);
	}

	// 서버 소식 · 확인으로 정해진 칸을 따라 seq 부터 대시를 다시 고른다 — 켬 · 방향 · 힘 · 막기 둘 · 근접 끊음을 칸에 다시 쓰고, 지금 대시 상태(_dash)도 바꾼다.
	// 서버가 켠 칸은 켜기 조건을 안 본다(StartFromServer — 서버가 켰다). 그 밖의 누름은 다시 고른 쿨다운 · 켜짐으로 다시 본다(앞 입력 = 칸의 dashMove · 요).
	// 근접은 다시 안 고른다 — 대시가 바뀌어 근접 끊음이 달라졌거나 이제 막히는 칸에 근접이 켜져 있으면 meleeInReplay 로 센다(걸음 쪽 밀기는 근접 소식 · AckMelee 가 고친다)
	bool CSelfPrediction::ReplayDash(uint32_t seq)
	{
		const Slot* base = nullptr;		// RerunMovesFrom 과 같은 앞 칸 — 다시 고른 뒤 걸음도 거기서 다시 돈다
		if (seq == _compareFromSeq + 1) base = &_spawnBase;
		else
		{
			const Slot& before = _ring[(seq - 1) % kRingSize];
			if (before.seq == seq - 1 && before.valid) base = &before;
		}
		if (!base)
		{
			++_dashCounters.replaysMissed;
			return false;
		}
		++_dashCounters.replays;
		CDashAbility dash = base->dashAfter;
		for (uint32_t at = seq; at <= _seq; ++at)
		{
			Slot& later = _ring[at % kRingSize];
			if (later.seq != at) continue;
			const bool oldCut = later.dashCutMelee;
			dash.NextInput();
			later.dashBlocksBefore = dash.BlocksPresses();
			bool started = false;
			if (later.dashServer)
			{
				dash.StartFromServer();		// 방향 · 힘은 소식 값 그대로(OnDashResult 가 칸에 넣었다)
				started = true;
				AddServerAbility(later, wire::PRESS_DASH);
			}
			else if (!later.dashDenied && (later.input.buttons & wire::BUTTON_DASH) != 0)
			{
				const DashStart start = dash.TryActivate(later.input.pitch, later.input.yaw, false);
				if (start.result == EDashResult::Started)
				{
					started = true;
					later.dashDir = (uint8_t)start.dir;
					later.dashForce = start.force;
				}
			}
			if (!started)
			{
				later.dashDir = 0;
				later.dashForce = Vec3();
			}
			later.dashStarted = started;
			later.dashCutMelee = started && MeleeActiveAt(later, wire::PRESS_DASH);		// 대시 차례에 켜져 있던 근접(앞 차례에 켠 근접 포함 · 앞 차례 던짐이 끊었으면 없음)
			later.dashBlocks = dash.BlocksPresses();
			later.dashBlocksFire = dash.BlocksHeldFire();
			if (oldCut != later.dashCutMelee || (DashBlocksAt(later, wire::PRESS_MELEE) && later.meleeStarted)) ++_dashCounters.meleeInReplay;
			dash.RememberInput(later.dashMove, later.input.yaw);
			later.dashAfter = dash;
		}
		_dash = dash;
		// 대시 막기가 바뀐 칸은 수류탄 던짐도 바뀐다(대시 중 막힘) — 같은 번호부터 다시 고른다. 걸음은 부르는 쪽이 이 번호부터 다시 돈다
		bool grenadeCutChanged = false;
		(void)ReplayGrenade(seq, grenadeCutChanged);
		return true;
	}

	void CSelfPrediction::OnGrenadeThrown(uint32_t seq)
	{
		Slot& slot = _ring[seq % kRingSize];
		if (seq == 0 || slot.seq != seq)
		{
			++_grenadeCounters.resultsNoSlot;
			return;
		}
		++_grenadeCounters.results;
		slot.grenadeServer = true;
		slot.grenadeDenied = false;
		if (slot.grenadeThrown) return;
		++_grenadeCounters.startMismatches;
		bool cutChanged = false;
		if (ReplayGrenade(seq, cutChanged) && cutChanged && !_waitingSpawn) RerunMovesFrom(seq);
	}

	void CSelfPrediction::AckGrenade(uint32_t lastSeq, bool serverDead)
	{
		bool cutChanged = false;
		const uint32_t replayFrom = MarkGrenadeAcked(lastSeq, serverDead || _waitingSpawn, cutChanged);
		if (replayFrom != 0 && cutChanged && !_waitingSpawn) RerunMovesFrom(replayFrom);
	}

	uint32_t CSelfPrediction::MarkGrenadeAcked(uint32_t lastSeq, bool serverDead, bool& cutChanged)
	{
		cutChanged = false;
		if (lastSeq > _seq) lastSeq = _seq;
		uint32_t replayFrom = 0;
		for (uint32_t seq = _grenadeAckedSeq + 1; seq <= lastSeq; ++seq)
		{
			Slot& slot = _ring[seq % kRingSize];
			if (slot.seq != seq || slot.grenadeServer) continue;
			slot.grenadeDenied = true;		// 서버가 돌린 입력인데 던짐 소식이 없다 = 서버는 안 던졌다. 다시 고를 때 이 칸에서는 안 던진다
			if (!slot.grenadeThrown) continue;
			++_grenadeCounters.unconfirmed;
			if (serverDead) ++_grenadeCounters.unconfirmedDead;
			if (replayFrom == 0) replayFrom = seq;
		}
		if (lastSeq > _grenadeAckedSeq) _grenadeAckedSeq = lastSeq;
		if (replayFrom != 0 && !ReplayGrenade(replayFrom, cutChanged)) return 0;
		return replayFrom;
	}

	uint64_t CSelfPrediction::CountDashStartedUnacked() const
	{
		uint64_t count = 0;
		for (uint32_t seq = _dashAckedSeq + 1; seq <= _seq; ++seq)
		{
			const Slot& slot = _ring[seq % kRingSize];
			if (slot.seq == seq && slot.dashStarted && !slot.dashServer) ++count;
		}
		return count;
	}

	uint64_t CSelfPrediction::CountGrenadeThrownUnacked() const
	{
		uint64_t count = 0;
		for (uint32_t seq = _grenadeAckedSeq + 1; seq <= _seq; ++seq)
		{
			const Slot& slot = _ring[seq % kRingSize];
			if (slot.seq == seq && slot.grenadeThrown && !slot.grenadeServer) ++count;
		}
		return count;
	}

	// 서버 소식 · 확인으로 정해진 칸을 따라 seq 부터 수류탄을 다시 고른다 — 던짐 · 근접 끊음을 칸에 다시 쓰고, 지금 수류탄 상태(_grenade)도 바꾼다.
	// 앞 칸 = 바로 앞 번호 칸(목숨을 넘어 잇는다 — 부활 대기 칸 · 자리 옮기기 칸도 grenadeAfter 를 남긴다). 서버가 던진 칸은 켜기 조건을 안 본다(StartFromServer).
	// 그 밖의 누름은 칸의 대시 막기 · 죽은 갈래로 다시 본다. 근접 능력(무기 쪽)은 다시 안 고른다 — 끊음이 바뀐 칸은 meleeInReplay 로 센다
	bool CSelfPrediction::ReplayGrenade(uint32_t seq, bool& cutChanged)
	{
		cutChanged = false;
		const Slot& before = _ring[(seq - 1) % kRingSize];
		if (seq == 0 || before.seq != seq - 1)		// seq 1 의 앞 칸 = 처음부터 비어 있는 0 번 칸(seq 0) — 새 수류탄 상태
		{
			++_grenadeCounters.replaysMissed;
			return false;
		}
		++_grenadeCounters.replays;
		CGrenadeAbility grenade = before.grenadeAfter;
		for (uint32_t at = seq; at <= _seq; ++at)
		{
			Slot& later = _ring[at % kRingSize];
			if (later.seq != at) continue;
			if (!later.grenadeTicks)
			{
				later.grenadeAfter = grenade;
				continue;
			}
			grenade.NextInput();
			bool thrown = false;
			if (later.grenadeServer)
			{
				grenade.StartFromServer();
				thrown = true;
				AddServerAbility(later, wire::PRESS_GRENADE);
			}
			else if (!later.grenadeDenied && (later.input.buttons & wire::BUTTON_GRENADE) != 0)
				thrown = grenade.TryActivate(later.grenadeDead, DashBlocksAt(later, wire::PRESS_GRENADE)) == EGrenadeResult::Thrown;
			// 던질 차례에 켜져 있던 근접을 끊는다 — 앞 차례에 켠 근접 포함(근접 → Q, L-5-a). 던질 수 있으면 앞 차례 대시는 안 켜졌다(켜졌으면 막힘)
			const bool cut = thrown && !later.grenadeDead && MeleeActiveAt(later, wire::PRESS_GRENADE);
			if (cut != later.grenadeCutMelee)
			{
				cutChanged = true;
				++_grenadeCounters.meleeInReplay;
			}
			if (thrown != later.grenadeThrown) ++(thrown ? _grenadeCounters.replayGained : _grenadeCounters.replayLost);
			later.grenadeThrown = thrown;
			later.grenadeCutMelee = cut;
			// Q → 대시 차례면 던짐이 대시보다 먼저 근접을 끊는다 — 던짐이 바뀌면 대시의 근접 끊음도 바뀐다(걸음 쪽 밀기 지우기)
			if (later.dashStarted && AbilityPos(later, wire::PRESS_GRENADE) < AbilityPos(later, wire::PRESS_DASH))
			{
				const bool dashCut = MeleeActiveAt(later, wire::PRESS_DASH);
				if (dashCut != later.dashCutMelee)
				{
					later.dashCutMelee = dashCut;
					cutChanged = true;
					++_grenadeCounters.meleeInReplay;
				}
			}
			later.grenadeAfter = grenade;
		}
		_grenade = grenade;
		return true;
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
		// Reconcile 의 다시 돌리기와 같은 값을 잇는다 — 걸음 상태 · 점프 능력 · 근접 밀기 번호 · 이동 막음 · 조준
		_move = base->move;
		_jump = base->jump;
		_meleePushId = base->meleePushId;
		_lockInputs = base->lockInputs;
		_ads = base->ads;
		for (uint32_t at = seq; at <= _seq; ++at)
		{
			Slot& later = _ring[at % kRingSize];
			if (later.seq != at) continue;
			(void)ApplyMove(later);
			RememberMove(later);
			later.valid = true;
		}
		return true;
	}

	// 근접 · 대시 · 수류탄 능력 — 서버 ApplyInput 과 같은 자리(무기 교체 뒤 · 걸음 앞) · 같은 차례(실린 누름 차례 — wire::AbilityOrder, L-5-a).
	// 세 입력 수를 먼저 세고(NextInput 셋), 차례대로 켠다 — 대시 켬 → 근접을 끊는다(StartDash) · 뒤 누름을 막는다 / 던짐 → 근접을 끊는다(ThrowGrenade) ·
	// 대시가 켜져 있으면 막힘 / 근접 → 대시가 켜져 있으면 막힘(TryMelee). 죽음은 모른다(서버만 막는다 — 소식이 안 와 AckMelee · AckDash 가 지운다).
	// 경기 끝 단계는 서버도 막지 않는다(L-3-b — Dash.h EDashResult)
	void CSelfPrediction::StepAbilities(Slot& slot, bool count)
	{
		const wire::Input& input = slot.input;
		_melee.NextInput();
		_dash.NextInput();
		// 자리 옮기기 칸(OnSpawn 다시 돌리기에서만 여기로 온다)은 서버 ApplyPlace 처럼 수류탄 입력 수를 안 센다
		if (slot.grenadeTicks) _grenade.NextInput();
		slot.meleeWasActive = _melee.IsActive();
		slot.dashBlocksBefore = _dash.BlocksPresses();
		slot.dashStarted = false;
		slot.dashDir = 0;
		slot.dashForce = Vec3();
		slot.dashCutMelee = false;
		slot.grenadeThrown = false;
		slot.grenadeCutMelee = false;
		slot.meleeStarted = false;
		// 차례는 입력으로 다시 정한다 — 서버 소식이 끼운 능력(AddServerAbility)은 부활 다시 돌리기(OnSpawn)가 내 누름으로 다시 고를 때 남기지 않는다
		slot.abilityCount = (uint8_t)wire::AbilityOrder(input.buttons, input.pressOrder, slot.abilityOrder, slot.reloadBeforeGrenade);
		for (int k = 0; k < slot.abilityCount; ++k)
		{
			switch (slot.abilityOrder[k])
			{
			case wire::PRESS_DASH:
			{
				if (count) ++_dashCounters.presses;
				const DashStart dash = _dash.TryActivate(input.pitch, input.yaw, false);
				if (dash.result == EDashResult::Started)
				{
					slot.dashStarted = true;
					slot.dashDir = (uint8_t)dash.dir;
					slot.dashForce = dash.force;
					slot.dashCutMelee = _melee.Cut();
					if (count) ++_dashCounters.started;
				}
				break;
			}
			case wire::PRESS_GRENADE:
				// 수류탄(G-3) — 대시가 켜져 있으면(이 입력에 먼저 켠 것 포함) 막힌다. 던지면 그 자리에서 근접을 끊는다(ThrowGrenade)
				slot.grenadeThrown = slot.grenadeTicks && _grenade.TryActivate(false, _dash.BlocksPresses()) == EGrenadeResult::Thrown;
				slot.grenadeCutMelee = slot.grenadeThrown && _melee.Cut();
				if (slot.grenadeThrown && count) ++_grenadeCounters.thrown;
				break;
			case wire::PRESS_MELEE:
				slot.meleeStarted = !_dash.BlocksPresses() && _melee.TryActivate(GetWeaponDef((EWeapon)_weapon), kTick);
				if (slot.meleeStarted) ++_meleeCounters.started;
				break;
			default:
				break;
			}
		}
		slot.dashBlocks = _dash.BlocksPresses();
		slot.dashBlocksFire = _dash.BlocksHeldFire();
		slot.grenadeAfter = _grenade;
	}

	void CSelfPrediction::RememberDash(Slot& slot, const Vec3& moveUsed)
	{
		_dash.RememberInput(moveUsed, slot.input.yaw);		// 서버와 같은 자리 — 걸음 뒤, 이 걸음이 가져간 이동 · 이 입력의 요
		slot.dashMove = moveUsed;
		slot.dashAfter = _dash;
	}

	// Room.cpp ApplyInput 과 같은 순서
	Vec3 CSelfPrediction::ApplyMove(const Slot& slot)
	{
		const wire::Input& input = slot.input;
		// 능력 자리 — 서버 ApplyInput 과 같은 켜는 차례(slot.abilityOrder, L-5-a)로 힘을 넣고 지운다. 근접이 먼저 켜져 밀기를 넣고 뒤 차례 대시 · 던짐이 그 힘을 지우는 입력이 있다
		const auto meleePush = [&]()
		{
			// 근접 밀기 — Lyra 는 능력이 켜지는 입력 처리에서 넣고 그 프레임 이동이 민다(E-2-1). 서버 TryMelee 와 같은 자리(능력 자리 근접 차례)
			if (slot.meleeStarted) _meleePushId = 0;		// 서버 TryMelee 와 같다 — 맞아야 밀기 번호가 생긴다
			if (slot.meleePush) _meleePushId = _sim.ApplyRootForce(_move, slot.meleePushForce, kMeleePushDuration);
		};
		bool meleeDone = false;
		for (int k = 0; k < slot.abilityCount; ++k)
		{
			switch (slot.abilityOrder[k])
			{
			case wire::PRESS_DASH:
				// 대시를 켠 입력 — 서버 StartDash 와 같은 차례: 끊은 근접의 밀기 힘 지우기 → 점프 끊기 → 일어서기 → 대시 힘
				if (slot.dashStarted)
				{
					if (slot.dashCutMelee && _meleePushId != 0) _sim.RemoveRootForce(_move, _meleePushId);
					_meleePushId = 0;
					if (_jump.Cut()) _sim.ReleaseJump(_move);
					if (_move.crouched) _sim.RequestUnCrouch(_move);
					_sim.ApplyRootForce(_move, slot.dashForce, kDashForceDuration, DashForceSettings());
				}
				break;
			case wire::PRESS_GRENADE:
				// 수류탄을 던져 근접을 끊은 입력 — 서버 ThrowGrenade 처럼 끊은 근접의 밀기 힘을 지운다(끊었을 때만 번호를 0 으로)
				if (slot.grenadeCutMelee)
				{
					if (_meleePushId != 0) _sim.RemoveRootForce(_move, _meleePushId);
					_meleePushId = 0;
				}
				break;
			case wire::PRESS_MELEE:
				meleePush();
				meleeDone = true;
				break;
			default:
				break;
			}
		}
		if (!meleeDone) meleePush();		// 근접 누름이 없는 칸 — 켬 · 밀기가 둘 다 없어 아무것도 안 한다(칸 값만 믿지 않고 같은 길로)
		const bool jump = (input.buttons & wire::BUTTON_JUMP) != 0;
		switch (_jump.Apply(jump, slot.dashBlocks))		// 대시가 켜져 있으면 새 누름은 버려진다(쥐고 있어도 끝난 뒤 안 켜진다)
		{
		case EJumpStep::Press:
			_sim.RequestUnCrouch(_move);		// 점프 전 일어서기(LyraGameplayAbility_Jump.cpp:55)
			_sim.PressJump(_move);
			break;
		case EJumpStep::Release:
			_sim.ReleaseJump(_move);
			break;
		case EJumpStep::Blocked:
		case EJumpStep::None:
			break;
		}
		if ((input.buttons & wire::BUTTON_CROUCH) != 0) _sim.ToggleCrouch(_move);		// 이동 막음 중에도(서버와 같다)

		Vec3 moveInput(input.moveX, input.moveY, 0);
		const double moveLength = Length(moveInput);
		if (moveLength > 1.0) moveInput = moveInput * (1.0 / moveLength);
		if (_lockInputs > 0)
		{
			--_lockInputs;
			moveInput = Vec3();
		}
		const Vec3 moveUsed = moveInput;		// 서버가 대시 「앞 입력」으로 기억하는 값 — 일부러 틀림 배율 앞
		if (_debugMoveScale != 1.0) moveInput = moveInput * _debugMoveScale;
		_ads.Apply((input.buttons & wire::BUTTON_ADS) != 0, slot.dashBlocks);		// 대시가 켜지는 입력에 끄고, 막힌 누름은 떼었다 다시 눌러야 켜진다(서버와 같다)
		_move.maxSpeed = AdsWalkSpeed(_maxWalkSpeed, _ads.active);
		// 근접 밀기는 위 능력 자리에서 넣었다 — 점프 깃발 · 앉기 바람 · 조준 속도는 힘 묶음을 안 건드려 걸음 바로 앞에서 넣던 판 13 과 같은 결과(L-5-a)
		_sim.Tick(_move, moveInput, kTick);
		return moveUsed;
	}

	void CSelfPrediction::ApplyWeapon(const Slot& slot, bool meleeActive)
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
		weaponInput.meleeActive = meleeActive || slot.meleeStarted;		// 서버와 같다 — 이 입력에 켠 근접은 뒤 차례가 끊었어도 켜진 셈(L-5-a)
		weaponInput.meleeStarted = slot.meleeStarted;
		weaponInput.reloadBeforeGrenade = slot.reloadBeforeGrenade;
		weaponInput.dashActive = slot.dashBlocksFire;
		weaponInput.dashStarted = slot.dashStarted;
		weaponInput.grenadeThrown = slot.grenadeThrown;		// 도는 재장전만 끊는다(서버 ApplyInput 과 같은 값)
		// 서버와 같은 순서(Room.cpp ApplyInput) — 대시가 앞 입력부터 켜져 있으면 쥔 사격을 발사 문 앞에서 뺀다
		if (weaponInput.fireHeld && weaponInput.dashActive) weaponInput.fireHeld = false;
		_fireGate.NextInput();
		if (weaponInput.fireHeld && _fireGate.IsBlocked((EWeapon)_weapon, _weapons[_weapon].IsFireActive())) weaponInput.fireHeld = false;
		_lastWeaponEvents = _weapons[_weapon].Tick(kTick, weaponInput, motion);
		_aimCamera.Tick(_ads.active, kTick);
		if (_lastWeaponEvents.fired) _fireGate.OnFired((EWeapon)_weapon, _weapons[_weapon].GetDef(), kTick);
	}

	void CSelfPrediction::RememberMove(Slot& slot) const
	{
		slot.move = _move;
		slot.jump = _jump;
		slot.meleePushId = _meleePushId;
		slot.lockInputs = _lockInputs;
		slot.ads = _ads;
	}

	void CSelfPrediction::RememberWeapon(Slot& slot, bool meleeActive)
	{
		slot.valid = true;
		slot.magazine = (int16_t)_weapons[_weapon].GetMagazine();
		slot.spare = (int16_t)_weapons[_weapon].GetSpare();
		slot.weapon = _weapon;
		slot.quickSlots = _loadout.PackSlots();
		slot.reloading = _weapons[_weapon].IsReloading();
		SaveWeapon(_weaponSnaps[slot.seq % kWeaponSnapCount], slot.seq, meleeActive);
	}

	void CSelfPrediction::SaveWeapon(WeaponSnap& snap, uint32_t seq, bool meleeActive) const
	{
		snap.seq = seq;
		snap.weapons = _weapons;
		snap.loadout = _loadout;
		snap.weapon = _weapon;
		snap.fireGate = _fireGate;
		snap.aimCamera = _aimCamera;
		snap.meleeActive = meleeActive;
	}

	void CSelfPrediction::LoadWeapon(const WeaponSnap& snap)
	{
		_weapons = snap.weapons;
		_loadout = snap.loadout;
		_weapon = snap.weapon;
		_fireGate = snap.fireGate;
		_aimCamera = snap.aimCamera;
	}
}
