// 내 캐릭터 미리 돌리기 — 서버 ApplyInput 과 같은 순서로 내 입력을 여기서도 돌린다
//
// 봇과 Lyra 플러그인이 같이 쓴다. 봇은 서버 값과 맞춰 보기만 하고(차이 0 이 시험),
// Lyra 는 틀어지면 Reconcile 로 서버 값에서 다시 돌린다.
//
// - 왜 한 벌인가: 봇 시험(위치·탄 차이 0)이 그대로 Lyra 쪽 회귀 시험이 된다
// - 왜 슬롯에 MoveState 를 통째로 두나: 패킷에 없는 값(바닥·점프 키 시간)이 되감기에 필요하다
#pragma once

#include "GameProtocol.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Weapon.h"

#include <array>
#include <cstdint>

namespace mo
{
	class CSelfPrediction
	{
	public:
		static constexpr uint32_t kRingSize = 1024;

		struct Slot
		{
			wire::Input input = wire::Input();
			uint32_t seq = 0;
			bool valid = false;		// 아래 값이 지금 목숨 기준으로 계산됐나
			MoveState move;			// 이 입력을 돌린 뒤
			bool jumpHeld = false;
			int lockInputs = 0;
			AdsAbility ads;			// 조준은 걸음 속도를 바꾼다 — 다시 돌릴 때 이 입력 뒤 상태에서 잇는다
			int16_t magazine = 0, spare = 0;
			uint8_t weapon = 0;
			bool reloading = false;
		};

		struct ReconcileResult
		{
			bool compared = false;
			bool corrected = false;
			double posDiff = 0;		// 같은 번호의 서버 위치 ↔ 내 값
			double velDiff = 0;
		};

		CSelfPrediction(CMoveSim& sim, const MatchRules& rules, float maxWalkSpeed);

		// 입력에 번호를 붙이고 기억한다. 부활을 기다리는 중이 아니면 바로 돌린다
		void AddInput(wire::Input& input);

		// 서버가 시작 자리로 옮겼다 — 그 번호 뒤의 입력을 새 자리에서 다시 돌린다
		void OnSpawn(double x, double y, double z, uint32_t lastSeq);	// 와이어 Spawn 은 double — float 로 줄이면 같은 입력을 돌려도 어긋난다

		// 내가 죽었다 — 부활 소식이 올 때까지 돌리지 않는다(서버도 죽은 동안 입력을 안 쓴다)
		void BeginWaitSpawn() { _waitingSpawn = true; }

		// 서버 상태와 같은 번호의 슬롯을 맞춰 본다. tolerance 를 넘으면 서버 값에서 다시 돌린다(위치·낙하 여부만, 탄은 서버 값을 그대로 보여 주면 된다)
		ReconcileResult Reconcile(const wire::PlayerState& server, double tolerance);

		// 맞춰 보기만 하는 쪽(봇)이 쓰는 슬롯. 비교할 수 없으면 nullptr
		const Slot* FindComparable(const wire::PlayerState& server) const;

		// 일부러 틀리기 — 시험이 잡는지 보려고
		void SetDebugMoveScale(double scale) { _debugMoveScale = scale; }
		void DebugNudge(const Vec3& by) { _move.location = _move.location + by; }

		const MoveState& GetMove() const { return _move; }
		const CWeaponCycle& GetWeapon() const { return _weapons[_weapon]; }
		uint8_t GetWeaponIndex() const { return _weapon; }
		uint32_t GetSeq() const { return _seq; }
		// 방금 돌린 입력의 무기 결과 — 쐈으면 이 입력 번호와 퍼짐으로 서버와 같은 탄 방향을 구한다(D15)
		const WeaponEvents& GetLastWeaponEvents() const { return _lastWeaponEvents; }
		bool IsWaitingSpawn() const { return _waitingSpawn; }
		bool IsLocked() const { return _lockInputs > 0; }
		bool IsAds() const { return _ads.active; }

	private:
		void ApplyMove(const wire::Input& input);
		void ApplyWeapon(const wire::Input& input);
		void Equip(const wire::Input& input);
		void RememberMove(Slot& slot) const;
		void RememberWeapon(Slot& slot) const;

		CMoveSim& _sim;
		const MatchRules& _rules;
		float _maxWalkSpeed;
		std::array<Slot, kRingSize> _ring;
		MoveState _move;
		std::array<CWeaponCycle, (size_t)EWeapon::Count> _weapons;
		CWeaponFireGate _fireGate;
		WeaponEvents _lastWeaponEvents;
		uint8_t _weapon = 0;
		bool _jumpHeld = false;
		int _lockInputs = 0;
		AdsAbility _ads;
		CAimCamera _aimCamera;		// 무기 쪽 — 다시 돌리기(Reconcile)는 걸음만 다시 하므로 건드리지 않는다
		bool _waitingSpawn = false;
		uint32_t _seq = 0;
		uint32_t _compareFromSeq = 0;
		double _debugMoveScale = 1.0;
	};
}
