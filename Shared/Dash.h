#pragma once

#include "Movement.h"

#include <cstdint>

// 대시(컨텐츠 대시) — Lyra GA_Hero_Dash 를 엔진 없이 옮긴 것. 정본 = MyDocs/claude-plans/mo-shooter/dash-plan.md
// - D-1 = 힘 하나(방향 → 힘 벡터 · 힘 설정). D-2 = 방향 고르기 · 능력(켜짐 · 쿨다운 · 켜기 조건). 막기 · 끊기는 무기 순환 · 서버 차례 쪽(Weapon · Room)
//   막기 · 끊기의 갈래 = 무기 순환(WeaponInput 의 대시 두 칸) · 조준(AdsAbility::Apply) · 점프(JumpAbility) · 근접(CMeleeAbility::Cut) — 서버와 대조 도구가 같은 함수를 쓴다
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다
namespace mo
{
	// 그래프 변수 · 노드 핀(원본 기준 — dash-plan.md 「Lyra」 표 「힘」 · 「힘이 끝나면」 줄)
	inline constexpr float kDashStrength = 1850.f;			// Strength — 힘 크기(초당 cm). D-0 실측 = 서서 누르면 켠 프레임 속도 1850
	inline constexpr float kDashForceDuration = 0.25f;		// RootMotionDuration — 60fps 에서 15프레임(D-0 실측 힘 프레임 79~93)
	inline constexpr float kDashClampOnFinish = 1000.f;		// ClampVelocityOnFinish — 힘이 끝나면 수평을 이 크기로 · 위를 이 값 이하로 자른다

	// 몽타주 넷 = 방향 넷(SelectDirectionalMontage)
	enum class EDashDir : uint8_t
	{
		Forward,	// 카메라 앞으로 — 위를 보면 위로도 뜬다
		Backward,	// 나머지 셋 = 마지막 이동 입력 쪽
		Left,
		Right,
	};

	// 노드 핀 bEnableGravity=false(위아래도 더함) · VelocityOnFinishMode=ClampVelocity · ClampVelocityOnFinish=1000
	RootForceSettings DashForceSettings();

	// 대시 방향(정규화 전) — 앞이면 카메라 앞 × (1, 1, 1.2), 그 밖이면 마지막 이동 입력(월드 축, 길이는 상관없다). 능력이 이 값으로 「방향 0」을 가른다
	Vec3 DashDirection(EDashDir dir, const Vec3& lastInput, float viewPitch, float viewYaw);

	// 힘 벡터 = 방향(DashDirection)의 단위 벡터 × 1850.
	// 작업이 방향을 정규화한다(엔진 AbilityTask_ApplyRootMotionConstantForce.cpp:40) — 그래서 위를 본 앞 대시는 수평이 준다(D-0 pitch_up20 수평 1695.35).
	// viewPitch · viewYaw = 카메라(= 조종 회전) 도. 방향이 0 이면 0 을 돌려준다 — 능력은 그 전에 취소한다(CDashAbility::TryActivate)
	Vec3 DashForce(EDashDir dir, const Vec3& lastInput, float viewPitch, float viewYaw);

	// ── 능력(D-2) — 켜짐 · 쿨다운은 입력 수로 센다(입력 하나 = Lyra 기록 한 프레임, 60/초)
	inline constexpr int kDashActiveInputs = 32;	// 켠 입력부터 다른 행동(Ability.Type.Action)이 막히는 입력 수 — D-0 실측(기록 켜짐 31프레임 + 꺼지는 프레임의 입력,
													// 쥔 사격이 켠 입력 + 32 에 켜짐). 원본 AbilityDuration 0.55초 × 60 = 33 과 다르다(까닭 미확인, dash-plan 「능력 끝」 줄)
	inline constexpr int kDashCooldownInputs = 91;	// 다시 켜는 첫 입력 = 켠 입력 + 91 — D-0 실측(매 프레임 누름 간격 91 · 91 · 91, 기록 쿨다운 남은 초가 0 보다 큰 줄 90개). 원본 GE_HeroDash_Cooldown 1.5초 × 60 = 90

	// 방향 고르기(SelectDirectionalMontage) — d = (마지막 이동 입력의 요) − (몸 앞의 요)를 (−180, 180] 로 접은 값.
	// |d| < 45 → 앞 · |d| > 135 → 뒤 · d < 0 → 왼 · 그 밖 → 오른(±45 · ±135 는 옆 — D-0 대각 넷). 이동 입력 0 의 요 = 0 이라 d = −몸 요
	// lastInput = 켜는 입력의 앞 입력 이동(월드 축), bodyYaw = 그때 몸 요(도)
	EDashDir SelectDashDir(const Vec3& lastInput, float bodyYaw);

	// 누름 하나의 결과 — 켬 말고는 모두 「안 켬」이라 동작은 같고, 통계(SERVER 줄)를 갈래마다 따로 세려고 나눈다.
	// 가르는 차례(죽음 → 켜져 있음 → 쿨다운 → 방향 0)는 우리가 정한 것이다 — 켜져 있는 동안은 쿨다운도 걸려 있어 둘이 겹친다.
	// 경기 끝 단계엔 막지 않는다 — Lyra 는 능력 입력 막음(GE_BlockAbilityInput)을 맵을 다시 열기 한 틱 전에야 건다(그래프 원본 · 기록 g0end1 · 2, L-3-b).
	// 우리 서버는 그 틱에 경기를 닫아(Finished) 입력을 안 돌린다
	enum class EDashResult : uint8_t
	{
		NotPressed,
		Started,
		NoDirection,	// 방향 0 — 취소, 쿨다운 안 걸림(원본 기준 Vector_IsNearlyZero → K2_CancelAbility, CommitAbility 앞)
		WhileActive,	// 켜진 동안 누름
		OnCooldown,
		Dead,
	};

	struct DashStart
	{
		EDashResult result = EDashResult::NotPressed;
		EDashDir dir = EDashDir::Forward;	// Started · NoDirection 일 때 고른 방향
		Vec3 force;							// Started 일 때만
	};

	// 대시 능력 — 서버 · 예측 · 대조 도구가 같이 쓴다. 입력 하나의 차례:
	//   NextInput → (대시 단추를 누른 입력이면) TryActivate → … 걸음 … → RememberInput(이번 입력의 이동 · 요)
	// 서버가 만든 빈 입력(serverMade)에는 NextInput · TryActivate 를 안 부른다 — 시계가 안 간다(D87, 근접 · 무기 시계와 같다).
	// RememberInput 은 그 입력에도 부른다 — 걸음이 그 입력으로 돌았으니 다음 입력의 「앞 입력」은 그것이다
	class CDashAbility
	{
	public:
		void NextInput();
		// 방향은 앞 입력(RememberInput 으로 기억한 이동 · 몸 요)으로, 힘의 카메라는 이번 입력(viewPitch · viewYaw)으로.
		// 앞 입력인 까닭 = 능력은 입력 처리에서 켜지고, 이번 입력은 이동 계산이 아직 안 가져갔다
		// (엔진 GetLastMovementInputVector · D-2 기록 turn_left · turn_back: 누른 프레임에 입력을 옆 · 뒤로 바꿔도 앞 몽타주, turn_back 은 뒤 입력인데 +X 로 521cm)
		DashStart TryActivate(float viewPitch, float viewYaw, bool dead);
		void RememberInput(const Vec3& move, float yaw) { _prevMove = move; _prevYaw = yaw; }
		bool Active() const { return _activeLeft > 0; }			// 이번 입력에 다른 행동을 막나 — 켠 입력 포함 kDashActiveInputs 개
		bool JustStarted() const { return _justStarted; }		// 이번 입력에 켰나 — 끊기(재장전 · 근접 · 조준 · 자동 사격 · 점프)는 이 입력에만
		// 막기(D-2) — 켠 입력부터 kDashActiveInputs 개. 새로 누른 근접 · 재장전 · 점프 · 조준은 켠 입력에도 막힌다(TryActivate 가 TryMelee · 점프보다 먼저다)
		bool BlocksPresses() const { return Active(); }
		// 쥔 사격만 켠 입력에 먼저 켜진다 — Lyra 는 쥠 능력(WhileInputActive = 사격 셋)을 누름 능력(대시)보다 먼저 켜고(LyraAbilitySystemComponent.cpp:232-283),
		// 대시가 켜지며 끊는다. D-0 기록 fire_then_dash 3751 = 켠 프레임에 탄창 10 → 9 · 사격 꺼짐. 그 뒤 입력부터 막힌다
		bool BlocksHeldFire() const { return Active() && !_justStarted; }
		int CooldownLeft() const { return _cooldownLeft; }		// 0 보다 크면 이번 입력에 못 켬
		// 서버가 이 입력에 켰다(예측이 서버 소식으로 고칠 때, D-3) — 켜기 조건 · 방향은 안 보고 켜짐 · 쿨다운만 TryActivate 의 켬과 같게 놓는다. NextInput 뒤에 부른다
		void StartFromServer();
		void Reset();		// 부활 · 판 시작 — 켜짐 · 쿨다운 · 앞 입력을 지운다(몸 요가 있으면 부르는 쪽이 RememberInput 으로 다시 넣는다)

	private:
		int _activeLeft = 0;
		int _cooldownLeft = 0;
		bool _justStarted = false;
		Vec3 _prevMove;
		float _prevYaw = 0.f;
	};
}
