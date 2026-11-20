#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Lyra 에서 뽑은 맵 충돌(.mocl)을 읽어 광선·캡슐 판정을 한다.
// - 왜: 서버는 언리얼 없이 돌아야 한다. 벽은 언리얼 물리가 들고 있던 모양 그대로 파일로 받는다.
// - 파일 형식은 Lyra 플러그인 MOBMapDumpSubsystem.h 머리 주석이 정본이다.
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다. 불러온 뒤에는 읽기만 하므로 여러 스레드가 함께 질의해도 된다.
namespace mo
{
	struct Vec3
	{
		double x = 0, y = 0, z = 0;
		Vec3() = default;
		Vec3(double inX, double inY, double inZ) : x(inX), y(inY), z(inZ) {}
		Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		Vec3 operator*(double s) const { return { x * s, y * s, z * s }; }
	};

	enum EMapFlag : uint32_t
	{
		MAP_BLOCK_MOVE = 1,			// 캡슐(이동)을 막는다 — 언리얼의 단순 충돌
		MAP_BLOCK_WEAPON = 2,		// 무기 광선을 막는다 — 언리얼의 복잡 충돌
		MAP_MOVABLE = 4,			// 움직일 수 있다고 표시된 물체
		MAP_CULL_BACKFACE = 8,		// 뒷면으로 오는 광선·캡슐은 통과 — 한 면짜리 메시. 캡슐은 처음부터 겹쳐 있어도 통과한다(Chaos 와 같음)
		MAP_SOLID = 16,				// 속이 찬 볼록 물체 — 안에서 쏜 광선은 거리 0 에 맞는다
		MAP_BLOCK_GRENADE = 32,		// 수류탄 몸(WorldDynamic)을 막는다 — 단순 충돌. 이동과 거의 같지만 사람만 막는 벽 · 수류탄만 막는 물체가 있다(G-0-d)
		MAP_BLOCK_BLAST = 64,		// 폭발 선(Weapon_Capsule 채널, 복잡 충돌)을 막는다. 무기 광선과 거의 같다(G-0-d)
	};

	enum class EMapLoad
	{
		Ok,
		TooSmall,
		BadMagic,
		BadVersion,
		Truncated,
		BadKind,
		CountMismatch,
	};

	struct RayHit
	{
		double distance = 0;		// 시작점에서 맞은 곳까지(cm)
		Vec3 normal;				// 광선 쪽을 보는 단위 법선
		uint32_t object = 0;		// 파일의 물체 번호
		uint32_t flags = 0;
		bool startInside = false;	// 속이 찬 물체 안에서 시작함
		float triangle[9] = {};		// 맞은 삼각형(구·캡슐이면 0)
	};

	struct CapsuleHit
	{
		double time = 0;				// 0(시작)~1(끝)
		double distance = 0;			// 시작점에서 멈춘 곳까지(cm)
		Vec3 normal;					// 맞은 곳 → 캡슐 축 방향 단위 벡터. 시작 겹침이면 빠져나갈 방향
		Vec3 impactPoint;				// 맞은 점
		Vec3 impactNormal;				// 맞은 면의 법선 — 언리얼 FHitResult::ImpactNormal 과 같은 규칙으로 고른다
		double penetrationDepth = 0;	// 시작 겹침일 때 빠져나갈 거리
		uint32_t object = 0;
		uint32_t flags = 0;
		bool startPenetrating = false;	// 시작부터 겹쳐 있음
	};

	class CMapCollision
	{
	public:
		// 다시 부르면 이전 내용을 버린다
		EMapLoad LoadFromMemory(const void* data, size_t size);

		// blockFlag = MAP_BLOCK_* 중 하나(이동 · 무기 · 수류탄 · 폭발 선). 가장 가까운 것 하나를 돌려준다
		bool Raycast(const Vec3& start, const Vec3& end, uint32_t blockFlag, RayHit& out) const;

		// 똑바로 선 캡슐을 중심 기준으로 start → end 로 옮겨 처음 닿는 곳을 찾는다
		// halfHeight = 캡슐 전체 높이의 절반(언리얼 GetScaledCapsuleHalfHeight 와 같은 뜻)
		bool SweepCapsule(const Vec3& start, const Vec3& end, double radius, double halfHeight, uint32_t blockFlag, CapsuleHit& out) const;

		// 기운 캡슐 — axis = 캡슐 축의 단위 벡터(언리얼 캡슐 회전의 위쪽). 쓸기 동안 회전은 그대로다(언리얼 쓸기도 시작 회전 하나)
		// 위 함수는 axis = (0, 0, 1) 인 이 함수다
		bool SweepCapsule(const Vec3& start, const Vec3& end, const Vec3& axis, double radius, double halfHeight, uint32_t blockFlag, CapsuleHit& out) const;

		// 시작 겹침을 물체마다 하나씩(그 물체에서 가장 깊은 것). out 은 위와 같다(물체를 가리지 않고 가장 깊은 것)
		// 언리얼은 물체(컴포넌트)마다 맞음을 따로 받아 MoveComponentImpl 이 빠져나가는 겹침을 뺀 나머지에서 고른다 — 가장 깊은 것 하나만 받으면
		// 그것이 빠지는 겹침일 때 다른 물체 겹침을 놓친다(L-1-a 기록 l1a1 p_crease_* — 바닥 · 지붕 상자에 함께 겹쳐 바닥 아래로 내려감)
		struct Penetrations
		{
			static constexpr size_t kMax = 8;
			CapsuleHit hits[kMax];
			size_t count = 0;
			size_t dropped = 0;		// kMax 를 넘어 못 담은 물체 수
		};
		bool SweepCapsule(const Vec3& start, const Vec3& end, const Vec3& axis, double radius, double halfHeight, uint32_t blockFlag, CapsuleHit& out, Penetrations* perObject) const;

		// 이 자리에 똑바로 선 캡슐을 두면 무언가와 겹치나. 뒷면 규칙을 쓰지 않는다(언리얼 겹침 검사와 같음)
		bool OverlapCapsule(const Vec3& center, double radius, double halfHeight, uint32_t blockFlag) const;

		uint32_t GetObjectCount() const { return _objectCount; }
		uint32_t GetTriangleCount() const { return _triangleCount; }			// 파일 기준
		uint32_t GetDegenerateCount() const { return _degenerateCount; }		// 넓이 0 이라 뺀 삼각형
		uint32_t GetPrimitiveCount() const { return (uint32_t)_prims.size(); }	// 구·캡슐
		size_t GetTreeTriangleCount(uint32_t blockFlag) const;	// 그 깃발이 쓰는 나무(단순 · 복잡) 기준 — 같은 나무의 다른 깃발 삼각형도 센다
		size_t GetTreeNodeCount(uint32_t blockFlag) const;
		size_t GetMemoryBytes() const;

	private:
		struct Node
		{
			float min[3];
			uint32_t first;		// 잎이면 첫 삼각형, 아니면 왼쪽 자식(오른쪽은 +1)
			float max[3];
			uint32_t count;		// 0 이면 잎이 아님
		};

		struct Tri
		{
			float v[9];
			uint32_t object;
			uint32_t flags;
		};

		struct Prim
		{
			Vec3 a, b;			// 구면 a == b
			double radius;
			uint32_t object;
			uint32_t flags;
		};

		// 속이 찬 볼록 물체. 삼각형은 면만 알려 주므로 한가운데 있는지는 면의 평면으로 따로 잰다
		struct Solid
		{
			float min[3];
			float max[3];
			uint32_t firstPlane;
			uint32_t planeCount;
			uint32_t object;
			uint32_t flags;
		};

		struct Plane
		{
			Vec3 normal;
			double offset;
		};

		struct Tree
		{
			std::vector<Node> nodes;
			std::vector<Tri> tris;
			std::vector<Solid> solids;
			std::vector<Plane> planes;
		};

		void Clear();
		// 나무는 모양 갈래로 둘 — 단순 충돌(이동 · 수류탄) · 복잡 충돌(무기 · 폭발 선). 한 나무에 깃발이 다른 물체가 섞이므로 질의는 깃발로 거른다
		// - 왜 깃발마다 나무를 두지 않나: expanse 는 둘이 거의 같아(다른 물체 53 · 1개) 120만 삼각형 나무가 둘 더 생긴다
		static constexpr uint32_t kSimpleFlags = MAP_BLOCK_MOVE | MAP_BLOCK_GRENADE;
		static constexpr uint32_t kComplexFlags = MAP_BLOCK_WEAPON | MAP_BLOCK_BLAST;
		const Tree& TreeFor(uint32_t blockFlag) const { return (blockFlag & kSimpleFlags) ? _simple : _complex; }
		static void Build(Tree& tree);
		static void BuildGrouped(Tree& tree, uint32_t mainFlag);
		static void AddSolid(Tree& tree, const uint8_t* triangles, uint32_t count, uint32_t object, uint32_t flags);
		static const Solid* FindSolidAt(const Tree& tree, const Vec3& point, uint32_t blockFlag);
		static const Solid* FindSolidOf(const Tree& tree, uint32_t object);
		// 맞은 점 근처에서 이동 방향과 가장 맞서는 면의 법선(언리얼 FindFaceIndex 와 같은 규칙)
		static Vec3 OpposingNormal(const Tree& tree, const Tri& hitTri, const Vec3& point, const Vec3& dir);

		Tree _simple;
		Tree _complex;
		std::vector<Prim> _prims;
		uint32_t _objectCount = 0;
		uint32_t _triangleCount = 0;
		uint32_t _degenerateCount = 0;
	};
}
