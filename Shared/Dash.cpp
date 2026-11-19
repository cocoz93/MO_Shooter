#include "Dash.h"

#include <cmath>

namespace mo
{
	namespace
	{
		constexpr double kPi = 3.141592653589793238462643383279502884;
		constexpr double kDashUpScale = 1.2;		// Get Direction 곱하기 노드 (1, 1, 1.2) — 그래프 주석 「Help with getting higher off ground」

		// FVector::GetSafeNormal(기본 허용 1e-8) — Movement.cpp 의 같은 이름 함수와 같은 식
		Vec3 SafeNormal(const Vec3& a)
		{
			const double sum = a.x * a.x + a.y * a.y + a.z * a.z;
			if (sum == 1.0) return a;
			if (sum < 1e-8) return Vec3();
			return a * (1.0 / std::sqrt(sum));
		}

		// FRotator::Vector — 카메라 앞. 엔진의 카메라 앞은 부품 회전(쿼터니언)에서 꺼내 끝자리가 다를 수 있다(1e-16 수준, 힘에선 1e-13cm/s)
		Vec3 RotatorVector(double pitchDegrees, double yawDegrees)
		{
			const double pitch = std::fmod(pitchDegrees, 360.0) * (kPi / 180.0);
			const double yaw = std::fmod(yawDegrees, 360.0) * (kPi / 180.0);
			const double cp = std::cos(pitch);
			return Vec3(cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch));
		}

		// Conv_VectorToRotator 의 요 — FMath::Atan2(double) 는 (0, 0) 에 0(GenericPlatformMath.cpp:80-85), 도 = 라디안 × (180 / π) double(UnrealMathUtility.h:870)
		double YawOf(const Vec3& v)
		{
			if (v.x == 0.0 && v.y == 0.0) return 0.0;
			return std::atan2(v.y, v.x) * (180.0 / kPi);
		}

		// FRotator::NormalizeAxis — (−180, 180] (Rotator.h:689-716). 엔진 NormalizedDeltaRotator 의 Normalize 는 PC 에서 벡터 명령 길
		// (UnrealMathVectorCommon.h.inl:642-654, 나머지 = X − 360 × 버림(X ÷ 360))인데, 여기 들어오는 값(두 요의 차, ±360 안)에서는 버림이 0 이라 fmod 길과 같다
		double NormalizeAxis(double angle)
		{
			angle = std::fmod(angle, 360.0);
			if (angle < 0.0) angle += 360.0;
			if (angle > 180.0) angle -= 360.0;
			return angle;
		}

		// FVector::IsNearlyZero — 성분마다 절댓값 ≤ 허용(Vector.h:1750-1756). 그래프 Vector_IsNearlyZero 허용 = 0.0001
		bool IsNearlyZero(const Vec3& v, double tolerance)
		{
			return std::fabs(v.x) <= tolerance && std::fabs(v.y) <= tolerance && std::fabs(v.z) <= tolerance;
		}
		constexpr double kDashZeroTolerance = 0.0001;
	}

	RootForceSettings DashForceSettings()
	{
		RootForceSettings settings;
		settings.ignoreZ = false;
		settings.finish = ERootForceFinish::ClampVelocity;
		settings.clamp = kDashClampOnFinish;
		return settings;
	}

	Vec3 DashDirection(EDashDir dir, const Vec3& lastInput, float viewPitch, float viewYaw)
	{
		if (dir != EDashDir::Forward) return lastInput;
		const Vec3 forward = RotatorVector(viewPitch, viewYaw);
		return Vec3(forward.x, forward.y, forward.z * kDashUpScale);
	}

	Vec3 DashForce(EDashDir dir, const Vec3& lastInput, float viewPitch, float viewYaw)
	{
		// ConstantForce->Force = WorldDirection(정규화된) × Strength(float)
		return SafeNormal(DashDirection(dir, lastInput, viewPitch, viewYaw)) * (double)kDashStrength;
	}

	EDashDir SelectDashDir(const Vec3& lastInput, float bodyYaw)
	{
		// NormalizedDeltaRotator(Conv_VectorToRotator(이동), Conv_VectorToRotator(GetActorForwardVector)).Yaw — 몸 앞은 벡터로 바꿨다 다시 각으로(엔진과 같은 길)
		const double d = NormalizeAxis(YawOf(lastInput) - YawOf(RotatorVector(0.0, bodyYaw)));
		const double absD = std::fabs(d);
		if (absD < 45.0) return EDashDir::Forward;
		if (absD > 135.0) return EDashDir::Backward;
		return d < 0.0 ? EDashDir::Left : EDashDir::Right;
	}

	void CDashAbility::NextInput()
	{
		if (_activeLeft > 0) --_activeLeft;
		if (_cooldownLeft > 0) --_cooldownLeft;
		_justStarted = false;
	}

	DashStart CDashAbility::TryActivate(float viewPitch, float viewYaw, bool dead, bool postGame)
	{
		const Vec3& prevMove = _prevMove;
		DashStart out;
		if (postGame) { out.result = EDashResult::PostGame; return out; }
		if (dead) { out.result = EDashResult::Dead; return out; }
		if (_activeLeft > 0) { out.result = EDashResult::WhileActive; return out; }
		if (_cooldownLeft > 0) { out.result = EDashResult::OnCooldown; return out; }

		// 그래프 차례 — 몽타주 고르기 → 방향 → 0 이면 취소(쿨다운 전) → CommitAbility(쿨다운) → 힘
		out.dir = SelectDashDir(prevMove, _prevYaw);
		if (IsNearlyZero(DashDirection(out.dir, prevMove, viewPitch, viewYaw), kDashZeroTolerance))
		{
			out.result = EDashResult::NoDirection;
			return out;
		}
		out.result = EDashResult::Started;
		out.force = DashForce(out.dir, prevMove, viewPitch, viewYaw);
		_activeLeft = kDashActiveInputs;
		_cooldownLeft = kDashCooldownInputs;
		_justStarted = true;
		return out;
	}

	void CDashAbility::Reset()
	{
		_activeLeft = 0;
		_cooldownLeft = 0;
		_justStarted = false;
		_prevMove = Vec3();
		_prevYaw = 0.f;
	}
}
