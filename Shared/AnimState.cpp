#include "AnimState.h"

#include <algorithm>
#include <cmath>

namespace mo
{
	namespace
	{
		// 값과 근거는 MOBSimpleAnimInstance.cpp 의 같은 이름 상수에 적혀 있다(Lyra 실행값에서 읽었거나 시험용 제안값)
		constexpr float kMoveFullSpeed = 300.f;
		constexpr float kBlendSeconds = 0.1f;
		constexpr float kMinPlayRate = 0.25f;
		constexpr float kMaxPlayRate = 1.5f;
		constexpr float kAimBlendSpeed = 10.f;
		constexpr float kJumpBlendSeconds = 0.1f;
		constexpr float kApexLeadSeconds = 0.25f;
		constexpr float kApexExitRemaining = 0.12f;
		constexpr float kLandFadeSeconds = 0.3f;
		constexpr float kLandedStandSeconds = 0.15f;
		constexpr float kLandedMoveSeconds = 0.2f;
		constexpr float kIdleBreakDelay = 6.f;
		constexpr float kStopClipSeconds = 1.35f;
		constexpr float kPlantReleaseSeconds = 0.2f;
		constexpr float kPlantLandDelay = 0.1f;
		constexpr float kPlantTimeSlack = 0.001f;
		constexpr int kLandTableHz = 120;
		constexpr int kTransitionTableHz = 120;
		constexpr float kCardinalDeadZone = 10.f;
		constexpr float kToStartSeconds = 0.21f;
		constexpr float kStopToStartSeconds = 0.175f;
		constexpr float kToStopSeconds = 0.28f;
		constexpr float kToPivotSeconds = 0.25f;
		constexpr float kStartToCycleSeconds = 0.99f;
		constexpr float kPivotToCycleSeconds = 0.5f;
		constexpr float kStopToIdleSeconds = 0.74f;
		constexpr float kStartExitTimes[] = { 0.791f, 1.165f, 1.185f, 1.122f };
		constexpr float kStopExitTimes[] = { 1.712f, 2.046f, 2.180f, 2.277f };
		constexpr float kPivotExitTimes[] = { 1.344f, 1.591f, 1.565f, 1.358f };
		constexpr float kStepLeaderWeight = 0.5f;
		constexpr float kPivotMinSpeed = 10.f;
		constexpr float kDistanceRateMin = 0.25f;
		constexpr float kDistanceRateMax = 5.f;

		constexpr float kSmallNumber = 1.e-8f;			// 엔진 UE_SMALL_NUMBER
		constexpr float kKindaSmall = 1.e-4f;			// 엔진 UE_KINDA_SMALL_NUMBER
		constexpr float kZeroWeight = 0.00001f;			// 엔진 ZERO_ANIMWEIGHT_THRESH
		constexpr float kPi = 3.1415926535897932f;		// 엔진 UE_PI

		// 패킷 시각의 눈금 — 1/6000초 칸. 가장 긴 클립(대기 몸짓 8.33초)이 들어가도록 고른 값으로, 2바이트 최대는 10.92초다. 0xFFFF 는 「안 함」 표시로 비워 둔다
		constexpr float kWireTimeScale = 6000.f;
		constexpr float kWireTimeMax = 65534.f / kWireTimeScale;
		constexpr float kWirePlantScale = 100.f;		// 발 디딤 내림 cm → 0.01cm 칸

		enum { kClipIdle, kClipFwd, kClipBwd, kClipLeft, kClipRight };
		enum { kJumpStart, kJumpStartLoop, kJumpApex, kJumpFallLoop, kJumpLand };
		enum { kTransitionStart, kTransitionStop, kTransitionPivot };
		enum { kDirFwd, kDirBwd, kDirLeft, kDirRight };
		constexpr int kJumpNone = -1;

		float Clamp(float x, float low, float high) { return std::max(low, std::min(x, high)); }

		float Smooth(float x)
		{
			x = Clamp(x, 0.f, 1.f);
			return x * x * (3.f - 2.f * x);
		}

		int CardinalFromAngle(float angle)
		{
			const float value = std::abs(angle);
			if (value <= 45.f + kCardinalDeadZone) return kDirFwd;
			if (value >= 135.f - kCardinalDeadZone) return kDirBwd;
			return angle > 0.f ? kDirRight : kDirLeft;
		}

		float AngleOf(const double v[3])
		{
			return std::atan2((float)v[1], (float)v[0]) * (180.f / kPi);
		}

		int Gcd(int a, int b)
		{
			while (b != 0)
			{
				const int t = a % b;
				a = b;
				b = t;
			}
			return a;
		}

		uint8_t ToWireByte(float value) { return (uint8_t)std::lround(Clamp(value, 0.f, 1.f) * 255.f); }
		uint16_t ToWireTime(float seconds) { return (uint16_t)std::lround(Clamp(seconds, 0.f, kWireTimeMax) * kWireTimeScale); }
		float FromWireTime(uint16_t value) { return (float)value / kWireTimeScale; }

		// 엔진 FRotator::NormalizeAxis 뒤 ±90 으로 자른다 — 언리얼 쪽 Pitch 와 같은 식
		float NormalizePitch(float degrees)
		{
			float d = std::fmod(degrees, 360.f);
			if (d < 0.f) d += 360.f;
			if (d > 180.f) d -= 360.f;
			return Clamp(d, -90.f, 90.f);
		}

		// 엔진 FMath::FInterpTo (float)
		float InterpTo(float current, float target, float deltaSeconds, float speed)
		{
			if (speed <= 0.f) return target;
			const float distance = target - current;
			if (distance * distance < kSmallNumber) return target;
			return current + distance * Clamp(deltaSeconds * speed, 0.f, 1.f);
		}
	}

	bool CAnimState::Init(const CAnimClips& clips, const char* bundleName)
	{
		const ClipBundle* bundle = clips.FindBundle(bundleName);
		if (!bundle || bundle->clips.size() != ACLIP_COUNT) return false;

		_stepCycle = 1;
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			const AnimClip& clip = bundle->clips[(size_t)ACLIP_IDLE + i];
			_lengths[i] = std::max(clip.length, 0.01f);
			_rootSpeeds[i] = clip.rootSpeed;

			// 발 표시를 왼발부터 줄 세운다. 번갈아 있고 개수가 짝수여야 한 바퀴 돌아도 발 순서가 안 깨진다
			const std::vector<ClipMarker>& markers = clip.markers;
			size_t firstLeft = 0;
			while (firstLeft < markers.size() && !markers[firstLeft].left) ++firstLeft;
			_stepTimes[i].clear();
			for (size_t m = 0; m < markers.size() && firstLeft < markers.size(); ++m)
			{
				const size_t index = (firstLeft + m) % markers.size();
				_stepTimes[i].push_back(markers[index].time + (index < firstLeft ? _lengths[i] : 0.f));
			}
			if (i != kClipIdle)
			{
				const int count = (int)_stepTimes[i].size();
				if (count == 0 || count % 2 != 0) return false;
				_stepCycle = _stepCycle / Gcd(_stepCycle, count) * count;
			}
		}

		if (_stepCycle > 255) return false;		// 패킷의 걸음 번호는 1바이트다

		for (int i = 0; i < kAnimStackClips; ++i) _stackLengths[i] = std::max(bundle->clips[(size_t)ACLIP_JUMP_START + i].length, 0.01f);
		for (const AnimClip& clip : bundle->clips)
		{
			if (clip.length > kWireTimeMax) return false;		// 패킷의 시각은 이 길이까지만 담는다
		}
		for (int t = 0; t < kAnimTransitions; ++t)
		{
			const AnimClip& clip = bundle->clips[(size_t)ACLIP_TRANSITION_FIRST + t];
			if (!clip.hasCurve || clip.curveHz != (uint32_t)kTransitionTableHz) return false;
			_transitionDistances[t] = &clip.curve;
			_transitionMarkers[t] = &clip.markers;
		}
		const AnimClip& land = bundle->clips[ACLIP_JUMP_LAND];
		if (!land.hasCurve || land.curveHz != (uint32_t)kLandTableHz) return false;
		_landDistances = &land.curve;
		_recoveryLength = std::max(bundle->clips[ACLIP_RECOVERY].length, 0.01f);
		_fidgetLength = std::max(bundle->clips[ACLIP_FIDGET].length, 0.01f);

		_groundFriction = bundle->groundFriction;
		_brakingFriction = bundle->brakingFriction;
		_brakingFrictionFactor = bundle->brakingFrictionFactor;
		_brakingDecelerationWalking = bundle->brakingDecelerationWalking;
		_gravity = -bundle->gravityZ;
		_separateBraking = bundle->separateBraking;

		Reset();
		return true;
	}

	void CAnimState::Reset()
	{
		for (int i = 0; i < kAnimWalkClips; ++i) _weights[i] = i == kClipIdle ? 1.f : 0.f;
		_step = 0;
		_stepFrac = 0.f;
		_timesStep = 0;
		_timesStepFrac = 0.f;
		// 판 시작(순간이동 + 멈춤)도 「방금 멈췄다」로 본다 — 대기 몸짓 시계와 같은 기준
		_idleTime = _lengths[kClipIdle] > kStopClipSeconds ? _lengths[kClipIdle] - kStopClipSeconds : 0.f;
		_timesIdle = _idleTime;
		_groundTime = kPlantLandDelay;
		_airTime = kPlantReleaseSeconds;
		_plantDrop = 0.f;
		_aimBlend = 1.f;
		_count = 0;
		_wasFalling = false;
		_recoveryTime = -1.f;
		_standTime = 0.f;
		_fidgetTime = -1.f;
		_fidgetLeaving = false;
		_moveState = EMoveState::Idle;
		_moveDirection = 0;
		_pivotAccelDirection[0] = _pivotAccelDirection[1] = 0.0;
		_beforePivot = false;
		_followClip = -1;
	}

	void CAnimState::StepSpan(int clip, int step, float& outStart, float& outEnd) const
	{
		const std::vector<float>& table = _stepTimes[clip];
		const size_t k = (size_t)step % table.size();
		outStart = table[k];
		outEnd = k + 1 < table.size() ? table[k + 1] : table[0] + _lengths[clip];
	}

	void CAnimState::UpdateIdleBreak(bool still, float deltaSeconds)
	{
		if (_fidgetTime >= 0.f)
		{
			_fidgetTime += deltaSeconds;
			if (!still) _fidgetLeaving = true;
			if (_fidgetTime >= _fidgetLength || (_fidgetLeaving && _weights[kClipIdle] <= kZeroWeight))
			{
				_fidgetTime = -1.f;
				_fidgetLeaving = false;
				_standTime = 0.f;
			}
			return;
		}
		_standTime = still ? _standTime + deltaSeconds : 0.f;
		if (_standTime >= kStopClipSeconds + kIdleBreakDelay)
		{
			_fidgetTime = 0.f;
			_standTime = 0.f;
		}
	}

	void CAnimState::PushJump(int clip, float fadeSeconds)
	{
		if (_count == 0)
		{
			_stack[0] = Layer();
			_count = 1;
		}
		if (_count == kAnimLayerMax)
		{
			for (int i = 1; i < kAnimLayerMax; ++i) _stack[i - 1] = _stack[i];
			--_count;
			++_stackOverflows;
		}
		Layer& layer = _stack[_count++];
		layer.clip = clip;
		layer.time = 0.f;
		layer.fade = fadeSeconds > 0.f ? 0.f : 1.f;
		layer.fadeSeconds = fadeSeconds;
	}

	float CAnimState::TimeFromDistance(int transition, float value) const
	{
		const std::vector<float>& table = *_transitionDistances[transition];
		const int num = (int)table.size();
		if (num < 2) return 0.f;
		int first = 1;
		int count = num - 2;
		while (count > 0)
		{
			const int half = count / 2;
			const int middle = first + half;
			if (value > table[(size_t)middle])
			{
				first = middle + 1;
				count -= half + 1;
			}
			else
			{
				count = half;
			}
		}
		const float span = table[(size_t)first] - table[(size_t)first - 1];
		const float alpha = std::abs(span) > kSmallNumber ? Clamp((value - table[(size_t)first - 1]) / span, 0.f, 1.f) : 0.f;
		return std::min(((float)(first - 1) + alpha) / kTransitionTableHz, _stackLengths[kAnimJumpClips + transition]);
	}

	float CAnimState::DistanceAtTime(int transition, float time) const
	{
		const std::vector<float>& table = *_transitionDistances[transition];
		if (table.empty()) return 0.f;
		const float position = Clamp(time * kTransitionTableHz, 0.f, (float)(table.size() - 1));
		const int index = std::min((int)std::floor(position), (int)table.size() - 2);
		if (index < 0) return table[0];
		const float a = table[(size_t)index];
		const float b = table[(size_t)index + 1];
		return a + (position - (float)index) * (b - a);
	}

	float CAnimState::TransitionClipTime(const Layer& layer, float deltaSeconds) const
	{
		const int transition = layer.clip - kAnimJumpClips;
		const auto advanceByTravel = [&]()
		{
			const float target = TimeFromDistance(transition, DistanceAtTime(transition, layer.time) + _frameTravel);
			const float rate = deltaSeconds > 0.f ? Clamp((target - layer.time) / deltaSeconds, kDistanceRateMin, kDistanceRateMax) : 1.f;
			return layer.time + rate * deltaSeconds;
		};
		switch (_moveState)
		{
		case EMoveState::Start:
			return advanceByTravel();
		case EMoveState::Stop:
			return _stopDistance > 0.f ? std::max(layer.time, TimeFromDistance(transition, -_stopDistance)) : layer.time + deltaSeconds;
		case EMoveState::Pivot:
			return _beforePivot ? std::max(layer.time, TimeFromDistance(transition, -_pivotDistance)) : advanceByTravel();
		case EMoveState::Idle:
		case EMoveState::Cycle:
		case EMoveState::Air:
			break;
		}
		return layer.time + deltaSeconds;
	}

	bool CAnimState::HasJumpLayer() const
	{
		for (int i = 0; i < _count; ++i)
		{
			if (_stack[i].clip != kJumpNone && _stack[i].clip < kAnimJumpClips) return true;
		}
		return false;
	}

	void CAnimState::EnterTransition(EMoveState state, int direction, float fadeSeconds)
	{
		const int kind = state == EMoveState::Start ? kTransitionStart : state == EMoveState::Stop ? kTransitionStop : kTransitionPivot;
		_moveState = state;
		_moveDirection = direction;
		_followClip = -1;
		PushJump(kAnimJumpClips + kind * 4 + direction, fadeSeconds);
	}

	void CAnimState::EnterLocomotion(EMoveState state, float fadeSeconds)
	{
		_followClip = -1;
		if (state == EMoveState::Cycle && _count > 0 && _stack[_count - 1].clip >= kAnimJumpClips)
		{
			const Layer& top = _stack[_count - 1];
			const int transition = top.clip - kAnimJumpClips;
			const int marker = StepMarkerAt(transition, top.time);
			if (marker != -1)
			{
				_step = (*_transitionMarkers[transition])[(size_t)marker - 1].left ? 0 : _stepCycle - 1;
				_followClip = top.clip;
				_followBaseStep = _step;
				_followBaseMarker = marker;
			}
		}
		if (state == EMoveState::Idle) _idleTime = 0.f;
		_moveState = state;
		PushJump(kJumpNone, fadeSeconds);
	}

	int CAnimState::StepMarkerAt(int transition, float time) const
	{
		const std::vector<ClipMarker>& markers = *_transitionMarkers[transition];
		for (size_t k = 1; k < markers.size(); ++k)
		{
			if (time >= markers[k - 1].time && time < markers[k].time) return (int)k;
		}
		return -1;
	}

	bool CAnimState::FollowStepsOfTransition(float deltaSeconds)
	{
		if (_followClip == -1) return false;
		float layers[kAnimLayerMax] = {};
		JumpWeights(layers);
		for (int i = 0; i < _count; ++i)
		{
			if (_stack[i].clip != _followClip) continue;
			if (layers[i] < kStepLeaderWeight) break;
			const int transition = _followClip - kAnimJumpClips;
			const float time = _stack[i].time + deltaSeconds;
			const int marker = StepMarkerAt(transition, time);
			if (marker == -1) break;
			const std::vector<ClipMarker>& markers = *_transitionMarkers[transition];
			_step = (_followBaseStep + marker - _followBaseMarker) % _stepCycle;
			_stepFrac = Clamp((time - markers[(size_t)marker - 1].time) / std::max(markers[(size_t)marker].time - markers[(size_t)marker - 1].time, 0.001f), 0.f, 1.f);
			return true;
		}
		_followClip = -1;
		return false;
	}

	float CAnimState::PredictStopDistance(float speed) const
	{
		const float friction = std::max(0.f, (_separateBraking ? _brakingFriction : _groundFriction) * std::max(0.f, _brakingFrictionFactor));
		const float braking = std::max(0.f, _brakingDecelerationWalking);
		const float divisor = friction * speed + braking;
		if (divisor <= 0.f) return 0.f;
		const float time = speed / divisor;
		return std::max(0.f, speed * time + 0.5f * (-friction * speed - braking) * time * time);
	}

	float CAnimState::PredictPivotDistance(const double acceleration[3], const double velocity[3]) const
	{
		const float size = (float)std::sqrt(acceleration[0] * acceleration[0] + acceleration[1] * acceleration[1]);
		double direction[2] = {};
		if (size > kSmallNumber)
		{
			const float oneOver = 1.0f / size;
			direction[0] = acceleration[0] * oneOver;
			direction[1] = acceleration[1] * oneOver;
		}
		const float along = (float)(velocity[0] * direction[0] + velocity[1] * direction[1]);
		if (along >= 0.f) return 0.f;
		const float speedAlong = -along;
		const float time = speedAlong / (size + 2.f * speedAlong * _groundFriction);
		const double speed2D = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
		const double forceX = acceleration[0] - (velocity[0] - direction[0] * speed2D) * _groundFriction;
		const double forceY = acceleration[1] - (velocity[1] - direction[1] * speed2D) * _groundFriction;
		const double x = velocity[0] * time + 0.5 * forceX * time * time;
		const double y = velocity[1] * time + 0.5 * forceY * time * time;
		return (float)std::sqrt(x * x + y * y);
	}

	void CAnimState::UpdateTransitions(const AnimMoveInput& input, float deltaSeconds)
	{
		if (input.falling)
		{
			_moveState = EMoveState::Air;
			return;
		}
		const bool accelerating = input.accelerating;
		if (_moveState == EMoveState::Air) _moveState = accelerating ? EMoveState::Cycle : EMoveState::Idle;

		const double* velocity = input.velocity;
		const double* acceleration = input.acceleration;
		const float speed = (float)std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
		_frameTravel = speed * deltaSeconds;

		// 엔진 FVector2D::GetSafeNormal
		double accelDirection[2] = {};
		const double squareSum = acceleration[0] * acceleration[0] + acceleration[1] * acceleration[1];
		if (squareSum > (double)kSmallNumber)
		{
			const double scale = 1.0 / std::sqrt(squareSum);
			accelDirection[0] = acceleration[0] * scale;
			accelDirection[1] = acceleration[1] * scale;
		}

		_beforePivot = accelerating && velocity[0] * accelDirection[0] + velocity[1] * accelDirection[1] < 0.0;
		_stopDistance = PredictStopDistance(speed);
		_pivotDistance = _beforePivot ? PredictPivotDistance(acceleration, velocity) : 0.f;

		const bool pivotNeeded = _beforePivot && speed > kPivotMinSpeed;
		const float topTime = _count > 0 ? _stack[_count - 1].time : 0.f;
		const auto enterPivot = [&]()
		{
			_pivotAccelDirection[0] = accelDirection[0];
			_pivotAccelDirection[1] = accelDirection[1];
			EnterTransition(EMoveState::Pivot, CardinalFromAngle(AngleOf(velocity)), kToPivotSeconds);
		};
		const auto enterStop = [&]() { EnterTransition(EMoveState::Stop, CardinalFromAngle(AngleOf(velocity)), kToStopSeconds); };
		switch (_moveState)
		{
		case EMoveState::Idle:
			if (accelerating) EnterTransition(EMoveState::Start, CardinalFromAngle(AngleOf(acceleration)), kToStartSeconds);
			break;
		case EMoveState::Start:
			if (!accelerating && speed < 1.f)
			{
				_moveState = EMoveState::Idle;
				PushJump(kJumpNone, kToStartSeconds);
			}
			else if (!accelerating) enterStop();
			else if (pivotNeeded) enterPivot();
			else if (topTime >= kStartExitTimes[_moveDirection]) EnterLocomotion(EMoveState::Cycle, kStartToCycleSeconds);
			break;
		case EMoveState::Cycle:
			if (!accelerating) enterStop();
			else if (pivotNeeded) enterPivot();
			break;
		case EMoveState::Stop:
			if (accelerating) EnterTransition(EMoveState::Start, CardinalFromAngle(AngleOf(acceleration)), kStopToStartSeconds);
			else if (speed < 1.f && topTime >= kStopExitTimes[_moveDirection]) EnterLocomotion(EMoveState::Idle, kStopToIdleSeconds);
			break;
		case EMoveState::Pivot:
			if (!accelerating) enterStop();
			else if (pivotNeeded && accelDirection[0] * _pivotAccelDirection[0] + accelDirection[1] * _pivotAccelDirection[1] < 0.0) enterPivot();
			else if (topTime >= kPivotExitTimes[_moveDirection]) EnterLocomotion(EMoveState::Cycle, kPivotToCycleSeconds);
			break;
		case EMoveState::Air:
			break;
		}
	}

	void CAnimState::JumpWeights(float out[kAnimLayerMax]) const
	{
		float remain = 1.f;
		for (int i = _count - 1; i >= 0; --i)
		{
			const float alpha = i == 0 ? 1.f : Smooth(_stack[i].fade);
			out[i] = remain * alpha;
			remain -= out[i];
		}
	}

	float CAnimState::LandTimeFromDistance(float groundDistance) const
	{
		const std::vector<float>& table = *_landDistances;
		const int num = (int)table.size();
		if (num == 0 || groundDistance >= table[0]) return 0.f;
		for (int k = 1; k < num; ++k)
		{
			if (table[(size_t)k] <= groundDistance)
			{
				const float span = table[(size_t)k - 1] - table[(size_t)k];
				const float frac = span > kSmallNumber ? (table[(size_t)k - 1] - groundDistance) / span : 1.f;
				return std::min(((float)(k - 1) + frac) / kLandTableHz, _stackLengths[kJumpLand]);
			}
		}
		return _stackLengths[kJumpLand];
	}

	void CAnimState::UpdateJump(bool falling, bool accelerating, float velocityZ, float timeToApex, float groundDistance, float deltaSeconds)
	{
		const std::vector<float>& land = *_landDistances;
		const int topClip = _count > 0 ? _stack[_count - 1].clip : kJumpNone;
		const float topTime = _count > 0 ? _stack[_count - 1].time : 0.f;
		if (falling && !_wasFalling)
		{
			PushJump(velocityZ > 0.f ? kJumpStart : kJumpFallLoop, kJumpBlendSeconds);
		}
		else if (falling)
		{
			switch (topClip)
			{
			case kJumpStart:
				if (topTime >= _stackLengths[kJumpStart] - kKindaSmall) PushJump(kJumpStartLoop, 0.f);
				break;
			case kJumpStartLoop:
				if (timeToApex < kApexLeadSeconds) PushJump(kJumpApex, kJumpBlendSeconds);
				break;
			case kJumpApex:
				if (_stackLengths[kJumpApex] - topTime < kApexExitRemaining) PushJump(kJumpFallLoop, kJumpBlendSeconds);
				break;
			case kJumpFallLoop:
				if (!land.empty() && groundDistance < land[0]) PushJump(kJumpLand, kLandFadeSeconds);
				break;
			default:
				break;
			}
		}
		else if (_wasFalling)
		{
			PushJump(kJumpNone, accelerating ? kLandedMoveSeconds : kLandedStandSeconds);
			_recoveryTime = 0.f;
			_idleTime = 0.f;
		}
		_wasFalling = falling;

		for (int i = 0; i < _count; ++i)
		{
			Layer& layer = _stack[i];
			if (i == _count - 1 && layer.clip >= kAnimJumpClips) layer.time = TransitionClipTime(layer, deltaSeconds);
			else layer.time = layer.clip == kJumpLand && !land.empty() ? std::max(layer.time, LandTimeFromDistance(groundDistance)) : layer.time + deltaSeconds;
			if (layer.fadeSeconds > 0.f) layer.fade = std::min(layer.fade + deltaSeconds / layer.fadeSeconds, 1.f);
		}
		// 다 덮인 아래 상태는 버린다. 걷기 · 서기만 남으면 점프 쪽을 끈다
		for (int i = _count - 1; i > 0; --i)
		{
			if (_stack[i].fade < 1.f) continue;
			for (int j = i; j < _count; ++j) _stack[j - i] = _stack[j];
			_count -= i;
			break;
		}
		if (_count == 1 && _stack[0].clip == kJumpNone) _count = 0;

		if (_recoveryTime >= 0.f)
		{
			_recoveryTime += deltaSeconds;
			if (_recoveryTime >= _recoveryLength) _recoveryTime = -1.f;
		}
	}

	void CAnimState::Update(const AnimMoveInput& input)
	{
		const float deltaSeconds = input.deltaSeconds;
		const double* velocity = input.velocity;
		const float speed = (float)std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);

		float targets[kAnimWalkClips] = {};
		const float moveAlpha = Clamp(speed / kMoveFullSpeed, 0.f, 1.f);
		targets[kClipIdle] = 1.f - moveAlpha;
		if (speed > 1.f)
		{
			// 0 = 앞, +90 = 오른쪽, ±180 = 뒤, -90 = 왼쪽 — 이웃한 두 방향 클립을 각도로 나눈다
			const float angle = AngleOf(velocity);
			const float value = std::abs(angle);
			const int side = angle >= 0.f ? kClipRight : kClipLeft;
			if (value <= 90.f)
			{
				targets[kClipFwd] = (1.f - value / 90.f) * moveAlpha;
				targets[side] = (value / 90.f) * moveAlpha;
			}
			else
			{
				targets[side] = (1.f - (value - 90.f) / 90.f) * moveAlpha;
				targets[kClipBwd] = ((value - 90.f) / 90.f) * moveAlpha;
			}
		}

		const float follow = 1.f - std::exp(-deltaSeconds / kBlendSeconds);
		float sum = 0.f;
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			_weights[i] += (targets[i] - _weights[i]) * follow;
			sum += _weights[i];
		}
		if (sum > kSmallNumber)
		{
			for (float& weight : _weights) weight /= sum;
		}

		// 달리기 진행 — 클립의 원래 속도와 실제 속도의 비로 재생하고, 발걸음 단위로 함께 넘어가 섞이는 두 클립의 발이 같은 때 닿게 한다
		float directionWeight = 0.f;
		float rootSpeed = 0.f;
		float stepSeconds = 0.f;
		for (int i = kClipFwd; i <= kClipRight; ++i)
		{
			float start, end;
			StepSpan(i, _step, start, end);
			directionWeight += _weights[i];
			rootSpeed += _weights[i] * _rootSpeeds[i];
			stepSeconds += _weights[i] * (end - start);
		}
		const bool stepsFollow = FollowStepsOfTransition(deltaSeconds);
		if (!stepsFollow && directionWeight > 0.001f && stepSeconds > kSmallNumber)
		{
			rootSpeed /= directionWeight;
			stepSeconds /= directionWeight;
			const float playRate = rootSpeed > 1.f ? Clamp(speed / rootSpeed, kMinPlayRate, kMaxPlayRate) : 1.f;
			_stepFrac += deltaSeconds * playRate / stepSeconds;
			while (_stepFrac >= 1.f)
			{
				_stepFrac -= 1.f;
				_step = (_step + 1) % _stepCycle;
			}
		}
		_idleTime = _lengths[kClipIdle] > 0.f ? std::fmod(_idleTime + deltaSeconds, _lengths[kClipIdle]) : 0.f;

		_times[kClipIdle] = _idleTime;
		_timesStep = _step;
		_timesStepFrac = _stepFrac;
		_timesIdle = _idleTime;
		for (int i = kClipFwd; i <= kClipRight; ++i)
		{
			float start, end;
			StepSpan(i, _step, start, end);
			_times[i] = std::fmod(start + _stepFrac * (end - start), _lengths[i]);
		}

		_pitch = input.pitch;
		const bool falling = input.falling;
		const bool accelerating = input.accelerating;
		_aimBlend = InterpTo(_aimBlend, accelerating ? 0.f : 1.f, deltaSeconds, kAimBlendSpeed);
		const float timeToApex = velocity[2] > 0.0 && _gravity > kSmallNumber ? (float)velocity[2] / _gravity : 0.f;
		const float groundDistance = input.groundDistance;

		// 발 디딤 내림 — 땅에선 잰 거리만큼, 뜬 직후엔 마지막으로 잰 거리만큼 내린다
		if (falling)
		{
			_airTime = _wasFalling ? _airTime + deltaSeconds : 0.f;
			_groundTime = 0.f;
		}
		else
		{
			_groundTime = _wasFalling ? 0.f : _groundTime + deltaSeconds;
			if (input.hasFloorGap) _plantDrop = input.plantGap;
		}
		const bool plant = falling ? _airTime < kPlantReleaseSeconds - kPlantTimeSlack : _groundTime >= kPlantLandDelay - kPlantTimeSlack;
		_plantOffset[0] = 0.f;
		_plantOffset[1] = 0.f;
		_plantOffset[2] = plant ? -_plantDrop : 0.f;

		UpdateTransitions(input, deltaSeconds);
		UpdateJump(falling, accelerating, (float)velocity[2], timeToApex, groundDistance, deltaSeconds);
		UpdateIdleBreak(!falling && !accelerating && speed < 1.f && !HasJumpLayer(), deltaSeconds);
	}

	void CAnimState::ToWire(wire::AnimStateWire& out) const
	{
		for (int i = 0; i < kAnimWalkClips; ++i) out.weights[i] = ToWireByte(_weights[i]);
		out.step = (uint8_t)_timesStep;
		out.stepFrac = (uint16_t)std::lround(Clamp(_timesStepFrac, 0.f, 1.f) * 65535.f);
		out.idleTime = ToWireTime(_timesIdle);
		out.aimBlend = ToWireByte(_aimBlend);
		out.fidgetTime = _fidgetTime < 0.f ? wire::kAnimTimeNone : ToWireTime(_fidgetTime);
		out.recoveryTime = _recoveryTime < 0.f ? wire::kAnimTimeNone : ToWireTime(_recoveryTime);
		out.plantDrop = (int16_t)std::lround(Clamp(-_plantOffset[2], -300.f, 300.f) * kWirePlantScale);
		out.layerCount = (uint8_t)_count;
		for (int i = 0; i < _count; ++i)
		{
			out.layers[i].clip = (int8_t)_stack[i].clip;
			out.layers[i].time = ToWireTime(_stack[i].time);
			out.layers[i].fade = ToWireByte(_stack[i].fade);
		}
	}

	void CAnimState::FromWire(const wire::AnimStateWire& in, float pitch, AnimPoseInput& out) const
	{
		float sum = 0.f;
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			out.weights[i] = (float)in.weights[i] / 255.f;
			sum += out.weights[i];
		}
		if (sum > kSmallNumber)
		{
			for (float& weight : out.weights) weight /= sum;
		}
		else
		{
			out.weights[kClipIdle] = 1.f;
		}

		out.times[kClipIdle] = FromWireTime(in.idleTime);
		const float frac = (float)in.stepFrac / 65535.f;
		for (int i = kClipFwd; i <= kClipRight; ++i)
		{
			float start, end;
			StepSpan(i, in.step, start, end);
			out.times[i] = std::fmod(start + frac * (end - start), _lengths[i]);
		}

		out.fidgetTime = in.fidgetTime == wire::kAnimTimeNone ? -1.f : FromWireTime(in.fidgetTime);
		out.recoveryTime = in.recoveryTime == wire::kAnimTimeNone ? -1.f : FromWireTime(in.recoveryTime);
		out.aimBlend = (float)in.aimBlend / 255.f;
		out.pitch = NormalizePitch(pitch);
		out.plantOffset[0] = 0.f;
		out.plantOffset[1] = 0.f;
		out.plantOffset[2] = -(float)in.plantDrop / kWirePlantScale;
		out.layerCount = std::min((int)in.layerCount, kAnimLayerMax);
		for (int i = 0; i < out.layerCount; ++i)
		{
			out.layers[i].clip = in.layers[i].clip;
			out.layers[i].time = FromWireTime(in.layers[i].time);
			out.layers[i].fade = (float)in.layers[i].fade / 255.f;
		}
	}

	void CAnimState::ToPoseInput(AnimPoseInput& out) const
	{
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			out.weights[i] = _weights[i];
			out.times[i] = _times[i];
		}
		out.fidgetTime = _fidgetTime;
		out.layerCount = _count;
		for (int i = 0; i < kAnimLayerMax; ++i)
		{
			out.layers[i].clip = (int16_t)_stack[i].clip;
			out.layers[i].time = _stack[i].time;
			out.layers[i].fade = _stack[i].fade;
		}
		out.recoveryTime = _recoveryTime;
		out.aimBlend = _aimBlend;
		out.pitch = _pitch;
		for (int c = 0; c < 3; ++c) out.plantOffset[c] = _plantOffset[c];
	}
}
