#pragma once

#include "MapCollision.h"

#include <cstddef>
#include <cstdint>

// 언리얼 UCharacterMovementComponent 의 걷기·낙하를 엔진 없이 옮긴 것.
// - 왜: 내 캐릭터는 클라와 서버가 같은 코드로 움직여야 한다. 정답지는 Lyra 실행값이므로 계산 순서·자료형(float/double)까지 엔진을 따른다.
// - 옮기지 않은 것: 움직이는 받침, 루트 모션, 수영·비행, 경사 예외(WalkableSlopeOverride). Lyra 첫 판 범위 밖이다.
//   그중 루트 모션은 근접 밀기(GA_Melee 의 덧붙이는 상수 힘) 한 가지만 컨텐츠 E-2-1 에서 옮겼다 — RootForceState. 애니 루트 모션 · 다른 설정의 힘은 그대로 없다
// - 앉기(Crouch · UnCrouch · 이동 앞뒤 상태 갱신)는 컨텐츠 D 에서 옮겼다. 앉은 키보다 큰 앉은 키(드문 갈래)는 안 옮겼다 — Lyra 는 65 < 90
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
		float maxDepenetrationWithPawn = 100.f;		// 사람 캡슐과 겹쳤을 때 한 번에 빼내는 최대 거리 — 엔진 기본값(CharacterMovementComponent.cpp:705, 원본 기준 · Lyra 덮음 미확인)
		float terminalVelocity = 4000.f;
		float minAnalogWalkSpeed = 0.f;
		float capsuleRadius = 35.f;
		float capsuleHalfHeight = 90.f;
		float maxWalkSpeedCrouched = 300.f;
		float crouchedHalfHeight = 40.f;		// 서 있는 반높이보다 크면 안 된다(그 갈래는 안 옮겼다)
		int maxSimulationIterations = 8;
		int maxJumpApexAttemptsPerSimulation = 2;
		int jumpMaxCount = 1;
		bool useSeparateBrakingFriction = false;
		bool applyGravityWhileJumping = true;
		bool dontFallBelowJumpZVelocityDuringJump = true;
		bool canJump = true;
		bool canCrouch = false;					// NavAgentProps.bCanCrouch(Lyra 는 켠다)
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
		bool pawn = false;			// 다른 사람 캡슐에 맞음(컨텐츠 F) — 캐릭터 캡슐은 밟고 오를 수 없다(엔진 Character.cpp:81 CanCharacterStepUpOn = ECB_No)
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

	// 루트 모션 상수 힘 하나(근접 밀기) — 언리얼 FRootMotionSourceGroup · FRootMotionSource_ConstantForce 중 GA_Melee 가 쓰는 설정만 옮겼다:
	// 덧붙임(Additive) · 위아래 속도는 안 건드림(bEnableGravity → IgnoreZAccumulate) · 끝 틱 자르기 끔(상수 힘 기본 DisablePartialEndTick) ·
	// 끝나면 마지막 밀기 속도 유지(MaintainLastRootMotionVelocity) · 세기 곡선 없음 · 넣은 그 이동부터 먹음(Lyra 단독 실행은 시작 시각 조정이 안 걸린다).
	// 한 번에 하나 — 근접 능력이 켜진 0.55초 동안은 다시 못 켜고 밀기는 0.05초라 둘이 겹칠 일이 없다. 겹치면 새것으로 바꾸고 rootForceReplaced 를 센다
	struct RootForceState
	{
		Vec3 force;						// 초당 cm, 세상 축 — 걸음마다 속도에 더한다(Force × 준비 시간 ÷ 이동 틱 = Force)
		float duration = 0.f;
		float time = 0.f;				// CurrentTime — 준비할 때마다 이동 틱만큼 는다
		bool active = false;			// 묶음에 있다 — 넣은 때부터 끝나 지워질 때까지(대기 칸 PendingAdd 도 여기 — 다음 준비에 바로 들어간다)
		bool finished = false;			// 시간 ≥ 길이(Finished 표시) — 다음 이동 시작에 지운다
		bool hasAdditive = false;		// bHasAdditiveSources — 지난 준비 결과
		bool additiveApplied = false;	// bIsAdditiveVelocityApplied — 지금 속도에 밀기가 더해져 있다
		Vec3 lastPreAdditiveVelocity;	// LastPreAdditiveVelocity — 밀기를 더하기 전 속도. 다음 걸음은 이 값에서 시작한다
	};

	// 한 캐릭터가 틱 사이에 들고 있어야 하는 것 전부
	struct MoveState
	{
		Vec3 location;			// 캡슐 중심
		Vec3 velocity;
		Vec3 acceleration;
		EMoveMode mode = EMoveMode::Falling;
		FloorResult floor;
		float maxSpeed = 600.f;		// 이번 틱 MaxWalkSpeed(조준 배수 반영, Lyra 는 이동 금지 태그가 붙으면 0). 앉아 걸을 때는 이동 코드가 maxWalkSpeedCrouched 로 바꾼다(GetMaxSpeed)
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

		bool wantsToCrouch = false;	// 언리얼 bWantsToCrouch — 키가 바꾸고, 실제 앉기 · 일어서기는 이동 앞뒤 상태 갱신이 한다
		bool crouched = false;		// 언리얼 bIsCrouched — 켜져 있으면 캡슐 반높이가 crouchedHalfHeight

		uint32_t randomState = 20261102;	// 언리얼은 무작위라 대조할 수 없는 자리. 클라·서버가 같아지도록 값으로 들고 간다

		RootForceState rootForce;		// 근접 밀기(컨텐츠 E-2-1)
		Vec3 lastUpdateVelocity;		// LastUpdateVelocity — 지난 이동 끝 속도. 밀기 중에 밖에서 속도를 바꾸면 그 차이를 덧붙이기 전 속도로 옮긴다.
										// 서버 값으로 속도를 덮을 때는 이 값도 같이 덮는다(ClientAdjustPosition 과 같음 — 그러면 옮길 차이가 없다)
	};

	// 다른 사람 캡슐(컨텐츠 F) — 선 캡슐(축이 세로). 이동 쓸기 · 겹침 풀기가 맵과 함께 본다(Lyra 는 밀지 않고 막거나 미끄러뜨린다 — F-1 실측).
	// 바닥 찾기 · 앉기 · 일어서기 검사는 맵만 본다 — 남의 머리 위에 서기 · 사람 밑에서 일어서기는 안 옮겼다(기록 없음)
	struct PawnCapsule
	{
		Vec3 center;
		float radius = 35.f;
		float halfHeight = 90.f;
		float yaw = 0.f;		// 그 사람이 바라보는 쪽(도). 엔진은 맞는 쪽 캡슐 지역 좌표에서 단정밀도로 푼다(F-2).
								// 0 · 180 도는 부호만 바뀌어 결과가 같다(BumpCheck 에서 yaw 를 0 으로 바꿔도 PASS). 그 밖의 각도는 돌린 좌표의 반올림이 달라지는데 Lyra 기록으로 대조하지 않았다
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
		// 앉기(컨텐츠 D) — 갈래마다
		uint64_t crouchGround = 0;			// 땅에서 앉음: 줄인 캡슐을 바닥 쪽으로 쓸어 내린다
		uint64_t crouchAir = 0;				// 공중에서 앉음: 중심 그대로 줄인다
		uint64_t uncrouchGround = 0;		// 땅에서 일어섬: 바닥 자리를 지키며 키운다
		uint64_t uncrouchGroundLowered = 0;	// 그중 머리 위가 걸려 바닥 쪽으로 내려서 선 것
		uint64_t uncrouchAir = 0;			// 공중에서 일어섬: 중심 그대로 키운다
		uint64_t uncrouchAirShifted = 0;	// 그중 걸려서 짧은 캡슐로 아래를 쓸어 자리를 옮겨 선 것
		uint64_t uncrouchBlocked = 0;		// 일어서려다 막힘 — 다음 틱에 다시 시도한다
		// 근접 밀기(컨텐츠 E-2-1)
		uint64_t rootForceApplied = 0;		// 넣음
		uint64_t rootForceReplaced = 0;		// 그중 앞 밀기가 아직 있어 바꿔 넣음(기대 0 — 겹칠 수 없는 자리)
		uint64_t rootForcePrepared = 0;		// 밀기를 준비한 이동 수(Lyra 0.05초 = 3)
		uint64_t rootForceFinished = 0;		// 끝나 지움 — 마지막 밀기 속도를 남김
		uint64_t rootForceRestores = 0;		// 걸음 앞에서 덧붙이기 전 속도로 되돌림
		uint64_t rootForceAdds = 0;			// 걸음 속도에 밀기를 더함
		uint64_t rootForceExternal = 0;		// 밀기 중 밖에서 바뀐 속도(발사 · 덮기)를 덧붙이기 전 속도로 옮김(차이가 0 이 아닐 때만)
		uint64_t rootForceApexRefund = 0;	// 낙하 꼭대기 쪼개기가 밀기 시간을 돌려줌
		// 다른 사람 캡슐(컨텐츠 F)
		uint64_t pawnHits = 0;				// 이동 쓸기가 맵보다 먼저 사람 캡슐에 닿음
		uint64_t pawnStartPenetrating = 0;	// 그중 시작부터 겹쳐 있음
		uint64_t pawnOddSimplex = 0;		// 사람 쓸기 반복에서 삼각형 단체가 퇴화하지 않음 — 세로 캡슐 둘이면 안 나온다(나오면 정확한 식으로 갈음)
		uint64_t pawnTouchStart = 0;		// 정확한 식으론 안 겹쳤는데 단정밀도 반복이 처음부터 여유 안(λ = 0)으로 끝남 — 엔진처럼 겹침 맞음(깊이 0 근처)으로 돌려준다
	};

	class CMoveSim
	{
	public:
		CMoveSim(const CMapCollision& map, const MoveParams& params) : _map(map), _p(params) {}

		// 언리얼 ACharacter::Jump / StopJumping
		void PressJump(MoveState& state) const;
		void ReleaseJump(MoveState& state) const;

		// ALyraCharacter::ToggleCrouch(앉기 키) — 앉았거나 앉으려는 중이면 일어서기를, 아니면 땅 위일 때만 앉기를 바란다
		void ToggleCrouch(MoveState& state) const;
		// ACharacter::UnCrouch — 일어서기를 바란다. Lyra 점프 능력이 점프 전에 부른다(LyraGameplayAbility_Jump.cpp:55)
		void RequestUnCrouch(MoveState& state) const;
		// 지금 캡슐 반높이
		float HalfHeightOf(const MoveState& state) const { return state.crouched ? CrouchedHalfHeight() : _p.capsuleHalfHeight; }
		// 앉은 캡슐의 메시 올림 = 선 반높이 − 앉은 반높이(Lyra 25). 앉으면 캡슐 중심이 이만큼 내려가고 메시는 그만큼 올라가 세상 자리가 그대로다(D62 7) —
		// 자세 계산(BodiesFromLocal · ComputeBoundRadius)에 넘기는 값
		float CrouchMeshLiftZ() const { return _p.capsuleHalfHeight - CrouchedHalfHeight(); }

		// 언리얼 ControlledCharacterMove 한 번. input 은 월드 축, 크기 1 이하. launch 가 있으면 LaunchCharacter 를 받은 것
		void Tick(MoveState& state, const Vec3& input, float deltaTime, const Vec3* launch = nullptr);

		// UCharacterMovementComponent::ApplyRootMotionSource(상수 힘) — 다음 Tick 의 이동부터 먹는다.
		// Lyra 근접은 능력이 켜지는 입력 처리에서 넣어 그 프레임 이동이 바로 민다(E-2-1 실측 — 넣은 프레임에 648.648 × 1/60 만큼 움직임)
		void ApplyRootForce(MoveState& state, const Vec3& force, float duration);

		// 밖에서 위치를 정해 준 뒤 바닥을 다시 잡는다. 이동은 하지 않는다
		void RefreshFloor(MoveState& state);

		// 다른 사람 캡슐(컨텐츠 F) — 다음 Tick 들의 이동 쓸기 · 겹침 풀기가 본다. 배열은 부른 쪽이 들고 있어야 한다. 비우려면 (nullptr, 0)
		void SetPawns(const PawnCapsule* pawns, size_t count) { _pawns = pawns; _pawnCount = pawns ? count : 0; }

		const MoveCounters& GetCounters() const { return _count; }
		void ResetCounters() { _count = MoveCounters(); }

	private:
		bool IsWalking() const { return _s->mode == EMoveMode::Walking; }
		bool IsFalling() const { return _s->mode == EMoveMode::Falling; }

		// 앉기 (언리얼 UCharacterMovementComponent)
		float CrouchedHalfHeight() const;	// Crouch 의 ClampedCrouchedHalfHeight = Max3(0, 반지름, 앉은 반높이)
		float HalfHeight() const { return HalfHeightOf(*_s); }
		float MaxSpeedNow() const;		// GetMaxSpeed — 걷기면 앉음에 따라, 낙하면 MaxWalkSpeed
		bool CanCrouchInCurrentState() const { return _p.canCrouch && (IsFalling() || IsWalking()); }
		void Crouch();
		void UnCrouch();
		void UpdateCharacterStateBeforeMovement();
		void UpdateCharacterStateAfterMovement();

		// 루트 모션 힘 (언리얼 CharacterMovementComponent · RootMotionSource) — RootForceState 의 설정 하나만
		void CleanUpRootForce();						// FRootMotionSourceGroup::CleanUpInvalidRootMotion
		void PrepareRootForce(float deltaTime);			// FRootMotionSourceGroup::PrepareRootMotion
		void RestorePreAdditiveRootMotionVelocity();
		void ApplyRootMotionToVelocity();				// 덧붙임 갈래만 — 덮어쓰기 · 애니 루트 모션은 없다

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
		bool SweepPawns(const Vec3& start, const Vec3& end, float radius, float halfHeight, CapsuleHit& out);	// 가장 먼저 닿는 사람(계수를 센다)
		bool OverlapPawns(const Vec3& center, float radius, float halfHeight) const;

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
		const PawnCapsule* _pawns = nullptr;
		size_t _pawnCount = 0;
	};
}
