// 내 캐릭터 미리 돌리기 — 서버 ApplyInput 과 같은 순서로 내 입력을 여기서도 돌린다
//
// 봇과 Lyra 플러그인이 같이 쓴다. 봇은 서버 값과 맞춰 보기만 하고(차이 0 이 시험),
// Lyra 는 틀어지면 Reconcile 로 서버 값에서 다시 돌린다.
//
// - 왜 한 벌인가: 봇 시험(위치·탄 차이 0)이 그대로 Lyra 쪽 회귀 시험이 된다
// - 왜 슬롯에 MoveState 를 통째로 두나: 패킷에 없는 값(바닥·점프 키 시간)이 되감기에 필요하다
#pragma once

#include "Dash.h"
#include "GameProtocol.h"
#include "Grenade.h"
#include "JumpPads.h"
#include "Loadout.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Weapon.h"

#include <array>
#include <cstdint>
#include <vector>

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
			bool meleeWasActive = false;	// 이 입력의 근접 NextInput 뒤에 근접이 켜져 있었나 — 대시를 켜면 끊는다(다시 고르기가 끊음을 다시 정할 때 읽는다, D-3)
			// 능력 자리 켜는 차례(L-5-a) — wire::AbilityOrder 값(서버 ApplyInput 과 같은 차례). 걸음 쪽 힘 넣기 · 지우기와 다시 고르기(ReplayDash · ReplayGrenade)가
			// 이 차례로 「그 능력 차례에 근접이 켜져 있었나 · 대시가 막았나」를 다시 본다. 서버가 켰다는데 내 누름이 없던 능력은 다시 고르기가 고정 차례 자리에 끼운다
			// (Prediction.cpp AddServerAbility — StepAbilities 가 입력으로 다시 정해 지운다)
			uint8_t abilityOrder[3] = {};
			uint8_t abilityCount = 0;
			bool reloadBeforeGrenade = false;	// 재장전 누름이 수류탄보다 먼저 — 무기 쪽이 던진 입력의 그 누름을 끊는다
			bool dashBlocksBefore = false;		// 이 입력 대시 NextInput 뒤 · 켜기 앞의 BlocksPresses — 대시보다 먼저 켠 능력은 이 값으로 막힌다
			// 대시(D-3) — 입력 쪽(StepAbilities · ReplayDash)이 정하고 걸음 · 무기 쪽이 읽는다. 서버 ApplyInput 의 대시 자리와 같은 값
			bool dashStarted = false;	// 이 입력에 대시를 켰다
			uint8_t dashDir = 0;		// EDashDir — 켰을 때
			Vec3 dashForce;				// 켰을 때 넣는 힘
			bool dashBlocks = false;	// CDashAbility::BlocksPresses(능력 자리 뒤) — 점프 · 조준 · 재장전 누름과 대시 뒤 차례의 근접 · 수류탄을 막는다(켠 입력 포함)
			bool dashBlocksFire = false;	// CDashAbility::BlocksHeldFire — 쥔 사격을 막는다(켠 입력 다음부터)
			bool dashCutMelee = false;	// 켜며 근접을 끊었다 — 걸음 쪽이 그 근접의 밀기 힘을 지운다
			bool dashServer = false;	// 서버 대시 소식(켬)을 받았다 — 다시 고를 때도 서버 값으로 켠다
			bool dashDenied = false;	// 서버가 이 입력까지 돌렸는데 소식이 없었다(서버는 안 켬) — 다시 고를 때 안 켠다
			Vec3 dashMove;				// 걸음이 쓴 이동(막음 뒤) — 다음 입력의 「앞 입력」(RememberInput)
			CDashAbility dashAfter;		// 이 입력 뒤 대시 상태 — 다시 고르기(ReplayDash)의 앞 칸
			// 수류탄(G-3) — 쿨다운만 서버와 같은 입력 수로 돌려 「이 입력에 던졌다」를 낸다(수류탄 자체는 예측 안 함, grenade-plan 「예측」).
			// 서버는 죽은 입력에도 쿨다운을 세고 부활에 안 지운다 — 부활 대기 입력까지 칸마다 grenadeAfter 를 남겨 한 사슬로 잇는다
			bool grenadeThrown = false;		// 이 입력에 던졌다 — 무기 쪽 재장전 · 사격 끊기 · 걸음 쪽 근접 밀기 지우기가 읽는다
			bool grenadeCutMelee = false;	// 던지며 근접을 끊었다 — 걸음 쪽이 그 근접의 밀기 힘을 지운다(서버 ThrowGrenade)
			bool grenadeDead = false;		// 부활을 기다리는 동안 넣은 입력 — 서버 죽은 갈래처럼 쿨다운만 준다(OnSpawn 이 새 몸으로 다시 돌리면 꺼짐)
			bool grenadeTicks = true;		// 수류탄 입력 수를 센다 — 자리 옮기기 칸은 서버도 안 센다(ApplyPlace)
			bool grenadeServer = false;		// 서버 던짐 소식을 받았다 — 다시 고를 때도 던진 것으로
			bool grenadeDenied = false;		// 서버가 이 입력까지 돌렸는데 던짐 소식이 없었다(서버는 안 던짐) — 다시 고를 때 안 던진다
			CGrenadeAbility grenadeAfter;	// 이 입력 뒤 수류탄 상태 — 다시 고르기(ReplayGrenade)의 앞 칸
			bool valid = false;		// 아래 값이 지금 목숨 기준으로 계산됐나
			MoveState move;			// 이 입력을 돌린 뒤
			JumpAbility jump;		// 점프 능력(버튼 · 켜짐) — 다시 돌릴 때 잇는다
			uint16_t meleePushId = 0;	// 남아 있는 근접 밀기 힘 번호(0 = 없음) — 대시가 근접을 끊으면 지운다(서버 Player::meleePushId)
			int lockInputs = 0;
			AdsAbility ads;			// 조준은 걸음 속도를 바꾼다 — 다시 돌릴 때 이 입력 뒤 상태에서 잇는다
			int16_t magazine = 0, spare = 0;
			uint8_t weapon = 0;
			uint8_t quickSlots = 0;	// 이 입력 뒤 가방(와이어 꼴 CLoadout::PackSlots) — 봇이 서버 상태와 견준다(W-3)
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
		// 부활을 기다리는 중이면 버린다 — 서버도 죽은 동안 입력을 안 쓴다(수류탄 쿨다운만 센다 — 서버 죽은 갈래와 같다, G-3)
		void AddInput(wire::Input& input, const Vec3* meleePush = nullptr);

		// 서버가 시작 자리로 옮겼다 — 그 번호 뒤의 입력을 새 자리에서 다시 돌린다
		// yaw = 놓인 요(와이어 Spawn.yaw) — 대시의 「앞 입력」(이동 없음 · 이 요)으로 넣는다(서버 SpawnPlayer 와 같다, D-3)
		void OnSpawn(double x, double y, double z, uint32_t lastSeq, float yaw);	// 와이어 Spawn 은 double — float 로 줄이면 같은 입력을 돌려도 어긋난다

		// 시험 자리 옮기기(C2S_DEBUG_PLACE)를 보낼 때(W-4 다리 무기 각본) — 다음 번호 하나를 이 옮기기에 쓰고, 서버 ApplyPlace(Room.cpp)와 같이
		// 걸음 · 점프 키 · 이동 막음만 새로 놓는다. 무기 · 가방 · 조준 · 근접은 그대로(서버도 안 건드린다 — OnSpawn 과 다른 점).
		// ammoWeapon < EWeapon::Count 면 그 무기 탄을 magazine · spare 로(DEBUG_PLACE_SET_AMMO). 돌려주는 값 = 보낼 DebugPlace.seq.
		// 속도 0 · 걷기(falling 0)로 보내는 옮기기만 따라 한다. 죽음(DEBUG_PLACE_KILL)은 여기서 안 다룬다 — 죽음 소식이 BeginWaitSpawn 으로 온다.
		// 그 번호의 서버 상태는 견주지 않고(부활 번호와 같은 자리), 그 번호로 집은 받침 소식(겹침을 옮긴 자리에서 다시 봄)은 이 칸에서 Give 한다
		uint32_t OnPlace(double x, double y, double z, uint8_t ammoWeapon, int16_t magazine, int16_t spare);

		// 내가 죽었다 — 부활 소식이 올 때까지 돌리지 않는다(서버도 죽은 동안 입력을 안 쓴다)
		void BeginWaitSpawn() { _waitingSpawn = true; }

		// 서버 상태와 같은 번호의 슬롯을 맞춰 본다. tolerance 를 넘으면 서버 값에서 다시 돌린다(위치·낙하 여부만, 탄은 서버 값을 그대로 보여 주면 된다)
		ReconcileResult Reconcile(const wire::PlayerState& server, double tolerance);

		// 서버가 아직 안 돌렸다고 본 입력(confirmedSeq 뒤 · 부활 뒤)을 그 앞 칸 상태에서 걸음만 다시 돌린다 — 걸음이 보는 바깥 몸(산 수류탄, L-8-b-2)이 바뀌었을 때.
		// 부르는 쪽이 새 몸을 걸고 부른다. 부활을 기다리는 중 · 돌릴 입력이 없음 · 앞 칸이 없으면 false
		bool RerunUnconfirmed(uint32_t confirmedSeq);

		// 다음 AddInput 에서 이 입력이 근접 능력을 켜나 — 켜질 입력에만 화면 근접 판정을 해 밀기를 넘기려고 넣기 전에 본다(E-3).
		// 대시가 켜져 있거나 이 입력에 켜지면 근접은 막힌다(D-2) — 대시 능력 사본을 한 입력 돌려 본다.
		// 이 입력에 수류탄을 던지면 도는 근접이 끊겨 같은 입력의 근접 누름이 켜진다(서버 TryMelee 가 ThrowGrenade 뒤, G-3) — 수류탄 사본도 돌려 본다
		bool WillMeleeStart(const wire::Input& input) const;

		// 서버 근접 결과(S2C_MELEE 중 공격자가 나) — 그 입력의 밀기를 서버 값으로 바꾸고, 예측과 다르면 그 입력부터 걸음을 다시 돌린다.
		// 서버는 이 소식을 그 입력을 돈 틱의 상태보다 먼저 보낸다(같은 연결, 판정이 입력 처리 안이고 상태는 틱 끝) — 상태를 맞춰 보기 전에 밀기가 고쳐져 있다
		void OnMeleeResult(uint32_t seq, bool push, const Vec3& force);

		// 서버 받침 소식 중 내가 무기 받침을 집은 것(W-3) — 무기 쪽(가방 · 탄 · 발사 문 · 조준 카메라)을 집기 직전으로 되살려 같은 Give 를 하고,
		// 결과 · 칸 · 탄이 소식과 다르면 소식 값으로 고쳐 센다. 그 뒤 입력의 무기 쪽을 다시 돌린다(걸음은 무기를 안 보므로 그대로).
		// 집기 직전 = 서버가 집은 자리. midInput(소식 PICKUP_TAKE) = 입력 seq 의 걸음 뒤 · 무기 순환 앞(Room.cpp ApplyInput 의 UpdatePadOverlaps) —
		// 앞 입력 뒤 칸에서 seq 의 교체를 다시 하고 Give 뒤 seq 의 순환을 돈다. 아니면(다시 검사 PICKUP_TAKE_RECHECK = 그 틱 입력 뒤 TickPads) seq 입력 뒤 칸에서 Give.
		// 부활 번호 그대로(seq = 부활 lastSeq)는 새 몸이 받침 안에 선 것이라 부활 칸에서 Give.
		// 예측은 받침 겹침을 스스로 짐작하지 않는다 — 남이 먼저 집으면 갈린다(weapon-pickup-plan.md 「예측」). 서버는 이 소식을 그 틱 상태보다 먼저 보낸다
		void OnPickup(uint32_t seq, bool midInput, uint8_t weapon, uint8_t give, uint8_t slot, int16_t magazine, int16_t spare);

		struct PickupCounters
		{
			uint64_t received = 0;			// 받은 내 무기 줍기 소식
			uint64_t rerun = 0;				// 그 칸에서 다시 돌림
			uint64_t noSlot = 0;			// 그 번호 칸이 없음(부활 전 목숨 · 기다리는 중 · 너무 옛 번호) — 못 고친다. 0 이어야 한다
			uint64_t resultDiffs = 0;		// 같은 Give 의 결과 · 칸이 소식과 다름 — 소식대로 고침. 0 이어야 한다
			uint64_t ammoFixes = 0;			// Give 뒤 그 무기 탄이 소식과 다름 — 소식대로 고침. 0 이어야 한다
			uint64_t meleeInWindow = 0;		// 다시 돌린 입력 중 근접을 켠 것 — 근접은 다시 안 돌린다(능력 길이가 무기마다 달라 갈릴 수 있다, 근접 셈이 잡는다)
		};
		const PickupCounters& GetPickupCounters() const { return _pickupCounters; }

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

		// 서버 대시 소식(내 것) — 서버가 입력 seq 에 대시를 켰다. 그 칸이 안 켰거나(서버의 앞 입력이 달랐다 — 빈 입력 등, dash-plan 「예측」) 방향 · 힘이 다르면
		// 그 칸을 서버 값으로 놓고 그 칸부터 대시를 다시 고른다(뒤 칸의 켬 · 막기 · 쿨다운 — 쿨다운 칸이 소식의 입력 번호로 맞는다) → 걸음을 다시 돌린다.
		// 무기 쪽은 다시 안 돈다(근접 · 줍기와 같은 범위). 서버는 이 소식을 그 입력을 돈 틱의 상태보다 먼저 보내야 한다(근접 소식과 같은 자리 — 보내는 쪽은 D-3-b)
		void OnDashResult(uint32_t seq, uint8_t dir, const Vec3& force);
		// 서버가 이 번호까지 입력을 돌렸다 — 그 안에서 내가 켰는데 소식이 없는 대시(서버는 안 켬)를 안 켠 것으로 놓고 다시 고른다. Reconcile 이 처음에 부른다(AckMelee 다음)
		void AckDash(uint32_t lastSeq);

		struct DashCounters
		{
			uint64_t presses = 0;			// 대시 단추가 든 입력(부활을 기다리는 중 제외) — AddInput 에서만 센다(부활 다시 돌리기는 안 셈)
			uint64_t started = 0;			// 그중 내가 켠 것(AddInput 때 판단, 다시 고르기로 바뀐 것은 안 셈)
			uint64_t respawnGained = 0;		// 부활 다시 돌리기(OnSpawn)에서 새로 켜진 칸 — 경기 시작은 산 사람도 다시 세워 쿨다운이 지워진다(서버 EnterPhase Playing)
			uint64_t respawnLost = 0;		// 같은 자리에서 꺼진 칸. 서버 켬 수 = started + respawnGained − respawnLost(고친 수가 0 일 때)
			uint64_t results = 0;			// 받은 서버 소식(내 것)
			uint64_t resultsNoSlot = 0;		// 받았지만 그 번호 칸이 없음(부활 전 목숨 · 기다리는 중)
			uint64_t startMismatches = 0;	// 서버는 켰는데 나는 안 켬 — 고침. 봇 판 기대 0
			uint64_t forceFixes = 0;		// 둘 다 켰는데 방향 · 힘이 다름 — 고침. 봇 판 기대 0
			uint64_t unconfirmed = 0;		// 내가 켰는데 서버 소식 없이 지나감(서버는 안 켬) — 고침. 봇 판 기대 0
			uint64_t unconfirmedDead = 0;	// 죽은 줄 모르고 켰는데 부활이 확인 번호를 옮기며 버린 칸(서버는 죽은 갈래로 셈) — 고침 없음. 서버 켬 = respawnLost 줄의 식 − 이 몫
			uint64_t replays = 0;			// 다시 고른 번
			uint64_t replaysMissed = 0;		// 다시 고를 앞 칸이 없어 못 함 — 다음 Reconcile 이 서버 자리로 고친다
			uint64_t meleeInReplay = 0;		// 다시 고르며 근접 끊음이 바뀐 칸 + 이제 대시가 막는데 근접이 켜진 칸 — 근접 능력은 다시 안 고른다(무기 쪽 근접 켜짐이 서버와 다를 수 있다).
											// 걸음 쪽 밀기는 근접 소식 · AckMelee 가 고친다. 대시 고침이 0 이면 0
		};
		const DashCounters& GetDashCounters() const { return _dashCounters; }
		bool IsDashActive() const { return _dash.Active(); }

		// 서버 수류탄 소식 중 내가 던진 것(G-3) — 서버가 입력 seq 에 던졌다. 그 칸이 안 던졌으면(앞 칸들이 서버와 갈렸다 — 죽음을 늦게 앎 · 대시 갈림.
		// 서버가 채운 빈 입력은 서버도 수류탄을 안 센다) 그 칸을 던짐으로 놓고 그 칸부터 수류탄을 다시 고른다(뒤 칸의 쿨다운 · 던짐). 근접 끊음이 바뀐 칸이 있으면 걸음을 다시 돌린다.
		// 무기 쪽(재장전 끊기)은 다시 안 돈다(근접 · 대시와 같은 범위). 서버는 이 소식을 그 입력을 돈 틱의 상태보다 먼저 보낸다(대시 소식과 같은 자리 — 보내는 쪽은 G-3-b)
		void OnGrenadeThrown(uint32_t seq);
		// 서버가 이 번호까지 입력을 돌렸다 — 그 안에서 내가 던졌는데 소식이 없는 것(서버는 안 던짐)을 안 던진 것으로 놓고 다시 고른다.
		// Reconcile 이 처음에 부른다 — 대시와 달리 부활 대기 · 죽음 중에도(서버는 죽은 입력에도 쿨다운을 센다).
		// serverDead = 이 확인을 실은 서버 상태가 죽음(STATE_DEAD). 그렇거나 내가 부활을 기다리는 중이면 지운 던짐을 unconfirmedDead 로 따로 센다.
		// 맞춰 보기만 하는 쪽(봇)도 상태마다 부른다 — 사람끼리 막기 판은 Reconcile 이 남 캡슐을 놓고 부른다(대시 확인과 같은 자리)
		void AckGrenade(uint32_t lastSeq, bool serverDead);

		struct GrenadeCounters
		{
			uint64_t presses = 0;			// 수류탄 단추가 든 입력(부활 대기 포함 — 서버도 죽은 입력의 누름을 센다) — AddInput 에서만 센다
			uint64_t thrown = 0;			// 그중 내가 던진 것(AddInput 때 판단, 다시 고르기로 바뀐 것은 안 셈)
			uint64_t respawnThrown = 0;		// 부활 다시 돌리기(OnSpawn)에서 새 몸으로 던짐이 된 칸 — 대기 중엔 죽은 갈래라 안 던졌다
			uint64_t respawnLost = 0;		// 같은 다시 돌리기에서 던짐이 아니게 된 칸(대기 칸은 안 던졌으니 0 이 보통)
			uint64_t results = 0;			// 받은 서버 던짐 소식(내 것)
			uint64_t resultsNoSlot = 0;		// 받았지만 그 번호 칸이 없음(고리를 넘은 옛 번호) — 못 고친다. 0 이어야 한다
			uint64_t startMismatches = 0;	// 서버는 던졌는데 나는 안 던짐 — 고침. 봇 판 기대 0
			uint64_t unconfirmed = 0;		// 내가 던졌는데 서버 소식 없이 지나감(서버는 안 던짐) — 고침. 봇 판 기대 0(아래 죽음 몫 뺀 수)
			// 그중 죽음을 늦게 안 몫 — 서버가 이미 죽은 몸으로 돌린 입력에 내가 산 몸으로 던졌다. 정직한 클라도 못 피한다(죽음 소식 전에 보낸 입력).
			// 대시는 부활이 쿨다운을 지워 이 고침이 필요 없지만 수류탄은 쿨다운이 목숨을 넘어 이어져 고친다. 서버 상태가 죽음인 확인으로 세므로,
			// 그 틱에 산 채로 돈 뒤 죽은 입력의 갈림도 여기 든다(그 앞이 이미 갈렸어야 생긴다)
			uint64_t unconfirmedDead = 0;
			uint64_t replays = 0;			// 다시 고른 번(ReplayDash 끝의 다시 고르기 포함)
			uint64_t replaysMissed = 0;		// 다시 고를 앞 칸이 없어 못 함. 0 이어야 한다
			uint64_t meleeInReplay = 0;		// 다시 고르며 근접 끊음이 바뀐 칸 — 걸음은 다시 돌리고, 근접 능력(무기 쪽)은 다시 안 고른다. 고침이 0 이면 0
			// 다시 고르기(ReplayGrenade)에서 던짐이 된 · 아니게 된 칸 — 소식 · 확인으로 고친 그 칸과 뒤 칸이 다 든다.
			// 칸의 던짐은 StepAbilities 와 여기서만 바뀌므로 「thrown + respawnThrown − respawnLost + replayGained − replayLost」 = 칸마다 마지막으로 정한 던짐의 합
			// (고리가 옛 칸을 덮어도 셈은 남는다, G-3-c 봇 판정)
			uint64_t replayGained = 0;
			uint64_t replayLost = 0;
		};
		const GrenadeCounters& GetGrenadeCounters() const { return _grenadeCounters; }

		// 점프대(jumppad-plan j-6) — 서버가 보낸 표(S2C_LAUNCHER)를 받아 서버 ApplyInput 과 같은 TickWithLaunchers 로 걷는다. 표가 없으면 점프대 없는 걸음 그대로.
		// 살아 있는 동안 받으면 지금 자리로 비트만 다시 센다(서버는 부활 소식보다 먼저 보내 보통은 부활 전에 다 온다)
		void SetLaunchers(const std::vector<Launcher>& table, double capsuleRadius);
		size_t GetLauncherCount() const { return _launcherTable.size(); }
		struct LaunchCounters
		{
			uint64_t fired = 0;			// 내 걸음 끝에 점프대가 쏨(AddInput 때만 센다 — 다시 돌리기는 안 셈)
			uint64_t applied = 0;		// 지난 걸음이 남긴 발사를 이번 걸음 머리에 넣음(AddInput 때만)
			uint64_t refused = 0;		// 겹침은 시작했는데 안 쏨(떨어지며 들어옴 — 지금 표 8개는 그 깃발이 다 꺼져 0)
			uint64_t restored = 0;		// 보정(Reconcile)이 서버 깃발로 발사를 다시 셈
			uint64_t restoreFails = 0;	// 서버 깃발은 켜졌는데 서버 자리에서 겹친 점프대가 없거나 안 쏘는 값 — 발사를 못 되살림. 0 이어야 한다
		};
		const LaunchCounters& GetLaunchCounters() const { return _launchCounters; }
		// 마지막 걸음이 쏜 점프대 번호(-1 = 안 쏨). AddInput 바로 뒤에만 그 입력 몫이다 — 보정의 다시 돌리기가 덮는다(다리가 내 몸 효과를 낼 점프대를 고른다)
		int GetLastLaunchPad() const { return _lastLaunch.fired; }
		// 아직 확인 안 된 번호(AckGrenade 가 본 번호 뒤) 중 서버 소식 없이 내가 던진 칸 — 판이 끝날 때 서버가 돌렸는지 모르는 몫(봇 판정에서 뺀다)
		uint64_t CountGrenadeThrownUnacked() const;
		// 대시도 같다 — 아직 확인 안 된 번호(AckDash 가 본 번호 뒤) 중 서버 소식 없이 내가 켠 칸. 판이 끝날 때 서버가 받고 안 돌린 입력의 몫(봇 판정에서 뺀다, L-11-c)
		uint64_t CountDashStartedUnacked() const;
		int GetGrenadeCooldownLeft() const { return _grenade.CooldownLeft(); }	// 남은 입력 수 — 다리가 던진 입력에 쿨다운 메시지를 낼 때 읽는다(grenade-plan 「예측」, G-5)

		// 맞춰 보기만 하는 쪽(봇)이 쓰는 슬롯. 비교할 수 없으면 nullptr
		const Slot* FindComparable(const wire::PlayerState& server) const;

		// 일부러 틀리기 — 시험이 잡는지 보려고
		void SetDebugMoveScale(double scale) { _debugMoveScale = scale; }
		void DebugNudge(const Vec3& by) { _move.location = _move.location + by; }

		const MoveState& GetMove() const { return _move; }
		const CWeaponCycle& GetWeapon() const { return _weapons[_weapon]; }
		uint8_t GetWeaponIndex() const { return _weapon; }
		const CLoadout& GetLoadout() const { return _loadout; }		// 봇이 가진 무기 중에서 고르려고(W-3)
		uint32_t GetSeq() const { return _seq; }
		// 방금 돌린 입력의 무기 결과 — 쐈으면 이 입력 번호와 퍼짐으로 서버와 같은 탄 방향을 구한다(D15)
		const WeaponEvents& GetLastWeaponEvents() const { return _lastWeaponEvents; }
		bool IsWaitingSpawn() const { return _waitingSpawn; }
		bool IsLocked() const { return _lockInputs > 0; }
		bool IsAds() const { return _ads.active; }

	private:
		// 입력 하나 뒤의 무기 쪽 — 줍기 소식이 그 입력에서 다시 돌리려고 남긴다. 걸음 쪽은 Slot 에 있다
		struct WeaponSnap
		{
			uint32_t seq = 0;
			WeaponCycles weapons;
			CLoadout loadout;
			uint8_t weapon = 0;
			CWeaponFireGate fireGate;
			CAimCamera aimCamera;
			bool meleeActive = false;	// 이 입력의 근접 능력이 켜져 있었나(StepAbilities 뒤 — 대시가 끊었으면 꺼짐) — 무기 순환이 읽는다
		};
		// 줍기 소식이 오기까지의 입력 수만 있으면 된다 — 왕복 + 서버 줄. 지연 시험(lag.sh) 최대 편도 150ms · 흔들림 50ms 면 왕복 400ms ≈ 24입력, 128 = 2.1초
		static constexpr uint32_t kWeaponSnapCount = 128;

		Vec3 ApplyMove(const Slot& slot);		// 입력 + 그 입력에 붙은 힘(근접 밀기 · 대시). 돌려주는 값 = 걸음이 쓴 이동(막음 뒤 · 일부러 틀림 배율 앞)
		// 근접 · 대시 능력 한 입력 — 서버 ApplyInput 처럼 걸음 앞(대시가 근접보다 먼저). count = 대시 누름 · 켬을 셀지(부활 다시 돌리기는 안 셈)
		void StepAbilities(Slot& slot, bool count);
		void RememberDash(Slot& slot, const Vec3& moveUsed);	// 걸음 뒤 — 다음 입력의 「앞 입력」 · 칸의 대시 사본
		bool ReplayDash(uint32_t seq);		// 그 앞 칸의 대시 상태에서 seq 부터 켬 · 막기를 다시 고른다(서버 소식 · 확인 못 함 칸을 따른다)
		// 바로 앞 칸의 수류탄 상태에서 seq 부터 던짐 · 근접 끊음을 다시 고른다(서버 소식 · 확인 칸을 따른다, 대시 막기는 칸 값). cutChanged = 근접 끊음이 바뀐 칸이 있다
		bool ReplayGrenade(uint32_t seq, bool& cutChanged);
		uint32_t MarkGrenadeAcked(uint32_t lastSeq, bool serverDead, bool& cutChanged);	// AckGrenade 의 걸음 앞 몫 — 다시 고른 첫 번호(0 = 없음)
		void ApplyWeapon(const Slot& slot, bool meleeActive);
		bool RerunMovesFrom(uint32_t seq);	// 그 앞 칸의 상태에서 seq 부터 걸음을 다시 돌린다
		void Equip(const wire::Input& input);
		void RememberMove(Slot& slot) const;
		void RememberWeapon(Slot& slot, bool meleeActive);		// 슬롯의 탄 · 가방 칸 + 무기 쪽 고리
		void SaveWeapon(WeaponSnap& snap, uint32_t seq, bool meleeActive) const;
		void LoadWeapon(const WeaponSnap& snap);

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
		CLoadout _loadout;		// 가방(W-2) — 서버 Room 과 같은 함수로 바꾼다
		std::array<WeaponSnap, kWeaponSnapCount> _weaponSnaps;
		WeaponSnap _spawnWeapon;	// 부활 직후 무기 쪽 — 부활 번호 그대로 집은 소식(새 몸이 받침 안에 섬)의 앞 칸
		// 마지막 자리 옮기기(OnPlace) — 그 번호로 집은 소식의 앞 칸(탄 정하기 앞)과 집은 뒤 다시 걸 탄 정하기. 부활이 끈다
		WeaponSnap _placeWeapon;
		bool _placeActive = false;
		uint8_t _placeAmmoWeapon = 0xFF;
		int16_t _placeMagazine = 0, _placeSpare = 0;
		PickupCounters _pickupCounters;
		uint8_t _weapon = 0;		// 든 무기 = _loadout.Held()
		JumpAbility _jump;			// 걸음 쪽 — 다시 돌리기가 칸에서 잇는다
		uint16_t _meleePushId = 0;	// 걸음 쪽 — 남은 근접 밀기 힘 번호
		CDashAbility _dash;			// 입력 쪽 — 다시 돌리기(걸음)는 칸에 남긴 켬 · 막기만 읽는다. 서버 소식이 다르면 ReplayDash 가 다시 고른다
		DashCounters _dashCounters;
		uint32_t _dashAckedSeq = 0;	// AckDash 가 본 마지막 번호
		CGrenadeAbility _grenade;	// 입력 쪽 — 목숨을 넘어 잇는다(서버 Player::grenade 도 부활에 안 지운다)
		GrenadeCounters _grenadeCounters;
		uint32_t _grenadeAckedSeq = 0;	// AckGrenade 가 본 마지막 번호
		int _lockInputs = 0;
		AdsAbility _ads;
		CAimCamera _aimCamera;		// 무기 쪽 — 다시 돌리기(Reconcile)는 걸음만 다시 하므로 건드리지 않는다
		bool _waitingSpawn = false;
		uint32_t _seq = 0;
		uint32_t _compareFromSeq = 0;
		double _debugMoveScale = 1.0;
		std::vector<Launcher> _launcherTable;	// 점프대 표 사본 — _launchers 가 가리킨다
		LauncherSet _launchers;
		double _capsuleRadius = 0;
		LaunchCounters _launchCounters;
		LauncherTickResult _lastLaunch;		// 마지막 ApplyMove 의 점프대 결과 — AddInput 이 센다
	};
}
