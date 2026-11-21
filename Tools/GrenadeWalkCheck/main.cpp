// 수류탄 위 걸음 대조 도구 — Lyra 걸음 칸(멈춘 수류탄 위를 지나감)을 Shared 이동(산 수류탄 몸을 올라설 수 있는 맵 몸으로 보는 CMoveSim)으로 다시 굴려 맞춰 본다(L-8-b).
//
//   GrenadeWalkCheck <map_gym.mocl> <기록 폴더> <tag> [<tag> ...]
//
// 기록 = cprobe_<tag>.log 의 MOB_CPROBE_GWALK 줄(칸 · 프레임마다 내 자리 · 속도 · 입력 · 바닥이 수류탄인가) + cprobe_<tag>_gren.csv(프레임마다 수류탄 자세).
// 칸마다 첫 줄 자리 · 속도에서 시작해 줄마다 앞 줄 입력(1 → +X)으로 한 틱씩 굴린다. 걸음이 보는 수류탄 = 앞 줄 프레임에 살아 있는(hidden 0) 수류탄 —
// 서버는 사람 걸음 뒤에 수류탄이 걸어 사람은 지난 틱 끝 수류탄 자리를 본다(Room::Tick 차례). 이번 기록은 멈춘 수류탄뿐이라 어느 프레임 자세인지 못 가른다.
// 대조군 — 수류탄 위를 지나는 칸(Lyra 바닥이 수류탄인 줄이 있음)은 수류탄을 빼고, 안 지나는 칸은 지나는 칸의 수류탄을 넣고 다시 굴려 어긋나야 한다(두 방향).
// 합격 = 줄마다 위치 · 속도 차 0.01 안 · 들린 줄 수 · 바닥이 수류탄인 줄 수 · 걸음이 수류탄에 막힌 프레임(MOB_CPROBE_HIT)이 Lyra 와 같음 · 대조군이 어긋남.
// 막힌 프레임은 위치가 같아도 갈래가 다를 수 있어 따로 본다 — 이동 쓸기가 수류탄을 안 보면 비탈 다시 쓸기 없이 바닥 찾기만으로 같은 자리에 닿는다(L-8-b-1 돌연변이 ①).
// 끝 코드: 0 = 전부 통과, 1 = 불일치, 2 = 입력 오류
#include "Grenade.h"
#include "MapCollision.h"
#include "Movement.h"
#include "Projectile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
	constexpr double kLiftEpsilon = 0.01;			// 첫 줄 높이보다 이만큼 위면 들림

	struct WalkRow
	{
		int idx = 0;
		int frame = 0;
		int onGren = 0;
		int input = 0;
		Vec3 location;
		Vec3 velocity;
	};

	struct GrenadePose
	{
		uint32_t id = 0;		// 이름 B_Grenade_C_<n> 의 n
		Vec3 location;
		Rotator rotation;
	};

	bool ReadFile(const std::string& path, std::vector<char>& out)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return false;
		out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return !out.empty();
	}

	// 「 key=값」의 값(공백까지). 없으면 빈 값
	std::string Field(const std::string& line, const char* key)
	{
		const std::string k = std::string(" ") + key + "=";
		const size_t at = line.find(k);
		if (at == std::string::npos) return std::string();
		const size_t from = at + k.size();
		const size_t to = line.find(' ', from);
		return line.substr(from, to == std::string::npos ? std::string::npos : to - from);
	}

	bool ParseVec(const std::string& text, Vec3& out)
	{
		return std::sscanf(text.c_str(), "%lf,%lf,%lf", &out.x, &out.y, &out.z) == 3;
	}

	// 칸 이름 → 줄(기록 차례). grenadeHits = 칸 이름 → 내 걸음이 수류탄에 막힌 프레임(MOB_CPROBE_HIT who=me other=B_Grenade… — 사람 OnActorHit,
	// 엔진 MoveComponent 가 막는 맞음마다 부름 · 원본 기준). 우리 쪽 짝 = CMoveSim::MoveComponent 의 standableHits
	bool LoadWalk(const std::string& path, std::map<std::string, std::vector<WalkRow>>& cells, std::vector<std::string>& order,
		std::map<std::string, std::vector<int>>& grenadeHits)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return false;
		std::string line;
		while (std::getline(in, line))
		{
			if (line.find("MOB_CPROBE_HIT ") != std::string::npos)
			{
				const std::string body = line.substr(line.find("MOB_CPROBE_HIT ") + std::strlen("MOB_CPROBE_HIT"));
				if (Field(body, "who") == "me" && Field(body, "other").rfind("B_Grenade", 0) == 0)
					grenadeHits[Field(body, "step")].push_back(std::atoi(Field(body, "frame").c_str()));
				continue;
			}
			if (line.find("MOB_CPROBE_GWALK step=") == std::string::npos) continue;
			const size_t tag = line.find("MOB_CPROBE_GWALK ");
			const std::string body = line.substr(tag + std::strlen("MOB_CPROBE_GWALK") );
			WalkRow r;
			const std::string step = Field(body, "step");
			if (step.empty() || !ParseVec(Field(body, "me"), r.location) || !ParseVec(Field(body, "vel"), r.velocity)) return false;
			r.idx = std::atoi(Field(body, "idx").c_str());
			r.frame = std::atoi(Field(body, "frame").c_str());
			r.onGren = std::atoi(Field(body, "onGren").c_str());
			r.input = std::atoi(Field(body, "input").c_str());
			if (cells.find(step) == cells.end()) order.push_back(step);
			cells[step].push_back(r);
		}
		return true;
	}

	// 프레임 → 그 프레임 끝 수류탄 자세(hidden 0 만)
	bool LoadGrenades(const std::string& path, std::map<int, std::vector<GrenadePose>>& byFrame)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return false;
		std::string line;
		std::getline(in, line);		// 머리줄 frame,name,age,x,y,z,vx,vy,vz,pitch,yaw,roll,hidden,...
		while (std::getline(in, line))
		{
			std::vector<std::string> c;
			std::istringstream ss(line);
			std::string cell;
			while (std::getline(ss, cell, ',')) c.push_back(cell);
			if (c.size() < 13) return false;
			if (std::atoi(c[12].c_str()) != 0) continue;
			GrenadePose g;
			const size_t under = c[1].rfind('_');
			g.id = under == std::string::npos ? 0u : (uint32_t)std::strtoul(c[1].c_str() + under + 1, nullptr, 10);
			g.location = Vec3(std::atof(c[3].c_str()), std::atof(c[4].c_str()), std::atof(c[5].c_str()));
			g.rotation.pitch = std::atof(c[9].c_str());
			g.rotation.yaw = std::atof(c[10].c_str());
			g.rotation.roll = std::atof(c[11].c_str());
			byFrame[std::atoi(c[0].c_str())].push_back(g);
		}
		return true;
	}

	enum class EMode { Real, NoGrenade, Inject };

	struct WalkResult
	{
		double maxPos = 0, maxVel = 0, maxLift = 0, lyraMaxLift = 0;
		int firstBad = -1, lifted = 0, lyraLifted = 0, bothLifted = 0, onStandable = 0, lyraOnGren = 0, bodies = 0;
		std::vector<int> hitFrames;		// 걸음이 산 수류탄 몸에 막힌 틱의 프레임(standableHits 가 오른 틱)
		MoveCounters counters;
	};

	WalkResult Run(const CMapCollision& map, const std::vector<WalkRow>& rows, const std::map<int, std::vector<GrenadePose>>& grenades,
		EMode mode, const std::vector<MapCapsule>& injected)
	{
		const ProjectileParams grenadeParams{};
		const MoveParams params = MakeLyraMoveParams();
		CMoveSim sim(map, params);
		std::vector<MapCapsule> standables;
		const auto setFrame = [&](int frame)
		{
			standables.clear();
			if (mode == EMode::Inject) standables = injected;
			else if (mode == EMode::Real)
			{
				const auto it = grenades.find(frame);
				if (it != grenades.end())
					for (const GrenadePose& g : it->second) standables.push_back(GrenadeStandable(g.location, g.rotation, grenadeParams, g.id));
			}
			sim.SetStandables(standables.data(), standables.size());
		};

		WalkResult out;
		MoveState me;
		me.location = rows[0].location;
		me.velocity = me.lastUpdateVelocity = rows[0].velocity;
		me.mode = EMoveMode::Walking;
		me.maxSpeed = params.maxWalkSpeed;
		setFrame(rows[0].frame);
		sim.RefreshFloor(me);
		const double baseZ = rows[0].location.z;
		for (size_t k = 1; k < rows.size(); ++k)
		{
			setFrame(rows[k - 1].frame);
			out.bodies += (int)standables.size();
			const Vec3 input = rows[k - 1].input ? Vec3(1, 0, 0) : Vec3();
			const uint64_t hitsBefore = sim.GetCounters().standableHits;
			sim.Tick(me, input, kFrameSeconds);
			const WalkRow& r = rows[k];
			if (sim.GetCounters().standableHits != hitsBefore) out.hitFrames.push_back(r.frame);
			const Vec3 dp = me.location - r.location, dv = me.velocity - r.velocity;
			const double pos = std::sqrt(dp.x * dp.x + dp.y * dp.y + dp.z * dp.z);
			const double vel = std::sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
			if (pos > out.maxPos) out.maxPos = pos;
			if (vel > out.maxVel) out.maxVel = vel;
			if ((pos > kPosTolerance || vel > kVelTolerance) && out.firstBad < 0) out.firstBad = r.idx;
			const double lift = me.location.z - baseZ, lyraLift = r.location.z - baseZ;
			const bool l1 = lift > kLiftEpsilon, l2 = lyraLift > kLiftEpsilon;
			out.lifted += l1;
			out.lyraLifted += l2;
			out.bothLifted += (l1 && l2);
			if (lift > out.maxLift) out.maxLift = lift;
			if (lyraLift > out.lyraMaxLift) out.lyraMaxLift = lyraLift;
			out.onStandable += (me.floor.IsWalkableFloor() && me.floor.hit.standable) ? 1 : 0;
			out.lyraOnGren += r.onGren;
		}
		out.counters = sim.GetCounters();
		return out;
	}

	std::string FrameList(const std::vector<int>& frames)
	{
		std::string text;
		for (int f : frames) text += (text.empty() ? "" : ",") + std::to_string(f);
		return text.empty() ? std::string("-") : text;
	}

	void Print(const char* tag, const std::string& step, const char* mode, size_t rows, const WalkResult& w, const std::vector<int>& lyraHits)
	{
		const MoveCounters& c = w.counters;
		std::printf("GWALK tag=%s step=%s mode=%s rows=%zu bodies=%d maxPosDiff=%.5f maxVelDiff=%.5f firstBadIdx=%d lifted=%d lyraLifted=%d both=%d maxLift=%.4f lyraMaxLift=%.4f"
			" onStandable=%d lyraOnGren=%d hitFrames=%s lyraHitFrames=%s standableHits=%llu rampSecond=%llu stepUpOk=%llu floorLine=%llu\n",
			tag, step.c_str(), mode, rows, w.bodies, w.maxPos, w.maxVel, w.firstBad, w.lifted, w.lyraLifted, w.bothLifted, w.maxLift, w.lyraMaxLift,
			w.onStandable, w.lyraOnGren, FrameList(w.hitFrames).c_str(), FrameList(lyraHits).c_str(), (unsigned long long)c.standableHits, (unsigned long long)c.rampSecondMoves, (unsigned long long)c.stepUpOk, (unsigned long long)c.floorLineTraces);
	}
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::printf("사용: GrenadeWalkCheck <map_gym.mocl> <기록 폴더> <tag> [<tag> ...]\n");
		return 2;
	}
	std::vector<char> mapBytes;
	CMapCollision map;
	if (!ReadFile(argv[1], mapBytes) || map.LoadFromMemory(mapBytes.data(), mapBytes.size()) != EMapLoad::Ok)
	{
		std::printf("GRENADEWALK 입력 오류 — 맵 %s\n", argv[1]);
		return 2;
	}
	const std::string dir = argv[2];
	int cells = 0, passed = 0, controlsDiffer = 0;
	for (int a = 3; a < argc; ++a)
	{
		const char* tag = argv[a];
		std::map<std::string, std::vector<WalkRow>> walk;
		std::vector<std::string> order;
		std::map<std::string, std::vector<int>> grenadeHits;
		std::map<int, std::vector<GrenadePose>> grenades;
		if (!LoadWalk(dir + "/cprobe_" + tag + ".log", walk, order, grenadeHits) || !LoadGrenades(dir + "/cprobe_" + tag + "_gren.csv", grenades))
		{
			std::printf("GRENADEWALK 입력 오류 — %s 기록\n", tag);
			return 2;
		}
		// 대조군에 넣을 수류탄 — 수류탄 위를 지나는 칸의 첫 「바닥이 수류탄」 줄 앞 프레임 몸
		std::vector<MapCapsule> injected;
		const ProjectileParams grenadeParams{};
		for (const std::string& step : order)
		{
			const std::vector<WalkRow>& rows = walk[step];
			for (size_t k = 1; k < rows.size() && injected.empty(); ++k)
			{
				if (!rows[k].onGren) continue;
				const auto it = grenades.find(rows[k - 1].frame);
				if (it != grenades.end())
					for (const GrenadePose& g : it->second) injected.push_back(GrenadeStandable(g.location, g.rotation, grenadeParams, g.id));
			}
		}
		for (const std::string& step : order)
		{
			const std::vector<WalkRow>& rows = walk[step];
			if (rows.size() < 2)
			{
				std::printf("GRENADEWALK 입력 오류 — %s %s 줄 %zu\n", tag, step.c_str(), rows.size());
				return 2;
			}
			++cells;
			const std::vector<int>& lyraHits = grenadeHits[step];
			const WalkResult real = Run(map, rows, grenades, EMode::Real, injected);
			Print(tag, step, "real", rows.size(), real, lyraHits);
			const bool overGrenade = real.lyraOnGren > 0;
			if (!overGrenade && injected.empty())
			{
				std::printf("GRENADEWALK 입력 오류 — %s 에 수류탄 위를 지나는 칸이 없어 대조군을 못 만듦\n", tag);
				return 2;
			}
			const WalkResult control = Run(map, rows, grenades, overGrenade ? EMode::NoGrenade : EMode::Inject, injected);
			Print(tag, step, overGrenade ? "noGrenade" : "inject", rows.size(), control, lyraHits);
			const bool ok = real.maxPos <= kPosTolerance && real.maxVel <= kVelTolerance && real.lifted == real.lyraLifted && real.bothLifted == real.lifted
				&& real.onStandable == real.lyraOnGren && real.hitFrames == lyraHits;
			const bool differs = control.maxPos > kPosTolerance;
			passed += ok ? 1 : 0;
			controlsDiffer += differs ? 1 : 0;
		}
	}
	const bool pass = cells > 0 && passed == cells && controlsDiffer == cells;
	std::printf("GRENADEWALK cells=%d pass=%d controlsDiffer=%d %s\n", cells, passed, controlsDiffer, pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
