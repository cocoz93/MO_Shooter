// 사용: NavCheck <nav.monv> [--pairs <pairs.csv>] [--break cut|join] [--load-test] | <nav.monv> --graph --map <mocl> --launchers <txt> --teleports <txt> [--pad-plans <봇 기록>] [--teleport-plans <봇 기록>]
// 봇 길찾기 n-1(bot-nav-plan) — Shared/Nav(ParseNav · FindNavPath)가 언리얼 경로 답과 「길 있음 / 없음」이 같은가.
//   답 = <nav.monv>.pairs.csv(MOBNavDumpSubsystem 이 언리얼 FindPathSync 에 물은 560쌍 — 무작위 300 · 위아래로 겹친 바닥 260). 언리얼 그래프 그대로(연결선 포함)라
//   어긋남은 0 이어야 한다. 같은 대조를 파이썬(Tools/LyraPilot/check_nav.py)도 하므로 두 구현의 수가 같아야 한다.
// --break  일부러 틀림(check_nav.py --break 와 같은 망가뜨림) — cut = 가장 큰 덩어리를 중심 높이 중앙값으로 둘로 끊음 · join = 덩어리끼리 다 이음. 어긋남 > 0 이어야 한다
// --load-test  읽기 검사 — 파일 바이트를 일부러 망가뜨린 여섯 판이 저마다 맞는 실패로 거절되는가
// --graph  n-2 — 우리 그래프(Shared/Nav BuildNavGraph — 점프대 · 순간이동 · 내려가기를 서버와 같은 이동 셈으로 밟아 본다)와 스폰 20곳에서 닿는 바닥 · 맨 위층 수.
//   대조군 = 순간이동 이음을 빼면 맨 위층 0. --pad-plans · --teleport-plans 를 주면 봇 각본 줄(같은 장치 · 같은 쪽)의 선 자리 · 멈춘 자리와 0.01cm 안에서 같아야 한다
// 끝 줄 NAVCHECK … PASS 면 0, 아니면 1(인자가 틀리면 2)
#include "MatchRules.h"
#include "Movement.h"
#include "Nav.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace
{
	constexpr int kUnrealSuccess = 3;	// ENavigationQueryResult::Success(pairs.csv result 칸)

	bool ReadFile(const char* path, std::vector<uint8_t>& out)
	{
		FILE* file = std::fopen(path, "rb");
		if (!file) return false;
		std::fseek(file, 0, SEEK_END);
		const long size = std::ftell(file);
		std::fseek(file, 0, SEEK_SET);
		out.resize(size > 0 ? (size_t)size : 0);
		const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
		std::fclose(file);
		return got == out.size();
	}

	struct Pair
	{
		std::string kind;
		uint32_t a = 0, b = 0;
		int result = 0;
		double length = 0;
	};

	bool ReadPairs(const char* path, std::vector<Pair>& out)
	{
		std::vector<uint8_t> bytes;
		if (!ReadFile(path, bytes)) return false;
		const std::string text(bytes.begin(), bytes.end());
		size_t start = 0;
		bool header = true;
		while (start < text.size())
		{
			size_t end = text.find('\n', start);
			if (end == std::string::npos) end = text.size();
			std::string line = text.substr(start, end - start);
			start = end + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()) continue;
			if (header)
			{
				if (line != "kind,a,b,result,points,length") return false;
				header = false;
				continue;
			}
			std::vector<std::string> cells;
			size_t from = 0;
			for (size_t comma; (comma = line.find(',', from)) != std::string::npos; from = comma + 1) cells.push_back(line.substr(from, comma - from));
			cells.push_back(line.substr(from));
			if (cells.size() != 6) return false;
			Pair pair;
			pair.kind = cells[0];
			pair.a = (uint32_t)std::strtoul(cells[1].c_str(), nullptr, 10);
			pair.b = (uint32_t)std::strtoul(cells[2].c_str(), nullptr, 10);
			pair.result = std::atoi(cells[3].c_str());
			pair.length = std::atof(cells[5].c_str());
			out.push_back(pair);
		}
		return !header;
	}

	// 덩어리(연결을 양방향으로 본 묶음) — check_nav.py components 와 같은 차례: 뿌리가 처음 나온 차례로 묶고 큰 것부터(크기가 같으면 먼저 나온 것 먼저)
	std::vector<std::vector<uint32_t>> Components(const mo::NavMesh& mesh)
	{
		std::vector<uint32_t> parent(mesh.nodes.size());
		std::iota(parent.begin(), parent.end(), 0u);
		auto find = [&parent](uint32_t x)
		{
			while (parent[x] != x)
			{
				parent[x] = parent[parent[x]];
				x = parent[x];
			}
			return x;
		};
		for (uint32_t i = 0; i < mesh.nodes.size(); ++i)
		{
			for (const uint32_t j : mesh.nodes[i].next) parent[find(i)] = find(j);
		}
		std::vector<std::vector<uint32_t>> groups;
		std::map<uint32_t, size_t> slot;
		for (uint32_t i = 0; i < mesh.nodes.size(); ++i)
		{
			const uint32_t root = find(i);
			auto it = slot.find(root);
			if (it == slot.end())
			{
				it = slot.emplace(root, groups.size()).first;
				groups.emplace_back();
			}
			groups[it->second].push_back(i);
		}
		std::stable_sort(groups.begin(), groups.end(), [](const std::vector<uint32_t>& l, const std::vector<uint32_t>& r) { return l.size() > r.size(); });
		return groups;
	}

	void Corrupt(mo::NavMesh& mesh, const std::string& mode)
	{
		const std::vector<std::vector<uint32_t>> groups = Components(mesh);
		if (mode == "cut")
		{
			// 가장 큰 덩어리를 높이로 둘로 — 이어졌던 바닥이 끊긴다(check_nav.py corrupt cut, 중앙값 = statistics.median)
			std::vector<double> zs;
			for (const uint32_t i : groups[0]) zs.push_back(mesh.nodes[i].center.z);
			std::sort(zs.begin(), zs.end());
			const size_t n = zs.size();
			const double middle = n % 2 ? zs[n / 2] : (zs[n / 2 - 1] + zs[n / 2]) / 2.0;
			size_t cut = 0;
			for (const uint32_t i : groups[0])
			{
				std::vector<uint32_t>& next = mesh.nodes[i].next;
				const bool high = mesh.nodes[i].center.z > middle;
				const size_t before = next.size();
				next.erase(std::remove_if(next.begin(), next.end(), [&](uint32_t j) { return (mesh.nodes[j].center.z > middle) != high; }), next.end());
				cut += before - next.size();
			}
			std::printf("NAVCHECK break=cut removed=%zu\n", cut);
		}
		else
		{
			// 덩어리를 다 잇는다 — 길이 없던 바닥에 길이 생긴다(check_nav.py corrupt join)
			for (size_t g = 0; g + 1 < groups.size(); ++g)
			{
				mesh.nodes[groups[g][0]].next.push_back(groups[g + 1][0]);
				mesh.nodes[groups[g + 1][0]].next.push_back(groups[g][0]);
			}
			std::printf("NAVCHECK break=join added=%zu\n", groups.empty() ? (size_t)0 : 2 * (groups.size() - 1));
		}
	}

	// 읽기 검사 — 같은 바이트를 하나씩 망가뜨려 ParseNav 가 맞는 이름으로 거절하나
	bool LoadTest(const std::vector<uint8_t>& bytes, const mo::NavMesh& mesh)
	{
		auto put = [](std::vector<uint8_t>& b, size_t at, uint32_t value) { std::memcpy(b.data() + at, &value, sizeof(value)); };
		// 노드 0 의 첫 이웃 자리 = 머리 48바이트(MONV · 판 · 노드 수 · 이웃 수 · 상자 24 · 반지름 · 키) + 종류 · 구역 8 + 중심 12 + 꼭짓점 수 4 + 꼭짓점 + 이웃 수 4
		const size_t firstNext = 48 + 8 + 12 + 4 + mesh.nodes[0].verts.size() * 12 + 4;
		struct Case { const char* name; std::vector<uint8_t> data; mo::ENavLoad want; };
		std::vector<Case> cases;
		cases.push_back({ "magic", bytes, mo::ENavLoad::BadMagic });
		cases.back().data[0] = 'X';
		cases.push_back({ "version", bytes, mo::ENavLoad::BadVersion });
		put(cases.back().data, 4, 2);
		cases.push_back({ "cutLastByte", bytes, mo::ENavLoad::Truncated });
		cases.back().data.pop_back();
		cases.push_back({ "extraByte", bytes, mo::ENavLoad::SizeMismatch });
		cases.back().data.push_back(0);
		cases.push_back({ "linkCount", bytes, mo::ENavLoad::BadLinks });
		put(cases.back().data, 12, (uint32_t)mesh.linkCount + 1);
		cases.push_back({ "neighborIndex", bytes, mo::ENavLoad::BadLinks });
		if (mesh.nodes[0].next.empty()) return false;
		put(cases.back().data, firstNext, (uint32_t)mesh.nodes.size());
		size_t ok = 0;
		for (const Case& c : cases)
		{
			mo::NavMesh scratch;
			const mo::ENavLoad got = mo::ParseNav(c.data.data(), c.data.size(), scratch);
			const bool pass = got == c.want && scratch.nodes.empty();
			ok += pass ? 1 : 0;
			std::printf("NAVCHECK loadTest case=%s want=%s got=%s %s\n", c.name, mo::NavLoadName(c.want), mo::NavLoadName(got), pass ? "ok" : "WRONG");
		}
		std::printf("NAVCHECK loadTest cases=%zu ok=%zu\n", cases.size(), ok);
		return ok == cases.size();
	}

	// ── --graph (n-2) — 우리 그래프 · 스폰에서 닿는 바닥 ──

	constexpr double kTopFloorZ = 250.0;	// 맨 위층 바닥 = 중심 높이가 이보다 높은 바닥(L_Expanse 38개 — 플랜 n-2 「지금 값」 434 / 602 · 맨 위층 12 를 셈한 선과 같다)

	bool ReadText(const char* path, std::vector<uint8_t>& out, const char* what)
	{
		if (ReadFile(path, out) && !out.empty()) return true;
		std::printf("NAVGRAPH cannot read %s %s FAIL\n", what, path);
		return false;
	}

	struct BotPlan
	{
		int device = -1, way = -1;
		bool ok = false, air = false;
		mo::Vec3 start, end;
	};

	// 봇 각본 줄(regress 봇점프대 · 봇순간이동 판 봇 기록) — JUMPPAD plan pad= … dir= … start=( ) … end=( ) / TELEPORT plan dev= … way= side= … start=( ) … end=( )
	void ReadBotPlans(const char* path, bool teleport, std::vector<BotPlan>& out)
	{
		std::vector<uint8_t> bytes;
		if (!path || !ReadFile(path, bytes)) return;
		const std::string text(bytes.begin(), bytes.end());
		const char* head = teleport ? "TELEPORT plan dev=" : "JUMPPAD plan pad=";
		for (size_t at = text.find(head); at != std::string::npos; at = text.find(head, at + 1))
		{
			const std::string line = text.substr(at, text.find('\n', at) - at);
			BotPlan p;
			int ok = 0;
			const size_t s = line.find("start=("), e = line.find("end=(");
			if (s == std::string::npos || e == std::string::npos) continue;
			if (std::sscanf(line.c_str() + s, "start=(%lf %lf %lf)", &p.start.x, &p.start.y, &p.start.z) != 3) continue;
			if (std::sscanf(line.c_str() + e, "end=(%lf %lf %lf)", &p.end.x, &p.end.y, &p.end.z) != 3) continue;
			if (teleport)
			{
				char way[16] = {};
				int target = 0;
				if (std::sscanf(line.c_str(), "TELEPORT plan dev=%d target=%d ok=%d way=%15s side=%d", &p.device, &target, &ok, way, &p.way) != 5) continue;
				p.air = std::strcmp(way, "air") == 0;
			}
			else
			{
				char kind[16] = {};
				if (std::sscanf(line.c_str(), "JUMPPAD plan pad=%d kind=%15s ok=%d dir=%d", &p.device, kind, &ok, &p.way) != 4) continue;
			}
			p.ok = ok != 0;
			out.push_back(p);
		}
	}

	double Gap(const mo::Vec3& a, const mo::Vec3& b)
	{
		const mo::Vec3 d = a - b;
		return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	}

	// 맵 · 점프대 · 순간이동 표를 읽고 우리 그래프를 짓는다(--graph · --walk 같이). 이동 셈이 맵 · 수치를 참조로 쥐므로 setup 은 옮기지 않는다
	struct GraphSetup
	{
		mo::CMapCollision map;
		std::vector<mo::Launcher> launchers;
		mo::TeleportTable teleports;
		mo::MoveParams params = mo::MakeLyraMoveParams();
		mo::MatchRules rules;
		std::unique_ptr<mo::CMoveSim> sim;
		mo::NavRehearsal rehearsal;
		mo::NavGraph graph;
		mo::NavGraphReport report;
	};

	bool LoadGraph(const mo::NavMesh& mesh, const char* mapPath, const char* launchersPath, const char* teleportsPath, GraphSetup& setup)
	{
		std::vector<uint8_t> mapBytes, launcherText, teleportText;
		if (!ReadText(mapPath, mapBytes, "map") || !ReadText(launchersPath, launcherText, "launchers") || !ReadText(teleportsPath, teleportText, "teleports")) return false;
		const mo::EMapLoad mapLoaded = setup.map.LoadFromMemory(mapBytes.data(), mapBytes.size());
		size_t badLine = 0;
		const mo::ELaunchersLoad lr = mo::ParseLaunchers(reinterpret_cast<const char*>(launcherText.data()), launcherText.size(), setup.launchers, badLine);
		const mo::ETeleportsLoad tr = mo::ParseTeleports(reinterpret_cast<const char*>(teleportText.data()), teleportText.size(), setup.teleports, badLine);
		if (mapLoaded != mo::EMapLoad::Ok || lr != mo::ELaunchersLoad::Ok || tr != mo::ETeleportsLoad::Ok)
		{
			std::printf("NAVGRAPH load map=%d launchers=%s teleports=%s FAIL\n", (int)mapLoaded, mo::LaunchersLoadName(lr), mo::TeleportsLoadName(tr));
			return false;
		}
		const mo::MoveParams& params = setup.params;
		setup.sim.reset(new mo::CMoveSim(setup.map, setup.params));
		mo::NavRehearsal& rehearsal = setup.rehearsal;
		rehearsal.sim = setup.sim.get();
		rehearsal.capsuleRadius = params.capsuleRadius;
		rehearsal.capsuleHalfHeight = params.capsuleHalfHeight;
		rehearsal.walkSpeed = params.maxWalkSpeed;
		rehearsal.killZ = setup.rules.fallKillZ;
		rehearsal.launchers.items = setup.launchers.data();
		rehearsal.launchers.count = setup.launchers.size();
		rehearsal.teleports.items = setup.teleports.items.data();
		rehearsal.teleports.count = setup.teleports.items.size();
		rehearsal.teleports.meshBaseZ = setup.teleports.meshBaseZ;
		if (!mo::BuildNavGraph(mesh, rehearsal, setup.graph, setup.report))
		{
			std::printf("NAVGRAPH ground nodes not first FAIL\n");
			return false;
		}
		return true;
	}

	bool RunGraph(const mo::NavMesh& mesh, const char* mapPath, const char* launchersPath, const char* teleportsPath, const char* padPlans, const char* teleportPlans)
	{
		GraphSetup setup;
		if (!LoadGraph(mesh, mapPath, launchersPath, teleportsPath, setup)) return false;
		const mo::MoveParams& params = setup.params;
		const mo::NavGraph& graph = setup.graph;
		const mo::NavGraphReport& report = setup.report;
		for (const mo::NavTry& t : report.tries)
		{
			std::printf("NAVGRAPH try kind=%s device=%d way=%d yaw=%.0f result=%s from=%d to=%d ticks=%u start=(%.2f %.2f %.2f) end=(%.2f %.2f %.2f)\n", mo::NavEdgeName(t.kind), t.device,
				t.way, t.yaw, mo::NavTryName(t.result), t.from, t.to, t.ticks, t.start.x, t.start.y, t.start.z, t.end.x, t.end.y, t.end.z);
		}
		int teleportLinks = 0, dropLinks = 0;
		for (const mo::NavLinkCheck& c : report.links)
		{
			std::printf("NAVGRAPH link node=%u kind=%s claims=%d confirmed=%d\n", c.node, mo::NavEdgeName(c.kind), c.claims, c.confirmed);
			if (c.confirmed > 0) ++(c.kind == mo::ENavEdge::Teleport ? teleportLinks : dropLinks);
		}

		// 봇 각본과 같은가 — 같은 장치 · 같은 쪽 리허설의 선 자리 · 멈춘 자리(각본 줄은 소수 둘째 자리까지라 0.01 안이면 같다)
		std::vector<BotPlan> plans;
		ReadBotPlans(padPlans, false, plans);
		const size_t padPlanCount = plans.size();
		ReadBotPlans(teleportPlans, true, plans);
		size_t planCompared = 0, planSame = 0, airOnly = 0, airFloorOk = 0;
		double planMax = 0;
		for (size_t i = 0; i < plans.size(); ++i)
		{
			const BotPlan& p = plans[i];
			const mo::ENavEdge kind = i < padPlanCount ? mo::ENavEdge::Pad : mo::ENavEdge::Teleport;
			if (!p.ok) continue;
			if (p.air)
			{
				// 각본이 공중 밀어 넣기로만 탄 장치 — 바닥에서 걸어 들어가는 리허설은 다 실패해야 한다(위층 장치)
				++airOnly;
				for (const mo::NavTry& t : report.tries) airFloorOk += (t.kind == kind && t.device == p.device && t.result == mo::ENavTry::Ok) ? 1 : 0;
				continue;
			}
			for (const mo::NavTry& t : report.tries)
			{
				if (t.kind != kind || t.device != p.device || t.way != p.way) continue;
				const double gap = std::max(Gap(t.start, p.start), Gap(t.end, p.end));
				++planCompared;
				planSame += gap <= 0.01 ? 1 : 0;
				planMax = std::max(planMax, gap);
				std::printf("NAVGRAPH plan kind=%s device=%d way=%d gap=%.4f %s\n", mo::NavEdgeName(kind), p.device, p.way, gap, gap <= 0.01 ? "same" : "DIFF");
			}
		}

		// 스폰 20곳 → 바닥 · 닿는 바닥
		size_t spawnCount = 0;
		const mo::SpawnPoint* spawns = mo::GetExpanseStarts(spawnCount);
		std::vector<uint32_t> starts;
		size_t spawnMiss = 0;
		// 고른 바닥까지 높이 차 · 다각형 밖 거리 최댓값 — 바닥 찾기 허용(60 · 60)이 실제 값에서 얼마나 떨어졌나
		double spawnDz = 0, spawnOut = 0, tryDz = 0, tryOut = 0;
		size_t tryPoints = 0;
		auto measure = [&mesh](int floor, const mo::Vec3& feet, double& dzMax, double& outMax)
		{
			double z = 0, outside = 0;
			mo::NavFloorHeight(mesh.nodes[floor], feet.x, feet.y, z, outside);
			dzMax = std::max(dzMax, std::fabs(z - feet.z));
			outMax = std::max(outMax, outside);
		};
		const mo::Vec3 down(0, 0, params.capsuleHalfHeight);
		for (size_t i = 0; i < spawnCount; ++i)
		{
			const int floor = mo::FindNavFloor(mesh, spawns[i].location - down, 60.0, 60.0);
			if (floor < 0)
			{
				++spawnMiss;
				continue;
			}
			starts.push_back((uint32_t)floor);
			measure(floor, spawns[i].location - down, spawnDz, spawnOut);
		}
		for (const mo::NavTry& t : report.tries)
		{
			if (t.result != mo::ENavTry::Ok) continue;
			measure(t.from, t.start - down, tryDz, tryOut);
			measure(t.to, t.end - down, tryDz, tryOut);
			tryPoints += 2;
		}
		size_t topAll = 0;
		for (size_t i = 0; i < graph.ground; ++i) topAll += mesh.nodes[i].center.z > kTopFloorZ ? 1 : 0;
		auto count = [&](uint32_t skip, size_t& reached, size_t& top)
		{
			const std::vector<bool> seen = mo::NavReachable(graph, starts, skip);
			reached = top = 0;
			for (size_t i = 0; i < graph.ground; ++i)
			{
				if (!seen[i]) continue;
				++reached;
				top += mesh.nodes[i].center.z > kTopFloorZ ? 1 : 0;
			}
			return seen;
		};
		size_t reached = 0, top = 0, offReached = 0, offTop = 0, noPadReached = 0, noPadTop = 0;
		const std::vector<bool> seen = count(0, reached, top);
		(void)count(1u << (uint32_t)mo::ENavEdge::Teleport, offReached, offTop);
		(void)count(1u << (uint32_t)mo::ENavEdge::Pad, noPadReached, noPadTop);
		std::string unreachable;
		for (size_t i = 0; i < graph.ground; ++i)
		{
			if (seen[i]) continue;
			if (!unreachable.empty()) unreachable += ',';
			unreachable += std::to_string(i);
		}
		std::printf("NAVGRAPH edges walk=%zu drop=%zu teleport=%zu pad=%zu dropUp=%zu\n", report.walkEdges, report.dropEdges, report.teleportEdges, report.padEdges, report.dropUp);
		std::printf("NAVGRAPH plans compared=%zu same=%zu maxGap=%.4f airOnly=%zu airFloorOk=%zu\n", planCompared, planSame, planMax, airOnly, airFloorOk);
		std::printf("NAVGRAPH spawns=%zu miss=%zu starts=%zu\n", spawnCount, spawnMiss, starts.size());
		std::printf("NAVGRAPH floor spawns dzMax=%.2f outsideMax=%.2f | tries points=%zu dzMax=%.2f outsideMax=%.2f (limit 60 60)\n", spawnDz, spawnOut, tryPoints, tryDz, tryOut);
		std::printf("NAVGRAPH control teleportsOff reachable=%zu top=%zu | padsOff reachable=%zu top=%zu\n", offReached, offTop, noPadReached, noPadTop);
		std::printf("NAVGRAPH unreachable count=%zu nodes=%s\n", graph.ground - reached, unreachable.c_str());
		const bool ok = spawnMiss == 0 && teleportLinks == 3 && dropLinks == 2 && report.dropUp == 0 && top >= 12 && offTop == 0 && planCompared > 0 &&
			planSame == planCompared && airFloorOk == 0;
		std::printf("NAVGRAPH teleports=%d drops=%d padLinks=%zu reachable=%zu top=%zu of %zu %s\n", teleportLinks, dropLinks, report.padEdges, reached, top, topAll, ok ? "PASS" : "FAIL");
		return ok;
	}

	// ── --walk (n-3) — 우리 그래프 길을 따라 우리 이동 셈으로 걸어 본다 ──

	constexpr uint32_t kWalkMaxTicks = 90 * mo::wire::kTickRate;	// 쌍 하나 최대 90초 — 걸어 본 가장 긴 쌍 17.5초(n-3)의 다섯 배(제 판단). 넘으면 timeout
	constexpr double kWalkGoalXY = 50.0, kWalkGoalDz = 100.0;		// 다른 바닥에 섰어도 목표에서 이 안이면 닿음
	constexpr double kPullSampleStep = 10.0, kPullOutside = 2.0;	// 꺾임 선 검사 — 10cm 마다 · 바닥 밖 2cm 넘으면 셈

	struct WalkPair
	{
		const char* kind = "";
		mo::Vec3 start, goal;
		uint32_t from = 0, to = 0;
		bool usesTeleport = false, usesPad = false, usesDrop = false;
	};

	// 씨앗 고정 난수(LCG) — 표준 분포 함수는 구현마다 수가 달라 쓰지 않는다
	uint32_t NextRandom(uint64_t& seed)
	{
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		return (uint32_t)(seed >> 33);
	}

	// 점이 길 바닥들 중 가장 가까운 것의 테두리 밖으로 나간 거리(안이면 0)
	double CorridorOutside(const mo::NavMesh& mesh, const std::vector<uint32_t>& corridor, double x, double y)
	{
		double nearest = std::numeric_limits<double>::infinity();
		for (const uint32_t f : corridor)
		{
			double z = 0, out = 0;
			mo::NavFloorHeight(mesh.nodes[f], x, y, z, out);
			nearest = std::min(nearest, out);
		}
		return nearest;
	}

	// 꺾임 선(계획 시작 → 꺾임점들)이 그 길의 바닥들 밖으로 나가는 점 수. 선의 양끝은 바닥 밖일 수 있다(내려앉은 자리 · 이음 시작 자리 — 바닥 허용 안) —
	// 그만큼은 비례로 봐준다(끝이 d 밖이면 그 끝에 가까울수록 d 까지). 양끝이 다 바닥 안인데 밖으로 나가면 모서리를 자른 것
	void CheckPull(const mo::NavMesh& mesh, const mo::CNavFollower& follower, size_t& samples, size_t& outside, bool trace)
	{
		const std::vector<uint32_t>& corridor = follower.Corridor();
		const std::vector<mo::Vec3>& points = follower.Points();
		if (corridor.empty() || points.empty()) return;
		mo::Vec3 prev = follower.PlanStart();
		for (const mo::Vec3& point : points)
		{
			const double length = std::hypot(point.x - prev.x, point.y - prev.y);
			const double outA = CorridorOutside(mesh, corridor, prev.x, prev.y), outB = CorridorOutside(mesh, corridor, point.x, point.y);
			for (double at = 0; at < length; at += kPullSampleStep)
			{
				const double u = at / length;
				const double x = prev.x + (point.x - prev.x) * u, y = prev.y + (point.y - prev.y) * u;
				const double by = CorridorOutside(mesh, corridor, x, y);
				const double allowed = kPullOutside + (1 - u) * outA + u * outB;
				++samples;
				outside += by > allowed ? 1 : 0;
				if (trace && by > allowed) std::printf("NAVTRACE outside at=(%.1f %.1f) by=%.1f allowed=%.1f\n", x, y, by, allowed);
			}
			prev = point;
		}
	}

	bool RunWalk(const mo::NavMesh& mesh, const char* mapPath, const char* launchersPath, const char* teleportsPath, double margin, size_t pairCount, const std::string& breakWalk,
		size_t tracePair)
	{
		GraphSetup setup;
		if (!LoadGraph(mesh, mapPath, launchersPath, teleportsPath, setup)) return false;
		const mo::NavGraph& graph = setup.graph;
		const double halfHeight = setup.params.capsuleHalfHeight;
		const mo::Vec3 up(0, 0, halfHeight);

		// 걷기 이음 전부의 맞닿은 선(걸어 본 길만이 아니라)
		size_t portalEdges = 0, portalFound = 0;
		double portalShortest = std::numeric_limits<double>::infinity();
		for (uint32_t f = 0; f < graph.ground; ++f)
		{
			for (const mo::NavEdge& e : graph.edges[f])
			{
				if (e.kind != mo::ENavEdge::Walk) continue;
				++portalEdges;
				mo::Vec3 l, r;
				if (!mo::NavPortal(mesh.nodes[f], mesh.nodes[e.to], l, r)) continue;
				++portalFound;
				portalShortest = std::min(portalShortest, std::hypot(l.x - r.x, l.y - r.y));
			}
		}
		std::printf("NAVWALK portals walkEdges=%zu found=%zu miss=%zu shortest=%.1f\n", portalEdges, portalFound, portalEdges - portalFound, portalShortest);

		// 스폰 → 바닥(--graph 와 같은 허용)
		size_t spawnCount = 0;
		const mo::SpawnPoint* spawns = mo::GetExpanseStarts(spawnCount);
		std::vector<int> spawnFloor(spawnCount, -1);
		std::vector<uint32_t> starts;
		for (size_t i = 0; i < spawnCount; ++i)
		{
			spawnFloor[i] = mo::FindNavFloor(mesh, spawns[i].location - up, 60.0, 60.0);
			if (spawnFloor[i] >= 0) starts.push_back((uint32_t)spawnFloor[i]);
		}
		const std::vector<bool> fromSpawns = mo::NavReachable(graph, starts);

		// 시작 · 목표가 점프대 · 순간이동 칸에 겹치면 서 있을 수 없는 자리 — 쌍에서 뺀다(L_Expanse 바닥 307 중심이 위로 쏘는 점프대 7 안)
		const mo::NavRehearsal& rh = setup.rehearsal;
		auto onDevice = [&rh, halfHeight](const mo::Vec3& at)
		{
			return mo::LauncherMask(rh.launchers.items, rh.launchers.count, at, rh.capsuleRadius, halfHeight) != 0 ||
				mo::TeleportMask(rh.teleports, at, rh.capsuleRadius, halfHeight) != 0;
		};
		size_t onDeviceSkipped = 0;

		auto classify = [&graph](WalkPair& p)
		{
			std::vector<mo::NavHop> hops;
			double cost = 0;
			if (!mo::FindGraphPath(graph, p.from, p.to, hops, cost)) return false;
			for (const mo::NavHop& hop : hops)
			{
				if (hop.edge < 0) continue;
				const mo::ENavEdge kind = graph.edges[hop.floor][hop.edge].kind;
				p.usesTeleport = p.usesTeleport || kind == mo::ENavEdge::Teleport;
				p.usesPad = p.usesPad || kind == mo::ENavEdge::Pad;
				p.usesDrop = p.usesDrop || kind == mo::ENavEdge::Drop;
			}
			return true;
		};

		// 쌍 — 1/3 은 스폰 → 스폰(다른 바닥 · 닿는 쌍), 나머지는 스폰에서 닿는 바닥 → 그 바닥에서 닿는 바닥(바닥 중심)
		uint64_t seed = 20261123;
		std::vector<WalkPair> pairs;
		std::vector<std::pair<size_t, size_t>> spawnPairs;
		for (size_t a = 0; a < spawnCount; ++a)
		{
			if (spawnFloor[a] < 0) continue;
			const std::vector<bool> reach = mo::NavReachable(graph, std::vector<uint32_t>(1, (uint32_t)spawnFloor[a]));
			for (size_t b = 0; b < spawnCount; ++b)
			{
				if (b == a || spawnFloor[b] < 0 || spawnFloor[b] == spawnFloor[a] || !reach[spawnFloor[b]]) continue;
				spawnPairs.push_back(std::make_pair(a, b));
			}
		}
		for (size_t i = spawnPairs.size(); i > 1; --i) std::swap(spawnPairs[i - 1], spawnPairs[NextRandom(seed) % i]);
		for (size_t i = 0; i < spawnPairs.size() && pairs.size() < pairCount / 3; ++i)
		{
			WalkPair p;
			p.kind = "spawn";
			p.start = spawns[spawnPairs[i].first].location;
			p.goal = spawns[spawnPairs[i].second].location;
			p.from = (uint32_t)spawnFloor[spawnPairs[i].first];
			p.to = (uint32_t)spawnFloor[spawnPairs[i].second];
			if (onDevice(p.start) || onDevice(p.goal))
			{
				++onDeviceSkipped;
				std::printf("NAVWALK skip onDevice kind=spawn from=%u to=%u\n", p.from, p.to);
				continue;
			}
			if (classify(p)) pairs.push_back(p);
		}
		std::vector<uint32_t> reachable;
		for (uint32_t i = 0; i < graph.ground; ++i)
		{
			if (fromSpawns[i]) reachable.push_back(i);
		}
		std::vector<std::pair<uint32_t, uint32_t>> used;
		for (size_t tries = 0; pairs.size() < pairCount && tries < 100000 && !reachable.empty(); ++tries)
		{
			const uint32_t s = reachable[NextRandom(seed) % reachable.size()];
			const std::vector<bool> reach = mo::NavReachable(graph, std::vector<uint32_t>(1, s));
			std::vector<uint32_t> goals;
			for (uint32_t g = 0; g < graph.ground; ++g)
			{
				if (g != s && reach[g]) goals.push_back(g);
			}
			if (goals.empty()) continue;
			const uint32_t g = goals[NextRandom(seed) % goals.size()];
			if (std::find(used.begin(), used.end(), std::make_pair(s, g)) != used.end()) continue;
			used.push_back(std::make_pair(s, g));
			WalkPair p;
			p.kind = "random";
			p.from = s;
			p.to = g;
			p.start = mesh.nodes[s].center + up + mo::Vec3(0, 0, 60);	// 바닥 높이 어긋남(최대 47 — n-2)보다 높이 놓고 내려앉힌다
			p.goal = mesh.nodes[g].center + up;
			if (onDevice(mesh.nodes[s].center + up) || onDevice(p.goal))
			{
				++onDeviceSkipped;
				std::printf("NAVWALK skip onDevice kind=random from=%u to=%u start=%d goal=%d\n", s, g, onDevice(mesh.nodes[s].center + up) ? 1 : 0, onDevice(p.goal) ? 1 : 0);
				continue;
			}
			if (classify(p)) pairs.push_back(p);
		}

		mo::NavFollowParams follow;
		follow.margin = margin;
		follow.straight = breakWalk == "straight";
		follow.noPull = breakWalk == "nopull";
		follow.edgeYawOffset = breakWalk == "edgeyaw" ? 90.0 : 0.0;
		std::printf("NAVWALK pairs made=%zu spawnPairs=%zu onDeviceSkipped=%zu\n", pairs.size(), spawnPairs.size(), onDeviceSkipped);
		size_t reached = 0, stuck = 0, noPath = 0, timeout = 0, died = 0, wrongPlace = 0, notSettled = 0;
		size_t pullSamples = 0, pullOutside = 0;
		uint64_t walkTicks = 0, walkSlides = 0;		// 꺾임점 따라 걷는 틱 · 그 틱에 막는 면을 따라 미끄러진 수(이동 셈 MoveCounters::slides) — 여유 값을 가른다
		size_t needTeleport = 0, needPad = 0, needDrop = 0, reachedTeleport = 0, reachedPad = 0, reachedDrop = 0;
		double seconds = 0, worstSeconds = 0;
		mo::NavFollowCounters total;
		for (size_t i = 0; i < pairs.size(); ++i)
		{
			const WalkPair& p = pairs[i];
			needTeleport += p.usesTeleport ? 1 : 0;
			needPad += p.usesPad ? 1 : 0;
			needDrop += p.usesDrop ? 1 : 0;
			mo::MoveState state;
			mo::NavPlaceBody(setup.rehearsal, state, p.start);
			bool settled = false;
			for (uint32_t k = 0; k < 180 && !settled; ++k)
			{
				(void)mo::NavTickBody(setup.rehearsal, state, 0, 0, 0);
				settled = k >= 30 && state.mode == mo::EMoveMode::Walking && state.velocity.x == 0 && state.velocity.y == 0 && state.velocity.z == 0;
			}
			if (!settled)
			{
				++notSettled;
				std::printf("NAVWALK pair=%zu kind=%s from=%u to=%u result=not-settled\n", i, p.kind, p.from, p.to);
				continue;
			}
			mo::CNavFollower follower(mesh, graph, halfHeight, follow);
			follower.SetGoal(p.goal);
			mo::NavObserve o;
			o.location = state.location;
			o.mode = state.mode;
			uint64_t lastPlans = 0;
			uint32_t ticks = 0;
			bool fell = false;
			const size_t pullBefore = pullOutside;
			mo::ENavFollow st = mo::ENavFollow::Walking;
			for (; ticks < kWalkMaxTicks; ++ticks)
			{
				mo::NavCommand command;
				st = follower.Step(o, command);
				if (follower.Counters().plans != lastPlans)
				{
					lastPlans = follower.Counters().plans;
					const size_t outsideBefore = pullOutside;
					CheckPull(mesh, follower, pullSamples, pullOutside, i == tracePair);
					if (i == tracePair)
					{
						std::printf("NAVTRACE plan=%llu tick=%u outside=%zu corridor=", (unsigned long long)lastPlans, ticks, pullOutside - outsideBefore);
						for (const uint32_t f : follower.Corridor()) std::printf("%u,", f);
						std::printf(" points=");
						for (const mo::Vec3& point : follower.Points()) std::printf("(%.1f %.1f %.1f)", point.x, point.y, point.z);
						std::printf("\n");
					}
				}
				if (st == mo::ENavFollow::Arrived || st == mo::ENavFollow::GaveUp || st == mo::ENavFollow::NoPath) break;
				const uint64_t slidesBefore = setup.sim->GetCounters().slides;
				const mo::LauncherTickResult t = mo::NavTickBody(setup.rehearsal, state, command.moveX, command.moveY, command.yaw);
				if (st == mo::ENavFollow::Walking)
				{
					++walkTicks;
					walkSlides += setup.sim->GetCounters().slides - slidesBefore;
				}
				o.location = state.location;
				o.mode = state.mode;
				o.fired = t.fired;
				o.teleported = t.teleported;
				if (i == tracePair)
				{
					std::printf("NAVTRACE tick=%u state=%s in=(%.3f %.3f) yaw=%.1f at=(%.1f %.1f %.1f) v=(%.1f %.1f %.1f) mode=%s fired=%d teleported=%d\n", ticks,
						mo::NavFollowName(st), command.moveX, command.moveY, command.yaw, state.location.x, state.location.y, state.location.z, state.velocity.x, state.velocity.y,
						state.velocity.z, state.mode == mo::EMoveMode::Walking ? "walk" : "fall", t.fired, t.teleported);
				}
				if (state.location.z < setup.rules.fallKillZ)
				{
					fell = true;
					break;
				}
			}
			const mo::Vec3 feet = state.location - up;
			const int floor = mo::FindNavFloor(mesh, feet, 60.0, 60.0);
			const bool atGoal = floor == (int)p.to ||
				(std::hypot(state.location.x - p.goal.x, state.location.y - p.goal.y) <= kWalkGoalXY && std::fabs(state.location.z - p.goal.z) <= kWalkGoalDz);
			const char* result = "reached";
			if (fell) result = "died", ++died;
			else if (st == mo::ENavFollow::Arrived && atGoal) result = "reached", ++reached;
			else if (st == mo::ENavFollow::Arrived) result = "wrong-place", ++wrongPlace;
			else if (st == mo::ENavFollow::GaveUp) result = "stuck", ++stuck;
			else if (st == mo::ENavFollow::NoPath) result = "no-path", ++noPath;
			else result = "timeout", ++timeout;
			const double s = ticks / (double)mo::wire::kTickRate;
			if (std::strcmp(result, "reached") == 0)
			{
				seconds += s;
				worstSeconds = std::max(worstSeconds, s);
				reachedTeleport += p.usesTeleport ? 1 : 0;
				reachedPad += p.usesPad ? 1 : 0;
				reachedDrop += p.usesDrop ? 1 : 0;
			}
			const mo::NavFollowCounters& c = follower.Counters();
			total.plans += c.plans;
			total.noFloor += c.noFloor;
			total.farFloor += c.farFloor;
			total.noPath += c.noPath;
			total.stuckRepaths += c.stuckRepaths;
			total.landRepaths += c.landRepaths;
			total.edgeTimeouts += c.edgeTimeouts;
			total.unplannedEvents += c.unplannedEvents;
			total.unplannedFalls += c.unplannedFalls;
			total.flyTimeouts += c.flyTimeouts;
			total.portalMiss += c.portalMiss;
			for (int k = 0; k < 4; ++k)
			{
				total.edges[k] += c.edges[k];
				total.edgeMisses[k] += c.edgeMisses[k];
			}
			std::printf("NAVWALK pair=%zu kind=%s from=%u to=%u need=%s%s%s result=%s seconds=%.2f plans=%llu stuck=%llu edges=%llu/%llu/%llu unplanned=%llu pullOutside=%zu end=(%.1f %.1f %.1f) floor=%d\n", i,
				p.kind, p.from, p.to, p.usesTeleport ? "T" : "-", p.usesPad ? "P" : "-", p.usesDrop ? "D" : "-", result, s, (unsigned long long)c.plans,
				(unsigned long long)c.stuckRepaths, (unsigned long long)c.edges[(int)mo::ENavEdge::Teleport], (unsigned long long)c.edges[(int)mo::ENavEdge::Pad],
				(unsigned long long)c.edges[(int)mo::ENavEdge::Drop], (unsigned long long)(c.unplannedEvents + c.unplannedFalls), pullOutside - pullBefore, state.location.x, state.location.y, state.location.z, floor);
		}
		const size_t walked = pairs.size() - notSettled;
		std::printf("NAVWALK need teleport=%zu pad=%zu drop=%zu | reached teleport=%zu pad=%zu drop=%zu\n", needTeleport, needPad, needDrop, reachedTeleport, reachedPad, reachedDrop);
		std::printf("NAVWALK follow plans=%llu stuckRepaths=%llu landRepaths=%llu edgeTimeouts=%llu unplannedEvents=%llu unplannedFalls=%llu flyTimeouts=%llu noFloor=%llu farFloor=%llu noPath=%llu "
			"portalMiss=%zu edges teleport=%llu pad=%llu drop=%llu misses teleport=%llu pad=%llu drop=%llu\n", (unsigned long long)total.plans, (unsigned long long)total.stuckRepaths, (unsigned long long)total.landRepaths,
			(unsigned long long)total.edgeTimeouts, (unsigned long long)total.unplannedEvents, (unsigned long long)total.unplannedFalls, (unsigned long long)total.flyTimeouts,
			(unsigned long long)total.noFloor, (unsigned long long)total.farFloor, (unsigned long long)total.noPath, total.portalMiss, (unsigned long long)total.edges[(int)mo::ENavEdge::Teleport],
			(unsigned long long)total.edges[(int)mo::ENavEdge::Pad], (unsigned long long)total.edges[(int)mo::ENavEdge::Drop], (unsigned long long)total.edgeMisses[(int)mo::ENavEdge::Teleport],
			(unsigned long long)total.edgeMisses[(int)mo::ENavEdge::Pad], (unsigned long long)total.edgeMisses[(int)mo::ENavEdge::Drop]);
		std::printf("NAVWALK pull samples=%zu outside=%zu | walkTicks=%llu walkSlides=%llu\n", pullSamples, pullOutside, (unsigned long long)walkTicks, (unsigned long long)walkSlides);
		const bool ok = pairs.size() == pairCount && notSettled == 0 && reached * 100 >= walked * 95 && pullOutside == 0 && portalFound == portalEdges;
		std::printf("NAVWALK pairs=%zu reached=%zu stuck=%zu noPath=%zu timeout=%zu died=%zu wrongPlace=%zu notSettled=%zu meanSeconds=%.2f worstSeconds=%.2f margin=%.1f break=%s %s\n",
			pairs.size(), reached, stuck, noPath, timeout, died, wrongPlace, notSettled, reached ? seconds / reached : 0.0, worstSeconds, margin, breakWalk.empty() ? "-" : breakWalk.c_str(),
			ok ? "PASS" : "FAIL");
		return ok;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: NavCheck <nav.monv> [--pairs <pairs.csv>] [--break cut|join] [--load-test] | <nav.monv> --graph --map <mocl> --launchers <txt> --teleports <txt> [--pad-plans <봇 기록>] [--teleport-plans <봇 기록>] | <nav.monv> --walk --map <mocl> --launchers <txt> --teleports <txt> [--pairs-count N] [--margin cm] [--break-walk straight|nopull|edgeyaw] [--trace 쌍]\n");
		return 2;
	}
	std::string pairsPath = std::string(argv[1]) + ".pairs.csv";
	std::string breakMode;
	bool loadTest = false, graphMode = false, walkMode = false;
	size_t walkPairs = 300, tracePair = (size_t)-1;
	double walkMargin = mo::NavFollowParams().margin;
	std::string breakWalk;
	const char* mapPath = nullptr;
	const char* launchersPath = nullptr;
	const char* teleportsPath = nullptr;
	const char* padPlans = nullptr;
	const char* teleportPlans = nullptr;
	for (int i = 2; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--pairs") == 0 && i + 1 < argc) pairsPath = argv[++i];
		else if (std::strcmp(argv[i], "--break") == 0 && i + 1 < argc) breakMode = argv[++i];
		else if (std::strcmp(argv[i], "--load-test") == 0) loadTest = true;
		else if (std::strcmp(argv[i], "--graph") == 0) graphMode = true;
		else if (std::strcmp(argv[i], "--walk") == 0) walkMode = true;
		else if (std::strcmp(argv[i], "--pairs-count") == 0 && i + 1 < argc) walkPairs = (size_t)std::strtoul(argv[++i], nullptr, 10);
		else if (std::strcmp(argv[i], "--margin") == 0 && i + 1 < argc) walkMargin = std::strtod(argv[++i], nullptr);
		else if (std::strcmp(argv[i], "--break-walk") == 0 && i + 1 < argc) breakWalk = argv[++i];
		else if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) tracePair = (size_t)std::strtoul(argv[++i], nullptr, 10);
		else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) mapPath = argv[++i];
		else if (std::strcmp(argv[i], "--launchers") == 0 && i + 1 < argc) launchersPath = argv[++i];
		else if (std::strcmp(argv[i], "--teleports") == 0 && i + 1 < argc) teleportsPath = argv[++i];
		else if (std::strcmp(argv[i], "--pad-plans") == 0 && i + 1 < argc) padPlans = argv[++i];
		else if (std::strcmp(argv[i], "--teleport-plans") == 0 && i + 1 < argc) teleportPlans = argv[++i];
		else
		{
			std::printf("NAVCHECK bad argument %s\n", argv[i]);
			return 2;
		}
	}
	if (graphMode && (!mapPath || !launchersPath || !teleportsPath))
	{
		std::printf("NAVCHECK --graph needs --map --launchers --teleports\n");
		return 2;
	}
	if (walkMode && (!mapPath || !launchersPath || !teleportsPath || walkPairs == 0 || (!breakWalk.empty() && breakWalk != "straight" && breakWalk != "nopull" && breakWalk != "edgeyaw")))
	{
		std::printf("NAVCHECK --walk needs --map --launchers --teleports (--pairs-count > 0 · --break-walk straight|nopull|edgeyaw)\n");
		return 2;
	}
	if (!breakMode.empty() && breakMode != "cut" && breakMode != "join")
	{
		std::printf("NAVCHECK bad --break %s (cut|join)\n", breakMode.c_str());
		return 2;
	}
	std::vector<uint8_t> bytes;
	if (!ReadFile(argv[1], bytes))
	{
		std::printf("NAVCHECK cannot read %s\n", argv[1]);
		return 1;
	}
	mo::NavMesh mesh;
	const mo::ENavLoad loaded = mo::ParseNav(bytes.data(), bytes.size(), mesh);
	if (loaded != mo::ENavLoad::Ok)
	{
		std::printf("NAVCHECK load=%s FAIL\n", mo::NavLoadName(loaded));
		return 1;
	}
	size_t ground = 0;
	for (const mo::NavNode& node : mesh.nodes) ground += node.kind == 0 ? 1 : 0;
	std::printf("NAVCHECK loaded version=%u nodes=%zu ground=%zu linkNodes=%zu links=%zu bytes=%zu agentRadius=%.1f agentHeight=%.1f\n", mesh.version, mesh.nodes.size(), ground,
		mesh.nodes.size() - ground, mesh.linkCount, bytes.size(), mesh.agentRadius, mesh.agentHeight);
	if (graphMode) return RunGraph(mesh, mapPath, launchersPath, teleportsPath, padPlans, teleportPlans) ? 0 : 1;
	if (walkMode) return RunWalk(mesh, mapPath, launchersPath, teleportsPath, walkMargin, walkPairs, breakWalk, tracePair) ? 0 : 1;
	bool ok = true;
	if (loadTest) ok = LoadTest(bytes, mesh) && ok;

	std::vector<std::vector<uint32_t>> groups = Components(mesh);
	if (!breakMode.empty())
	{
		Corrupt(mesh, breakMode);
		groups = Components(mesh);
	}
	std::printf("NAVCHECK groups=%zu largest=%zu\n", groups.size(), groups.empty() ? (size_t)0 : groups[0].size());

	std::vector<Pair> pairs;
	if (!ReadPairs(pairsPath.c_str(), pairs) || pairs.empty())
	{
		std::printf("NAVCHECK cannot read pairs %s FAIL\n", pairsPath.c_str());
		return 1;
	}
	// 종류별 · (우리 길 있음, 언리얼 길 있음) 칸 수 — check_nav.py compare 와 같은 줄
	std::map<std::string, std::array<size_t, 4>> table;
	size_t badPair = 0, pathBad = 0, mismatch = 0;
	std::vector<double> ratios;
	std::vector<uint32_t> path;
	for (const Pair& pair : pairs)
	{
		if (pair.a >= ground || pair.b >= ground)
		{
			++badPair;
			continue;
		}
		double length = 0;
		const bool ours = mo::FindNavPath(mesh, pair.a, pair.b, path, length);
		const bool unreal = pair.result == kUnrealSuccess;
		std::array<size_t, 4>& row = table[pair.kind];
		++row[(ours ? 0 : 2) + (unreal ? 0 : 1)];
		if (ours != unreal) ++mismatch;
		if (!ours) continue;
		// 돌려받은 길이 이어진 노드인가 · 거리 합이 맞나(함수 자체 검사)
		double sum = 0;
		bool chain = !path.empty() && path.front() == pair.a && path.back() == pair.b;
		for (size_t i = 0; chain && i + 1 < path.size(); ++i)
		{
			const std::vector<uint32_t>& next = mesh.nodes[path[i]].next;
			chain = std::find(next.begin(), next.end(), path[i + 1]) != next.end();
			const mo::Vec3 d = mesh.nodes[path[i + 1]].center - mesh.nodes[path[i]].center;
			sum += std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		}
		if (!chain || std::fabs(sum - length) > 1e-6) ++pathBad;
		if (unreal && pair.length > 0) ratios.push_back(length / pair.length);
	}
	for (auto& kv : table)
	{
		const std::array<size_t, 4>& row = kv.second;
		std::printf("NAVCHECK pairs kind=%s bothYes=%zu bothNo=%zu graphYesUnrealNo=%zu graphNoUnrealYes=%zu\n", kv.first.c_str(), row[0], row[3], row[1], row[2]);
	}
	// 길이는 판정에 안 쓴다 — 우리 = 노드 중심 이음(곧게 펴기 전, n-3) · 언리얼 = 당긴 줄. 둘 다 길이 있는 쌍(같은 바닥 쌍은 언리얼 길이 0 이라 뺌)의 비만 본다
	std::sort(ratios.begin(), ratios.end());
	if (!ratios.empty())
	{
		std::printf("NAVCHECK length measured=%zu oursOverUnreal min=%.3f p50=%.3f max=%.3f\n", ratios.size(), ratios.front(), ratios[ratios.size() / 2], ratios.back());
	}
	ok = ok && mismatch == 0 && badPair == 0 && pathBad == 0;
	std::printf("NAVCHECK pairs=%zu mismatch=%zu badPair=%zu pathBad=%zu %s\n", pairs.size(), mismatch, badPair, pathBad, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
