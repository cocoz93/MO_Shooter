#include "MapCollision.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace mo
{
	namespace
	{
		constexpr double kInf = std::numeric_limits<double>::infinity();
		constexpr float kFloatMax = std::numeric_limits<float>::max();
		constexpr double kSmall = 1e-8;		// 언리얼 UE_SMALL_NUMBER — Chaos 광선 판정과 같은 허용치
		constexpr double kCullParallel = 1e-4;	// 언리얼 UE_KINDA_SMALL_NUMBER — Chaos 캡슐 판정의 「면과 나란함」 기준
		constexpr float kNodePad = 0.01f;	// float 반올림으로 나무 상자가 삼각형을 비껴가지 않게 하는 여유(cm)
		constexpr uint32_t kLeafSize = 4;
		constexpr int kBins = 16;
		constexpr int kMaxStack = 128;
		constexpr double kOpposingSearch = 1.0;	// 언리얼 FindFaceIndex 가 쓰는 거리(cm) — 맞은 점 근처의 면을 찾는 범위

		inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline Vec3 Cross(const Vec3& a, const Vec3& b)
		{
			return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		}
		inline double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
		inline double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
		inline Vec3 VertexOf(const float* v, int index)
		{
			return { v[index * 3], v[index * 3 + 1], v[index * 3 + 2] };
		}

		struct Box
		{
			float min[3] = { kFloatMax, kFloatMax, kFloatMax };
			float max[3] = { -kFloatMax, -kFloatMax, -kFloatMax };
			void Add(const float* p)
			{
				for (int i = 0; i < 3; ++i)
				{
					min[i] = std::min(min[i], p[i]);
					max[i] = std::max(max[i], p[i]);
				}
			}
			void Add(const Box& o)
			{
				for (int i = 0; i < 3; ++i)
				{
					min[i] = std::min(min[i], o.min[i]);
					max[i] = std::max(max[i], o.max[i]);
				}
			}
			float Area() const
			{
				const float dx = max[0] - min[0], dy = max[1] - min[1], dz = max[2] - min[2];
				if (dx < 0) return 0;
				return dx * dy + dy * dz + dz * dx;
			}
		};

		// 광선 하나의 나무 순회에 쓰는 값. 0 인 축은 아주 작은 값으로 바꿔 0 × 무한 = NaN 을 피한다
		struct Slab
		{
			double origin[3];
			double inv[3];
			Slab(const Vec3& o, const Vec3& d)
			{
				origin[0] = o.x; origin[1] = o.y; origin[2] = o.z;
				const double dir[3] = { d.x, d.y, d.z };
				for (int i = 0; i < 3; ++i)
				{
					inv[i] = 1.0 / (std::fabs(dir[i]) < 1e-30 ? 1e-30 : dir[i]);
				}
			}
			// pad = 상자를 부풀리는 양. 맞으면 들어가는 t 를 돌려주고 아니면 무한
			template <typename NodeType>
			double Enter(const NodeType& node, const double* pad, double maxT) const
			{
				double t0 = 0, t1 = maxT;
				for (int i = 0; i < 3; ++i)
				{
					double a = (node.min[i] - pad[i] - origin[i]) * inv[i];
					double b = (node.max[i] + pad[i] - origin[i]) * inv[i];
					if (a > b) std::swap(a, b);
					t0 = a > t0 ? a : t0;
					t1 = b < t1 ? b : t1;
					if (t0 > t1) return kInf;
				}
				return t0;
			}
		};

		// Chaos RayTriangleIntersectionAndBary 와 같은 계산(양면). 뒷면 처리는 부르는 쪽이 한다
		// dir 은 단위 벡터, t 는 cm
		inline bool RayTriangle(const Vec3& start, const Vec3& dir, double maxT, const Vec3& a, const Vec3& b, const Vec3& c, double& outT, Vec3& outCross)
		{
			const Vec3 ab = b - a;
			const Vec3 ac = c - a;
			const Vec3 normal = Cross(ab, ac);
			const Vec3 negDir = dir * -1.0;
			const double den = Dot(negDir, normal);
			if (std::fabs(den) < kSmall) return false;

			const double invDen = 1.0 / den;
			const Vec3 toA = start - a;
			const double t = Dot(toA, normal) * invDen;
			if (t < 0 || t > maxT) return false;

			const Vec3 crossed = Cross(negDir, toA);
			const double u = Dot(ac, crossed) * invDen;
			if (u < -kSmall || u > 1 + kSmall) return false;
			const double v = -Dot(ab, crossed) * invDen;
			if (v < -kSmall || u + v > 1 + kSmall) return false;

			outT = t;
			outCross = normal;
			return true;
		}

		// 점 → 선분 a-b 의 가장 가까운 점
		inline Vec3 ClosestOnSegment(const Vec3& p, const Vec3& a, const Vec3& b)
		{
			const Vec3 ab = b - a;
			const double len2 = Dot(ab, ab);
			if (len2 < 1e-18) return a;
			return a + ab * Clamp01(Dot(p - a, ab) / len2);
		}

		// 선분 둘의 가장 가까운 두 점(Ericson, Real-Time Collision Detection 5.1.9)
		inline void ClosestSegmentSegment(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1, Vec3& c2)
		{
			const Vec3 d1 = q1 - p1;
			const Vec3 d2 = q2 - p2;
			const Vec3 r = p1 - p2;
			const double a = Dot(d1, d1);
			const double e = Dot(d2, d2);
			const double f = Dot(d2, r);
			double s = 0, t = 0;
			if (a < 1e-18 && e < 1e-18)
			{
				c1 = p1; c2 = p2;
				return;
			}
			if (a < 1e-18)
			{
				t = Clamp01(f / e);
			}
			else
			{
				const double c = Dot(d1, r);
				if (e < 1e-18)
				{
					s = Clamp01(-c / a);
				}
				else
				{
					const double b = Dot(d1, d2);
					const double denom = a * e - b * b;
					s = denom > 1e-12 ? Clamp01((b * f - c * e) / denom) : 0;
					t = (b * s + f) / e;
					if (t < 0)
					{
						t = 0;
						s = Clamp01(-c / a);
					}
					else if (t > 1)
					{
						t = 1;
						s = Clamp01((b - c) / a);
					}
				}
			}
			c1 = p1 + d1 * s;
			c2 = p2 + d2 * t;
		}

		// 점 → 삼각형의 가장 가까운 점(Ericson 5.1.5)
		inline Vec3 ClosestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
		{
			const Vec3 ab = b - a, ac = c - a, ap = p - a;
			const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
			if (d1 <= 0 && d2 <= 0) return a;
			const Vec3 bp = p - b;
			const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
			if (d3 >= 0 && d4 <= d3) return b;
			const double vc = d1 * d4 - d3 * d2;
			if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
			const Vec3 cp = p - c;
			const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
			if (d6 >= 0 && d5 <= d6) return c;
			const double vb = d5 * d2 - d1 * d6;
			if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
			const double va = d3 * d6 - d5 * d4;
			if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
			{
				return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
			}
			const double denom = 1.0 / (va + vb + vc);
			return a + ab * (vb * denom) + ac * (vc * denom);
		}

		// 캡슐 축 선분과 삼각형의 가장 가까운 두 점. 돌려준 값 = 거리
		double SegmentTriangleDistance(const Vec3& s0, const Vec3& s1, const Vec3& a, const Vec3& b, const Vec3& c, Vec3& onSeg, Vec3& onTri)
		{
			const Vec3 d = s1 - s0;
			const double len = Length(d);
			if (len > 1e-9)
			{
				double t; Vec3 unused;
				if (RayTriangle(s0, d * (1.0 / len), len, a, b, c, t, unused))
				{
					onSeg = onTri = s0 + d * (t / len);
					return 0;
				}
			}

			double best = kInf;
			const auto consider = [&](const Vec3& p, const Vec3& q)
			{
				const Vec3 diff = p - q;
				const double dist2 = Dot(diff, diff);
				if (dist2 < best)
				{
					best = dist2;
					onSeg = p;
					onTri = q;
				}
			};
			consider(s0, ClosestOnTriangle(s0, a, b, c));
			consider(s1, ClosestOnTriangle(s1, a, b, c));
			const Vec3* edges[3][2] = { {&a, &b}, {&b, &c}, {&c, &a} };
			for (const auto& edge : edges)
			{
				Vec3 c1, c2;
				ClosestSegmentSegment(s0, s1, *edge[0], *edge[1], c1, c2);
				consider(c1, c2);
			}
			return std::sqrt(best);
		}

		// 광선 o + d·t (t = 0~1) 가 선분 a-b 를 반지름 r 로 감싼 캡슐에 처음 들어가는 t
		bool RayCapsule(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, double r, double& outT)
		{
			const double r2 = r * r;
			const Vec3 nearest = ClosestOnSegment(o, a, b);
			const Vec3 fromNearest = o - nearest;
			if (Dot(fromNearest, fromNearest) <= r2)
			{
				outT = 0;
				return true;
			}

			double best = kInf;
			const Vec3 ab = b - a;
			const double dd = Dot(ab, ab);

			if (dd > 1e-18)
			{
				const Vec3 m = o - a;
				const double md = Dot(m, ab);
				const double nd = Dot(d, ab);
				const double qa = dd * Dot(d, d) - nd * nd;
				const double qb = dd * Dot(m, d) - nd * md;
				const double qc = dd * (Dot(m, m) - r2) - md * md;
				if (qa > 1e-12)
				{
					const double disc = qb * qb - qa * qc;
					if (disc < 0) return false;		// 무한 원기둥도 안 지나면 캡슐도 안 지난다
					const double t = (-qb - std::sqrt(disc)) / qa;
					if (t >= 0 && t <= 1)
					{
						const double y = md + t * nd;
						if (y >= 0 && y <= dd) best = t;
					}
				}
			}

			const auto sphere = [&](const Vec3& c)
			{
				const Vec3 m = o - c;
				const double qa = Dot(d, d);
				if (qa < 1e-18) return;
				const double qb = Dot(m, d);
				const double qc = Dot(m, m) - r2;
				const double disc = qb * qb - qa * qc;
				if (disc < 0) return;
				const double t = (-qb - std::sqrt(disc)) / qa;
				if (t >= 0 && t <= 1 && t < best) best = t;
			};
			sphere(a);
			if (dd > 1e-18) sphere(b);

			if (best == kInf) return false;
			outT = best;
			return true;
		}

		// 움직이는 선분 s0-s1(이동 d)과 고정 선분 e0-e1 사이 거리가 r 이 되는 첫 t.
		// 민코프스키 합의 네 모서리(끝점 ↔ 상대 선분) + 납작한 두 면으로 나눠 푼다
		void SweepSegmentSegment(const Vec3& s0, const Vec3& s1, const Vec3& d, const Vec3& e0, const Vec3& e1, double r, double& best)
		{
			double t;
			if (RayCapsule(s0, d, e0, e1, r, t) && t < best) best = t;
			if (RayCapsule(s1, d, e0, e1, r, t) && t < best) best = t;
			const Vec3 negD = d * -1.0;
			if (RayCapsule(e0, negD, s0, s1, r, t) && t < best) best = t;
			if (RayCapsule(e1, negD, s0, s1, r, t) && t < best) best = t;

			const Vec3 ds = s1 - s0;
			const Vec3 de = e1 - e0;
			Vec3 n = Cross(ds, de);
			const double nLen = Length(n);
			if (nLen < 1e-9) return;
			n = n * (1.0 / nLen);

			const Vec3 w0 = s0 - e0;
			const double h0 = Dot(w0, n);
			const double hd = Dot(d, n);
			double planeT;
			if (h0 > r && hd < 0) planeT = (r - h0) / hd;
			else if (h0 < -r && hd > 0) planeT = (-r - h0) / hd;
			else return;
			if (planeT < 0 || planeT > 1 || planeT >= best) return;

			const Vec3 w = w0 + d * planeT;
			const double a = Dot(ds, ds), b = Dot(ds, de), c = Dot(de, de);
			const double dw = Dot(ds, w), ew = Dot(de, w);
			const double denom = a * c - b * b;
			if (denom < 1e-12) return;
			const double sp = (b * ew - c * dw) / denom;
			const double up = (a * ew - b * dw) / denom;
			if (sp >= 0 && sp <= 1 && up >= 0 && up <= 1) best = planeT;
		}

		// 캡슐 축 s0-s1 이 d 만큼 움직일 때 삼각형에 처음 닿는 t. 면 + 세 변
		void SweepSegmentTriangle(const Vec3& s0, const Vec3& s1, const Vec3& d, double r, const Vec3& a, const Vec3& b, const Vec3& c, double& best)
		{
			Vec3 n = Cross(b - a, c - a);
			const double nLen = Length(n);
			if (nLen < 1e-9) return;
			n = n * (1.0 / nLen);

			const Vec3* ends[2] = { &s0, &s1 };
			for (const Vec3* end : ends)
			{
				const double h0 = Dot(*end - a, n);
				const double hd = Dot(d, n);
				double t;
				double side;
				if (h0 > r && hd < 0) { t = (r - h0) / hd; side = 1; }
				else if (h0 < -r && hd > 0) { t = (-r - h0) / hd; side = -1; }
				else continue;
				if (t < 0 || t > 1 || t >= best) continue;

				const Vec3 onPlane = *end + d * t - n * (side * r);
				const Vec3 snapped = ClosestOnTriangle(onPlane, a, b, c);
				const Vec3 diff = onPlane - snapped;
				if (Dot(diff, diff) < 1e-10) best = t;
			}

			SweepSegmentSegment(s0, s1, d, a, b, r, best);
			SweepSegmentSegment(s0, s1, d, b, c, r, best);
			SweepSegmentSegment(s0, s1, d, c, a, r, best);
		}
	}

	void CMapCollision::Clear()
	{
		_move = Tree();
		_weapon = Tree();
		_prims.clear();
		_objectCount = _triangleCount = _degenerateCount = 0;
	}

	EMapLoad CMapCollision::LoadFromMemory(const void* data, size_t size)
	{
		Clear();
		const uint8_t* cursor = static_cast<const uint8_t*>(data);
		const uint8_t* const fileEnd = cursor + size;
		const auto left = [&]() { return (size_t)(fileEnd - cursor); };
		const auto readU32 = [&]() { uint32_t v; std::memcpy(&v, cursor, 4); cursor += 4; return v; };
		const auto readVec = [&]()
		{
			float v[3];
			std::memcpy(v, cursor, 12);
			cursor += 12;
			return Vec3(v[0], v[1], v[2]);
		};
		const auto readF = [&]() { float v; std::memcpy(&v, cursor, 4); cursor += 4; return (double)v; };

		if (size < 40) return EMapLoad::TooSmall;
		if (std::memcmp(cursor, "MOCL", 4) != 0) return EMapLoad::BadMagic;
		cursor += 4;
		const uint32_t version = readU32();
		if (version != 2) return EMapLoad::BadVersion;	// 1 은 뒷면·속 찬 물체 표시가 없어 판정이 조용히 달라진다
		const uint32_t objectCount = readU32();
		const uint32_t triangleCount = readU32();
		cursor += 24;	// 범위는 쓰지 않는다

		_move.tris.reserve(triangleCount);
		_weapon.tris.reserve(triangleCount);

		uint32_t seenTriangles = 0;
		for (uint32_t object = 0; object < objectCount; ++object)
		{
			if (left() < 8) return EMapLoad::Truncated;
			const uint32_t flags = readU32();
			const uint32_t kind = readU32();
			if (kind == 0)
			{
				if (left() < 4) return EMapLoad::Truncated;
				const uint32_t triCount = readU32();
				if (left() < (size_t)triCount * 36) return EMapLoad::Truncated;
				if (flags & MAP_SOLID)
				{
					if (flags & MAP_BLOCK_MOVE) AddSolid(_move, cursor, triCount, object, flags);
					if (flags & MAP_BLOCK_WEAPON) AddSolid(_weapon, cursor, triCount, object, flags);
				}
				for (uint32_t i = 0; i < triCount; ++i)
				{
					Tri tri;
					std::memcpy(tri.v, cursor, 36);
					cursor += 36;
					tri.object = object;
					tri.flags = flags;
					const Vec3 a = VertexOf(tri.v, 0), b = VertexOf(tri.v, 1), c = VertexOf(tri.v, 2);
					const Vec3 cr = Cross(b - a, c - a);
					if (Dot(cr, cr) < 1e-16)
					{
						++_degenerateCount;
						continue;
					}
					if (flags & MAP_BLOCK_MOVE) _move.tris.push_back(tri);
					if (flags & MAP_BLOCK_WEAPON) _weapon.tris.push_back(tri);
				}
				seenTriangles += triCount;
			}
			else if (kind == 1)
			{
				if (left() < 16) return EMapLoad::Truncated;
				Prim prim;
				prim.a = prim.b = readVec();
				prim.radius = readF();
				prim.object = object;
				prim.flags = flags;
				_prims.push_back(prim);
			}
			else if (kind == 2)
			{
				if (left() < 28) return EMapLoad::Truncated;
				Prim prim;
				prim.a = readVec();
				prim.b = readVec();
				prim.radius = readF();
				prim.object = object;
				prim.flags = flags;
				_prims.push_back(prim);
			}
			else
			{
				return EMapLoad::BadKind;
			}
		}
		if (seenTriangles != triangleCount || left() != 0) return EMapLoad::CountMismatch;

		_objectCount = objectCount;
		_triangleCount = triangleCount;
		Build(_move);
		Build(_weapon);
		return EMapLoad::Ok;
	}

	void CMapCollision::AddSolid(Tree& tree, const uint8_t* triangles, uint32_t count, uint32_t object, uint32_t flags)
	{
		Solid solid;
		solid.firstPlane = (uint32_t)tree.planes.size();
		solid.object = object;
		solid.flags = flags;
		Box bounds;
		for (uint32_t i = 0; i < count; ++i)
		{
			float v[9];
			std::memcpy(v, triangles + (size_t)i * 36, 36);
			for (int vi = 0; vi < 3; ++vi) bounds.Add(v + vi * 3);

			const Vec3 a = VertexOf(v, 0), b = VertexOf(v, 1), c = VertexOf(v, 2);
			const Vec3 cross = Cross(b - a, c - a);
			const double len = Length(cross);
			if (len < 1e-8) continue;
			const Vec3 normal = cross * (1.0 / len);	// 파일에서 삼각형은 바깥을 본다
			tree.planes.push_back({ normal, Dot(normal, a) });
		}
		solid.planeCount = (uint32_t)tree.planes.size() - solid.firstPlane;
		std::memcpy(solid.min, bounds.min, sizeof(solid.min));
		std::memcpy(solid.max, bounds.max, sizeof(solid.max));
		tree.solids.push_back(solid);
	}

	const CMapCollision::Solid* CMapCollision::FindSolidAt(const Tree& tree, const Vec3& point)
	{
		for (const Solid& solid : tree.solids)
		{
			if (point.x < solid.min[0] || point.x > solid.max[0]
				|| point.y < solid.min[1] || point.y > solid.max[1]
				|| point.z < solid.min[2] || point.z > solid.max[2])
			{
				continue;
			}
			bool inside = solid.planeCount > 0;
			for (uint32_t i = 0; i < solid.planeCount; ++i)
			{
				const Plane& plane = tree.planes[solid.firstPlane + i];
				if (Dot(plane.normal, point) > plane.offset)
				{
					inside = false;
					break;
				}
			}
			if (inside) return &solid;
		}
		return nullptr;
	}

	// 구간별 넓이 비용으로 나누는 묶음 상자 나무
	void CMapCollision::Build(Tree& tree)
	{
		const uint32_t n = (uint32_t)tree.tris.size();
		tree.nodes.clear();
		if (n == 0) return;

		struct Item
		{
			Box box;
			float centroid[3];
			uint32_t tri;
		};
		std::vector<Item> items(n);
		for (uint32_t i = 0; i < n; ++i)
		{
			Item& item = items[i];
			item.tri = i;
			for (int vi = 0; vi < 3; ++vi) item.box.Add(tree.tris[i].v + vi * 3);
			for (int axis = 0; axis < 3; ++axis) item.centroid[axis] = (item.box.min[axis] + item.box.max[axis]) * 0.5f;
		}

		tree.nodes.reserve((size_t)n * 2);
		tree.nodes.push_back(Node{});

		struct Work { uint32_t node, begin, end; };
		std::vector<Work> stack;
		stack.push_back({ 0, 0, n });

		const auto writeBox = [](Node& node, const Box& box)
		{
			for (int i = 0; i < 3; ++i)
			{
				node.min[i] = box.min[i] - kNodePad;
				node.max[i] = box.max[i] + kNodePad;
			}
		};

		while (!stack.empty())
		{
			const Work work = stack.back();
			stack.pop_back();
			const uint32_t count = work.end - work.begin;

			Box bounds, centroids;
			for (uint32_t i = work.begin; i < work.end; ++i)
			{
				bounds.Add(items[i].box);
				centroids.Add(items[i].centroid);
			}
			writeBox(tree.nodes[work.node], bounds);

			int bestAxis = -1;
			int bestSplit = 0;
			float bestCost = bounds.Area() * (float)count;
			if (count > kLeafSize)
			{
				for (int axis = 0; axis < 3; ++axis)
				{
					const float lo = centroids.min[axis];
					const float extent = centroids.max[axis] - lo;
					if (extent < 1e-5f) continue;
					const float scale = (float)kBins / extent;

					Box binBox[kBins];
					uint32_t binCount[kBins] = {};
					for (uint32_t i = work.begin; i < work.end; ++i)
					{
						const int bin = std::min(kBins - 1, (int)((items[i].centroid[axis] - lo) * scale));
						binBox[bin].Add(items[i].box);
						++binCount[bin];
					}

					float rightArea[kBins];
					uint32_t rightCount[kBins];
					Box acc;
					uint32_t accCount = 0;
					for (int b = kBins - 1; b > 0; --b)
					{
						acc.Add(binBox[b]);
						accCount += binCount[b];
						rightArea[b] = acc.Area();
						rightCount[b] = accCount;
					}
					acc = Box();
					accCount = 0;
					for (int b = 0; b < kBins - 1; ++b)
					{
						acc.Add(binBox[b]);
						accCount += binCount[b];
						if (accCount == 0 || rightCount[b + 1] == 0) continue;
						const float cost = acc.Area() * (float)accCount + rightArea[b + 1] * (float)rightCount[b + 1];
						if (cost < bestCost)
						{
							bestCost = cost;
							bestAxis = axis;
							bestSplit = b + 1;
						}
					}
				}
			}

			if (bestAxis < 0)
			{
				tree.nodes[work.node].first = work.begin;
				tree.nodes[work.node].count = count;
				continue;
			}

			const float lo = centroids.min[bestAxis];
			const float scale = (float)kBins / (centroids.max[bestAxis] - lo);
			const auto middle = std::partition(items.begin() + work.begin, items.begin() + work.end, [&](const Item& item)
			{
				return std::min(kBins - 1, (int)((item.centroid[bestAxis] - lo) * scale)) < bestSplit;
			});
			const uint32_t mid = (uint32_t)(middle - items.begin());

			const uint32_t left = (uint32_t)tree.nodes.size();
			tree.nodes.push_back(Node{});
			tree.nodes.push_back(Node{});
			tree.nodes[work.node].first = left;
			tree.nodes[work.node].count = 0;
			stack.push_back({ left + 1, mid, work.end });
			stack.push_back({ left, work.begin, mid });
		}

		std::vector<Tri> ordered(n);
		for (uint32_t i = 0; i < n; ++i) ordered[i] = tree.tris[items[i].tri];
		tree.tris.swap(ordered);
		tree.nodes.shrink_to_fit();
	}

	bool CMapCollision::Raycast(const Vec3& start, const Vec3& end, uint32_t blockFlag, RayHit& out) const
	{
		const Vec3 delta = end - start;
		const double length = Length(delta);
		if (length < 1e-9) return false;
		const Vec3 dir = delta * (1.0 / length);

		if (const Solid* solid = FindSolidAt(TreeFor(blockFlag), start))
		{
			out.distance = 0;
			out.normal = dir * -1.0;
			out.object = solid->object;
			out.flags = solid->flags;
			out.startInside = true;
			std::memset(out.triangle, 0, sizeof(out.triangle));
			return true;
		}

		double best = length;
		bool found = false;
		Vec3 bestCross;
		const Tri* bestTri = nullptr;
		bool bestInside = false;

		const Tree& tree = TreeFor(blockFlag);
		if (!tree.nodes.empty())
		{
			const Slab slab(start, dir);
			const double noPad[3] = { 0, 0, 0 };
			uint32_t stack[kMaxStack];
			int top = 0;
			if (slab.Enter(tree.nodes[0], noPad, best) != kInf) stack[top++] = 0;

			while (top > 0)
			{
				const Node& node = tree.nodes[stack[--top]];
				if (node.count > 0)
				{
					for (uint32_t i = 0; i < node.count; ++i)
					{
						const Tri& tri = tree.tris[node.first + i];
						const Vec3 a = VertexOf(tri.v, 0), b = VertexOf(tri.v, 1), c = VertexOf(tri.v, 2);
						double t;
						Vec3 cross;
						if (!RayTriangle(start, dir, best, a, b, c, t, cross)) continue;
						if (Dot(dir, cross) > 0 && (tri.flags & MAP_CULL_BACKFACE)) continue;
						best = t;
						found = true;
						bestCross = cross;
						bestTri = &tri;
					}
					continue;
				}

				const uint32_t left = node.first;
				const double tl = slab.Enter(tree.nodes[left], noPad, best);
				const double tr = slab.Enter(tree.nodes[left + 1], noPad, best);
				if (tl <= tr)
				{
					if (tr != kInf) stack[top++] = left + 1;
					if (tl != kInf) stack[top++] = left;
				}
				else
				{
					if (tl != kInf) stack[top++] = left;
					stack[top++] = left + 1;
				}
			}
		}

		const Prim* bestPrim = nullptr;
		for (const Prim& prim : _prims)
		{
			if (!(prim.flags & blockFlag)) continue;
			double t;
			if (RayCapsule(start, delta, prim.a, prim.b, prim.radius, t) && t * length < best)
			{
				best = t * length;
				found = true;
				bestPrim = &prim;
				bestTri = nullptr;
				bestInside = t == 0;
			}
		}

		if (!found) return false;
		out.distance = best;
		out.startInside = bestInside;
		if (bestPrim)
		{
			const Vec3 at = start + dir * best;
			const Vec3 diff = at - ClosestOnSegment(at, bestPrim->a, bestPrim->b);
			const double len = Length(diff);
			out.normal = len > 1e-9 ? diff * (1.0 / len) : dir * -1.0;
			out.object = bestPrim->object;
			out.flags = bestPrim->flags;
			std::memset(out.triangle, 0, sizeof(out.triangle));
			return true;
		}

		double crossLen = Length(bestCross);
		Vec3 normal = bestCross * (1.0 / crossLen);
		if (Dot(normal, dir) > 0) normal = normal * -1.0;
		out.normal = normal;
		out.object = bestTri->object;
		out.flags = bestTri->flags;
		std::memcpy(out.triangle, bestTri->v, sizeof(out.triangle));
		return true;
	}

	const CMapCollision::Solid* CMapCollision::FindSolidOf(const Tree& tree, uint32_t object)
	{
		// 물체 번호 순서로 넣었다
		const auto it = std::lower_bound(tree.solids.begin(), tree.solids.end(), object,
			[](const Solid& solid, uint32_t value) { return solid.object < value; });
		return (it != tree.solids.end() && it->object == object) ? &*it : nullptr;
	}

	Vec3 CMapCollision::OpposingNormal(const Tree& tree, const Tri& hitTri, const Vec3& point, const Vec3& dir)
	{
		const Vec3 a0 = VertexOf(hitTri.v, 0), b0 = VertexOf(hitTri.v, 1), c0 = VertexOf(hitTri.v, 2);
		const Vec3 cross0 = Cross(b0 - a0, c0 - a0);
		Vec3 result = cross0 * (1.0 / Length(cross0));
		double bestDot = kInf;

		// 볼록 물체 — 맞은 점에서 가까운 평면 중 가장 맞서는 것. 평면은 삼각형과 달리 끝이 없다(Chaos FConvex 와 같음)
		if (const Solid* solid = (hitTri.flags & MAP_SOLID) ? FindSolidOf(tree, hitTri.object) : nullptr)
		{
			const double extent = std::max({ solid->max[0] - solid->min[0], solid->max[1] - solid->min[1], solid->max[2] - solid->min[2] });
			const double search = std::max(kOpposingSearch, extent * 1e-4);
			for (uint32_t i = 0; i < solid->planeCount; ++i)
			{
				const Plane& plane = tree.planes[solid->firstPlane + i];
				if (std::fabs(Dot(plane.normal, point) - plane.offset) >= search) continue;
				const double dot = Dot(plane.normal, dir);
				if (dot < bestDot)
				{
					bestDot = dot;
					result = plane.normal;
				}
			}
			return result;
		}

		// 메시 — 같은 물체의 삼각형 중 맞은 점 1cm 안에 있는 것
		if (tree.nodes.empty()) return result;
		const double search2 = kOpposingSearch * kOpposingSearch;
		uint32_t stack[kMaxStack];
		int top = 0;
		stack[top++] = 0;
		while (top > 0)
		{
			const Node& node = tree.nodes[stack[--top]];
			if (point.x < node.min[0] - kOpposingSearch || point.x > node.max[0] + kOpposingSearch
				|| point.y < node.min[1] - kOpposingSearch || point.y > node.max[1] + kOpposingSearch
				|| point.z < node.min[2] - kOpposingSearch || point.z > node.max[2] + kOpposingSearch)
			{
				continue;
			}
			if (node.count == 0)
			{
				stack[top++] = node.first;
				stack[top++] = node.first + 1;
				continue;
			}
			for (uint32_t i = 0; i < node.count; ++i)
			{
				const Tri& tri = tree.tris[node.first + i];
				if (tri.object != hitTri.object) continue;
				const Vec3 a = VertexOf(tri.v, 0), b = VertexOf(tri.v, 1), c = VertexOf(tri.v, 2);
				const Vec3 diff = ClosestOnTriangle(point, a, b, c) - point;
				if (Dot(diff, diff) >= search2) continue;
				const Vec3 cross = Cross(b - a, c - a);
				const Vec3 normal = cross * (1.0 / Length(cross));
				const double dot = Dot(normal, dir);
				if (dot < bestDot)
				{
					bestDot = dot;
					result = normal;
				}
			}
		}
		return result;
	}

	bool CMapCollision::SweepCapsule(const Vec3& start, const Vec3& end, double radius, double halfHeight, uint32_t blockFlag, CapsuleHit& out) const
	{
		const double axisHalf = std::max(0.0, halfHeight - radius);
		const Vec3 up(0, 0, axisHalf);
		const Vec3 s0 = start - up;
		const Vec3 s1 = start + up;
		const Vec3 delta = end - start;
		const double length = Length(delta);

		double best = 1.0 + 1e-12;		// 끝에 딱 닿는 것까지 인정
		const Tri* bestTri = nullptr;
		const Prim* bestPrim = nullptr;
		bool penetrating = false;

		// 시작 겹침 — 여럿이면 가장 깊은 것을 고른다(Chaos 메시 판정이 「더 음수인 시간」을 고르는 것과 같음)
		double bestDepth = -kInf;
		Vec3 depthNormal(0, 0, 1), depthPoint = start;
		uint32_t depthObject = 0, depthFlags = 0;

		const auto notePenetration = [&](double depth, const Vec3& normal, const Vec3& point, uint32_t object, uint32_t flags)
		{
			if (depth > bestDepth)
			{
				bestDepth = depth;
				depthNormal = normal;
				depthPoint = point;
				depthObject = object;
				depthFlags = flags;
			}
			best = 0;
			penetrating = true;
		};

		// 볼록 물체는 Chaos 가 물체 전체로 푼다 — 평면 중 가장 조금 밀어도 되는 쪽으로 나간다
		const auto solidPenetration = [&](const Tree& inTree, const Solid& solid, Vec3& inOutNormal, double& inOutDepth)
		{
			double shallowest = kInf;
			for (uint32_t pi = 0; pi < solid.planeCount; ++pi)
			{
				const Plane& plane = inTree.planes[solid.firstPlane + pi];
				const double lowest = std::min(Dot(plane.normal, s0), Dot(plane.normal, s1)) - plane.offset;
				const double push = radius - lowest;
				if (push < shallowest)
				{
					shallowest = push;
					inOutNormal = plane.normal;
				}
			}
			if (shallowest != kInf) inOutDepth = shallowest;
		};

		const Tree& tree = TreeFor(blockFlag);
		if (!tree.nodes.empty())
		{
			const Slab slab(start, delta);
			const double pad[3] = { radius, radius, axisHalf + radius };
			uint32_t stack[kMaxStack];
			int top = 0;
			if (slab.Enter(tree.nodes[0], pad, best) != kInf) stack[top++] = 0;

			while (top > 0)
			{
				const Node& node = tree.nodes[stack[--top]];
				if (node.count > 0)
				{
					for (uint32_t i = 0; i < node.count; ++i)
					{
						const Tri& tri = tree.tris[node.first + i];
						const Vec3 a = VertexOf(tri.v, 0), b = VertexOf(tri.v, 1), c = VertexOf(tri.v, 2);

						// Chaos FTriangleMeshSweepVisitor 와 같은 규칙 — 뒷면 쪽으로 가면 겹쳐 있어도 통과,
						// 면과 나란히 가면 처음부터 겹칠 때만 닿는다
						bool parallelToCulled = false;
						if ((tri.flags & MAP_CULL_BACKFACE) && length > 1e-9)
						{
							const Vec3 cross = Cross(b - a, c - a);
							const double along = Dot(cross, delta) / (Length(cross) * length);
							if (along > kCullParallel) continue;
							parallelToCulled = along >= -kCullParallel;
						}

						Vec3 onSeg, onTri;
						const double dist = SegmentTriangleDistance(s0, s1, a, b, c, onSeg, onTri);
						if (dist <= radius)
						{
							Vec3 normal;
							double depth;
							if (dist > 1e-9)
							{
								normal = (onSeg - onTri) * (1.0 / dist);
								depth = radius - dist;
							}
							else
							{
								// 축이 면을 뚫었다 — 덜 움직여도 되는 쪽으로
								const Vec3 cross = Cross(b - a, c - a);
								const Vec3 n = cross * (1.0 / Length(cross));
								const double h0 = Dot(s0 - a, n), h1 = Dot(s1 - a, n);
								const double pushPlus = radius - std::min(h0, h1);
								const double pushMinus = radius + std::max(h0, h1);
								normal = pushPlus <= pushMinus ? n : n * -1.0;
								depth = std::min(pushPlus, pushMinus);
							}
							if (const Solid* solid = (tri.flags & MAP_SOLID) ? FindSolidOf(tree, tri.object) : nullptr)
							{
								solidPenetration(tree, *solid, normal, depth);
							}
							notePenetration(depth, normal, onTri, tri.object, tri.flags);
							continue;
						}
						if (penetrating || parallelToCulled) continue;

						double t = best;
						SweepSegmentTriangle(s0, s1, delta, radius, a, b, c, t);
						if (t < best)
						{
							best = t;
							bestTri = &tri;
						}
					}
					continue;
				}

				const uint32_t left = node.first;
				const double tl = slab.Enter(tree.nodes[left], pad, best);
				const double tr = slab.Enter(tree.nodes[left + 1], pad, best);
				if (tl <= tr)
				{
					if (tr != kInf) stack[top++] = left + 1;
					if (tl != kInf) stack[top++] = left;
				}
				else
				{
					if (tl != kInf) stack[top++] = left;
					if (tr != kInf) stack[top++] = left + 1;
				}
			}
		}

		for (const Prim& prim : _prims)
		{
			if (!(prim.flags & blockFlag)) continue;
			Vec3 c1, c2;
			ClosestSegmentSegment(s0, s1, prim.a, prim.b, c1, c2);
			const double reach = radius + prim.radius;
			const double dist = Length(c1 - c2);
			if (dist <= reach)
			{
				notePenetration(reach - dist, dist > 1e-9 ? (c1 - c2) * (1.0 / dist) : Vec3(0, 0, 1), c2, prim.object, prim.flags);
				continue;
			}
			if (penetrating) continue;
			double t = best;
			SweepSegmentSegment(s0, s1, delta, prim.a, prim.b, reach, t);
			if (t < best)
			{
				best = t;
				bestPrim = &prim;
				bestTri = nullptr;
			}
		}

		// 면만 판정하므로 속이 찬 물체 한가운데서 시작하면 위에서 안 걸린다
		if (!penetrating)
		{
			if (const Solid* solid = FindSolidAt(tree, start))
			{
				Vec3 normal(0, 0, 1);
				double depth = 0;
				solidPenetration(tree, *solid, normal, depth);
				notePenetration(depth, normal, start, solid->object, solid->flags);
			}
		}

		if (penetrating)
		{
			out.time = 0;
			out.distance = 0;
			out.normal = depthNormal;
			out.impactNormal = depthNormal;
			out.impactPoint = depthPoint;
			out.penetrationDepth = bestDepth > 0 ? bestDepth : 0;
			out.object = depthObject;
			out.flags = depthFlags;
			out.startPenetrating = true;
			return true;
		}

		if (!bestTri && !bestPrim) return false;

		if (best > 1.0) best = 1.0;
		const Vec3 at = delta * best;
		const Vec3 p0 = s0 + at, p1 = s1 + at;
		Vec3 onSeg, onShape;
		if (bestTri)
		{
			const Vec3 a = VertexOf(bestTri->v, 0), b = VertexOf(bestTri->v, 1), c = VertexOf(bestTri->v, 2);
			SegmentTriangleDistance(p0, p1, a, b, c, onSeg, onShape);
			out.object = bestTri->object;
			out.flags = bestTri->flags;
		}
		else
		{
			ClosestSegmentSegment(p0, p1, bestPrim->a, bestPrim->b, onSeg, onShape);
			out.object = bestPrim->object;
			out.flags = bestPrim->flags;
		}
		const Vec3 diff = onSeg - onShape;
		const double len = Length(diff);
		out.normal = len > 1e-9 ? diff * (1.0 / len) : (length > 1e-9 ? delta * (-1.0 / length) : Vec3(0, 0, 1));
		out.impactPoint = onShape;
		if (bestTri)
		{
			out.impactNormal = OpposingNormal(tree, *bestTri, onShape, length > 1e-9 ? delta * (1.0 / length) : Vec3(0, 0, -1));
		}
		else
		{
			out.impactNormal = out.normal;		// 구·캡슐은 면이 없다(언리얼도 받은 법선을 그대로 쓴다)
		}
		out.penetrationDepth = 0;
		out.time = best;
		out.distance = best * length;
		out.startPenetrating = false;
		return true;
	}

	bool CMapCollision::OverlapCapsule(const Vec3& center, double radius, double halfHeight, uint32_t blockFlag) const
	{
		const double axisHalf = std::max(0.0, halfHeight - radius);
		const Vec3 up(0, 0, axisHalf);
		const Vec3 s0 = center - up;
		const Vec3 s1 = center + up;

		const Tree& tree = TreeFor(blockFlag);
		if (!tree.nodes.empty())
		{
			const double minBox[3] = { center.x - radius, center.y - radius, s0.z - radius };
			const double maxBox[3] = { center.x + radius, center.y + radius, s1.z + radius };
			uint32_t stack[kMaxStack];
			int top = 0;
			stack[top++] = 0;
			while (top > 0)
			{
				const Node& node = tree.nodes[stack[--top]];
				if (minBox[0] > node.max[0] || maxBox[0] < node.min[0]
					|| minBox[1] > node.max[1] || maxBox[1] < node.min[1]
					|| minBox[2] > node.max[2] || maxBox[2] < node.min[2])
				{
					continue;
				}
				if (node.count == 0)
				{
					stack[top++] = node.first;
					stack[top++] = node.first + 1;
					continue;
				}
				for (uint32_t i = 0; i < node.count; ++i)
				{
					const Tri& tri = tree.tris[node.first + i];
					Vec3 onSeg, onTri;
					if (SegmentTriangleDistance(s0, s1, VertexOf(tri.v, 0), VertexOf(tri.v, 1), VertexOf(tri.v, 2), onSeg, onTri) <= radius)
					{
						return true;
					}
				}
			}
		}

		for (const Prim& prim : _prims)
		{
			if (!(prim.flags & blockFlag)) continue;
			Vec3 c1, c2;
			ClosestSegmentSegment(s0, s1, prim.a, prim.b, c1, c2);
			if (Length(c1 - c2) <= radius + prim.radius) return true;
		}
		return FindSolidAt(tree, center) != nullptr;
	}

	size_t CMapCollision::GetTreeTriangleCount(uint32_t blockFlag) const
	{
		return TreeFor(blockFlag).tris.size();
	}

	size_t CMapCollision::GetTreeNodeCount(uint32_t blockFlag) const
	{
		return TreeFor(blockFlag).nodes.size();
	}

	size_t CMapCollision::GetMemoryBytes() const
	{
		return _move.tris.capacity() * sizeof(Tri) + _move.nodes.capacity() * sizeof(Node)
			+ _weapon.tris.capacity() * sizeof(Tri) + _weapon.nodes.capacity() * sizeof(Node)
			+ (_move.solids.capacity() + _weapon.solids.capacity()) * sizeof(Solid)
			+ (_move.planes.capacity() + _weapon.planes.capacity()) * sizeof(Plane)
			+ _prims.capacity() * sizeof(Prim);
	}
}
