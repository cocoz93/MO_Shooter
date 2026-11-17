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
			bool meleePush = false;	// 이 입력의 걸음 앞에 근접 밀기를 넣었다(컨텐츠 E-2-1) — 다시 돌릴 때도 같은 입력에서 다시 넣는다
			Vec3 meleePushForce;
			bool meleeStarted = false;	// 이 입력에 내 근접 능력이 켜졌다(E-3)
			bool meleeServer = false;	// 서버 근접 결과(S2C_MELEE)를 받았다 — 받은 뒤의 밀기는 서버 값이다
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

		// 입력에 번호를 붙이고 기억한다. 부활을 기다리는 중이 아니면 바로 돌린다.
		// meleePush = 이 입력에서 내 화면 근접이 맞았을 때의 밀기 힘(MeleePushForce). 부르는 쪽이 화면 상대 자리 · 내 손으로 ResolveMelee 를 불러 정한다(E-3 연결).
		//   이 입력에 근접 능력이 안 켜지면 버리고 센다(WillMeleeStart 로 미리 본다). 서버 결과가 오면 OnMeleeResult 가 서버 값으로 바꾼다
		// 부활을 기다리는 중이면 버린다 — 서버도 죽은 동안 입력을 안 쓴다
		void AddInput(wire::Input& input, const Vec3* meleePush = nullptr);

		// 서버가 시작 자리로 옮겼다 — 그 번호 뒤의 입력을 새 자리에서 다시 돌린다
		void OnSpawn(double x, double y, double z, uint32_t lastSeq);	// 와이어 Spawn 은 double — float 로 줄이면 같은 입력을 돌려도 어긋난다

		// 내가 죽었다 — 부활 소식이 올 때까지 돌리지 않는다(서버도 죽은 동안 입력을 안 쓴다)
		void BeginWaitSpawn() { _waitingSpawn = true; }

		// 서버 상태와 같은 번호의 슬롯을 맞춰 본다. tolerance 를 넘으면 서버 값에서 다시 돌린다(위치·낙하 여부만, 탄은 서버 값을 그대로 보여 주면 된다)
		ReconcileResult Reconcile(const wire::PlayerState& server, double tolerance);

		// 다음 AddInput 에서 이 입력이 근접 능력을 켜나 — 켜질 입력에만 화면 근접 판정을 해 밀기를 넘기려고 넣기 전에 본다(E-3)
		bool WillMeleeStart(const wire::Input& input) const { return (input.buttons & wire::BUTTON_MELEE) != 0 && !_waitingSpawn && _melee.WillActivateNextInput(); }

		// 서버 근접 결과(S2C_MELEE 중 공격자가 나) — 그 입력의 밀기를 서버 값으로 바꾸고, 예측과 다르면 그 입력부터 걸음을 다시 돌린다.
		// 서버는 이 소식을 그 입력을 돈 틱의 상태보다 먼저 보낸다(같은 연결, 판정이 입력 처리 안이고 상태는 틱 끝) — 상태를 맞춰 보기 전에 밀기가 고쳐져 있다
		void OnMeleeResult(uint32_t seq, bool push, const Vec3& force);

		// 서버가 이 번호까지 입력을 돌렸다 — 그 안에서 내가 켰는데 서버 결과가 안 온 근접(서버는 안 켰다)의 밀기를 지우고 다시 돌린다.
		// Reconcile 이 처음에 부른다. 맞춰 보기만 하는 쪽(봇)은 FindComparable 앞에 부른다
		void AckMelee(uint32_t lastSeq);

		struct MeleeCounters
		{
			uint64_t started = 0;			// 내가 켠 근접
			uint64_t pushesGiven = 0;		// 부르는 쪽이 넘긴 밀기 — 켜진 입력에만
			uint64_t pushesIgnored = 0;		// 근접이 안 켜진 입력에 넘긴 밀기 — 버림. 0 이어야 한다
			uint64_t results = 0;			// 받은 서버 결과(내 것)
			uint64_t resultsNoSlot = 0;		// 받았지만 그 번호 칸이 없음(부활 전 목숨 · 기다리는 중)
			uint64_t startMismatches = 0;	// 서버는 켰는데 나는 안 켬 — 능력 칸 수가 어긋났다. 0 이어야 한다
			uint64_t pushHitFixes = 0;		// 밀기 있음 · 없음이 달라 서버 값으로 다시 돌림
			uint64_t pushForceFixes = 0;	// 둘 다 밀기인데 힘이 달라 다시 돌림(화면 몸 ↔ 서버 되감은 몸 차이)
			uint64_t unconfirmed = 0;		// 내가 켰는데 서버 결과 없이 지나감(서버는 안 켰다)
			uint64_t rerunsMissed = 0;		// 다시 돌릴 앞 칸이 없어 못 돌림 — 다음 Reconcile 이 서버 자리로 고친다
		};
		const MeleeCounters& GetMeleeCounters() const { return _meleeCounters; }
		bool IsMeleeActive() const { return _melee.IsActive(); }

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
		void ApplyMove(const Slot& slot);		// 입력 + 그 입력에 붙은 밀기
		void StepMelee(Slot& slot);			// 근접 능력 한 입력 — 서버 ApplyInput 처럼 걸음 앞
		void ApplyWeapon(const Slot& slot);
		bool RerunMovesFrom(uint32_t seq);	// 그 앞 칸의 상태에서 seq 부터 걸음을 다시 돌린다
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
		CMeleeAbility _melee;		// 무기 쪽 — 다시 돌리기는 걸음만 하므로 건드리지 않는다
		MeleeCounters _meleeCounters;
		Slot _spawnBase;			// 부활 직후 상태 — 부활 뒤 첫 입력부터 다시 돌릴 때의 앞 칸
		uint32_t _meleeAckedSeq = 0;	// AckMelee 가 본 마지막 번호
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
