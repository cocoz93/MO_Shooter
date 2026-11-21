#include "Grenade.h"
#include "AnimClips.h"
#include "AnimPose.h"

#include <algorithm>
#include <cmath>

namespace mo
{
	namespace
	{
		constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

		// Curve_GrenadeDamage 에디터 평가 값 — 등급 0.100, 0.101, …, 1.000
		constexpr float kGrenadeCurveTable[] =
		{
#include "GrenadeDamageCurve.inl"
		};
		constexpr size_t kGrenadeCurveCount = sizeof(kGrenadeCurveTable) / sizeof(kGrenadeCurveTable[0]);
		constexpr double kGrenadeCurveStep = 0.001;
		static_assert(kGrenadeCurveCount == 901, "Curve table must hold 0.1..1.0 at 0.001 spacing");
	}

	void CGrenadeAbility::NextInput()
	{
		if (_cooldownLeft > 0) --_cooldownLeft;
		_justThrown = false;
	}

	EGrenadeResult CGrenadeAbility::TryActivate(bool dead, bool dashing)
	{
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
		// 겨눔 선 깃발 — 단순 모양 + 무기 채널(맵 파일 판 4, L-7 · 헤더 주석)
		if (map.Raycast(start, end, MAP_BLOCK_AIM, rh))
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

	double GrenadeDamageCurve(double grade)
	{
		const double x = std::min(std::max(grade, kGrenadeBlastMinGrade), 1.0);
		const double pos = (x - kGrenadeBlastMinGrade) / kGrenadeCurveStep;	// 표 칸 번호(소수)
		const size_t i = static_cast<size_t>(pos);
		if (i + 1 >= kGrenadeCurveCount) return kGrenadeCurveTable[kGrenadeCurveCount - 1];
		const double a = pos - static_cast<double>(i);
		const double v0 = kGrenadeCurveTable[i];
		const double v1 = kGrenadeCurveTable[i + 1];
		return v0 + a * (v1 - v0);
	}

	GrenadeBlastHit TraceGrenadeBlast(const CMapCollision& map, const Vec3& at, const GrenadeBlastTarget& target)
	{
		GrenadeBlastHit out;
		const Vec3 seg = target.center - at;
		const double len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
		// 선 끝이 캡슐 가운데라 늘 맞는다(t ≤ 1). 안에서 터지면 t = 0 — Lyra 도 거리 0 에 캡슐(G-0-a foe_close 의 나)
		double t = 1.0;
		Vec3 normal;
		const double axisHalf = std::max(0.0, target.halfHeight - target.radius);
		if (!SegmentCapsuleZ(at - target.center, seg, axisHalf, target.radius, t, normal)) t = 1.0;
		out.bodyDistance = t * len;
		out.distance = out.bodyDistance;
		out.grade = std::min(std::max(out.bodyDistance / kGrenadeBlastRadius, kGrenadeBlastMinGrade), 1.0);
		if (out.bodyDistance >= kGrenadeBlastRadius) return out;	// 곡선(1) = 0 — 막혔든 아니든 피해 0
		RayHit rh;
		if (map.Raycast(at, target.center, MAP_BLOCK_BLAST, rh) && rh.distance < out.bodyDistance)
		{
			out.blocked = true;
			out.distance = rh.distance;
			return out;
		}
		out.damage = kGrenadeBlastDamage * GrenadeDamageCurve(out.grade);
		return out;
	}

	bool GrenadeDamageApplies(bool alive, bool self, bool sameTeam, bool immune)
	{
		return alive && !immune && (self || !sameTeam);
	}

	size_t BodyObstacles(const BodyPlacement& body, const ProjectileObstacle& base, ProjectileObstacle* out, size_t maxCount)
	{
		if (body.skip || !body.pose || !body.shapes) return 0;
		const double yaw = body.yawDegrees * kDegToRad;
		const double cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
		// 캐릭터 요 0 좌표 → 세계(TraceBody 의 toWorld 와 같다)
		const auto toWorld = [&](const double v[3]) { return Vec3(v[0] * cosYaw - v[1] * sinYaw, v[0] * sinYaw + v[1] * cosYaw, v[2]); };
		size_t written = 0;
		for (uint32_t i = 0; i < body.shapeCount && written < maxCount; ++i)
		{
			const ClipBody& shape = body.shapes[i];
			const BodyPose& pose = body.pose[i];
			ProjectileObstacle o = base;
			o.part = (int32_t)i;
			o.center = body.center + toWorld(pose.center);
			switch ((EBodyShape)shape.kind)
			{
			case EBodyShape::Sphere:
				o.shape = EObstacleShape::Capsule;
				o.axis = toWorld(pose.axisZ);
				o.radius = shape.a;
				o.halfHeight = shape.a;
				break;
			case EBodyShape::Sphyl:
				o.shape = EObstacleShape::Capsule;
				o.axis = toWorld(pose.axisZ);
				o.radius = shape.a;
				o.halfHeight = shape.b * 0.5 + shape.a;
				break;
			case EBodyShape::Box:
				o.shape = EObstacleShape::Box;
				o.boxX = toWorld(pose.axisX);
				o.boxY = toWorld(pose.axisY);
				o.boxZ = toWorld(pose.axisZ);
				o.halfExtent = Vec3(shape.a * 0.5, shape.b * 0.5, shape.c * 0.5);
				break;
			}
			out[written++] = o;
		}
		return written;
	}
}
