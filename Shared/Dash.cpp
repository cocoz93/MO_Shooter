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
	}

	RootForceSettings DashForceSettings()
	{
		RootForceSettings settings;
		settings.ignoreZ = false;
		settings.finish = ERootForceFinish::ClampVelocity;
		settings.clamp = kDashClampOnFinish;
		return settings;
	}

	Vec3 DashForce(EDashDir dir, const Vec3& lastInput, float viewPitch, float viewYaw)
	{
		Vec3 direction = lastInput;
		if (dir == EDashDir::Forward)
		{
			const Vec3 forward = RotatorVector(viewPitch, viewYaw);
			direction = Vec3(forward.x, forward.y, forward.z * kDashUpScale);
		}
		// ConstantForce->Force = WorldDirection(정규화된) × Strength(float)
		return SafeNormal(direction) * (double)kDashStrength;
	}
}
