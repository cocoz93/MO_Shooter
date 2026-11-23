#include "Nav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

namespace mo
{
	namespace
	{
		constexpr uint32_t kNavVersion = 1;					// MOBNavDumpSubsystem AppendU32(File, 1)
		constexpr uint32_t kNoNode = 0xFFFFFFFFu;
		constexpr size_t kNodeFixedBytes = 8 + 12 + 4 + 4;	// 종류 · 구역 · 중심 · 꼭짓점 수 · 이웃 수 — 꼭짓점 · 이웃 없는 노드도 이만큼은 있다

		double Length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

		class CNavReader
		{
		public:
			CNavReader(const uint8_t* data, size_t size) : _data(data), _size(size) {}
			bool U32(uint32_t& out) { return Read(&out, sizeof(out)); }
			bool F32(double& out)
			{
				float value = 0;
				if (!Read(&value, sizeof(value))) return false;
				out = value;
				return true;
			}
			bool Vec(Vec3& out) { return F32(out.x) && F32(out.y) && F32(out.z); }
			size_t Remaining() const { return _size - _pos; }

		private:
			bool Read(void* out, size_t bytes)
			{
				if (_size - _pos < bytes) return false;
				std::memcpy(out, _data + _pos, bytes);
				_pos += bytes;
				return true;
			}

			const uint8_t* _data;
			size_t _size;
			size_t _pos = 0;
		};
	}

	ENavLoad ParseNav(const void* data, size_t size, NavMesh& out)
	{
		if (!data || size < 4 || std::memcmp(data, "MONV", 4) != 0) return ENavLoad::BadMagic;
		CNavReader reader(static_cast<const uint8_t*>(data) + 4, size - 4);
		NavMesh mesh;
		uint32_t nodeCount = 0, linkCount = 0;
		if (!reader.U32(mesh.version)) return ENavLoad::Truncated;
		if (mesh.version != kNavVersion) return ENavLoad::BadVersion;
		if (!reader.U32(nodeCount) || !reader.U32(linkCount) || !reader.Vec(mesh.boundsMin) || !reader.Vec(mesh.boundsMax) ||
			!reader.F32(mesh.agentRadius) || !reader.F32(mesh.agentHeight)) return ENavLoad::Truncated;
		// 망가진 머리가 큰 수를 들고 와도 남은 바이트보다 많이 잡지 않는다
		if (nodeCount > reader.Remaining() / kNodeFixedBytes) return ENavLoad::Truncated;
		mesh.nodes.resize(nodeCount);
		size_t seen = 0;
		for (NavNode& node : mesh.nodes)
		{
			uint32_t vertCount = 0, nextCount = 0;
			if (!reader.U32(node.kind) || !reader.U32(node.area) || !reader.Vec(node.center) || !reader.U32(vertCount)) return ENavLoad::Truncated;
			if (vertCount > reader.Remaining() / 12) return ENavLoad::Truncated;
			node.verts.resize(vertCount);
			for (Vec3& v : node.verts)
			{
				if (!reader.Vec(v)) return ENavLoad::Truncated;
			}
			if (!reader.U32(nextCount) || nextCount > reader.Remaining() / 4) return ENavLoad::Truncated;
			node.next.resize(nextCount);
			for (uint32_t& n : node.next)
			{
				if (!reader.U32(n)) return ENavLoad::Truncated;
			}
			seen += nextCount;
		}
		if (reader.Remaining() != 0) return ENavLoad::SizeMismatch;
		if (seen != linkCount) return ENavLoad::BadLinks;
		for (const NavNode& node : mesh.nodes)
		{
			for (const uint32_t n : node.next)
			{
				if (n >= nodeCount) return ENavLoad::BadLinks;
			}
		}
		mesh.linkCount = seen;
		out = std::move(mesh);
		return ENavLoad::Ok;
	}

	const char* NavLoadName(ENavLoad result)
	{
		switch (result)
		{
		case ENavLoad::Ok: return "ok";
		case ENavLoad::BadMagic: return "bad-magic";
		case ENavLoad::BadVersion: return "bad-version";
		case ENavLoad::Truncated: return "truncated";
		case ENavLoad::SizeMismatch: return "size-mismatch";
		case ENavLoad::BadLinks: return "bad-links";
		}
		return "?";
	}

	bool FindNavPath(const NavMesh& mesh, uint32_t from, uint32_t to, std::vector<uint32_t>& outPath, double& outLength)
	{
		outPath.clear();
		outLength = 0;
		const size_t count = mesh.nodes.size();
		if (from >= count || to >= count) return false;
		std::vector<double> best(count, std::numeric_limits<double>::infinity());
		std::vector<uint32_t> previous(count, kNoNode);
		using Entry = std::pair<double, uint32_t>;
		std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
		best[from] = 0;
		open.push({ 0.0, from });
		while (!open.empty())
		{
			const Entry top = open.top();
			open.pop();
			if (top.first > best[top.second]) continue;		// 더 짧은 길로 이미 닫힌 노드의 옛 칸
			if (top.second == to) break;
			const NavNode& node = mesh.nodes[top.second];
			for (const uint32_t n : node.next)
			{
				const double cost = top.first + Length(mesh.nodes[n].center - node.center);
				if (cost < best[n])
				{
					best[n] = cost;
					previous[n] = top.second;
					open.push({ cost, n });
				}
			}
		}
		if (best[to] == std::numeric_limits<double>::infinity()) return false;
		for (uint32_t at = to; at != kNoNode; at = previous[at]) outPath.push_back(at);
		for (size_t i = 0, j = outPath.size() - 1; i < j; ++i, --j) std::swap(outPath[i], outPath[j]);
		outLength = best[to];
		return true;
	}

	// ── 자리 → 바닥 ──

	namespace
	{
		bool InsideXY(const std::vector<Vec3>& verts, double x, double y)
		{
			bool hit = false;
			for (size_t i = 0, j = verts.size() - 1; i < verts.size(); j = i++)
			{
				const Vec3& a = verts[i];
				const Vec3& b = verts[j];
				if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) hit = !hit;
			}
			return hit;
		}
	}

	void NavFloorHeight(const NavNode& node, double x, double y, double& outZ, double& outOutside)
	{
		const std::vector<Vec3>& v = node.verts;
		outZ = node.center.z;
		outOutside = 0;
		if (v.size() < 3)
		{
			outOutside = std::hypot(x - node.center.x, y - node.center.y);
			return;
		}
		if (InsideXY(v, x, y))
		{
			for (size_t i = 1; i + 1 < v.size(); ++i)
			{
				// 부채 삼각형 (0, i, i+1) 무게중심 좌표 — 언리얼 바닥 다각형은 볼록이다(L_Expanse 602개 다, 꼭짓점 3~6)
				const Vec3& p1 = v[0];
				const Vec3& p2 = v[i];
				const Vec3& p3 = v[i + 1];
				const double d = (p2.y - p3.y) * (p1.x - p3.x) + (p3.x - p2.x) * (p1.y - p3.y);
				if (std::fabs(d) < 1e-9) continue;
				const double a = ((p2.y - p3.y) * (x - p3.x) + (p3.x - p2.x) * (y - p3.y)) / d;
				const double b = ((p3.y - p1.y) * (x - p3.x) + (p1.x - p3.x) * (y - p3.y)) / d;
				const double c = 1.0 - a - b;
				if (a >= -1e-9 && b >= -1e-9 && c >= -1e-9)
				{
					outZ = a * p1.z + b * p2.z + c * p3.z;
					return;
				}
			}
			return;		// 안에 들었는데 삼각형 셈 끝자락에 걸림 — 중심 높이
		}
		double best = std::numeric_limits<double>::infinity();
		for (size_t i = 0, j = v.size() - 1; i < v.size(); j = i++)
		{
			const double ex = v[i].x - v[j].x, ey = v[i].y - v[j].y, len2 = ex * ex + ey * ey;
			double t = len2 > 0 ? ((x - v[j].x) * ex + (y - v[j].y) * ey) / len2 : 0.0;
			t = t < 0 ? 0 : (t > 1 ? 1 : t);
			const double dist = std::hypot(x - (v[j].x + ex * t), y - (v[j].y + ey * t));
			if (dist < best)
			{
				best = dist;
				outZ = v[j].z + (v[i].z - v[j].z) * t;
			}
		}
		outOutside = best;
	}

	int FindNavFloor(const NavMesh& mesh, const Vec3& feet, double maxDz, double maxOutside)
	{
		int inside = -1, near = -1;
		double insideDz = std::numeric_limits<double>::infinity(), nearOut = insideDz, nearDz = insideDz;
		for (size_t i = 0; i < mesh.nodes.size(); ++i)
		{
			if (mesh.nodes[i].kind != 0) continue;
			double z = 0, outside = 0;
			NavFloorHeight(mesh.nodes[i], feet.x, feet.y, z, outside);
			const double dz = std::fabs(z - feet.z);
			if (dz > maxDz) continue;
			if (outside == 0)
			{
				if (dz < insideDz)
				{
					insideDz = dz;
					inside = (int)i;
				}
			}
			else if (outside <= maxOutside && (outside < nearOut || (outside == nearOut && dz < nearDz)))
			{
				nearOut = outside;
				nearDz = dz;
				near = (int)i;
			}
		}
		return inside >= 0 ? inside : near;
	}

	// ── 우리 그래프 ──

	const char* NavEdgeName(ENavEdge kind)
	{
		switch (kind)
		{
		case ENavEdge::Walk: return "walk";
		case ENavEdge::Drop: return "drop";
		case ENavEdge::Teleport: return "teleport";
		case ENavEdge::Pad: return "pad";
		}
		return "?";
	}

	const char* NavTryName(ENavTry result)
	{
		switch (result)
		{
		case ENavTry::Ok: return "ok";
		case ENavTry::NotSettled: return "not-settled";
		case ENavTry::SettleEvent: return "settle-event";
		case ENavTry::FarFloor: return "far-floor";
		case ENavTry::NoEvent: return "no-event";
		case ENavTry::OtherDevice: return "other-device";
		case ENavTry::NoLand: return "no-land";
		case ENavTry::NoStop: return "no-stop";
		case ENavTry::Chained: return "chained";
		case ENavTry::BelowKillZ: return "below-kill-z";
		case ENavTry::StartOffMesh: return "start-off-mesh";
		case ENavTry::EndOffMesh: return "end-off-mesh";
		case ENavTry::SameFloor: return "same-floor";
		}
		return "?";
	}

	namespace
	{
		constexpr float kNavTick = 1.f / (float)wire::kTickRate;
		// 봇 각본(BotClient PlanJumpPads · PlanTeleports)과 같은 값 — 리허설이 각본 끝 자리와 같아야 대조가 된다(NavCheck --graph 가 견준다)
		constexpr double kRehearseBackoff = 150.0;		// kJumpPadBackoff — 구 · 상자 + 몸 반지름 밖으로 더 물러서기
		constexpr double kRehearseDropHeight = 150.0;	// 장치 높이 + 150 에서 떨어뜨려 바닥에 멈춘 자리를 놓을 자리로 쓴다
		constexpr double kRehearseFarFloor = 400.0;		// 멈춘 자리가 장치 높이에서 이보다 멀면 그 쪽은 버린다
		constexpr uint32_t kRehearseSettleMax = 180, kRehearseSettleMin = 30;
		constexpr uint32_t kRehearseHold = 30;			// kJumpPadHoldTicks — 다시 놓은 뒤 0.5초 서 있기
		constexpr uint32_t kRehearseWalkMax = 300;		// kJumpPadWalkMax — 쏘이거나 옮겨질 때까지 최대 5초
		constexpr uint32_t kRehearseFlyMax = 600;		// kJumpPadFlyMax — 쏘인 뒤 내려앉을 때까지
		constexpr uint32_t kRehearseSettleAfter = 120;	// kJumpPadSettleMax — 내려앉은 뒤 멈출 때까지
		constexpr uint32_t kRehearseTeleportFly = 600 + 120;	// kTeleportFlyMax
		// 리허설 끝 자리 → 바닥(FindNavFloor) 허용 — L_Expanse 에서 잰 값(NavCheck --graph 「NAVGRAPH floor」 줄): 스폰 20곳 높이 차 최대 35.5 · 밖 0,
		// 받아들인 리허설 점 56개 높이 차 최대 47.2 · 밖 38.9. 버린 리허설 14개(밖 64 ~ 106)를 110 까지 받으면 점프대 이음이 22 → 31 로 늘지만 닿는 바닥 수는 같다
		constexpr double kRehearseFloorDz = 60.0;
		constexpr double kRehearseFloorOutside = 60.0;
		constexpr uint32_t kDropWalkMax = 120;			// 내려가기 — 떨어지기 시작할 때까지 최대 2초

		bool StandingStill(const MoveState& m)
		{
			return m.mode == EMoveMode::Walking && m.velocity.x == 0 && m.velocity.y == 0 && m.velocity.z == 0;
		}

		// 서버 ApplyPlace · 예측 OnPlace 와 같은 차례 — 새 상태 · 걷기 · 바닥 다시 찾기 · 점프대 · 순간이동 칸 다시 셈
		void Place(const NavRehearsal& r, MoveState& state, const Vec3& at)
		{
			state = MoveState();
			state.location = at;
			state.mode = EMoveMode::Walking;
			state.maxSpeed = (float)r.walkSpeed;
			r.sim->RefreshFloor(state);
			ResetLaunchers(state, r.launchers, r.capsuleRadius, r.sim->HalfHeightOf(state));
			ResetTeleports(state, r.teleports, r.capsuleRadius, r.sim->HalfHeightOf(state));
		}

		LauncherTickResult Step(const NavRehearsal& r, MoveState& state, double moveX, double moveY, double yaw)
		{
			return TickWithLaunchers(*r.sim, state, Vec3(moveX, moveY, 0), kNavTick, yaw, r.launchers, r.capsuleRadius, r.teleports);
		}

		bool AnyEvent(const LauncherTickResult& t) { return t.fired >= 0 || t.teleported >= 0; }

		// 놓고 멈출 때까지 서기 — 봇 각본의 「내려앉기」
		ENavTry Settle(const NavRehearsal& r, MoveState& state, const Vec3& at, double yaw)
		{
			Place(r, state, at);
			for (uint32_t i = 0; i < kRehearseSettleMax; ++i)
			{
				if (AnyEvent(Step(r, state, 0, 0, yaw))) return ENavTry::SettleEvent;
				if (i >= kRehearseSettleMin && StandingStill(state)) return ENavTry::Ok;
			}
			return ENavTry::NotSettled;
		}

		void Finish(const NavMesh& mesh, const NavRehearsal& r, NavTry& out)
		{
			const Vec3 down(0, 0, r.capsuleHalfHeight);
			out.from = FindNavFloor(mesh, out.start - down, kRehearseFloorDz, kRehearseFloorOutside);
			out.to = FindNavFloor(mesh, out.end - down, kRehearseFloorDz, kRehearseFloorOutside);
			if (out.from < 0) out.result = ENavTry::StartOffMesh;
			else if (out.to < 0) out.result = ENavTry::EndOffMesh;
			else if (out.from == out.to) out.result = ENavTry::SameFloor;
			else out.result = ENavTry::Ok;
		}
	}

	NavTry RehearseDevice(const NavMesh& mesh, const NavRehearsal& r, ENavEdge kind, int device, int way)
	{
		NavTry out;
		out.kind = kind;
		out.device = device;
		out.way = way;
		Vec3 center, dir, placeDir;
		double reach = 0;
		if (kind == ENavEdge::Pad)
		{
			const Launcher& pad = r.launchers.items[device];
			out.yaw = way * 45.0;
			const double radians = out.yaw * 3.14159265358979323846 / 180.0;
			dir = Vec3((float)std::cos(radians), (float)std::sin(radians), 0);	// 각본은 입력 칸(float)에 담아 쓴다
			placeDir = dir;
			center = pad.sphereCenter;
			reach = pad.sphereRadius;
		}
		else
		{
			const Teleporter& dev = r.teleports.items[device];
			const Vec3 axis = way < 2 ? dev.boxAxisX : dev.boxAxisY;
			const Vec3 d = (way % 2 == 0) ? axis * -1.0 : axis;
			out.yaw = (float)(std::atan2(d.y, d.x) * (180.0 / 3.14159265358979323846));
			dir = Vec3((float)d.x, (float)d.y, 0);
			placeDir = d;
			center = dev.boxCenter;
			reach = way < 2 ? dev.boxExtent.x : dev.boxExtent.y;
		}
		const double back = reach + r.capsuleRadius + kRehearseBackoff;
		MoveState state;
		out.result = Settle(r, state, Vec3(center.x - placeDir.x * back, center.y - placeDir.y * back, center.z + kRehearseDropHeight), out.yaw);
		if (out.result != ENavTry::Ok) return out;
		if (std::fabs(state.location.z - center.z) > kRehearseFarFloor)
		{
			out.result = ENavTry::FarFloor;
			return out;
		}
		out.start = state.location;
		// 본 돌리기 — 그 자리에 다시 놓고 서 있기 → 곧장 걷기
		Place(r, state, out.start);
		for (uint32_t i = 0; i < kRehearseHold; ++i)
		{
			if (AnyEvent(Step(r, state, 0, 0, out.yaw)))
			{
				out.result = ENavTry::SettleEvent;
				return out;
			}
		}
		double yaw = out.yaw;
		bool event = false;
		for (uint32_t i = 0; i < kRehearseWalkMax && !event; ++i)
		{
			const LauncherTickResult t = Step(r, state, dir.x, dir.y, yaw);
			++out.ticks;
			if (!AnyEvent(t)) continue;
			const int hit = kind == ENavEdge::Pad ? t.fired : t.teleported;
			if (hit != device || (kind == ENavEdge::Pad ? t.teleported >= 0 : t.fired >= 0))
			{
				out.result = ENavTry::OtherDevice;
				return out;
			}
			if (t.teleported >= 0 && t.teleportForceYaw) yaw = t.teleportYaw;
			event = true;
		}
		if (!event)
		{
			out.result = ENavTry::NoEvent;
			return out;
		}
		bool flew = false, landed = false;
		if (kind == ENavEdge::Pad)
		{
			// 내려앉을 때까지 입력 그대로(PlanJumpPads)
			for (uint32_t i = 0; i < kRehearseFlyMax && !landed; ++i)
			{
				const LauncherTickResult t = Step(r, state, dir.x, dir.y, yaw);
				++out.ticks;
				if (AnyEvent(t))
				{
					out.result = ENavTry::Chained;
					return out;
				}
				if (state.mode == EMoveMode::Falling) flew = true;
				if (flew && state.mode == EMoveMode::Walking) landed = true;
			}
			if (!landed)
			{
				out.result = ENavTry::NoLand;
				return out;
			}
		}
		// 입력 놓고 멈출 때까지 — 순간이동은 날았다 내려앉아 멈춰야 한다(PlanTeleports)
		const uint32_t settleMax = kind == ENavEdge::Pad ? kRehearseSettleAfter : kRehearseTeleportFly;
		bool stopped = false;
		for (uint32_t i = 0; i < settleMax && !stopped; ++i)
		{
			const LauncherTickResult t = Step(r, state, 0, 0, yaw);
			++out.ticks;
			if (AnyEvent(t))
			{
				out.result = ENavTry::Chained;
				return out;
			}
			if (state.mode == EMoveMode::Falling) flew = true;
			stopped = (kind == ENavEdge::Pad || flew) && StandingStill(state);
		}
		out.end = state.location;
		if (!stopped)
		{
			out.result = ENavTry::NoStop;
			return out;
		}
		if (state.location.z < r.killZ)
		{
			out.result = ENavTry::BelowKillZ;
			return out;
		}
		Finish(mesh, r, out);
		return out;
	}

	NavTry RehearseDrop(const NavMesh& mesh, const NavRehearsal& r, const Vec3& feet, double yawDegrees, int linkNode, int way)
	{
		NavTry out;
		out.kind = ENavEdge::Drop;
		out.device = linkNode;
		out.way = way;
		out.yaw = yawDegrees;
		const double radians = yawDegrees * 3.14159265358979323846 / 180.0;
		const double dx = (float)std::cos(radians), dy = (float)std::sin(radians);
		MoveState state;
		out.result = Settle(r, state, Vec3(feet.x, feet.y, feet.z + r.capsuleHalfHeight + kRehearseDropHeight), yawDegrees);
		if (out.result != ENavTry::Ok) return out;
		out.start = state.location;
		bool falling = false;
		for (uint32_t i = 0; i < kDropWalkMax && !falling; ++i)
		{
			if (AnyEvent(Step(r, state, dx, dy, yawDegrees)))
			{
				out.result = ENavTry::OtherDevice;
				return out;
			}
			++out.ticks;
			falling = state.mode == EMoveMode::Falling;
		}
		if (!falling)
		{
			// 가장자리를 못 벗어남 — 거꾸로(낮은 끝 → 높은 끝) 판의 보통 결과. 멈춘 자리 그대로 바닥을 본다
			out.result = ENavTry::NoEvent;
			out.end = state.location;
			return out;
		}
		bool stopped = false;
		for (uint32_t i = 0; i < kRehearseTeleportFly && !stopped; ++i)
		{
			if (AnyEvent(Step(r, state, 0, 0, yawDegrees)))
			{
				out.result = ENavTry::Chained;
				return out;
			}
			++out.ticks;
			stopped = StandingStill(state);
		}
		out.end = state.location;
		if (!stopped)
		{
			out.result = ENavTry::NoStop;
			return out;
		}
		if (state.location.z < r.killZ)
		{
			out.result = ENavTry::BelowKillZ;
			return out;
		}
		Finish(mesh, r, out);
		return out;
	}

	namespace
	{
		// 같은 두 바닥 사이 같은 종류는 싼 것 하나
		void AddEdge(NavGraph& graph, uint32_t from, const NavEdge& edge)
		{
			for (NavEdge& old : graph.edges[from])
			{
				if (old.to != edge.to || old.kind != edge.kind) continue;
				if (edge.cost < old.cost) old = edge;
				return;
			}
			graph.edges[from].push_back(edge);
		}

		bool EdgeFromTry(const NavTry& t, double walkSpeed, NavEdge& out)
		{
			if (t.result != ENavTry::Ok) return false;
			out.to = (uint32_t)t.to;
			out.kind = t.kind;
			out.device = t.device;
			out.start = t.start;
			out.moveYaw = t.yaw;
			out.end = t.end;
			out.ticks = t.ticks;
			out.cost = t.ticks * (double)kNavTick * walkSpeed;
			return true;
		}

		double Distance(const Vec3& a, const Vec3& b) { return Length(a - b); }
	}

	std::vector<uint32_t> NavWalkGroups(const NavGraph& graph)
	{
		std::vector<uint32_t> parent(graph.ground);
		for (uint32_t i = 0; i < graph.ground; ++i) parent[i] = i;
		auto find = [&parent](uint32_t x)
		{
			while (parent[x] != x)
			{
				parent[x] = parent[parent[x]];
				x = parent[x];
			}
			return x;
		};
		for (uint32_t i = 0; i < graph.ground; ++i)
		{
			for (const NavEdge& e : graph.edges[i])
			{
				if (e.kind == ENavEdge::Walk) parent[find(i)] = find(e.to);
			}
		}
		std::vector<uint32_t> group(graph.ground);
		for (uint32_t i = 0; i < graph.ground; ++i) group[i] = find(i);
		return group;
	}

	std::vector<bool> NavReachable(const NavGraph& graph, const std::vector<uint32_t>& starts, uint32_t skipKinds)
	{
		std::vector<bool> seen(graph.ground, false);
		std::vector<uint32_t> todo;
		for (const uint32_t s : starts)
		{
			if (s >= graph.ground || seen[s]) continue;
			seen[s] = true;
			todo.push_back(s);
		}
		while (!todo.empty())
		{
			const uint32_t i = todo.back();
			todo.pop_back();
			for (const NavEdge& e : graph.edges[i])
			{
				if ((skipKinds >> (uint32_t)e.kind) & 1u) continue;
				if (seen[e.to]) continue;
				seen[e.to] = true;
				todo.push_back(e.to);
			}
		}
		return seen;
	}

	bool BuildNavGraph(const NavMesh& mesh, const NavRehearsal& r, NavGraph& out, NavGraphReport& report)
	{
		NavGraph graph;
		report = NavGraphReport();
		while (graph.ground < mesh.nodes.size() && mesh.nodes[graph.ground].kind == 0) ++graph.ground;
		for (size_t i = graph.ground; i < mesh.nodes.size(); ++i)
		{
			if (mesh.nodes[i].kind == 0) return false;
		}
		graph.edges.resize(graph.ground);
		// 걷기 — 바닥끼리 이웃(연결선 노드를 거치는 이음은 아래 리허설이 대신한다)
		for (uint32_t i = 0; i < graph.ground; ++i)
		{
			for (const uint32_t j : mesh.nodes[i].next)
			{
				if (j >= graph.ground) continue;
				NavEdge e;
				e.to = j;
				e.cost = Distance(mesh.nodes[i].center, mesh.nodes[j].center);
				AddEdge(graph, i, e);
			}
		}
		for (const auto& list : graph.edges) report.walkEdges += list.size();
		const std::vector<uint32_t> walkGroup = NavWalkGroups(graph);

		auto addTry = [&](const NavTry& t)
		{
			report.tries.push_back(t);
			NavEdge e;
			if (!EdgeFromTry(t, r.walkSpeed, e)) return;
			const size_t before = graph.edges[t.from].size();
			AddEdge(graph, (uint32_t)t.from, e);
			if (graph.edges[t.from].size() == before) return;
			(t.kind == ENavEdge::Pad ? report.padEdges : t.kind == ENavEdge::Teleport ? report.teleportEdges : report.dropEdges)++;
		};
		for (size_t k = 0; k < r.launchers.count; ++k)
		{
			for (int way = 0; way < 8; ++way) addTry(RehearseDevice(mesh, r, ENavEdge::Pad, (int)k, way));
		}
		for (size_t k = 0; k < r.teleports.count; ++k)
		{
			for (int way = 0; way < 4; ++way) addTry(RehearseDevice(mesh, r, ENavEdge::Teleport, (int)k, way));
		}

		// 언리얼 연결선 — 끝에 선 몸이 순간이동 칸과 겹치면 순간이동, 아니면 내려가기(높은 끝 → 낮은 끝 리허설 · 거꾸로 한 번)
		const Vec3 up(0, 0, r.capsuleHalfHeight);
		for (uint32_t i = (uint32_t)graph.ground; i < mesh.nodes.size(); ++i)
		{
			const NavNode& link = mesh.nodes[i];
			NavLinkCheck check;
			check.node = i;
			bool teleportLink = false;
			for (const Vec3& end : link.verts)
			{
				for (size_t k = 0; k < r.teleports.count; ++k)
				{
					teleportLink = teleportLink || TeleportOverlaps(r.teleports.items[k], end + up, r.capsuleRadius, r.capsuleHalfHeight);
				}
			}
			check.kind = teleportLink ? ENavEdge::Teleport : ENavEdge::Drop;
			if (!teleportLink && link.verts.size() == 2)
			{
				const Vec3& a = link.verts[0];
				const Vec3& b = link.verts[1];
				const Vec3& high = a.z >= b.z ? a : b;
				const Vec3& low = a.z >= b.z ? b : a;
				const double downYaw = std::atan2(low.y - high.y, low.x - high.x) * (180.0 / 3.14159265358979323846);
				addTry(RehearseDrop(mesh, r, high, downYaw, (int)i, 0));
				const NavTry back = RehearseDrop(mesh, r, low, downYaw + 180.0, (int)i, 1);
				report.tries.push_back(back);
				const bool backEnded = back.result != ENavTry::NotSettled && back.result != ENavTry::SettleEvent && back.result != ENavTry::OtherDevice &&
					back.result != ENavTry::Chained;	// 이 넷은 끝 자리를 안 채운다
				const int highFloor = FindNavFloor(mesh, high, kRehearseFloorDz, kRehearseFloorOutside);
				const Vec3 backFeet = back.end - up;
				const int backFloor = FindNavFloor(mesh, backFeet, kRehearseFloorDz, kRehearseFloorOutside);
				if (backEnded && highFloor >= 0 && backFloor >= 0 && walkGroup[backFloor] == walkGroup[highFloor]) ++report.dropUp;
			}
			// 언리얼이 잇는 (바닥 → 바닥) — 이 연결선을 이웃에 둔 바닥 → 연결선의 이웃 바닥
			for (uint32_t f = 0; f < graph.ground; ++f)
			{
				const std::vector<uint32_t>& fn = mesh.nodes[f].next;
				if (std::find(fn.begin(), fn.end(), i) == fn.end()) continue;
				for (const uint32_t t : link.next)
				{
					if (t >= graph.ground || t == f) continue;
					++check.claims;
					bool ours = false;
					for (uint32_t s = 0; s < graph.ground && !ours; ++s)
					{
						if (walkGroup[s] != walkGroup[f]) continue;
						for (const NavEdge& e : graph.edges[s])
						{
							ours = ours || (e.kind == check.kind && walkGroup[e.to] == walkGroup[t]);
						}
					}
					check.confirmed += ours ? 1 : 0;
				}
			}
			report.links.push_back(check);
		}
		out = std::move(graph);
		return true;
	}

	void NavPlaceBody(const NavRehearsal& rehearsal, MoveState& state, const Vec3& at) { Place(rehearsal, state, at); }

	LauncherTickResult NavTickBody(const NavRehearsal& rehearsal, MoveState& state, double moveX, double moveY, double yawDegrees)
	{
		return Step(rehearsal, state, moveX, moveY, yawDegrees);
	}

	// ── 길 따라 걷기 ──

	bool FindGraphPath(const NavGraph& graph, uint32_t from, uint32_t to, std::vector<NavHop>& outHops, double& outCost)
	{
		outHops.clear();
		outCost = 0;
		if (from >= graph.ground || to >= graph.ground) return false;
		const double inf = std::numeric_limits<double>::infinity();
		std::vector<double> best(graph.ground, inf);
		std::vector<int> prevFloor(graph.ground, -1), prevEdge(graph.ground, -1);
		using Item = std::pair<double, uint32_t>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
		best[from] = 0;
		open.push(Item(0.0, from));
		while (!open.empty())
		{
			const Item top = open.top();
			open.pop();
			if (top.first > best[top.second]) continue;
			if (top.second == to) break;
			const std::vector<NavEdge>& list = graph.edges[top.second];
			for (size_t k = 0; k < list.size(); ++k)
			{
				const double cost = top.first + list[k].cost;
				if (cost >= best[list[k].to]) continue;
				best[list[k].to] = cost;
				prevFloor[list[k].to] = (int)top.second;
				prevEdge[list[k].to] = (int)k;
				open.push(Item(cost, list[k].to));
			}
		}
		if (best[to] == inf) return false;
		NavHop last;
		last.floor = to;
		outHops.push_back(last);
		for (uint32_t at = to; at != from; at = (uint32_t)prevFloor[at])
		{
			NavHop hop;
			hop.floor = (uint32_t)prevFloor[at];
			hop.edge = prevEdge[at];
			outHops.push_back(hop);
		}
		std::reverse(outHops.begin(), outHops.end());
		outCost = best[to];
		return true;
	}

	void NavFloorDistances(const NavGraph& graph, uint32_t from, std::vector<double>& out)
	{
		const double inf = std::numeric_limits<double>::infinity();
		out.assign(graph.ground, inf);
		if (from >= graph.ground) return;
		using Item = std::pair<double, uint32_t>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
		out[from] = 0;
		open.push(Item(0.0, from));
		while (!open.empty())
		{
			const Item top = open.top();
			open.pop();
			if (top.first > out[top.second]) continue;
			for (const NavEdge& e : graph.edges[top.second])
			{
				const double cost = top.first + e.cost;
				if (cost >= out[e.to]) continue;
				out[e.to] = cost;
				open.push(Item(cost, e.to));
			}
		}
	}

	bool NavPortal(const NavNode& a, const NavNode& b, Vec3& outLeft, Vec3& outRight)
	{
		// 언리얼 이웃 바닥은 테두리 한 변을 나눠 갖는다(타일 경계에선 한 변의 일부). 같은 선 위(수평 2cm 안)에 겹친 몫이 1cm 넘는 변 짝 중 가장 긴 겹침
		constexpr double kLineGap = 2.0, kMinOverlap = 1.0;
		const std::vector<Vec3>& av = a.verts;
		const std::vector<Vec3>& bv = b.verts;
		double bestLength = 0;
		Vec3 p, q;
		for (size_t i = 0; i < av.size(); ++i)
		{
			const Vec3& a0 = av[i];
			const Vec3& a1 = av[(i + 1) % av.size()];
			const double ex = a1.x - a0.x, ey = a1.y - a0.y, length = std::hypot(ex, ey);
			if (length < 1e-6) continue;
			const double ux = ex / length, uy = ey / length;
			for (size_t j = 0; j < bv.size(); ++j)
			{
				const Vec3& b0 = bv[j];
				const Vec3& b1 = bv[(j + 1) % bv.size()];
				const double gap0 = std::fabs(ux * (b0.y - a0.y) - uy * (b0.x - a0.x));
				const double gap1 = std::fabs(ux * (b1.y - a0.y) - uy * (b1.x - a0.x));
				if (gap0 > kLineGap || gap1 > kLineGap) continue;
				const double t0 = ux * (b0.x - a0.x) + uy * (b0.y - a0.y);
				const double t1 = ux * (b1.x - a0.x) + uy * (b1.y - a0.y);
				const double lo = std::max(0.0, std::min(t0, t1)), hi = std::min(length, std::max(t0, t1));
				if (hi - lo < kMinOverlap || hi - lo <= bestLength) continue;
				bestLength = hi - lo;
				p = a0 + (a1 - a0) * (lo / length);
				q = a0 + (a1 - a0) * (hi / length);
			}
		}
		if (bestLength <= 0) return false;
		// a 중심에서 볼 때 q 가 p 의 x → y 쪽(외적 > 0)이면 q 가 그 쪽 끝
		const Vec3& c = a.center;
		const bool qLeft = (p.x - c.x) * (q.y - c.y) - (p.y - c.y) * (q.x - c.x) > 0;
		outLeft = qLeft ? q : p;
		outRight = qLeft ? p : q;
		return true;
	}

	namespace
	{
		// (a − o) × (b − o) — 양수면 b 가 o → a 선의 x → y 쪽
		double Cross2(const Vec3& o, const Vec3& a, const Vec3& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }
		bool SameXY(const Vec3& a, const Vec3& b) { return std::fabs(a.x - b.x) < 1e-6 && std::fabs(a.y - b.y) < 1e-6; }
		double DistanceXY(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }
	}

	void NavStringPull(const NavMesh& mesh, const std::vector<uint32_t>& corridor, const Vec3& start, const Vec3& goal, double margin, std::vector<Vec3>& outPoints,
		size_t& outPortalMiss)
	{
		outPoints.clear();
		std::vector<Vec3> lefts(1, start), rights(1, start);
		for (size_t i = 0; i + 1 < corridor.size(); ++i)
		{
			const NavNode& a = mesh.nodes[corridor[i]];
			const NavNode& b = mesh.nodes[corridor[i + 1]];
			Vec3 l, r;
			if (!NavPortal(a, b, l, r))
			{
				++outPortalMiss;
				l = r = (a.center + b.center) * 0.5;
			}
			else if (margin > 0)
			{
				const double length = DistanceXY(l, r);
				if (length <= 2 * margin) l = r = (l + r) * 0.5;
				else
				{
					const Vec3 inward = (r - l) * (margin / length);
					l = l + inward;
					r = r - inward;
				}
			}
			lefts.push_back(l);
			rights.push_back(r);
		}
		lefts.push_back(goal);
		rights.push_back(goal);
		auto add = [&](const Vec3& point)
		{
			if (SameXY(point, start) || (!outPoints.empty() && SameXY(point, outPoints.back()))) return;
			outPoints.push_back(point);
		};
		// 깔때기 — 꼭짓점(apex)에서 왼쪽 · 오른쪽 끝을 맞닿은 선마다 좁히고, 한쪽이 다른 쪽을 넘으면 넘긴 끝이 꺾임점이 되어 거기서 다시 시작
		Vec3 apex = start, left = start, right = start;
		size_t apexIndex = 0, leftIndex = 0, rightIndex = 0;
		for (size_t i = 1; i < lefts.size(); ++i)
		{
			const Vec3& l = lefts[i];
			const Vec3& r = rights[i];
			if (Cross2(apex, right, r) >= 0)		// 오른쪽 끝이 안쪽(x → y 쪽)으로 좁아짐
			{
				if (SameXY(apex, right) || Cross2(apex, left, r) < 0)
				{
					right = r;
					rightIndex = i;
				}
				else
				{
					add(left);
					apex = left;
					apexIndex = leftIndex;
					right = left = apex;
					rightIndex = leftIndex = apexIndex;
					i = apexIndex;
					continue;
				}
			}
			if (Cross2(apex, left, l) <= 0)			// 왼쪽 끝이 안쪽(반대 쪽)으로 좁아짐
			{
				if (SameXY(apex, left) || Cross2(apex, right, l) > 0)
				{
					left = l;
					leftIndex = i;
				}
				else
				{
					add(right);
					apex = right;
					apexIndex = rightIndex;
					right = left = apex;
					rightIndex = leftIndex = apexIndex;
					i = apexIndex;
					continue;
				}
			}
		}
		add(goal);
		if (outPoints.empty()) outPoints.push_back(goal);
	}

	const char* NavFollowName(ENavFollow state)
	{
		switch (state)
		{
		case ENavFollow::Walking: return "walking";
		case ENavFollow::Edge: return "edge";
		case ENavFollow::Flying: return "flying";
		case ENavFollow::Arrived: return "arrived";
		case ENavFollow::NoPath: return "no-path";
		case ENavFollow::GaveUp: return "gave-up";
		}
		return "?";
	}

	CNavFollower::CNavFollower(const NavMesh& mesh, const NavGraph& graph, double halfHeight, const NavFollowParams& params)
		: _mesh(mesh), _graph(graph), _halfHeight(halfHeight), _p(params), _groups(NavWalkGroups(graph))
	{
	}

	void CNavFollower::SetGoal(const Vec3& goal, bool resetStuck)
	{
		_goal = goal;
		if (resetStuck) _stucks = 0;
		if (Busy()) return;		// 내려앉으면 Flying 이 다시 찾기를 건다
		// 걷던 중 같은 목표 다시 찾기 — 막힘 틱 셈을 이어 가야 0.5초 갱신 아래서도 1초 막힘이 난다. 막힘 · 내려앉음으로 이미 다시 찾기가 걸렸으면 그쪽 대로(0 부터)
		_keepProgress = !resetStuck && _state == ENavFollow::Walking && !_needPlan;
		_state = ENavFollow::Walking;
		_needPlan = true;
	}

	void CNavFollower::Reset(const Vec3& goal)
	{
		_goal = goal;
		_stucks = 0;
		_keepProgress = false;
		_flyEdgeTo = -1;
		_state = ENavFollow::Walking;
		_needPlan = true;
	}

	int CNavFollower::FloorOf(const Vec3& capsuleCenter) const
	{
		const Vec3 feet = capsuleCenter - Vec3(0, 0, _halfHeight);
		const int floor = FindNavFloor(_mesh, feet, _p.floorDz, _p.floorOutside);
		return floor >= 0 ? floor : FindNavFloor(_mesh, feet, _p.floorFarDz, _p.floorFarOutside);
	}

	double CNavFollower::EdgeStartDistance(const Vec3& at) const
	{
		if (_state != ENavFollow::Walking || _needPlan || _edgeIndex < 0 || _points.empty()) return std::numeric_limits<double>::infinity();
		return DistanceXY(at, _points.back());
	}

	void CNavFollower::Plan(const NavObserve& o)
	{
		_needPlan = false;
		++_counters.plans;
		_planStart = o.location;
		_corridor.clear();
		_points.clear();
		_index = 0;
		_edgeFloor = _edgeIndex = -1;
		_bestDistance = std::numeric_limits<double>::infinity();
		if (!_keepProgress) _progressTicks = 0;
		_keepProgress = false;
		_state = ENavFollow::Walking;
		if (_p.straight)
		{
			_points.push_back(_goal);
			return;
		}
		const Vec3 down(0, 0, _halfHeight);
		auto floorOf = [this](const Vec3& feet)
		{
			int floor = FindNavFloor(_mesh, feet, _p.floorDz, _p.floorOutside);
			if (floor >= 0) return floor;
			floor = FindNavFloor(_mesh, feet, _p.floorFarDz, _p.floorFarOutside);
			if (floor >= 0) ++_counters.farFloor;
			return floor;
		};
		const int from = floorOf(o.location - down);
		const int to = floorOf(_goal - down);
		if (from < 0 || to < 0 || (uint32_t)from >= _graph.ground || (uint32_t)to >= _graph.ground)
		{
			++_counters.noFloor;
			_state = ENavFollow::NoPath;
			return;
		}
		std::vector<NavHop> hops;
		double cost = 0;
		if (!FindGraphPath(_graph, (uint32_t)from, (uint32_t)to, hops, cost))
		{
			++_counters.noPath;
			_state = ENavFollow::NoPath;
			return;
		}
		// 이번 구간 = 첫 걷기 밖 이음까지(그 이음을 타면 내려앉은 자리에서 다시 찾는다)
		size_t k = 0;
		_corridor.push_back(hops[0].floor);
		while (k + 1 < hops.size() && _graph.edges[hops[k].floor][hops[k].edge].kind == ENavEdge::Walk)
		{
			++k;
			_corridor.push_back(hops[k].floor);
		}
		Vec3 target = _goal;
		if (k + 1 < hops.size())
		{
			_edgeFloor = (int)hops[k].floor;
			_edgeIndex = hops[k].edge;
			target = _graph.edges[_edgeFloor][_edgeIndex].start;
		}
		if (_p.noPull) _points.assign(1, target);
		else NavStringPull(_mesh, _corridor, o.location, target, _p.margin, _points, _counters.portalMiss);
	}

	void CNavFollower::EnterFlying(EFlyInput input, const NavObserve& o)
	{
		_state = ENavFollow::Flying;
		_flyInput = input;
		_flyTicks = 0;
		_flyEdgeTo = -1;
		_flew = o.mode == EMoveMode::Falling;
	}

	void CNavFollower::Steer(const Vec3& from, const Vec3& to, NavCommand& out)
	{
		const double dx = to.x - from.x, dy = to.y - from.y, length = std::hypot(dx, dy);
		if (length < 1e-6) return;
		out.moveX = (float)(dx / length);	// 와이어 입력 칸(float) 값
		out.moveY = (float)(dy / length);
		_yaw = (float)(std::atan2(dy, dx) * (180.0 / 3.14159265358979323846));
		out.yaw = _yaw;
	}

	void CNavFollower::SteerYaw(double yawDegrees, NavCommand& out)
	{
		const double radians = yawDegrees * 3.14159265358979323846 / 180.0;
		out.moveX = (float)std::cos(radians);	// 리허설 · 봇 각본처럼 입력 칸(float) 값
		out.moveY = (float)std::sin(radians);
		_yaw = (float)yawDegrees;
		out.yaw = _yaw;
	}

	ENavFollow CNavFollower::Step(const NavObserve& o, NavCommand& out)
	{
		out = NavCommand();
		out.yaw = _yaw;
		if (_state == ENavFollow::Arrived || _state == ENavFollow::GaveUp || _state == ENavFollow::NoPath) return _state;
		const bool event = o.fired >= 0 || o.teleported >= 0;
		if (_state == ENavFollow::Flying)
		{
			_flew = _flew || o.mode == EMoveMode::Falling;
			++_flyTicks;
			const bool landed = o.mode == EMoveMode::Walking && (_flew || _flyTicks >= _p.noFlightTicks);
			if (!landed)
			{
				if (_flyTicks > _p.flyMax)
				{
					++_counters.flyTimeouts;
					_state = ENavFollow::GaveUp;
					return _state;
				}
				if (_flyInput == EFlyInput::Edge && _edgeIndex >= 0) SteerYaw(_graph.edges[_edgeFloor][_edgeIndex].moveYaw + _p.edgeYawOffset, out);
				else if (_flyInput == EFlyInput::Toward && _index < _points.size()) Steer(o.location, _points[_index], out);
				return _state;
			}
			++_counters.landRepaths;
			if (_flyEdgeTo >= 0)
			{
				const int floor = FloorOf(o.location);
				if (floor < 0 || (uint32_t)floor >= _graph.ground || _groups[floor] != _groups[_flyEdgeTo]) ++_counters.edgeMisses[(uint32_t)_flyEdgeKind];
			}
			_needPlan = true;
		}
		if (_needPlan) Plan(o);
		if (_state == ENavFollow::NoPath) return _state;

		if (_state == ENavFollow::Walking)
		{
			if (event)
			{
				// 길에 없는 장치를 밟음 — 내려앉은 뒤 다시 찾는다
				++_counters.unplannedEvents;
				EnterFlying(o.fired >= 0 ? EFlyInput::Toward : EFlyInput::None, o);
				return _state;
			}
			if (o.mode == EMoveMode::Falling)
			{
				++_counters.unplannedFalls;
				EnterFlying(EFlyInput::Toward, o);
				if (_index < _points.size()) Steer(o.location, _points[_index], out);
				return _state;
			}
			while (_index < _points.size())
			{
				const Vec3& point = _points[_index];
				const Vec3& prev = _index == 0 ? _planStart : _points[_index - 1];
				const double distance = DistanceXY(o.location, point);
				const bool passed = distance < 100.0 && (point.x - prev.x) * (o.location.x - point.x) + (point.y - prev.y) * (o.location.y - point.y) > 0;
				if (distance > _p.arrive && !passed) break;
				++_index;
				_bestDistance = std::numeric_limits<double>::infinity();
				_progressTicks = 0;
			}
			if (_index >= _points.size())
			{
				if (_edgeIndex < 0)
				{
					_state = ENavFollow::Arrived;
					return _state;
				}
				_state = ENavFollow::Edge;
				_edgeTicks = 0;
			}
			else
			{
				// 나아감 = 목표 꺾임점까지 거리가 stuckDistance 넘게 줄어듦(새 꺾임점의 첫 거리는 나아감이 아니다 — 막힌 채 다시 찾기만 되풀이하면 포기하게)
				const double distance = DistanceXY(o.location, _points[_index]);
				if (_bestDistance == std::numeric_limits<double>::infinity())
				{
					_bestDistance = distance;	// 틱 셈은 Plan · 꺾임점 넘김이 0 으로 둔다(봇의 0.5초 다시 찾기는 이어 간다)
				}
				else if (distance < _bestDistance - _p.stuckDistance)
				{
					_bestDistance = distance;
					_progressTicks = 0;
					_stucks = 0;
				}
				else if (++_progressTicks > _p.stuckTicks)
				{
					++_counters.stuckRepaths;
					if (++_stucks > _p.giveUpStucks)
					{
						_state = ENavFollow::GaveUp;
						return _state;
					}
					_needPlan = true;
				}
				Steer(o.location, _points[_index], out);
				return _state;
			}
		}

		// 걷기 밖 이음 — 리허설처럼 그 방향으로 곧장 걸어 사건(쏘임 · 옮김 · 떨어짐)을 기다린다
		const NavEdge& edge = _graph.edges[_edgeFloor][_edgeIndex];
		const bool hit = edge.kind == ENavEdge::Pad ? o.fired == edge.device : edge.kind == ENavEdge::Teleport ? o.teleported == edge.device : o.mode == EMoveMode::Falling;
		if (hit)
		{
			++_counters.edges[(uint32_t)edge.kind];
			_stucks = 0;
			EnterFlying(edge.kind == ENavEdge::Pad ? EFlyInput::Edge : EFlyInput::None, o);
			_flyEdgeTo = (int)edge.to;
			_flyEdgeKind = edge.kind;
			if (_flyInput == EFlyInput::Edge) SteerYaw(edge.moveYaw + _p.edgeYawOffset, out);
			return _state;
		}
		if (event || o.mode == EMoveMode::Falling)
		{
			++_counters.unplannedEvents;
			++_counters.edgeMisses[(uint32_t)edge.kind];
			EnterFlying(EFlyInput::None, o);
			return _state;
		}
		if (++_edgeTicks > _p.edgeWalkMax)
		{
			++_counters.edgeTimeouts;
			++_counters.edgeMisses[(uint32_t)edge.kind];
			if (++_stucks > _p.giveUpStucks)
			{
				_state = ENavFollow::GaveUp;
				return _state;
			}
			_needPlan = true;
			_state = ENavFollow::Walking;
			return _state;
		}
		SteerYaw(edge.moveYaw + _p.edgeYawOffset, out);
		return _state;
	}
}
