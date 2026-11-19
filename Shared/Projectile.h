#pragma once

// 날아가는 물체(수류탄) — 언리얼 UProjectileMovementComponent 의 걸음 중 B_Grenade 설정이 타는 갈래만 옮긴다(G-1)
// - 옮긴 것: 걸음 고리 · 걸음 쪼개기 · 속도 · 옮김(v0·t + ½(v1 − v0)·t) · 회전 따름 · 쓸어 옮기기
// - 안 옮긴 것: 유도(꺼짐) · 보간(화면 몫) · 평면 묶기(꺼짐) · 물리 시뮬(안 씀)
// - 맞은 뒤 처리(튕김 · 미끄러짐 · 멈춤)는 G-1-c 에서 넣는다 — 지금은 첫 부딪힘에서 그 걸음을 멈추고 알린다

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
	};

	struct ProjectileHit
	{
		EProjectileHitKind kind = EProjectileHitKind::Map;
		uint32_t id = 0;								// 맵이면 물체 번호, 아니면 막는 것의 id
		double time = 0;								// 이 걸음 안에서 멈춘 시각(뒤로 물린 값, 언리얼 FHitResult::Time)
		double contactDistance = 0;						// 걸음 시작에서 맞닿은 곳까지(물리기 전)
		Vec3 normal;
		Vec3 impactPoint;
		bool startPenetrating = false;
	};

	struct ProjectileState
	{
		Vec3 location;									// 뿌리(메시) 자리
		Rotator rotation;
		Vec3 velocity;
		int frames = 0;									// 생긴 프레임 = 0 에서 걸은 프레임 수
		bool launchCleanedUp = false;					// 타이머가 돌았나 — 돈 뒤로 회전이 속도를 따르고 던진 사람과 부딪힌다
		bool stopped = false;
	};

	Vec3 LaunchVelocity(const Rotator& spawnRotation, const ProjectileParams& p);	// 회전 × 정규화(처음 방향) × 처음 속도

	// 한 프레임 걸음 — 언리얼 TickComponent 의 고리. 막는 것 = 맵(수류탄 깃발) + obstacles
	// hits 에 이번 프레임 부딪힘을 차례로 담는다(maxHits 를 넘는 것은 세기만). 돌려주는 값 = 부딪힘 수
	size_t TickProjectile(const CMapCollision& map, const ProjectileParams& p, ProjectileState& s, float deltaSeconds,
		const ProjectileObstacle* obstacles, size_t obstacleCount, ProjectileHit* hits, size_t maxHits);

	// 기운 캡슐 둘의 쓸기 — a 가 delta 만큼 옮겨 갈 때 b 에 처음 닿는 시각(0~1). 두 캡슐 거리가 옮김에 대해 볼록이라 가장 가까운 때를 황금 분할로, 그 앞에서 처음 닿는 때를 이분법으로 찾는다
	bool SweepCapsuleCapsule(const Vec3& centerA, const Vec3& axisA, double radiusA, double halfHeightA, const Vec3& delta,
		const Vec3& centerB, const Vec3& axisB, double radiusB, double halfHeightB, double& outTime, Vec3& outNormal, Vec3& outImpact, bool& outStartPenetrating);
}
