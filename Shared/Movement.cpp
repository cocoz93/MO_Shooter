#include "Movement.h"

#include <algorithm>
#include <cmath>

namespace mo
{
	MoveParams MakeLyraMoveParams()
	{
		MoveParams p;
		p.maxWalkSpeed = 600.f;
		p.maxAcceleration = 1200.f;
		p.groundFriction = 8.f;
		p.brakingDecelerationWalking = 1400.f;
		p.brakingDecelerationFalling = 0.f;
		p.brakingFriction = 3.f;
		p.brakingFrictionFactor = 1.f;
		p.useSeparateBrakingFriction = true;
		p.brakingSubStepTime = 0.030303031206130981f;
		p.gravityZ = -980.f;
		p.jumpZVelocity = 500.f;
		p.jumpMaxHoldTime = 0.f;
		p.jumpMaxCount = 1;
		p.airControl = 0.4f;
		p.airControlBoostMultiplier = 4.f;
		p.airControlBoostVelocityThreshold = 50.f;
		p.fallingLateralFriction = 0.f;
		p.maxStepHeight = 50.f;
		p.walkableFloorZ = 0.71000003814697266f;
		p.perchRadiusThreshold = 6.f;
		p.perchAdditionalHeight = 40.f;
		p.maxSimulationTimeStep = 0.05f;
		p.maxSimulationIterations = 8;
		p.maxJumpApexAttemptsPerSimulation = 2;
		p.applyGravityWhileJumping = true;
		p.dontFallBelowJumpZVelocityDuringJump = true;
		p.maxDepenetrationWithGeometry = 500.f;
		p.terminalVelocity = 4000.f;
		p.minAnalogWalkSpeed = 200.f;
		p.capsuleRadius = 35.f;
		p.capsuleHalfHeight = 90.f;
		return p;
	}

	namespace
	{
		constexpr float kMinTickTime = 1e-6f;
		constexpr float kMinFloorDist = 1.9f;
		constexpr float kMaxFloorDist = 2.4f;
		constexpr float kBrakeToStopVelocity = 10.f;
		constexpr float kSweepEdgeRejectDistance = 0.15f;
		constexpr float kMaxStepSideZ = 0.08f;
		constexpr float kVerticalSlopeNormalZ = 0.001f;
		constexpr float kKindaSmall = 1e-4f;
		constexpr float kSmall = 1e-8f;
		constexpr float kDelta = 1e-5f;
		constexpr float kPullback = 0.125f;				// p.PenetrationPullbackDistance
		constexpr float kOverlapInflation = 0.1f;		// p.PenetrationOverlapCheckInflation

		inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline Vec3 Cross(const Vec3& a, const Vec3& b)
		{
			return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		}
		inline double SizeSq(const Vec3& a) { return Dot(a, a); }
		inline double Size(const Vec3& a) { return std::sqrt(Dot(a, a)); }
		inline double SizeSq2D(const Vec3& a) { return a.x * a.x + a.y * a.y; }
		inline Vec3 Flat(const Vec3& a) { return { a.x, a.y, 0.0 }; }
		inline bool IsZero(const Vec3& a) { return a.x == 0 && a.y == 0 && a.z == 0; }
		inline bool Same(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
		inline bool IsNearlyZero(const Vec3& a, double tolerance = kKindaSmall)
		{
			return std::fabs(a.x) <= tolerance && std::fabs(a.y) <= tolerance && std::fabs(a.z) <= tolerance;
		}
		inline bool NearlyEqual(const Vec3& a, const Vec3& b, double tolerance = kKindaSmall)
		{
			return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
		}
		inline Vec3 SafeNormal(const Vec3& a)
		{
			const double sum = SizeSq(a);
			if (sum == 1.0) return a;
			if (sum < (double)kSmall) return Vec3();
			return a * (1.0 / std::sqrt(sum));
		}
		inline Vec3 ClampedToMaxSize(const Vec3& a, double maxSize)
		{
			if (maxSize < (double)kKindaSmall) return Vec3();
			const double sq = SizeSq(a);
			if (sq > maxSize * maxSize) return a * (maxSize / std::sqrt(sq));
			return a;
		}
		inline Vec3 PlaneProject(const Vec3& v, const Vec3& normal) { return v - normal * Dot(v, normal); }
		inline float Square(float v) { return v * v; }
		inline float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

		void FillFromCapsule(MoveHit& out, const CapsuleHit& hit, const Vec3& start, const Vec3& end)
		{
			out.blocking = true;
			out.startPenetrating = hit.startPenetrating;
			out.time = (float)hit.time;
			out.penetrationDepth = (float)hit.penetrationDepth;
			out.location = start + (end - start) * hit.time;
			out.impactPoint = hit.impactPoint;
			out.normal = hit.normal;
			out.impactNormal = hit.impactNormal;
			out.traceStart = start;
			out.traceEnd = end;
		}

		void InitHit(MoveHit& out, const Vec3& start, const Vec3& end)
		{
			out = MoveHit();
			out.traceStart = start;
			out.traceEnd = end;
		}

		// FFindFloorResult::SetFromSweep
		void SetFromSweep(FloorResult& floor, const MoveHit& hit, float sweepFloorDist, bool walkable)
		{
			floor.blockingHit = hit.IsValidBlockingHit();
			floor.walkableFloor = walkable;
			floor.lineTrace = false;
			floor.floorDist = sweepFloorDist;
			floor.lineDist = 0.f;
			floor.hit = hit;
		}

		// FFindFloorResult::SetFromLineTrace — 시간·맞은 점·위치·궤적은 캡슐 판정 것을 남긴다
		void SetFromLineTrace(FloorResult& floor, const MoveHit& hit, float sweepFloorDist, float lineDist, bool walkable)
		{
			if (floor.hit.blocking && hit.blocking)
			{
				const MoveHit old = floor.hit;
				floor.hit = hit;
				floor.hit.time = old.time;
				floor.hit.impactPoint = old.impactPoint;
				floor.hit.location = old.location;
				floor.hit.traceStart = old.traceStart;
				floor.hit.traceEnd = old.traceEnd;
				floor.lineTrace = true;
				floor.floorDist = sweepFloorDist;
				floor.lineDist = lineDist;
				floor.walkableFloor = walkable;
			}
		}
	}

	// ---------------------------------------------------------------- 점프

	void CMoveSim::PressJump(MoveState& state) const
	{
		state.pressedJump = true;
		state.jumpKeyHoldTime = 0.f;
	}

	void CMoveSim::ReleaseJump(MoveState& state) const
	{
		state.pressedJump = false;
		state.wasJumping = false;
		state.jumpKeyHoldTime = 0.f;
		state.jumpForceTimeRemaining = 0.f;
		if (state.mode != EMoveMode::Falling)
		{
			state.jumpCurrentCount = 0;
			state.jumpCurrentCountPreJump = 0;
		}
	}

	void CMoveSim::ResetJumpState()
	{
		ReleaseJump(*_s);
	}

	bool CMoveSim::JumpIsAllowed() const
	{
		const MoveState& s = *_s;
		if (!_p.canJump) return false;		// Lyra CanAttemptJump 은 웅크림을 보지 않는다. 걷기·낙하만 있으니 모드 검사는 항상 참

		if (!s.wasJumping || _p.jumpMaxHoldTime <= 0.f)
		{
			if (s.jumpCurrentCount == 0 && IsFalling())
			{
				return s.jumpCurrentCount + 1 < _p.jumpMaxCount;
			}
			return s.jumpCurrentCount < _p.jumpMaxCount;
		}
		const bool held = s.pressedJump && s.jumpKeyHoldTime < _p.jumpMaxHoldTime;
		return held && (s.jumpCurrentCount < _p.jumpMaxCount || (s.wasJumping && s.jumpCurrentCount == _p.jumpMaxCount));
	}

	bool CMoveSim::DoJump()
	{
		MoveState& s = *_s;
		if (!JumpIsAllowed()) return false;

		const bool firstJump = s.jumpCurrentCountPreJump == 0;
		if (firstJump || _p.dontFallBelowJumpZVelocityDuringJump)
		{
			s.velocity.z = std::max<double>(s.velocity.z, _p.jumpZVelocity);
		}
		SetMode(EMoveMode::Falling);
		return true;
	}

	void CMoveSim::CheckJumpInput()
	{
		MoveState& s = *_s;
		s.jumpCurrentCountPreJump = s.jumpCurrentCount;
		if (!s.pressedJump) return;

		if (s.jumpCurrentCount == 0 && IsFalling())
		{
			++s.jumpCurrentCount;
		}
		const bool didJump = DoJump();
		if (didJump && !s.wasJumping)
		{
			++s.jumpCurrentCount;
			s.jumpForceTimeRemaining = _p.jumpMaxHoldTime;
			++_count.jumps;
		}
		s.wasJumping = didJump;
	}

	void CMoveSim::ClearJumpInput(float deltaTime)
	{
		MoveState& s = *_s;
		if (s.pressedJump)
		{
			s.jumpKeyHoldTime += deltaTime;
			if (s.jumpKeyHoldTime >= _p.jumpMaxHoldTime)
			{
				s.pressedJump = false;
			}
		}
		else
		{
			s.jumpForceTimeRemaining = 0.f;
			s.wasJumping = false;
		}
	}

	// ---------------------------------------------------------------- 틱

	void CMoveSim::Tick(MoveState& state, const Vec3& input, float deltaTime, const Vec3* launch)
	{
		_s = &state;
		MoveState& s = state;
		++_count.ticks;

		// ControlledCharacterMove — 입력을 가속도로 바꾸기 전에 점프부터 본다(점프한 틱의 가속도가 낙하 규칙을 따르게)
		CheckJumpInput();

		s.acceleration = ClampedToMaxSize(Flat(input), 1.0) * (double)_p.maxAcceleration;
		if (SizeSq(s.acceleration) > 0.0 && _p.maxAcceleration > kSmall)
		{
			const double ratio = Size(s.acceleration) / _p.maxAcceleration;
			s.analogInputModifier = (float)(ratio < 0 ? 0 : (ratio > 1 ? 1 : ratio));
		}
		else
		{
			s.analogInputModifier = 0.f;
		}

		// PerformMovement
		if (launch && !IsZero(*launch))
		{
			s.velocity = *launch;
			SetMode(EMoveMode::Falling);
			s.forceNextFloorCheck = true;
			++_count.launches;
		}

		ClearJumpInput(deltaTime);
		s.numJumpApexAttempts = 0;
		StartNewPhysics(deltaTime, 0);
	}

	void CMoveSim::RefreshFloor(MoveState& state)
	{
		_s = &state;
		if (state.mode == EMoveMode::Walking)
		{
			FindFloor(state.location, state.floor, nullptr);
		}
		else
		{
			state.floor = FloorResult();
		}
	}

	void CMoveSim::SetMode(EMoveMode mode)
	{
		MoveState& s = *_s;
		if (s.mode == mode) return;
		s.mode = mode;

		// UCharacterMovementComponent::OnMovementModeChanged
		if (mode == EMoveMode::Walking)
		{
			s.velocity.z = 0.0;
			FindFloor(s.location, s.floor, nullptr);
			AdjustFloorHeight();
		}
		else
		{
			s.floor = FloorResult();
		}

		// ACharacter::OnMovementModeChanged
		if (!s.pressedJump || !IsFalling())
		{
			ResetJumpState();
		}
	}

	void CMoveSim::StartNewPhysics(float deltaTime, int iterations)
	{
		if (deltaTime < kMinTickTime || iterations >= _p.maxSimulationIterations) return;
		switch (_s->mode)
		{
		case EMoveMode::Walking: PhysWalking(deltaTime, iterations); break;
		case EMoveMode::Falling: PhysFalling(deltaTime, iterations); break;
		}
	}

	float CMoveSim::GetSimulationTimeStep(float remainingTime, int iterations) const
	{
		if (remainingTime > _p.maxSimulationTimeStep && iterations < _p.maxSimulationIterations)
		{
			remainingTime = std::min(_p.maxSimulationTimeStep, remainingTime * 0.5f);
		}
		return std::max(kMinTickTime, remainingTime);
	}

	// ---------------------------------------------------------------- 속도

	bool CMoveSim::IsExceedingMaxSpeed(float maxSpeed) const
	{
		maxSpeed = std::max(0.f, maxSpeed);
		const float maxSpeedSquared = Square(maxSpeed);
		return SizeSq(_s->velocity) > (double)(maxSpeedSquared * 1.01f);
	}

	void CMoveSim::CalcVelocity(float deltaTime, float friction, float brakingDeceleration)
	{
		MoveState& s = *_s;
		if (deltaTime < kMinTickTime) return;

		friction = std::max(0.f, friction);
		const float maxInputSpeed = std::max(s.maxSpeed * s.analogInputModifier, _p.minAnalogWalkSpeed);
		const float maxSpeed = std::max(0.f, maxInputSpeed);

		const bool zeroAcceleration = IsZero(s.acceleration);
		const bool velocityOverMax = IsExceedingMaxSpeed(maxSpeed);

		if (zeroAcceleration || velocityOverMax)
		{
			const Vec3 oldVelocity = s.velocity;
			const float brakingFriction = _p.useSeparateBrakingFriction ? _p.brakingFriction : friction;
			ApplyVelocityBraking(deltaTime, brakingFriction, brakingDeceleration);

			if (velocityOverMax && SizeSq(s.velocity) < (double)Square(maxSpeed) && Dot(s.acceleration, oldVelocity) > 0.0)
			{
				s.velocity = SafeNormal(oldVelocity) * (double)maxSpeed;
			}
		}
		else
		{
			const Vec3 accelDir = SafeNormal(s.acceleration);
			const float velSize = (float)Size(s.velocity);
			s.velocity = s.velocity - (s.velocity - accelDir * (double)velSize) * (double)std::min(deltaTime * friction, 1.f);
		}

		if (!zeroAcceleration)
		{
			const float newMaxInputSpeed = IsExceedingMaxSpeed(maxInputSpeed) ? (float)Size(s.velocity) : maxInputSpeed;
			s.velocity = s.velocity + s.acceleration * (double)deltaTime;
			s.velocity = ClampedToMaxSize(s.velocity, newMaxInputSpeed);
		}
	}

	void CMoveSim::ApplyVelocityBraking(float deltaTime, float friction, float brakingDeceleration)
	{
		MoveState& s = *_s;
		if (IsZero(s.velocity) || deltaTime < kMinTickTime) return;

		const float frictionFactor = std::max(0.f, _p.brakingFrictionFactor);
		friction = std::max(0.f, friction * frictionFactor);
		brakingDeceleration = std::max(0.f, brakingDeceleration);
		const bool zeroFriction = friction == 0.f;
		const bool zeroBraking = brakingDeceleration == 0.f;
		if (zeroFriction && zeroBraking) return;

		const Vec3 oldVel = s.velocity;
		float remainingTime = deltaTime;
		const float maxTimeStep = ClampF(_p.brakingSubStepTime, 1.0f / 75.0f, 1.0f / 20.0f);
		const Vec3 revAccel = zeroBraking ? Vec3() : SafeNormal(s.velocity) * -(double)brakingDeceleration;
		while (remainingTime >= kMinTickTime)
		{
			const float dt = (remainingTime > maxTimeStep && !zeroFriction) ? std::min(maxTimeStep, remainingTime * 0.5f) : remainingTime;
			remainingTime -= dt;
			s.velocity = s.velocity + (s.velocity * -(double)friction + revAccel) * (double)dt;
			if (Dot(s.velocity, oldVel) <= 0.0)
			{
				s.velocity = Vec3();
				return;
			}
		}

		const float sizeSq = (float)SizeSq(s.velocity);
		if (sizeSq <= kKindaSmall || (!zeroBraking && sizeSq <= Square(kBrakeToStopVelocity)))
		{
			s.velocity = Vec3();
		}
	}

	Vec3 CMoveSim::NewFallVelocity(const Vec3& initial, const Vec3& gravity, float deltaTime) const
	{
		Vec3 result = initial;
		if (deltaTime > 0.f)
		{
			result = result + gravity * (double)deltaTime;
			const float terminalLimit = std::fabs(_p.terminalVelocity);
			if (SizeSq(result) > (double)Square(terminalLimit))
			{
				const Vec3 gravityDir = SafeNormal(gravity);
				if (Dot(result, gravityDir) > (double)terminalLimit)
				{
					result = PlaneProject(result, gravityDir) + gravityDir * (double)terminalLimit;
				}
			}
		}
		return result;
	}

	Vec3 CMoveSim::GetFallingLateralAcceleration() const
	{
		const MoveState& s = *_s;
		Vec3 fallAcceleration = Flat(s.acceleration);
		if (SizeSq(fallAcceleration) > 0.0)
		{
			float tickAirControl = _p.airControl;
			if (tickAirControl != 0.f && _p.airControlBoostMultiplier > 0.f
				&& SizeSq2D(s.velocity) < (double)Square(_p.airControlBoostVelocityThreshold))
			{
				tickAirControl = std::min(1.f, _p.airControlBoostMultiplier * tickAirControl);
			}
			fallAcceleration = fallAcceleration * (double)tickAirControl;
			fallAcceleration = ClampedToMaxSize(fallAcceleration, _p.maxAcceleration);
		}
		return fallAcceleration;
	}

	// 언리얼에서 호출 자리마다 bCheckForValidLandingSpot = false 라 그 갈래는 없다
	Vec3 CMoveSim::LimitAirControl(const Vec3& fallAcceleration, const MoveHit& hit) const
	{
		Vec3 result = fallAcceleration;
		if (hit.IsValidBlockingHit() && hit.normal.z > kVerticalSlopeNormalZ)
		{
			if (Dot(fallAcceleration, hit.normal) < 0.0)
			{
				const Vec3 normal2D = SafeNormal(Flat(hit.normal));
				result = PlaneProject(fallAcceleration, normal2D);
			}
		}
		else if (hit.startPenetrating)
		{
			return Dot(result, hit.normal) > 0.0 ? result : Vec3();
		}
		return result;
	}

	// ---------------------------------------------------------------- 캡슐 옮기기

	// UPrimitiveComponent::MoveComponentImpl
	bool CMoveSim::MoveComponent(const Vec3& delta, bool sweep, MoveHit* outHit, bool neverIgnoreBlockingOverlaps)
	{
		MoveState& s = *_s;
		const Vec3 traceStart = s.location;
		const Vec3 traceEnd = traceStart + delta;
		const float deltaSizeSq = (float)SizeSq(traceEnd - traceStart);
		const float minMovementDistSq = sweep ? Square(4.f * kKindaSmall) : 0.f;

		if (deltaSizeSq <= minMovementDistSq)
		{
			if (outHit) InitHit(*outHit, traceStart, traceEnd);
			return true;
		}

		if (!sweep)
		{
			s.location = traceEnd;
			if (outHit) InitHit(*outHit, traceStart, traceEnd);
			return !Same(traceStart, s.location);
		}

		CapsuleHit sweepHit;
		const bool hit = _map.SweepCapsule(traceStart, traceEnd, _p.capsuleRadius, _p.capsuleHalfHeight, MAP_BLOCK_MOVE, sweepHit);

		bool blocked = false;
		MoveHit blocking;
		if (hit)
		{
			FillFromCapsule(blocking, sweepHit, traceStart, traceEnd);
			if (blocking.startPenetrating) ++_count.startPenetrating;

			// PullBackHit — 면에 딱 붙지 않게 시간을 조금 물린다. 닿은 위치(location)는 그대로 둔다
			const float dist = std::sqrt(deltaSizeSq);
			const float desiredTimeBack = ClampF(0.1f, 0.1f / dist, 1.f / dist) + 0.001f;
			blocking.time = ClampF(blocking.time - desiredTimeBack, 0.f, 1.f);

			blocked = true;
			// ShouldIgnoreHitResult — 빠져나가는 방향이면 겹침을 무시한다
			if (blocking.startPenetrating && !neverIgnoreBlockingOverlaps)
			{
				const float moveDot = (float)Dot(blocking.impactNormal, SafeNormal(delta));
				if (moveDot > 0.f) blocked = false;
			}
		}

		if (!blocked)
		{
			s.location = traceEnd;
			if (outHit) InitHit(*outHit, traceStart, traceEnd);
		}
		else
		{
			Vec3 newLocation = traceStart + (traceEnd - traceStart) * (double)blocking.time;
			if ((float)SizeSq(newLocation - traceStart) <= minMovementDistSq)
			{
				newLocation = traceStart;
				blocking.time = 0.f;
			}
			s.location = newLocation;
			if (outHit) *outHit = blocking;
		}
		return !Same(traceStart, s.location);
	}

	// UMovementComponent::SafeMoveUpdatedComponent — 첫 시도는 겹침을 무시하지 않고, 풀어낸 뒤 재시도는 무시할 수 있다
	bool CMoveSim::SafeMove(const Vec3& delta, MoveHit& outHit)
	{
		bool moved = MoveComponent(delta, true, &outHit, true);
		if (outHit.startPenetrating)
		{
			const Vec3 adjustment = GetPenetrationAdjustment(outHit);
			if (ResolvePenetration(adjustment, outHit))
			{
				moved = MoveComponent(delta, true, &outHit, false);
			}
		}
		return moved;
	}

	Vec3 CMoveSim::GetPenetrationAdjustment(const MoveHit& hit) const
	{
		if (!hit.startPenetrating) return Vec3();
		const float depth = hit.penetrationDepth > 0.f ? hit.penetrationDepth : 0.125f;
		const Vec3 result = hit.normal * (double)(depth + kPullback);
		return ClampedToMaxSize(result, _p.maxDepenetrationWithGeometry);
	}

	bool CMoveSim::ResolvePenetration(const Vec3& adjustment, const MoveHit& hit)
	{
		MoveState& s = *_s;
		bool result = false;
		if (!IsZero(adjustment))
		{
			const bool encroached = _map.OverlapCapsule(hit.traceStart + adjustment, _p.capsuleRadius + kOverlapInflation,
				_p.capsuleHalfHeight + kOverlapInflation, MAP_BLOCK_MOVE);
			if (!encroached)
			{
				MoveComponent(adjustment, false, nullptr, false);
				++_count.resolveTeleport;
				result = true;
			}
			else
			{
				++_count.resolveSweep;
				MoveHit sweepOut;
				bool moved = MoveComponent(adjustment, true, &sweepOut, false);
				if (!moved && sweepOut.startPenetrating)
				{
					const Vec3 secondMtd = GetPenetrationAdjustment(sweepOut);
					const Vec3 combined = adjustment + secondMtd;
					if (!Same(secondMtd, adjustment) && !IsZero(combined))
					{
						moved = MoveComponent(combined, true, nullptr, false);
					}
				}
				if (!moved)
				{
					const Vec3 moveDelta = hit.traceEnd - hit.traceStart;
					if (!IsZero(moveDelta))
					{
						moved = MoveComponent(adjustment + moveDelta, true, nullptr, false);
						if (!moved && Dot(moveDelta, adjustment) > 0.0)
						{
							moved = MoveComponent(moveDelta, true, nullptr, false);
						}
					}
				}
				result = moved;
			}
		}
		// UCharacterMovementComponent::ResolvePenetrationImpl — 지금까지 쌓인 값을 돌려준다
		s.justTeleported = s.justTeleported || result;
		return s.justTeleported;
	}

	Vec3 CMoveSim::ComputeSlideVector(const Vec3& delta, float time, const Vec3& normal) const
	{
		Vec3 result = PlaneProject(delta, normal) * (double)time;
		if (IsFalling())
		{
			result = HandleSlopeBoosting(result, delta, time, normal);
		}
		return result;
	}

	Vec3 CMoveSim::HandleSlopeBoosting(const Vec3& slideResult, const Vec3& delta, float time, const Vec3& normal) const
	{
		Vec3 result = slideResult;
		const float resultZ = (float)result.z;
		if (resultZ > 0.f)
		{
			const float zLimit = (float)delta.z * time;
			if (resultZ - zLimit > kKindaSmall)
			{
				if (zLimit > 0.f)
				{
					const float upPercent = zLimit / resultZ;
					result = result * (double)upPercent;
				}
				else
				{
					result = Vec3();
				}
				const Vec3 remainderXY = Flat(slideResult - result);
				const Vec3 normalXY = SafeNormal(Flat(normal));
				const Vec3 adjust = PlaneProject(remainderXY, normalXY);
				result = result + adjust;
			}
		}
		return result;
	}

	float CMoveSim::SlideAlongSurface(const Vec3& delta, float time, const Vec3& inNormal, MoveHit& hit)
	{
		MoveState& s = *_s;
		if (!hit.blocking) return 0.f;
		++_count.slides;

		// UCharacterMovementComponent::SlideAlongSurface
		Vec3 normal = inNormal;
		const double normalZ = normal.z;
		if (IsWalking())
		{
			if (normalZ > 0.0)
			{
				if (!IsWalkable(hit))
				{
					normal = SafeNormal(Flat(normal));
				}
			}
			else if (normalZ < -(double)kKindaSmall)
			{
				if (s.floor.floorDist < kMinFloorDist && s.floor.blockingHit)
				{
					const Vec3 floorNormal = s.floor.hit.normal;
					const bool floorOpposed = Dot(delta, floorNormal) < 0.0 && floorNormal.z < (double)(1.f - kDelta);
					if (floorOpposed)
					{
						normal = floorNormal;
					}
					normal = SafeNormal(Flat(normal));
				}
			}
		}

		// UMovementComponent::SlideAlongSurface
		float percentTimeApplied = 0.f;
		const Vec3 oldHitNormal = normal;
		Vec3 slideDelta = ComputeSlideVector(delta, time, normal);
		if (Dot(slideDelta, delta) > 0.0)
		{
			SafeMove(slideDelta, hit);
			const float firstHitPercent = hit.time;
			percentTimeApplied = firstHitPercent;
			if (hit.IsValidBlockingHit())
			{
				TwoWallAdjust(slideDelta, hit, oldHitNormal);
				if (!IsNearlyZero(slideDelta, 1e-3f) && Dot(slideDelta, delta) > 0.0)
				{
					SafeMove(slideDelta, hit);
					const float secondHitPercent = hit.time * (1.f - firstHitPercent);
					percentTimeApplied += secondHitPercent;
				}
			}
			return ClampF(percentTimeApplied, 0.f, 1.f);
		}
		return 0.f;
	}

	void CMoveSim::TwoWallAdjust(Vec3& outDelta, const MoveHit& hit, const Vec3& oldHitNormal)
	{
		const MoveState& s = *_s;
		++_count.twoWalls;
		const Vec3 inDelta = outDelta;

		// UMovementComponent::TwoWallAdjust
		Vec3 delta = outDelta;
		const Vec3 hitNormal = hit.normal;
		if (Dot(oldHitNormal, hitNormal) <= 0.0)
		{
			const Vec3 desiredDir = delta;
			const Vec3 newDir = SafeNormal(Cross(hitNormal, oldHitNormal));
			delta = newDir * (Dot(delta, newDir) * (double)(1.f - hit.time));
			if (Dot(desiredDir, delta) < 0.0)
			{
				delta = delta * -1.0;
			}
		}
		else
		{
			const Vec3 desiredDir = delta;
			delta = ComputeSlideVector(delta, 1.f - hit.time, hitNormal);
			if (Dot(delta, desiredDir) <= 0.0)
			{
				delta = Vec3();
			}
			else if (std::fabs(Dot(hitNormal, oldHitNormal) - 1.0) < (double)kKindaSmall)
			{
				delta = delta + hitNormal * (double)0.01f;
			}
		}

		// UCharacterMovementComponent::TwoWallAdjust
		if (IsWalking())
		{
			const double deltaZ = delta.z;
			if (deltaZ > 0.0)
			{
				const double hitNormalZ = hit.normal.z;
				if ((hitNormalZ >= (double)_p.walkableFloorZ || IsWalkable(hit)) && hitNormalZ > (double)kKindaSmall)
				{
					const float time = 1.f - hit.time;
					const Vec3 scaledDelta = SafeNormal(delta) * Size(inDelta);
					const double newDeltaZ = scaledDelta.z / hitNormalZ;
					delta = (Flat(inDelta) + Vec3(0, 0, newDeltaZ)) * (double)time;
					if (delta.z > (double)_p.maxStepHeight)
					{
						const float rescale = (float)(_p.maxStepHeight / delta.z);
						delta = delta * (double)rescale;
					}
				}
				else
				{
					delta = Flat(delta);
				}
			}
			else if (deltaZ < 0.0)
			{
				if (s.floor.floorDist < kMinFloorDist && s.floor.blockingHit)
				{
					delta = Flat(delta);
				}
			}
		}
		outDelta = delta;
	}

	void CMoveSim::SweepTest(const Vec3& start, const Vec3& end, float radius, float halfHeight, MoveHit& out)
	{
		InitHit(out, start, end);
		CapsuleHit hit;
		if (_map.SweepCapsule(start, end, radius, halfHeight, MAP_BLOCK_MOVE, hit))
		{
			FillFromCapsule(out, hit, start, end);
		}
	}

	// ---------------------------------------------------------------- 걷기

	Vec3 CMoveSim::ComputeGroundMovementDelta(const Vec3& delta, const MoveHit& rampHit, bool hitFromLineTrace) const
	{
		const Vec3 floorNormal = rampHit.impactNormal;
		const double floorNormalZ = floorNormal.z;
		const double contactNormalZ = rampHit.normal.z;
		if (floorNormalZ < (double)(1.f - kKindaSmall) && floorNormalZ > (double)kKindaSmall && contactNormalZ > (double)kKindaSmall
			&& !hitFromLineTrace && IsWalkable(rampHit))
		{
			const float floorDotDelta = (float)Dot(floorNormal, delta);
			Vec3 rampMovement = delta;
			rampMovement.z = -floorDotDelta / floorNormalZ;
			return rampMovement;		// bMaintainHorizontalGroundVelocity = true 전제
		}
		return delta;
	}

	void CMoveSim::MoveAlongFloor(const Vec3& velocity, float deltaTime, bool& outComputedFloor, FloorResult& outFloor)
	{
		MoveState& s = *_s;
		if (!s.floor.IsWalkableFloor()) return;

		const Vec3 delta = Flat(velocity) * (double)deltaTime;
		MoveHit hit;
		Vec3 rampVector = ComputeGroundMovementDelta(delta, s.floor.hit, s.floor.lineTrace);
		SafeMove(rampVector, hit);

		if (hit.startPenetrating)
		{
			SlideAlongSurface(delta, 1.f, hit.normal, hit);
			if (hit.startPenetrating)
			{
				s.justTeleported = true;	// OnCharacterStuckInGeometry
			}
		}
		else if (hit.IsValidBlockingHit())
		{
			float percentTimeApplied = hit.time;
			if (hit.time > 0.f && hit.normal.z > (double)kKindaSmall && IsWalkable(hit))
			{
				const float initialPercentRemaining = 1.f - percentTimeApplied;
				rampVector = ComputeGroundMovementDelta(delta * (double)initialPercentRemaining, hit, false);
				SafeMove(rampVector, hit);
				++_count.rampSecondMoves;

				const float secondHitPercent = hit.time * initialPercentRemaining;
				percentTimeApplied = ClampF(percentTimeApplied + secondHitPercent, 0.f, 1.f);
			}

			if (hit.IsValidBlockingHit())
			{
				// 걷는 중의 유효한 부딪힘이면 CanStepUp 은 참이다(맵에 턱 금지 표시가 없다)
				if (!StepUp(delta * (double)(1.f - percentTimeApplied), hit, outComputedFloor, outFloor))
				{
					SlideAlongSurface(delta, 1.f - percentTimeApplied, hit.normal, hit);
				}
			}
		}
	}

	bool CMoveSim::StepUp(const Vec3& delta, const MoveHit& inHit, bool& outComputedFloor, FloorResult& outFloor)
	{
		MoveState& s = *_s;
		if (!inHit.IsValidBlockingHit() || !IsWalking() || _p.maxStepHeight <= 0.f) return false;
		++_count.stepUpTried;

		const Vec3 oldLocation = s.location;
		const float pawnRadius = _p.capsuleRadius;
		const float pawnHalfHeight = _p.capsuleHalfHeight;

		const float initialImpactZ = (float)inHit.impactPoint.z;
		const float oldLocationZ = (float)oldLocation.z;
		if (initialImpactZ > oldLocationZ + (pawnHalfHeight - pawnRadius))
		{
			return false;
		}

		float stepTravelUpHeight = _p.maxStepHeight;
		float stepTravelDownHeight = stepTravelUpHeight;
		const float stepSideZ = (float)inHit.impactNormal.z;
		float pawnInitialFloorBaseZ = oldLocationZ - pawnHalfHeight;
		float pawnFloorPointZ = pawnInitialFloorBaseZ;

		if (s.floor.IsWalkableFloor())
		{
			const float floorDist = std::max(0.f, s.floor.GetDistanceToFloor());
			pawnInitialFloorBaseZ -= floorDist;
			stepTravelUpHeight = std::max(stepTravelUpHeight - floorDist, 0.f);
			stepTravelDownHeight = _p.maxStepHeight + kMaxFloorDist * 2.f;

			const bool hitVerticalFace = !IsWithinEdgeTolerance(inHit.location, inHit.impactPoint, pawnRadius);
			if (!s.floor.lineTrace && !hitVerticalFace)
			{
				pawnFloorPointZ = (float)s.floor.hit.impactPoint.z;
			}
			else
			{
				pawnFloorPointZ -= s.floor.floorDist;
			}
		}

		if (initialImpactZ <= pawnInitialFloorBaseZ)
		{
			return false;
		}

		const auto reject = [&]()
		{
			s.location = oldLocation;
			++_count.stepUpRejected;
			return false;
		};

		// 위로
		MoveHit sweepUpHit;
		MoveComponent(Vec3(0, 0, stepTravelUpHeight), true, &sweepUpHit, false);
		if (sweepUpHit.startPenetrating) return reject();

		// 앞으로
		MoveHit hit;
		MoveComponent(delta, true, &hit, false);
		if (hit.blocking)
		{
			if (hit.startPenetrating) return reject();

			const float forwardHitTime = hit.time;
			const float forwardSlideAmount = SlideAlongSurface(delta, 1.f - hit.time, hit.normal, hit);
			if (forwardHitTime == 0.f && forwardSlideAmount == 0.f) return reject();
		}

		// 아래로
		MoveComponent(Vec3(0, 0, -(double)stepTravelDownHeight), true, &hit, false);
		if (hit.startPenetrating) return reject();

		bool computedFloor = false;
		FloorResult stepDownFloor;
		if (hit.IsValidBlockingHit())
		{
			const float deltaZ = (float)hit.impactPoint.z - pawnFloorPointZ;
			if (deltaZ > _p.maxStepHeight) return reject();

			if (!IsWalkable(hit))
			{
				if (Dot(delta, hit.impactNormal) < 0.0) return reject();
				if (hit.location.z > (double)oldLocationZ) return reject();
			}

			if (!IsWithinEdgeTolerance(hit.location, hit.impactPoint, pawnRadius)) return reject();

			FindFloor(s.location, stepDownFloor, &hit);
			if (hit.location.z > (double)oldLocationZ)
			{
				if (!stepDownFloor.blockingHit && stepSideZ < kMaxStepSideZ) return reject();
			}
			computedFloor = true;
		}

		outComputedFloor = computedFloor;
		outFloor = stepDownFloor;
		++_count.stepUpOk;
		return true;
	}

	void CMoveSim::MaintainHorizontalGroundVelocity()
	{
		_s->velocity.z = 0.0;
	}

	void CMoveSim::AdjustFloorHeight()
	{
		MoveState& s = *_s;
		if (!s.floor.IsWalkableFloor()) return;

		float oldFloorDist = s.floor.floorDist;
		if (s.floor.lineTrace)
		{
			if (oldFloorDist < kMinFloorDist && s.floor.lineDist >= kMinFloorDist)
			{
				return;		// 올라가면 못 걷는 벽을 타게 된다
			}
			oldFloorDist = s.floor.lineDist;
		}

		if (oldFloorDist < kMinFloorDist || oldFloorDist > kMaxFloorDist)
		{
			++_count.floorHeightAdjusts;
			MoveHit adjustHit;
			const double initialZ = s.location.z;
			const float avgFloorDist = (kMinFloorDist + kMaxFloorDist) * 0.5f;
			const float moveDist = avgFloorDist - oldFloorDist;
			SafeMove(Vec3(0, 0, moveDist), adjustHit);

			if (!adjustHit.IsValidBlockingHit())
			{
				s.floor.floorDist += moveDist;
			}
			else if (moveDist > 0.f)
			{
				s.floor.floorDist += (float)(s.location.z - initialZ);
			}
			else
			{
				s.floor.floorDist = (float)(s.location.z - adjustHit.location.z);
				if (IsWalkable(adjustHit))
				{
					SetFromSweep(s.floor, adjustHit, s.floor.floorDist, true);
				}
			}

			s.justTeleported = s.justTeleported || oldFloorDist < 0.f;
			s.forceNextFloorCheck = true;
		}
	}

	void CMoveSim::StartFalling(int iterations, float remainingTime, float timeTick, const Vec3& delta, const Vec3& subLocation)
	{
		MoveState& s = *_s;
		const float desiredDist = (float)Size(delta);
		const float actualDist = (float)Size(Flat(s.location - subLocation));
		remainingTime = desiredDist < kKindaSmall ? 0.f : remainingTime + timeTick * (1.f - std::min(1.f, actualDist / desiredDist));

		++_count.walkOffLedge;
		if (IsWalking())
		{
			SetMode(EMoveMode::Falling);
		}
		StartNewPhysics(remainingTime, iterations);
	}

	void CMoveSim::PhysWalking(float deltaTime, int iterations)
	{
		MoveState& s = *_s;
		if (deltaTime < kMinTickTime) return;

		s.justTeleported = false;
		float remainingTime = deltaTime;

		while (remainingTime >= kMinTickTime && iterations < _p.maxSimulationIterations)
		{
			++iterations;
			++_count.walkingIterations;
			s.justTeleported = false;
			const float timeTick = GetSimulationTimeStep(remainingTime, iterations);
			remainingTime -= timeTick;

			const Vec3 oldLocation = s.location;

			MaintainHorizontalGroundVelocity();
			s.acceleration = Flat(s.acceleration);

			CalcVelocity(timeTick, _p.groundFriction, _p.brakingDecelerationWalking);

			const Vec3 moveVelocity = s.velocity;
			const Vec3 delta = moveVelocity * (double)timeTick;
			const bool zeroDelta = IsNearlyZero(delta);
			bool computedFloor = false;
			FloorResult stepDownFloor;

			if (zeroDelta)
			{
				remainingTime = 0.f;
			}
			else
			{
				MoveAlongFloor(moveVelocity, timeTick, computedFloor, stepDownFloor);
			}

			if (computedFloor)
			{
				s.floor = stepDownFloor;
			}
			else
			{
				FindFloor(s.location, s.floor, nullptr);
			}

			if (s.floor.IsWalkableFloor())
			{
				AdjustFloorHeight();
			}
			else if (s.floor.hit.startPenetrating && remainingTime <= 0.f)
			{
				// 겹친 채로 바닥을 못 찾았다 — 아래로 가지 말고 빠져나온다
				++_count.floorPopOut;
				MoveHit popHit = s.floor.hit;
				popHit.traceEnd = popHit.traceStart + Vec3(0, 0, kMaxFloorDist);
				const Vec3 adjustment = GetPenetrationAdjustment(popHit);
				ResolvePenetration(adjustment, popHit);
				s.forceNextFloorCheck = true;
			}

			if (!s.floor.IsWalkableFloor() && !s.floor.hit.startPenetrating)
			{
				// bCanWalkOffLedges = true 전제라 CheckFall 은 항상 떨어뜨린다
				StartFalling(iterations, remainingTime, timeTick, delta, oldLocation);
				return;
			}

			if (!s.justTeleported && timeTick >= kMinTickTime)
			{
				s.velocity = (s.location - oldLocation) * (1.0 / (double)timeTick);
				MaintainHorizontalGroundVelocity();
			}

			if (Same(s.location, oldLocation))
			{
				remainingTime = 0.f;
				break;
			}
		}

		MaintainHorizontalGroundVelocity();
	}

	// ---------------------------------------------------------------- 바닥

	bool CMoveSim::IsWalkable(const MoveHit& hit) const
	{
		if (!hit.IsValidBlockingHit()) return false;
		const double impactNormalZ = hit.impactNormal.z;
		if (impactNormalZ < (double)kKindaSmall) return false;
		if (impactNormalZ < (double)_p.walkableFloorZ) return false;
		return true;
	}

	bool CMoveSim::IsWithinEdgeTolerance(const Vec3& capsuleLocation, const Vec3& impactPoint, float radius) const
	{
		const float distFromCenterSq = (float)SizeSq2D(impactPoint - capsuleLocation);
		const float reducedRadiusSq = Square(std::max(kSweepEdgeRejectDistance + kKindaSmall, radius - kSweepEdgeRejectDistance));
		return distFromCenterSq < reducedRadiusSq;
	}

	void CMoveSim::ComputeFloorDist(const Vec3& location, float lineDistance, float sweepDistance, FloorResult& out, float sweepRadius, const MoveHit* downwardSweep)
	{
		out = FloorResult();
		const float pawnRadius = _p.capsuleRadius;
		const float pawnHalfHeight = _p.capsuleHalfHeight;

		bool skipSweep = false;
		if (downwardSweep && downwardSweep->IsValidBlockingHit())
		{
			const Vec3 traceDelta = downwardSweep->traceStart - downwardSweep->traceEnd;
			const bool isDownward = traceDelta.z > 0.0;
			const bool isVertical = SizeSq2D(traceDelta) <= (double)kKindaSmall;
			if (isDownward && isVertical)
			{
				if (IsWithinEdgeTolerance(downwardSweep->location, downwardSweep->impactPoint, pawnRadius))
				{
					skipSweep = true;
					++_count.floorSuppliedSweep;
					const bool walkable = IsWalkable(*downwardSweep);
					const float floorDist = (float)(location.z - downwardSweep->location.z);
					SetFromSweep(out, *downwardSweep, floorDist, walkable);
					if (walkable) return;
				}
			}
		}

		if (sweepDistance < lineDistance) return;

		if (!skipSweep && sweepDistance > 0.f && sweepRadius > 0.f)
		{
			const float shrinkScale = 0.9f;
			const float shrinkScaleOverlap = 0.1f;
			float shrinkHeight = (pawnHalfHeight - pawnRadius) * (1.f - shrinkScale);
			float traceDist = sweepDistance + shrinkHeight;
			float radius = sweepRadius;
			float halfHeight = pawnHalfHeight - shrinkHeight;

			MoveHit hit;
			++_count.floorSweeps;
			SweepTest(location, location + Vec3(0, 0, -(double)traceDist), radius, halfHeight, hit);

			if (hit.blocking)
			{
				if (hit.startPenetrating || !IsWithinEdgeTolerance(location, hit.impactPoint, radius))
				{
					radius = std::max(0.f, radius - kSweepEdgeRejectDistance - kKindaSmall);
					// FCollisionShape::IsNearlyZero — 반지름·반높이가 거의 0 일 때만 건너뛴다
					if (radius > kKindaSmall)
					{
						shrinkHeight = (pawnHalfHeight - pawnRadius) * (1.f - shrinkScaleOverlap);
						traceDist = sweepDistance + shrinkHeight;
						halfHeight = std::max(pawnHalfHeight - shrinkHeight, radius);
						++_count.floorSecondSweeps;
						SweepTest(location, location + Vec3(0, 0, -(double)traceDist), radius, halfHeight, hit);
					}
				}

				const float maxPenetrationAdjust = std::max(kMaxFloorDist, pawnRadius);
				const float sweepResult = std::max(-maxPenetrationAdjust, hit.time * traceDist - shrinkHeight);

				SetFromSweep(out, hit, sweepResult, false);
				if (hit.IsValidBlockingHit() && IsWalkable(hit))
				{
					if (sweepResult <= sweepDistance)
					{
						out.walkableFloor = true;
						return;
					}
				}
			}
		}

		if (!out.blockingHit && !out.hit.startPenetrating)
		{
			out.floorDist = sweepDistance;
			return;
		}

		if (lineDistance > 0.f)
		{
			const float shrinkHeight = pawnHalfHeight;
			const float traceDist = lineDistance + shrinkHeight;
			const Vec3 lineEnd = location + Vec3(0, 0, -(double)traceDist);

			RayHit ray;
			++_count.floorLineTraces;
			if (_map.Raycast(location, lineEnd, MAP_BLOCK_MOVE, ray))
			{
				const float time = (float)(ray.distance / (double)traceDist);
				if (time > 0.f)
				{
					MoveHit lineHit;
					InitHit(lineHit, location, lineEnd);
					lineHit.blocking = true;
					lineHit.time = time;
					lineHit.location = location + Vec3(0, 0, -ray.distance);
					lineHit.impactPoint = lineHit.location;
					lineHit.normal = ray.normal;
					lineHit.impactNormal = ray.normal;

					const float maxPenetrationAdjust = std::max(kMaxFloorDist, pawnRadius);
					const float lineResult = std::max(-maxPenetrationAdjust, time * traceDist - shrinkHeight);

					out.blockingHit = true;
					if (lineResult <= lineDistance && IsWalkable(lineHit))
					{
						++_count.floorLineWalkable;
						SetFromLineTrace(out, lineHit, out.floorDist, lineResult, true);
						return;
					}
				}
			}
		}

		out.walkableFloor = false;
	}

	float CMoveSim::GetValidPerchRadius() const
	{
		const float threshold = std::max(0.f, _p.perchRadiusThreshold);
		return ClampF(_p.capsuleRadius - threshold, 0.11f, _p.capsuleRadius);
	}

	bool CMoveSim::ShouldComputePerchResult(const MoveHit& hit) const
	{
		if (!hit.IsValidBlockingHit()) return false;
		if (std::max(0.f, _p.perchRadiusThreshold) <= kSweepEdgeRejectDistance) return false;

		const float distFromCenterSq = (float)SizeSq2D(hit.impactPoint - hit.location);
		const float standOnEdgeRadius = GetValidPerchRadius();
		return distFromCenterSq > Square(standOnEdgeRadius);
	}

	bool CMoveSim::ComputePerchResult(float testRadius, const MoveHit& hit, float maxFloorDist, FloorResult& out)
	{
		if (maxFloorDist <= 0.f) return false;

		const Vec3 capsuleLocation = hit.location;
		const float hitAboveBase = std::max(0.f, (float)(hit.impactPoint.z - capsuleLocation.z) + _p.capsuleHalfHeight);
		const float perchLineDist = std::max(0.f, maxFloorDist - hitAboveBase);
		const float perchSweepDist = std::max(0.f, maxFloorDist);
		const float actualSweepDist = perchSweepDist + _p.capsuleRadius;
		ComputeFloorDist(capsuleLocation, perchLineDist, actualSweepDist, out, testRadius, nullptr);

		if (!out.IsWalkableFloor()) return false;
		if (hitAboveBase + out.floorDist > maxFloorDist)
		{
			out.walkableFloor = false;
			return false;
		}
		return true;
	}

	void CMoveSim::FindFloor(const Vec3& location, FloorResult& out, const MoveHit* downwardSweep)
	{
		MoveState& s = *_s;
		const float heightCheckAdjust = IsWalking() ? kMaxFloorDist + kKindaSmall : -kMaxFloorDist;
		const float floorSweepTraceDist = std::max(kMaxFloorDist, _p.maxStepHeight + heightCheckAdjust);
		const float floorLineTraceDist = floorSweepTraceDist;
		if (floorLineTraceDist > 0.f || floorSweepTraceDist > 0.f)
		{
			s.forceNextFloorCheck = false;
			ComputeFloorDist(location, floorLineTraceDist, floorSweepTraceDist, out, _p.capsuleRadius, downwardSweep);
		}

		// 수직 판정 결과가 캡슐 가장자리면 안쪽 반지름으로 다시 본다 — 턱 끝에 얼마나 걸칠 수 있나
		if (out.blockingHit && !out.lineTrace && ShouldComputePerchResult(out.hit))
		{
			++_count.perchTried;
			float maxPerchFloorDist = std::max(kMaxFloorDist, _p.maxStepHeight + heightCheckAdjust);
			if (IsWalking())
			{
				maxPerchFloorDist += std::max(0.f, _p.perchAdditionalHeight);
			}

			FloorResult perch;
			if (ComputePerchResult(GetValidPerchRadius(), out.hit, maxPerchFloorDist, perch))
			{
				++_count.perchOk;
				const float avgFloorDist = (kMinFloorDist + kMaxFloorDist) * 0.5f;
				const float moveUpDist = avgFloorDist - out.floorDist;
				if (moveUpDist + perch.floorDist >= maxPerchFloorDist)
				{
					out.floorDist = avgFloorDist;
				}
				if (!out.walkableFloor)
				{
					++_count.perchUnwalkableOverride;
					SetFromLineTrace(out, perch.hit, out.floorDist, std::max(out.floorDist, kMinFloorDist), true);
				}
			}
			else
			{
				++_count.perchFail;
				out.walkableFloor = false;
			}
		}
	}

	bool CMoveSim::IsValidLandingSpot(const Vec3& location, const MoveHit& hit)
	{
		if (!hit.blocking) return false;

		if (!hit.startPenetrating)
		{
			if (!IsWalkable(hit)) return false;

			const double lowerHemisphereZ = hit.location.z - (double)_p.capsuleHalfHeight + (double)_p.capsuleRadius;
			if (hit.impactPoint.z >= lowerHemisphereZ) return false;
			if (!IsWithinEdgeTolerance(hit.location, hit.impactPoint, _p.capsuleRadius)) return false;
		}
		else
		{
			if (hit.normal.z < (double)kKindaSmall) return false;
		}

		FloorResult floor;
		FindFloor(location, floor, &hit);
		return floor.IsWalkableFloor();
	}

	bool CMoveSim::ShouldCheckForValidLandingSpot(const MoveHit& hit) const
	{
		if (hit.normal.z > (double)kKindaSmall && !NearlyEqual(hit.normal, hit.impactNormal))
		{
			if (IsWithinEdgeTolerance(_s->location, hit.impactPoint, _p.capsuleRadius))
			{
				return true;
			}
		}
		return false;
	}

	// ---------------------------------------------------------------- 낙하

	void CMoveSim::ProcessLanded(float remainingTime, int iterations)
	{
		++_count.landed;
		if (IsFalling())
		{
			SetMode(EMoveMode::Walking);
		}
		StartNewPhysics(remainingTime, iterations);
	}

	void CMoveSim::PhysFalling(float deltaTime, int iterations)
	{
		MoveState& s = *_s;
		if (deltaTime < kMinTickTime) return;

		const Vec3 fallAcceleration = Flat(GetFallingLateralAcceleration());
		const bool hasLimitedAirControl = SizeSq2D(fallAcceleration) > 0.0;

		float remainingTime = deltaTime;
		while (remainingTime >= kMinTickTime && iterations < _p.maxSimulationIterations)
		{
			++iterations;
			++_count.fallingIterations;
			float timeTick = GetSimulationTimeStep(remainingTime, iterations);
			remainingTime -= timeTick;

			const Vec3 oldLocation = s.location;
			s.justTeleported = false;
			const Vec3 oldVelocityWithRootMotion = s.velocity;
			const Vec3 oldVelocity = s.velocity;

			const float maxDecel = _p.brakingDecelerationFalling;
			{
				const Vec3 savedAcceleration = s.acceleration;
				s.acceleration = fallAcceleration;
				s.velocity.z = 0.0;
				CalcVelocity(timeTick, _p.fallingLateralFriction, maxDecel);
				s.velocity.z = oldVelocity.z;
				s.acceleration = savedAcceleration;
			}

			const Vec3 gravity(0, 0, _p.gravityZ);
			float gravityTime = timeTick;

			bool endingJumpForce = false;
			if (s.jumpForceTimeRemaining > 0.f)
			{
				const float jumpForceTime = std::min(s.jumpForceTimeRemaining, timeTick);
				gravityTime = _p.applyGravityWhileJumping ? timeTick : std::max(0.f, timeTick - jumpForceTime);
				s.jumpForceTimeRemaining -= jumpForceTime;
				if (s.jumpForceTimeRemaining <= 0.f)
				{
					ResetJumpState();
					endingJumpForce = true;
				}
			}

			s.velocity = NewFallVelocity(s.velocity, gravity, gravityTime);

			// 꼭대기를 정확히 지나도록 틱을 쪼갠다 — 프레임이 달라도 점프 높이가 같아지게
			if (oldVelocityWithRootMotion.z > 0.0 && s.velocity.z <= 0.0 && s.numJumpApexAttempts < _p.maxJumpApexAttemptsPerSimulation)
			{
				const Vec3 derivedAccel = (s.velocity - oldVelocityWithRootMotion) * (1.0 / (double)timeTick);
				if (std::fabs(derivedAccel.z) > (double)kSmall)
				{
					const float timeToApex = (float)(-oldVelocityWithRootMotion.z / derivedAccel.z);
					const float apexTimeMinimum = 0.0001f;
					if (timeToApex >= apexTimeMinimum && timeToApex < timeTick)
					{
						const Vec3 apexVelocity = oldVelocityWithRootMotion + derivedAccel * (double)timeToApex;
						s.velocity = apexVelocity;
						s.velocity.z = 0.0;

						const float timeToRefund = timeTick - timeToApex;
						remainingTime += timeToRefund;
						timeTick = timeToApex;
						--iterations;
						++s.numJumpApexAttempts;
						++_count.apexSubsteps;
					}
				}
			}

			Vec3 adjusted = (oldVelocityWithRootMotion + s.velocity) * 0.5 * (double)timeTick;
			if (endingJumpForce && !_p.applyGravityWhileJumping)
			{
				const float nonGravityTime = std::max(0.f, timeTick - gravityTime);
				adjusted = oldVelocityWithRootMotion * (double)nonGravityTime + (oldVelocityWithRootMotion + s.velocity) * 0.5 * (double)gravityTime;
			}

			MoveHit hit;
			SafeMove(adjusted, hit);

			float lastMoveTimeSlice = timeTick;
			float subTimeTickRemaining = timeTick * (1.f - hit.time);

			if (hit.blocking)
			{
				if (IsValidLandingSpot(s.location, hit))
				{
					remainingTime += subTimeTickRemaining;
					ProcessLanded(remainingTime, iterations);
					return;
				}

				adjusted = s.velocity * (double)timeTick;

				if (!hit.startPenetrating && ShouldCheckForValidLandingSpot(hit))
				{
					const Vec3 pawnLocation = s.location;
					FloorResult floorResult;
					FindFloor(pawnLocation, floorResult, nullptr);
					if (!floorResult.lineTrace && floorResult.IsWalkableFloor() && IsValidLandingSpot(pawnLocation, floorResult.hit))
					{
						++_count.landedOnEdgeFloor;
						remainingTime += subTimeTickRemaining;
						ProcessLanded(remainingTime, iterations);
						return;
					}
				}

				Vec3 velocityNoAirControl = oldVelocity;
				Vec3 airControlAccel = s.acceleration;
				if (hasLimitedAirControl)
				{
					{
						const Vec3 savedAcceleration = s.acceleration;
						const Vec3 savedVelocity = s.velocity;
						s.acceleration = Vec3();
						s.velocity = oldVelocity;
						s.velocity.z = 0.0;
						CalcVelocity(timeTick, _p.fallingLateralFriction, maxDecel);
						velocityNoAirControl = Vec3(s.velocity.x, s.velocity.y, oldVelocity.z);
						velocityNoAirControl = NewFallVelocity(velocityNoAirControl, gravity, gravityTime);
						s.acceleration = savedAcceleration;
						s.velocity = savedVelocity;
					}

					airControlAccel = (s.velocity - velocityNoAirControl) * (1.0 / (double)timeTick);
					const Vec3 airControlDeltaV = LimitAirControl(airControlAccel, hit) * (double)lastMoveTimeSlice;
					adjusted = (velocityNoAirControl + airControlDeltaV) * (double)lastMoveTimeSlice;
				}

				const Vec3 oldHitNormal = hit.normal;
				const Vec3 oldHitImpactNormal = hit.impactNormal;
				Vec3 delta = ComputeSlideVector(adjusted, 1.f - hit.time, oldHitNormal);

				if (subTimeTickRemaining > kKindaSmall && !s.justTeleported)
				{
					s.velocity = delta * (1.0 / (double)subTimeTickRemaining);
				}

				if (subTimeTickRemaining > kKindaSmall && Dot(delta, adjusted) > 0.0)
				{
					SafeMove(delta, hit);

					if (hit.blocking)
					{
						++_count.fallSecondWall;
						lastMoveTimeSlice = subTimeTickRemaining;
						subTimeTickRemaining = subTimeTickRemaining * (1.f - hit.time);

						if (IsValidLandingSpot(s.location, hit))
						{
							remainingTime += subTimeTickRemaining;
							ProcessLanded(remainingTime, iterations);
							return;
						}

						if (hasLimitedAirControl && hit.normal.z > (double)kVerticalSlopeNormalZ)
						{
							const Vec3 lastMoveNoAirControl = velocityNoAirControl * (double)lastMoveTimeSlice;
							delta = ComputeSlideVector(lastMoveNoAirControl, 1.f, oldHitNormal);
						}

						const Vec3 preTwoWallDelta = delta;
						TwoWallAdjust(delta, hit, oldHitNormal);
						delta = HandleSlopeBoosting(delta, preTwoWallDelta, 1.f - hit.time, hit.normal);

						if (hasLimitedAirControl)
						{
							const Vec3 airControlDeltaV = LimitAirControl(airControlAccel, hit) * (double)subTimeTickRemaining;
							if (Dot(airControlDeltaV, oldHitNormal) > 0.0)
							{
								delta = delta + airControlDeltaV * (double)subTimeTickRemaining;
							}
						}

						if (subTimeTickRemaining > kKindaSmall && !s.justTeleported)
						{
							s.velocity = delta * (1.0 / (double)subTimeTickRemaining);
						}

						const bool ditch = oldHitImpactNormal.z > 0.0 && hit.impactNormal.z > 0.0
							&& std::fabs(delta.z) <= (double)kKindaSmall && Dot(hit.impactNormal, oldHitImpactNormal) < 0.0;
						SafeMove(delta, hit);
						if (hit.time == 0.f)
						{
							++_count.fallStuckSideStep;
							Vec3 sideDelta = SafeNormal(Flat(oldHitNormal + hit.impactNormal));
							if (IsNearlyZero(sideDelta))
							{
								sideDelta = SafeNormal(Vec3(oldHitNormal.y, -oldHitNormal.x, 0));
							}
							SafeMove(sideDelta, hit);
						}

						if (ditch || IsValidLandingSpot(s.location, hit) || hit.time == 0.f)
						{
							remainingTime = 0.f;
							ProcessLanded(remainingTime, iterations);
							return;
						}
						if (std::max(0.f, _p.perchRadiusThreshold) > 0.f && hit.time == 1.f && oldHitImpactNormal.z >= (double)_p.walkableFloorZ)
						{
							// 턱 끝에 걸치는 반지름 안쪽 틈에 끼었다 — 드문 경우. 튕겨 빠져나온다
							const float zMovedDist = (float)std::fabs(s.location.z - oldLocation.z);
							const float movedDist2D = (float)std::sqrt(SizeSq2D(s.location - oldLocation));
							if (zMovedDist <= 0.2f * timeTick && movedDist2D <= 4.f * timeTick)
							{
								++_count.fallVirtualDitch;
								const auto random01 = [&s]()
								{
									s.randomState = s.randomState * 1664525u + 1013904223u;
									return (float)(s.randomState >> 8) / 16777216.f;
								};
								s.velocity.x += (double)(0.25f * s.maxSpeed * (random01() - 0.5f));
								s.velocity.y += (double)(0.25f * s.maxSpeed * (random01() - 0.5f));
								s.velocity.z = (double)std::max(_p.jumpZVelocity * 0.25f, 1.f);
								delta = s.velocity * (double)timeTick;
								SafeMove(delta, hit);
							}
						}
					}
				}
			}

			if (SizeSq2D(s.velocity) <= (double)(kKindaSmall * 10.f))
			{
				s.velocity.x = 0.0;
				s.velocity.y = 0.0;
			}
		}
	}
}
