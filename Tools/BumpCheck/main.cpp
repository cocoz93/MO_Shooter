// 부딪힘 대조 도구 — Lyra 연습장 부딪힘 기록과 Shared 이동(다른 사람 캡슐을 보는 CMoveSim)을 맞춰 본다(컨텐츠 F-2).
//
//   BumpCheck <map_gym.mocl> <cprobe_<tag>.csv> [<cprobe_<tag2>.csv> ...]
//
// 기록 = Tools/LyraPilot/record_content_probe.ps1 -Mode bump. 칸 이름이 bump_ 로 시작하는 줄만 쓴다.
// 칸마다 첫 줄의 두 사람 자리 · 속도에서 시작해, 줄마다 Lyra 와 같은 입력(나 walk → +X, 상대 foeWalk → −X — 그 줄 프레임에 넣은 누름을
// 다음 줄 이동이 먹는다)으로 한 틱씩 굴린다. 둘 다 걷는 칸은 기록에서 읽은 차례대로 움직인다(D80 — 닿는 줄에서 속도가 큰 쪽이 먼저).
// 움직이는 쪽은 다른 쪽의 「지금」 캡슐을 본다 — 먼저 움직인 쪽은 이 틱 새 자리, 나중 쪽은 앞 틱 자리를 본다(Lyra 한 실행과 같은 꼴).
// 줄마다 두 사람 자리 · 수평 속도 차이를 본다. 합격 = 0.01cm · 0.01cm/s 안(이동 대조 over0.01 · 밀기 대조 kPushPosTolerance 와 같은 선)
// 끝 코드: 0 = 전부 통과, 1 = 불일치, 2 = 입력 오류
#include "MapCollision.h"
#include "Movement.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace mo;

namespace
{
	constexpr float kFrameSeconds = 1.f / 60.f;		// 기록은 고정 프레임 60(-fps=60 -UseFixedTimeStep)
	constexpr double kPosTolerance = 0.01;
	constexpr double kVelTolerance = 0.01;

	bool ReadFile(const char* path, std::vector<char>& out)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return false;
		out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return !out.empty();
	}

	struct Table
	{
		std::vector<std::string> header;
		std::vector<std::vector<std::string>> rows;
		int Col(const char* name) const
		{
			const auto it = std::find(header.begin(), header.end(), std::string(name));
			return it == header.end() ? -1 : (int)(it - header.begin());
		}
	};

	bool ReadCsv(const char* path, Table& out)
	{
		std::ifstream in(path);
		if (!in) return false;
		std::string line;
		const auto split = [](const std::string& s)
		{
			std::vector<std::string> cells;
			std::istringstream ss(s);
			std::string cell;
			while (std::getline(ss, cell, ',')) cells.push_back(cell);
			return cells;
		};
		if (!std::getline(in, line)) return false;
		if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);
		out.header = split(line);
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			out.rows.push_back(split(line));
		}
		return true;
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("사용: BumpCheck <map_gym.mocl> <cprobe_<tag>.csv> [...]\n");
		return 2;
	}
	std::vector<char> mapData;
	CMapCollision map;
	if (!ReadFile(argv[1], mapData) || map.LoadFromMemory(mapData.data(), mapData.size()) != EMapLoad::Ok)
	{
		std::printf("BUMPCHECK FAIL 맵을 못 읽음 %s\n", argv[1]);
		return 2;
	}
	const MoveParams params = MakeLyraMoveParams();
	const char* traceStep = std::getenv("BUMP_TRACE");		// 칸 이름 — 그 칸은 줄마다 차이를 찍는다(원인 찾기용)
	const bool oneStep = std::getenv("BUMP_ONESTEP") != nullptr;	// 줄마다 Lyra 상태에서 한 틱만(원인 찾기용)
	const double perturbY = std::getenv("BUMP_PERTURB") ? std::atof(std::getenv("BUMP_PERTURB")) : 0.0;	// 내 시작 y 를 이만큼 흔든다(원인 찾기용)
	CMoveSim sim(map, params);

	int cells = 0, fails = 0, walkCells = 0, flippedFirst = 0, rowsCompared = 0;
	double worstPos = 0, worstVel = 0;
	for (int f = 2; f < argc; ++f)
	{
		Table t;
		if (!ReadCsv(argv[f], t))
		{
			std::printf("BUMPCHECK FAIL 기록을 못 읽음 %s\n", argv[f]);
			return 2;
		}
		const int cStep = t.Col("step"), cPx = t.Col("px"), cPy = t.Col("py"), cPz = t.Col("pz"), cVx = t.Col("vx"), cVy = t.Col("vy"), cVz = t.Col("vz");
		const int cCrouched = t.Col("crouched"), cFx = t.Col("foeX"), cFy = t.Col("foeY"), cFz = t.Col("foeZ"), cFhh = t.Col("foeHH");
		const int cWalk = t.Col("walk"), cFoeWalk = t.Col("foeWalk"), cFvx = t.Col("foeVx"), cFvy = t.Col("foeVy");
		const int cYaw = t.Col("yaw"), cFyaw = t.Col("foeYaw");
		if (cStep < 0 || cPx < 0 || cPy < 0 || cPz < 0 || cVx < 0 || cVy < 0 || cVz < 0 || cCrouched < 0 || cFx < 0 || cFy < 0 || cFz < 0 || cFhh < 0
			|| cWalk < 0 || cFoeWalk < 0 || cFvx < 0 || cFvy < 0 || cYaw < 0 || cFyaw < 0)
		{
			std::printf("BUMPCHECK FAIL 기록 표 머리에 부딪힘 칸(step · px.. · foeX.. · walk · foeWalk · foeVx · foeVy · yaw · foeYaw)이 없음 %s\n", argv[f]);
			return 2;
		}
		const auto num = [](const std::vector<std::string>& row, int col) { return (int)row.size() > col ? std::atof(row[(size_t)col].c_str()) : 0.0; };

		// 칸마다 줄 모으기
		std::vector<std::pair<std::string, std::vector<size_t>>> steps;
		for (size_t r = 0; r < t.rows.size(); ++r)
		{
			if ((int)t.rows[r].size() <= cFvy) continue;
			const std::string& name = t.rows[r][(size_t)cStep];
			if (name.rfind("bump_", 0) != 0) continue;
			if (steps.empty() || steps.back().first != name) steps.push_back({ name, {} });
			steps.back().second.push_back(r);
		}

		for (const auto& step : steps)
		{
			const std::vector<size_t>& idx = step.second;
			if (idx.size() < 2) continue;
			const auto& row0 = t.rows[idx[0]];
			const bool foeWalks = num(row0, cFoeWalk) != 0.0;

			// 차례 — 닿는 줄(두 반지름 + 1cm 안)에서 속도가 큰 쪽이 먼저(D80, check_bump.py first 와 같은 식)
			bool foeFirst = false;
			if (foeWalks)
			{
				for (size_t k : idx)
				{
					const auto& row = t.rows[k];
					if (std::hypot(num(row, cPx) - num(row, cFx), num(row, cPy) - num(row, cFy)) > 2.0 * params.capsuleRadius + 1.0) continue;
					foeFirst = std::hypot(num(row, cFvx), num(row, cFvy)) > std::hypot(num(row, cVx), num(row, cVy)) + 0.01;
					break;
				}
				++walkCells;
				if (foeFirst) ++flippedFirst;
			}

			MoveState me, foe;
			me.location = Vec3(num(row0, cPx), num(row0, cPy) + perturbY, num(row0, cPz));
			me.velocity = Vec3(num(row0, cVx), num(row0, cVy), num(row0, cVz));
			me.lastUpdateVelocity = me.velocity;
			me.mode = EMoveMode::Walking;
			me.crouched = me.wantsToCrouch = num(row0, cCrouched) != 0.0;
			me.maxSpeed = params.maxWalkSpeed;
			foe.location = Vec3(num(row0, cFx), num(row0, cFy), num(row0, cFz));
			foe.velocity = Vec3(num(row0, cFvx), num(row0, cFvy), 0.0);
			foe.lastUpdateVelocity = foe.velocity;
			foe.mode = EMoveMode::Walking;
			foe.crouched = foe.wantsToCrouch = num(row0, cFhh) < params.capsuleHalfHeight - 0.5;
			foe.maxSpeed = params.maxWalkSpeed;
			sim.SetPawns(nullptr, 0);
			sim.RefreshFloor(me);
			sim.RefreshFloor(foe);

			const auto capsuleOf = [&](const MoveState& s, double yaw)
			{
				PawnCapsule c;
				c.center = s.location;
				c.radius = params.capsuleRadius;
				c.halfHeight = sim.HalfHeightOf(s);
				c.yaw = (float)yaw;		// 엔진은 맞는 쪽 지역 좌표에서 단정밀도로 푼다(F-2). 이 기록은 0 · 180 도뿐이라 결과에 영향 없음
				return c;
			};
			const uint64_t hitsBefore = sim.GetCounters().pawnHits;
			uint64_t traceHits = hitsBefore, traceSlides = sim.GetCounters().slides, traceTwoWalls = sim.GetCounters().twoWalls;
			double maxPos = 0, maxVel = 0;
			size_t firstBad = 0;
			bool fail = false;
			for (size_t k = 1; k < idx.size(); ++k)
			{
				const auto& prev = t.rows[idx[k - 1]];
				const auto& row = t.rows[idx[k]];
				const Vec3 meInput = num(prev, cWalk) != 0.0 ? Vec3(1, 0, 0) : Vec3();
				const Vec3 foeInput = num(prev, cFoeWalk) != 0.0 ? Vec3(-1, 0, 0) : Vec3();
				if (oneStep)
				{
					// 원인 찾기 — 앞 줄의 Lyra 상태(소수 셋째 자리 기록)에서 다시 시작해 한 틱만 굴린다
					me.location = Vec3(num(prev, cPx), num(prev, cPy), num(prev, cPz));
					me.velocity = me.lastUpdateVelocity = Vec3(num(prev, cVx), num(prev, cVy), num(prev, cVz));
					foe.location = Vec3(num(prev, cFx), num(prev, cFy), num(prev, cFz));
					foe.velocity = foe.lastUpdateVelocity = Vec3(num(prev, cFvx), num(prev, cFvy), 0.0);
				}
				const MoveState meStart = me;
				const auto moveMe = [&]
				{
					const PawnCapsule other = capsuleOf(foe, num(prev, cFyaw));
					sim.SetPawns(&other, 1);
					sim.Tick(me, meInput, kFrameSeconds);
					sim.SetPawns(nullptr, 0);
				};
				const auto moveFoe = [&]
				{
					if (!foeWalks) return;		// 서 있는 상대는 Lyra 에서도 안 움직였다(F-1 — 밀림 0.00)
					const PawnCapsule other = capsuleOf(me, num(prev, cYaw));
					sim.SetPawns(&other, 1);
					sim.Tick(foe, foeInput, kFrameSeconds);
					sim.SetPawns(nullptr, 0);
				};
				if (foeFirst) { moveFoe(); moveMe(); }
				else { moveMe(); moveFoe(); }

				const Vec3 lyraMe(num(row, cPx), num(row, cPy), num(row, cPz));
				const Vec3 lyraFoe(num(row, cFx), num(row, cFy), num(row, cFz));
				const double dMe = std::sqrt((me.location.x - lyraMe.x) * (me.location.x - lyraMe.x) + (me.location.y - lyraMe.y) * (me.location.y - lyraMe.y) + (me.location.z - lyraMe.z) * (me.location.z - lyraMe.z));
				const double dFoe = std::sqrt((foe.location.x - lyraFoe.x) * (foe.location.x - lyraFoe.x) + (foe.location.y - lyraFoe.y) * (foe.location.y - lyraFoe.y) + (foe.location.z - lyraFoe.z) * (foe.location.z - lyraFoe.z));
				const double vMe = std::hypot(me.velocity.x - num(row, cVx), me.velocity.y - num(row, cVy));
				const double vFoe = foeWalks ? std::hypot(foe.velocity.x - num(row, cFvx), foe.velocity.y - num(row, cFvy)) : 0.0;
				const double dp = std::max(dMe, dFoe), dv = std::max(vMe, vFoe);
				maxPos = std::max(maxPos, dp);
				maxVel = std::max(maxVel, dv);
				if ((dp > kPosTolerance || dv > kVelTolerance) && !fail)
				{
					fail = true;
					firstBad = k;
					std::printf("BUMP_FIRST_DIFF file=%d step=%s row=%zu me=%.3f,%.3f,%.3f lyra=%.3f,%.3f,%.3f foe=%.3f,%.3f lyraFoe=%.3f,%.3f vMe=%.3f,%.3f lyraV=%.3f,%.3f\n",
						f - 1, step.first.c_str(), k, me.location.x, me.location.y, me.location.z, lyraMe.x, lyraMe.y, lyraMe.z, foe.location.x, foe.location.y, lyraFoe.x, lyraFoe.y,
						me.velocity.x, me.velocity.y, num(row, cVx), num(row, cVy));
				}
				if (traceStep && step.first == traceStep && f == 2)
				{
					const MoveCounters& now = sim.GetCounters();
					std::printf("TRACE row=%zu gap=%.4f mine=%.4f me=%.5f,%.5f lyra=%.5f,%.5f v=%.4f,%.4f lyraV=%.4f,%.4f dp=%.5f dv=%.4f hits=%llu slides=%llu twoWalls=%llu\n", k,
						std::hypot(lyraMe.x - lyraFoe.x, lyraMe.y - lyraFoe.y), std::hypot(me.location.x - foe.location.x, me.location.y - foe.location.y),
						me.location.x, me.location.y, lyraMe.x, lyraMe.y, me.velocity.x, me.velocity.y, num(row, cVx), num(row, cVy), dp, dv,
						(unsigned long long)(now.pawnHits - traceHits), (unsigned long long)(now.slides - traceSlides), (unsigned long long)(now.twoWalls - traceTwoWalls));
					if (oneStep && !foeWalks)
					{
						// 사람 없이 굴린 끝 - 실제 끝 = (1 - 맞은 시각) x (이동 · 법선) x 법선 — 방향이 미끄러진 면의 법선, 길이가 깎인 양
						MoveState fr = meStart;
						sim.Tick(fr, meInput, kFrameSeconds);
						const double lx = fr.location.x - lyraMe.x, ly = fr.location.y - lyraMe.y;
						const double mx = fr.location.x - me.location.x, my = fr.location.y - me.location.y;
						const double gx = meStart.location.x - foe.location.x, gy = meStart.location.y - foe.location.y;
						const double kDeg = 57.29577951308232;
						std::printf("IMPLIED row=%zu startGap=%.5f lyraN=%.4f mineN=%.4f startN=%.4f dN_mdeg=%.2f lyraCut=%.5f mineCut=%.5f dCut=%.5f\n", k, std::hypot(gx, gy),
							std::atan2(ly, lx) * kDeg, std::atan2(my, mx) * kDeg, std::atan2(gy, gx) * kDeg, (std::atan2(ly, lx) - std::atan2(my, mx)) * kDeg * 1000.0,
							std::hypot(lx, ly), std::hypot(mx, my), std::hypot(lx, ly) - std::hypot(mx, my));
					}
					traceHits = sim.GetCounters().pawnHits;
					traceSlides = sim.GetCounters().slides;
					traceTwoWalls = sim.GetCounters().twoWalls;
				}
				++rowsCompared;
			}
			const uint64_t hits = sim.GetCounters().pawnHits - hitsBefore;
			++cells;
			if (fail) ++fails;
			worstPos = std::max(worstPos, maxPos);
			worstVel = std::max(worstVel, maxVel);
			std::printf("BUMP %s file=%d step=%s rows=%zu foeWalk=%d first=%s pawnHits=%llu maxPosDiff=%.4f maxVelDiff=%.4f firstBadRow=%zu\n", fail ? "DIFF" : "ok", f - 1, step.first.c_str(),
				idx.size(), foeWalks ? 1 : 0, foeWalks ? (foeFirst ? "foe" : "me") : "-", (unsigned long long)hits, maxPos, maxVel, firstBad);
		}
	}
	const MoveCounters& mc = sim.GetCounters();
	std::printf("BUMP 계수 pawnHits=%llu pawnStartPenetrating=%llu pawnOddSimplex=%llu pawnTouchStart=%llu slides=%llu twoWalls=%llu stepUpTried=%llu walkCells=%d foeFirst=%d rows=%d\n", (unsigned long long)mc.pawnHits,
		(unsigned long long)mc.pawnStartPenetrating, (unsigned long long)mc.pawnOddSimplex, (unsigned long long)mc.pawnTouchStart, (unsigned long long)mc.slides, (unsigned long long)mc.twoWalls, (unsigned long long)mc.stepUpTried,
		walkCells, flippedFirst, rowsCompared);
	// pawnOddSimplex — 엔진 계산을 옮기지 않은 갈래(퇴화하지 않는 삼각형 · 핵끼리 맞닿은 깊은 겹침)를 탄 횟수. 0 이 아니면 그 쓸기는 정확한 식으로 갈음돼 대조 근거가 없다
	// pawnTouchStart — 처음부터 여유 안으로 끝나 엔진 IsDone 갈래를 옮긴 대로 탄 횟수(이 기록에서 0 이면 그 갈래는 Lyra 기록과 대조되지 않은 것)
	const bool pass = cells > 0 && fails == 0 && mc.pawnHits > 0 && mc.pawnOddSimplex == 0;
	std::printf("BUMPCHECK %s cells=%d fails=%d maxPosDiff=%.4f maxVelDiff=%.4f\n", pass ? "PASS" : "FAIL", cells, fails, worstPos, worstVel);
	return pass ? 0 : 1;
}
