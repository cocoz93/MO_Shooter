#pragma once

// 날아가는 물체(수류탄) — 언리얼 UProjectileMovementComponent 의 걸음 중 B_Grenade 설정이 타는 갈래만 옮긴다(G-1)
// - 옮긴 것: 걸음 고리 · 걸음 쪼개기 · 속도 · 옮김(v0·t + ½(v1 − v0)·t) · 회전 따름 · 쓸어 옮기기
//   맞은 뒤(G-1-c): 겹침 풀기(SafeMoveUpdatedComponent) · 튕김 · 미끄러짐 · 구석 · 멈춤 · 튕김 덧반복
// - 안 옮긴 것: 유도(꺼짐) · 보간(화면 몫) · 평면 묶기(꺼짐) · 물리 시뮬(안 씀) · 세계 밖 검사(KillZ — 연습장 기록에 없음)

#include <cstddef>
#include <cstdint>
#include "MapCollision.h"

namespace mo
{
	// 언리얼 FRotator 와 같은 뜻(도) — 피치는 y 축, 요는 z 축, 굴림은 x 축 둘레
	struct Rotator
	{
		double pitch = 0, yaw = 0, roll = 0;
	};

	Vec3 RotateVector(const Rotator& rotation, const Vec3& v);	// FRotationMatrix(rotation).TransformVector(v)
	Rotator RotatorFromDirection(const Vec3& direction);			// FVector::ToOrientationRotator — 굴림 0

	struct ProjectileParams
	{
		// B_Grenade 의 ProjectileMovement 틀 값(원본 기준 — 플랜 27 · 28줄 · dump_gren2.txt)
		float initialSpeed = 2500.f;
		float maxSpeed = 2550.f;
		Vec3 launchDirection{ 1.0, 0.0, 0.15 };		// 몸 축 기준 처음 방향 — 크기는 처음 속도가 덮는다
		float gravityZ = -980.f;						// 세계 중력 × 배율 1
		float bounciness = 0.3f;
		float friction = 0.8f;
		float minFrictionFraction = 0.8f;
		float stopSpeed = 25.f;							// 튕긴 뒤 이보다 느리면 멈춤
		// 튕김 각이 마찰을 바꾸나 — 엔진 기본 꺼짐(ProjectileMovementComponent.cpp:60). 덤프에 안 나와 첫 튕김 수치로 확인(G-1-c — 마찰 0.8 그대로여야 기록 새 속도가 나온다)
		bool bounceAngleAffectsFriction = false;
		float maxStepSeconds = 0.05f;					// 걸음 쪼개기 — 한 걸음 최대 시간
		int maxIterations = 4;
		int bounceExtraIterations = 1;
		// 몸 = 메시 SM_grenade 의 캡슐 하나(메시 축 기준 가운데 z 7 · 반지름 5 · 반높이 9, 플랜 29줄)
		double capsuleRadius = 5.0;
		double capsuleHalfHeight = 9.0;
		Vec3 capsuleOffset{ 0.0, 0.0, 7.0 };
		// 생긴 뒤 이 시간이 지나면 던진 사람과 다시 부딪히고 회전이 속도를 따른다(PostLaunchCleanup 타이머, 플랜 30줄)
		float launchCleanupSeconds = 0.16f;
	};

	enum class EProjectileHitKind : uint8_t
	{
		Map,
		Pawn,
		Projectile,
	};

	// 맵 밖의 막는 것 — 사람 캡슐 · 다른 수류탄. 캡슐은 가운데 · 축(단위 벡터) · 반지름 · 반높이
	struct ProjectileObstacle
	{
		Vec3 center;
		Vec3 axis{ 0.0, 0.0, 1.0 };
		double radius = 0;
		double halfHeight = 0;
		uint32_t id = 0;
		EProjectileHitKind kind = EProjectileHitKind::Pawn;
		bool owner = false;								// 던진 사람 — 생긴 뒤 한동안 서로 무시
		// 맞음 알림이 나가면 그 자리에서 걸음을 끝낸다(다른 팀 사람 — B_Grenade ReceiveHit → Detonate). 누가 그런지는 부르는 쪽이 정한다
		bool detonateOnHit = false;
	};

	// 막혀 맞은 것 하나 — 쓸어 옮기기 한 번의 막는 맞음(언리얼 FHitResult 중 쓰는 칸)
	struct ProjectileHit
	{
		EProjectileHitKind kind = EProjectileHitKind::Map;
		uint32_t id = 0;								// 맵이면 물체 번호, 아니면 막는 것의 id
		double time = 0;								// 이 쓸기 안에서 멈춘 시각(뒤로 물린 값, 언리얼 FHitResult::Time)
		double contactDistance = 0;						// 쓸기 시작에서 맞닿은 곳까지(물리기 전)
		Vec3 normal;
		Vec3 impactNormal;								// 겹침 무시 · 겹침 고르기가 이것을 본다
		Vec3 impactPoint;
		double penetrationDepth = 0;					// 시작 겹침일 때 빠져나갈 거리
		bool startPenetrating = false;
		Vec3 traceStart, traceEnd;						// 이 쓸기의 뿌리 시작 · 끝
		// 맞음 알림(ReceiveHit)이 나갔나 — 겹침 풀기 첫 시도의 시작 겹침은 안 나간다(MOVECOMP_DisableBlockingOverlapDispatch)
		bool dispatched = false;
		bool bounced = false;							// 이 맞음으로 튕김 셈(HandleImpact)이 돌았다
	};

	// 갈래마다 지나간 수 — 시험이 「안 탄 갈래」를 가려낸다
	struct ProjectileCounters
	{
		uint32_t steps = 0;								// 걸음 고리 한 바퀴
		uint32_t blockedSteps = 0;						// 그 바퀴가 막힘으로 끝남
		uint32_t extraIterations = 0;					// 튕김 덧반복으로 되돌린 바퀴
		uint32_t startPenetrating = 0;					// 첫 시도가 시작 겹침
		uint32_t resolveMoved = 0;						// 겹침 풀기가 빠져나감(쓸기 한 번)
		uint32_t resolveCombined = 0;					// 두 법선을 더해 다시
		uint32_t resolveWithMove = 0;					// 풀기 + 원래 옮김으로 다시
		uint32_t resolveFailed = 0;
		uint32_t ignoredOverlaps = 0;					// 빠져나가는 겹침이라 무시
		uint32_t minMoveZeroed = 0;						// 너무 짧은 옮김을 0 으로
		uint32_t bounces = 0;
		uint32_t stopsOnImpact = 0;						// 튕긴 속도가 문턱 아래
		uint32_t stopsOnSlide = 0;						// 미끄러진 속도가 문턱 아래
		uint32_t multiHits = 0;							// 바로 앞 맞음에 이어 시각 0 맞음
		uint32_t slides = 0;							// 면을 따라 미끄러짐
		uint32_t corners = 0;							// 두 면 사이 구석 — 두 법선의 외적 방향
		uint32_t slideMoves = 0;						// 미끄러져 남은 시간을 옮김
		uint32_t slideHits = 0;							// 그 옮김이 다시 막힘
		uint32_t slideFriction = 0;						// 면으로 누르는 힘에 마찰
		uint32_t detonations = 0;
	};

	struct ProjectileState
	{
		Vec3 location;									// 뿌리(메시) 자리
		Rotator rotation;
		Vec3 velocity;
		int frames = 0;									// 생긴 프레임 = 0 에서 걸은 프레임 수
		bool launchCleanedUp = false;					// 타이머가 돌았나 — 돈 뒤로 회전이 속도를 따르고 던진 사람과 부딪힌다
		bool stopped = false;							// 멈춤(StopSimulating) — 속도 0, 더는 안 걷는다
		bool detonated = false;							// detonateOnHit 인 것에 맞음 알림이 나가 그 자리에서 걸음을 끝냄
		// 프레임을 넘어 이어지는 엔진 칸(언리얼 PreviousHitTime · PreviousHitNormal · bIsSliding, 처음 값 :61-63)
		float previousHitTime = 1.f;
		Vec3 previousHitNormal{ 0.0, 0.0, 1.0 };
		bool sliding = false;
		ProjectileCounters count;
	};

	Vec3 LaunchVelocity(const Rotator& spawnRotation, const ProjectileParams& p);	// 회전 × 정규화(처음 방향) × 처음 속도

	// 한 프레임 걸음 — 언리얼 TickComponent 의 고리. 막는 것 = 맵(수류탄 깃발) + obstacles
	// hits 에 이번 프레임 막는 맞음을 차례로 담는다(maxHits 를 넘는 것은 세기만). 돌려주는 값 = 맞음 수
	size_t TickProjectile(const CMapCollision& map, const ProjectileParams& p, ProjectileState& s, float deltaSeconds,
		const ProjectileObstacle* obstacles, size_t obstacleCount, ProjectileHit* hits, size_t maxHits);

	// 기운 캡슐 둘의 쓸기 — a 가 delta 만큼 옮겨 갈 때 b 에 처음 닿는 시각(0~1). 두 캡슐 거리가 옮김에 대해 볼록이라 가장 가까운 때를 황금 분할로, 그 앞에서 처음 닿는 때를 이분법으로 찾는다
	// 시작부터 겹치면 시각 0 · 겹친 깊이 = 두 반지름 합 − 축 거리
	bool SweepCapsuleCapsule(const Vec3& centerA, const Vec3& axisA, double radiusA, double halfHeightA, const Vec3& delta,
		const Vec3& centerB, const Vec3& axisB, double radiusB, double halfHeightB, double& outTime, Vec3& outNormal, Vec3& outImpact,
		bool& outStartPenetrating, double& outPenetrationDepth);
}
