// 대시 대조 도구 — Lyra 대시 기록과 Shared 이동(대시 힘)을 맞춰 본다(대시 D-1).
//
//   DashCheck <cprobe_<tag>.log> <cprobe_<tag>.csv> <map_gym.mocl>
//
// 기록 = Tools/LyraPilot/record_content_probe.ps1 대시 판(D-0). 기록 표 한 줄 = Lyra 한 프레임의 이동 뒤 상태. 기록 도구의 틱은 이동 뒤에 돈다 —
// 그 틱에서 넣은 이동 입력 · 누른 키 · 앉기 키는 다음 프레임에 먹는다. 능력이 켜진 줄(ev 의 dash_start · melee_start)은 그 프레임 이동 앞에 켜졌다.
// 칸(step)마다 첫 능력이 켜진 줄의 앞 줄(자리 · 속도 · 이동 모드 · 앉음)에서 시작해 칸 끝까지 프레임마다 Shared 이동을 돌려 자리 · 속도를 견준다
//   이동 입력 = 앞 줄 inX · inY(월드 축) · 걷기 최대 속도 = 그 줄 maxSpeed(앉은 줄은 600 — 이동 코드가 앉으면 300 으로 바꾼다)
//   대시가 켜진 줄 = 그 줄 이동 앞에 힘을 넣는다. 방향 종류 = 그 줄 몽타주(고르기는 D-2 가 견준다), 입력 · 카메라 = 앞 줄 lastInX · lastInY · ctlPitch · ctlYaw
//     (켜지는 순간의 GetLastMovementInputVector = 앞 프레임 이동이 가져간 입력 = 앞 줄 lastIn). 앞 줄이 앉았으면 일어서기(그래프 bIsCrouched → UnCrouch),
//     근접 밀기가 남았으면 지운다(대시가 근접을 끊음 → 작업 OnDestroy → RemoveRootMotionSourceByID)
//   근접이 켜진 줄 = 밀기 힘을 넣는다. 크기 · 방향 = 그 줄 Lyra 속도 − 힘 없이 같은 입력으로 돌린 속도(이 기록은 맞은 거리를 안 남긴다 — 그 한 프레임은 맞춰 넣은 값이다)
//   앉기 키(crouch_toggle)가 있는 줄 = 다음 줄 이동 앞에 ToggleCrouch
//   새 점프(jumpActive 0 → 1)가 켜진 줄부터는 안 견준다(재생은 점프 키를 안 누른다 — 기대 0번)
//   매 프레임 누름 칸(spam)에서 대시가 꺼진 줄 = 기록 도구가 집으로 옮긴 뒤 값 — 견주지 않고 그 줄에서 다시 시작한다(kHomeAfterDashStep)
// 대조군 = 같은 재생을 힘 없이(일어서기 · 앉기 키는 그대로) — 거의 모든 칸이 어긋나야 이 대조가 대시를 본다
// 끝 코드: 0 = 전부 통과, 1 = 불일치, 2 = 입력 오류
#include "Dash.h"
#include "MapCollision.h"
#include "Movement.h"
#include "Weapon.h"

#include <algorithm>
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
	// 기록 표가 자리 · 속도를 소수 다섯째 자리로 남긴다. 근접 밀기 대조(MeleeCheck kPushPosTolerance · kPushVelTolerance)와 같은 기준(E-2-1)
	constexpr double kPosTolerance = 0.01;
	constexpr double kVelTolerance = 0.01;
	constexpr float kFrameSeconds = 1.f / 60.f;		// Lyra 기록 프레임(t 칸 0.0167 간격)
	constexpr double kUnrealModeFalling = 3.0;		// EMovementMode::MOVE_Falling
	// 매 프레임 누름 칸 — 기록 도구가 대시가 꺼질 때마다 집으로 옮기고 멈춘다(MOBContentProbeSubsystem.cpp 켜짐 가장자리 `if (S.Act == EAct::DashSpam) GoHome();`
	// = SetActorLocationAndRotation + StopMovementImmediately). 그 줄은 이동 뒤가 아니라 옮긴 뒤 값이라 견주지 않고 그 줄에서 다시 시작한다(속도 0 인지는 센다)
	constexpr const char* kHomeAfterDashStep = "spam";

	bool ReadFile(const char* path, std::vector<char>& out)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path, "rb") != 0 || !file) return false;
		std::fseek(file, 0, SEEK_END);
		const long size = std::ftell(file);
		std::fseek(file, 0, SEEK_SET);
		out.resize((size_t)size);
		const size_t read = std::fread(out.data(), 1, out.size(), file);
		std::fclose(file);
		return read == out.size();
	}

	double Dist(const Vec3& a, const Vec3& b) { const Vec3 d = a - b; return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); }

	struct Table
	{
		std::vector<std::string> header;
		std::vector<std::vector<std::string>> rows;
		int Column(const char* name) const
		{
			const auto it = std::find(header.begin(), header.end(), std::string(name));
			return it == header.end() ? -1 : (int)(it - header.begin());
		}
	};

	struct Columns
	{
		int step, ev, px, py, pz, vx, vy, vz, speed, maxSpeed, crouched, mode, montage, jumpActive, ctlPitch, ctlYaw, lastInX, lastInY, inX, inY;
		bool Ok() const
		{
			const int all[] = { step, ev, px, py, pz, vx, vy, vz, speed, maxSpeed, crouched, mode, montage, jumpActive, ctlPitch, ctlYaw, lastInX, lastInY, inX, inY };
			for (int c : all) if (c < 0) return false;
			return true;
		}
	};

	struct StepRange
	{
		std::string name;
		size_t begin = 0, end = 0;		// 기록 표 줄 [begin, end)
	};

	struct Replay
	{
		int frames = 0;
		int dashes = 0;
		int melees = 0;
		int crouchKeys = 0;
		int cutRow = -1;				// 새 점프로 멈춘 줄(-1 = 칸 끝까지)
		int firstDiffRow = -1;
		int badMontage = 0;
		int homes = 0;					// 기록 도구가 집으로 옮긴 줄(견주지 않음)
		int badHome = 0;				// 옮긴 줄인데 속도가 0 이 아님(전제가 틀림)
		double maxPos = 0, maxVel = 0;
		Vec3 firstForce;
		bool fail = false;
	};

	bool HasEvent(const std::string& ev, const char* name)
	{
		const std::string token = std::string(name) + ";";
		return ev.find(token) != std::string::npos;
	}

	bool MontageDir(const std::string& montage, EDashDir& out)
	{
		if (montage == "AM_MM_Dash_Forward") out = EDashDir::Forward;
		else if (montage == "AM_MM_Dash_Backward") out = EDashDir::Backward;
		else if (montage == "AM_MM_Dash_Left") out = EDashDir::Left;
		else if (montage == "AM_MM_Dash_Right") out = EDashDir::Right;
		else return false;
		return true;
	}

	class CReplayer
	{
	public:
		CReplayer(const Table& table, const Columns& c, const CMapCollision& map, const MoveParams& params)
			: _t(table), _c(c), _params(params), _sim(map, params), _scratch(map, params) {}

		const MoveCounters& Counters() const { return _sim.GetCounters(); }

		Replay Run(const StepRange& step, size_t startRow, bool withForce)
		{
			Replay out;
			MoveState s;
			const std::vector<std::string>& before = Row(startRow);
			s.location = Vec3(Cell(before, _c.px), Cell(before, _c.py), Cell(before, _c.pz));
			s.velocity = Vec3(Cell(before, _c.vx), Cell(before, _c.vy), Cell(before, _c.vz));
			s.lastUpdateVelocity = s.velocity;
			s.mode = Cell(before, _c.mode) == kUnrealModeFalling ? EMoveMode::Falling : EMoveMode::Walking;
			s.crouched = s.wantsToCrouch = Cell(before, _c.crouched) != 0.0;
			s.maxSpeed = MaxSpeedOf(before);
			_sim.RefreshFloor(s);
			uint16_t meleeId = 0;
			for (size_t r = startRow + 1; r < step.end; ++r)
			{
				const std::vector<std::string>& prev = Row(r - 1);
				const std::vector<std::string>& now = Row(r);
				if (Cell(now, _c.jumpActive) != 0.0 && Cell(prev, _c.jumpActive) == 0.0)
				{
					out.cutRow = (int)r;
					break;
				}
				const Vec3 input(Cell(prev, _c.inX), Cell(prev, _c.inY), 0.0);
				const std::string& ev = Text(now, _c.ev);
				// 이 프레임 입력 처리 — 앞 줄 틱의 앉기 키, 이 줄에 켜진 능력
				if (HasEvent(Text(prev, _c.ev), "crouch_toggle"))
				{
					_sim.ToggleCrouch(s);
					++out.crouchKeys;
				}
				s.maxSpeed = MaxSpeedOf(now);
				if (HasEvent(ev, "melee_start"))
				{
					++out.melees;
					if (withForce)
					{
						MoveState probe = s;
						_scratch.Tick(probe, input, kFrameSeconds);
						const Vec3 force(Cell(now, _c.vx) - probe.velocity.x, Cell(now, _c.vy) - probe.velocity.y, 0.0);
						meleeId = _sim.ApplyRootForce(s, force, kMeleePushDuration);
					}
				}
				if (HasEvent(ev, "dash_start"))
				{
					++out.dashes;
					if (meleeId != 0)
					{
						_sim.RemoveRootForce(s, meleeId);
						meleeId = 0;
					}
					if (Cell(prev, _c.crouched) != 0.0) _sim.RequestUnCrouch(s);
					EDashDir dir = EDashDir::Forward;
					if (!MontageDir(Text(now, _c.montage), dir)) ++out.badMontage;
					const Vec3 force = DashForce(dir, Vec3(Cell(prev, _c.lastInX), Cell(prev, _c.lastInY), 0.0), (float)Cell(prev, _c.ctlPitch), (float)Cell(prev, _c.ctlYaw));
					if (out.dashes == 1) out.firstForce = force;
					if (withForce) _sim.ApplyRootForce(s, force, kDashForceDuration, DashForceSettings());
				}
				_sim.Tick(s, input, kFrameSeconds);
				const Vec3 lyraPos(Cell(now, _c.px), Cell(now, _c.py), Cell(now, _c.pz));
				const Vec3 lyraVel(Cell(now, _c.vx), Cell(now, _c.vy), Cell(now, _c.vz));
				if (step.name == kHomeAfterDashStep && HasEvent(ev, "dash_end"))
				{
					++out.homes;
					if (Dist(lyraVel, Vec3()) != 0.0) ++out.badHome;
					s.location = lyraPos;
					s.velocity = s.lastUpdateVelocity = Vec3();
					s.mode = Cell(now, _c.mode) == kUnrealModeFalling ? EMoveMode::Falling : EMoveMode::Walking;
					_sim.RefreshFloor(s);
					continue;
				}
				const double posDiff = Dist(s.location, lyraPos);
				const double velDiff = Dist(s.velocity, lyraVel);
				out.maxPos = std::max(out.maxPos, posDiff);
				out.maxVel = std::max(out.maxVel, velDiff);
				if ((posDiff > kPosTolerance || velDiff > kVelTolerance) && out.firstDiffRow < 0) out.firstDiffRow = (int)r;
				++out.frames;
			}
			out.fail = out.frames == 0 || out.firstDiffRow >= 0 || out.badMontage > 0 || out.badHome > 0;
			return out;
		}

	private:
		const std::vector<std::string>& Row(size_t r) const { return _t.rows[r]; }
		static double Cell(const std::vector<std::string>& cells, int col) { return (int)cells.size() > col ? std::atof(cells[(size_t)col].c_str()) : 0.0; }
		static const std::string& Text(const std::vector<std::string>& cells, int col)
		{
			static const std::string empty;
			return (int)cells.size() > col ? cells[(size_t)col] : empty;
		}
		// 기록 maxSpeed = GetMaxSpeed — 앉아 걸으면 앉은 속도라, 앉은 줄은 서서 걷는 값을 준다(이동 코드가 앉음을 보고 바꾼다)
		float MaxSpeedOf(const std::vector<std::string>& cells) const
		{
			return Cell(cells, _c.crouched) != 0.0 ? _params.maxWalkSpeed : (float)Cell(cells, _c.maxSpeed);
		}

		const Table& _t;
		const Columns& _c;
		const MoveParams& _params;
		CMoveSim _sim;
		CMoveSim _scratch;		// 근접 밀기 크기 구하기 전용 — 계수가 본 재생에 안 섞이게
	};
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::printf("사용: DashCheck <cprobe.log> <cprobe.csv> <map_gym.mocl>\n");
		return 2;
	}

	std::vector<char> mapData;
	CMapCollision map;
	if (!ReadFile(argv[3], mapData) || map.LoadFromMemory(mapData.data(), mapData.size()) != EMapLoad::Ok)
	{
		std::printf("DASHCHECK FAIL 맵을 못 읽음: %s\n", argv[3]);
		return 2;
	}

	// ── 로그 — 칸마다 대시가 켜진 횟수(starts) 합. 기록 표의 dash_start 수와 같아야 대시를 빠짐없이 견준다
	std::ifstream log(argv[1]);
	if (!log)
	{
		std::printf("DASHCHECK FAIL 로그를 못 엶: %s\n", argv[1]);
		return 2;
	}
	int logStarts = 0, logSteps = 0, badLines = 0;
	std::string line;
	while (std::getline(log, line))
	{
		const size_t at = line.find("MOB_CPROBE_DASH ");
		if (at == std::string::npos) continue;
		const size_t key = line.find(" starts=", at);
		if (key == std::string::npos) { ++badLines; continue; }
		logStarts += std::atoi(line.c_str() + key + 8);
		++logSteps;
	}

	// ── 기록 표
	std::ifstream csv(argv[2]);
	Table table;
	if (!csv || !std::getline(csv, line))
	{
		std::printf("DASHCHECK FAIL 기록 표를 못 엶: %s\n", argv[2]);
		return 2;
	}
	{
		std::istringstream in(line);
		std::string cell;
		while (std::getline(in, cell, ',')) table.header.push_back(cell);
	}
	while (std::getline(csv, line))
	{
		std::vector<std::string> cells;
		std::istringstream in(line);
		std::string cell;
		while (std::getline(in, cell, ',')) cells.push_back(cell);
		table.rows.push_back(cells);
	}
	Columns c;
	c.step = table.Column("step"); c.ev = table.Column("ev");
	c.px = table.Column("px"); c.py = table.Column("py"); c.pz = table.Column("pz");
	c.vx = table.Column("vx"); c.vy = table.Column("vy"); c.vz = table.Column("vz");
	c.speed = table.Column("speed2d"); c.maxSpeed = table.Column("maxSpeed"); c.crouched = table.Column("crouched"); c.mode = table.Column("mode");
	c.montage = table.Column("montage"); c.jumpActive = table.Column("jumpActive");
	c.ctlPitch = table.Column("ctlPitch"); c.ctlYaw = table.Column("ctlYaw");
	c.lastInX = table.Column("lastInX"); c.lastInY = table.Column("lastInY"); c.inX = table.Column("inX"); c.inY = table.Column("inY");
	if (!c.Ok())
	{
		std::printf("DASHCHECK FAIL 기록 표 머리에 대시 대조 칸이 없음(대시 판 기록인가 — D-0 record_content_probe.ps1)\n");
		return 2;
	}

	std::vector<StepRange> steps;
	for (size_t r = 0; r < table.rows.size(); ++r)
	{
		const std::string name = (int)table.rows[r].size() > c.step ? table.rows[r][(size_t)c.step] : std::string();
		if (steps.empty() || steps.back().name != name)
		{
			if (!steps.empty()) steps.back().end = r;
			StepRange s;
			s.name = name;
			s.begin = r;
			steps.push_back(s);
		}
	}
	if (!steps.empty()) steps.back().end = table.rows.size();

	int csvStarts = 0;
	for (const auto& cells : table.rows)
	{
		if ((int)cells.size() > c.ev && HasEvent(cells[(size_t)c.ev], "dash_start")) ++csvStarts;
	}
	if (csvStarts == 0 || csvStarts != logStarts || badLines != 0)
	{
		std::printf("DASHCHECK FAIL 입력 — 대시 %d번(로그 %d칸) · %d번(표) · 못 읽은 줄 %d\n", logStarts, logSteps, csvStarts, badLines);
		return 2;
	}

	// ── 대조 — 칸마다 힘 넣은 재생 · 힘 없는 대조군
	const MoveParams params = MakeLyraMoveParams();
	CReplayer replayer(table, c, map, params);
	CReplayer control(table, c, map, params);
	int compared = 0, fails = 0, frames = 0, dashes = 0, melees = 0, crouchKeys = 0, cuts = 0, badMontage = 0, homes = 0, badHome = 0;
	int controlFails = 0;
	double maxPos = 0, maxVel = 0, controlMinPos = 1e30;
	for (const StepRange& step : steps)
	{
		size_t first = step.end;
		for (size_t r = step.begin; r < step.end; ++r)
		{
			const std::string& ev = table.rows[r].size() > (size_t)c.ev ? table.rows[r][(size_t)c.ev] : std::string();
			if (HasEvent(ev, "dash_start") || HasEvent(ev, "melee_start")) { first = r; break; }
		}
		if (first == step.end) continue;		// 대시가 안 켜진 칸(still_yaw90 취소 · 준비 칸)
		if (first == 0 || first - 1 < step.begin)
		{
			std::printf("DASH INPUT step=%s 첫 능력 줄 앞에 같은 칸 줄이 없음\n", step.name.c_str());
			++fails;
			continue;
		}
		const Replay mine = replayer.Run(step, first - 1, true);
		const Replay none = control.Run(step, first - 1, false);
		++compared;
		if (mine.fail) ++fails;
		if (none.fail) ++controlFails;
		frames += mine.frames;
		dashes += mine.dashes;
		melees += mine.melees;
		crouchKeys += mine.crouchKeys;
		badMontage += mine.badMontage;
		homes += mine.homes;
		badHome += mine.badHome;
		if (mine.cutRow >= 0) ++cuts;
		maxPos = std::max(maxPos, mine.maxPos);
		maxVel = std::max(maxVel, mine.maxVel);
		controlMinPos = std::min(controlMinPos, none.maxPos);
		std::printf("DASH %s step=%s dashes=%d melee=%d crouchKeys=%d homes=%d frames=%d force=%.3f,%.3f,%.3f maxPosDiff=%.4f maxVelDiff=%.4f firstDiffFrame=%s cutFrame=%s noForceMaxPosDiff=%.2f\n",
			mine.fail ? "DIFF" : "ok", step.name.c_str(), mine.dashes, mine.melees, mine.crouchKeys, mine.homes, mine.frames, mine.firstForce.x, mine.firstForce.y, mine.firstForce.z, mine.maxPos, mine.maxVel,
			mine.firstDiffRow >= 0 ? table.rows[(size_t)mine.firstDiffRow][0].c_str() : "-", mine.cutRow >= 0 ? table.rows[(size_t)mine.cutRow][0].c_str() : "-", none.maxPos);
	}

	const MoveCounters& mc = replayer.Counters();
	std::printf("DASH move compared=%d fails=%d frames=%d maxPosDiff=%.4f maxVelDiff=%.4f dashes=%d melee=%d crouchKeys=%d cut=%d badMontage=%d homes=%d badHome=%d\n",
		compared, fails, frames, maxPos, maxVel, dashes, melees, crouchKeys, cuts, badMontage, homes, badHome);
	std::printf("DASH noForce compared=%d fails=%d minMaxPosDiff=%.2f\n", compared, controlFails, compared > 0 ? controlMinPos : 0.0);
	std::printf("DASH forceCount applied=%llu second=%llu replaced=%llu prepared=%llu finished=%llu removed=%llu clamped=%llu zAdds=%llu liftoffs=%llu restores=%llu adds=%llu external=%llu apexRefund=%llu\n",
		(unsigned long long)mc.rootForceApplied, (unsigned long long)mc.rootForceSecond, (unsigned long long)mc.rootForceReplaced, (unsigned long long)mc.rootForcePrepared,
		(unsigned long long)mc.rootForceFinished, (unsigned long long)mc.rootForceRemoved, (unsigned long long)mc.rootForceClamped, (unsigned long long)mc.rootForceZAdds,
		(unsigned long long)mc.rootForceLiftoffs, (unsigned long long)mc.rootForceRestores, (unsigned long long)mc.rootForceAdds, (unsigned long long)mc.rootForceExternal,
		(unsigned long long)mc.rootForceApexRefund);
	std::printf("DASHCHECK %s steps=%d dashes=%d fails=%d\n", fails == 0 ? "PASS" : "FAIL", compared, dashes, fails);
	return fails == 0 ? 0 : 1;
}
