#pragma once

// 수류탄 능력 · 던짐(컨텐츠 G-2) — Lyra GA_Grenade 를 엔진 없이 옮긴 것. 정본 = MyDocs/claude-plans/mo-shooter/grenade-plan.md
// - 날아감은 Shared/Projectile(G-1). 여기는 켜기(쿨다운) · 던짐 계산(놓을 자리 · 겨눔 선 · 방향 · 첫 속도) · 폭발(사람마다 선 · 곡선 피해)
// - 서버 · 예측 · 대조 도구(Tools/GrenadeCheck)가 같은 함수를 쓴다
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다

#include "Projectile.h"
#include "Weapon.h"

#include <cstddef>
#include <cstdint>

namespace mo
{
	// ── 능력 — 쿨다운은 입력 수로 센다(입력 하나 = Lyra 기록 한 프레임, 60/초 · 대시와 같은 꼴)
	inline constexpr int kGrenadeCooldownInputs = 301;	// 다시 켜는 첫 입력 = 켠 입력 + 301 — G-0-c 실측(매 프레임 누름 간격 301 · 301 · 301, g0c2 · g0c3 같음).
														// 원본 GE_Grenade_Cooldown 5초 × 60 = 300 — 대시(1.5초 → 91)와 같이 하나 더 걸린다

	// 신관 — 생긴 프레임 걸음을 나이 0 으로 세어, 나이 120 걸음을 간 뒤 그 자리에서 터진다(TimeBeforeExplode 2.0초).
	// G-0-a 실측 = 타이머 터짐 18/18 이 120(121 은 0) · g0a1 B_Grenade_C_0 나이 120 줄 자리 = 터짐 자리(앞 줄 + 그 프레임 걸음)
	inline constexpr int kGrenadeFuseFrames = 120;

	// 신관이 다 됐나 — 이번 걸음까지 걸은 수(state.frames, 생긴 프레임 걸음 = 1)가 kGrenadeFuseFrames 를 넘었나. 참이 되는 첫 걸음 = 나이 120.
	// 서버 TickGrenades 와 GrenadeCheck 신관 대조가 같이 쓴다 — 한쪽만 바뀌면 대조가 못 잡으니 판정은 여기 하나에 둔다
	inline bool GrenadeFuseExpired(const ProjectileState& state) { return state.frames > kGrenadeFuseFrames; }

	// 누름 하나의 결과 — 던짐 말고는 모두 「안 켬」이라 동작은 같고, 통계(SERVER 줄)를 갈래마다 따로 세려고 나눈다.
	// 가르는 차례(죽음 → 대시 중 → 쿨다운)는 우리가 정한 것이다(대시 능력과 같은 차례). 경기 끝 단계엔 막지 않는다(대시와 같다 — Dash.h EDashResult)
	enum class EGrenadeResult : uint8_t
	{
		NotPressed,
		Thrown,
		OnCooldown,		// Ability.ActivateFail.Cooldown
		WhileDash,		// 대시가 Ability.Type.Action 을 막음(TagRelationships_ShooterHero 4번 · Ability.ActivateFail.TagsBlocked)
		Dead,			// 죽음 · 죽는 중
	};

	// 수류탄 능력 — 입력 하나의 차례: NextInput → (수류탄 단추를 누른 입력이면) TryActivate
	// 서버가 만든 빈 입력(serverMade)에는 둘 다 안 부른다 — 시계가 안 간다(D87, 근접 · 대시와 같다).
	// 쿨다운은 죽어도 안 풀린다(Lyra 는 쿨다운 효과가 선수 상태 ASC 에 있다 — G-0-b2 실측) → 부활에 Reset 을 부르지 않는다
	class CGrenadeAbility
	{
	public:
		void NextInput();
		EGrenadeResult TryActivate(bool dead, bool dashing);
		bool JustThrown() const { return _justThrown; }		// 이번 입력에 던졌나 — 재장전 · 근접 끊기는 이 입력에만
		int CooldownLeft() const { return _cooldownLeft; }	// 0 보다 크면 이번 입력에 못 켬
		// 서버가 이 입력에 던졌다(예측이 서버 소식으로 고칠 때) — 켜기 조건은 안 보고 쿨다운만 TryActivate 의 던짐과 같게 놓는다. NextInput 뒤에 부른다
		void StartFromServer();
		void Reset();		// 판 시작

	private:
		int _cooldownLeft = 0;
		bool _justThrown = false;
	};

	// ── 던짐 — GA_Grenade 그래프 「Get Spawn Location」 · 「Get Spawn Rotation」(원본 기준 ga_flow.txt:65-158)
	inline constexpr double kGrenadeSpawnForward = 5.0;		// 몸 앞 × 5
	inline constexpr double kGrenadeSpawnUp = 25.0;			// 몸 위 × 25 — 던진 사람 캡슐 안이다
	inline constexpr double kGrenadeTraceStart = 450.0;		// Start Trace Offset — 카메라에서 선 시작까지
	inline constexpr double kGrenadeTraceRange = 3150.0;	// Default Targeting Range — 카메라에서 선 끝까지
	inline constexpr double kGrenadeAimMinDistance = 45.0;	// 맞은 거리(선 시작에서) < 45 면 조종 회전 그대로 — 안 맞음도 거리 0 이라 이 갈래(G-0-a 짐작 1)

	// 카메라 거리(입력 camBack, G-3) — 카메라에서 입력 시작점까지 시선 방향 거리. 보내는 쪽이 싣고 서버는 G-0 에서 잰 범위로 자른다(플랜 101줄).
	// G-0 실측 20칸 = 서서 피치 0 283.98(앉음 · 점프 · 달리기 같음) · +45 133.5 · +60 102.8 · −20 329.6 · −35 364.8 · −60 417.3 · 등이 벽 30.0
	// (content-impl-progress.md G-0 「camBack」 줄). 봇은 대표값으로 서서 피치 0 을 싣는다
	inline constexpr double kGrenadeCamBackMin = 30.0;
	inline constexpr double kGrenadeCamBackMax = 417.3;
	inline constexpr double kGrenadeCamBackStanding = 283.98;
	// 와이어 값을 범위로 자른다. 유한 값만 넘길 것 — 서버는 NaN · 무한 입력을 통째로 버린다(Room.cpp IsFiniteInput)
	inline double ClampGrenadeCamBack(float camBack)
	{
		const double v = camBack;
		return v < kGrenadeCamBackMin ? kGrenadeCamBackMin : (v > kGrenadeCamBackMax ? kGrenadeCamBackMax : v);
	}

	// 놓을 자리 = 몸 위치(캡슐 중심) + 몸 앞 × 5 + 몸 위 × 25. 몸 회전은 요만 있다(사람 액터는 똑바로 선다)
	Vec3 GrenadeSpawnLocation(const Vec3& bodyCenter, double bodyYawDegrees);

	// 겨눔 선 구간 = 카메라 + 방향 × 450 ~ 카메라 + 방향 × 3150. 방향 = 카메라 회전(Conv_RotatorToVector)
	void GrenadeTraceSegment(const Vec3& camera, const Rotator& cameraRotation, Vec3& outStart, Vec3& outEnd);

	// 겨눔 선 결과(LineTraceSingle · 무기 채널 · 던진 사람 뺌) — 안 맞으면 hit = false · distance = 0(BreakHitResult 기본값)
	struct GrenadeAimHit
	{
		bool hit = false;
		double distance = 0;	// 선 시작에서 맞은 곳까지
		Vec3 location;			// 맞은 곳(선이라 ImpactPoint 와 같다)
		bool body = false;		// 사람 부위 몸에 맞음(아니면 맵)
		uint32_t bodyId = 0;	// 맞은 몸의 BodyPlacement::id
		uint32_t shape = 0;		// 맞은 부위(BodyHit::shape)
	};

	// 맵(무기 깃발) + 사람 부위 몸 중 가장 가까운 것. bodies 중 id == ignoreId 는 뺀다(던진 사람, bIgnoreSelf).
	// Lyra 는 사람 메시 물리 몸에 맞는다(G-0 기록 comp=CharacterMesh0) — 우리 쪽은 총알과 같은 부위 몸(TraceBody)
	// 맵 몫은 Lyra 와 모양이 다르다 — Lyra 겨눔 선은 단순 충돌(bTraceComplex 기본 false)인데 맵 파일엔 「단순 모양 + 무기 채널」 깃발이 없어
	// 무기 깃발(복잡 모양)로 쏜다. 선 시작(카메라 앞 450)이 상자 안이면 Lyra 는 거리 0(→ 조종 회전), 우리는 반대쪽 면에 맞는다
	// (gym 기록 겨눔 490 중 28) — content-impl-progress.md 미결 21
	GrenadeAimHit TraceGrenadeAim(const CMapCollision& map, const Vec3& start, const Vec3& end,
		const BodyPlacement* bodies, size_t bodyCount, uint32_t ignoreId);

	enum class EGrenadeAimBranch : uint8_t
	{
		Control,	// 맞은 거리 < 45(안 맞음 포함) — 조종 회전 그대로
		LookAtHit,	// 놓을 자리에서 맞은 곳을 보는 회전
		LookAtEnd,	// 거리 ≥ 45 인데 안 맞음 — 그래프에 있는 갈래(Select 의 거짓 쪽). 안 맞으면 거리가 0 이라 실제로는 안 온다
	};

	struct GrenadeThrow
	{
		Vec3 location;			// 놓을 자리 = 수류탄 뿌리(메시)
		Rotator rotation;		// 생길 때 회전 — 날아감의 몸 축(기운 캡슐)
		Vec3 velocity;			// 첫 속도 = 회전 × 정규화(1, 0, 0.15) × 2500(LaunchVelocity)
		EGrenadeAimBranch branch = EGrenadeAimBranch::Control;
	};

	// 놓을 자리 · 조종 회전 · 겨눔 선 결과 → 던짐. traceEnd = LookAtEnd 갈래가 보는 끝점
	GrenadeThrow ComputeGrenadeThrow(const Vec3& spawnLocation, const Rotator& controlRotation, const Vec3& traceEnd,
		const GrenadeAimHit& aim, const ProjectileParams& params);

	// ── 폭발(G-2-b) — 피해 = 105 × 곡선(clamp(선 거리 ÷ 450, 0.1, 1)). 사람마다 수류탄 자리에서 캡슐 가운데로 선 하나(G-0-b 실측)
	inline constexpr double kGrenadeBlastRadius = 450.0;	// 겹침 반지름 = 등급 분모
	inline constexpr double kGrenadeBlastDamage = 105.0;	// 곡선 값 1 일 때 피해
	inline constexpr double kGrenadeBlastMinGrade = 0.1;	// 등급 아래 끝 — 붙어 터져도 곡선(0.1) × 105 = 104.42

	// 곡선 Curve_GrenadeDamage — 등급 0.1~1.0 을 0.001 간격 901점 표(GrenadeDamageCurve.inl)로 직선 보간. 범위 밖은 끝 값
	double GrenadeDamageCurve(double grade);

	// 폭발 선의 과녁 = 사람 캡슐(똑바로 선다)
	struct GrenadeBlastTarget
	{
		Vec3 center;				// 캡슐 중심 = 액터 위치 — 선 끝
		double radius = 35.0;
		double halfHeight = 90.0;	// 반구 포함. 앉으면 65(MoveParams::crouchedHalfHeight)
	};

	struct GrenadeBlastHit
	{
		bool blocked = false;		// 맵이 몸보다 먼저 맞음 — 피해 0(D107). 몸까지 450 이상이면 맵을 안 봐서 false
		double bodyDistance = 0;	// 선을 따라 캡슐 겉까지. 캡슐 안에서 터지면 0
		double distance = 0;		// 첫 맞음까지 — 막혔으면 맵, 아니면 bodyDistance
		double grade = 1.0;			// clamp(bodyDistance ÷ 450, 0.1, 1)
		double damage = 0;			// 105 × 곡선(등급). 막혔으면 0 — 팀 · 무적 · 죽음은 GrenadeDamageApplies
	};

	// 수류탄 자리 → 캡슐 가운데 선(Lyra = Weapon_Capsule 채널 · 복잡 충돌 → 맵 MAP_BLOCK_BLAST).
	// - 다른 사람 · 수류탄은 막지 않는다 — Lyra 무시 목록 = 겹침 안 다른 사람 + 그 수류탄이고, 몸까지 450 안의 선 위에 걸리는 사람은 반드시 겹침 안이다
	// - 겹침(450 구)은 따로 안 본다 — 겹침 밖이면 선 거리도 450 이상이라 곡선 0, 피해가 같다(G-0-b 짐작 6). 그래서 몸까지 450 이상이면 맵 선을 안 쏜다
	GrenadeBlastHit TraceGrenadeBlast(const CMapCollision& map, const Vec3& at, const GrenadeBlastTarget& target);

	// 그 사람에게 피해가 들어가나 — Lyra 팀 규칙(던진 사람 자신은 들어감 · 같은 팀 0, LyraTeamSubsystem::CanCauseDamage) · 무적(부활 · 경기 끝) ·
	// 죽은 몸(Lyra 는 죽기 시작하면 충돌을 꺼 겹침 · 선에 안 걸린다). 던진 사람의 생사는 안 본다(죽은 뒤에도 피해 — G-0-b2 짐작 3)
	bool GrenadeDamageApplies(bool alive, bool self, bool sameTeam, bool immune);
}
