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

		// 상자 지역 좌표의 선분 p + s·d(s ∈ [0,1]) 와 반 길이 e 상자(가운데 0 · 축 = 좌표축) 사이 가장 가까운 거리. 거리 제곱 = 축마다 면 밖으로 넘친 몫의 제곱 합이라
		// 선분이 면 평면을 지나는 s(축마다 둘) 사이 조각에서는 2차식 — 조각마다 꼭짓점을 바로 구해 어림이 없다. outSeg · outBox = 선분 위 · 상자 위 가장 가까운 점
		double SegmentBoxClosest(const Vec3& p, const Vec3& d, const Vec3& e, Vec3& outSeg, Vec3& outBox)
		{
			const double pc[3] = { p.x, p.y, p.z }, dc[3] = { d.x, d.y, d.z }, ec[3] = { e.x, e.y, e.z };
			double cuts[8];
			int count = 0;
			cuts[count++] = 0.0;
			cuts[count++] = 1.0;
			for (int i = 0; i < 3; ++i)
			{
				if (dc[i] == 0.0) continue;
				const double faces[2] = { ec[i], -ec[i] };
				for (const double face : faces)
				{
					const double s = (face - pc[i]) / dc[i];
					if (s > 0.0 && s < 1.0) cuts[count++] = s;
				}
			}
			std::sort(cuts, cuts + count);
			const auto squaredAt = [&](double s)
			{
				double sum = 0;
				for (int i = 0; i < 3; ++i)
				{
					const double v = pc[i] + dc[i] * s;
					const double over = v > ec[i] ? v - ec[i] : (v < -ec[i] ? v + ec[i] : 0.0);
					sum += over * over;
				}
				return sum;
			};
			double bestS = 0.0, best = squaredAt(0.0);
			for (int k = 0; k + 1 < count; ++k)
			{
				const double lo = cuts[k], hi = cuts[k + 1];
				if (hi <= lo) continue;
				const double mid = 0.5 * (lo + hi);
				double qa = 0, qb = 0;	// 이 조각의 거리 제곱 = qa·s² + qb·s + 상수(넘친 축만 든다)
				for (int i = 0; i < 3; ++i)
				{
					const double v = pc[i] + dc[i] * mid;
					if (v > ec[i]) { qa += dc[i] * dc[i]; qb += 2.0 * dc[i] * (pc[i] - ec[i]); }
					else if (v < -ec[i]) { qa += dc[i] * dc[i]; qb += 2.0 * dc[i] * (pc[i] + ec[i]); }
				}
				const double s = qa > 0.0 ? std::min(hi, std::max(lo, -qb / (2.0 * qa))) : lo;
				const double value = squaredAt(s);
				if (value < best)
				{
					best = value;
					bestS = s;
				}
			}
			outSeg = p + d * bestS;
			outBox = Vec3(std::min(e.x, std::max(-e.x, outSeg.x)), std::min(e.y, std::max(-e.y, outSeg.y)), std::min(e.z, std::max(-e.z, outSeg.z)));
			return std::sqrt(best);
		}

		Vec3 Cross(const Vec3& a, const Vec3& b)
		{
			return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
		}

		bool IsZero(const Vec3& v) { return v.x == 0 && v.y == 0 && v.z == 0; }

		double NormalizeAxis(double degrees)
		{
			double d = std::fmod(degrees, 360.0);
			if (d > 180.0) d -= 360.0;
			else if (d <= -180.0) d += 360.0;
			return d;
		}

		constexpr float kPenetrationPullback = 0.125f;		// p.PenetrationPullbackDistance(MovementComponent.cpp:593)
		constexpr double kNormalsParallel = 0.999845;		// UE_THRESH_NORMALS_ARE_PARALLEL — FVector::Coincident(Vector.h:1429-1433)
		constexpr float kMinMovementDistSq = (4.f * 1e-4f) * (4.f * 1e-4f);	// 쓸기의 가장 짧은 옮김(PrimitiveComponent.cpp:3283)
		constexpr double kSameTolerance = 1e-4;				// FVector · FRotator::Equals 기본 — 이 안 차이는 안 옮긴 것(SceneComponent.cpp:3315-3317)
		constexpr float kSlideDotTolerance = 0.01f;			// HandleDeflection DotTolerance(:344)

		bool SameLocation(const Vec3& a, const Vec3& b)
		{
			return std::fabs(a.x - b.x) <= kSameTolerance && std::fabs(a.y - b.y) <= kSameTolerance && std::fabs(a.z - b.z) <= kSameTolerance;
		}

		bool SameRotation(const Rotator& a, const Rotator& b)
		{
			return std::fabs(NormalizeAxis(a.pitch - b.pitch)) <= kSameTolerance && std::fabs(NormalizeAxis(a.yaw - b.yaw)) <= kSameTolerance
				&& std::fabs(NormalizeAxis(a.roll - b.roll)) <= kSameTolerance;
		}

		// 한 프레임 걸음의 일꾼 — 엔진 멤버 함수와 같은 이름 · 같은 차례로 둔다(ProjectileMovementComponent · MovementComponent · PrimitiveComponent)
		class CStep
		{
		public:
			CStep(const CMapCollision& map, const ProjectileParams& p, ProjectileState& s, const ProjectileObstacle* obstacles, size_t obstacleCount,
				ProjectileHit* hits, size_t maxHits)
				: _map(map), _p(p), _s(s), _obstacles(obstacles), _obstacleCount(obstacleCount), _hits(hits), _maxHits(maxHits)
			{
			}

			// TickComponent 의 걸음 고리(ProjectileMovementComponent.cpp:196-330)
			size_t Run(float deltaSeconds)
			{
				float remaining = deltaSeconds;
				int numImpacts = 0, iterations = 0;
				while (!Halted() && remaining >= kMinTickTime && iterations < _p.maxIterations)
				{
					++iterations;
					++_s.count.steps;
					const float timeTick = SimulationTimeStep(_p, remaining, iterations);
					remaining -= timeTick;

					const Vec3 oldVelocity = _s.velocity;
					const Vec3 delta = ComputeMoveDelta(_p, oldVelocity, timeTick);
					// 회전은 옛 속도를 따른다(:225). 쓸기는 옮기기 전 회전으로 하고 새 회전은 끝에서 붙인다
					const Rotator newRotation = (_s.launchCleanedUp && !NearlyZero(oldVelocity, kVelocityTolerance)) ? RotatorFromDirection(oldVelocity) : _s.rotation;

					Blocking hit;
					hit.Reset(_s.location, _s.location + delta);
					SafeMove(delta, newRotation, hit);
					if (Halted()) break;	// 맞음 알림으로 터짐(:250-253)

					if (!hit.blocking)
					{
						_s.previousHitTime = 1.f;
						_s.sliding = false;
						_s.velocity = ComputeVelocity(_p, _s.velocity, timeTick);
						continue;
					}

					// 맞은 때까지의 속도(:277) → 막힘 처리(:285-308) → 남은 시간을 고리에 돌려줌(:315-328)
					++_s.count.blockedSteps;
					_s.velocity = (hit.hit.time > kKindaSmall) ? ComputeVelocity(_p, oldVelocity, timeTick * (float)hit.hit.time) : oldVelocity;
					++numImpacts;
					float subTickTimeRemaining = timeTick * (1.f - (float)hit.hit.time);
					if (HandleBlockingHit(hit, timeTick, subTickTimeRemaining) == EBlockResult::Abort || Halted()) break;
					HandleDeflection(hit, oldVelocity, subTickTimeRemaining);	// 엔진도 돌려준 값을 안 본다 — 멈췄으면 고리 조건이 끝낸다
					_s.previousHitTime = (float)hit.hit.time;
					_s.previousHitNormal = hit.hit.normal;
					if (subTickTimeRemaining >= kMinTickTime)
					{
						remaining += subTickTimeRemaining;
						if (numImpacts <= _p.bounceExtraIterations)
						{
							--iterations;
							++_s.count.extraIterations;
						}
					}
				}
				return _hitCount;
			}

		private:
			// 쓸어 옮기기의 막는 맞음 — 엔진 FHitResult 처럼 고리 · 미끄러짐이 같은 칸을 덮어 쓴다
			struct Blocking
			{
				bool blocking = false;
				ProjectileHit hit;
				size_t index = SIZE_MAX;					// hits 안 자리(넘쳤으면 SIZE_MAX)
				const ProjectileObstacle* obstacle = nullptr;

				// FHitResult::Init — 시각 1 · 법선 0
				void Reset(const Vec3& start, const Vec3& end)
				{
					*this = Blocking();
					hit.time = 1.0;
					hit.traceStart = start;
					hit.traceEnd = end;
				}
			};

			enum class EBlockResult
			{
				Abort,
				Deflect,
			};

			bool Halted() const { return _s.stopped || _s.detonated; }
			bool UnderStopThreshold() const { return SizeSquared(_s.velocity) < (double)(_p.stopSpeed * _p.stopSpeed); }	// IsVelocityUnderSimulationThreshold

			// StopSimulating(:540-548)
			void Stop()
			{
				_s.velocity = Vec3();
				_s.stopped = true;
			}

			// 쓸기 — 맵(수류탄 깃발)과 막는 것들. 맞음 고르기는 MoveComponentImpl(PrimitiveComponent.cpp:3372-3396) — 시작 겹침이 있으면
			// 그중 맞은 면 법선이 옮김을 가장 거스르는 것, 없으면 가장 이른 맞음. 빠져나가는 겹침은 무시(ShouldIgnoreHitResult :3135-3163 —
			// 거리 · 내적 허용 두 값은 기본 0 이라 시작 겹침 · 내적 > 0 만 남는다 :125 · :134, Lyra 설정에 덮는 줄 없음)
			// 맵 시작 겹침은 물체마다 받아 하나씩 고른다(L-1-a — 가장 깊은 것 하나만 받으면 그것이 빠지는 겹침일 때 다른 물체 겹침을 놓쳤다).
			// 남은 한계: 시작 겹침이 하나라도 있으면 맵은 쓸기 맞음을 안 센다 — 겹침이 모두 빠지는 쪽이면 그 뒤 맵 맞음은 못 본다(이동 쪽 Movement.cpp 와 같은 한계)
			bool Sweep(const Vec3& delta, bool neverIgnoreOverlaps, Blocking& out)
			{
				const Vec3 center = _s.location + RotateVector(_s.rotation, _p.capsuleOffset);
				const Vec3 axis = RotateVector(_s.rotation, Vec3(0.0, 0.0, 1.0));
				const double length = std::sqrt(SizeSquared(delta));
				const Vec3 moveDir = SafeNormal(delta);
				bool havePenetrating = false, haveHit = false;
				double bestNormalDotDelta = 0;

				const auto consider = [&](const ProjectileHit& h, const ProjectileObstacle* o)
				{
					if (h.startPenetrating && !neverIgnoreOverlaps && Dot(h.impactNormal, moveDir) > 0.0)
					{
						++_s.count.ignoredOverlaps;
						return;
					}
					if (h.startPenetrating)
					{
						const double normalDotDelta = Dot(h.impactNormal, delta);
						if (!havePenetrating || normalDotDelta < bestNormalDotDelta)
						{
							bestNormalDotDelta = normalDotDelta;
							havePenetrating = true;
							out.hit = h;
							out.obstacle = o;
						}
					}
					else if (!havePenetrating && (!haveHit || h.time < out.hit.time))
					{
						haveHit = true;
						out.hit = h;
						out.obstacle = o;
					}
				};

				const auto considerMap = [&](const CapsuleHit& mapHit)
				{
					ProjectileHit h;
					h.kind = EProjectileHitKind::Map;
					h.id = mapHit.object;
					h.time = mapHit.time;
					h.contactDistance = mapHit.distance;
					h.normal = mapHit.normal;
					h.impactNormal = mapHit.impactNormal;
					h.impactPoint = mapHit.impactPoint;
					h.penetrationDepth = mapHit.penetrationDepth;
					h.startPenetrating = mapHit.startPenetrating;
					consider(h, nullptr);
				};
				CapsuleHit mapHit;
				CMapCollision::Penetrations mapPens;
				if (_map.SweepCapsule(center, center + delta, axis, _p.capsuleRadius, _p.capsuleHalfHeight, MAP_BLOCK_GRENADE, mapHit, &mapPens))
				{
					if (mapHit.startPenetrating && mapPens.count > 0)
					{
						for (size_t i = 0; i < mapPens.count; ++i) considerMap(mapPens.hits[i]);
						_s.count.mapPenetrationsDropped += (uint32_t)mapPens.dropped;
					}
					else considerMap(mapHit);
				}
				for (size_t i = 0; i < _obstacleCount; ++i)
				{
					const ProjectileObstacle& o = _obstacles[i];
					if (o.owner && !_s.launchCleanedUp) continue;	// 던진 사람과는 타이머 전까지 서로 무시(IgnoreActorWhenMoving)
					ProjectileHit h;
					const bool swept = o.shape == EObstacleShape::Box
						? SweepCapsuleBox(center, axis, _p.capsuleRadius, _p.capsuleHalfHeight, delta, o.center, o.boxX, o.boxY, o.boxZ, o.halfExtent,
							h.time, h.normal, h.impactPoint, h.startPenetrating, h.penetrationDepth)
						: SweepCapsuleCapsule(center, axis, _p.capsuleRadius, _p.capsuleHalfHeight, delta, o.center, o.axis, o.radius, o.halfHeight,
							h.time, h.normal, h.impactPoint, h.startPenetrating, h.penetrationDepth);
					if (!swept) continue;
					h.kind = o.kind;
					h.id = o.id;
					h.part = o.part;
					h.contactDistance = h.time * length;
					// 캡슐끼리는 면이 없다 — 받은 법선 그대로. 상자도 같이 둔다 — 면에 맞으면 면 법선과 같고, 모서리 · 꼭짓점 맞음은 Lyra 대조가 없다(L-8-a-1 기록 6 은 다 Sphyl)
					h.impactNormal = h.normal;
					consider(h, &o);
				}
				return havePenetrating || haveHit;
			}

			// UPrimitiveComponent::MoveComponentImpl 중 이 설정이 타는 갈래. 쓸기 모양의 회전 = 옮기기 전 회전(:3354 InitialRotationQuat)
			// 돌려주는 값 = 자리나 회전이 바뀌었나(:3493 · SceneComponent.cpp:3315-3317 — 1e-4 안 차이는 안 바뀐 것이라 그대로 둔다)
			// dispatchOverlaps = 시작 겹침 맞음도 알림을 내나(:3539)
			bool MoveComponent(const Vec3& delta, const Rotator& newRotation, bool sweep, bool neverIgnoreOverlaps, bool dispatchOverlaps, Blocking* out)
			{
				const Vec3 traceStart = _s.location, traceEnd = traceStart + delta;
				double deltaSizeSq = SizeSquared(delta);
				const float minMovementDistSq = sweep ? kMinMovementDistSq : 0.f;
				if (deltaSizeSq <= (double)minMovementDistSq)
				{
					if (SameRotation(newRotation, _s.rotation))
					{
						if (out) out->Reset(traceStart, traceEnd);
						return true;
					}
					deltaSizeSq = 0;
				}

				Blocking found;
				found.Reset(traceStart, traceEnd);
				Vec3 newLocation = traceStart;
				if (sweep && deltaSizeSq > 0)
				{
					if (Sweep(delta, neverIgnoreOverlaps, found))
					{
						found.blocking = true;
						// PullBackHit — 면에 딱 붙지 않게 시간을 조금 물린다. 엔진은 float 로 잰다(:3095-3099 · :3361)
						const float dist = (float)std::sqrt(deltaSizeSq);
						const float timeBack = std::clamp(0.1f, 0.1f / dist, 1.f / dist) + 0.001f;
						found.hit.time = (double)std::clamp((float)found.hit.time - timeBack, 0.f, 1.f);
						newLocation = traceStart + delta * found.hit.time;
						if ((float)SizeSquared(newLocation - traceStart) <= minMovementDistSq)	// 아주 짧은 옮김은 안 옮김(:3444-3456)
						{
							newLocation = traceStart;
							found.hit.time = 0;
							++_s.count.minMoveZeroed;
						}
					}
					else newLocation = traceEnd;
				}
				else if (deltaSizeSq > 0) newLocation = traceEnd;

				const bool diffLocation = !SameLocation(newLocation, _s.location);
				const bool diffRotation = !SameRotation(newRotation, _s.rotation);
				if (diffLocation || diffRotation)
				{
					_s.location = newLocation;
					if (diffRotation) _s.rotation = newRotation;
				}

				if (found.blocking)
				{
					found.hit.traceStart = traceStart;
					found.hit.traceEnd = traceEnd;
					found.hit.dispatched = !found.hit.startPenetrating || dispatchOverlaps;
					if (_hitCount < _maxHits)
					{
						found.index = _hitCount;
						_hits[_hitCount] = found.hit;
					}
					++_hitCount;
					// 맞음 알림(DispatchBlockingHit → ReceiveHit) — 다른 팀 사람이면 그 자리에서 터진다. 엔진은 미룸 없이 곧장 부른다(bSimulationUseScopedMovement 꺼짐 :29)
					if (found.hit.dispatched && found.obstacle && found.obstacle->detonateOnHit)
					{
						_s.detonated = true;
						++_s.count.detonations;
					}
				}
				if (out) *out = found;
				return diffLocation || diffRotation;
			}

			// UMovementComponent::SafeMoveUpdatedComponent(MovementComponent.cpp:558-588) — 첫 시도는 겹침을 무시하지 않고 그 알림도 안 낸다
			// (p.MoveIgnoreFirstBlockingOverlap 0 :548). 시작 겹침이면 풀어낸 뒤 원래 옮김을 다시 한다
			bool SafeMove(const Vec3& delta, const Rotator& newRotation, Blocking& out)
			{
				bool moved = MoveComponent(delta, newRotation, true, true, false, &out);
				if (out.blocking && out.hit.startPenetrating)
				{
					++_s.count.startPenetrating;
					const Vec3 adjustment = PenetrationAdjustment(out.hit);
					if (ResolvePenetration(adjustment, out.hit, newRotation))
						moved = MoveComponent(delta, newRotation, true, false, true, &out);
				}
				return moved;
			}

			// GetPenetrationAdjustment(:608-622) — 법선 × (겹친 깊이 + 0.125)
			Vec3 PenetrationAdjustment(const ProjectileHit& hit) const
			{
				if (!hit.startPenetrating) return Vec3();
				const float depth = hit.penetrationDepth > 0 ? (float)hit.penetrationDepth : 0.125f;
				return hit.normal * (double)(depth + kPenetrationPullback);
			}

			// ResolvePenetrationImpl(:624-708). 엔진은 먼저 풀린 자리에서 경계 상자(Bounds.BoxExtent + 0.1, 뿌리 가운데)가 겹치는지 보고
			// 안 겹치면 쓸지 않고 옮긴다. 그 상자는 메시 그림 경계라 우리 쪽에 없어 겹친 갈래(쓸어서 빠져나감)만 둔다 —
			// 빠져나가는 길에 다른 것이 없으면 두 갈래는 같은 자리에 선다(G-1-c 기록 15판의 풀기 842번이 모두 이 갈래로 자리 · 속도가 맞음)
			bool ResolvePenetration(const Vec3& adjustment, const ProjectileHit& hit, const Rotator& newRotation)
			{
				if (IsZero(adjustment)) return false;
				Blocking sweepOut;
				bool moved = MoveComponent(adjustment, newRotation, true, false, true, &sweepOut);
				if (!moved && sweepOut.blocking && sweepOut.hit.startPenetrating)
				{
					// 두 법선을 더해 여러 면에서 한 번에 빠져나간다(:670-680)
					const Vec3 secondMtd = PenetrationAdjustment(sweepOut.hit);
					const Vec3 combined = adjustment + secondMtd;
					if (!(secondMtd.x == adjustment.x && secondMtd.y == adjustment.y && secondMtd.z == adjustment.z) && !IsZero(combined))
					{
						++_s.count.resolveCombined;
						moved = MoveComponent(combined, newRotation, true, false, true, nullptr);
					}
				}
				if (!moved)
				{
					// 풀기 + 원래 옮김, 그래도 안 되면 원래 옮김만(:683-700)
					const Vec3 moveDelta = hit.traceEnd - hit.traceStart;
					if (!IsZero(moveDelta))
					{
						++_s.count.resolveWithMove;
						moved = MoveComponent(adjustment + moveDelta, newRotation, true, false, true, nullptr);
						if (!moved && Dot(moveDelta, adjustment) > 0.0)
							moved = MoveComponent(moveDelta, newRotation, true, false, true, nullptr);
					}
				}
				if (moved) ++_s.count.resolveMoved;
				else ++_s.count.resolveFailed;
				return moved;
			}

			// HandleBlockingHit(:551-574) — 세계 밖 검사(CheckStillInWorld)는 안 옮긴다(연습장 기록에 없음)
			EBlockResult HandleBlockingHit(Blocking& b, float timeTick, float& subTickTimeRemaining)
			{
				HandleImpact(b);
				if (Halted()) return EBlockResult::Abort;
				if (b.hit.startPenetrating) return EBlockResult::Abort;
				subTickTimeRemaining = timeTick * (1.f - (float)b.hit.time);
				return EBlockResult::Deflect;
			}

			// HandleImpact(:605-634) — 튕김 켬이라 튕긴 속도를 셈하고, 문턱 아래면 멈춤
			void HandleImpact(Blocking& b)
			{
				_s.velocity = ComputeBounceResult(b.hit);
				++_s.count.bounces;
				b.hit.bounced = true;
				if (b.index != SIZE_MAX) _hits[b.index].bounced = true;
				_s.velocity = LimitVelocity(_p, _s.velocity);
				if (UnderStopThreshold())
				{
					++_s.count.stopsOnImpact;
					Stop();
				}
			}

			// ComputeBounceResult(:576-603) — 면으로 들어가는 속도를 되돌리고, 면을 따르는 속도에만 마찰, 되돌린 몫에 반발. 엔진처럼 내적 · 마찰은 float
			Vec3 ComputeBounceResult(const ProjectileHit& hit) const
			{
				Vec3 temp = _s.velocity;
				const Vec3& normal = hit.normal;
				const float vDotNormal = (float)Dot(temp, normal);
				if (vDotNormal <= 0.f)
				{
					const Vec3 projectedNormal = normal * (double)-vDotNormal;
					temp = temp + projectedNormal;
					const float scaledFriction = (_p.bounceAngleAffectsFriction || _s.sliding)
						? (float)(std::clamp((double)-vDotNormal / std::sqrt(SizeSquared(temp)), (double)_p.minFrictionFraction, 1.0) * (double)_p.friction)
						: _p.friction;
					temp = temp * (double)std::clamp(1.f - scaledFriction, 0.f, 1.f);
					temp = temp + projectedNormal * (double)std::max(_p.bounciness, 0.f);
					temp = LimitVelocity(_p, temp);
				}
				return temp;
			}

			// HandleDeflection(:336-386) — 튕긴 속도가 아직 면으로 들어가면(또는 같은 면에 연달아 맞으면) 면을 따라 미끄러진다
			bool HandleDeflection(Blocking& b, const Vec3& oldVelocity, float& subTickTimeRemaining)
			{
				const Vec3 normal = b.hit.normal;
				const bool multiHit = _s.previousHitTime < 1.f && b.hit.time <= kKindaSmall;
				if (multiHit) ++_s.count.multiHits;
				_s.sliding = (multiHit && Dot(_s.previousHitNormal, normal) >= kNormalsParallel)
					|| (Dot(SafeNormal(_s.velocity), normal) <= (double)kSlideDotTolerance);
				if (_s.sliding)
				{
					if (multiHit && Dot(_s.previousHitNormal, normal) <= 0.0)
					{
						// 90도 이하 구석 — 두 법선의 외적 방향으로(:350-361)
						++_s.count.corners;
						const Vec3 newDir = SafeNormal(Cross(normal, _s.previousHitNormal));
						_s.velocity = newDir * Dot(_s.velocity, newDir);
						if (Dot(oldVelocity, _s.velocity) < 0.0) _s.velocity = _s.velocity * -1.0;
					}
					else
					{
						++_s.count.slides;
						_s.velocity = _s.velocity - normal * Dot(_s.velocity, normal);	// ComputeSlideVector(v, 1, n) = 면에 투영
					}
					if (UnderStopThreshold())
					{
						++_s.count.stopsOnSlide;
						Stop();
						return false;
					}
					if (subTickTimeRemaining > kKindaSmall)
					{
						if (!HandleSliding(b, subTickTimeRemaining)) return false;
					}
				}
				return true;
			}

			// HandleSliding(:389-447) — 면과 나란해진 속도로 남은 시간을 옮긴다. 다시 막히면 튕김 셈, 안 막히면 면으로 누르는 중력에 마찰
			// b 를 그 옮김의 맞음으로 덮는다 — 엔진이 같은 FHitResult 를 받아 쓰고, 고리가 그 시각 · 법선을 앞 맞음으로 적는다
			bool HandleSliding(Blocking& b, float& subTickTimeRemaining)
			{
				const Vec3 oldHitNormal = b.hit.normal;
				++_s.count.slideMoves;
				SafeMove(_s.velocity * (double)subTickTimeRemaining, _s.rotation, b);
				if (Halted()) return false;

				if (b.blocking)
				{
					++_s.count.slideHits;
					const float timeTick = subTickTimeRemaining;
					subTickTimeRemaining = timeTick * (1.f - (float)b.hit.time);
					if (HandleBlockingHit(b, timeTick, subTickTimeRemaining) == EBlockResult::Abort || Halted()) return false;
				}
				else
				{
					const Vec3 postTickVelocity = ComputeVelocity(_p, _s.velocity, subTickTimeRemaining);
					const Vec3 force = postTickVelocity - _s.velocity;
					const float forceDotN = (float)Dot(force, oldHitNormal);
					if (forceDotN < 0.f)
					{
						++_s.count.slideFriction;
						const Vec3 projectedForce = force - oldHitNormal * Dot(force, oldHitNormal);	// VectorPlaneProject
						const Vec3 newVelocity = _s.velocity + projectedForce;
						const Vec3 frictionForce = SafeNormal(newVelocity) * -std::min((double)(-forceDotN * _p.friction), std::sqrt(SizeSquared(newVelocity)));
						_s.velocity = newVelocity + frictionForce;
					}
					else
					{
						_s.velocity = postTickVelocity;
					}
					if (UnderStopThreshold())
					{
						++_s.count.stopsOnSlide;
						Stop();
						return false;
					}
					subTickTimeRemaining = 0.f;
				}
				return true;
			}

			const CMapCollision& _map;
			const ProjectileParams& _p;
			ProjectileState& _s;
			const ProjectileObstacle* _obstacles;
			size_t _obstacleCount;
			ProjectileHit* _hits;
			size_t _maxHits;
			size_t _hitCount = 0;
		};
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
		CStep step(map, p, s, obstacles, obstacleCount, hits, maxHits);
		const size_t hitCount = step.Run(deltaSeconds);

		// B_Grenade 의 PostLaunchCleanup 타이머(0.16초) — 나이 10 걸음 뒤에 켜진 것으로 두면 나이 11 걸음부터 속도를 따른다. 엔진 차례가 아니라 기록으로 맞춘 규칙(G-0-a · G-1-b 545발 회전 대조)
		if (!s.launchCleanedUp && s.frames * (double)deltaSeconds >= p.launchCleanupSeconds) s.launchCleanedUp = true;
		++s.frames;
		return hitCount;
	}

	bool SweepCapsuleCapsule(const Vec3& centerA, const Vec3& axisA, double radiusA, double halfHeightA, const Vec3& delta,
		const Vec3& centerB, const Vec3& axisB, double radiusB, double halfHeightB, double& outTime, Vec3& outNormal, Vec3& outImpact,
		bool& outStartPenetrating, double& outPenetrationDepth)
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

		outPenetrationDepth = 0;
		const double d0 = distanceAt(0.0, ca, cb);
		if (d0 <= reach)
		{
			outPenetrationDepth = reach - d0;
			return finish(0.0, ca, cb, true);
		}
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

	bool SweepCapsuleBox(const Vec3& centerA, const Vec3& axisA, double radiusA, double halfHeightA, const Vec3& delta,
		const Vec3& boxCenter, const Vec3& boxX, const Vec3& boxY, const Vec3& boxZ, const Vec3& halfExtent, double& outTime, Vec3& outNormal, Vec3& outImpact,
		bool& outStartPenetrating, double& outPenetrationDepth)
	{
		const auto toBox = [&](const Vec3& v) { return Vec3(Dot(v, boxX), Dot(v, boxY), Dot(v, boxZ)); };
		const auto toWorld = [&](const Vec3& v) { return boxX * v.x + boxY * v.y + boxZ * v.z; };
		const Segment a = CapsuleSegment(centerA, axisA, radiusA, halfHeightA);
		const Vec3 start = toBox(a.a - boxCenter), along = toBox(a.b - a.a), move = toBox(delta);
		const double reach = radiusA;
		Vec3 ps, pb;
		const auto distanceAt = [&](double t, Vec3& seg, Vec3& box) { return SegmentBoxClosest(start + move * t, along, halfExtent, seg, box); };

		const auto finish = [&](double t, const Vec3& seg, const Vec3& box, bool startPen) {
			outTime = t;
			outNormal = SafeNormal(toWorld(seg - box));	// 상자 → 쓸린 캡슐 축
			outImpact = boxCenter + toWorld(box);
			outStartPenetrating = startPen;
			return true;
		};

		outPenetrationDepth = 0;
		const double d0 = distanceAt(0.0, ps, pb);
		if (d0 <= reach)
		{
			outPenetrationDepth = reach - d0;
			if (d0 > 0.0) return finish(0.0, ps, pb, true);
			// 축 선분이 상자에 닿거나 든다 — 가까운 점이 상자 안이라 방향이 없다. 그 점에서 가장 얕은 면으로 뺀다
			const double pc[3] = { ps.x, ps.y, ps.z }, ec[3] = { halfExtent.x, halfExtent.y, halfExtent.z };
			int face = 0;
			for (int i = 1; i < 3; ++i)
				if (ec[i] - std::fabs(pc[i]) < ec[face] - std::fabs(pc[face])) face = i;
			const double sign = pc[face] < 0.0 ? -1.0 : 1.0;
			double local[3] = { 0.0, 0.0, 0.0 };
			local[face] = sign;
			double onFace[3] = { pc[0], pc[1], pc[2] };
			onFace[face] = sign * ec[face];
			outTime = 0.0;
			outNormal = toWorld(Vec3(local[0], local[1], local[2]));
			outImpact = boxCenter + toWorld(Vec3(onFace[0], onFace[1], onFace[2]));
			outStartPenetrating = true;
			outPenetrationDepth = reach + (ec[face] - std::fabs(pc[face]));
			return true;
		}
		const double length = std::sqrt(SizeSquared(delta));
		if (d0 - length > reach) return false;				// 끝까지 가도 못 닿는다

		// SweepCapsuleCapsule 과 같은 찾기 — 가장 가까운 때를 황금 분할로, 그 앞에서 처음 닿는 때를 이분법으로
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
		if (distanceAt(tMin, ps, pb) > reach) return false;
		lo = 0.0;
		hi = tMin;
		for (int i = 0; i < 64; ++i)
		{
			const double mid = 0.5 * (lo + hi);
			if (distanceAt(mid, ps, pb) > reach) lo = mid;
			else hi = mid;
		}
		distanceAt(hi, ps, pb);
		return finish(hi, ps, pb, false);
	}
}
