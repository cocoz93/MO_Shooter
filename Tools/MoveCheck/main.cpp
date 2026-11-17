// 조각 1-c 대조 도구 — Lyra 에 재생한 입력을 Shared/Movement 에 그대로 넣어 위치·속도·이동 모드를 비교한다.
//
// 사용: MoveCheck <map.mocl> <move.csv> [--mismatch <out.csv>] [--limit <프레임 수>]
//
// 입력 파일 = MOBMoveRecordSubsystem 이 쓰는 move_<이름>.csv
//   #param <이름> <값>      실행값(블루프린트가 덮은 뒤의 값)
//   frame,seg,ev,dt, 입력3, 앞 위치3, 앞 속도3, 앞 모드, ext, 발사대3, pressed,was,hold,force,count,countPre, maxSpeed,
//   뒤 위치3, 뒤 속도3, 뒤 모드, fBlock,fWalk,fLine,fDist,fLineDist, aCount [, ads] [, cev,cwant,crouched,half,walk,cwantA,crouchedA,halfA]
//   ads(-Ads 기록만) = 이번 프레임에 조준을 누른 상태. 다음 프레임 maxSpeed 가 MaxWalkSpeed × 0.5 인지 센다(GA_ADS — C 조준)
//   앉기 칸(-Crouch 기록만, 컨텐츠 D) = cev(0 없음 / 1 앉기 키 ToggleCrouch / 2 점프 전 일어서기 UnCrouch) · 사건 전 앉으려 함 · 앉음 ·
//   캡슐 반높이 · MaxWalkSpeed, 그리고 이동 뒤 앉으려 함 · 앉음 · 반높이. 앞 상태는 사건 전에 잡았으므로 한 프레임 대조도 사건을 넣고 돌린다
//   어느 칸이 있는지는 머리줄(frame,seg,…)의 이름으로 가린다
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
		int ads = -1;	// -1 = 칸 없음
		int cev = 0, cwant = 0, crouched = 0, cwantA = 0, crouchedA = 0;
		float half = 0, walk = 0, halfA = 0;
	};

	// 머리줄에서 읽은 덧붙은 칸
	struct Columns
	{
		bool ads = false;
		bool crouch = false;
	};

	Columns ParseHeader(const char* line)
	{
		Columns c;
		std::string name;
		for (const char* p = line;; ++p)
		{
			if (*p == ',' || *p == '\0' || *p == '\r' || *p == '\n')
			{
				if (name == "ads") c.ads = true;
				if (name == "cev") c.crouch = true;
				name.clear();
				if (*p != ',') break;
			}
			else
			{
				name += *p;
			}
		}
		return c;
	}

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

	bool ParseRow(char* line, const Columns& cols, Row& r)
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
		if (cols.ads) r.ads = I();
		if (cols.crouch)
		{
			r.cev = I(); r.cwant = I(); r.crouched = I(); r.half = F(); r.walk = F();
			r.cwantA = I(); r.crouchedA = I(); r.halfA = F();
		}
		return *s == '\0' || *s == '\r' || *s == '\n';
	}

	// 앉기 사건 — 기록기가 이동 전에 부른 것과 같은 함수
	void ApplyCrouchEvent(CMoveSim& sim, MoveState& s, const Row& r)
	{
		switch (r.cev)
		{
		case 1: sim.ToggleCrouch(s); break;
		case 2: sim.RequestUnCrouch(s); break;
		default: break;
		}
	}

	void SyncState(MoveState& s, const Row& r, CMoveSim& sim, const Columns& cols)
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
		if (cols.crouch)
		{
			// 앉은 캡슐로 바닥을 찾아야 하므로 RefreshFloor 전에
			s.wantsToCrouch = r.cwant != 0;
			s.crouched = r.crouched != 0;
		}
		sim.RefreshFloor(s);
	}

	void RunTick(CMoveSim& sim, MoveState& s, const Row& r, const Columns& cols)
	{
		const bool hasLaunch = r.launch.x != 0 || r.launch.y != 0 || r.launch.z != 0;
		// maxSpeed 칸은 이동 전 GetMaxSpeed — 앉아 걸을 때 이미 300 이다. 이동 코드는 MaxWalkSpeed 를 받아 그 틱의 앉음으로 고르므로
		// 앉기 기록은 walk 칸(MaxWalkSpeed)을 준다. 0 이면 이동 금지(Lyra GetMaxSpeed 의 태그 갈래)라 그대로 0
		s.maxSpeed = (cols.crouch && r.maxSpeed != 0.f) ? r.walk : r.maxSpeed;
		sim.Tick(s, r.input, r.dt, hasLaunch ? &r.launch : nullptr);
	}

	struct DiffStats
	{
		size_t frames = 0, over001 = 0, over01 = 0, over1 = 0, velOver1 = 0, modeMismatch = 0, walking = 0, falling = 0;
		double maxDiff = 0, maxVel = 0;
		std::vector<double> diffs;
		// 앉기 — 이동 뒤 앉음 · 앉으려 함 · 캡슐 반높이가 Lyra 와 다른 프레임 수, 그리고 Lyra 가 앉은 프레임 수(0 이면 안 본 것)
		size_t crouchedFrames = 0, crouchedMismatch = 0, wantMismatch = 0, halfMismatch = 0;

		void AddCrouch(const CMoveSim& sim, const MoveState& s, const Row& r)
		{
			if (r.crouchedA) ++crouchedFrames;
			if ((s.crouched ? 1 : 0) != r.crouchedA) ++crouchedMismatch;
			if ((s.wantsToCrouch ? 1 : 0) != r.cwantA) ++wantMismatch;
			if (sim.HalfHeightOf(s) != r.halfA) ++halfMismatch;
		}

		void PrintCrouch(const char* name) const
		{
			std::printf("%s crouch crouchedFrames=%zu crouchedMismatch=%zu wantMismatch=%zu halfMismatch=%zu\n",
				name, crouchedFrames, crouchedMismatch, wantMismatch, halfMismatch);
		}

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
		MOVE_DELTA(crouchGround); MOVE_DELTA(crouchAir); MOVE_DELTA(uncrouchGround); MOVE_DELTA(uncrouchGroundLowered);
		MOVE_DELTA(uncrouchAir); MOVE_DELTA(uncrouchAirShifted); MOVE_DELTA(uncrouchBlocked);
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

	// 앉기 갈래 — 앉기 기록일 때만 찍는다(옛 기록의 COUNT 줄 수를 바꾸지 않게 이름을 따로)
	void PrintCrouchCounters(const MoveCounters& c)
	{
		std::printf("CROUCH_COUNT crouchGround=%llu crouchAir=%llu uncrouchGround=%llu lowered=%llu uncrouchAir=%llu airShifted=%llu blocked=%llu\n",
			(unsigned long long)c.crouchGround, (unsigned long long)c.crouchAir, (unsigned long long)c.uncrouchGround, (unsigned long long)c.uncrouchGroundLowered,
			(unsigned long long)c.uncrouchAir, (unsigned long long)c.uncrouchAirShifted, (unsigned long long)c.uncrouchBlocked);
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
		// 앉아 턱 끝에서 멈추는 갈래(CanWalkOffLedges 의 앉기 조건)는 안 옮겼다 — Lyra 는 참
		if (v.count("bCanWalkOffLedgesWhenCrouching")) warn("bCanWalkOffLedgesWhenCrouching", v["bCanWalkOffLedgesWhenCrouching"] == 0);
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
	Columns cols;
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
		if (std::strncmp(line, "frame,", 6) == 0)
		{
			cols = ParseHeader(line);
			continue;
		}
		if (line[0] < '0' || line[0] > '9') continue;
		Row r;
		if (ParseRow(line, cols, r)) rows.push_back(r);
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
	if (values.count("CrouchedHalfHeight"))
	{
		params.canCrouch = true;
		params.maxWalkSpeedCrouched = (float)values["MaxWalkSpeedCrouched"];
		params.crouchedHalfHeight = (float)values["CrouchedHalfHeight"];
	}

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
	if (cols.crouch)
	{
		std::printf("PARAM crouch canCrouch=%d maxWalkCrouched=%g crouchedHalf=%g\n", params.canCrouch ? 1 : 0, params.maxWalkSpeedCrouched, params.crouchedHalfHeight);
	}
	std::printf("ROWS total=%zu bad=%zu\n", rows.size(), badRows);
	if (rows.empty()) return 1;

	// 조준 걸음 속도 — 규칙 = 앞 프레임까지 누른 조준이면 MaxWalkSpeed × ADSMultiplier(0.5, GA_ADS 기본 객체), 아니면 MaxWalkSpeed.
	// 같은 프레임 누름으로 보는 셈(lag0)도 같이 세 어느 쪽이 Lyra 인지 숫자로 가린다. 개수 = 조준 속도 프레임 · 바뀐 횟수
	// 구간 경계는 둘로 가른다 — 기록기는 시간이 다 되면 같은 몸을 순간 이동시키고(조준 능력이 이어진다),
	// 몸이 바뀌면(죽고 다시 태어남) 구간을 일찍 끊는다. 새 몸은 조준이 꺼진 채 기본 속도로 시작하니 그 첫 프레임은 MaxWalkSpeed 가 정답이다
	if (rows.front().ads >= 0)
	{
		constexpr float kAdsMultiplier = 0.5f;
		std::vector<size_t> segFrames;
		for (const Row& r : rows)
		{
			if (r.seg < 0) continue;
			if ((size_t)r.seg >= segFrames.size()) segFrames.resize(r.seg + 1, 0);
			++segFrames[r.seg];
		}
		size_t fullSeg = 0;
		for (size_t n : segFrames) fullSeg = (std::max)(fullSeg, n);

		size_t compared = 0, adsFrames = 0, changes = 0, ruleDiff = 0, lag0Diff = 0;
		size_t seamTeleport = 0, seamRespawn = 0, seamRespawnHeld = 0;
		int firstBad = -1;
		for (size_t i = 1; i < rows.size(); ++i)
		{
			const Row& prev = rows[i - 1];
			const Row& cur = rows[i];
			if (cur.frame != prev.frame + 1) continue;
			++compared;
			const bool bRespawn = cur.seg != prev.seg && prev.seg >= 0 && (size_t)prev.seg < segFrames.size() && segFrames[prev.seg] < fullSeg;
			if (cur.seg != prev.seg) ++(bRespawn ? seamRespawn : seamTeleport);
			if (bRespawn && prev.ads) ++seamRespawnHeld;
			const float expected = bRespawn ? params.maxWalkSpeed : params.maxWalkSpeed * (prev.ads ? kAdsMultiplier : 1.f);
			const float expectedLag0 = params.maxWalkSpeed * (cur.ads ? kAdsMultiplier : 1.f);
			if (cur.maxSpeed != params.maxWalkSpeed) ++adsFrames;
			if (cur.maxSpeed != prev.maxSpeed) ++changes;
			if (cur.maxSpeed != expected)
			{
				++ruleDiff;
				if (firstBad < 0) firstBad = cur.frame;
			}
			if (cur.maxSpeed != expectedLag0) ++lag0Diff;
		}
		std::printf("ADS compared=%zu adsSpeedFrames=%zu changes=%zu ruleDiff=%zu (firstBad frame %d) lag0Diff=%zu | seam teleport=%zu respawn=%zu (held %zu) fullSeg=%zu\n",
			compared, adsFrames, changes, ruleDiff, firstBad, lag0Diff, seamTeleport, seamRespawn, seamRespawnHeld, fullSeg);
	}

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
		SyncState(s, r, oneSim, cols);
		const MoveCounters beforeCounters = oneSim.GetCounters();
		if (cols.crouch) ApplyCrouchEvent(oneSim, s, r);
		RunTick(oneSim, s, r, cols);
		oneStats.Add(s, r);
		if (cols.crouch) oneStats.AddCrouch(oneSim, s, r);

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
	if (cols.crouch) oneStats.PrintCrouch("ONE");
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
			SyncState(run, r, runSim, cols);
			if (cols.crouch) ApplyCrouchEvent(runSim, run, r);
		}
		else if (r.ext != 0)
		{
			++resyncs;
			SyncState(run, r, runSim, cols);
			if (cols.crouch) ApplyCrouchEvent(runSim, run, r);
		}
		else
		{
			// 기록기 순서 = 점프 전 일어서기 → 점프. 점프 허용 검사는 틱 안에서 하므로 둘 다 틱 전에 넣으면 된다
			if (cols.crouch && r.cev == 2) ApplyCrouchEvent(runSim, run, r);
			switch (r.ev)
			{
			case 1: runSim.PressJump(run); break;
			case 2: runSim.ReleaseJump(run); break;
			default: break;
			}
			if (cols.crouch && r.cev == 1) ApplyCrouchEvent(runSim, run, r);
		}

		QueryPerformanceCounter(&t0);
		RunTick(runSim, run, r, cols);
		QueryPerformanceCounter(&t1);
		runSeconds += (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
		++runTicks;

		runStats.Add(run, r);
		if (cols.crouch) runStats.AddCrouch(runSim, run, r);
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
	if (cols.crouch) runStats.PrintCrouch("RUN");
	PrintCounters(runSim.GetCounters());
	if (cols.crouch) PrintCrouchCounters(runSim.GetCounters());
	std::printf("TIME tick=%.2fus (ticks=%zu)\n", runTicks ? runSeconds * 1e6 / (double)runTicks : 0.0, runTicks);
	return 0;
}
