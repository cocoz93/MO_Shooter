#pragma once

#include "MapCollision.h"

#include <cstdint>

// 언리얼 UCharacterMovementComponent 의 걷기·낙하를 엔진 없이 옮긴 것.
// - 왜: 내 캐릭터는 클라와 서버가 같은 코드로 움직여야 한다. 정답지는 Lyra 실행값이므로 계산 순서·자료형(float/double)까지 엔진을 따른다.
// - 옮기지 않은 것: 움직이는 받침, 루트 모션, 수영·비행, 웅크리기, 경사 예외(WalkableSlopeOverride). Lyra 첫 판 범위 밖이다.
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다.
namespace mo
{
	// 실행값. Lyra 는 블루프린트가 C++ 기본값을 덮으므로 기록 CSV 머리에서 읽어 넣는다
	struct MoveParams
	{
		float maxWalkSpeed = 600.f;
		float maxAcceleration = 2048.f;
		float groundFriction = 8.f;
		float brakingDecelerationWalking = 2048.f;
		float brakingDecelerationFalling = 0.f;
		float brakingFriction = 0.f;
		float brakingFrictionFactor = 2.f;
		float brakingSubStepTime = 1.f / 33.f;
		float gravityZ = -980.f;
		float jumpZVelocity = 420.f;
		float jumpMaxHoldTime = 0.f;
		float airControl = 0.05f;
		float airControlBoostMultiplier = 2.f;
		float airControlBoostVelocityThreshold = 25.f;
		float fallingLateralFriction = 0.f;
		float maxStepHeight = 45.f;
		float perchRadiusThreshold = 0.f;
		float perchAdditionalHeight = 40.f;
		float walkableFloorZ = 0.71f;
		float maxSimulationTimeStep = 0.05f;
		float maxDepenetrationWithGeometry = 500.f;
		float terminalVelocity = 4000.f;
		float minAnalogWalkSpeed = 0.f;
		float capsuleRadius = 35.f;
		float capsuleHalfHeight = 90.f;
		int maxSimulationIterations = 8;
		int maxJumpApexAttemptsPerSimulation = 2;
		int jumpMaxCount = 1;
		bool useSeparateBrakingFriction = false;
		bool applyGravityWhileJumping = true;
		bool dontFallBelowJumpZVelocityDuringJump = true;
		bool canJump = true;
	};

	// Lyra 실행값 — 블루프린트가 덮은 뒤의 값(move_expanse_fixed.csv 머리). Tools/ServerProbe 가 기록과 대조한다
	MoveParams MakeLyraMoveParams();

	enum class EMoveMode : uint8_t
	{
		Walking,
		Falling,
	};

	// FHitResult 에서 이동이 쓰는 칸만
	struct MoveHit
	{
		bool blocking = false;
		bool startPenetrating = false;
		float time = 1.f;
		float penetrationDepth = 0.f;
		Vec3 location;			// 닿은 순간의 캡슐 중심(뒤로 물리기 전)
		Vec3 impactPoint;
		Vec3 normal;
		Vec3 impactNormal;
		Vec3 traceStart;
		Vec3 traceEnd;

		bool IsValidBlockingHit() const { return blocking && !startPenetrating; }
	};

	struct FloorResult
	{
		bool blockingHit = false;
		bool walkableFloor = false;
		bool lineTrace = false;
		float floorDist = 0.f;
		float lineDist = 0.f;
		MoveHit hit;

		bool IsWalkableFloor() const { return blockingHit && walkableFloor; }
		float GetDistanceToFloor() const { return lineTrace ? lineDist : floorDist; }
	};

	// 한 캐릭터가 틱 사이에 들고 있어야 하는 것 전부
	struct MoveState
	{
		Vec3 location;			// 캡슐 중심
		Vec3 velocity;
		Vec3 acceleration;
		EMoveMode mode = EMoveMode::Falling;
		FloorResult floor;
		float maxSpeed = 600.f;		// 이번 틱 최대 속도(Lyra 는 이동 금지 태그가 붙으면 0 을 준다)
		float analogInputModifier = 0.f;
		bool justTeleported = false;
		bool forceNextFloorCheck = false;

		bool pressedJump = false;
		bool wasJumping = false;
		float jumpKeyHoldTime = 0.f;
		float jumpForceTimeRemaining = 0.f;
		int jumpCurrentCount = 0;
		int jumpCurrentCountPreJump = 0;
		int numJumpApexAttempts = 0;

		uint32_t randomState = 20261102;	// 언리얼은 무작위라 대조할 수 없는 자리. 클라·서버가 같아지도록 값으로 들고 간다
	};

	// 갈래마다 몇 번 탔나. 대조에서 0 이 나오면 「안 틀림」이 아니라 「안 지나감」일 수 있다
	struct MoveCounters
	{
		uint64_t ticks = 0;
		uint64_t walkingIterations = 0;
		uint64_t fallingIterations = 0;
		uint64_t floorSweeps = 0;
		uint64_t floorSecondSweeps = 0;
		uint64_t floorLineTraces = 0;
		uint64_t floorLineWalkable = 0;
		uint64_t floorSuppliedSweep = 0;
		uint64_t floorHeightAdjusts = 0;
		uint64_t rampSecondMoves = 0;
		uint64_t stepUpTried = 0;
		uint64_t stepUpOk = 0;
		uint64_t stepUpRejected = 0;
		uint64_t slides = 0;
		uint64_t twoWalls = 0;
		uint64_t walkOffLedge = 0;
		uint64_t landed = 0;
		uint64_t landedOnEdgeFloor = 0;
		uint64_t fallSecondWall = 0;
		uint64_t fallStuckSideStep = 0;
		uint64_t fallVirtualDitch = 0;
		uint64_t apexSubsteps = 0;
		uint64_t jumps = 0;
		uint64_t launches = 0;
		uint64_t startPenetrating = 0;
		uint64_t resolveTeleport = 0;
		uint64_t resolveSweep = 0;
		uint64_t floorPopOut = 0;
		uint64_t perchTried = 0;
		uint64_t perchOk = 0;
		uint64_t perchUnwalkableOverride = 0;
		uint64_t perchFail = 0;
	};

	class CMoveSim
	{
	public:
		CMoveSim(const CMapCollision& map, const MoveParams& params) : _map(map), _p(params) {}

		// 언리얼 ACharacter::Jump / StopJumping
		void PressJump(MoveState& state) const;
		void ReleaseJump(MoveState& state) const;

		// 언리얼 ControlledCharacterMove 한 번. input 은 월드 축, 크기 1 이하. launch 가 있으면 LaunchCharacter 를 받은 것
		void Tick(MoveState& state, const Vec3& input, float deltaTime, const Vec3* launch = nullptr);

		// 밖에서 위치를 정해 준 뒤 바닥을 다시 잡는다. 이동은 하지 않는다
		void RefreshFloor(MoveState& state);

		const MoveCounters& GetCounters() const { return _count; }
		void ResetCounters() { _count = MoveCounters(); }

	private:
		bool IsWalking() const { return _s->mode == EMoveMode::Walking; }
		bool IsFalling() const { return _s->mode == EMoveMode::Falling; }

		// 점프 (언리얼 ACharacter 쪽)
		bool JumpIsAllowed() const;
		void CheckJumpInput();
		void ClearJumpInput(float deltaTime);
		void ResetJumpState();
		bool DoJump();

		// 이동 모드
		void SetMode(EMoveMode mode);
		void StartNewPhysics(float deltaTime, int iterations);
		void PhysWalking(float deltaTime, int iterations);
		void PhysFalling(float deltaTime, int iterations);
		void StartFalling(int iterations, float remainingTime, float timeTick, const Vec3& delta, const Vec3& subLocation);
		void ProcessLanded(float remainingTime, int iterations);
		float GetSimulationTimeStep(float remainingTime, int iterations) const;

		// 속도
		void CalcVelocity(float deltaTime, float friction, float brakingDeceleration);
		void ApplyVelocityBraking(float deltaTime, float friction, float brakingDeceleration);
		bool IsExceedingMaxSpeed(float maxSpeed) const;
		Vec3 NewFallVelocity(const Vec3& initial, const Vec3& gravity, float deltaTime) const;
		Vec3 GetFallingLateralAcceleration() const;
		Vec3 LimitAirControl(const Vec3& fallAcceleration, const MoveHit& hit) const;

		// 캡슐 옮기기
		bool MoveComponent(const Vec3& delta, bool sweep, MoveHit* outHit, bool neverIgnoreBlockingOverlaps);
		bool SafeMove(const Vec3& delta, MoveHit& outHit);
		Vec3 GetPenetrationAdjustment(const MoveHit& hit) const;
		bool ResolvePenetration(const Vec3& adjustment, const MoveHit& hit);
		float SlideAlongSurface(const Vec3& delta, float time, const Vec3& inNormal, MoveHit& hit);
		void TwoWallAdjust(Vec3& delta, const MoveHit& hit, const Vec3& oldHitNormal);
		Vec3 ComputeSlideVector(const Vec3& delta, float time, const Vec3& normal) const;
		Vec3 HandleSlopeBoosting(const Vec3& slideResult, const Vec3& delta, float time, const Vec3& normal) const;
		void SweepTest(const Vec3& start, const Vec3& end, float radius, float halfHeight, MoveHit& out);

		// 걷기
		void MoveAlongFloor(const Vec3& velocity, float deltaTime, bool& outComputedFloor, FloorResult& outFloor);
		bool StepUp(const Vec3& delta, const MoveHit& inHit, bool& outComputedFloor, FloorResult& outFloor);
		Vec3 ComputeGroundMovementDelta(const Vec3& delta, const MoveHit& rampHit, bool hitFromLineTrace) const;
		void AdjustFloorHeight();
		void MaintainHorizontalGroundVelocity();

		// 바닥
		bool IsWalkable(const MoveHit& hit) const;
		bool IsWithinEdgeTolerance(const Vec3& capsuleLocation, const Vec3& impactPoint, float radius) const;
		void ComputeFloorDist(const Vec3& location, float lineDistance, float sweepDistance, FloorResult& out, float sweepRadius, const MoveHit* downwardSweep);
		void FindFloor(const Vec3& location, FloorResult& out, const MoveHit* downwardSweep);
		float GetValidPerchRadius() const;
		bool ShouldComputePerchResult(const MoveHit& hit) const;
		bool ComputePerchResult(float testRadius, const MoveHit& hit, float maxFloorDist, FloorResult& out);
		bool IsValidLandingSpot(const Vec3& location, const MoveHit& hit);
		bool ShouldCheckForValidLandingSpot(const MoveHit& hit) const;

		const CMapCollision& _map;
		const MoveParams& _p;
		MoveState* _s = nullptr;
		MoveCounters _count;
	};
}
