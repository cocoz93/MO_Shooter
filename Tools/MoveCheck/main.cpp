// 조각 1-c 대조 도구 — Lyra 에 재생한 입력을 Shared/Movement 에 그대로 넣어 위치·속도·이동 모드를 비교한다.
//
// 사용: MoveCheck <map.mocl> <move.csv> [--mismatch <out.csv>] [--limit <프레임 수>] [--launchers <launchers.txt>] [--teleports <teleports.txt>]
//
// 입력 파일 = MOBMoveRecordSubsystem 이 쓰는 move_<이름>.csv
//   #param <이름> <값>      실행값(블루프린트가 덮은 뒤의 값)
//   frame,seg,ev,dt, 입력3, 앞 위치3, 앞 속도3, 앞 모드, ext, 발사대3, pressed,was,hold,force,count,countPre, maxSpeed,
//   뒤 위치3, 뒤 속도3, 뒤 모드, fBlock,fWalk,fLine,fDist,fLineDist, aCount [, ads] [, cev,cwant,crouched,half,walk,cwantA,crouchedA,halfA]
//   ads(-Ads 기록만) = 이번 프레임에 조준을 누른 상태. 다음 프레임 maxSpeed 가 MaxWalkSpeed × 0.5 인지 센다(GA_ADS — C 조준)
//   앉기 칸(-Crouch 기록만, 컨텐츠 D) = cev(0 없음 / 1 앉기 키 ToggleCrouch / 2 점프 전 일어서기 UnCrouch) · 사건 전 앉으려 함 · 앉음 ·
//   캡슐 반높이 · MaxWalkSpeed, 그리고 이동 뒤 앉으려 함 · 앉음 · 반높이. 앞 상태는 사건 전에 잡았으므로 한 프레임 대조도 사건을 넣고 돌린다
//   점프대 칸(-Pads 기록만, jumppad-plan j-2) = yaw(이동 뒤 몸 요) · pads(이동 뒤 몸이 든 점프대 비트, 차례는 #pad 줄) · cue(쓰지 않음)
//   순간이동 칸(-Teleports 기록만, teleport-plan t-2 — 점프대 칸 뒤) = tele(이동 뒤 몸이 든 순간이동 칸 비트, 차례는 #tele 줄) · tdev(이번 이동 안에서 옮겨졌으면
//   들어간 장치 번호, 아니면 -1) · tcue(쓰지 않음) · cyaw(이동 뒤 조종 요 — bForceDirection 이 돌린다)
//   어느 칸이 있는지는 머리줄(frame,seg,…)의 이름으로 가린다
//   ev: 0 없음 / 1 Jump / 2 StopJumping. 모드: 1 걷기 / 3 낙하. ext=1 이면 프레임 사이에 밖에서 위치가 바뀐 것
//
// 두 가지를 잰다
//   한 프레임: 매 프레임 Lyra 상태에서 시작해 한 번만 돌린 차이 — 어느 프레임에서 어긋나는지 본다
//   이어 달리기: 구간 첫 프레임만 맞추고 끝까지 돌린 차이 — 서버가 실제로 쓰는 방식이다
// --launchers 가 있으면(점프대 기록 + make_launchers.py 표) 셋을 더 잰다(jumppad-plan j-3)
//   PAD  : 프레임마다 Lyra 자리로 센 우리 겹침 비트 ↔ 기록 pads · 우리 겹침 시작이 셈한 발사 ↔ 다음 프레임 기록 발사(프레임 · 속도)
//   RUN  : 이어 달리기가 기록 발사 칸 대신 TickWithLaunchers(우리 겹침 · 우리 셈)로 쏜다. 순간이동(B_Teleport)이 든 구간은 --teleports 가 없으면 뺀다
//   RUNPAD: 그 이어 달리기에서 쏜 프레임 · 속도가 기록과 같은가
// --teleports 가 있으면(순간이동 기록 + make_teleports.py 표, teleport-plan t-3 — --launchers 도 같이 줘야 한다) 넷을 더 잰다
//   TELE_TABLE: 표 읽기 · 기록 #tele 이름 맞춤 · 나갈 자리가 칸 안인 수(0 이어야 잇달아 옮기는 일이 없다)
//   TELE : 프레임마다 Lyra 자리로 센 우리 순간이동 비트 ↔ 기록 tele, 그리고 Lyra 이동 앞 상태에서 한 번 돌린 우리 이동 + 순간이동(TickWithLaunchers)의
//          옮긴 프레임 · 장치 · 자리 · 속도 · 모드 · 몸 요 · 조종 요 · 다음 프레임 발사 ↔ 기록 tdev · 뒤 자리 · 뒤 속도 · amode · yaw · cyaw · 다음 줄 발사 칸.
//          TELE_RESTORE = 옮긴 자리 · 속도를 단정밀도로 줄여 RestoreLaunchers 가 순간이동 발사를 다시 고르나 · 안 옮긴 자리를 옮긴 자리로 잘못 보는 수
//   RUN  : 순간이동이 든 구간을 빼지 않는다 — TickWithLaunchers 가 점프대 · 순간이동을 다 우리 셈으로 돈다. RUNTELE = 옮긴 프레임 · 장치가 기록과 같은가

#include "JumpPads.h"
#include "MapCollision.h"
#include "Movement.h"
#include "Teleports.h"

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
		double yaw = 0;
		int pads = 0, cue = 0;
		int tele = 0, tdev = -1, tcue = 0;
		double cyaw = 0;
	};

	// 머리줄에서 읽은 덧붙은 칸
	struct Columns
	{
		bool ads = false;
		bool crouch = false;
		bool pads = false;	// yaw,pads,cue
		bool tele = false;	// tele,tdev,tcue,cyaw
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
				if (name == "pads") c.pads = true;
				if (name == "tele") c.tele = true;
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
		if (cols.pads)
		{
			r.yaw = D(); r.pads = I(); r.cue = I();
		}
		if (cols.tele)
		{
			r.tele = I(); r.tdev = I(); r.tcue = I(); r.cyaw = D();
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

	// launchers 가 비었으면 기록 발사 칸을 넣고, 있으면 우리 겹침 · 우리 셈으로 쏜다(TickWithLaunchers — 서버 · 예측과 같은 함수)
	// 순간이동은 점프대와 같이 줄 때만(main 이 --teleports 에 --launchers 를 요구) — 기록 발사 칸과 우리 순간이동 발사가 섞이지 않게
	LauncherTickResult RunTick(CMoveSim& sim, MoveState& s, const Row& r, const Columns& cols, const LauncherSet& launchers = LauncherSet(), double capsuleRadius = 0,
		const TeleportSet& teleports = TeleportSet())
	{
		const bool hasLaunch = r.launch.x != 0 || r.launch.y != 0 || r.launch.z != 0;
		// maxSpeed 칸은 이동 전 GetMaxSpeed — 앉아 걸을 때 이미 300 이다. 이동 코드는 MaxWalkSpeed 를 받아 그 틱의 앉음으로 고르므로
		// 앉기 기록은 walk 칸(MaxWalkSpeed)을 준다. 0 이면 이동 금지(Lyra GetMaxSpeed 의 태그 갈래)라 그대로 0
		s.maxSpeed = (cols.crouch && r.maxSpeed != 0.f) ? r.walk : r.maxSpeed;
		if (launchers.count) return TickWithLaunchers(sim, s, r.input, r.dt, r.yaw, launchers, capsuleRadius, teleports);
		sim.Tick(s, r.input, r.dt, hasLaunch ? &r.launch : nullptr);
		return LauncherTickResult();
	}

	bool IsZeroVec(const Vec3& v) { return v.x == 0 && v.y == 0 && v.z == 0; }

	// 두 요의 차(도, −180 ~ 180)
	double YawDiff(double a, double b)
	{
		double d = std::fmod(a - b + 180.0, 360.0);
		if (d < 0) d += 360.0;
		return d - 180.0;
	}

	bool ReadFile(const char* path, std::vector<char>& out)
	{
		FILE* f = nullptr;
		if (fopen_s(&f, path, "rb") != 0 || !f) return false;
		_fseeki64(f, 0, SEEK_END);
		out.resize((size_t)_ftelli64(f));
		_fseeki64(f, 0, SEEK_SET);
		const size_t got = std::fread(out.data(), 1, out.size(), f);
		std::fclose(f);
		return got == out.size();
	}

	// 순간이동 이동 — 한 이동(1/60초)에 500cm 넘게 옮긴 것. 정상 이동은 종단 속도로도 못 간다(30,000cm/s 가 필요). B_Teleport 가 이동 안에서 자리를 옮긴다(j-2 기록 구간 둘)
	bool IsTeleportMove(const Row& r) { return Dist(r.ap, r.p) > 500.0; }

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
		std::printf("usage: MoveCheck <map.mocl> <move.csv> [--mismatch <out.csv>] [--limit <frames>] [--launchers <launchers.txt>] [--teleports <teleports.txt>]\n");
		return 2;
	}
	const char* mismatchPath = nullptr;
	const char* launchersPath = nullptr;
	const char* teleportsPath = nullptr;
	size_t limit = (size_t)-1;
	for (int i = 3; i + 1 < argc; i += 2)
	{
		if (std::strcmp(argv[i], "--mismatch") == 0) mismatchPath = argv[i + 1];
		else if (std::strcmp(argv[i], "--limit") == 0) limit = (size_t)std::strtoull(argv[i + 1], nullptr, 10);
		else if (std::strcmp(argv[i], "--launchers") == 0) launchersPath = argv[i + 1];
		else if (std::strcmp(argv[i], "--teleports") == 0) teleportsPath = argv[i + 1];
	}
	if (teleportsPath && !launchersPath)
	{
		std::printf("--teleports needs --launchers (the run then fires every launch itself, pads and teleports)\n");
		return 2;
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
	std::vector<std::string> recPadNames;	// 기록 #pad 줄 — pads 비트 차례
	std::vector<std::string> recTeleNames;	// 기록 #tele 줄 — tele 비트 · tdev 차례
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
		if (std::strncmp(line, "#pad ", 5) == 0)
		{
			char name[256];
			int index = -1;
			if (sscanf_s(line + 5, "%d %255s", &index, name, (unsigned)sizeof(name)) == 2 && index >= 0)
			{
				if ((size_t)index >= recPadNames.size()) recPadNames.resize((size_t)index + 1);
				recPadNames[(size_t)index] = name;
			}
			continue;
		}
		if (std::strncmp(line, "#tele ", 6) == 0)
		{
			char name[256];
			int index = -1;
			if (sscanf_s(line + 6, "%d %255s", &index, name, (unsigned)sizeof(name)) == 2 && index >= 0)
			{
				if ((size_t)index >= recTeleNames.size()) recTeleNames.resize((size_t)index + 1);
				recTeleNames[(size_t)index] = name;
			}
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

	// 점프대(jumppad-plan j-3) — 표를 읽고 기록의 pads 비트 차례(#pad 줄)를 표 차례로 옮긴다. 이름으로 맞춘다
	std::vector<Launcher> launchers;
	std::vector<int> recToOurs;		// 기록 #pad 번호 → 표 번호(-1 = 표에 없음)
	LauncherSet launcherSet;
	bool launcherWirePass = true;	// 아래 PAD_WIRE — PADCHECK 판정에 같이 든다
	if (launchersPath)
	{
		std::vector<char> text;
		FILE* lf = nullptr;
		if (fopen_s(&lf, launchersPath, "rb") != 0 || !lf)
		{
			std::printf("cannot open %s\n", launchersPath);
			return 1;
		}
		_fseeki64(lf, 0, SEEK_END);
		text.resize((size_t)_ftelli64(lf));
		_fseeki64(lf, 0, SEEK_SET);
		const size_t got = std::fread(text.data(), 1, text.size(), lf);
		std::fclose(lf);
		size_t badLine = 0;
		const ELaunchersLoad lr = got == text.size() ? ParseLaunchers(text.data(), text.size(), launchers, badLine) : ELaunchersLoad::BadLine;
		if (lr != ELaunchersLoad::Ok)
		{
			std::printf("launchers load failed: %s line %zu\n", LaunchersLoadName(lr), badLine);
			return 1;
		}
		// 시험의 시험 — 구 반지름을 일부러 바꿔(cm, 음수 가능) 겹침 · 발사 프레임 대조가 잡는지 본다
		char* radiusBreak = nullptr;
		size_t radiusBreakLen = 0;
		if (_dupenv_s(&radiusBreak, &radiusBreakLen, "MOVECHECK_BREAK_LAUNCHER_RADIUS") == 0 && radiusBreak)
		{
			const double add = std::atof(radiusBreak);
			for (Launcher& l : launchers) l.sphereRadius += add;
			std::printf("BREAK launcherRadius%+g\n", add);
			std::free(radiusBreak);
		}
		launcherSet.items = launchers.data();
		launcherSet.count = launchers.size();
		size_t mapped = 0;
		recToOurs.assign(recPadNames.size(), -1);
		for (size_t i = 0; i < recPadNames.size(); ++i)
		{
			for (size_t j = 0; j < launchers.size(); ++j)
			{
				if (recPadNames[i] == launchers[j].name)
				{
					recToOurs[i] = (int)j;
					++mapped;
					break;
				}
			}
		}
		std::printf("PAD launchers=%zu recPads=%zu mapped=%zu padsColumns=%d\n", launchers.size(), recPadNames.size(), mapped, cols.pads ? 1 : 0);

		// 와이어 왕복(j-5) — 표 → S2C_LAUNCHER → 표가 칸마다 같은가(셈 값은 double 그대로 실어 == 로 같아야 한다).
		// 거름 = 틀린 소식을 받아들이면 bad — 하나씩 칸을 틀리게 만든다. 이름 63자는 받고 64자는 쓰기에서 거른다
		size_t wireOk = 0, wireBad = 0, rejectTried = 0, rejectOk = 0;
		const auto sameLauncher = [](const Launcher& a, const Launcher& b)
		{
			return a.kind == b.kind && a.name == b.name && a.sphereCenter.x == b.sphereCenter.x && a.sphereCenter.y == b.sphereCenter.y && a.sphereCenter.z == b.sphereCenter.z &&
				a.sphereRadius == b.sphereRadius && a.upOverride == b.upOverride && a.multiplier == b.multiplier && a.useTarget == b.useTarget &&
				a.target.x == b.target.x && a.target.y == b.target.y && a.target.z == b.target.z && a.minVerticalVelocity == b.minVerticalVelocity && a.noFallEntry == b.noFallEntry;
		};
		for (size_t i = 0; i < launchers.size(); ++i)
		{
			wire::LauncherInfo info;
			Launcher back;
			const bool ok = LauncherToWire(launchers[i], i, launchers.size(), info) && info.h.size == sizeof(info) && info.h.type == (uint16_t)wire::Msg::S2C_LAUNCHER &&
				info.index == i && info.count == launchers.size() && LauncherFromWire(info, back) && sameLauncher(launchers[i], back);
			++(ok ? wireOk : wireBad);
		}
		if (!launchers.empty())
		{
			const size_t n = launchers.size();
			const auto expectReject = [&](bool accepted) { ++rejectTried; if (!accepted) ++rejectOk; };
			wire::LauncherInfo base;
			Launcher back;
			LauncherToWire(launchers[0], 0, n, base);
			Launcher named = launchers[0];
			named.name.assign(kMaxLauncherName, 'a');
			wire::LauncherInfo info;
			if (!(LauncherToWire(named, 0, n, info) && LauncherFromWire(info, back) && back.name == named.name)) ++wireBad;	// 63자는 지나간다
			named.name.assign(kMaxLauncherName + 1, 'a');
			expectReject(LauncherToWire(named, 0, n, info));
			expectReject(LauncherToWire(launchers[0], n, n, info));
			info = base; info.kind = (uint8_t)ELauncherKind::Push + 1; expectReject(LauncherFromWire(info, back));
			info = base; info.flags = 1 << 2; expectReject(LauncherFromWire(info, back));
			info = base; info.index = info.count; expectReject(LauncherFromWire(info, back));
			info = base; info.count = 0; info.index = 0; expectReject(LauncherFromWire(info, back));
			info = base; info.count = (uint8_t)(kMaxLaunchers + 1); info.index = 0; expectReject(LauncherFromWire(info, back));
			info = base; info.sphereRadius = 0; expectReject(LauncherFromWire(info, back));
			info = base; std::memset(info.name, 'a', sizeof(info.name)); expectReject(LauncherFromWire(info, back));	// 0 으로 안 끝남
			info = base; std::memset(info.name, 0, sizeof(info.name)); expectReject(LauncherFromWire(info, back));	// 빈 이름
		}
		launcherWirePass = wireBad == 0 && wireOk == launchers.size() && rejectOk == rejectTried && rejectTried > 0;
		std::printf("PAD_WIRE size=%zu ok=%zu bad=%zu reject=%zu/%zu\n", sizeof(wire::LauncherInfo), wireOk, wireBad, rejectOk, rejectTried);
	}

	// PAD 한 프레임 — Lyra 자리(이동 앞 · 뒤)로 우리 겹침을 세고, 겹침이 시작된 이동의 끝 속도 · 요로 셈한 발사를 다음 프레임 기록 발사와 맞댄다
	if (launcherSet.count && cols.pads)
	{
		const double radius = params.capsuleRadius;
		size_t maskFrames = 0, maskLyraIn = 0, maskOursIn = 0, maskMismatch = 0;
		size_t begins = 0, fired = 0, refused = 0, matched = 0, beginNoLaunch = 0, launchNoBegin = 0, beginAtEnd = 0;
		size_t teleportMoves = 0, teleportLaunch = 0, lyraLaunchRows = 0, launchOrphan = 0, speedOver001 = 0;
		double speedMax = 0;
		std::vector<size_t> firedPerPad(launcherSet.count, 0);
		size_t restoreTried = 0, restoreFail = 0, restoreMaskDiff = 0;	// 보정 되살림(j-6) — 아래 PAD_RESTORE
		double restoreMax = 0;
		int printedPad = 0;
		for (size_t k = 0; k < rows.size(); ++k)
		{
			const Row& r = rows[k];
			const bool consecutivePrev = k > 0 && rows[k - 1].frame + 1 == r.frame && rows[k - 1].seg == r.seg;
			if (!IsZeroVec(r.launch))
			{
				++lyraLaunchRows;
				if (!consecutivePrev) ++launchOrphan;
			}
			const double halfBefore = (cols.crouch && r.crouched) ? params.crouchedHalfHeight : params.capsuleHalfHeight;
			const double halfAfter = (cols.crouch && r.crouchedA) ? params.crouchedHalfHeight : params.capsuleHalfHeight;
			const uint8_t after = LauncherMask(launcherSet.items, launcherSet.count, r.ap, radius, halfAfter);
			uint8_t lyra = 0;
			for (size_t b = 0; b < recToOurs.size() && b < 31; ++b)
			{
				if (((r.pads >> b) & 1) && recToOurs[b] >= 0) lyra |= (uint8_t)(1u << recToOurs[b]);
			}
			++maskFrames;
			if (lyra) ++maskLyraIn;
			if (after) ++maskOursIn;
			if (lyra != after)
			{
				++maskMismatch;
				if (printedPad < 20)
				{
					++printedPad;
					std::printf("PAD_MASK_DIFF frame=%d seg=%d lyra=%u mine=%u ap=(%.3f %.3f %.3f)\n", r.frame, r.seg, lyra, after, r.ap.x, r.ap.y, r.ap.z);
				}
			}
			const Row* next = (k + 1 < rows.size() && rows[k + 1].frame == r.frame + 1 && rows[k + 1].seg == r.seg) ? &rows[k + 1] : nullptr;
			if (IsTeleportMove(r))
			{
				// 순간이동 — 점프대 대조 밖(--teleports 면 아래 TELE 가 본다). 뒤따르는 발사는 B_Teleport 몫이라 따로 센다
				++teleportMoves;
				if (next && !IsZeroVec(next->launch)) ++teleportLaunch;
				continue;
			}
			const uint8_t before = LauncherMask(launcherSet.items, launcherSet.count, r.p, radius, halfBefore);
			const uint8_t began = (uint8_t)(after & ~before);
			Vec3 expect;
			bool expectFire = false;
			int firedPad = -1;
			for (size_t i = 0; i < launcherSet.count; ++i)
			{
				if (!(began & (1u << i))) continue;
				++begins;
				Vec3 out;
				if (LauncherLaunch(launcherSet.items[i], r.av, r.av, r.yaw, out))
				{
					expect = out;
					expectFire = true;
					firedPad = (int)i;
				}
				else
				{
					++refused;
				}
			}
			if (expectFire)
			{
				++fired;
				++firedPerPad[(size_t)firedPad];
				// 보정 되살림(j-6) — 상태 패킷처럼 이동 끝 자리 · 속도 · 요를 단정밀도로 줄여 RestoreLaunchers 가 같은 점프대 · 같은 발사를 다시 세나(D159 의 오차를 잰다)
				MoveState s;
				s.location = Vec3((float)r.ap.x, (float)r.ap.y, (float)r.ap.z);
				s.velocity = Vec3((float)r.av.x, (float)r.av.y, (float)r.av.z);
				s.crouched = cols.crouch && r.crouchedA;
				++restoreTried;
				if (!RestoreLaunchers(s, launcherSet, radius, halfAfter, true, (double)(float)r.yaw) || !s.launchPending) ++restoreFail;
				else
				{
					restoreMax = std::max(restoreMax, Dist(s.pendingLaunch, expect));
					if (s.launcherMask != after) ++restoreMaskDiff;
				}
			}
			if (!next)
			{
				if (expectFire) ++beginAtEnd;
				continue;
			}
			const bool lyraFire = !IsZeroVec(next->launch);
			if (expectFire && lyraFire)
			{
				++matched;
				const double d = Dist(expect, next->launch);
				speedMax = std::max(speedMax, d);
				if (d > 0.01)
				{
					++speedOver001;
					if (printedPad < 20)
					{
						++printedPad;
						std::printf("PAD_SPEED_DIFF frame=%d pad=%d d=%.5f mine=(%.4f %.4f %.4f) lyra=(%.4f %.4f %.4f)\n", next->frame, firedPad, d,
							expect.x, expect.y, expect.z, next->launch.x, next->launch.y, next->launch.z);
					}
				}
			}
			else if (expectFire || lyraFire)
			{
				++(expectFire ? beginNoLaunch : launchNoBegin);
				if (printedPad < 20)
				{
					++printedPad;
					std::printf("PAD_FRAME_DIFF frame=%d seg=%d mineFire=%d lyraFire=%d began=%u before=%u after=%u\n", r.frame, r.seg, expectFire ? 1 : 0, lyraFire ? 1 : 0, began, before, after);
				}
			}
		}
		std::printf("PAD mask frames=%zu lyraIn=%zu mineIn=%zu mismatch=%zu | begins=%zu fired=%zu refused=%zu matched=%zu beginNoLaunch=%zu launchNoBegin=%zu beginAtEnd=%zu | speedMax=%.6f over0.01=%zu | lyraLaunchRows=%zu teleportMoves=%zu teleportLaunch=%zu orphan=%zu\n",
			maskFrames, maskLyraIn, maskOursIn, maskMismatch, begins, fired, refused, matched, beginNoLaunch, launchNoBegin, beginAtEnd, speedMax, speedOver001,
			lyraLaunchRows, teleportMoves, teleportLaunch, launchOrphan);
		std::printf("PAD_RESTORE tried=%zu fail=%zu maskDiff=%zu maxDiff=%.6f\n", restoreTried, restoreFail, restoreMaskDiff, restoreMax);
		std::printf("PAD_COVER");
		for (size_t i = 0; i < launcherSet.count; ++i) std::printf(" %s%zu=%zu", LauncherKindName(launcherSet.items[i].kind), i, firedPerPad[i]);
		std::printf("\n");
		// 판정 — 겹침 비트가 다 같고, 시작마다 다음 프레임에 같은 속도로 쏘고, 우리가 모르는 발사가 없다(순간이동 몫만 남는다)
		const bool pass = maskMismatch == 0 && beginNoLaunch == 0 && launchNoBegin == 0 && speedOver001 == 0 && matched > 0 &&
			lyraLaunchRows == matched + teleportLaunch + launchOrphan && launchOrphan == 0 && launcherWirePass &&
			restoreTried == fired && restoreFail == 0 && restoreMaskDiff == 0 && restoreMax <= 0.01;
		std::printf("PADCHECK %s\n", pass ? "PASS" : "FAIL");
	}

	// 순간이동(teleport-plan t-3) — 표를 읽고 기록 #tele 차례를 표 차례로 옮긴다(이름으로)
	TeleportTable teleTable;
	TeleportSet teleSet;
	std::vector<int> recTeleToOurs;		// 기록 #tele 번호 → 표 번호(-1 = 표에 없음)
	bool teleTableOk = true;
	bool teleWirePass = true;	// 아래 TELE_WIRE — TELECHECK 판정에 같이 든다
	if (teleportsPath)
	{
		std::vector<char> text;
		size_t badLine = 0;
		const ETeleportsLoad tr = ReadFile(teleportsPath, text) ? ParseTeleports(text.data(), text.size(), teleTable, badLine) : ETeleportsLoad::BadLine;
		if (tr != ETeleportsLoad::Ok)
		{
			std::printf("teleports load failed: %s line %zu\n", TeleportsLoadName(tr), badLine);
			return 1;
		}
		// 시험의 시험 — 칸 반너비에 일부러 더하거나(cm, 음수 가능 → 옮김 프레임 어긋남) 「들어간 자리 그대로」를 꺼(→ 문 포털 옮긴 자리 어긋남) 대조가 잡는지 본다
		char* boxBreak = nullptr;
		size_t boxBreakLen = 0;
		if (_dupenv_s(&boxBreak, &boxBreakLen, "MOVECHECK_BREAK_TELEPORT_BOX") == 0 && boxBreak)
		{
			const double add = std::atof(boxBreak);
			for (Teleporter& t : teleTable.items) t.boxExtent = t.boxExtent + Vec3(add, add, add);
			std::printf("BREAK teleportBox%+g\n", add);
			std::free(boxBreak);
		}
		char* keepBreak = nullptr;
		size_t keepBreakLen = 0;
		if (_dupenv_s(&keepBreak, &keepBreakLen, "MOVECHECK_BREAK_TELEPORT_KEEP") == 0 && keepBreak)
		{
			for (Teleporter& t : teleTable.items) t.keepRelative = false;
			std::printf("BREAK teleportKeepRelative=off\n");
			std::free(keepBreak);
		}
		teleSet.items = teleTable.items.data();
		teleSet.count = teleTable.items.size();
		teleSet.meshBaseZ = teleTable.meshBaseZ;
		size_t mapped = 0;
		recTeleToOurs.assign(recTeleNames.size(), -1);
		for (size_t i = 0; i < recTeleNames.size(); ++i)
		{
			for (size_t j = 0; j < teleTable.items.size(); ++j)
			{
				if (recTeleNames[i] == teleTable.items[j].name)
				{
					recTeleToOurs[i] = (int)j;
					++mapped;
					break;
				}
			}
		}
		// 나갈 자리가 칸 안이면 Lyra 는 잇달아 옮길 텐데 우리는 한 번만 옮긴다(ApplyTeleports) — 선 몸으로 센다
		size_t exitsInBox = 0;
		for (const Teleporter& t : teleTable.items)
		{
			if (TeleportMask(teleSet, t.exit, params.capsuleRadius, params.capsuleHalfHeight)) ++exitsInBox;
		}
		teleTableOk = cols.tele && mapped == recTeleNames.size() && mapped == teleSet.count && exitsInBox == 0 && teleTable.standHalfHeight == (double)params.capsuleHalfHeight;
		std::printf("TELE_TABLE teleports=%zu recTeles=%zu mapped=%zu teleColumns=%d meshBaseZ=%g standHalf=%g (param %g) exitsInBox=%zu\n", teleSet.count, recTeleNames.size(),
			mapped, cols.tele ? 1 : 0, teleTable.meshBaseZ, teleTable.standHalfHeight, params.capsuleHalfHeight, exitsInBox);

		// 와이어 왕복(t-5) — 표 → S2C_TELEPORT → 표가 칸마다 같은가(셈 값은 double 그대로 실어 == 로 같아야 한다). 점프대 PAD_WIRE 와 같은 꼴.
		// 거름 = 틀린 소식을 받아들이면 bad — 하나씩 칸을 틀리게 만든다. 이름 63자는 받고 64자는 쓰기에서 거른다
		size_t wireOk = 0, wireBad = 0, rejectTried = 0, rejectOk = 0;
		const auto same3 = [](const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
		const auto sameTeleporter = [&](const Teleporter& a, const Teleporter& b)
		{
			return a.name == b.name && a.target == b.target && same3(a.boxCenter, b.boxCenter) && same3(a.boxAxisX, b.boxAxisX) && same3(a.boxAxisY, b.boxAxisY) &&
				same3(a.boxExtent, b.boxExtent) && same3(a.root, b.root) && same3(a.rootForward, b.rootForward) && same3(a.exit, b.exit) && same3(a.exitForward, b.exitForward) &&
				a.keepRelative == b.keepRelative && a.launchOnTeleport == b.launchOnTeleport && a.launchMultiplier == b.launchMultiplier && a.forceDirection == b.forceDirection &&
				a.offsetZMax == b.offsetZMax;
		};
		const size_t n = teleTable.items.size();
		for (size_t i = 0; i < n; ++i)
		{
			wire::TeleportInfo info;
			Teleporter back;
			double backMesh = 0;
			const bool ok = TeleportToWire(teleTable.items[i], i, n, teleTable.meshBaseZ, info) && info.h.size == sizeof(info) && info.h.type == (uint16_t)wire::Msg::S2C_TELEPORT &&
				info.index == i && info.count == n && TeleportFromWire(info, back, backMesh) && sameTeleporter(teleTable.items[i], back) && backMesh == teleTable.meshBaseZ;
			++(ok ? wireOk : wireBad);
		}
		{
			const auto expectReject = [&](bool accepted) { ++rejectTried; if (!accepted) ++rejectOk; };
			const double nan = std::nan("");
			wire::TeleportInfo base;
			Teleporter back;
			double backMesh = 0;
			TeleportToWire(teleTable.items[0], 0, n, teleTable.meshBaseZ, base);
			Teleporter named = teleTable.items[0];
			named.name.assign(kMaxTeleportName, 'a');
			wire::TeleportInfo info;
			if (!(TeleportToWire(named, 0, n, teleTable.meshBaseZ, info) && TeleportFromWire(info, back, backMesh) && back.name == named.name)) ++wireBad;	// 63자는 지나간다
			named.name.assign(kMaxTeleportName + 1, 'a');
			expectReject(TeleportToWire(named, 0, n, teleTable.meshBaseZ, info));
			expectReject(TeleportToWire(teleTable.items[0], n, n, teleTable.meshBaseZ, info));		// 차례가 표 밖
			Teleporter self = teleTable.items[0];
			self.target = 0;
			expectReject(TeleportToWire(self, 0, n, teleTable.meshBaseZ, info));					// 짝이 저 자신
			info = base; info.flags = 1 << 3; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.index = info.count; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.count = 0; info.index = 0; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.count = (uint8_t)(kMaxTeleports + 1); expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.target = info.count; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.target = info.index; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.boxExtent[0] = 0; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.boxAxisX[2] = 0.5; expectReject(TeleportFromWire(info, back, backMesh));		// 칸이 기욺
			info = base; info.rootForward[0] *= 2; info.rootForward[1] *= 2; expectReject(TeleportFromWire(info, back, backMesh));	// 단위 벡터 아님
			info = base; info.exitForward[0] *= 2; info.exitForward[1] *= 2; info.exitForward[2] *= 2; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.offsetZMax = -1; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.rootForward[0] = 0; info.rootForward[1] = 0; info.rootForward[2] = 1;
			expectReject(TeleportFromWire(info, back, backMesh));		// 「그대로」 장치(표 0 = 문 포털)인데 뿌리 앞방향이 수평 아님
			info = base; info.meshBaseZ = nan; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.boxCenter[1] = nan; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; info.launchMultiplier = nan; expectReject(TeleportFromWire(info, back, backMesh));
			info = base; std::memset(info.name, 'a', sizeof(info.name)); expectReject(TeleportFromWire(info, back, backMesh));	// 0 으로 안 끝남
			info = base; std::memset(info.name, 0, sizeof(info.name)); expectReject(TeleportFromWire(info, back, backMesh));	// 빈 이름
		}
		teleWirePass = wireBad == 0 && wireOk == n && rejectOk == rejectTried && rejectTried > 0 && teleTable.items[0].keepRelative;
		std::printf("TELE_WIRE size=%zu ok=%zu bad=%zu reject=%zu/%zu\n", sizeof(wire::TeleportInfo), wireOk, wireBad, rejectOk, rejectTried);
	}

	// TELE 한 프레임 — Lyra 자리(이동 뒤)로 센 우리 비트 ↔ 기록 tele, 그리고 Lyra 이동 앞 상태에서 우리 이동 + 순간이동을 한 번 돌려 옮김을 맞댄다.
	// 이번 이동 머리 발사는 기록 칸을 넣는다(점프대 · 순간이동 몫 다 Lyra 값 — 지난 프레임 셈이 틀려도 이 프레임 대조로 안 번진다). 점프대는 안 쏜다(빈 표)
	if (teleSet.count && cols.tele)
	{
		const double radius = params.capsuleRadius;
		CMoveSim teleSim(map, params);
		size_t maskFrames = 0, maskLyraIn = 0, maskOursIn = 0, maskMismatch = 0;
		size_t lyraTeles = 0, mineTeles = 0, matched = 0, mineOnly = 0, lyraOnly = 0, deviceDiff = 0, beginsOver1 = 0;
		size_t posOver001 = 0, velOver001 = 0, modeDiff = 0, yawDiff = 0, cyawChecked = 0, cyawDiff = 0, walkToFall = 0, fallToFall = 0, otherModes = 0;
		size_t launchMatched = 0, launchOver001 = 0, launchMissing = 0, launchAtEnd = 0;
		size_t restoreTried = 0, restoreFail = 0, restoreDeviceDiff = 0, restoreMaskDiff = 0, falseExit = 0, falseExitPending = 0;
		double posMax = 0, velMax = 0, yawMax = 0, launchMax = 0, restoreMax = 0;
		std::vector<size_t> perDevice(teleSet.count, 0);
		int printedTele = 0;
		const auto note = [&]() { if (printedTele >= 20) return false; ++printedTele; return true; };
		for (size_t k = 0; k < rows.size(); ++k)
		{
			const Row& r = rows[k];
			EMoveMode unused;
			if (!ToMode(r.modeBefore, unused)) continue;
			const double halfBefore = (cols.crouch && r.crouched) ? params.crouchedHalfHeight : params.capsuleHalfHeight;
			const double halfAfter = (cols.crouch && r.crouchedA) ? params.crouchedHalfHeight : params.capsuleHalfHeight;
			const uint8_t after = TeleportMask(teleSet, r.ap, radius, halfAfter);
			uint8_t lyra = 0;
			for (size_t b = 0; b < recTeleToOurs.size() && b < 31; ++b)
			{
				if (((r.tele >> b) & 1) && recTeleToOurs[b] >= 0) lyra |= (uint8_t)(1u << recTeleToOurs[b]);
			}
			++maskFrames;
			if (lyra) ++maskLyraIn;
			if (after) ++maskOursIn;
			if (lyra != after)
			{
				++maskMismatch;
				if (note()) std::printf("TELE_MASK_DIFF frame=%d seg=%d lyra=%u mine=%u ap=(%.3f %.3f %.3f)\n", r.frame, r.seg, lyra, after, r.ap.x, r.ap.y, r.ap.z);
			}

			MoveState s;
			SyncState(s, r, teleSim, cols);
			if (cols.crouch) ApplyCrouchEvent(teleSim, s, r);
			ResetTeleports(s, teleSet, radius, halfBefore);
			s.launchPending = !IsZeroVec(r.launch);
			s.pendingLaunch = r.launch;
			s.maxSpeed = (cols.crouch && r.maxSpeed != 0.f) ? r.walk : r.maxSpeed;	// RunTick 과 같다
			const LauncherTickResult res = TickWithLaunchers(teleSim, s, r.input, r.dt, r.yaw, LauncherSet(), radius, teleSet);
			if (res.teleportBegins > 1) ++beginsOver1;
			const Row* next = (k + 1 < rows.size() && rows[k + 1].frame == r.frame + 1 && rows[k + 1].seg == r.seg) ? &rows[k + 1] : nullptr;
			const bool lyraTele = r.tdev >= 0;
			const bool mineTele = res.teleported >= 0;
			if (lyraTele) ++lyraTeles;
			if (mineTele) ++mineTeles;
			if (!mineTele && !lyraTele)
			{
				// 안 옮긴 자리를 보정이 「옮긴 자리」로 볼까 — 발사가 남은 프레임이면 순간이동 발사를 잘못 넣는다
				const Vec3 apFloat((float)r.ap.x, (float)r.ap.y, (float)r.ap.z);
				if (MatchTeleportExit(teleSet, apFloat, radius, 0.01) >= 0)
				{
					++falseExit;
					if (next && !IsZeroVec(next->launch))
					{
						++falseExitPending;
						if (note()) std::printf("TELE_FALSE_EXIT frame=%d seg=%d ap=(%.4f %.4f %.4f)\n", r.frame, r.seg, r.ap.x, r.ap.y, r.ap.z);
					}
				}
			}
			if (lyraTele != mineTele)
			{
				++(mineTele ? mineOnly : lyraOnly);
				if (note()) std::printf("TELE_FRAME_DIFF frame=%d seg=%d mine=%d lyra=%d begins=%d one-frame d=%.4f\n", r.frame, r.seg, res.teleported, r.tdev, res.teleportBegins, Dist(s.location, r.ap));
				continue;
			}
			if (!mineTele) continue;

			++matched;
			++perDevice[(size_t)res.teleported];
			const int lyraDev = (size_t)r.tdev < recTeleToOurs.size() ? recTeleToOurs[(size_t)r.tdev] : -1;
			if (lyraDev != res.teleported)
			{
				++deviceDiff;
				if (note()) std::printf("TELE_DEVICE_DIFF frame=%d mine=%d lyra=%d\n", r.frame, res.teleported, lyraDev);
			}
			const double dp = Dist(s.location, r.ap), dv = Dist(s.velocity, r.av);
			posMax = std::max(posMax, dp);
			velMax = std::max(velMax, dv);
			if (dp > 0.01)
			{
				++posOver001;
				if (note()) std::printf("TELE_POS_DIFF frame=%d dev=%d d=%.5f mine=(%.4f %.4f %.4f) lyra=(%.4f %.4f %.4f)\n", r.frame, res.teleported, dp,
					s.location.x, s.location.y, s.location.z, r.ap.x, r.ap.y, r.ap.z);
			}
			if (dv > 0.01) ++velOver001;
			if (FromMode(s.mode) != r.modeAfter) ++modeDiff;
			if (r.modeBefore == kModeWalking && r.modeAfter == kModeFalling) ++walkToFall;
			else if (r.modeBefore == kModeFalling && r.modeAfter == kModeFalling) ++fallToFall;
			else ++otherModes;
			const double dy = std::fabs(YawDiff(res.teleportYaw, r.yaw));
			yawMax = std::max(yawMax, dy);
			if (dy > 0.01) ++yawDiff;
			if (res.teleportForceYaw)
			{
				++cyawChecked;
				if (std::fabs(YawDiff(res.teleportYaw, r.cyaw)) > 0.01) ++cyawDiff;
			}
			// 뒤따르는 발사 — 다음 프레임 기록 발사 칸(이동 머리가 먹은 값)
			const Teleporter& t = teleSet.items[(size_t)res.teleported];
			if (!next) ++launchAtEnd;
			else if (t.launchOnTeleport && s.launchPending && !IsZeroVec(next->launch))
			{
				++launchMatched;
				const double d = Dist(s.pendingLaunch, next->launch);
				launchMax = std::max(launchMax, d);
				if (d > 0.01) ++launchOver001;
			}
			else if (t.launchOnTeleport || s.launchPending || !IsZeroVec(next->launch))
			{
				++launchMissing;
				if (note()) std::printf("TELE_LAUNCH_DIFF frame=%d dev=%d mine=%d lyra=%d\n", r.frame, res.teleported, s.launchPending ? 1 : 0, IsZeroVec(next->launch) ? 0 : 1);
			}
			// 보정 되살림 — 상태 패킷처럼 옮긴 자리 · 속도 · 요를 단정밀도로 줄여 RestoreLaunchers(점프대 표도 같이)가 같은 장치 · 같은 발사 · 같은 비트를 다시 세나
			MoveState rs;
			rs.location = Vec3((float)r.ap.x, (float)r.ap.y, (float)r.ap.z);
			rs.velocity = Vec3((float)r.av.x, (float)r.av.y, (float)r.av.z);
			rs.crouched = cols.crouch && r.crouchedA;
			if (MatchTeleportExit(teleSet, rs.location, radius, 0.01) != res.teleported) ++restoreDeviceDiff;
			if (t.launchOnTeleport)
			{
				++restoreTried;
				if (!RestoreLaunchers(rs, launcherSet, radius, halfAfter, true, (double)(float)r.yaw, teleSet) || !rs.launchPending) ++restoreFail;
				else
				{
					restoreMax = std::max(restoreMax, Dist(rs.pendingLaunch, TeleportLaunch(t)));
					if (rs.teleportMask != after) ++restoreMaskDiff;
				}
			}
		}
		std::printf("TELE mask frames=%zu lyraIn=%zu mineIn=%zu mismatch=%zu | lyraTeles=%zu mineTeles=%zu matched=%zu mineOnly=%zu lyraOnly=%zu deviceDiff=%zu beginsOver1=%zu\n",
			maskFrames, maskLyraIn, maskOursIn, maskMismatch, lyraTeles, mineTeles, matched, mineOnly, lyraOnly, deviceDiff, beginsOver1);
		std::printf("TELE pos max=%.6f over0.01=%zu | vel max=%.6f over0.01=%zu | modeDiff=%zu (walk->fall %zu fall->fall %zu other %zu) | yaw max=%.6f over0.01=%zu | cyaw checked=%zu over0.01=%zu\n",
			posMax, posOver001, velMax, velOver001, modeDiff, walkToFall, fallToFall, otherModes, yawMax, yawDiff, cyawChecked, cyawDiff);
		std::printf("TELE launch matched=%zu max=%.6f over0.01=%zu missing=%zu atEnd=%zu\n", launchMatched, launchMax, launchOver001, launchMissing, launchAtEnd);
		std::printf("TELE_RESTORE tried=%zu fail=%zu deviceDiff=%zu maskDiff=%zu maxDiff=%.6f | falseExit=%zu (launch pending %zu)\n",
			restoreTried, restoreFail, restoreDeviceDiff, restoreMaskDiff, restoreMax, falseExit, falseExitPending);
		std::printf("TELE_COVER");
		bool covered = true;
		for (size_t i = 0; i < teleSet.count; ++i)
		{
			std::printf(" %zu=%zu", i, perDevice[i]);
			covered = covered && perDevice[i] > 0;
		}
		std::printf("\n");
		// 판정 — 비트가 다 같고, 옮긴 프레임 · 장치 · 자리 · 속도 · 모드 · 요 · 발사가 다 같고, 보정이 같은 발사를 되살리고, 장치마다 한 번 이상 옮겼다
		const bool pass = teleTableOk && maskMismatch == 0 && mineOnly == 0 && lyraOnly == 0 && deviceDiff == 0 && matched > 0 && matched == lyraTeles &&
			posOver001 == 0 && velOver001 == 0 && modeDiff == 0 && yawDiff == 0 && cyawDiff == 0 && launchOver001 == 0 && launchMissing == 0 &&
			restoreFail == 0 && restoreDeviceDiff == 0 && restoreMaskDiff == 0 && restoreMax <= 0.01 && falseExitPending == 0 && covered && teleWirePass;
		std::printf("TELECHECK %s\n", pass ? "PASS" : "FAIL");
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

	// 점프대 이어 달리기 — 순간이동 표가 없으면 순간이동이 든 구간은 뺀다(우리 이동이 안 옮겨 그 뒤가 다 어긋난다). 표가 있으면 다 돈다
	std::vector<int> teleportSegs;
	if (launcherSet.count && !teleSet.count)
	{
		for (const Row& r : rows)
		{
			if (IsTeleportMove(r) && std::find(teleportSegs.begin(), teleportSegs.end(), r.seg) == teleportSegs.end()) teleportSegs.push_back(r.seg);
		}
	}
	size_t runPadMatched = 0, runPadMineOnly = 0, runPadLyraOnly = 0, runPadSpeedOver001 = 0, runPadSkippedRows = 0;
	size_t runTeleMatched = 0, runTeleMineOnly = 0, runTeleLyraOnly = 0, runTeleDeviceDiff = 0;
	int runTelePrinted = 0;
	double runPadSpeedMax = 0;
	int runPadPrinted = 0;

	LARGE_INTEGER freq, t0, t1;
	QueryPerformanceFrequency(&freq);
	double runSeconds = 0;
	size_t runTicks = 0;
	for (const Row& r : rows)
	{
		EMoveMode unused;
		if (!ToMode(r.modeBefore, unused)) continue;
		if (std::find(teleportSegs.begin(), teleportSegs.end(), r.seg) != teleportSegs.end())
		{
			++runPadSkippedRows;
			continue;
		}
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
			if (launcherSet.count) ResetLaunchers(run, launcherSet, params.capsuleRadius, runSim.HalfHeightOf(run));
			if (teleSet.count) ResetTeleports(run, teleSet, params.capsuleRadius, runSim.HalfHeightOf(run));
		}
		else if (r.ext != 0)
		{
			++resyncs;
			SyncState(run, r, runSim, cols);
			if (cols.crouch) ApplyCrouchEvent(runSim, run, r);
			if (launcherSet.count) ResetLaunchers(run, launcherSet, params.capsuleRadius, runSim.HalfHeightOf(run));
			if (teleSet.count) ResetTeleports(run, teleSet, params.capsuleRadius, runSim.HalfHeightOf(run));
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

		const bool padPending = run.launchPending;
		const Vec3 padLaunch = run.pendingLaunch;
		QueryPerformanceCounter(&t0);
		const LauncherTickResult runResult = RunTick(runSim, run, r, cols, launcherSet, params.capsuleRadius, teleSet);
		QueryPerformanceCounter(&t1);
		runSeconds += (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
		++runTicks;
		if (launcherSet.count)
		{
			// 이번 이동 머리에 우리가 넣은 발사 ↔ 기록 발사 칸
			const bool lyraFire = !IsZeroVec(r.launch);
			if (padPending && lyraFire)
			{
				++runPadMatched;
				const double d = Dist(padLaunch, r.launch);
				runPadSpeedMax = std::max(runPadSpeedMax, d);
				if (d > 0.01) ++runPadSpeedOver001;
			}
			else if (padPending || lyraFire)
			{
				++(padPending ? runPadMineOnly : runPadLyraOnly);
				if (runPadPrinted < 20)
				{
					++runPadPrinted;
					std::printf("RUNPAD_FRAME_DIFF frame=%d seg=%d mine=%d lyra=%d\n", r.frame, r.seg, padPending ? 1 : 0, lyraFire ? 1 : 0);
				}
			}
		}

		if (teleSet.count && cols.tele)
		{
			const bool lyraTele = r.tdev >= 0, mineTele = runResult.teleported >= 0;
			if (lyraTele && mineTele)
			{
				++runTeleMatched;
				const int lyraDev = (size_t)r.tdev < recTeleToOurs.size() ? recTeleToOurs[(size_t)r.tdev] : -1;
				if (lyraDev != runResult.teleported) ++runTeleDeviceDiff;
			}
			else if (lyraTele || mineTele)
			{
				++(mineTele ? runTeleMineOnly : runTeleLyraOnly);
				if (runTelePrinted < 20)
				{
					++runTelePrinted;
					std::printf("RUNTELE_FRAME_DIFF frame=%d seg=%d mine=%d lyra=%d d=%.4f\n", r.frame, r.seg, runResult.teleported, r.tdev, Dist(run.location, r.ap));
				}
			}
		}

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
	if (launcherSet.count)
	{
		std::printf("RUNPAD matched=%zu mineOnly=%zu lyraOnly=%zu speedMax=%.6f over0.01=%zu | teleportSegs=%zu skippedRows=%zu\n",
			runPadMatched, runPadMineOnly, runPadLyraOnly, runPadSpeedMax, runPadSpeedOver001, teleportSegs.size(), runPadSkippedRows);
	}
	if (teleSet.count && cols.tele)
	{
		std::printf("RUNTELE matched=%zu mineOnly=%zu lyraOnly=%zu deviceDiff=%zu\n", runTeleMatched, runTeleMineOnly, runTeleLyraOnly, runTeleDeviceDiff);
	}
	PrintCounters(runSim.GetCounters());
	if (cols.crouch) PrintCrouchCounters(runSim.GetCounters());
	std::printf("TIME tick=%.2fus (ticks=%zu)\n", runTicks ? runSeconds * 1e6 / (double)runTicks : 0.0, runTicks);
	return 0;
}
