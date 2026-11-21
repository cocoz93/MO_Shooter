#include "Movement.h"

#include <algorithm>
#include <cfloat>
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
		p.maxWalkSpeedCrouched = 300.f;		// 0-3 실행 기록 · move_expanse_crouch.csv 머리(컨텐츠 D)
		p.crouchedHalfHeight = 65.f;		// LyraCharacter.cpp:62
		p.canCrouch = true;					// LyraCharacter.cpp:60
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

		// 선 캡슐 둘의 맞은 점 = 서 있는 쪽 축에서 두 축이 겹치는 높이(겹치지 않으면 가까운 끝) + 법선 × 그쪽 반지름
		// at = 움직이는 쪽 중심(축 반길이 segMover), center = 서 있는 쪽 중심(축 반길이 segPawn · 반지름 rPawn)
		inline Vec3 UprightImpactPoint(const Vec3& at, double segMover, const Vec3& center, double segPawn, const Vec3& n, double rPawn)
		{
			const double lo = std::max(center.z - segPawn, at.z - segMover), hi = std::min(center.z + segPawn, at.z + segMover);
			const double zb = lo <= hi ? (lo + hi) * 0.5 : (at.z > center.z ? center.z + segPawn : center.z - segPawn);
			return Vec3(center.x, center.y, zb) + n * rPawn;
		}

		// 선 캡슐 둘(컨텐츠 F) — 움직이는 캡슐(반지름 ra · 반높이 hha)이 서 있는 캡슐(center · rb · hhb)에 처음 닿는 시각.
		// 두 축이 모두 세로라 「점(움직이는 쪽 중심) ↔ 축 반길이 (hha−ra)+(hhb−rb) · 반지름 ra+rb 인 세로 캡슐」 광선 문제로 줄어든다.
		// 시각은 맞닿는 순간 그대로다(뒤로 물리기는 MoveComponent 가 맵 맞음과 같은 식으로 한다 — F-1 멈춤 거리 70.100 = 70 + 0.1)
		bool SweepUprightCapsule(const Vec3& start, const Vec3& end, double ra, double hha, const Vec3& center, double rb, double hhb, CapsuleHit& out)
		{
			const double R = ra + rb;
			const double segA = std::max(0.0, hha - ra), segB = std::max(0.0, hhb - rb);
			const double L = segA + segB;
			const auto axisPoint = [&](const Vec3& p) { return Vec3(center.x, center.y, center.z + std::max(-L, std::min(L, p.z - center.z))); };
			const auto fill = [&](const Vec3& at, double time, const Vec3& fallbackNormal)
			{
				const Vec3 off = at - axisPoint(at);
				const double len = Size(off);
				const Vec3 n = len > 1e-8 ? off * (1.0 / len) : fallbackNormal;
				out = CapsuleHit();
				out.time = time;
				out.distance = Size(end - start) * time;
				out.normal = n;
				out.impactNormal = n;
				out.impactPoint = UprightImpactPoint(at, segA, center, segB, n, rb);
				out.penetrationDepth = time == 0.0 ? std::max(0.0, R - len) : 0.0;
				out.startPenetrating = time == 0.0;
			};
			const Vec3 d = end - start;
			const Vec3 off0 = start - axisPoint(start);
			if (SizeSq(off0) < R * R)
			{
				// 시작부터 겹침 — 빠져나갈 방향은 축에서 멀어지는 쪽. 중심이 축 위면 움직임 반대쪽(그것도 없으면 +X)
				const Vec3 back = SafeNormal(Flat(d) * -1.0);
				fill(start, 0.0, IsZero(back) ? Vec3(1, 0, 0) : back);
				return true;
			}
			double best = 2.0;
			// 옆면 — 수평 원과 만나는 첫 시각, 그때 높이가 축 반길이 안
			const double hx = start.x - center.x, hy = start.y - center.y;
			const double a2 = d.x * d.x + d.y * d.y;
			if (a2 > 0.0)
			{
				const double b = hx * d.x + hy * d.y;
				const double c = hx * hx + hy * hy - R * R;
				const double disc = b * b - a2 * c;
				if (disc >= 0.0 && b < 0.0)
				{
					const double t = (-b - std::sqrt(disc)) / a2;
					if (t >= 0.0 && t <= 1.0 && std::fabs(start.z + d.z * t - center.z) <= L) best = t;
				}
			}
			// 위 · 아래 끝 구
			const double A = Dot(d, d);
			for (int side = -1; side <= 1 && A > 0.0; side += 2)
			{
				const Vec3 m = start - Vec3(center.x, center.y, center.z + side * L);
				const double B = Dot(m, d), Cc = Dot(m, m) - R * R;
				const double disc = B * B - A * Cc;
				if (disc < 0.0 || B >= 0.0) continue;
				const double t = (-B - std::sqrt(disc)) / A;
				if (t < 0.0 || t > 1.0 || side * (start.z + d.z * t - center.z) < L) continue;
				best = std::min(best, t);
			}
			if (best > 1.0) return false;
			fill(start + d * best, best, SafeNormal(Flat(d) * -1.0));
			return true;
		}

		// 사람끼리 쓸기를 엔진과 같은 단정밀도로(19 F-2) — Chaos GJKRaycast2ImplSimd(UE 5.8 GJK.h 1590~1800)를 세로 캡슐 둘에 대해 옮긴 것.
		// 엔진은 맞은 거리는 정확히 내지만 법선은 마지막으로 다가간 반복의 방향(V)을 돌려준다 — 정확한 법선과 최대 0.03도 다르고,
		// 비스듬히 스치는 칸에서 그 차이가 틱마다 쌓여 위치가 0.28 cm 까지 벌어졌다(BumpCheck still_p20).
		// 같은 계산을 파이썬으로 흉내 낸 것이 Lyra 기록 471번(cprobe_f1e) 전부와 법선 소수 8자리 · 거리 1e-6 까지 같았다.
		// - 곱더하기는 곱 · 더하기를 따로 반올림한다(Windows 기본 빌드는 FMA 꺼짐 — WindowsPlatform.h PLATFORM_ALWAYS_HAS_FMA3 0)
		// - 내적 = (x*x + y*y) + z*z, 정규화 = v * (1 / sqrt(내적))  (UnrealMathSSE.h InternalVectorDot3X · VectorNormalize)
		// 그래서 이 묶음은 float 연산 순서가 곧 사양이다. 식을 「정리」하지 말 것
		namespace chaos_f
		{
			struct V3f { float x, y, z; };
			inline V3f Sub(const V3f& a, const V3f& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
			inline V3f Neg(const V3f& a) { return { -a.x, -a.y, -a.z }; }
			inline float Dot3(const V3f& a, const V3f& b)
			{
				const float px = a.x * b.x, py = a.y * b.y, pz = a.z * b.z;
				return (px + py) + pz;
			}
			inline V3f Normalize(const V3f& v)
			{
				const float r = 1.f / std::sqrt(Dot3(v, v));
				return { v.x * r, v.y * r, v.z * r };
			}
			// 선분 받침 — 축 (0,0,1) 과의 내적이 0 이상이면 위 끝(CoreSegment.h 53~60)
			inline const V3f& Support(const V3f& dir, const V3f& bottom, const V3f& top) { return dir.z >= 0.f ? top : bottom; }

			// VectorLineSimplexFindOrigin(SimplexVectorized.h 12~62) — X0 은 Simplex[0] 참조라 갈아 끼운 뒤 값을 쓴다
			inline V3f LineSimplex(V3f* S, V3f* As, V3f* Bs, int& n)
			{
				const V3f x1 = S[1];
				const V3f x0ToX1 = Sub(S[1], S[0]);
				const float dot = Dot3(Neg(S[0]), x0ToX1);
				const bool isX0 = 0.f >= dot;
				const float sq = Dot3(x0ToX1, x0ToX1);
				const bool isX1 = dot >= sq || FLT_MIN >= sq;
				if (isX1)
				{
					S[0] = S[1];
					As[0] = As[1];
					Bs[0] = Bs[1];
				}
				float ratio = dot / sq;
				ratio = ratio > 0.f ? ratio : 0.f;		// _mm_max_ps(NaN, 0) = 0
				ratio = ratio < 1.f ? ratio : 1.f;
				V3f closest = { ratio * x0ToX1.x + S[0].x, ratio * x0ToX1.y + S[0].y, ratio * x0ToX1.z + S[0].z };
				if (isX0) closest = S[0];
				else if (isX1) closest = x1;
				n = isX0 || isX1 ? 1 : 2;
				return closest;
			}

			// VectorSimplexFindClosestToOrigin — 세로 캡슐 둘이면 삼각형은 늘 퇴화해 선분으로 돌아간다(SimplexVectorized.h 66~88).
			// 퇴화하지 않는 삼각형 · 사면체는 옮기지 않았다 → false
			inline bool FindClosest(V3f* S, int& n, V3f* As, V3f* Bs, V3f& out)
			{
				switch (n)
				{
				case 1:
					out = S[0];
					return true;
				case 2:
					out = LineSimplex(S, As, Bs, n);
					return true;
				case 3:
				{
					const V3f ab = Sub(S[1], S[0]), ac = Sub(S[2], S[0]);
					const V3f tri = { ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x };
					const V3f tiny = { S[0].x * FLT_MIN, S[0].y * FLT_MIN, S[0].z * FLT_MIN };
					if (!(Dot3(tiny, tiny) >= Dot3(tri, tri))) return false;
					n = 2;
					out = LineSimplex(S, As, Bs, n);
					return true;
				}
				default:
					return false;
				}
			}

			// Touch = 처음부터 여유 안(λ = 0)에서 끝남 — outLambda 에 겹친 깊이(0 이상), outNormal 에 빠져나갈 방향
			enum class Ray { Miss, Hit, Touch, Odd };
			// A = 맞는 쪽(지역 원점, 선분 반길이 segA), B = 움직이는 쪽(지역 좌표 선분 끝 bBot · bTop). 법선은 A 에서 B 쪽
			inline Ray Raycast(float segA, const V3f& bBot, const V3f& bTop, const V3f& rayDir, float rayLen, float inflation, float& outLambda, V3f& outNormal)
			{
				const V3f aBot = { 0.f, 0.f, -segA }, aTop = { 0.f, 0.f, segA };
				constexpr float kEps2 = 1e-6f;
				const float inflation2 = inflation * inflation + kEps2;
				V3f x = { 0.f, 0.f, 0.f };
				V3f sa = Support(rayDir, aBot, aTop), sb = Support(Neg(rayDir), bBot, bTop);
				V3f As[4] = { sa, x, x, x }, Bs[4] = { sb, x, x, x }, S[4] = { x, x, x, x };
				float lambda = 0.f;
				V3f v = Sub(x, Sub(sa, sb));
				V3f normal = { 0.f, 0.f, 1.f };
				int n = 0;
				float preDist2 = FLT_MAX;
				// 엔진 쓸기는 MTD 를 늘 켠다(SceneQuery.cpp 380 · GeometryQueries.cpp 65 — 원본 기준). 그래서 여유 안에서 시작하면
				// 바로 멈추지 않고 핵(선분)끼리 가장 가까운 점을 더 찾는다(GJK.h 1655 · 1727 · 1744~1762). 그 사이 실제로는 떨어져 있었음이 드러나면 광선이 다시 나아간다.
				// 이 이어 돌기는 F-2 봇 판에서 결과를 바꾼 적이 없고(꺼도 숫자가 같았다) Lyra 기록과도 대조하지 않았다 — 원본을 읽고 옮긴 것이다
				const float initialDist2 = Dot3(v, v);
				bool closeEnough = initialDist2 < inflation2 && initialDist2 < kEps2;
				bool inflatedCloseEnough = closeEnough;
				bool degenerate = false;
				for (int it = 0; !(closeEnough || degenerate) && it < 32; ++it)
				{
					v = Normalize(v);
					sa = Support(v, aBot, aTop);
					sb = Support(Neg(v), bBot, bTop);
					const V3f p = Sub(sa, sb);
					const V3f w = Sub(x, p);
					As[n] = sa;
					Bs[n] = sb;
					const float vDotW = Dot3(v, w);
					if (vDotW > inflation)
					{
						// 아직 떨어져 있다 — 광선을 따라 다가간다(보수적 전진). 이때의 V 가 법선이 된다
						const float vDotRay = Dot3(v, rayDir);
						if (vDotRay >= 0.f) return Ray::Miss;
						const float preLambda = lambda;
						lambda = lambda - (vDotW - inflation) / vDotRay;
						if (lambda > preLambda)
						{
							if (lambda > rayLen) return Ray::Miss;
							x = { lambda * rayDir.x + 0.f, lambda * rayDir.y + 0.f, lambda * rayDir.z + 0.f };
							normal = v;
							for (int i = 0; i < 3; ++i) S[i] = Sub(x, Sub(As[i], Bs[i]));
							S[n++] = Sub(x, p);
							preDist2 = FLT_MAX;
							inflatedCloseEnough = false;
						}
					}
					else
					{
						S[n++] = w;
					}
					// 엔진 그대로 — 거리(vDotW)와 거리 제곱(preDist2)을 견준다(GJK.h 1731)
					if (inflatedCloseEnough && vDotW >= 0.f) closeEnough = kEps2 + vDotW >= preDist2;
					if (!closeEnough)
					{
						if (!FindClosest(S, n, As, Bs, v)) return Ray::Odd;
						const float newDist2 = Dot3(v, v);
						closeEnough = inflation2 > newDist2;
						degenerate = newDist2 >= preDist2;
						preDist2 = newDist2;
						if (closeEnough)
						{
							inflatedCloseEnough = lambda == 0.f && preDist2 > kEps2 && inflation2 > kEps2 && n < 4;
							closeEnough = !inflatedCloseEnough;
						}
					}
					else
					{
						// 끝내기 전 단체 정리 한 번(GJK.h 1767)
						if (!FindClosest(S, n, As, Bs, v)) return Ray::Odd;
					}
				}
				if (lambda > 0.f)
				{
					outLambda = lambda;
					outNormal = normal;
					return Ray::Hit;
				}
				// 처음부터 여유 안 — GJK.h 1800~1849 의 IsDone 갈래. 핵끼리 맞닿은 깊은 겹침(preDist2 ≤ 1e-6)은 EPA 로 가는데 옮기지 않았다.
				// F-2 봇 판에서는 정확한 간격이 70.0000157(여유 70)인데 단정밀도 반복이 더 나아가지 못하고(degenerate) 끝나 깊이 0 으로 나왔다
				// (예측이 받은 남의 위치와 딱 맞닿은 채 다시 돌린 걸음 — 서버 쪽 0). 이 갈래도 Lyra 기록과 대조하지 않았다
				if (!(inflation > 0.f && preDist2 > kEps2 && FLT_MAX > preDist2)) return Ray::Odd;
				outNormal = Normalize(v);
				outLambda = std::max(std::min(inflation - std::sqrt(preDist2), FLT_MAX), 0.f);
				return Ray::Touch;
			}
		}

		// 엔진의 사람끼리 쓸기 — 맞는 쪽 캡슐의 지역 좌표(그 사람 yaw 로 돌린 것)에서 위 계산을 돌린다.
		// 시작부터 겹침 · 옮기지 않은 단체(odd)는 정확한 식(SweepUprightCapsule)으로 갈음한다
		bool SweepPawnLikeChaos(const Vec3& start, const Vec3& end, float radius, float halfHeight, const PawnCapsule& pawn, CapsuleHit& out, bool& odd, bool& touch)
		{
			odd = false;
			touch = false;
			CapsuleHit exact;
			const bool exactHit = SweepUprightCapsule(start, end, radius, halfHeight, pawn.center, pawn.radius, pawn.halfHeight, exact);
			// 시작부터 겹침 — 엔진은 EPA 로 빠져나갈 방향을 찾는다. 그 길은 옮기지 않았다(F-1 · F-2 기록에 겹친 채 시작한 사람 쓸기 없음)
			if (exactHit && exact.startPenetrating)
			{
				out = exact;
				return true;
			}
			// SceneQuery.cpp 512~533 — 쓸기 길이는 float 로 줄이고(DeltaSize), 방향 = 차이 × (1 / 그 float 길이)(FVector::operator/)
			const Vec3 delta = end - start;
			const float lengthF = (float)Size(delta);
			if (!(lengthF > kSmall))
			{
				if (exactHit) out = exact;
				return exactHit;
			}
			const double rScale = 1.0 / (double)lengthF;
			const Vec3 dir(delta.x * rScale, delta.y * rScale, delta.z * rScale);
			const double yawRad = (double)pawn.yaw * (3.14159265358979323846 / 180.0);
			const double c = std::cos(yawRad), s = std::sin(yawRad);
			const auto toLocal = [&](const Vec3& w) { return Vec3(c * w.x + s * w.y, -s * w.x + c * w.y, w.z); };
			const Vec3 b = toLocal(start - pawn.center);
			const Vec3 ld = toLocal(dir);
			const double segMover = std::max(0.0, (double)halfHeight - (double)radius);
			const double segPawn = std::max(0.0, (double)pawn.halfHeight - (double)pawn.radius);
			// 움직이는 쪽 선분 끝은 double 로 구한 뒤 float 로 (GeometryQueries.cpp ComputeLocalCapsule)
			const chaos_f::V3f bBot = { (float)b.x, (float)b.y, (float)(b.z - segMover) };
			const chaos_f::V3f bTop = { (float)b.x, (float)b.y, (float)(b.z - segMover + 2.0 * segMover) };
			const chaos_f::V3f rayDir = { (float)ld.x, (float)ld.y, (float)ld.z };
			float lambda = 0.f;
			chaos_f::V3f nLocal = { 0.f, 0.f, 0.f };
			switch (chaos_f::Raycast((float)segPawn, bBot, bTop, rayDir, lengthF, radius + pawn.radius, lambda, nLocal))
			{
			case chaos_f::Ray::Miss:
				return false;
			case chaos_f::Ray::Odd:
				odd = true;
				if (exactHit) out = exact;
				return exactHit;
			case chaos_f::Ray::Touch:
			{
				// 엔진 — 거리 = −깊이(≤ 0)라 처음부터 겹침으로 바뀐다(ChaosInterfaceWrapperCore.h HadInitialOverlap · CollisionConversions.cpp 583~621).
				// 시각 0 · 깊이 = |거리| · 법선 = 빠져나갈 방향. 0 거리의 법선 다시 구하기(같은 파일 647)는 엔진에서 빈 함수다
				touch = true;
				const Vec3 nt = SafeNormal(Vec3(c * nLocal.x - s * nLocal.y, s * nLocal.x + c * nLocal.y, nLocal.z));
				out = CapsuleHit();
				out.time = 0.0;
				out.distance = 0.0;
				out.normal = nt;
				out.impactNormal = nt;
				out.impactPoint = UprightImpactPoint(start, segMover, pawn.center, segPawn, nt, pawn.radius);
				out.penetrationDepth = lambda;
				out.startPenetrating = true;
				return true;
			}
			case chaos_f::Ray::Hit:
				break;
			}
			const Vec3 n = SafeNormal(Vec3(c * nLocal.x - s * nLocal.y, s * nLocal.x + c * nLocal.y, nLocal.z));
			// CollisionConversions.cpp 369 — 시각 = float(거리 / 쓸기 길이)
			const double time = (double)(float)((double)lambda / (double)lengthF);
			out = CapsuleHit();
			out.time = time;
			out.distance = lambda;
			out.normal = n;
			out.impactNormal = n;
			out.impactPoint = UprightImpactPoint(start + delta * time, segMover, pawn.center, segPawn, n, pawn.radius);
			out.penetrationDepth = 0.0;
			out.startPenetrating = false;
			return true;
		}

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
			out.standable = (hit.object & kMapExtraObjectBit) != 0;	// 사람 맞음은 object 0 이라 안 붙는다
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
		// CanAttemptJump = IsJumpAllowed && !bWantsToCrouch && (걷기 || 낙하). 걷기·낙하만 있으니 모드 검사는 항상 참.
		// Lyra 가 뺀 것은 ACharacter 의 앉음(bIsCrouched) 검사뿐이다(LyraCharacter.cpp:461) — 앉으려는 중이면 여전히 못 뛴다
		if (!_p.canJump || s.wantsToCrouch) return false;

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

	// ---------------------------------------------------------------- 앉기 (컨텐츠 D)

	void CMoveSim::ToggleCrouch(MoveState& state) const
	{
		if (state.crouched || state.wantsToCrouch)
		{
			state.wantsToCrouch = false;		// ACharacter::UnCrouch
		}
		else if (state.mode == EMoveMode::Walking && _p.canCrouch)
		{
			state.wantsToCrouch = true;			// ACharacter::Crouch — CanCrouch(앉지 않음 · CanEverCrouch)
		}
	}

	void CMoveSim::RequestUnCrouch(MoveState& state) const
	{
		state.wantsToCrouch = false;
	}

	float CMoveSim::CrouchedHalfHeight() const
	{
		return std::max(std::max(0.f, _p.capsuleRadius), _p.crouchedHalfHeight);
	}

	float CMoveSim::MaxSpeedNow() const
	{
		// LyraCharacterMovementComponent::GetMaxSpeed — 이동 금지면 0(부르는 쪽이 maxSpeed 를 0 으로 준다), 아니면 엔진 GetMaxSpeed:
		// 걷기 = 앉았으면 MaxWalkSpeedCrouched, 낙하 = MaxWalkSpeed(앉아 떨어져도). 조준 배수는 MaxWalkSpeed 만 바꾼다(0-3 실행 기록)
		const MoveState& s = *_s;
		if (s.maxSpeed == 0.f) return 0.f;
		return (IsWalking() && s.crouched) ? _p.maxWalkSpeedCrouched : s.maxSpeed;
	}

	void CMoveSim::UpdateCharacterStateBeforeMovement()
	{
		const MoveState& s = *_s;
		if (s.crouched && (!s.wantsToCrouch || !CanCrouchInCurrentState()))
		{
			UnCrouch();
		}
		else if (!s.crouched && s.wantsToCrouch && CanCrouchInCurrentState())
		{
			Crouch();
		}
	}

	void CMoveSim::UpdateCharacterStateAfterMovement()
	{
		if (_s->crouched && !CanCrouchInCurrentState()) UnCrouch();
	}

	void CMoveSim::Crouch()
	{
		MoveState& s = *_s;
		if (!CanCrouchInCurrentState()) return;
		if (HalfHeight() == _p.crouchedHalfHeight)
		{
			s.crouched = true;
			return;
		}

		// 캡슐을 줄인다(중심 그대로). 엔진은 SetCapsuleSize 뒤에 SetIsCrouched 를 하지만 그 사이에 앉음을 읽는 곳이 없어 같다.
		// bCrouchMaintainsBaseLocation = 걷기 모드에 들어올 때 참, 나갈 때 거짓(OnMovementModeChanged) → 걷기 모드인가와 같다
		const float halfHeightAdjust = HalfHeight() - CrouchedHalfHeight();
		s.crouched = true;
		if (IsWalking())
		{
			// 바닥 자리를 지키게 줄인 캡슐을 아래로 쓸어 내린다(MoveComponent 를 직접 — 평면 제한 없이)
			MoveComponent(Vec3(0.0, 0.0, -(double)halfHeightAdjust), true, nullptr, false);
			++_count.crouchGround;
		}
		else
		{
			++_count.crouchAir;
		}
		s.forceNextFloorCheck = true;
	}

	void CMoveSim::UnCrouch()
	{
		MoveState& s = *_s;
		if (!s.crouched) return;

		constexpr float kSweepInflation = kKindaSmall * 10.f;		// UE_KINDA_SMALL_NUMBER * 10
		constexpr double kMinExtent = (double)kKindaSmall * 10.0;	// GetPawnCapsuleExtent 의 「0 까지 줄이지 않기」
		const float radius = _p.capsuleRadius;
		const float currentHalf = HalfHeight();
		const float halfHeightAdjust = _p.capsuleHalfHeight - currentHalf;
		const Vec3 pawnLocation = s.location;

		// 선 캡슐보다 살짝 큰 캡슐 — GetPawnCapsuleCollisionShape(SHRINK_HeightCustom, -SweepInflation - 차이). 반높이 계산은 double, 모양에 담길 때 float
		const float shrink = -kSweepInflation - halfHeightAdjust;
		const float standHalf = (float)std::max((double)currentHalf - (double)shrink, kMinExtent);
		bool encroached = true;

		if (!IsWalking())
		{
			// 제자리에서 키운다
			encroached = _map.OverlapCapsule(pawnLocation, radius, standHalf, MAP_BLOCK_MOVE, Standables());
			if (encroached && halfHeightAdjust > 0.f)
			{
				// 반지름만큼 짧은 캡슐로 아래를 쓸어 바닥을 찾고, 거기서 일어설 수 있나 본다
				const float shrinkHalf = currentHalf - radius;
				const float traceDist = currentHalf - shrinkHalf;
				const float shortHalf = (float)std::max((double)currentHalf - (double)shrinkHalf, kMinExtent);
				MoveHit hit;
				SweepTest(pawnLocation, pawnLocation + Vec3(0.0, 0.0, -(double)traceDist), radius, shortHalf, hit);
				if (hit.startPenetrating)
				{
					encroached = true;
				}
				else
				{
					const float distanceToBase = (hit.time * traceDist) + shortHalf;
					const Vec3 newLocation = pawnLocation + Vec3(0.0, 0.0, (double)(-distanceToBase + standHalf + kSweepInflation + kMinFloorDist / 2.f));
					encroached = _map.OverlapCapsule(newLocation, radius, standHalf, MAP_BLOCK_MOVE, Standables());
					if (!encroached)
					{
						MoveComponent(newLocation - pawnLocation, false, nullptr, false);
						++_count.uncrouchAirShifted;
					}
				}
			}
			if (!encroached) ++_count.uncrouchAir;
		}
		else
		{
			// 바닥 자리를 지키며 키운다
			Vec3 standingLocation = pawnLocation + Vec3(0.0, 0.0, (double)(standHalf - currentHalf));
			encroached = _map.OverlapCapsule(standingLocation, radius, standHalf, MAP_BLOCK_MOVE, Standables());
			bool lowered = false;
			if (encroached)
			{
				// 머리 위가 살짝 걸리면 바닥 쪽으로 붙여 다시 본다
				const float minFloorDist = kKindaSmall * 10.f;
				if (s.floor.blockingHit && s.floor.floorDist > minFloorDist)
				{
					standingLocation.z -= (double)(s.floor.floorDist - minFloorDist);
					encroached = _map.OverlapCapsule(standingLocation, radius, standHalf, MAP_BLOCK_MOVE, Standables());
					lowered = true;
				}
			}
			if (!encroached)
			{
				MoveComponent(standingLocation - pawnLocation, false, nullptr, false);
				s.forceNextFloorCheck = true;
				++_count.uncrouchGround;
				if (lowered) ++_count.uncrouchGroundLowered;
			}
		}

		// 아직 걸리면 그만둔다 — 앉은 채로 남고, bWantsToCrouch 가 거짓이면 다음 틱에 다시 시도한다
		if (encroached)
		{
			++_count.uncrouchBlocked;
			return;
		}
		s.crouched = false;
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

		// PerformMovement — 앉기 · 일어서기는 발사보다 먼저(UpdateCharacterStateBeforeMovement → HandlePendingLaunch)
		// 밀기(루트 모션 힘)는 원본 차례대로 — 밖에서 바뀐 속도 옮기기 → 끝난 밀기 지우기 → 앉기 · 발사 → 발사가 바꾼 속도 옮기기 → 밀기 준비(2815~2941)
		RootForceState& rootForce = s.rootForce;
		if (rootForce.hasAdditive)
		{
			const Vec3 adjustment = s.velocity - s.lastUpdateVelocity;
			if (!IsZero(adjustment))
			{
				rootForce.lastPreAdditiveVelocity = rootForce.lastPreAdditiveVelocity + adjustment;
				++_count.rootForceExternal;
			}
		}
		const bool hasRootForce = rootForce.count > 0;	// HasRootMotionSources — 지우기 전에 본다. 그래서 막 끝난 이동에도 준비가 돌아 hasAdditive 가 꺼진다
		if (hasRootForce) CleanUpRootForce();
		const Vec3 oldVelocity = s.velocity;
		UpdateCharacterStateBeforeMovement();
		if (launch && !IsZero(*launch))
		{
			s.velocity = *launch;
			SetMode(EMoveMode::Falling);
			s.forceNextFloorCheck = true;
			++_count.launches;
		}
		if (rootForce.hasAdditive)
		{
			const Vec3 adjustment = s.velocity - oldVelocity;
			if (!IsZero(adjustment))
			{
				rootForce.lastPreAdditiveVelocity = rootForce.lastPreAdditiveVelocity + adjustment;
				++_count.rootForceExternal;
			}
		}
		if (hasRootForce) PrepareRootForce(deltaTime);

		ClearJumpInput(deltaTime);
		s.numJumpApexAttempts = 0;
		StartNewPhysics(deltaTime, 0);
		UpdateCharacterStateAfterMovement();
		s.lastUpdateVelocity = s.velocity;		// PerformMovement 끝(3165)
	}

	// ---------------------------------------------------------------- 루트 모션 힘(근접 밀기 컨텐츠 E-2-1 · 대시 D-1)

	namespace
	{
		// FRootMotionSourceGroup::AccumulateRootMotionVelocityFromSource — 덧붙임 갈래. 위아래 무시면 그 힘 앞의 위 속도로 되돌린다
		void AccumulateRootForce(const RootForceSource& source, Vec3& inOutVelocity)
		{
			const double inputZ = inOutVelocity.z;
			inOutVelocity = inOutVelocity + source.force;
			if (source.settings.ignoreZ) inOutVelocity.z = inputZ;
		}

		// FVector::GetClampedToMaxSize2D — 엔진 식 그대로(크기 × 역제곱근, 나누기가 아니다 — 반올림이 다르다)
		Vec3 ClampedToMaxSize2D(const Vec3& v, double maxSize)
		{
			if (maxSize < (double)kKindaSmall) return Vec3(0.0, 0.0, v.z);
			const double sq = v.x * v.x + v.y * v.y;
			if (sq > maxSize * maxSize)
			{
				const double scale = maxSize * (1.0 / std::sqrt(sq));
				return Vec3(v.x * scale, v.y * scale, v.z);
			}
			return v;
		}
	}

	uint16_t CMoveSim::ApplyRootForce(MoveState& state, const Vec3& force, float duration, const RootForceSettings& settings)
	{
		RootForceState& rootForce = state.rootForce;
		++_count.rootForceApplied;
		if (rootForce.count > 0) ++_count.rootForceSecond;
		if (rootForce.count == kRootForceSlots)
		{
			++_count.rootForceReplaced;
			for (int i = 1; i < kRootForceSlots; ++i) rootForce.sources[i - 1] = rootForce.sources[i];
			--rootForce.count;
		}
		// 번호 0 은 쓰지 않는다(엔진 Invalid = 0 을 건너뜀)
		if (++rootForce.nextId == 0) ++rootForce.nextId;
		RootForceSource& source = rootForce.sources[rootForce.count++];
		source = RootForceSource();
		source.force = force;
		source.duration = duration;
		source.id = rootForce.nextId;
		source.settings = settings;
		// hasAdditive · additiveApplied · lastPreAdditiveVelocity 는 묶음 쪽 값이라 그대로 둔다(원본은 대기 칸에 넣기만 한다)
		return source.id;
	}

	void CMoveSim::RemoveRootForce(MoveState& state, uint16_t id) const
	{
		RootForceState& rootForce = state.rootForce;
		for (int i = 0; i < rootForce.count; ++i)
		{
			if (rootForce.sources[i].id == id) rootForce.sources[i].removing = true;
		}
	}

	void CMoveSim::CleanUpRootForce()
	{
		MoveState& s = *_s;
		RootForceState& rootForce = s.rootForce;
		int kept = 0;
		for (int i = 0; i < rootForce.count; ++i)
		{
			const RootForceSource source = rootForce.sources[i];
			if (!source.finished && !source.removing)
			{
				rootForce.sources[kept++] = source;
				continue;
			}
			if (source.removing) ++_count.rootForceRemoved;
			else ++_count.rootForceFinished;
			// 대기 칸에서 지워지면 묶음에 들어간 적이 없어 남길 몫 · 끝 처리가 없다(PendingAddRootMotionSources.RemoveAll — 1420~1442)
			if (source.pending) continue;
			// 덧붙인 힘이 지워질 때 그 속도를 덧붙이기 전 속도에 더해 남긴다(RootMotionSource.cpp 1362~1377) — 다음 걸음의 되돌림이 이 값을 쓰므로
			// 마지막 힘 속도가 그대로 이어진다. 근접 밀기는 유지 갈래라 여기까지 — Lyra 기록도 1000 으로 안 잘렸다
			if (rootForce.additiveApplied) AccumulateRootForce(source, rootForce.lastPreAdditiveVelocity);
			if (source.settings.finish == ERootForceFinish::ClampVelocity)
			{
				// 위는 양수만 자른다 — 떨어지는 속도는 안 늦춘다(1383 주석). 덧붙인 채면 덧붙이기 전 속도가 속도를 덮으므로 그것도 자른다
				const double clamp = (double)source.settings.clamp;
				s.velocity = ClampedToMaxSize2D(s.velocity, clamp);
				s.velocity.z = std::min(s.velocity.z, clamp);
				if (rootForce.additiveApplied)
				{
					rootForce.lastPreAdditiveVelocity = ClampedToMaxSize2D(rootForce.lastPreAdditiveVelocity, clamp);
					rootForce.lastPreAdditiveVelocity.z = std::min(rootForce.lastPreAdditiveVelocity.z, clamp);
				}
				++_count.rootForceClamped;
			}
		}
		rootForce.count = kept;
	}

	void CMoveSim::PrepareRootForce(float deltaTime)
	{
		RootForceState& rootForce = _s->rootForce;
		rootForce.hasAdditive = false;
		// 대기 칸을 묶음에 붙이고(넣은 차례 그대로) 모두 준비한다(bForcePrepareAll — PerformMovement 2941)
		for (int i = 0; i < rootForce.count; ++i)
		{
			RootForceSource& source = rootForce.sources[i];
			source.pending = false;
			// 끝 틱 자르기 끔 · 시작 시각 조정 없음 → 준비 시간 = 이동 틱이라 이번 몫 = Force × (틱 ÷ 틱) = Force.
			// 시간은 float 로 쌓는다(SetTime → CheckTimeOut) — 1/60 세 번의 합이 반올림으로 0.05 를 넘어 3번째 이동에서 끝난다(Lyra 3프레임과 같음).
			// 대시 0.25 는 15번째에 꼭 0.25 가 되어 끝난다(D-0 힘 15프레임)
			source.time = source.time + deltaTime;
			source.finished = source.time >= source.duration;
			rootForce.hasAdditive = true;
			++_count.rootForcePrepared;
		}
	}

	void CMoveSim::RestorePreAdditiveRootMotionVelocity()
	{
		RootForceState& rootForce = _s->rootForce;
		if (!rootForce.additiveApplied) return;
		_s->velocity = rootForce.lastPreAdditiveVelocity;
		rootForce.additiveApplied = false;
		++_count.rootForceRestores;
	}

	void CMoveSim::ApplyRootMotionToVelocity(float deltaTime)
	{
		MoveState& s = *_s;
		RootForceState& rootForce = s.rootForce;
		if (!rootForce.hasAdditive) return;
		const Vec3 oldVelocity = s.velocity;
		rootForce.lastPreAdditiveVelocity = s.velocity;
		for (int i = 0; i < rootForce.count; ++i)
		{
			const RootForceSource& source = rootForce.sources[i];
			AccumulateRootForce(source, s.velocity);
			if (!source.settings.ignoreZ) ++_count.rootForceZAdds;
		}
		rootForce.additiveApplied = true;
		++_count.rootForceAdds;

		// 힘이 위 속도를 더했고 땅 위면 낙하로 바꿔 뜰 수 있게 한다(4566~4586) — 기준 = 이 틱에 중력이 빼는 양(민감 검사 설정은 상수 힘이 안 켠다).
		// 근접 밀기는 위아래를 안 건드려 늘 0 이다
		const double appliedZ = s.velocity.z - oldVelocity.z;
		if (appliedZ != 0.0 && IsWalking())
		{
			const float liftoffBound = std::max(-_p.gravityZ * deltaTime, kSmall);
			if (appliedZ > (double)liftoffBound)
			{
				SetMode(EMoveMode::Falling);
				++_count.rootForceLiftoffs;
			}
		}
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
		const float maxInputSpeed = std::max(MaxSpeedNow() * s.analogInputModifier, _p.minAnalogWalkSpeed);
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
		bool hit = _map.SweepCapsule(traceStart, traceEnd, _p.capsuleRadius, HalfHeight(), MAP_BLOCK_MOVE, sweepHit, Standables());
		// 다른 사람 캡슐(컨텐츠 F) — 맵보다 먼저 닿거나(같은 시각이면 맵), 맵 맞음이 빠져나가는 겹침이라 무시될 때 사람 맞음을 쓴다
		// (엔진은 여러 맞음을 받아 무시하지 않는 첫 맞음을 고른다 — PrimitiveComponent MoveComponentImpl, 원본 기준)
		bool pawnHit = false;
		CapsuleHit pawnSweep;
		if (_pawnCount > 0 && SweepPawns(traceStart, traceEnd, _p.capsuleRadius, HalfHeight(), pawnSweep))
		{
			const bool mapIgnored = hit && sweepHit.startPenetrating && !neverIgnoreBlockingOverlaps && (float)Dot(sweepHit.impactNormal, SafeNormal(delta)) > 0.f;
			if (!hit || mapIgnored || pawnSweep.time < sweepHit.time)
			{
				sweepHit = pawnSweep;
				hit = pawnHit = true;
			}
		}

		bool blocked = false;
		MoveHit blocking;
		if (hit)
		{
			FillFromCapsule(blocking, sweepHit, traceStart, traceEnd);
			blocking.pawn = pawnHit;
			if (pawnHit)
			{
				++_count.pawnHits;
				if (blocking.startPenetrating) ++_count.pawnStartPenetrating;
			}
			if (blocking.standable) ++_count.standableHits;
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
		// 사람에게서 빼낼 때는 따로 한도(엔진 UCharacterMovementComponent::GetPenetrationAdjustment — 맞은 것이 폰이면 MaxDepenetrationWithPawn)
		return ClampedToMaxSize(result, hit.pawn ? _p.maxDepenetrationWithPawn : _p.maxDepenetrationWithGeometry);
	}

	bool CMoveSim::ResolvePenetration(const Vec3& adjustment, const MoveHit& hit)
	{
		MoveState& s = *_s;
		bool result = false;
		if (!IsZero(adjustment))
		{
			const bool encroached = _map.OverlapCapsule(hit.traceStart + adjustment, _p.capsuleRadius + kOverlapInflation,
				HalfHeight() + kOverlapInflation, MAP_BLOCK_MOVE, Standables())
				|| OverlapPawns(hit.traceStart + adjustment, _p.capsuleRadius + kOverlapInflation, HalfHeight() + kOverlapInflation);
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
		if (_map.SweepCapsule(start, end, radius, halfHeight, MAP_BLOCK_MOVE, hit, Standables()))
		{
			FillFromCapsule(out, hit, start, end);
		}
	}

	bool CMoveSim::SweepPawns(const Vec3& start, const Vec3& end, float radius, float halfHeight, CapsuleHit& out)
	{
		bool any = false;
		for (size_t i = 0; i < _pawnCount; ++i)
		{
			CapsuleHit h;
			bool odd = false, touch = false;
			const bool got = SweepPawnLikeChaos(start, end, radius, halfHeight, _pawns[i], h, odd, touch);
			if (odd) ++_count.pawnOddSimplex;
			if (touch) ++_count.pawnTouchStart;
			if (!got) continue;
			if (!any || h.time < out.time)
			{
				out = h;
				any = true;
			}
		}
		return any;
	}

	bool CMoveSim::OverlapPawns(const Vec3& center, float radius, float halfHeight) const
	{
		for (size_t i = 0; i < _pawnCount; ++i)
		{
			const PawnCapsule& p = _pawns[i];
			const double L = std::max(0.0, (double)halfHeight - radius) + std::max(0.0, (double)p.halfHeight - p.radius);
			const double dz = std::max(0.0, std::fabs(center.z - p.center.z) - L);
			const double dx = center.x - p.center.x, dy = center.y - p.center.y;
			const double R = (double)radius + p.radius;
			if (dx * dx + dy * dy + dz * dz < R * R) return true;
		}
		return false;
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
				// 걷는 중의 유효한 부딪힘이면 CanStepUp 은 참이다(맵에 턱 금지 표시가 없다). 사람 캡슐은 거짓 — 오르지 않고 바로 미끄러진다(컨텐츠 F,
				// 엔진 MoveAlongFloor 의 「!CanCharacterStepUp → HandleImpact · SlideAlongSurface」 갈래)
				if (hit.pawn || !StepUp(delta * (double)(1.f - percentTimeApplied), hit, outComputedFloor, outFloor))
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
		const float pawnHalfHeight = HalfHeight();

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

			RestorePreAdditiveRootMotionVelocity();
			MaintainHorizontalGroundVelocity();
			s.acceleration = Flat(s.acceleration);

			CalcVelocity(timeTick, _p.groundFriction, _p.brakingDecelerationWalking);
			ApplyRootMotionToVelocity(timeTick);
			if (!IsWalking())
			{
				// 힘이 땅에서 띄웠다(대시 위 보고 앞) — 이 틱은 아직 안 움직였으니 시간 · 반복 수를 그대로 넘겨 낙하로 다시 시작한다(5723~5729)
				StartNewPhysics(remainingTime + timeTick, iterations - 1);
				return;
			}

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
		const float pawnHalfHeight = HalfHeight();

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
			if (_map.Raycast(location, lineEnd, MAP_BLOCK_MOVE, ray, Standables()))
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
					lineHit.standable = (ray.object & kMapExtraObjectBit) != 0;

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
		const float hitAboveBase = std::max(0.f, (float)(hit.impactPoint.z - capsuleLocation.z) + HalfHeight());
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

			const double lowerHemisphereZ = hit.location.z - (double)HalfHeight() + (double)_p.capsuleRadius;
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
			RestorePreAdditiveRootMotionVelocity();
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
			ApplyRootMotionToVelocity(timeTick);

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
						// 묶음의 힘마다 돌려준 시간을 돌려준다(5002) — SetTime 이 끝남 표시를 다시 본다. 걸음 중엔 대기 칸이 없다(준비가 다 붙였다)
						for (int i = 0; i < s.rootForce.count; ++i)
						{
							RootForceSource& source = s.rootForce.sources[i];
							source.time = std::max(0.f, source.time - timeToRefund);
							source.finished = source.time >= source.duration;
							++_count.rootForceApexRefund;
						}
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
								s.velocity.x += (double)(0.25f * MaxSpeedNow() * (random01() - 0.5f));
								s.velocity.y += (double)(0.25f * MaxSpeedNow() * (random01() - 0.5f));
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
