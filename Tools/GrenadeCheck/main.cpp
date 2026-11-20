// 수류탄 대조 도구 — Lyra 연습장 기록과 Shared/Projectile 걸음을 맞춰 본다(컨텐츠 G-1).
//
//   GrenadeCheck <map_gym.mocl> <기록 폴더> <태그>... [--verbose] [--pawn-row now|prev]
//
// 기록 = Tools/LyraPilot/record_content_probe.ps1 -Mode grenade(G-0). 태그마다 셋을 읽는다
//   cprobe_<태그>.log      MOB_CPROBE_GSPAWN(생긴 자리 · 회전 · 첫 속도) · GHIT(부딪힘) · GBOUNCE(튕김) · GSTOP(멈춤) · GBOOM(터짐)
//                          · GDROP · GWALLSET(기록 도구가 옮긴 수류탄) · FOE_ACTIVE(기록 도구가 상대를 숨기고 충돌을 끈 구간)
//   cprobe_<태그>.csv      사람 둘(나 · 상대)의 캡슐 — 수류탄이 맞을 수 있는 사람
//   cprobe_<태그>_gren.csv 수류탄마다 프레임 끝 자리 · 속도 · 회전(나이 0 줄 = 생긴 프레임의 걸음 뒤)
// 날아감(G-1-b) — 수류탄마다 생긴 상태에서 1/60 초씩 걸어 첫 부딪힘 전 프레임까지 자리 · 속도 · 회전을 견주고,
//   첫 부딪힘이 같은 프레임에 같은 것(맵 · 나 · 상대 · 수류탄)에 났는지, 맞은 거리 · 물린 시각 · 법선이 같은지 본다
// 튕김(G-1-c) — 첫 부딪힘 뒤로도 터지거나 기록이 끝날 때까지 걸어 프레임마다 자리 · 속도 · 회전, 맞음마다 종류 · 시각 · 거리 · 법선 · 겹친 깊이,
//   튕긴 프레임 · 멈춘 프레임 · 멈춘 자리 · 맞아서 터진 프레임을 견준다
//   - 맞음 알림은 맞은 쪽에도 간다(멈춘 수류탄에 다른 수류탄 · 사람이 닿음) — 그 줄은 이 수류탄의 쓸기가 아니라 뺀다(뿌리 자리가 쓸기 선 위에 없음)
//   - 같은 팀 = 생긴 단계가 foe_same(기록 대본이 상대를 내 팀에 넣은 판) — 상대에 맞아도 튕긴다. 그 밖엔 던진 사람이 아닌 사람에 맞으면 터진다
// 신관(G-2-d) — 사람 맞음 없이 터진 것(GBOOM)은 그 프레임이 GrenadeFuseExpired 가 처음 참이 된 걸음과 같은지,
//   맞아 터진 것은 그 앞에 신관이 안 다 됐는지 본다(서버 TickGrenades 와 같은 함수)
// 능력 · 던짐(G-2-a) — 기록 전체를 한 줄기로(쿨다운이 칸을 넘는다) 입력마다 CGrenadeAbility 를 서버 차례(NextInput → TryActivate)로 돌려 Lyra 와 견준다
//   입력 r = 표 r 줄 프레임. 누름 = r−1 줄 g_press(누른 프레임의 다음 프레임 입력 처리에서 켜진다) · 막힘 · 쿨다운 = r−1 줄. 던짐 = GSPAWN frame=r
//   던짐 계산은 표에서 — 몸 · 몸 요 = r−1 줄(이번 입력 이동 전, G-0-a 짐작 4) · 카메라 · 조종 회전 = r 줄(표 카메라 칸은 한 프레임 늦은 값, G-0-a)
//   겨눔 선 = 맵(무기 깃발) + 상대 부위 몸(서 있는 자세 · r−1 줄 자리). Lyra 는 상대 메시 물리 몸에 맞으니 그 갈래는 따로 센다.
//   Lyra 선이 상자 안에서 시작한 것(거리 0)도 따로 센다 — Lyra 겨눔 선은 단순 충돌, 우리 무기 깃발은 복잡 모양(미결 21)
//   기록 도구 손질 둘을 따라 한다 — 쿨다운 지움(GCDCLEAR) · 몸 없는 프레임(죽은 뒤 새 몸까지, 표에 줄이 없음)도 시간은 간다
// 폭발 · 피해(G-2-b) — 터짐(GBOOM)마다 나 · 상대 칸을 TraceGrenadeBlast · GrenadeDamageApplies 로 셈해 Lyra 폭발 선(GTRACE) · 체력(GBOOM 앞>뒤) ·
//   피해 소식 크기(GMSG)와 견준다. 처치 = 체력이 0 이 됨. 팀 · 무적 · 앉음은 기록 칸 그대로 넣는다(무적 시간 · 팀 정하기는 서버 몫)
// 끝 코드: 0 = 전부 통과, 1 = 불일치, 2 = 입력 오류
#include "Grenade.h"
#include "MapCollision.h"
#include "Movement.h"
#include "Projectile.h"
#include "Weapon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace mo;

namespace
{
	// 허용 차 — 기록 표가 소수 여섯째 자리. 위치 · 속도는 이동 대조(E-2-1)와 같은 기준, 회전은 제 판단(축이 쓸기에 들어간다)
	constexpr double kPosTolerance = 0.01;
	constexpr double kVelTolerance = 0.01;
	constexpr double kRotTolerance = 0.001;
	// 첫 부딪힘 — 맞은 거리 · 법선은 기운 캡슐 탐침(G-1-a)과 같은 자릿수, 물린 시각은 엔진이 float 로 잰다(제 판단, 구현 전에 적음)
	constexpr double kHitDistTolerance = 0.01;
	constexpr double kHitTimeTolerance = 1e-5;
	constexpr double kHitNormalTolerance = 0.001;
	constexpr double kHitDepthTolerance = 0.01;		// 시작 겹침 깊이 — 맞은 거리와 같은 자릿수(제 판단, 구현 전에 적음)
	// 튕김 뒤 맞음의 물린 시각 — 시각 차 × 쓸기 길이(cm)로 견준다. 느려진 수류탄은 쓸기가 몇 cm 뿐이라(예: 맞은 거리 0.51 · 시각 0.2025 → 쓸기 2.5cm)
	// 시각 1e-5 가 0.00025cm 로 거리 기준보다 마흔 배 좁다
	constexpr double kHitTimeCmTolerance = 0.01;
	// 튕김 뒤 프레임 — 자리 · 속도는 첫 부딪힘 전과 같은 기준(15판 최대 0.0063 · 0.0053, 플랜 163줄 「튕김마다 오차가 커지므로 첫 판 뒤 다시 정함」)
	constexpr double kBouncePosTolerance = 0.01;
	constexpr double kBounceVelTolerance = 0.01;
	// 회전은 걸음 시작 속도의 방향이다 — 튕긴 뒤 느린 속도(8cm/s 안팎)에서는 속도 차 0.0004 만으로 방향이 0.002도 넘게 갈린다.
	// 15판 최대 0.0031도 · 속도 차(이 프레임 끝 · 앞 프레임 끝 중 큰 쪽)로 설명 안 되는 몫 최대 0.000005도(bounce 줄 unexplained).
	// 그래서 회전 차 자체는 날아감 기준(0.001)의 열 배로 두고, 설명 안 되는 몫을 날아감 기준 그대로 PASS 에 넣는다
	constexpr double kBounceRotTolerance = 0.01;
	constexpr double kStopPosTolerance = 1.0;		// 멈춘 자리 — 플랜 163줄 「1cm 이하」
	constexpr double kPassiveReach = 0.5;			// 이 수류탄 뿌리 자리가 맞음의 쓸기 선에서 이보다 멀면 남이 옮겨 와 닿은 알림(기록 1,862 건은 1e-4 안 · 4 건은 7.8cm 이상)
	constexpr float kFrameSeconds = 1.f / 60.f;	// 기록은 고정 프레임(-UseFixedTimeStep)
	constexpr double kPawnRadius = 35.0;			// Lyra 사람 캡슐 반지름(Movement.h capsuleRadius)
	constexpr double kOwnerReach = 40.0;			// 생긴 자리 = 던진 사람 중심 + 앞 5 + 위 25 — 이 안이면 던진 사람으로 본다
	constexpr int kMaxFrames = 400;
	// 던짐(G-2-a) — 생김 줄은 소수 열째 자리, 표 입력은 다섯째(자리) · 여섯째(각) 자리. 놓을 자리는 더하기뿐이라 표 자릿수 안,
	// 회전 · 첫 속도는 날아감 기준(0.001° · 0.01cm/s)과 같게(제 판단, 구현 전에 적음)
	constexpr double kThrowPosTolerance = 0.001;
	constexpr double kThrowRotTolerance = 0.001;
	constexpr double kThrowVelTolerance = 0.01;
	constexpr double kAimDistTolerance = 0.01;		// 겨눔 선 맵 맞은 거리 — G-1 맞음 거리와 같은 기준
	constexpr double kStandingHalfHeight = 89.0;	// 상대 캡슐 반높이가 이보다 작으면 앉음 — 서 있는 자세로 쏜 부위 몸이 안 맞는다(따로 셈)

	enum : uint32_t
	{
		kIdMe = 1,
		kIdFoe = 2,
		kIdGrenadeBase = 100,
	};

	bool ReadFile(const std::string& path, std::vector<char>& out)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path.c_str(), "rb") != 0 || !file) return false;
		std::fseek(file, 0, SEEK_END);
		const long size = std::ftell(file);
		std::fseek(file, 0, SEEK_SET);
		out.resize((size_t)size);
		const size_t read = std::fread(out.data(), 1, out.size(), file);
		std::fclose(file);
		return read == out.size();
	}

	std::vector<std::string> Lines(const std::vector<char>& data)
	{
		std::vector<std::string> out;
		std::string line;
		for (char c : data)
		{
			if (c == '\n')
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				out.push_back(line);
				line.clear();
			}
			else line.push_back(c);
		}
		if (!line.empty()) out.push_back(line);
		return out;
	}

	std::vector<std::string> SplitComma(const std::string& line)
	{
		std::vector<std::string> out;
		std::string cell;
		std::istringstream in(line);
		while (std::getline(in, cell, ',')) out.push_back(cell);
		return out;
	}

	// "a=1 b=x,y,z" → 이름별 값
	std::map<std::string, std::string> KeyValues(const std::string& text)
	{
		std::map<std::string, std::string> out;
		std::istringstream in(text);
		std::string token;
		while (in >> token)
		{
			const size_t eq = token.find('=');
			if (eq == std::string::npos || eq == 0) continue;
			out[token.substr(0, eq)] = token.substr(eq + 1);
		}
		return out;
	}

	bool ParseVec(const std::string& text, Vec3& out)
	{
		return std::sscanf(text.c_str(), "%lf,%lf,%lf", &out.x, &out.y, &out.z) == 3;
	}

	double Dist(const Vec3& a, const Vec3& b)
	{
		const Vec3 d = a - b;
		return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	}

	double AngleDiff(double a, double b)
	{
		double d = std::fmod(a - b, 360.0);
		if (d > 180.0) d -= 360.0;
		if (d < -180.0) d += 360.0;
		return std::fabs(d);
	}

	double RotDiff(const Rotator& a, const Rotator& b)
	{
		return std::max(AngleDiff(a.pitch, b.pitch), std::max(AngleDiff(a.yaw, b.yaw), AngleDiff(a.roll, b.roll)));
	}

	struct GrenRow
	{
		int frame = 0, age = 0;
		Vec3 location, velocity;
		Rotator rotation;
		bool hidden = false, active = false;
	};

	struct PawnRow
	{
		bool ok = false;
		Vec3 me, foe;
		double meHalfHeight = 0, foeHalfHeight = 0;
		double meHp = 0, foeHp = 0;
	};

	struct RecordHit
	{
		int frame = -1, age = -1;
		std::string who, comp;
		double time = 0, distance = 0, penetrationDepth = 0;
		Vec3 normal;
		bool startPenetrating = false;
	};

	struct Teleport
	{
		int frame = 0;
		Vec3 location, velocity;
		bool keepVelocity = false;		// GWALLSET — 멈춘 수류탄 자리만 옮김
	};

	struct Grenade
	{
		std::string name, step;
		int spawnFrame = 0;
		Vec3 location, velocity;
		Rotator rotation;
		std::map<int, GrenRow> rows;		// 나이 → 줄
		RecordHit firstHit;
		std::map<int, std::vector<RecordHit>> hits;	// 프레임 → 이 수류탄이 쓸다 맞은 것(맞은 쪽 알림은 뺌)
		std::map<int, int> bounces;			// 프레임 → 튕김 수
		int stopFrame = -1;
		Vec3 stopLocation;
		std::vector<Teleport> teleports;
		uint32_t owner = 0;				// kIdMe · kIdFoe · 0(못 찾음)
		bool foeActive = true;			// 생길 때 상대 충돌이 켜져 있었나(기록 MOB_CPROBE_FOE_ACTIVE — 꺼진 상대는 숨김 · 충돌 끔)
		bool exploded = false;			// 로그를 읽는 동안 GBOOM 을 지났나
		int boomFrame = -1;				// GBOOM 프레임(-1 = 터지기 전에 기록이 끝남)
	};

	void AddCounters(ProjectileCounters& a, const ProjectileCounters& b)
	{
		a.steps += b.steps; a.blockedSteps += b.blockedSteps; a.extraIterations += b.extraIterations;
		a.startPenetrating += b.startPenetrating; a.resolveMoved += b.resolveMoved; a.resolveCombined += b.resolveCombined;
		a.resolveWithMove += b.resolveWithMove; a.resolveFailed += b.resolveFailed; a.ignoredOverlaps += b.ignoredOverlaps;
		a.minMoveZeroed += b.minMoveZeroed; a.bounces += b.bounces; a.stopsOnImpact += b.stopsOnImpact; a.stopsOnSlide += b.stopsOnSlide;
		a.multiHits += b.multiHits; a.slides += b.slides; a.corners += b.corners; a.slideMoves += b.slideMoves; a.slideHits += b.slideHits;
		a.slideFriction += b.slideFriction; a.detonations += b.detonations; a.mapPenetrationsDropped += b.mapPenetrationsDropped;
	}

	struct Totals
	{
		int grenades = 0, frames = 0, fails = 0;
		double maxPos = 0, maxVel = 0, maxRot = 0, maxLaunch = 0;
		int sameKind[4] = {};	// 같음의 종류별 — 맵 · 나 · 상대 · 수류탄
		int same = 0, early = 0, late = 0, kindDiff = 0, noneBoth = 0, ownerUnknown = 0, launchFails = 0;
		int hitCompared = 0, hitFails = 0, meshHits = 0;
		double maxHitDist = 0, maxHitTime = 0, maxHitNormal = 0;
		int rowsRead = 0, spawnsRead = 0, hitsRead = 0;
		// 튕김(G-1-c) — 첫 부딪힘 프레임부터
		int bounceGrenades = 0, bounceFrames = 0, bounceFails = 0;
		double maxBouncePos = 0, maxBounceVel = 0, maxBounceRot = 0;
		int bounceFrameDiff = 0, recBounces = 0, simBounces = 0;
		int stopFrameDiff = 0, stopsCompared = 0;
		double maxStopPos = 0;
		int allHitsCompared = 0, allHitFails = 0, hitFrameDiff = 0, hitCountDiff = 0, passiveHits = 0, meshFrames = 0;
		double maxAllHitDist = 0, maxAllHitTime = 0, maxAllHitNormal = 0, maxAllHitDepth = 0;
		int detonateSame = 0, detonateDiff = 0;
		int fuseCompared = 0, fuseDiff = 0;
		int foeInactiveGrenades = 0, foeToggleWhileLive = 0, wallMoves = 0;
		double maxAllHitTimeCm = 0;
		double maxRotUnexplained = -1e9;
		ProjectileCounters count;

		void Add(const Totals& o)
		{
			grenades += o.grenades; frames += o.frames; fails += o.fails;
			maxPos = std::max(maxPos, o.maxPos); maxVel = std::max(maxVel, o.maxVel); maxRot = std::max(maxRot, o.maxRot);
			maxLaunch = std::max(maxLaunch, o.maxLaunch);
			for (int k = 0; k < 4; ++k) sameKind[k] += o.sameKind[k];
			same += o.same; early += o.early; late += o.late; kindDiff += o.kindDiff; noneBoth += o.noneBoth;
			ownerUnknown += o.ownerUnknown; launchFails += o.launchFails;
			hitCompared += o.hitCompared; hitFails += o.hitFails; meshHits += o.meshHits;
			maxHitDist = std::max(maxHitDist, o.maxHitDist); maxHitTime = std::max(maxHitTime, o.maxHitTime);
			maxHitNormal = std::max(maxHitNormal, o.maxHitNormal);
			rowsRead += o.rowsRead; spawnsRead += o.spawnsRead; hitsRead += o.hitsRead;
			bounceGrenades += o.bounceGrenades; bounceFrames += o.bounceFrames; bounceFails += o.bounceFails;
			maxBouncePos = std::max(maxBouncePos, o.maxBouncePos); maxBounceVel = std::max(maxBounceVel, o.maxBounceVel);
			maxBounceRot = std::max(maxBounceRot, o.maxBounceRot);
			bounceFrameDiff += o.bounceFrameDiff; recBounces += o.recBounces; simBounces += o.simBounces;
			stopFrameDiff += o.stopFrameDiff; stopsCompared += o.stopsCompared; maxStopPos = std::max(maxStopPos, o.maxStopPos);
			allHitsCompared += o.allHitsCompared; allHitFails += o.allHitFails; hitFrameDiff += o.hitFrameDiff; hitCountDiff += o.hitCountDiff;
			passiveHits += o.passiveHits; meshFrames += o.meshFrames;
			maxAllHitDist = std::max(maxAllHitDist, o.maxAllHitDist); maxAllHitTime = std::max(maxAllHitTime, o.maxAllHitTime);
			maxAllHitNormal = std::max(maxAllHitNormal, o.maxAllHitNormal); maxAllHitDepth = std::max(maxAllHitDepth, o.maxAllHitDepth);
			detonateSame += o.detonateSame; detonateDiff += o.detonateDiff;
			fuseCompared += o.fuseCompared; fuseDiff += o.fuseDiff;
			foeInactiveGrenades += o.foeInactiveGrenades; foeToggleWhileLive += o.foeToggleWhileLive; wallMoves += o.wallMoves;
			maxAllHitTimeCm = std::max(maxAllHitTimeCm, o.maxAllHitTimeCm);
			maxRotUnexplained = std::max(maxRotUnexplained, o.maxRotUnexplained);
			AddCounters(count, o.count);
		}
	};

	double PointSegmentDistance(const Vec3& p, const Vec3& a, const Vec3& b)
	{
		const Vec3 ab = b - a, ap = p - a;
		const double len = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
		const double t = len < 1e-12 ? 0.0 : std::clamp((ab.x * ap.x + ab.y * ap.y + ab.z * ap.z) / len, 0.0, 1.0);
		return Dist(a + ab * t, p);
	}

	int KindIndex(const ProjectileHit& hit)
	{
		switch (hit.kind)
		{
		case EProjectileHitKind::Map: return 0;
		case EProjectileHitKind::Pawn: return hit.id == kIdMe ? 1 : 2;
		case EProjectileHitKind::Projectile: return 3;
		}
		return 0;
	}

	const char* KindName(const ProjectileHit& hit)
	{
		switch (hit.kind)
		{
		case EProjectileHitKind::Map: return "world";
		case EProjectileHitKind::Pawn: return hit.id == kIdMe ? "me" : "foe";
		case EProjectileHitKind::Projectile: return "gren";
		}
		return "?";
	}

	// 이름만으로 고르지 않고 그 프레임 앞에서 가장 늦게 생긴 같은 이름을 고른다 — 이름이 다시 쓰여도 섞이지 않게
	Grenade* FindGrenade(std::vector<Grenade>& grenades, const std::string& name, int frame)
	{
		Grenade* best = nullptr;
		for (Grenade& g : grenades)
			if (g.name == name && g.spawnFrame <= frame && (!best || g.spawnFrame > best->spawnFrame)) best = &g;
		return best;
	}

	bool RunTag(const CMapCollision& map, const std::string& dir, const std::string& tag, bool verbose, bool pawnRowNow, Totals& total)
	{
		std::vector<char> logData, csvData, grenData;
		const std::string base = dir + "/cprobe_" + tag;
		if (!ReadFile(base + ".log", logData) || !ReadFile(base + ".csv", csvData) || !ReadFile(base + "_gren.csv", grenData))
		{
			std::printf("GRENADECHECK input tag=%s 읽기 실패\n", tag.c_str());
			return false;
		}
		Totals t;
		std::vector<Grenade> grenades;

		// 로그 — 생김 · 부딪힘 · 튕김 · 멈춤 · 옮김
		struct PendingHit { std::string name; RecordHit hit; };
		std::vector<PendingHit> hits;
		struct PendingDrop { std::string name; Teleport tp; };
		std::vector<PendingDrop> drops;
		struct PendingMark { std::string name; int frame; Vec3 location; };
		std::vector<PendingMark> bounceMarks, stopMarks;
		bool foeActive = true;	// 기록 대본이 상대를 숨기고 충돌을 끈 구간(SetFoeActive) — 줄에 프레임이 없어 로그 차례로 가린다
		for (const std::string& line : Lines(logData))
		{
			const size_t foeAt = line.find("MOB_CPROBE_FOE_ACTIVE on=");
			if (foeAt != std::string::npos)
			{
				foeActive = line[foeAt + 25] == '1';
				for (const Grenade& g : grenades)
					if (!g.exploded) ++t.foeToggleWhileLive;	// 날아가는 동안 바뀌면 생길 때 값 하나로는 못 덮는다 — 0 이어야 한다
				continue;
			}
			const size_t at = line.find("MOB_CPROBE_G");
			if (at == std::string::npos) continue;
			const size_t space = line.find(' ', at);
			if (space == std::string::npos) continue;
			const std::string kind = line.substr(at + 11, space - at - 11);
			if (kind == "GBOUNCE" || kind == "GSTOP")
			{
				const auto kv = KeyValues(line.substr(space + 1));
				PendingMark m;
				m.name = kv.count("name") ? kv.at("name") : "";
				m.frame = kv.count("frame") ? std::atoi(kv.at("frame").c_str()) : -1;
				if (m.name.empty() || m.frame < 0 || (kind == "GSTOP" && (!kv.count("loc") || !ParseVec(kv.at("loc"), m.location))))
				{
					std::printf("GRENADECHECK input tag=%s %s 줄 꼴 다름\n", tag.c_str(), kind.c_str());
					return false;
				}
				(kind == "GBOUNCE" ? bounceMarks : stopMarks).push_back(m);
				continue;
			}
			if (kind != "GSPAWN" && kind != "GHIT" && kind != "GDROP" && kind != "GBOOM" && kind != "GWALLSET") continue;
			const auto kv = KeyValues(line.substr(space + 1));
			if (kind == "GBOOM")
			{
				const int frame = kv.count("frame") ? std::atoi(kv.at("frame").c_str()) : -1;
				Grenade* g = kv.count("name") ? FindGrenade(grenades, kv.at("name"), frame) : nullptr;
				if (!g)
				{
					std::printf("GRENADECHECK input tag=%s GBOOM 줄 꼴 다름\n", tag.c_str());
					return false;
				}
				g->exploded = true;
				g->boomFrame = frame;
				continue;
			}
			if (kind == "GWALLSET")
			{
				// 벽 너머 폭발 판 — 멈춘 수류탄을 면 앞으로 옮김(속도 0 그대로). 줄에 이름이 없어 같은 단계에서 가장 늦게 생긴 것
				PendingDrop d;
				d.tp.frame = kv.count("frame") ? std::atoi(kv.at("frame").c_str()) : -1;
				d.tp.keepVelocity = true;
				const std::string step = kv.count("step") ? kv.at("step") : "";
				const Grenade* owner = nullptr;
				for (const Grenade& g : grenades)
					if (g.step == step && g.spawnFrame <= d.tp.frame && (!owner || g.spawnFrame > owner->spawnFrame)) owner = &g;
				if (!owner || !kv.count("grenade") || !ParseVec(kv.at("grenade"), d.tp.location))
				{
					std::printf("GRENADECHECK input tag=%s GWALLSET 줄 꼴 다름\n", tag.c_str());
					return false;
				}
				d.name = owner->name;
				drops.push_back(d);
				++t.wallMoves;
				continue;
			}
			if (kind == "GSPAWN")
			{
				Grenade g;
				g.name = kv.count("name") ? kv.at("name") : "";
				g.step = kv.count("step") ? kv.at("step") : "";
				g.spawnFrame = kv.count("frame") ? std::atoi(kv.at("frame").c_str()) : -1;
				Vec3 rot;
				if (g.name.empty() || g.spawnFrame < 0 || !kv.count("loc") || !kv.count("rot") || !kv.count("vel")
					|| !ParseVec(kv.at("loc"), g.location) || !ParseVec(kv.at("rot"), rot) || !ParseVec(kv.at("vel"), g.velocity))
				{
					std::printf("GRENADECHECK input tag=%s GSPAWN 줄 꼴 다름\n", tag.c_str());
					return false;
				}
				g.rotation.pitch = rot.x;
				g.rotation.yaw = rot.y;
				g.rotation.roll = rot.z;
				g.foeActive = foeActive;
				if (!foeActive) ++t.foeInactiveGrenades;
				grenades.push_back(g);
				++t.spawnsRead;
			}
			else if (kind == "GHIT")
			{
				PendingHit h;
				h.name = kv.count("name") ? kv.at("name") : "";
				h.hit.frame = kv.count("frame") ? std::atoi(kv.at("frame").c_str()) : -1;
				h.hit.age = kv.count("age") ? std::atoi(kv.at("age").c_str()) : -1;
				h.hit.who = kv.count("who") ? kv.at("who") : "";
				h.hit.comp = kv.count("comp") ? kv.at("comp") : "";
				h.hit.time = kv.count("time") ? std::atof(kv.at("time").c_str()) : 0;
				h.hit.distance = kv.count("dist") ? std::atof(kv.at("dist").c_str()) : 0;
				h.hit.penetrationDepth = kv.count("penDepth") ? std::atof(kv.at("penDepth").c_str()) : 0;
				h.hit.startPenetrating = kv.count("pen") && kv.at("pen") == "1";
				Vec3 grenLoc, traceStart, traceEnd;
				if (h.name.empty() || h.hit.frame < 0 || !kv.count("normal") || !ParseVec(kv.at("normal"), h.hit.normal)
					|| !kv.count("grenLoc") || !kv.count("start") || !kv.count("end")
					|| !ParseVec(kv.at("grenLoc"), grenLoc) || !ParseVec(kv.at("start"), traceStart) || !ParseVec(kv.at("end"), traceEnd))
				{
					std::printf("GRENADECHECK input tag=%s GHIT 줄 꼴 다름\n", tag.c_str());
					return false;
				}
				++t.hitsRead;
				if (PointSegmentDistance(grenLoc, traceStart, traceEnd) > kPassiveReach)
				{
					++t.passiveHits;	// 남이 옮겨 와 닿은 알림 — 이 수류탄의 쓸기가 아니다
					continue;
				}
				hits.push_back(h);
			}
			else
			{
				PendingDrop d;
				d.name = kv.count("g2") ? kv.at("g2") : "";
				d.tp.frame = kv.count("frame") ? std::atoi(kv.at("frame").c_str()) : -1;
				if (d.name.empty() || !kv.count("after") || !kv.count("vel") || !ParseVec(kv.at("after"), d.tp.location) || !ParseVec(kv.at("vel"), d.tp.velocity))
				{
					std::printf("GRENADECHECK input tag=%s GDROP 줄 꼴 다름\n", tag.c_str());
					return false;
				}
				drops.push_back(d);
			}
		}
		for (const PendingHit& h : hits)
		{
			Grenade* g = FindGrenade(grenades, h.name, h.hit.frame);
			if (!g) continue;
			if (g->firstHit.frame < 0) g->firstHit = h.hit;
			g->hits[h.hit.frame].push_back(h.hit);
		}
		for (const PendingMark& m : bounceMarks)
		{
			Grenade* g = FindGrenade(grenades, m.name, m.frame);
			if (g) ++g->bounces[m.frame];
		}
		for (const PendingMark& m : stopMarks)
		{
			Grenade* g = FindGrenade(grenades, m.name, m.frame);
			if (g && g->stopFrame < 0)
			{
				g->stopFrame = m.frame;
				g->stopLocation = m.location;
			}
		}
		for (const PendingDrop& d : drops)
		{
			Grenade* g = FindGrenade(grenades, d.name, d.tp.frame);
			if (g) g->teleports.push_back(d.tp);
		}

		// 수류탄 표
		{
			const std::vector<std::string> lines = Lines(grenData);
			if (lines.empty() || lines[0].rfind("frame,name,age,x,y,z,vx,vy,vz,pitch,yaw,roll,hidden,pmActive", 0) != 0)
			{
				std::printf("GRENADECHECK input tag=%s 수류탄 표 머리 다름\n", tag.c_str());
				return false;
			}
			for (size_t i = 1; i < lines.size(); ++i)
			{
				const std::vector<std::string> c = SplitComma(lines[i]);
				if (c.size() < 14) continue;
				GrenRow r;
				r.frame = std::atoi(c[0].c_str());
				r.age = std::atoi(c[2].c_str());
				r.location = Vec3(std::atof(c[3].c_str()), std::atof(c[4].c_str()), std::atof(c[5].c_str()));
				r.velocity = Vec3(std::atof(c[6].c_str()), std::atof(c[7].c_str()), std::atof(c[8].c_str()));
				r.rotation.pitch = std::atof(c[9].c_str());
				r.rotation.yaw = std::atof(c[10].c_str());
				r.rotation.roll = std::atof(c[11].c_str());
				r.hidden = c[12] == "1";
				r.active = c[13] == "1";
				Grenade* g = FindGrenade(grenades, c[1], r.frame);
				if (!g) continue;
				g->rows[r.age] = r;
				++t.rowsRead;
			}
		}

		// 사람 표 — 프레임마다 마지막 줄
		std::map<int, PawnRow> pawns;
		{
			const std::vector<std::string> lines = Lines(csvData);
			if (lines.empty())
			{
				std::printf("GRENADECHECK input tag=%s 사람 표 비었음\n", tag.c_str());
				return false;
			}
			const std::vector<std::string> head = SplitComma(lines[0]);
			std::map<std::string, size_t> col;
			for (size_t i = 0; i < head.size(); ++i) if (!col.count(head[i])) col[head[i]] = i;
			const char* need[] = { "frame", "px", "py", "pz", "capHH", "hp", "foeX", "foeY", "foeZ", "foeHH", "foeHp" };
			for (const char* n : need)
				if (!col.count(n))
				{
					std::printf("GRENADECHECK input tag=%s 사람 표에 %s 칸 없음\n", tag.c_str(), n);
					return false;
				}
			for (size_t i = 1; i < lines.size(); ++i)
			{
				const std::vector<std::string> c = SplitComma(lines[i]);
				if (c.size() < head.size()) continue;
				const auto num = [&](const char* n) { return std::atof(c[col[n]].c_str()); };
				PawnRow p;
				p.ok = true;
				p.me = Vec3(num("px"), num("py"), num("pz"));
				p.foe = Vec3(num("foeX"), num("foeY"), num("foeZ"));
				p.meHalfHeight = num("capHH");
				p.foeHalfHeight = num("foeHH");
				p.meHp = num("hp");
				p.foeHp = num("foeHp");
				pawns[std::atoi(c[col["frame"]].c_str())] = p;
			}
		}

		const ProjectileParams params;
		for (size_t gi = 0; gi < grenades.size(); ++gi)
		{
			Grenade& g = grenades[gi];
			++t.grenades;

			// 첫 속도 공식 — 회전 × 정규화(1, 0, 0.15) × 2500 이 기록 첫 속도와 같은가
			const double launchDiff = Dist(LaunchVelocity(g.rotation, params), g.velocity);
			t.maxLaunch = std::max(t.maxLaunch, launchDiff);
			if (launchDiff > kVelTolerance) ++t.launchFails;

			// 던진 사람 — 생긴 프레임 사람 표에서 가까운 쪽
			{
				const auto it = pawns.find(g.spawnFrame);
				if (it != pawns.end())
				{
					const double dMe = Dist(it->second.me, g.location), dFoe = Dist(it->second.foe, g.location);
					if (dMe <= kOwnerReach && dMe <= dFoe) g.owner = kIdMe;
					else if (dFoe <= kOwnerReach) g.owner = kIdFoe;
				}
				if (g.owner == 0) ++t.ownerUnknown;
			}

			ProjectileState s;
			s.location = g.location;
			s.rotation = g.rotation;
			s.velocity = g.velocity;
			bool failed = false, bounceFailed = false, bounceCompared = false, afterFirst = false;
			double prevDv = 0, prevSpeed = 0;	// 앞 프레임 끝 속도 차 · 기록 속도 크기(회전 차를 설명할 몫)
			std::string firstFail, firstBounceFail;
			bool simHit = false;
			int simHitFrame = -1, simStopFrame = -1, simDetonateFrame = -1, recDetonateFrame = -1;
			int simFuseFrame = -1;		// 서버 신관 판정(GrenadeFuseExpired)이 처음 참이 된 걸음의 프레임
			Vec3 simStopLocation;
			ProjectileHit simFirst;
			const int recHitFrame = g.firstHit.frame;
			const bool sameTeam = g.step == "foe_same";

			for (int age = 0; age < kMaxFrames; ++age)
			{
				const int frame = g.spawnFrame + age;
				const auto rowIt = g.rows.find(age);
				if (rowIt == g.rows.end()) break;	// 기록 끝 · 사라짐
				const GrenRow& r = rowIt->second;

				// 막는 것 — 사람 둘 · 다른 수류탄(이 프레임 걸음 전 자리 = 앞 프레임 끝). 던진 사람이 아닌 사람은 같은 팀 판이 아니면 맞자마자 터진다
				std::vector<ProjectileObstacle> obstacles;
				const auto pawnIt = pawns.find(pawnRowNow ? frame : frame - 1);
				if (pawnIt != pawns.end())
				{
					const PawnRow& p = pawnIt->second;
					if (p.meHp > 0)
					{
						ProjectileObstacle o;
						o.center = p.me; o.radius = kPawnRadius; o.halfHeight = p.meHalfHeight;
						o.id = kIdMe; o.kind = EProjectileHitKind::Pawn; o.owner = g.owner == kIdMe;
						o.detonateOnHit = !o.owner && !sameTeam;
						obstacles.push_back(o);
					}
					if (p.foeHp > 0 && g.foeActive)
					{
						ProjectileObstacle o;
						o.center = p.foe; o.radius = kPawnRadius; o.halfHeight = p.foeHalfHeight;
						o.id = kIdFoe; o.kind = EProjectileHitKind::Pawn; o.owner = g.owner == kIdFoe;
						o.detonateOnHit = !o.owner && !sameTeam;
						obstacles.push_back(o);
					}
				}
				for (size_t oi = 0; oi < grenades.size(); ++oi)
				{
					if (oi == gi) continue;
					const Grenade& other = grenades[oi];
					const int otherAge = frame - 1 - other.spawnFrame;
					if (otherAge < 0) continue;
					const auto ot = other.rows.find(otherAge);
					if (ot == other.rows.end() || ot->second.hidden) continue;
					ProjectileObstacle o;
					o.center = ot->second.location + RotateVector(ot->second.rotation, params.capsuleOffset);
					o.axis = RotateVector(ot->second.rotation, Vec3(0.0, 0.0, 1.0));
					o.radius = params.capsuleRadius;
					o.halfHeight = params.capsuleHalfHeight;
					o.id = kIdGrenadeBase + (uint32_t)oi;
					o.kind = EProjectileHitKind::Projectile;
					obstacles.push_back(o);
				}

				constexpr size_t kFrameHitSlots = 16;
				ProjectileHit frameHits[kFrameHitSlots];
				const size_t n = TickProjectile(map, params, s, kFrameSeconds, obstacles.data(), obstacles.size(), frameHits, kFrameHitSlots);
				if (simFuseFrame < 0 && GrenadeFuseExpired(s)) simFuseFrame = frame;	// 아래 갈래가 끊기 전에 — 숨김 · 맞아 터짐 줄에서도 센다
				std::vector<const ProjectileHit*> simHits;	// 알림이 나간 맞음 — 기록 GHIT 와 같은 것
				int simBounced = 0;
				for (size_t i = 0; i < std::min(n, kFrameHitSlots); ++i)
				{
					if (frameHits[i].dispatched) simHits.push_back(&frameHits[i]);
					if (frameHits[i].bounced) ++simBounced;
				}
				const auto recIt = g.hits.find(frame);
				const std::vector<RecordHit>* recHits = recIt != g.hits.end() ? &recIt->second : nullptr;
				const auto bounceIt = g.bounces.find(frame);
				const int recBounced = bounceIt != g.bounces.end() ? bounceIt->second : 0;

				if (!simHit && !simHits.empty())
				{
					simHit = true;
					simHitFrame = frame;
					simFirst = *simHits[0];
				}
				const bool hitHere = recHits != nullptr || !simHits.empty();
				const bool flight = !afterFirst && !hitHere;

				// 맞음마다 — 같은 차례끼리 종류 · 시작 겹침 · 맞은 거리 · 물린 시각 · 법선 · 겹친 깊이
				if (hitHere)
				{
					const size_t recCount = recHits ? recHits->size() : 0;
					if (recCount == 0 || simHits.empty()) ++t.hitFrameDiff;
					else
					{
						if (recCount != simHits.size()) ++t.hitCountDiff;
						for (size_t i = 0; i < std::min(recCount, simHits.size()); ++i)
						{
							const RecordHit& rh = (*recHits)[i];
							const ProjectileHit& sh = *simHits[i];
							if (rh.comp == "CharacterMesh0")
							{
								++t.meshFrames;	// 사람 메시 물리 몸 — 우리는 캡슐만(미결 18)
								continue;
							}
							++t.allHitsCompared;
							const double dd = std::fabs(sh.contactDistance - rh.distance), dt = std::fabs(sh.time - rh.time), dn = Dist(sh.normal, rh.normal);
							const double dtCm = dt * Dist(sh.traceStart, sh.traceEnd);
							const double dpen = (rh.startPenetrating || sh.startPenetrating) ? std::fabs(sh.penetrationDepth - rh.penetrationDepth) : 0.0;
							t.maxAllHitDist = std::max(t.maxAllHitDist, dd);
							t.maxAllHitTime = std::max(t.maxAllHitTime, dt);
							t.maxAllHitTimeCm = std::max(t.maxAllHitTimeCm, dtCm);
							t.maxAllHitNormal = std::max(t.maxAllHitNormal, dn);
							t.maxAllHitDepth = std::max(t.maxAllHitDepth, dpen);
							if (rh.who != KindName(sh) || rh.startPenetrating != sh.startPenetrating
								|| dd > kHitDistTolerance || dtCm > kHitTimeCmTolerance || dn > kHitNormalTolerance || dpen > kHitDepthTolerance)
							{
								++t.allHitFails;
								if (verbose)
									std::printf("  hit %s@%d age=%d #%zu %s/%s pen=%d/%d dist=%.6f/%.6f time=%.7f/%.7f normal=%.6f depth=%.6f/%.6f\n",
										g.name.c_str(), g.spawnFrame, age, i, rh.who.c_str(), KindName(sh), rh.startPenetrating ? 1 : 0, sh.startPenetrating ? 1 : 0,
										sh.contactDistance, rh.distance, sh.time, rh.time, dn, sh.penetrationDepth, rh.penetrationDepth);
							}
						}
					}
					if (verbose && (recCount != simHits.size()))
						std::printf("  hitCount %s@%d age=%d rec=%zu sim=%zu\n", g.name.c_str(), g.spawnFrame, age, recCount, simHits.size());
				}
				// 튕긴 수 · 멈춘 프레임
				if (recBounced != simBounced)
				{
					++t.bounceFrameDiff;
					if (verbose) std::printf("  bounce %s@%d age=%d rec=%d sim=%d\n", g.name.c_str(), g.spawnFrame, age, recBounced, simBounced);
				}
				t.recBounces += recBounced;
				t.simBounces += simBounced;
				if (s.stopped && simStopFrame < 0)
				{
					simStopFrame = frame;
					simStopLocation = s.location;
				}

				// 터짐 · 숨김. 상대에 맞아 터지면 그 걸음 안에서 숨어 그 프레임 줄이 이미 숨김이다
				if (r.hidden)
				{
					if (s.detonated) simDetonateFrame = frame;
					if (recHits && recBounced == 0)
						for (const RecordHit& rh : *recHits)
							if (rh.who == "foe" || rh.who == "me") recDetonateFrame = frame;
					break;
				}
				if (s.detonated)
				{
					simDetonateFrame = frame;
					break;
				}

				const double dp = Dist(s.location, r.location), dv = Dist(s.velocity, r.velocity), dr = RotDiff(s.rotation, r.rotation);
				if (flight)
				{
					// 부딪힘 전 프레임 대조(G-1-b)
					++t.frames;
					t.maxPos = std::max(t.maxPos, dp);
					t.maxVel = std::max(t.maxVel, dv);
					t.maxRot = std::max(t.maxRot, dr);
					if ((dp > kPosTolerance || dv > kVelTolerance || dr > kRotTolerance) && !failed)
					{
						failed = true;
						char buf[256];
						std::snprintf(buf, sizeof(buf), "age=%d pos=%.6f vel=%.6f rot=%.6f", age, dp, dv, dr);
						firstFail = buf;
					}
				}
				else
				{
					// 첫 부딪힘 프레임부터(G-1-c)
					bounceCompared = true;
					++t.bounceFrames;
					t.maxBouncePos = std::max(t.maxBouncePos, dp);
					t.maxBounceVel = std::max(t.maxBounceVel, dv);
					t.maxBounceRot = std::max(t.maxBounceRot, dr);
					// 회전 차 중 속도 차로 설명 안 되는 몫 — 속도 차 dv 가 바꿀 수 있는 방향은 dv / |v| 라디안까지.
					// 회전은 걸음을 시작할 때 속도의 방향이라(ProjectileMovementComponent.cpp ComputeMoveDelta 옆 OldVelocity) 앞 프레임 끝 속도도 본다
					const double speed = Dist(r.velocity, Vec3());
					// 회전 차 판정 몫 — 속도 차로 설명이 되면(아래 갈래 · 설명 안 되는 몫이 날아감 기준 kRotTolerance 이하) 0, 안 되면 · 셀 수 없으면(느림 · 멈춤) 회전 차 그대로.
					// L-1-a 기록 l1a1 a_step_z300 나이 72(멈추기 세 프레임 전): 회전 차 0.0116도 · 속도 차 0.0020(기준 안) · 설명 안 되는 몫 0.000003도 — 고정 0.01도만 보면 넘김
					double rotJudge = dr;
					if (speed > 1.0 && prevSpeed > 1.0 && !s.stopped)
					{
						const double unexplained = dr - std::max(dv / speed, prevDv / prevSpeed) * 57.29577951308232;
						rotJudge = unexplained > kRotTolerance ? dr : 0.0;
						t.maxRotUnexplained = std::max(t.maxRotUnexplained, unexplained);
						if (verbose && unexplained > 0.0005)
							std::printf("  rot %s@%d age=%d rot=%.6f vel=%.6f speed=%.3f hit=%d sim=%.6f,%.6f,%.6f rec=%.6f,%.6f,%.6f\n", g.name.c_str(), g.spawnFrame, age, dr, dv, speed,
								hitHere ? 1 : 0, s.rotation.pitch, s.rotation.yaw, s.rotation.roll, r.rotation.pitch, r.rotation.yaw, r.rotation.roll);
					}
					if ((dp > kBouncePosTolerance || dv > kBounceVelTolerance || rotJudge > kBounceRotTolerance) && !bounceFailed)
					{
						bounceFailed = true;
						char buf[256];
						std::snprintf(buf, sizeof(buf), "age=%d pos=%.6f vel=%.6f rot=%.6f", age, dp, dv, dr);
						firstBounceFail = buf;
					}
				}
				afterFirst = afterFirst || hitHere;
				prevDv = dv;
				prevSpeed = Dist(r.velocity, Vec3());

				// 기록 도구가 옮긴 수류탄 — 그 프레임 걸음 뒤 자리 · 속도를 덮는다(회전은 그대로)
				for (const Teleport& tp : g.teleports)
					if (tp.frame == frame)
					{
						s.location = tp.location;
						if (!tp.keepVelocity) s.velocity = tp.velocity;
					}
			}

			// 튕김 뒤 — 멈춘 프레임 · 자리, 맞아서 터진 프레임
			if (bounceCompared) ++t.bounceGrenades;
			if (bounceFailed) ++t.bounceFails;
			if (g.stopFrame >= 0 || simStopFrame >= 0)
			{
				if (g.stopFrame != simStopFrame) ++t.stopFrameDiff;
				else
				{
					++t.stopsCompared;
					const double d = Dist(simStopLocation, g.stopLocation);
					t.maxStopPos = std::max(t.maxStopPos, d);
					if (d > kStopPosTolerance) ++t.stopFrameDiff;
				}
			}
			if (simDetonateFrame >= 0 || recDetonateFrame >= 0)
			{
				if (simDetonateFrame == recDetonateFrame) ++t.detonateSame;
				else ++t.detonateDiff;
			}
			// 신관(G-2-d) — 기록에 사람 맞아 터짐이 없는데 터졌으면(GBOOM) 신관 터짐: 그 프레임 = 신관 판정이 처음 참이 된 걸음.
			// 맞아 터진 칸은 그 프레임 앞에서 신관이 다 되면 안 된다(같은 걸음이면 서버는 맞아 터짐을 먼저 본다 — Room TickGrenades)
			if (g.boomFrame >= 0)
			{
				++t.fuseCompared;
				const bool fuseFail = recDetonateFrame < 0 ? simFuseFrame != g.boomFrame : (simFuseFrame >= 0 && simFuseFrame < recDetonateFrame);
				if (fuseFail) ++t.fuseDiff;
				if (verbose && fuseFail)
					std::printf("  fuse %s@%d step=%s boom=%d hitBoom=%d simFuse=%d\n", g.name.c_str(), g.spawnFrame, g.step.c_str(), g.boomFrame, recDetonateFrame, simFuseFrame);
			}
			AddCounters(t.count, s.count);
			if (verbose && (bounceFailed || g.stopFrame != simStopFrame || simDetonateFrame != recDetonateFrame))
				std::printf("  after %s@%d step=%s stop rec=%d sim=%d detonate rec=%d sim=%d %s%s\n", g.name.c_str(), g.spawnFrame, g.step.c_str(),
					g.stopFrame, simStopFrame, recDetonateFrame, simDetonateFrame, bounceFailed ? "fail " : "", firstBounceFail.c_str());

			// 첫 부딪힘
			const char* result;
			if (!simHit && recHitFrame < 0) { ++t.noneBoth; result = "none"; }
			else if (simHit && simHitFrame == recHitFrame)
			{
				if (g.firstHit.who != KindName(simFirst))
				{
					++t.kindDiff;
					result = "kindDiff";
				}
				else if (g.firstHit.comp == "CharacterMesh0")
				{
					// 상대 메시 물리 몸(팔 등)에 먼저 맞음 — 내 쪽은 사람을 캡슐로만 막아 맞은 거리를 못 견준다(프레임 · 종류만 견줌, 미결 18)
					++t.same;
					++t.sameKind[KindIndex(simFirst)];
					++t.meshHits;
					result = "same";
				}
				else
				{
					++t.same;
					++t.sameKind[KindIndex(simFirst)];
					result = "same";
					// 기록의 dist 는 뒤로 물리기 전 맞은 거리, time 은 물린 뒤 시각(PrimitiveComponent.cpp:3361)
					const double dd = std::fabs(simFirst.contactDistance - g.firstHit.distance);
					const double dt = std::fabs(simFirst.time - g.firstHit.time);
					const double dn = Dist(simFirst.normal, g.firstHit.normal);
					++t.hitCompared;
					t.maxHitDist = std::max(t.maxHitDist, dd);
					t.maxHitTime = std::max(t.maxHitTime, dt);
					t.maxHitNormal = std::max(t.maxHitNormal, dn);
					if (dd > kHitDistTolerance || dt > kHitTimeTolerance || dn > kHitNormalTolerance)
					{
						++t.hitFails;
						if (verbose)
							std::printf("  hitDiff %s@%d %s dist=%.6f/%.6f time=%.7f/%.7f normal=%.6f\n", g.name.c_str(), g.spawnFrame,
								g.firstHit.who.c_str(), simFirst.contactDistance, g.firstHit.distance, simFirst.time, g.firstHit.time, dn);
					}
				}
			}
			else if (simHit && (recHitFrame < 0 || simHitFrame < recHitFrame)) { ++t.early; result = "early"; }
			else { ++t.late; result = "late"; }

			if (failed) ++t.fails;
			if (verbose && (failed || (std::strcmp(result, "same") != 0 && std::strcmp(result, "none") != 0)))
				std::printf("  %s@%d step=%s owner=%u rec=%d/%s sim=%d/%s %s%s%s\n", g.name.c_str(), g.spawnFrame, g.step.c_str(), g.owner,
					recHitFrame, g.firstHit.who.c_str(), simHitFrame, simHit ? KindName(simFirst) : "-", result,
					failed ? " fail " : "", firstFail.c_str());
		}

		std::printf("GRENADECHECK tag=%s grenades=%d frames=%d fails=%d maxPosDiff=%.6f maxVelDiff=%.6f maxRotDiff=%.6f firstHit same=%d early=%d late=%d kindDiff=%d none=%d (rows=%d spawns=%d hits=%d)\n",
			tag.c_str(), t.grenades, t.frames, t.fails, t.maxPos, t.maxVel, t.maxRot, t.same, t.early, t.late, t.kindDiff, t.noneBoth,
			t.rowsRead, t.spawnsRead, t.hitsRead);
		std::printf("GRENADECHECK tag=%s bounce grenades=%d frames=%d fails=%d maxPosDiff=%.6f maxVelDiff=%.6f bounceFrameDiff=%d stopFrameDiff=%d hitFails=%d detonateDiff=%d\n",
			tag.c_str(), t.bounceGrenades, t.bounceFrames, t.bounceFails, t.maxBouncePos, t.maxBounceVel, t.bounceFrameDiff, t.stopFrameDiff,
			t.allHitFails + t.hitFrameDiff + t.hitCountDiff, t.detonateDiff);
		total.Add(t);
		return true;
	}

	// ── 능력 · 던짐(G-2-a)
	struct AbilityTotals
	{
		int inputs = 0, presses = 0, lyraThrown = 0, rowGaps = 0, gapFrames = 0, gapNotDead = 0, cdClears = 0;
		int results[6] = {};		// EGrenadeResult 별
		int throwDiff = 0, cooldownDiff = 0, postGameDiff = 0, gabilCompared = 0, gabilDiff = 0;
		int throwCompared = 0, throwFails = 0, branchDiff = 0;
		int branches[3] = {};		// EGrenadeAimBranch 별 — Lyra 선 결과로 돌린 것
		double maxThrowPos = 0, maxThrowRot = 0, maxThrowVel = 0;
		int aimCompared = 0, aimNone = 0, aimMap = 0, aimBody = 0, traceDiff = 0, endFails = 0, startInside = 0, startInsideDiff = 0, simpleSame = 0;
		double maxAimMapDist = 0, maxEndRot = 0, maxEndVel = 0;
		int bodyCompared = 0, bodyMiss = 0, crouchedFoe = 0;
		double maxBodyDist = 0, maxBodyRot = 0;

		void Add(const AbilityTotals& o)
		{
			inputs += o.inputs; presses += o.presses; lyraThrown += o.lyraThrown; rowGaps += o.rowGaps;
			gapFrames += o.gapFrames; gapNotDead += o.gapNotDead; cdClears += o.cdClears;
			startInside += o.startInside; startInsideDiff += o.startInsideDiff; simpleSame += o.simpleSame;
			for (int k = 0; k < 6; ++k) results[k] += o.results[k];
			throwDiff += o.throwDiff; cooldownDiff += o.cooldownDiff; postGameDiff += o.postGameDiff;
			gabilCompared += o.gabilCompared; gabilDiff += o.gabilDiff;
			throwCompared += o.throwCompared; throwFails += o.throwFails; branchDiff += o.branchDiff;
			for (int k = 0; k < 3; ++k) branches[k] += o.branches[k];
			maxThrowPos = std::max(maxThrowPos, o.maxThrowPos); maxThrowRot = std::max(maxThrowRot, o.maxThrowRot);
			maxThrowVel = std::max(maxThrowVel, o.maxThrowVel);
			aimCompared += o.aimCompared; aimNone += o.aimNone; aimMap += o.aimMap; aimBody += o.aimBody;
			traceDiff += o.traceDiff; endFails += o.endFails;
			maxAimMapDist = std::max(maxAimMapDist, o.maxAimMapDist); maxEndRot = std::max(maxEndRot, o.maxEndRot);
			maxEndVel = std::max(maxEndVel, o.maxEndVel);
			bodyCompared += o.bodyCompared; bodyMiss += o.bodyMiss; crouchedFoe += o.crouchedFoe;
			maxBodyDist = std::max(maxBodyDist, o.maxBodyDist); maxBodyRot = std::max(maxBodyRot, o.maxBodyRot);
		}
	};

	struct AbilityRow
	{
		double t = 0;
		std::string ev;				// 그 프레임 줄들의 사건을 이어 붙임
		Vec3 body, camera, foe;
		double yaw = 0, hp = 0, cooldown = 0, camPitch = 0, camYaw = 0, ctlPitch = 0, ctlYaw = 0, foeYaw = 0, foeHalfHeight = 0, foeHp = 0;
		bool dash = false;
	};

	struct SpawnRecord
	{
		std::string step;
		Vec3 location, velocity;
		Rotator rotation;
		bool foeActive = true;
	};

	struct AimRecord
	{
		bool hit = false, body = false;
		double distance = 0;
		Vec3 location;
		std::string branch;
	};

	int BranchIndex(const std::string& name)
	{
		if (name == "ctl") return (int)EGrenadeAimBranch::Control;
		if (name == "hit") return (int)EGrenadeAimBranch::LookAtHit;
		return (int)EGrenadeAimBranch::LookAtEnd;
	}

	bool RunAbilityTag(const CMapCollision& map, const std::string& dir, const std::string& tag, bool verbose, AbilityTotals& total)
	{
		std::vector<char> logData, csvData;
		const std::string base = dir + "/cprobe_" + tag;
		if (!ReadFile(base + ".log", logData) || !ReadFile(base + ".csv", csvData))
		{
			std::printf("GRENADECHECK input tag=%s 읽기 실패\n", tag.c_str());
			return false;
		}
		AbilityTotals t;

		// 로그 — 생김 · 겨눔 선 · 능력 알림 · 단계 · 죽은 몸(기록 도구가 체력만 1000 으로 되돌린 몸)
		std::map<int, SpawnRecord> spawns;
		std::map<int, AimRecord> aims;
		std::map<int, std::string> gabil;		// 프레임 → on · cooldown · blocked(수류탄 능력만, 그 프레임 첫 줄)
		std::set<int> cdClears;					// 기록 도구가 쿨다운 효과를 지운 프레임(t_cdclear) — 그 프레임 표 값이 이미 0 이라 다음 입력부터 0
		int postFrame = -1;
		bool startDead = false;
		double revivedAt = -1;					// 시작 때 죽은 몸이면 새 몸이 생긴 때(초)
		bool foeActive = true;
		for (const std::string& line : Lines(logData))
		{
			const size_t at = line.find("MOB_CPROBE_");
			if (at == std::string::npos) continue;
			const size_t space = line.find(' ', at);
			if (space == std::string::npos) continue;
			const std::string kind = line.substr(at + 11, space - at - 11);
			const std::map<std::string, std::string> kv = KeyValues(line.substr(space + 1));
			const auto get = [&](const char* k) { const auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };
			if (kind == "FOE_ACTIVE") foeActive = get("on") == "1";
			else if (kind == "GCDCLEAR") cdClears.insert(std::atoi(get("frame").c_str()));
			else if (kind == "PARAMS") startDead = std::atof(get("hp").c_str()) <= 0.0;
			else if (kind == "PAWN_CHANGED")
			{
				if (startDead && revivedAt < 0 && get("to") != "None") revivedAt = std::atof(get("t").c_str());
			}
			else if (kind == "PHASE")
			{
				if (get("post") == "1" && postFrame < 0) postFrame = std::atoi(get("frame").c_str());
			}
			else if (kind == "GSPAWN")
			{
				SpawnRecord s;
				s.step = get("step");
				Vec3 rot;
				if (!ParseVec(get("loc"), s.location) || !ParseVec(get("vel"), s.velocity) || !ParseVec(get("rot"), rot)) continue;
				s.rotation.pitch = rot.x; s.rotation.yaw = rot.y; s.rotation.roll = rot.z;
				s.foeActive = foeActive;
				spawns[std::atoi(get("frame").c_str())] = s;
			}
			else if (kind == "GAIM")
			{
				if (get("which") != "now") continue;
				AimRecord a;
				a.hit = get("hit") == "1";
				a.distance = std::atof(get("dist").c_str());
				ParseVec(get("hitLoc"), a.location);
				a.body = get("comp") == "CharacterMesh0";
				a.branch = get("branch");
				aims[std::atoi(get("frame").c_str())] = a;
			}
			else if (kind == "GABIL")
			{
				if (line.find("GA_Grenade") == std::string::npos) continue;
				const int frame = std::atoi(get("frame").c_str());
				const std::string ev = get("ev");
				std::string what;
				if (ev == "on") what = "on";
				else if (ev == "fail" && line.find("ActivateFail.Cooldown") != std::string::npos) what = "cooldown";
				else if (ev == "fail" && line.find("ActivateFail.TagsBlocked") != std::string::npos) what = "blocked";
				else if (ev == "fail") what = "otherFail";
				if (!what.empty() && !gabil.count(frame)) gabil[frame] = what;
			}
		}

		// 표 — 프레임마다(같은 프레임 줄이 여럿이면 값은 마지막 줄, 사건은 이어 붙임)
		std::map<int, AbilityRow> rows;
		{
			const std::vector<std::string> lines = Lines(csvData);
			if (lines.empty()) return false;
			const std::vector<std::string> head = SplitComma(lines[0]);
			std::map<std::string, size_t> col;
			for (size_t i = 0; i < head.size(); ++i) if (!col.count(head[i])) col[head[i]] = i;
			const char* need[] = { "frame", "t", "ev", "px", "py", "pz", "yaw", "hp", "gCooldown", "dashActive", "camX", "camPitch", "camYaw",
				"ctlPitch", "ctlYaw", "foeX", "foeY", "foeZ", "foeYaw", "foeHH", "foeHp" };
			for (const char* n : need)
				if (!col.count(n))
				{
					std::printf("GRENADECHECK input tag=%s 사람 표에 %s 칸 없음\n", tag.c_str(), n);
					return false;
				}
			// 카메라 z 는 이름이 둘이다(앞쪽 camZ 는 다른 값) — camX 바로 뒤 둘을 쓴다
			const size_t camX = col["camX"];
			if (camX + 2 >= head.size() || head[camX + 1] != "camY" || head[camX + 2] != "camZ")
			{
				std::printf("GRENADECHECK input tag=%s 카메라 칸 차례가 다름\n", tag.c_str());
				return false;
			}
			for (size_t i = 1; i < lines.size(); ++i)
			{
				const std::vector<std::string> c = SplitComma(lines[i]);
				if (c.size() < head.size()) continue;
				const auto num = [&](const char* n) { return std::atof(c[col[n]].c_str()); };
				AbilityRow& r = rows[std::atoi(c[col["frame"]].c_str())];
				r.t = num("t");
				r.ev += c[col["ev"]];
				r.body = Vec3(num("px"), num("py"), num("pz"));
				r.yaw = num("yaw");
				r.hp = num("hp");
				r.cooldown = num("gCooldown");
				r.dash = std::atoi(c[col["dashActive"]].c_str()) != 0;
				r.camera = Vec3(std::atof(c[camX].c_str()), std::atof(c[camX + 1].c_str()), std::atof(c[camX + 2].c_str()));
				r.camPitch = num("camPitch"); r.camYaw = num("camYaw");
				r.ctlPitch = num("ctlPitch"); r.ctlYaw = num("ctlYaw");
				r.foe = Vec3(num("foeX"), num("foeY"), num("foeZ"));
				r.foeYaw = num("foeYaw"); r.foeHalfHeight = num("foeHH"); r.foeHp = num("foeHp");
			}
		}
		if (rows.size() < 2) return false;

		const ProjectileParams params;
		CGrenadeAbility ability;
		// 죽음 = 체력 0, 또는 시작 몸이 죽은 몸(PARAMS hp=0 — 표 체력은 기록 도구가 1000 으로 되돌려 보임)인 동안 새 몸이 생기기 전까지
		const auto isDead = [&](const AbilityRow& row) { return row.hp <= 0.0 || (startDead && (revivedAt < 0 || row.t < revivedAt)); };
		int prevFrame = rows.begin()->first;
		for (auto it = std::next(rows.begin()); it != rows.end(); ++it)
		{
			const int r = it->first;
			const AbilityRow& prev = rows[prevFrame];
			const AbilityRow& cur = it->second;
			// 표에 없는 프레임 = 몸이 없던 프레임(죽은 뒤 새 몸까지 — 앞 줄 체력 0). Lyra 쿨다운은 시간으로 가니 빈 프레임도 입력 하나씩 센다
			if (r != prevFrame + 1)
			{
				++t.rowGaps;
				if (!isDead(prev)) ++t.gapNotDead;
			}
			for (int f = prevFrame + 1; f <= r; ++f)
			{
				if (cdClears.count(f - 1))
				{
					++t.cdClears;
					ability.Reset();			// 기록 도구가 쿨다운 효과를 지움 — 서버에는 없는 손질
				}
				if (f == r) break;
				ability.NextInput();
				++t.gapFrames;
			}
			prevFrame = r;
			++t.inputs;

			ability.NextInput();
			if ((ability.CooldownLeft() > 0) != (prev.cooldown > 0.0)) ++t.cooldownDiff;

			const bool pressed = prev.ev.find("g_press") != std::string::npos;
			const bool dead = isDead(prev);
			const bool postGame = postFrame >= 0 && r > postFrame;
			EGrenadeResult res = EGrenadeResult::NotPressed;
			if (pressed)
			{
				++t.presses;
				res = ability.TryActivate(dead, prev.dash, postGame);
			}
			++t.results[(int)res];

			const auto sp = spawns.find(r);
			const bool lyraThrown = sp != spawns.end();
			if (lyraThrown) ++t.lyraThrown;
			if (lyraThrown != (res == EGrenadeResult::Thrown))
			{
				if (lyraThrown && res == EGrenadeResult::PostGame)
				{
					++t.postGameDiff;			// 미결 15 — 우리는 끝 단계에 막는다. 뒤 쿨다운 대조가 이어지게 Lyra 쪽으로 맞춘다
					ability.StartFromServer();
				}
				else
				{
					++t.throwDiff;
					if (verbose) std::printf("  throwDiff r=%d pressed=%d res=%d lyra=%d cdLeft=%d prevCd=%.4f\n", r, pressed, (int)res, lyraThrown, ability.CooldownLeft(), prev.cooldown);
				}
			}
			const auto gb = gabil.find(r);
			if (gb != gabil.end())
			{
				++t.gabilCompared;
				const bool same = (gb->second == "on" && (res == EGrenadeResult::Thrown || (res == EGrenadeResult::PostGame && lyraThrown)))
					|| (gb->second == "cooldown" && res == EGrenadeResult::OnCooldown)
					|| (gb->second == "blocked" && res == EGrenadeResult::WhileDash);
				if (!same)
				{
					++t.gabilDiff;
					if (verbose) std::printf("  gabilDiff r=%d lyra=%s res=%d\n", r, gb->second.c_str(), (int)res);
				}
			}
			if (!lyraThrown) continue;

			// 던짐 — Lyra 선 결과로 한 번(계산 대조) · 우리 선으로 한 번(겨눔 선 대조)
			const SpawnRecord& s = sp->second;
			const auto am = aims.find(r);
			if (am == aims.end()) { ++t.throwFails; continue; }
			const AimRecord& la = am->second;
			++t.throwCompared;
			const Vec3 spawn = GrenadeSpawnLocation(prev.body, prev.yaw);
			Rotator camRot; camRot.pitch = cur.camPitch; camRot.yaw = cur.camYaw;
			Rotator ctlRot; ctlRot.pitch = cur.ctlPitch; ctlRot.yaw = cur.ctlYaw;
			Vec3 traceStart, traceEnd;
			GrenadeTraceSegment(cur.camera, camRot, traceStart, traceEnd);

			GrenadeAimHit lyraAim;
			lyraAim.hit = la.hit; lyraAim.distance = la.distance; lyraAim.location = la.location; lyraAim.body = la.body;
			const GrenadeThrow fromLyra = ComputeGrenadeThrow(spawn, ctlRot, traceEnd, lyraAim, params);
			++t.branches[(int)fromLyra.branch];
			if ((int)fromLyra.branch != BranchIndex(la.branch)) ++t.branchDiff;
			const double dPos = Dist(spawn, s.location), dRot = RotDiff(fromLyra.rotation, s.rotation), dVel = Dist(fromLyra.velocity, s.velocity);
			t.maxThrowPos = std::max(t.maxThrowPos, dPos); t.maxThrowRot = std::max(t.maxThrowRot, dRot); t.maxThrowVel = std::max(t.maxThrowVel, dVel);
			const bool throwFail = dPos > kThrowPosTolerance || dRot > kThrowRotTolerance || dVel > kThrowVelTolerance;
			if (throwFail) ++t.throwFails;

			BodyPlacement bodies[2];
			bodies[0].center = prev.body; bodies[0].yawDegrees = prev.yaw; bodies[0].id = kIdMe;
			bodies[1].center = prev.foe; bodies[1].yawDegrees = prev.foeYaw; bodies[1].id = kIdFoe;
			bodies[1].skip = !s.foeActive || prev.foeHp <= 0.0;
			const bool foeCrouched = !bodies[1].skip && prev.foeHalfHeight < kStandingHalfHeight;
			if (foeCrouched) ++t.crouchedFoe;
			const GrenadeAimHit ours = TraceGrenadeAim(map, traceStart, traceEnd, bodies, 2, kIdMe);
			const GrenadeThrow end = ComputeGrenadeThrow(spawn, ctlRot, traceEnd, ours, params);
			++t.aimCompared;
			const char* aimResult = "same";
			if (la.body || ours.body)
			{
				++t.bodyCompared;
				const double dEndRot = RotDiff(end.rotation, s.rotation);
				t.maxBodyRot = std::max(t.maxBodyRot, dEndRot);
				if (la.body && ours.body && la.hit == ours.hit)
				{
					++t.aimBody;
					t.maxBodyDist = std::max(t.maxBodyDist, std::fabs(ours.distance - la.distance));
				}
				else { ++t.bodyMiss; aimResult = "bodyMiss"; }
			}
			else
			{
				if (!la.hit) ++t.aimNone; else ++t.aimMap;
				// 진단 — 같은 선을 단순 나무(수류탄 깃발 · 속 찬 상자는 안에서 거리 0)로 쏘면 Lyra 와 같나. 겨눔 선은 단순 충돌이다(GA_Grenade 의 LineTraceSingle 에
				// bTraceComplex 핀이 없음 = 기본 false, 폭발 선은 true 가 찍힘 — gren0/ga_flow.txt:107-116 · b_flow.txt:53-60). 맵 파일엔 「단순 모양 + 무기 채널」 깃발이 없다(미결 21)
				RayHit simpleHit;
				const bool simpleHitAny = map.Raycast(traceStart, traceEnd, MAP_BLOCK_GRENADE, simpleHit);
				if (simpleHitAny == la.hit && (!la.hit || std::fabs(simpleHit.distance - la.distance) <= kAimDistTolerance)) ++t.simpleSame;
				// Lyra 가 상자 안에서 시작(거리 0 · 맞은 점 = 선 시작) — 우리 무기 깃발은 복잡 모양(면)이라 안에서 시작해도 거리 0 이 안 된다. 따로 센다
				const bool lyraInside = la.hit && la.distance == 0.0 && Dist(la.location, traceStart) <= kAimDistTolerance;
				if (lyraInside) ++t.startInside;
				const double dDist = std::fabs(ours.distance - la.distance);
				const double dEndRot = RotDiff(end.rotation, s.rotation), dEndVel = Dist(end.velocity, s.velocity);
				if (lyraInside && (!ours.hit || dDist > kAimDistTolerance)) { ++t.startInsideDiff; aimResult = "startInside"; }
				else
				{
					if (la.hit && ours.hit) t.maxAimMapDist = std::max(t.maxAimMapDist, dDist);
					t.maxEndRot = std::max(t.maxEndRot, dEndRot); t.maxEndVel = std::max(t.maxEndVel, dEndVel);
					if (la.hit != ours.hit || dDist > kAimDistTolerance) { ++t.traceDiff; aimResult = "traceDiff"; }
					else if (dEndRot > kThrowRotTolerance || dEndVel > kThrowVelTolerance) { ++t.endFails; aimResult = "endFail"; }
				}
			}
			if (verbose && (throwFail || std::strcmp(aimResult, "same") != 0 || (int)fromLyra.branch != BranchIndex(la.branch)))
				std::printf("  throw r=%d step=%s branch lyra=%s ours=%d pos=%.6f rot=%.6f vel=%.6f aim %s lyra hit=%d body=%d dist=%.4f at=%.4f,%.4f,%.4f ours hit=%d body=%d dist=%.4f at=%.4f,%.4f,%.4f start=%.4f,%.4f,%.4f%s\n",
					r, s.step.c_str(), la.branch.c_str(), (int)fromLyra.branch, dPos, dRot, dVel, aimResult, la.hit, la.body, la.distance,
					la.location.x, la.location.y, la.location.z, ours.hit, ours.body, ours.distance, ours.location.x, ours.location.y, ours.location.z,
					traceStart.x, traceStart.y, traceStart.z, foeCrouched ? " foeCrouched" : "");
		}

		std::printf("GRENADECHECK tag=%s ability presses=%d thrown=%d lyraThrown=%d throwDiff=%d cooldownDiff=%d postGameDiff=%d gabilDiff=%d throw fails=%d branchDiff=%d aim traceDiff=%d endFails=%d startInsideDiff=%d bodyMiss=%d cdClears=%d gapFrames=%d\n",
			tag.c_str(), t.presses, t.results[(int)EGrenadeResult::Thrown], t.lyraThrown, t.throwDiff, t.cooldownDiff, t.postGameDiff, t.gabilDiff,
			t.throwFails, t.branchDiff, t.traceDiff, t.endFails, t.startInsideDiff, t.bodyMiss, t.cdClears, t.gapFrames);
		total.Add(t);
		return true;
	}

	// ── 폭발 · 피해(G-2-b)
	// 선 거리는 겨눔 선과 같은 기준(구현 전에 적음). 피해는 플랜 170줄 「피해 차 0.001 이하」 — 체력 칸은 소수 셋째 자리라 반올림이 0.0005 까지 든다
	constexpr double kBlastDistTolerance = 0.01;
	constexpr double kDamageTolerance = 0.001;

	struct BlastTotals
	{
		int booms = 0, targets = 0, alive = 0, traced = 0, lineCompared = 0, lineFails = 0, bodyLines = 0, mapLines = 0, farMap = 0;
		int deadAfter = 0, deadAfterNotKilled = 0, notTraced = 0, notTracedNear = 0, notTracedHurt = 0, tracedNotAlive = 0;
		int hhDiff = 0, rowMissing = 0, foeHidden = 0, sameFrameBooms = 0;
		double maxLineDist = 0;
		int damageCompared = 0, damageFails = 0, msgCompared = 0, msgFails = 0, msgMissing = 0, msgExtra = 0, msgOtherInst = 0;
		int hurt = 0, kills = 0, killDiff = 0, sameTeam = 0, immune = 0, dead = 0, crouched = 0;
		double maxDamage = 0, maxMsg = 0;

		void Add(const BlastTotals& o)
		{
			booms += o.booms; targets += o.targets; alive += o.alive; traced += o.traced; lineCompared += o.lineCompared;
			lineFails += o.lineFails; bodyLines += o.bodyLines; mapLines += o.mapLines; farMap += o.farMap;
			deadAfter += o.deadAfter; deadAfterNotKilled += o.deadAfterNotKilled; notTraced += o.notTraced; notTracedNear += o.notTracedNear;
			notTracedHurt += o.notTracedHurt; tracedNotAlive += o.tracedNotAlive;
			hhDiff += o.hhDiff; rowMissing += o.rowMissing; foeHidden += o.foeHidden; sameFrameBooms += o.sameFrameBooms;
			maxLineDist = std::max(maxLineDist, o.maxLineDist);
			damageCompared += o.damageCompared; damageFails += o.damageFails; msgCompared += o.msgCompared; msgFails += o.msgFails;
			msgMissing += o.msgMissing; msgExtra += o.msgExtra; msgOtherInst += o.msgOtherInst;
			hurt += o.hurt; kills += o.kills; killDiff += o.killDiff; sameTeam += o.sameTeam; immune += o.immune; dead += o.dead; crouched += o.crouched;
			maxDamage = std::max(maxDamage, o.maxDamage); maxMsg = std::max(maxMsg, o.maxMsg);
		}
	};

	struct BoomRecord
	{
		int frame = 0;
		std::string name, step;
		Vec3 location;
		double hpBefore[2] = {}, hpAfter[2] = {};	// [0] 나 · [1] 상대
		bool sameTeamFoe = false, foeCrouched = false, foeActive = true;
		bool immune[2] = {};
	};

	struct BlastTraceRecord
	{
		bool hit = false, body = false;
		double distance = 0;
	};

	struct BlastRow
	{
		Vec3 body, foe;
		bool crouched = false;
		double halfHeight = 0, foeHalfHeight = 0;
	};

	bool ParseHpPair(const std::string& text, double& before, double& after)
	{
		return std::sscanf(text.c_str(), "%lf>%lf", &before, &after) == 2;
	}

	// 터짐마다 두 사람(나 · 상대) 칸 — 기록의 터진 자리 · 그 프레임 표 자리(G-0 기록은 터짐 앞뒤 세 줄 자리가 같다 · 313/313)에서 폭발 선 · 피해를 셈해
	//   Lyra 선(GTRACE — 기록 도구가 피해 뒤에 다시 쏜 선, 그래서 그 폭발에 죽은 몸엔 안 맞는다) · 체력(GBOOM 앞>뒤) · 피해 소식(GMSG 크기 = 자르지 않은 피해)과 견준다.
	//   던진 사람은 늘 나(기록 대본). 같은 팀 = foeCmp 0 · 무적 = 기록 칸(부활 · 경기 끝 — 무적 시간은 서버 몫, 미결 14) · 상대 숨김(FOE_ACTIVE 0) = 충돌 꺼짐이라 죽은 몸과 같게
	bool RunBlastTag(const CMapCollision& map, const std::string& dir, const std::string& tag, bool verbose, BlastTotals& total)
	{
		std::vector<char> logData, csvData;
		const std::string base = dir + "/cprobe_" + tag;
		if (!ReadFile(base + ".log", logData) || !ReadFile(base + ".csv", csvData))
		{
			std::printf("GRENADECHECK input tag=%s 읽기 실패\n", tag.c_str());
			return false;
		}
		BlastTotals t;
		const MoveParams move = MakeLyraMoveParams();

		std::vector<BoomRecord> booms;
		std::map<std::string, BlastTraceRecord> traces[2];				// 수류탄 이름 → 사람별 선
		std::map<std::pair<int, int>, std::vector<double>> msgs;		// (프레임, 사람) → 피해 소식 크기
		bool hasMsgLines = false, foeActive = true;
		for (const std::string& line : Lines(logData))
		{
			const size_t at = line.find("MOB_CPROBE_");
			if (at == std::string::npos) continue;
			const size_t space = line.find(' ', at);
			if (space == std::string::npos) continue;
			const std::string kind = line.substr(at + 11, space - at - 11);
			const std::map<std::string, std::string> kv = KeyValues(line.substr(space + 1));
			const auto get = [&](const char* k) { const auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };
			if (kind == "FOE_ACTIVE") foeActive = get("on") == "1";
			else if (kind == "GBOOM")
			{
				BoomRecord b;
				b.frame = std::atoi(get("frame").c_str());
				b.name = get("name");
				b.step = get("step");
				if (!ParseVec(get("loc"), b.location) || !ParseHpPair(get("myHp"), b.hpBefore[0], b.hpAfter[0])
					|| !ParseHpPair(get("foeHp"), b.hpBefore[1], b.hpAfter[1]))
				{
					std::printf("GRENADECHECK input tag=%s 터짐 줄 읽기 실패 frame=%d\n", tag.c_str(), b.frame);
					return false;
				}
				b.sameTeamFoe = get("foeCmp") == "0";
				b.foeCrouched = get("foeCrouched") == "1";
				b.immune[0] = get("myImmune") == "1";
				b.immune[1] = get("foeImmune") == "1";
				b.foeActive = foeActive;
				booms.push_back(b);
			}
			else if (kind == "GTRACE")
			{
				BlastTraceRecord r;
				r.hit = get("hit") == "1";
				r.body = get("hitComp") == "CollisionCylinder";
				r.distance = std::atof(get("dist").c_str());
				traces[get("who") == "me" ? 0 : 1][get("name")] = r;
			}
			else if (kind == "GMSG")
			{
				hasMsgLines = true;
				if (get("verb") != "Lyra.Damage.Message" || line.find("DamageType.Grenade") == std::string::npos) continue;
				if (get("instWho") != "me") ++t.msgOtherInst;
				msgs[{ std::atoi(get("frame").c_str()), get("targetWho") == "me" ? 0 : 1 }].push_back(std::atof(get("mag").c_str()));
			}
		}

		std::map<int, BlastRow> rows;
		{
			const std::vector<std::string> lines = Lines(csvData);
			if (lines.empty()) return false;
			const std::vector<std::string> head = SplitComma(lines[0]);
			std::map<std::string, size_t> col;
			for (size_t i = 0; i < head.size(); ++i) if (!col.count(head[i])) col[head[i]] = i;
			const char* need[] = { "frame", "px", "py", "pz", "crouched", "capHH", "foeX", "foeY", "foeZ", "foeHH" };
			for (const char* n : need)
				if (!col.count(n))
				{
					std::printf("GRENADECHECK input tag=%s 사람 표에 %s 칸 없음\n", tag.c_str(), n);
					return false;
				}
			for (size_t i = 1; i < lines.size(); ++i)
			{
				const std::vector<std::string> c = SplitComma(lines[i]);
				if (c.size() < head.size()) continue;
				const auto num = [&](const char* n) { return std::atof(c[col[n]].c_str()); };
				BlastRow& r = rows[std::atoi(c[col["frame"]].c_str())];
				r.body = Vec3(num("px"), num("py"), num("pz"));
				r.crouched = std::atoi(c[col["crouched"]].c_str()) != 0;
				r.halfHeight = num("capHH");
				r.foe = Vec3(num("foeX"), num("foeY"), num("foeZ"));
				r.foeHalfHeight = num("foeHH");
			}
		}

		std::map<int, int> boomsPerFrame;
		for (const BoomRecord& b : booms) ++boomsPerFrame[b.frame];
		for (const BoomRecord& b : booms)
		{
			++t.booms;
			if (boomsPerFrame[b.frame] > 1) ++t.sameFrameBooms;		// 피해 소식은 프레임으로만 묶여 갈리지 않는다
			const auto rowIt = rows.find(b.frame);
			for (int who = 0; who < 2; ++who)
			{
				++t.targets;
				const double before = b.hpBefore[who], after = b.hpAfter[who];
				const bool hidden = who == 1 && !b.foeActive;
				if (hidden) ++t.foeHidden;
				const bool alive = before > 0.0 && !hidden;
				const bool self = who == 0;
				const bool sameTeam = self || b.sameTeamFoe;
				const bool immune = b.immune[who];
				const auto traceIt = traces[who].find(b.name);
				const bool traced = traceIt != traces[who].end();

				GrenadeBlastHit blast;
				bool lineKnown = false;
				if (alive)
				{
					++t.alive;
					if (immune) ++t.immune;
					if (!self && b.sameTeamFoe) ++t.sameTeam;
					if (rowIt == rows.end()) ++t.rowMissing;
					else
					{
						const BlastRow& row = rowIt->second;
						const bool crouched = self ? row.crouched : b.foeCrouched;
						if (crouched) ++t.crouched;
						GrenadeBlastTarget target;
						target.center = self ? row.body : row.foe;
						target.radius = move.capsuleRadius;
						target.halfHeight = crouched ? move.crouchedHalfHeight : move.capsuleHalfHeight;
						if (std::fabs(target.halfHeight - (self ? row.halfHeight : row.foeHalfHeight)) > kBlastDistTolerance) ++t.hhDiff;
						blast = TraceGrenadeBlast(map, b.location, target);
						lineKnown = true;
					}
				}
				else ++t.dead;

				// 선 — Lyra 가 쏜 사람만(겹침 안). 안 쏜 사람은 우리 피해가 0 이어야 한다
				const char* lineResult = "-";
				if (traced && !alive) { ++t.tracedNotAlive; lineResult = "tracedNotAlive"; }
				else if (traced && lineKnown)
				{
					++t.traced;
					const BlastTraceRecord& lr = traceIt->second;
					if (!lr.hit)
					{
						++t.deadAfter;		// 피해 뒤에 쏜 선이라 그 폭발에 죽은 몸엔 안 맞는다 — 피해는 소식 크기로 본다
						if (after > 0.0) { ++t.deadAfterNotKilled; lineResult = "deadAfterNotKilled"; }
						else lineResult = "deadAfter";
					}
					else
					{
						if (lr.body) ++t.bodyLines; else ++t.mapLines;
						if (!lr.body && blast.bodyDistance >= kGrenadeBlastRadius) { ++t.farMap; lineResult = "farMap"; }	// 우리는 맵을 안 쏜 칸 — 피해 0 은 같다
						else
						{
							++t.lineCompared;
							const double d = std::fabs(blast.distance - lr.distance);
							t.maxLineDist = std::max(t.maxLineDist, d);
							const bool fail = blast.blocked == lr.body || d > kBlastDistTolerance;
							if (fail) ++t.lineFails;
							lineResult = fail ? "lineFail" : "same";
						}
					}
				}
				else if (alive && lineKnown)
				{
					++t.notTraced;
					if (blast.bodyDistance < kGrenadeBlastRadius) ++t.notTracedNear;
				}

				// 피해 · 체력 · 처치 · 소식
				const bool applies = lineKnown && GrenadeDamageApplies(alive, self, sameTeam, immune);
				const double damage = applies ? blast.damage : 0.0;
				if (alive && lineKnown && !traced && damage > 0.0) { ++t.notTracedHurt; lineResult = "notTracedHurt"; }
				const double oursAfter = std::max(0.0, before - damage);
				++t.damageCompared;
				const double hpDiff = std::fabs(oursAfter - after);
				double msgDiff = 0.0;
				const auto msgIt = msgs.find({ b.frame, who });
				const bool hasMsg = msgIt != msgs.end() && !msgIt->second.empty();
				if (hasMsgLines)
				{
					if (damage > 0.0 && !hasMsg) ++t.msgMissing;
					if (damage <= 0.0 && hasMsg) ++t.msgExtra;
					if (damage > 0.0 && hasMsg)
					{
						++t.msgCompared;
						msgDiff = std::fabs(msgIt->second.front() - damage);
						t.maxMsg = std::max(t.maxMsg, msgDiff);
						if (msgDiff > kDamageTolerance) ++t.msgFails;
					}
				}
				t.maxDamage = std::max(t.maxDamage, hpDiff);
				const bool damageFail = hpDiff > kDamageTolerance;
				if (damageFail) ++t.damageFails;
				if (damage > 0.0) ++t.hurt;
				const bool oursKill = before > 0.0 && oursAfter <= 0.0, lyraKill = before > 0.0 && after <= 0.0;
				if (oursKill) ++t.kills;
				if (oursKill != lyraKill) ++t.killDiff;

				if (verbose && (damageFail || msgDiff > kDamageTolerance || oursKill != lyraKill
					|| (std::strcmp(lineResult, "same") != 0 && std::strcmp(lineResult, "-") != 0)))
					std::printf("  blast frame=%d step=%s who=%s alive=%d line=%s ours blocked=%d body=%.4f dist=%.4f grade=%.6f damage=%.4f applies=%d hp %.3f>%.3f ours>%.3f msg=%.4f\n",
						b.frame, b.step.c_str(), self ? "me" : "foe", alive, lineResult, blast.blocked, blast.bodyDistance, blast.distance, blast.grade,
						blast.damage, applies, before, after, oursAfter, hasMsg ? msgIt->second.front() : -1.0);
			}
		}

		std::printf("GRENADECHECK tag=%s blast booms=%d lineCompared=%d lineFails=%d notTracedHurt=%d damage compared=%d fails=%d msgFails=%d kills=%d killDiff=%d\n",
			tag.c_str(), t.booms, t.lineCompared, t.lineFails, t.notTracedHurt, t.damageCompared, t.damageFails, t.msgFails, t.kills, t.killDiff);
		total.Add(t);
		return true;
	}
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::printf("사용: GrenadeCheck <map_gym.mocl> <기록 폴더> <태그>... [--verbose] [--pawn-row now|prev]\n");
		return 2;
	}
	std::vector<char> mapData;
	if (!ReadFile(argv[1], mapData))
	{
		std::printf("GRENADECHECK input 맵 파일 읽기 실패\n");
		return 2;
	}
	CMapCollision map;
	const EMapLoad load = map.LoadFromMemory(mapData.data(), mapData.size());
	if (load != EMapLoad::Ok)
	{
		std::printf("GRENADECHECK input 맵 불러오기 실패 code=%d\n", (int)load);
		return 2;
	}
	const std::string dir = argv[2];
	bool verbose = false, pawnRowNow = false;
	std::vector<std::string> tags;
	for (int i = 3; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
		else if (std::strcmp(argv[i], "--pawn-row") == 0 && i + 1 < argc) pawnRowNow = std::strcmp(argv[++i], "now") == 0;
		else tags.push_back(argv[i]);
	}

	Totals total;
	AbilityTotals ab;
	BlastTotals bl;
	for (const std::string& tag : tags)
	{
		if (!RunTag(map, dir, tag, verbose, pawnRowNow, total)) return 2;
		if (!RunAbilityTag(map, dir, tag, verbose, ab)) return 2;
		if (!RunBlastTag(map, dir, tag, verbose, bl)) return 2;
	}

	const bool pass = total.fails == 0 && total.early == 0 && total.late == 0 && total.kindDiff == 0 && total.hitFails == 0
		&& total.launchFails == 0 && total.ownerUnknown == 0 && total.grenades > 0 && total.frames > 0
		&& total.bounceFails == 0 && total.bounceFrameDiff == 0 && total.stopFrameDiff == 0 && total.detonateDiff == 0
		&& total.allHitFails == 0 && total.hitFrameDiff == 0 && total.hitCountDiff == 0 && total.bounceFrames > 0 && total.foeToggleWhileLive == 0
		&& total.fuseCompared > 0 && total.fuseDiff == 0
		&& total.maxRotUnexplained <= kRotTolerance	// 속도 차로 설명 안 되는 회전 차는 날아감 기준 그대로
		// 능력 · 던짐(G-2-a) — 경기 끝 갈래 다름(postGameDiff)은 미결 15 라 넣지 않는다. 사람 메시 겨눔(bodyMiss · 거리 차)은 부위 몸 몫이라 넣지 않는다
		&& ab.lyraThrown > 0 && ab.throwDiff == 0 && ab.cooldownDiff == 0 && ab.gabilDiff == 0 && ab.gapNotDead == 0
		&& ab.throwCompared == ab.lyraThrown && ab.throwFails == 0 && ab.branchDiff == 0 && ab.traceDiff == 0 && ab.endFails == 0
		// 폭발 · 피해(G-2-b)
		&& bl.damageCompared > 0 && bl.lineCompared > 0 && bl.lineFails == 0 && bl.notTracedHurt == 0 && bl.tracedNotAlive == 0
		&& bl.deadAfterNotKilled == 0 && bl.hhDiff == 0 && bl.rowMissing == 0 && bl.sameFrameBooms == 0 && bl.msgOtherInst == 0
		&& bl.damageFails == 0 && bl.msgFails == 0 && bl.msgMissing == 0 && bl.msgExtra == 0 && bl.killDiff == 0
		&& total.count.mapPenetrationsDropped == 0;	// 맵 시작 겹침을 물체 칸이 모자라 못 본 것(L-1-a) — 나면 아래 줄이 뜬다
	const ProjectileCounters& c = total.count;
	std::printf("GRENADECHECK flight grenades=%d frames=%d fails=%d maxPosDiff=%.6f maxVelDiff=%.6f maxRotDiff=%.6f firstHit same=%d early=%d late=%d kindDiff=%d none=%d\n",
		total.grenades, total.frames, total.fails, total.maxPos, total.maxVel, total.maxRot, total.same, total.early, total.late, total.kindDiff, total.noneBoth);
	std::printf("GRENADECHECK firstHit compared=%d fails=%d maxDistDiff=%.6f maxTimeDiff=%.7f maxNormalDiff=%.6f pawnMeshNotCompared=%d\n",
		total.hitCompared, total.hitFails, total.maxHitDist, total.maxHitTime, total.maxHitNormal, total.meshHits);
	std::printf("GRENADECHECK firstHit kinds world=%d me=%d foe=%d gren=%d\n", total.sameKind[0], total.sameKind[1], total.sameKind[2], total.sameKind[3]);
	std::printf("GRENADECHECK launch grenades=%d fails=%d maxVelDiff=%.6f ownerUnknown=%d (rows=%d spawns=%d hits=%d)\n",
		total.grenades, total.launchFails, total.maxLaunch, total.ownerUnknown, total.rowsRead, total.spawnsRead, total.hitsRead);
	std::printf("GRENADECHECK bounce grenades=%d frames=%d fails=%d maxPosDiff=%.6f maxVelDiff=%.6f maxRotDiff=%.6f (unexplained=%.6f) bounceFrameDiff=%d (rec=%d sim=%d) stopFrameDiff=%d (compared=%d) maxStopPosDiff=%.6f\n",
		total.bounceGrenades, total.bounceFrames, total.bounceFails, total.maxBouncePos, total.maxBounceVel, total.maxBounceRot, total.maxRotUnexplained,
		total.bounceFrameDiff, total.recBounces, total.simBounces, total.stopFrameDiff, total.stopsCompared, total.maxStopPos);
	std::printf("GRENADECHECK hits compared=%d fails=%d frameDiff=%d countDiff=%d maxDistDiff=%.6f maxTimeDiff=%.7f maxTimeCmDiff=%.6f maxNormalDiff=%.6f maxDepthDiff=%.6f pawnMeshNotCompared=%d passive=%d\n",
		total.allHitsCompared, total.allHitFails, total.hitFrameDiff, total.hitCountDiff, total.maxAllHitDist, total.maxAllHitTime, total.maxAllHitTimeCm,
		total.maxAllHitNormal, total.maxAllHitDepth, total.meshFrames, total.passiveHits);
	std::printf("GRENADECHECK detonate same=%d diff=%d\n", total.detonateSame, total.detonateDiff);
	std::printf("GRENADECHECK fuse compared=%d diff=%d (frames=%d)\n", total.fuseCompared, total.fuseDiff, kGrenadeFuseFrames);
	std::printf("GRENADECHECK script foeInactiveGrenades=%d foeToggleWhileLive=%d wallMoves=%d\n",
		total.foeInactiveGrenades, total.foeToggleWhileLive, total.wallMoves);
	std::printf("GRENADECHECK branches steps=%u blocked=%u extraIterations=%u startPenetrating=%u resolveMoved=%u resolveCombined=%u resolveWithMove=%u resolveFailed=%u "
		"ignoredOverlaps=%u minMoveZeroed=%u bounces=%u stopsOnImpact=%u stopsOnSlide=%u multiHits=%u slides=%u corners=%u slideMoves=%u slideHits=%u slideFriction=%u detonations=%u\n",
		c.steps, c.blockedSteps, c.extraIterations, c.startPenetrating, c.resolveMoved, c.resolveCombined, c.resolveWithMove, c.resolveFailed,
		c.ignoredOverlaps, c.minMoveZeroed, c.bounces, c.stopsOnImpact, c.stopsOnSlide, c.multiHits, c.slides, c.corners, c.slideMoves, c.slideHits,
		c.slideFriction, c.detonations);
	if (c.mapPenetrationsDropped > 0) std::printf("GRENADECHECK branches mapPenetrationsDropped=%u\n", c.mapPenetrationsDropped);
	std::printf("GRENADECHECK ability inputs=%d presses=%d thrown=%d lyraThrown=%d onCooldown=%d whileDash=%d dead=%d postGame=%d throwDiff=%d cooldownDiff=%d postGameDiff=%d gabil=%d gabilDiff=%d rowGaps=%d gapFrames=%d gapNotDead=%d cdClears=%d\n",
		ab.inputs, ab.presses, ab.results[(int)EGrenadeResult::Thrown], ab.lyraThrown, ab.results[(int)EGrenadeResult::OnCooldown],
		ab.results[(int)EGrenadeResult::WhileDash], ab.results[(int)EGrenadeResult::Dead], ab.results[(int)EGrenadeResult::PostGame],
		ab.throwDiff, ab.cooldownDiff, ab.postGameDiff, ab.gabilCompared, ab.gabilDiff, ab.rowGaps, ab.gapFrames, ab.gapNotDead, ab.cdClears);
	std::printf("GRENADECHECK throw compared=%d fails=%d maxPosDiff=%.6f maxRotDiff=%.6f maxVelDiff=%.6f ctl=%d hit=%d end=%d branchDiff=%d\n",
		ab.throwCompared, ab.throwFails, ab.maxThrowPos, ab.maxThrowRot, ab.maxThrowVel, ab.branches[0], ab.branches[1], ab.branches[2], ab.branchDiff);
	std::printf("GRENADECHECK aim compared=%d none=%d mapHits=%d traceDiff=%d maxMapDistDiff=%.6f endFails=%d maxEndRotDiff=%.6f maxEndVelDiff=%.6f startInside=%d startInsideDiff=%d simpleSame=%d bodyCompared=%d bodyHits=%d bodyMiss=%d maxBodyDistDiff=%.4f maxBodyRotDiff=%.4f crouchedFoe=%d\n",
		ab.aimCompared, ab.aimNone, ab.aimMap, ab.traceDiff, ab.maxAimMapDist, ab.endFails, ab.maxEndRot, ab.maxEndVel, ab.startInside, ab.startInsideDiff, ab.simpleSame, ab.bodyCompared, ab.aimBody,
		ab.bodyMiss, ab.maxBodyDist, ab.maxBodyRot, ab.crouchedFoe);
	std::printf("GRENADECHECK blast booms=%d targets=%d alive=%d dead=%d foeHidden=%d traced=%d lineCompared=%d lineFails=%d body=%d map=%d maxLineDistDiff=%.6f farMap=%d deadAfter=%d deadAfterNotKilled=%d notTraced=%d notTracedNear=%d notTracedHurt=%d tracedNotAlive=%d crouched=%d hhDiff=%d rowMissing=%d sameFrameBooms=%d\n",
		bl.booms, bl.targets, bl.alive, bl.dead, bl.foeHidden, bl.traced, bl.lineCompared, bl.lineFails, bl.bodyLines, bl.mapLines, bl.maxLineDist, bl.farMap,
		bl.deadAfter, bl.deadAfterNotKilled, bl.notTraced, bl.notTracedNear, bl.notTracedHurt, bl.tracedNotAlive, bl.crouched, bl.hhDiff, bl.rowMissing, bl.sameFrameBooms);
	std::printf("GRENADECHECK damage compared=%d fails=%d maxDamageDiff=%.6f msgCompared=%d msgFails=%d maxMsgDiff=%.6f msgMissing=%d msgExtra=%d msgOtherInst=%d hurt=%d kills=%d killDiff=%d sameTeam=%d immune=%d\n",
		bl.damageCompared, bl.damageFails, bl.maxDamage, bl.msgCompared, bl.msgFails, bl.maxMsg, bl.msgMissing, bl.msgExtra, bl.msgOtherInst,
		bl.hurt, bl.kills, bl.killDiff, bl.sameTeam, bl.immune);
	std::printf("GRENADECHECK %s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
