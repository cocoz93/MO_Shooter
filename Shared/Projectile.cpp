#include "Projectile.h"

#include <algorithm>
#include <cmath>

namespace mo
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kDegToRad = kPi / 180.0;
		constexpr double kRadToDeg = 180.0 / kPi;
		constexpr float kMinTickTime = 1e-6f;				// UProjectileMovementComponent::MIN_TICK_TIME
		constexpr double kKindaSmall = 1e-4;				// UE_KINDA_SMALL_NUMBER
		// 회전 따름의 「속도가 거의 0」 문턱 — 복제 속도 반올림 기본(정수)이면 1.0(ProjectileMovementComponent.cpp:188-193, ReplicatedState.cpp:35)
		constexpr double kVelocityTolerance = 1.0;

		double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		double SizeSquared(const Vec3& v) { return Dot(v, v); }

		bool NearlyZero(const Vec3& v, double tolerance)
		{
			return std::fabs(v.x) <= tolerance && std::fabs(v.y) <= tolerance && std::fabs(v.z) <= tolerance;
		}

		Vec3 SafeNormal(const Vec3& v)
		{
			const double sq = SizeSquared(v);
			if (sq < 1e-8) return Vec3();
			return v * (1.0 / std::sqrt(sq));
		}

		// LimitVelocity — 최대 속도로 자름(GetClampedToMaxSize). 평면 묶기는 꺼져 있다
		Vec3 LimitVelocity(const ProjectileParams& p, const Vec3& v)
		{
			const double maxSpeed = p.maxSpeed;
			if (maxSpeed <= 0) return v;
			const double sq = SizeSquared(v);
			if (sq > maxSpeed * maxSpeed)
			{
				const double scale = maxSpeed / std::sqrt(sq);
				return v * scale;
			}
			return v;
		}

		// ComputeVelocity — v = v0 + a·t, 가속 = 중력뿐(밀어 주는 힘 · 유도 없음). 시간은 엔진처럼 float 로 받는다
		Vec3 ComputeVelocity(const ProjectileParams& p, const Vec3& v, float seconds)
		{
			const Vec3 accel(0.0, 0.0, (double)p.gravityZ);
			return LimitVelocity(p, v + accel * (double)seconds);
		}

		// ComputeMoveDelta — p = v0·t + ½(v1 − v0)·t (ProjectileMovementComponent.cpp:480-493). ½·t 는 float 곱이다
		Vec3 ComputeMoveDelta(const ProjectileParams& p, const Vec3& v, float seconds)
		{
			const Vec3 next = ComputeVelocity(p, v, seconds);
			return v * (double)seconds + (next - v) * (double)(0.5f * seconds);
		}

		// GetSimulationTimeStep — 중력이 있으면 쪼개기 갈래를 탄다(ShouldUseSubStepping). 1/60 초 프레임은 최대 걸음보다 짧아 한 걸음이다
		float SimulationTimeStep(const ProjectileParams& p, float remaining, int iterations)
		{
			if (remaining > p.maxStepSeconds && iterations < p.maxIterations)
				remaining = std::min(p.maxStepSeconds, remaining * 0.5f);
			return std::max(kMinTickTime, remaining);
		}

		// 선분 둘의 가장 가까운 점(Ericson 「Real-Time Collision Detection」 5.1.9)
		double SegmentSegmentClosest(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1, Vec3& c2)
		{
			const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
			const double a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
			double s = 0, t = 0;
			constexpr double kEps = 1e-12;
			if (a <= kEps && e <= kEps)
			{
				c1 = p1;
				c2 = p2;
				return std::sqrt(SizeSquared(c1 - c2));
			}
			if (a <= kEps)
			{
				t = std::clamp(f / e, 0.0, 1.0);
			}
			else
			{
				const double c = Dot(d1, r);
				if (e <= kEps)
				{
					s = std::clamp(-c / a, 0.0, 1.0);
				}
				else
				{
					const double b = Dot(d1, d2);
					const double denom = a * e - b * b;
					s = denom != 0 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
					t = (b * s + f) / e;
					if (t < 0)
					{
						t = 0;
						s = std::clamp(-c / a, 0.0, 1.0);
					}
					else if (t > 1)
					{
						t = 1;
						s = std::clamp((b - c) / a, 0.0, 1.0);
					}
				}
			}
			c1 = p1 + d1 * s;
			c2 = p2 + d2 * t;
			return std::sqrt(SizeSquared(c1 - c2));
		}

		struct Segment
		{
			Vec3 a, b;
		};

		Segment CapsuleSegment(const Vec3& center, const Vec3& axis, double radius, double halfHeight)
		{
			const Vec3 half = axis * std::max(0.0, halfHeight - radius);
			return { center - half, center + half };
		}

		// 기운 캡슐 쓸기 하나 — 맵(수류탄 깃발)과 막는 것들 중 가장 이른 맞음
		bool SweepFirst(const CMapCollision& map, const ProjectileParams& p, const ProjectileState& s, const Vec3& delta,
			const ProjectileObstacle* obstacles, size_t obstacleCount, ProjectileHit& out)
		{
			const Vec3 center = s.location + RotateVector(s.rotation, p.capsuleOffset);
			const Vec3 axis = RotateVector(s.rotation, Vec3(0.0, 0.0, 1.0));
			const double length = std::sqrt(SizeSquared(delta));
			bool any = false;

			CapsuleHit mapHit;
			if (map.SweepCapsule(center, center + delta, axis, p.capsuleRadius, p.capsuleHalfHeight, MAP_BLOCK_GRENADE, mapHit))
			{
				out.kind = EProjectileHitKind::Map;
				out.id = mapHit.object;
				out.time = mapHit.time;
				out.contactDistance = mapHit.distance;
				out.normal = mapHit.normal;
				out.impactPoint = mapHit.impactPoint;
				out.startPenetrating = mapHit.startPenetrating;
				any = true;
			}
			for (size_t i = 0; i < obstacleCount; ++i)
			{
				const ProjectileObstacle& o = obstacles[i];
				if (o.owner && !s.launchCleanedUp) continue;	// 던진 사람과는 타이머 전까지 서로 무시(IgnoreActorWhenMoving)
				double t;
				Vec3 normal, impact;
				bool startPen;
				if (!SweepCapsuleCapsule(center, axis, p.capsuleRadius, p.capsuleHalfHeight, delta, o.center, o.axis, o.radius, o.halfHeight, t, normal, impact, startPen))
					continue;
				if (any && t >= out.time) continue;
				out.kind = o.kind;
				out.id = o.id;
				out.time = t;
				out.contactDistance = t * length;
				out.normal = normal;
				out.impactPoint = impact;
				out.startPenetrating = startPen;
				any = true;
			}
			return any;
		}
	}

	Vec3 RotateVector(const Rotator& r, const Vec3& v)
	{
		const double sp = std::sin(r.pitch * kDegToRad), cp = std::cos(r.pitch * kDegToRad);
		const double sy = std::sin(r.yaw * kDegToRad), cy = std::cos(r.yaw * kDegToRad);
		const double sr = std::sin(r.roll * kDegToRad), cr = std::cos(r.roll * kDegToRad);
		const Vec3 axisX(cp * cy, cp * sy, sp);
		const Vec3 axisY(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp);
		const Vec3 axisZ(-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp);
		return axisX * v.x + axisY * v.y + axisZ * v.z;
	}

	Rotator RotatorFromDirection(const Vec3& d)
	{
		Rotator r;
		r.yaw = std::atan2(d.y, d.x) * kRadToDeg;
		r.pitch = std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)) * kRadToDeg;
		r.roll = 0;
		return r;
	}

	Vec3 LaunchVelocity(const Rotator& spawnRotation, const ProjectileParams& p)
	{
		// InitializeComponent — 처음 속도가 크기를 덮고(:89-93), 몸 축 기준 값을 세계로 돌린다(:95-98)
		return RotateVector(spawnRotation, SafeNormal(p.launchDirection) * (double)p.initialSpeed);
	}

	size_t TickProjectile(const CMapCollision& map, const ProjectileParams& p, ProjectileState& s, float deltaSeconds,
		const ProjectileObstacle* obstacles, size_t obstacleCount, ProjectileHit* hits, size_t maxHits)
	{
		size_t hitCount = 0;
		float remaining = deltaSeconds;
		int iterations = 0;
		// TickComponent 의 걸음 고리(ProjectileMovementComponent.cpp:196-330)
		while (!s.stopped && remaining >= kMinTickTime && iterations < p.maxIterations)
		{
			++iterations;
			const float timeTick = SimulationTimeStep(p, remaining, iterations);
			remaining -= timeTick;

			const Vec3 oldVelocity = s.velocity;
			const Vec3 delta = ComputeMoveDelta(p, oldVelocity, timeTick);
			// 회전은 옛 속도를 따른다(:225). 쓸기는 옮기기 전 회전으로 하고 새 회전은 끝에서 붙인다
			const Rotator newRotation = (s.launchCleanedUp && !NearlyZero(oldVelocity, kVelocityTolerance)) ? RotatorFromDirection(oldVelocity) : s.rotation;

			ProjectileHit hit;
			const bool blocked = SizeSquared(delta) > 0 && SweepFirst(map, p, s, delta, obstacles, obstacleCount, hit);
			if (!blocked)
			{
				s.location = s.location + delta;
				s.rotation = newRotation;
				s.velocity = ComputeVelocity(p, s.velocity, timeTick);
				continue;
			}

			// PullBackHit — 면에 딱 붙지 않게 시간을 조금 물린다. 엔진은 float 로 잰다(PrimitiveComponent.cpp:3095-3099)
			const float dist = (float)std::sqrt(SizeSquared(delta));
			const float timeBack = std::clamp(0.1f, 0.1f / dist, 1.f / dist) + 0.001f;
			hit.time = (double)std::clamp((float)hit.time - timeBack, 0.f, 1.f);
			s.location = s.location + delta * hit.time;
			s.rotation = newRotation;
			s.velocity = (hit.time > kKindaSmall) ? ComputeVelocity(p, oldVelocity, timeTick * (float)hit.time) : oldVelocity;
			if (hitCount < maxHits) hits[hitCount] = hit;
			++hitCount;
			break;	// 튕김 · 미끄러짐 · 멈춤은 G-1-c — 지금은 이 프레임 나머지 걸음을 버린다
		}

		// B_Grenade 의 PostLaunchCleanup 타이머(0.16초) — 나이 10 걸음 뒤에 켜진 것으로 두면 나이 11 걸음부터 속도를 따른다. 엔진 차례가 아니라 기록으로 맞춘 규칙(G-0-a · G-1-b 545발 회전 대조)
		if (!s.launchCleanedUp && s.frames * (double)deltaSeconds >= p.launchCleanupSeconds) s.launchCleanedUp = true;
		++s.frames;
		return hitCount;
	}

	bool SweepCapsuleCapsule(const Vec3& centerA, const Vec3& axisA, double radiusA, double halfHeightA, const Vec3& delta,
		const Vec3& centerB, const Vec3& axisB, double radiusB, double halfHeightB, double& outTime, Vec3& outNormal, Vec3& outImpact, bool& outStartPenetrating)
	{
		const Segment a = CapsuleSegment(centerA, axisA, radiusA, halfHeightA);
		const Segment b = CapsuleSegment(centerB, axisB, radiusB, halfHeightB);
		const double reach = radiusA + radiusB;
		Vec3 ca, cb;
		const auto distanceAt = [&](double t, Vec3& pa, Vec3& pb) {
			const Vec3 move = delta * t;
			return SegmentSegmentClosest(a.a + move, a.b + move, b.a, b.b, pa, pb);
		};

		const auto finish = [&](double t, const Vec3& pa, const Vec3& pb, bool startPen) {
			outTime = t;
			outNormal = SafeNormal(pa - pb);	// 맞은 쪽(b) → 쓸린 캡슐(a) 축
			outImpact = pb + outNormal * radiusB;
			outStartPenetrating = startPen;
			return true;
		};

		const double d0 = distanceAt(0.0, ca, cb);
		if (d0 <= reach) return finish(0.0, ca, cb, true);
		const double length = std::sqrt(SizeSquared(delta));
		if (d0 - length > reach) return false;				// 끝까지 가도 못 닿는다

		// 거리가 t 에 대해 볼록 — 가장 가까운 때를 황금 분할로 찾고, 그 앞에서 처음 닿는 때를 이분법으로
		double lo = 0.0, hi = 1.0;
		constexpr double kGolden = 0.6180339887498949;
		for (int i = 0; i < 80; ++i)
		{
			const double m1 = hi - (hi - lo) * kGolden, m2 = lo + (hi - lo) * kGolden;
			Vec3 p1, p2, p3, p4;
			if (distanceAt(m1, p1, p2) < distanceAt(m2, p3, p4)) hi = m2;
			else lo = m1;
		}
		const double tMin = 0.5 * (lo + hi);
		if (distanceAt(tMin, ca, cb) > reach) return false;
		lo = 0.0;
		hi = tMin;
		for (int i = 0; i < 64; ++i)
		{
			const double mid = 0.5 * (lo + hi);
			if (distanceAt(mid, ca, cb) > reach) lo = mid;
			else hi = mid;
		}
		distanceAt(hi, ca, cb);
		return finish(hi, ca, cb, false);
	}
}
