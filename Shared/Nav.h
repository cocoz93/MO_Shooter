#pragma once

#include "JumpPads.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// 봇 길찾기(bot-nav-plan) — 언리얼이 계산한 걸을 수 있는 바닥(L_Expanse nav_expanse.monv — Lyra 다리 MOBNavDumpSubsystem 이 뽑는다, Epic 맵 값이라 리포 밖)을
// 읽고 바닥 사이 가장 짧은 길을 찾는다. 봇과 검사 도구(Tools/NavCheck)가 같이 쓴다. 서버와 다리는 안 부른다(bot-nav-plan 「목표 구조」).
// 언리얼 안에서도 컴파일돼야 하므로 예외 · RTTI 를 쓰지 않는다(Shared 규칙). 읽은 뒤에는 읽기만 하므로 여러 스레드가 함께 길을 찾아도 된다.
namespace mo
{
	struct NavNode
	{
		uint32_t kind = 0;				// 0 = 바닥 다각형 · 1 = 바닥 사이 연결선(언리얼 off-mesh link — L_Expanse 는 순간이동 3 · 내려가는 길 2, bot-nav-plan 「지금」 2)
		uint32_t area = 0;				// 언리얼 길찾기 구역 번호(읽기만 — 길 셈에 안 쓴다)
		Vec3 center;
		std::vector<Vec3> verts;		// 꼭짓점(언리얼 GetPolyVerts 차례)
		std::vector<uint32_t> next;		// 이 노드에서 갈 수 있는 노드(언리얼 GetPolyNeighbors — 한 방향 연결선은 한쪽에만 있다)
	};

	struct NavMesh
	{
		uint32_t version = 0;
		Vec3 boundsMin;					// 바닥 꼭짓점을 감싼 상자
		Vec3 boundsMax;
		double agentRadius = 0;			// 언리얼 길찾기 몸 반지름 · 키(L_Expanse 35 · 144 — 반지름은 우리 몸 MoveParams::capsuleRadius 35 와 같다)
		double agentHeight = 0;
		std::vector<NavNode> nodes;
		size_t linkCount = 0;			// 이웃 칸 수 합(머리 값과 같아야 읽힌다)
	};

	enum class ENavLoad
	{
		Ok,
		BadMagic,		// "MONV" 가 아님
		BadVersion,		// 판 1 이 아님
		Truncated,		// 머리 · 노드가 파일 끝을 넘음
		SizeMismatch,	// 노드를 다 읽었는데 바이트가 남음
		BadLinks,		// 이웃 칸 수 합이 머리 값과 다름 · 없는 노드 번호
	};

	// 파일 꼴 = MOBNavDumpSubsystem 이 쓰는 것(Tools/LyraPilot/check_nav.py load 와 같은 읽기) — "MONV" · 판 · 노드 수 · 이웃 칸 수(u32) · 상자 최소 · 최대(f32 3)
	//   · 몸 반지름 · 키(f32) · 노드마다 [종류 · 구역(u32) · 중심(f32 3) · 꼭짓점 수(u32) · 꼭짓점(f32 3 씩) · 이웃 수(u32) · 이웃(u32 씩)]. 작은 끝 차례(x86 그대로 읽는다).
	// 틀리면 out 을 안 건드린다
	ENavLoad ParseNav(const void* data, size_t size, NavMesh& out);
	const char* NavLoadName(ENavLoad result);

	// 가장 짧은 길 — 노드 중심 사이 곧은 거리를 더한 다익스트라. 못 가면 false · outPath 비움 · outLength 0. outPath = from … to(노드 번호, from == to 면 하나)
	bool FindNavPath(const NavMesh& mesh, uint32_t from, uint32_t to, std::vector<uint32_t>& outPath, double& outLength);

	// ── 자리 → 바닥 (n-2) ──

	// 바닥 다각형의 (x, y) 자리 높이 — 안에 들면 꼭짓점 부채 삼각형으로 보간하고 outOutside = 0, 밖이면 가장 가까운 테두리 점의 높이와 그 점까지 수평 거리
	void NavFloorHeight(const NavNode& node, double x, double y, double& outZ, double& outOutside);
	// 발 자리(캡슐 중심 − 반높이)가 선 바닥 노드(종류 0) — 수평으로 안에 드는 바닥 중 그 자리 높이와 차가 maxDz 안인 가장 가까운 것,
	// 없으면 높이 차가 maxDz 안이면서 테두리에서 maxOutside 안인 바닥 중 테두리가 가장 가까운 것. 다 없으면 -1
	int FindNavFloor(const NavMesh& mesh, const Vec3& feet, double maxDz, double maxOutside);

	// ── 우리 그래프 (n-2) — 언리얼 바닥 이웃 + 우리 이동 셈(서버와 같은 TickWithLaunchers)으로 밟아 본 점프대 · 순간이동 · 내려가기 ──

	enum class ENavEdge : uint8_t { Walk, Drop, Teleport, Pad };
	const char* NavEdgeName(ENavEdge kind);

	struct NavEdge
	{
		uint32_t to = 0;
		double cost = 0;				// 걷기 셈 cm — 걷기 = 두 바닥 중심 사이 거리 · 나머지 = 걷기 시작부터 멈출 때까지 틱 × 걷기 속도 한 틱 몫
		ENavEdge kind = ENavEdge::Walk;
		int device = -1;				// 점프대 · 순간이동 표 차례 · 내려가기는 언리얼 연결선 노드 번호 · 걷기 -1
		// 걷기 밖 이음을 타는 법(리허설 그대로) — start(캡슐 중심)에 멈춰 서서 moveYaw 쪽으로 곧장 걷는다. 점프대는 내려앉을 때까지 입력을 그대로 두고(봇 점프대 각본과 같다),
		// 순간이동은 옮겨지면 · 내려가기는 떨어지기 시작하면 입력을 놓는다(순간이동 각본과 같다) → end 에 멈춘다
		Vec3 start;
		double moveYaw = 0;
		Vec3 end;
		uint32_t ticks = 0;				// 걷기 시작부터 멈출 때까지
	};

	struct NavGraph
	{
		size_t ground = 0;							// 바닥 노드 수 — 그래프 노드 번호 = 바닥 노드 번호(파일에서 바닥이 연결선보다 앞에 몰려 있어야 한다)
		std::vector<std::vector<NavEdge>> edges;	// 바닥마다 나가는 이음
	};

	// 리허설 재료
	struct NavRehearsal
	{
		CMoveSim* sim = nullptr;		// 맵 · 이동 계수(상태는 안 든다). 다른 사람 캡슐은 비워 둘 것
		double capsuleRadius = 0;
		double capsuleHalfHeight = 0;
		double walkSpeed = 0;			// 놓을 때 MoveState::maxSpeed · 이음 비용(cm/s)
		double killZ = 0;				// 이 아래로 떨어지는 이음은 버린다(MatchRules::fallKillZ)
		LauncherSet launchers;
		TeleportSet teleports;
	};

	enum class ENavTry : uint8_t { Ok, NotSettled, SettleEvent, FarFloor, NoEvent, OtherDevice, NoLand, NoStop, Chained, BelowKillZ, StartOffMesh, EndOffMesh, SameFloor };
	const char* NavTryName(ENavTry result);

	struct NavTry
	{
		ENavEdge kind = ENavEdge::Pad;
		int device = -1;
		int way = 0;					// 점프대 = 방향 번호(요 45도씩) · 순간이동 = 상자 축 쪽(0 +X · 1 −X · 2 +Y · 3 −Y, 봇 순간이동 각본과 같다) · 내려가기 = 0 내려감 · 1 거꾸로
		double yaw = 0;
		ENavTry result = ENavTry::NotSettled;
		Vec3 start, end;				// 멈춰 선 자리 · 멈춘 자리(캡슐 중심)
		uint32_t ticks = 0;
		int from = -1, to = -1;			// 선 바닥 · 멈춘 바닥(노드 번호)
	};

	// 장치 하나를 한 쪽으로 밟아 본다 — 봇 각본(PlanJumpPads · PlanTeleports 의 바닥 갈래)과 같은 차례: 장치 뒤쪽(구 · 상자 + 몸 반지름 + 150cm) 높이 + 150 에 놓고 멈출 때까지 →
	// 그 자리에 다시 놓고 30틱 서기 → 곧장 걷기(그 장치가 쏘거나 옮길 때까지) → 점프대는 내려앉을 때까지 입력 그대로 · 순간이동은 놓고 → 멈출 때까지
	NavTry RehearseDevice(const NavMesh& mesh, const NavRehearsal& rehearsal, ENavEdge kind, int device, int way);
	// 바닥 자리(발)에서 yaw 쪽으로 걷다가 떨어지기 시작하면 입력을 놓는다 — 내려가는 길 확인
	NavTry RehearseDrop(const NavMesh& mesh, const NavRehearsal& rehearsal, const Vec3& feet, double yawDegrees, int linkNode, int way);

	struct NavLinkCheck
	{
		uint32_t node = 0;				// 언리얼 연결선 노드 번호
		ENavEdge kind = ENavEdge::Drop;	// Teleport = 끝에 선 몸이 순간이동 칸과 겹침 · Drop = 그 밖
		int claims = 0;					// 언리얼 그래프가 이 연결선으로 잇는 (바닥 → 바닥) 수
		int confirmed = 0;				// 그중 우리 같은 종류 이음이 같은 걷기 덩어리끼리 잇는 수
	};

	struct NavGraphReport
	{
		size_t walkEdges = 0, dropEdges = 0, teleportEdges = 0, padEdges = 0;
		size_t dropUp = 0;				// 내려가기 거꾸로(낮은 끝 → 높은 끝 쪽) 리허설이 높은 끝 덩어리에 닿은 수 — 0 이어야(위로는 못 오른다)
		std::vector<NavTry> tries;		// 리허설 전부
		std::vector<NavLinkCheck> links;
	};

	// 우리 그래프를 짓는다 — 걷기 = 언리얼 바닥 이웃(연결선 노드는 뺀다) · 점프대 = 8방향 · 순간이동 = 상자 축 네 쪽 리허설 · 내려가기 = 순간이동이 아닌 언리얼 연결선의
	// 높은 끝에서 낮은 끝 쪽(거꾸로도 한 번 — dropUp). 같은 두 바닥 사이 같은 종류는 싼 것 하나. 언리얼 연결선은 그래프에 안 넣고 대조만 한다(report.links).
	// false = 바닥 노드가 앞에 몰려 있지 않음
	bool BuildNavGraph(const NavMesh& mesh, const NavRehearsal& rehearsal, NavGraph& out, NavGraphReport& report);

	// 걷기 이음만으로 묶은 덩어리 번호(바닥마다 — 이음을 양방향으로 본다)
	std::vector<uint32_t> NavWalkGroups(const NavGraph& graph);
	// 시작 바닥들에서 이음을 따라 닿는 바닥(바닥마다). skipKinds = 안 탈 종류 비트(1 << ENavEdge) — 대조군(순간이동 끔)
	std::vector<bool> NavReachable(const NavGraph& graph, const std::vector<uint32_t>& starts, uint32_t skipKinds = 0);

	// 리허설 몸 놓기 · 한 틱 — 서버 ApplyPlace · 예측 OnPlace 와 같은 차례로 놓고, 서버 ApplyInput 과 같은 TickWithLaunchers 로 한 틱 걷는다(NavCheck --walk 가 쓴다)
	void NavPlaceBody(const NavRehearsal& rehearsal, MoveState& state, const Vec3& at);
	LauncherTickResult NavTickBody(const NavRehearsal& rehearsal, MoveState& state, double moveX, double moveY, double yawDegrees);

	// ── 길 따라 걷기 (n-3) — 봇(BotClient --nav= · n-4)과 NavCheck --walk 가 같은 셈을 쓴다 ──

	struct NavHop
	{
		uint32_t floor = 0;
		int edge = -1;					// 이 바닥에서 나가는 이음(graph.edges[floor] 차례) — 마지막 바닥은 -1
	};

	// 우리 그래프에서 이음 비용 합이 가장 작은 길(다익스트라). 못 가면 false · outHops 비움 · outCost 0
	bool FindGraphPath(const NavGraph& graph, uint32_t from, uint32_t to, std::vector<NavHop>& outHops, double& outCost);
	// from 에서 바닥마다 이음 비용 합이 가장 작은 값(못 가면 무한) — 봇이 길 거리로 가장 가까운 목표를 고른다(n-4)
	void NavFloorDistances(const NavGraph& graph, uint32_t from, std::vector<double>& out);

	// 이웃 바닥 a → b 가 맞닿은 선분(수평으로 겹친 테두리 중 가장 긴 것). outLeft · outRight = a 쪽에서 맞닿은 선을 볼 때 x → y 로 도는 쪽 끝 · 반대쪽 끝.
	// 못 찾으면 false(L_Expanse 걷기 이음 1,236 은 다 찾는다 — NavCheck --walk 「portals」 줄)
	bool NavPortal(const NavNode& a, const NavNode& b, Vec3& outLeft, Vec3& outRight);

	// 줄 당기기(수평) — start 에서 corridor(이어진 바닥들 · 첫 바닥 = start 가 선 바닥)를 지나 goal 까지 꺾임점. outPoints = 꺾임점들 + goal(start 는 안 넣는다).
	// margin = 맞닿은 선 양끝을 안쪽으로 줄이는 거리(선이 2 × margin 보다 짧으면 가운데 한 점). 맞닿은 선을 못 찾은 이웃 수를 outPortalMiss 에 더한다
	void NavStringPull(const NavMesh& mesh, const std::vector<uint32_t>& corridor, const Vec3& start, const Vec3& goal, double margin, std::vector<Vec3>& outPoints,
		size_t& outPortalMiss);

	struct NavFollowParams
	{
		// NavStringPull margin — 15 = arrive 와 같게(꺾임점을 15cm 앞에서 넘기면 모서리를 그만큼 안쪽으로 자른다). NavCheck --walk 300쌍 걸어 보기(n-3)에서
		// 0 · 5 · 10 · 15 · 20 · 30 · 50 다 300/300 닿고, 걷는 틱에 막는 면을 따라 미끄러진 수(이동 셈 MoveCounters::slides)가 84 · 74 · 59 · 35 · 47 · 37 · 26 —
		// 15 부터 한 띠 · 평균 7.85 → 7.90초(D166)
		double margin = 15.0;
		double arrive = 15.0;			// 꺾임점에 이만큼 들거나 지나치면 다음 점(cm) — 한 틱 걸음(600cm/s ÷ 60 = 10cm)보다 커야 안 지나친다
		double floorDz = 60.0, floorOutside = 60.0;		// 자리 → 바닥 허용(리허설과 같은 값 — D165)
		// 위 허용으로 못 찾으면 한 번 더 — n-2 에서 버린 점프대 리허설 14개의 시작 · 끝 자리가 바닥 밖 64 ~ 106 이라 그걸 덮게(제 판단 · 300쌍에서 1번 씀)
		double floorFarDz = 120.0, floorFarOutside = 200.0;
		uint32_t stuckTicks = 60;		// 이 틱(1초 — 걷기로 600cm 갈 시간) 동안 목표 꺾임점 쪽으로 stuckDistance 를 못 줄이면 다시 찾기(제 판단)
		double stuckDistance = 30.0;
		uint32_t giveUpStucks = 3;		// 나아감 없이 다시 찾기를 이만큼 거듭하면 포기(제 판단)
		uint32_t edgeWalkMax = 300;		// 이음 시작 자리에서 사건(쏘임 · 옮김 · 떨어짐)까지 — 리허설 걷기 최대와 같다
		uint32_t flyMax = 720;			// 사건 뒤 내려앉을 때까지 — 리허설 순간이동 최대와 같다
		uint32_t noFlightTicks = 60;	// 사건 뒤 이만큼 안 날면(순간이동이 날리지 않고 옮기기만 함) 내려앉은 것으로 본다
		bool straight = false;			// 일부러 틀림 — 길 없이 목표로 곧장(지금 봇 걸음)
		bool noPull = false;			// 일부러 틀림 — 바닥 길은 찾되 줄 당기기 없이 구간 끝으로 곧장
		double edgeYawOffset = 0;		// 일부러 틀림 — 이음 걷는 방향을 돌림(도)
	};

	// 한 틱 관측 — NavCheck 는 TickWithLaunchers 결과, 봇은 예측 결과를 넣는다
	struct NavObserve
	{
		Vec3 location;					// 캡슐 중심
		EMoveMode mode = EMoveMode::Walking;
		int fired = -1;					// 바로 앞 틱에 쏜 점프대 · 옮긴 순간이동 차례(없으면 -1)
		int teleported = -1;
	};

	struct NavCommand
	{
		double moveX = 0, moveY = 0;	// 세계 축 입력(길이 1 또는 0) — 와이어 입력 칸(float)으로 자른 값이라 서버가 셈하는 값과 같다
		double yaw = 0;					// 도(float 로 자름) — 입력이 0 이면 앞 값
	};

	enum class ENavFollow : uint8_t
	{
		Walking,						// 꺾임점 따라 걷는 중
		Edge,							// 걷기 밖 이음 시작 자리에서 그 방향으로 걷는 중(사건 기다림)
		Flying,							// 사건 · 떨어짐 뒤 내려앉기 기다림 — 내려앉으면 다시 찾는다
		Arrived,
		NoPath,							// 바닥을 못 찾음 · 길 없음 — SetGoal 을 다시 부를 때까지 입력 0
		GaveUp,							// 막힘 거듭 · 내려앉기 시간 초과
	};
	const char* NavFollowName(ENavFollow state);

	struct NavFollowCounters
	{
		uint64_t plans = 0, noFloor = 0, farFloor = 0, noPath = 0;
		uint64_t stuckRepaths = 0, landRepaths = 0, edgeTimeouts = 0, unplannedEvents = 0, unplannedFalls = 0, flyTimeouts = 0;
		uint64_t edges[4] = {};			// 탄 걷기 밖 이음(ENavEdge 차례 — Walk 칸은 안 씀)
		uint64_t edgeMisses[4] = {};	// 이음을 타려다 놓침 — 사건 없이 edgeWalkMax 지남 · 다른 장치 · 떨어짐 이음이 아닌데 걷다 떨어짐 · 내려앉은 바닥이 이음 끝 바닥과 다른 걷기 덩어리
		size_t portalMiss = 0;
	};

	class CNavFollower
	{
		enum class EFlyInput : uint8_t { None, Edge, Toward };

	public:
		CNavFollower(const NavMesh& mesh, const NavGraph& graph, double halfHeight, const NavFollowParams& params);

		// 목표 = 캡슐 중심 자리. 다음 Step 이 지금 자리에서 길을 찾는다. resetStuck = 막힘 셈을 0 으로(새 목표). false = 같은 목표를 다시 찾기(봇의 0.5초 갱신 —
		// 막힘이 거듭되면 그대로 포기까지 간다. 걷던 중이면 막힘 틱 셈도 이어 간다). 이음을 타는 중(Edge · Flying)이면 목표만 바꾸고 내려앉은 뒤 찾는다
		void SetGoal(const Vec3& goal, bool resetStuck = true);
		// 몸이 길 밖에서 옮겨졌다(부활) · 따라 걷기를 쉬었다(봇 옆걸음) — 타던 이음 · 날기를 버리고 다음 Step 이 지금 자리에서 찾는다. 셈은 그대로
		void Reset(const Vec3& goal);
		ENavFollow Step(const NavObserve& o, NavCommand& out);
		// 캡슐 중심 자리 → 바닥(Plan 과 같은 두 단 허용 · 셈 안 함). 없으면 -1
		int FloorOf(const Vec3& capsuleCenter) const;
		// 이음을 타는 중 — 부르는 쪽이 요를 out.yaw 로 두고(밀기 점프대는 몸 방향으로 쏜다) 몸을 옮기는 다른 단추를 안 누른다
		bool SteersYaw() const { return _state == ENavFollow::Edge || (_state == ENavFollow::Flying && _flyInput == EFlyInput::Edge); }
		bool Busy() const { return _state == ENavFollow::Edge || _state == ENavFollow::Flying; }
		// 걷는 중이고 이 길이 이음으로 끝나면 이음 시작 자리(마지막 꺾임점)까지 수평 거리, 아니면 무한 — 부르는 쪽이 이음 앞에서 몸을 옮기는 단추를 거두게(봇 대시, n-4)
		double EdgeStartDistance(const Vec3& at) const;

		ENavFollow State() const { return _state; }
		const std::vector<Vec3>& Points() const { return _points; }		// 지금 걷는 꺾임점(마지막 = 목표 또는 이음 시작 자리)
		const std::vector<uint32_t>& Corridor() const { return _corridor; }
		const Vec3& PlanStart() const { return _planStart; }
		const NavFollowCounters& Counters() const { return _counters; }

	private:
		void Plan(const NavObserve& o);
		void EnterFlying(EFlyInput input, const NavObserve& o);
		void Steer(const Vec3& from, const Vec3& to, NavCommand& out);
		void SteerYaw(double yawDegrees, NavCommand& out);

		const NavMesh& _mesh;
		const NavGraph& _graph;
		double _halfHeight;
		NavFollowParams _p;
		std::vector<uint32_t> _groups;			// NavWalkGroups — 이음 놓침(내려앉은 덩어리) 가르기

		ENavFollow _state = ENavFollow::NoPath;
		bool _needPlan = false;
		bool _keepProgress = false;				// SetGoal(…, false) 로 걷던 중 다시 찾기 — Plan 이 막힘 틱 셈(_progressTicks)을 이어 간다
		Vec3 _goal, _planStart;
		std::vector<uint32_t> _corridor;
		std::vector<Vec3> _points;
		size_t _index = 0;
		int _edgeFloor = -1, _edgeIndex = -1;	// 이 구간 끝의 걷기 밖 이음(없으면 -1 — 끝 = 목표)
		double _bestDistance = 0;
		uint32_t _progressTicks = 0, _stucks = 0, _edgeTicks = 0, _flyTicks = 0;
		EFlyInput _flyInput = EFlyInput::None;
		int _flyEdgeTo = -1;					// 지금 날고 있는 이음의 끝 바닥(길에 없는 사건 · 떨어짐이면 -1)
		ENavEdge _flyEdgeKind = ENavEdge::Walk;
		bool _flew = false;
		double _yaw = 0;
		NavFollowCounters _counters;
	};
}
