// 조각 1-c 대조 도구 — Lyra 에 재생한 입력을 Shared/Movement 에 그대로 넣어 위치·속도·이동 모드를 비교한다.
//
// 사용: MoveCheck <map.mocl> <move.csv> [--mismatch <out.csv>] [--limit <프레임 수>]
//
// 입력 파일 = MOBMoveRecordSubsystem 이 쓰는 move_<이름>.csv
//   #param <이름> <값>      실행값(블루프린트가 덮은 뒤의 값)
//   frame,seg,ev,dt, 입력3, 앞 위치3, 앞 속도3, 앞 모드, ext, 발사대3, pressed,was,hold,force,count,countPre, maxSpeed,
//   뒤 위치3, 뒤 속도3, 뒤 모드, fBlock,fWalk,fLine,fDist,fLineDist, aCount
//   ev: 0 없음 / 1 Jump / 2 StopJumping. 모드: 1 걷기 / 3 낙하. ext=1 이면 프레임 사이에 밖에서 위치가 바뀐 것
//
// 두 가지를 잰다
//   한 프레임: 매 프레임 Lyra 상태에서 시작해 한 번만 돌린 차이 — 어느 프레임에서 어긋나는지 본다
//   이어 달리기: 구간 첫 프레임만 맞추고 끝까지 돌린 차이 — 서버가 실제로 쓰는 방식이다

#include "MapCollision.h"
#include "Movement.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace mo;

namespace
{
	constexpr int kModeWalking = 1;
	constexpr int kModeFalling = 3;

	struct Row
	{
		int frame = 0, seg = 0, ev = 0, modeBefore = 0, ext = 0, modeAfter = 0;
		int pressed = 0, was = 0, count = 0, countPre = 0, aCount = 0, fBlock = 0, fWalk = 0, fLine = 0;
		float dt = 0, hold = 0, force = 0, maxSpeed = 0, fDist = 0, fLineDist = 0;
		Vec3 input, p, v, launch, ap, av;
	};

	double Dist(const Vec3& a, const Vec3& b)
	{
		const Vec3 d = a - b;
		return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	}

	bool ToMode(int value, EMoveMode& out)
	{
		switch (value)
		{
		case kModeWalking: out = EMoveMode::Walking; return true;
		case kModeFalling: out = EMoveMode::Falling; return true;
		default: return false;
		}
	}

	int FromMode(EMoveMode mode)
	{
		switch (mode)
		{
		case EMoveMode::Walking: return kModeWalking;
		case EMoveMode::Falling: return kModeFalling;
		}
		return 0;
	}

	bool ParseRow(char* line, Row& r)
	{
		char* s = line;
		const auto D = [&]() { const double v = std::strtod(s, &s); if (*s == ',') ++s; return v; };
		const auto F = [&]() { const float v = std::strtof(s, &s); if (*s == ',') ++s; return v; };
		const auto I = [&]() { const int v = (int)std::strtol(s, &s, 10); if (*s == ',') ++s; return v; };
		const auto V = [&]() { Vec3 v; v.x = D(); v.y = D(); v.z = D(); return v; };

		r.frame = I(); r.seg = I(); r.ev = I(); r.dt = F();
		r.input = V(); r.p = V(); r.v = V();
		r.modeBefore = I(); r.ext = I();
		r.launch = V();
		r.pressed = I(); r.was = I(); r.hold = F(); r.force = F(); r.count = I(); r.countPre = I(); r.maxSpeed = F();
		r.ap = V(); r.av = V();
		r.modeAfter = I();
		r.fBlock = I(); r.fWalk = I(); r.fLine = I(); r.fDist = F(); r.fLineDist = F(); r.aCount = I();
		return *s == '\0' || *s == '\r' || *s == '\n';
	}

	void SyncState(MoveState& s, const Row& r, CMoveSim& sim)
	{
		s = MoveState();
		s.location = r.p;
		s.velocity = r.v;
		ToMode(r.modeBefore, s.mode);
		s.pressedJump = r.pressed != 0;
		s.wasJumping = r.was != 0;
		s.jumpKeyHoldTime = r.hold;
		s.jumpForceTimeRemaining = r.force;
		s.jumpCurrentCount = r.count;
		s.jumpCurrentCountPreJump = r.countPre;
		s.maxSpeed = r.maxSpeed;
		sim.RefreshFloor(s);
	}

	void RunTick(CMoveSim& sim, MoveState& s, const Row& r)
	{
		const bool hasLaunch = r.launch.x != 0 || r.launch.y != 0 || r.launch.z != 0;
		s.maxSpeed = r.maxSpeed;
		sim.Tick(s, r.input, r.dt, hasLaunch ? &r.launch : nullptr);
	}

	struct DiffStats
	{
		size_t frames = 0, over001 = 0, over01 = 0, over1 = 0, velOver1 = 0, modeMismatch = 0, walking = 0, falling = 0;
		double maxDiff = 0, maxVel = 0;
		std::vector<double> diffs;

		void Add(const MoveState& s, const Row& r)
		{
			const double d = Dist(s.location, r.ap);
			const double dv = Dist(s.velocity, r.av);
			++frames;
			diffs.push_back(d);
			maxDiff = std::max(maxDiff, d);
			maxVel = std::max(maxVel, dv);
			if (d > 0.01) ++over001;
			if (d > 0.1) ++over01;
			if (d > 1.0) ++over1;
			if (dv > 1.0) ++velOver1;
			if (FromMode(s.mode) != r.modeAfter) ++modeMismatch;
			if (r.modeAfter == kModeWalking) ++walking;
			if (r.modeAfter == kModeFalling) ++falling;
		}

		double Percentile(double q)
		{
			if (diffs.empty()) return 0;
			std::sort(diffs.begin(), diffs.end());
			return diffs[std::min(diffs.size() - 1, (size_t)(q * (double)diffs.size()))];
		}

		void Print(const char* name)
		{
			const double p99 = Percentile(0.99);
			const double p50 = Percentile(0.5);
			std::printf("%s frames=%zu (walk=%zu fall=%zu) posMax=%.4f p50=%.5f p99=%.4f over0.01=%zu over0.1=%zu over1=%zu | velMax=%.3f velOver1=%zu | modeMismatch=%zu\n",
				name, frames, walking, falling, maxDiff, p50, p99, over001, over01, over1, maxVel, velOver1, modeMismatch);
		}
	};

	// 어긋난 프레임에 어느 갈래를 탔는지 — 두 스냅샷의 차이만 찍는다
	void PrintCounterDelta(const MoveCounters& a, const MoveCounters& b)
	{
#define MOVE_DELTA(name) if (b.name != a.name) std::printf(" " #name "=%llu", (unsigned long long)(b.name - a.name))
		MOVE_DELTA(walkingIterations); MOVE_DELTA(fallingIterations); MOVE_DELTA(floorSweeps); MOVE_DELTA(floorSecondSweeps);
		MOVE_DELTA(floorLineTraces); MOVE_DELTA(floorLineWalkable); MOVE_DELTA(floorSuppliedSweep); MOVE_DELTA(floorHeightAdjusts);
		MOVE_DELTA(rampSecondMoves); MOVE_DELTA(stepUpTried); MOVE_DELTA(stepUpOk); MOVE_DELTA(stepUpRejected); MOVE_DELTA(slides);
		MOVE_DELTA(twoWalls); MOVE_DELTA(walkOffLedge); MOVE_DELTA(landed); MOVE_DELTA(landedOnEdgeFloor); MOVE_DELTA(fallSecondWall);
		MOVE_DELTA(fallStuckSideStep); MOVE_DELTA(apexSubsteps); MOVE_DELTA(jumps); MOVE_DELTA(launches); MOVE_DELTA(startPenetrating);
		MOVE_DELTA(resolveTeleport); MOVE_DELTA(resolveSweep); MOVE_DELTA(floorPopOut); MOVE_DELTA(perchTried); MOVE_DELTA(perchOk);
		MOVE_DELTA(perchUnwalkableOverride); MOVE_DELTA(perchFail);
#undef MOVE_DELTA
	}

	void PrintCounters(const MoveCounters& c)
	{
		std::printf("COUNT ticks=%llu walkIter=%llu fallIter=%llu | floorSweep=%llu second=%llu line=%llu lineWalk=%llu supplied=%llu adjust=%llu popOut=%llu\n",
			(unsigned long long)c.ticks, (unsigned long long)c.walkingIterations, (unsigned long long)c.fallingIterations,
			(unsigned long long)c.floorSweeps, (unsigned long long)c.floorSecondSweeps, (unsigned long long)c.floorLineTraces, (unsigned long long)c.floorLineWalkable,
			(unsigned long long)c.floorSuppliedSweep, (unsigned long long)c.floorHeightAdjusts, (unsigned long long)c.floorPopOut);
		std::printf("COUNT perchTry=%llu perchOk=%llu perchOverride=%llu perchFail=%llu\n",
			(unsigned long long)c.perchTried, (unsigned long long)c.perchOk, (unsigned long long)c.perchUnwalkableOverride, (unsigned long long)c.perchFail);
		std::printf("COUNT ramp2=%llu stepTry=%llu stepOk=%llu stepReject=%llu slide=%llu twoWall=%llu ledgeFall=%llu | landed=%llu landedEdge=%llu fallWall2=%llu fallStuck=%llu apex=%llu | jump=%llu launch=%llu | startPen=%llu resolveTeleport=%llu resolveSweep=%llu\n",
			(unsigned long long)c.rampSecondMoves, (unsigned long long)c.stepUpTried, (unsigned long long)c.stepUpOk, (unsigned long long)c.stepUpRejected,
			(unsigned long long)c.slides, (unsigned long long)c.twoWalls, (unsigned long long)c.walkOffLedge,
			(unsigned long long)c.landed, (unsigned long long)c.landedOnEdgeFloor, (unsigned long long)c.fallSecondWall, (unsigned long long)c.fallStuckSideStep,
			(unsigned long long)c.apexSubsteps, (unsigned long long)c.jumps, (unsigned long long)c.launches,
			(unsigned long long)c.startPenetrating, (unsigned long long)c.resolveTeleport, (unsigned long long)c.resolveSweep);
	}

	// 옮기지 않은 갈래를 타는 값이면 대조가 뜻이 없다. 값을 먼저 확인한다
	void WarnUnported(std::map<std::string, double>& v)
	{
		const auto warn = [&](const char* name, bool bad)
		{
			if (bad) std::printf("WARN unported %s=%g\n", name, v[name]);
		};
		warn("bCanWalkOffLedges", v["bCanWalkOffLedges"] == 0);
		warn("bAlwaysCheckFloor", v["bAlwaysCheckFloor"] == 0);
		warn("bUseFlatBaseForFloorChecks", v["bUseFlatBaseForFloorChecks"] != 0);
		warn("bForceMaxAccel", v["bForceMaxAccel"] != 0);
		warn("bMaintainHorizontalGroundVelocity", v["bMaintainHorizontalGroundVelocity"] == 0);
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: MoveCheck <map.mocl> <move.csv> [--mismatch <out.csv>] [--limit <frames>]\n");
		return 2;
	}
	const char* mismatchPath = nullptr;
	size_t limit = (size_t)-1;
	for (int i = 3; i + 1 < argc; i += 2)
	{
		if (std::strcmp(argv[i], "--mismatch") == 0) mismatchPath = argv[i + 1];
		else if (std::strcmp(argv[i], "--limit") == 0) limit = (size_t)std::strtoull(argv[i + 1], nullptr, 10);
	}

	std::vector<uint8_t> mapBytes;
	{
		FILE* mf = nullptr;
		if (fopen_s(&mf, argv[1], "rb") != 0 || !mf)
		{
			std::printf("cannot open %s\n", argv[1]);
			return 1;
		}
		_fseeki64(mf, 0, SEEK_END);
		mapBytes.resize((size_t)_ftelli64(mf));
		_fseeki64(mf, 0, SEEK_SET);
		const size_t got = std::fread(mapBytes.data(), 1, mapBytes.size(), mf);
		std::fclose(mf);
		if (got != mapBytes.size()) return 1;
	}
	static CMapCollision map;
	const EMapLoad loaded = map.LoadFromMemory(mapBytes.data(), mapBytes.size());
	if (loaded != EMapLoad::Ok)
	{
		std::printf("map load failed: %d\n", (int)loaded);
		return 1;
	}

	FILE* f = nullptr;
	if (fopen_s(&f, argv[2], "rb") != 0 || !f)
	{
		std::printf("cannot open %s\n", argv[2]);
		return 1;
	}

	std::map<std::string, double> values;
	std::vector<Row> rows;
	size_t badRows = 0;
	static char line[4096];
	while (std::fgets(line, sizeof(line), f))
	{
		if (std::strncmp(line, "#param ", 7) == 0)
		{
			char name[128];
			double value = 0;
			if (sscanf_s(line + 7, "%127s %lf", name, (unsigned)sizeof(name), &value) == 2) values[name] = value;
			continue;
		}
		if (line[0] < '0' || line[0] > '9') continue;
		Row r;
		if (ParseRow(line, r)) rows.push_back(r);
		else ++badRows;
		if (rows.size() >= limit) break;
	}
	std::fclose(f);

	WarnUnported(values);
	MoveParams params;
	params.maxWalkSpeed = (float)values["MaxWalkSpeed"];
	params.maxAcceleration = (float)values["MaxAcceleration"];
	params.groundFriction = (float)values["GroundFriction"];
	params.brakingDecelerationWalking = (float)values["BrakingDecelerationWalking"];
	params.brakingDecelerationFalling = (float)values["BrakingDecelerationFalling"];
	params.brakingFriction = (float)values["BrakingFriction"];
	params.brakingFrictionFactor = (float)values["BrakingFrictionFactor"];
	params.useSeparateBrakingFriction = values["bUseSeparateBrakingFriction"] != 0;
	params.brakingSubStepTime = (float)values["BrakingSubStepTime"];
	params.gravityZ = (float)values["GravityZ"];
	params.jumpZVelocity = (float)values["JumpZVelocity"];
	params.jumpMaxHoldTime = (float)values["JumpMaxHoldTime"];
	params.jumpMaxCount = (int)values["JumpMaxCount"];
	params.airControl = (float)values["AirControl"];
	params.airControlBoostMultiplier = (float)values["AirControlBoostMultiplier"];
	params.airControlBoostVelocityThreshold = (float)values["AirControlBoostVelocityThreshold"];
	params.fallingLateralFriction = (float)values["FallingLateralFriction"];
	params.maxStepHeight = (float)values["MaxStepHeight"];
	params.perchRadiusThreshold = (float)values["PerchRadiusThreshold"];
	params.perchAdditionalHeight = (float)values["PerchAdditionalHeight"];
	params.walkableFloorZ = (float)values["WalkableFloorZ"];
	params.maxSimulationTimeStep = (float)values["MaxSimulationTimeStep"];
	params.maxSimulationIterations = (int)values["MaxSimulationIterations"];
	params.maxJumpApexAttemptsPerSimulation = (int)values["MaxJumpApexAttemptsPerSimulation"];
	params.applyGravityWhileJumping = values["bApplyGravityWhileJumping"] != 0;
	params.dontFallBelowJumpZVelocityDuringJump = values["bDontFallBelowJumpZVelocityDuringJump"] != 0;
	params.maxDepenetrationWithGeometry = (float)values["MaxDepenetrationWithGeometry"];
	params.terminalVelocity = (float)values["TerminalVelocity"];
	params.minAnalogWalkSpeed = (float)values["MinAnalogWalkSpeed"];
	params.capsuleRadius = (float)values["CapsuleRadius"];
	params.capsuleHalfHeight = (float)values["CapsuleHalfHeight"];

	// 시험의 시험 — 환경 변수로 값을 일부러 틀려 대조가 잡는지 본다
	char* breakValue = nullptr;
	size_t breakLen = 0;
	if (_dupenv_s(&breakValue, &breakLen, "MOVECHECK_BREAK_ACCEL") == 0 && breakValue)
	{
		params.maxAcceleration = (float)std::atof(breakValue);
		std::printf("BREAK maxAcceleration=%g\n", params.maxAcceleration);
		std::free(breakValue);
	}

	std::printf("PARAM maxWalk=%g accel=%g friction=%g brakeWalk=%g brakeFall=%g brakeFrictionFactor=%g sepBrake=%d gravity=%g jumpZ=%g hold=%g airControl=%g stepH=%g walkableZ=%g maxSimStep=%g iter=%d terminal=%g capsule=%g/%g\n",
		params.maxWalkSpeed, params.maxAcceleration, params.groundFriction, params.brakingDecelerationWalking, params.brakingDecelerationFalling,
		params.brakingFrictionFactor, params.useSeparateBrakingFriction ? 1 : 0, params.gravityZ, params.jumpZVelocity, params.jumpMaxHoldTime, params.airControl,
		params.maxStepHeight, params.walkableFloorZ, params.maxSimulationTimeStep, params.maxSimulationIterations, params.terminalVelocity, params.capsuleRadius, params.capsuleHalfHeight);
	std::printf("ROWS total=%zu bad=%zu\n", rows.size(), badRows);
	if (rows.empty()) return 1;

	// 한 프레임 — 매번 Lyra 상태에서 시작한다
	FILE* mismatch = nullptr;
	if (mismatchPath && fopen_s(&mismatch, mismatchPath, "wb") == 0 && mismatch)
	{
		std::fprintf(mismatch, "frame,seg,dt,diff,velDiff,modeBefore,modeAfter,mineMode,px,py,pz,vx,vy,vz,ax,ay,az,avx,avy,avz,mx,my,mz,mvx,mvy,mvz,ix,iy,fBlock,fWalk,fLine,fDist,fLineDist,mBlock,mWalk,mLine,mDist,mLineDist,ev\n");
	}
	CMoveSim oneSim(map, params);
	DiffStats oneStats;
	std::map<int, size_t> oneByModePair;
	int printed = 0;
	size_t floorWalkDiff = 0;
	double floorDistMax = 0;
	size_t floorDistCompared = 0, floorDistOver01 = 0;
	for (const Row& r : rows)
	{
		EMoveMode unused;
		if (!ToMode(r.modeBefore, unused)) continue;
		MoveState s;
		SyncState(s, r, oneSim);
		const MoveCounters beforeCounters = oneSim.GetCounters();
		RunTick(oneSim, s, r);
		oneStats.Add(s, r);

		const double d = Dist(s.location, r.ap);
		if (d > 0.1) ++oneByModePair[r.modeBefore * 10 + r.modeAfter];
		if (d > 0.01 && printed < 40)
		{
			++printed;
			std::printf("DIFF frame=%d seg=%d d=%.4f mode %d->%d(mine %d) ev=%d\n  before p=(%.3f %.3f %.3f) v=(%.2f %.2f %.2f) input=(%.3f %.3f)\n  lyra   p=(%.4f %.4f %.4f) v=(%.3f %.3f %.3f)\n  mine   p=(%.4f %.4f %.4f) v=(%.3f %.3f %.3f)\n  branches:",
				r.frame, r.seg, d, r.modeBefore, r.modeAfter, FromMode(s.mode), r.ev, r.p.x, r.p.y, r.p.z, r.v.x, r.v.y, r.v.z, r.input.x, r.input.y,
				r.ap.x, r.ap.y, r.ap.z, r.av.x, r.av.y, r.av.z, s.location.x, s.location.y, s.location.z, s.velocity.x, s.velocity.y, s.velocity.z);
			PrintCounterDelta(beforeCounters, oneSim.GetCounters());
			std::printf("\n");
		}
		if ((r.fWalk != 0) != s.floor.walkableFloor) ++floorWalkDiff;
		if (r.modeAfter == kModeWalking && FromMode(s.mode) == kModeWalking)
		{
			const double fd = std::fabs((double)r.fDist - (double)s.floor.floorDist);
			++floorDistCompared;
			floorDistMax = std::max(floorDistMax, fd);
			if (fd > 0.1) ++floorDistOver01;
		}
		if (mismatch && (d > 0.1 || FromMode(s.mode) != r.modeAfter))
		{
			std::fprintf(mismatch, "%d,%d,%.9g,%.4f,%.4f,%d,%d,%d,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.6g,%.6g,%d,%d,%d,%.6g,%.6g,%d,%d,%d,%.6g,%.6g,%d\n",
				r.frame, r.seg, r.dt, d, Dist(s.velocity, r.av), r.modeBefore, r.modeAfter, FromMode(s.mode),
				r.p.x, r.p.y, r.p.z, r.v.x, r.v.y, r.v.z, r.ap.x, r.ap.y, r.ap.z, r.av.x, r.av.y, r.av.z,
				s.location.x, s.location.y, s.location.z, s.velocity.x, s.velocity.y, s.velocity.z, r.input.x, r.input.y,
				r.fBlock, r.fWalk, r.fLine, r.fDist, r.fLineDist,
				s.floor.blockingHit ? 1 : 0, s.floor.walkableFloor ? 1 : 0, s.floor.lineTrace ? 1 : 0, s.floor.floorDist, s.floor.lineDist, r.ev);
		}
	}
	if (mismatch) std::fclose(mismatch);
	oneStats.Print("ONE");
	std::printf("ONE floor walkableDiff=%zu distCompared=%zu distOver0.1=%zu distMax=%.4f | over0.1 by mode(before->after):", floorWalkDiff, floorDistCompared, floorDistOver01, floorDistMax);
	for (const auto& pair : oneByModePair) std::printf(" %d->%d=%zu", pair.first / 10, pair.first % 10, pair.second);
	std::printf("\n");

	// 이어 달리기 — 구간 첫 프레임과 ext 프레임만 Lyra 에 맞춘다
	CMoveSim runSim(map, params);
	DiffStats runStats;
	MoveState run;
	int currentSeg = -1;
	size_t segments = 0, segOver1 = 0, segOver10 = 0, resyncs = 0;
	double segMax = 0, worstSeg = 0;
	int worstSegId = -1;
	float segClock = 0.f, firstOver1At = -1.f;
	bool segFirstSmallPrinted = false;
	std::vector<float> firstOver1;
	const auto closeSeg = [&]()
	{
		if (currentSeg < 0) return;
		++segments;
		if (segMax > 1.0) ++segOver1;
		if (segMax > 10.0) ++segOver10;
		if (segMax > worstSeg) { worstSeg = segMax; worstSegId = currentSeg; }
		if (firstOver1At >= 0.f) firstOver1.push_back(firstOver1At);
	};

	LARGE_INTEGER freq, t0, t1;
	QueryPerformanceFrequency(&freq);
	double runSeconds = 0;
	size_t runTicks = 0;
	for (const Row& r : rows)
	{
		EMoveMode unused;
		if (!ToMode(r.modeBefore, unused)) continue;
		if (r.seg != currentSeg)
		{
			closeSeg();
			currentSeg = r.seg;
			segMax = 0;
			segClock = 0.f;
			firstOver1At = -1.f;
			segFirstSmallPrinted = false;
			SyncState(run, r, runSim);
		}
		else if (r.ext != 0)
		{
			++resyncs;
			SyncState(run, r, runSim);
		}
		else
		{
			switch (r.ev)
			{
			case 1: runSim.PressJump(run); break;
			case 2: runSim.ReleaseJump(run); break;
			default: break;
			}
		}

		QueryPerformanceCounter(&t0);
		RunTick(runSim, run, r);
		QueryPerformanceCounter(&t1);
		runSeconds += (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
		++runTicks;

		runStats.Add(run, r);
		const double d = Dist(run.location, r.ap);
		segMax = std::max(segMax, d);
		segClock += r.dt;
		if (d > 1.0 && firstOver1At < 0.f)
		{
			firstOver1At = segClock;
			std::printf("RUN_FIRST_OVER1 seg=%d frame=%d t=%.2f mode=%d d=%.3f\n", r.seg, r.frame, segClock, r.modeAfter, d);
		}
		if (d > 0.01 && !segFirstSmallPrinted)
		{
			segFirstSmallPrinted = true;
			std::printf("RUN_FIRST_OVER0.01 seg=%d frame=%d t=%.2f mode=%d d=%.4f\n", r.seg, r.frame, segClock, r.modeAfter, d);
		}
	}
	closeSeg();
	runStats.Print("RUN");
	std::sort(firstOver1.begin(), firstOver1.end());
	std::printf("RUN segments=%zu over1cm=%zu over10cm=%zu worst=%.3f (seg %d) resync=%zu firstOver1 median=%.2fs\n",
		segments, segOver1, segOver10, worstSeg, worstSegId, resyncs, firstOver1.empty() ? 0.f : firstOver1[firstOver1.size() / 2]);
	PrintCounters(runSim.GetCounters());
	std::printf("TIME tick=%.2fus (ticks=%zu)\n", runTicks ? runSeconds * 1e6 / (double)runTicks : 0.0, runTicks);
	return 0;
}
