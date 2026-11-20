#include "Grenade.h"

#include <cmath>

namespace mo
{
	namespace
	{
		constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
	}

	void CGrenadeAbility::NextInput()
	{
		if (_cooldownLeft > 0) --_cooldownLeft;
		_justThrown = false;
	}

	EGrenadeResult CGrenadeAbility::TryActivate(bool dead, bool dashing, bool postGame)
	{
		if (postGame) return EGrenadeResult::PostGame;
		if (dead) return EGrenadeResult::Dead;
		if (dashing) return EGrenadeResult::WhileDash;
		if (_cooldownLeft > 0) return EGrenadeResult::OnCooldown;
		// 그래프 차례 — 비용 · 쿨다운 검사 → CommitAbility(쿨다운 시작) → 놓을 자리 · 방향 → (서버만) 생성 → 몽타주 → 쿨다운 메시지 → 바로 능력 끝
		_cooldownLeft = kGrenadeCooldownInputs;
		_justThrown = true;
		return EGrenadeResult::Thrown;
	}

	void CGrenadeAbility::StartFromServer()
	{
		_cooldownLeft = kGrenadeCooldownInputs;
		_justThrown = true;
	}

	void CGrenadeAbility::Reset()
	{
		_cooldownLeft = 0;
		_justThrown = false;
	}

	Vec3 GrenadeSpawnLocation(const Vec3& bodyCenter, double bodyYawDegrees)
	{
		// K2_GetActorLocation + GetActorForwardVector × 5 + GetActorUpVector × 25 — 사람 액터 회전은 요만(위 = +Z)
		const double yaw = bodyYawDegrees * kDegToRad;
		const Vec3 forward(std::cos(yaw), std::sin(yaw), 0.0);
		return bodyCenter + forward * kGrenadeSpawnForward + Vec3(0.0, 0.0, kGrenadeSpawnUp);
	}

	void GrenadeTraceSegment(const Vec3& camera, const Rotator& cameraRotation, Vec3& outStart, Vec3& outEnd)
	{
		const Vec3 dir = RotateVector(cameraRotation, Vec3(1.0, 0.0, 0.0));	// Conv_RotatorToVector = 회전 행렬의 X 축
		outStart = camera + dir * kGrenadeTraceStart;
		outEnd = camera + dir * kGrenadeTraceRange;
	}

	GrenadeAimHit TraceGrenadeAim(const CMapCollision& map, const Vec3& start, const Vec3& end,
		const BodyPlacement* bodies, size_t bodyCount, uint32_t ignoreId)
	{
		GrenadeAimHit out;
		const Vec3 seg = end - start;
		const double len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
		RayHit rh;
		// 플랜 그대로 무기 채널 깃발 — 단순 모양 깃발이 생기면 그쪽으로 바꾼다(미결 21, 헤더 주석)
		if (map.Raycast(start, end, MAP_BLOCK_WEAPON, rh))
		{
			out.hit = true;
			out.distance = rh.distance;
			out.location = len > 0.0 ? start + seg * (rh.distance / len) : start;
		}
		for (size_t i = 0; i < bodyCount; ++i)
		{
			const BodyPlacement& b = bodies[i];
			if (b.skip || b.id == ignoreId) continue;
			BodyHit bh;
			if (!TraceBody(b, start, end, 0.0, bh)) continue;
			const double d = bh.time * len;
			if (out.hit && d >= out.distance) continue;
			out.hit = true;
			out.distance = d;
			out.location = bh.point;
			out.body = true;
			out.bodyId = b.id;
			out.shape = bh.shape;
		}
		return out;
	}

	GrenadeThrow ComputeGrenadeThrow(const Vec3& spawnLocation, const Rotator& controlRotation, const Vec3& traceEnd,
		const GrenadeAimHit& aim, const ProjectileParams& params)
	{
		GrenadeThrow out;
		out.location = spawnLocation;
		// Less_DoubleDouble(BreakHitResult.Distance, 45) → 참이면 GetControlRotation,
		// 거짓이면 Select(LineTraceSingle 반환값) — 맞으면 FindLookAtRotation(놓을 자리, 맞은 곳) · 안 맞으면 (놓을 자리, 선 끝)
		if (aim.distance < kGrenadeAimMinDistance)
		{
			out.branch = EGrenadeAimBranch::Control;
			out.rotation = controlRotation;
		}
		else if (aim.hit)
		{
			out.branch = EGrenadeAimBranch::LookAtHit;
			out.rotation = RotatorFromDirection(aim.location - spawnLocation);	// FindLookAtRotation = MakeRotFromX(목표 − 시작)
		}
		else
		{
			out.branch = EGrenadeAimBranch::LookAtEnd;
			out.rotation = RotatorFromDirection(traceEnd - spawnLocation);
		}
		out.velocity = LaunchVelocity(out.rotation, params);
		return out;
	}
}
