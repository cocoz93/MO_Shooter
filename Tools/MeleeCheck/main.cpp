// 근접 대조 도구 — Lyra 연습장 기록과 Shared ResolveMelee 를 맞춰 본다(컨텐츠 E-1).
//
//   MeleeCheck <cprobe_<tag>.log> <cprobe_<tag>.csv> <clips.bin> <map_gym.mocl>
//
// 기록 = Tools/LyraPilot/record_content_probe.ps1 -Mode gym. 근접 능력이 켜질 때마다 기록 도구(MOBContentProbeSubsystem)가 남기는 줄:
//   MOB_CPROBE_MTRACE which=prev|now   손(앞 프레임 끝 · 이 프레임 끝) · 몸 앞 · 상대 캡슐, 그리고 그 손으로 기록 도구가 직접 한 쓸기 결과(이 프레임 몸)
//   MOB_CPROBE_MPART  which=prev|now   상대 메시 몸체(물리 몸 요소마다, 상대 몸 기준)
// 셋을 견준다
//   능력       = 앞 프레임 손 + 앞 프레임 몸 ↔ 능력이 실제로 낸 결과(피해 줄 MOB_CPROBE_MELEE, 맞은 거리 = 다음 프레임 밀기 속도 × 350 ÷ 2200)
//   재시험 앞손 = 앞 프레임 손 + 이 프레임 몸 ↔ 기록 도구 쓸기 prev 줄
//   재시험 이손 = 이 프레임 손 + 이 프레임 몸 ↔ 기록 도구 쓸기 now 줄
// 그리고 손 계산(CAnimPoseEvaluator::HandRightFromLocal)의 자기 일관 — 손 뼈에서 손 부위 중심까지 거리 = 그 부위의 뼈 기준 오프셋 길이
// 밀기(컨텐츠 E-2-1) — 능력 대조가 맞은 근접마다, 능력 프레임 앞 줄의 자리 · 속도에서 내 맞은 거리로 밀기를 넣고 Shared 이동을 프레임마다 돌려
//   기록 표의 공격자 자리 · 수평 속도와 견준다. 멈출 때까지. 상대 캡슐에 닿은 프레임부터는 안 견준다(플레이어끼리 부딪힘은 컨텐츠 F)
// 예측 다시 돌리기(컨텐츠 E-2-1) — 내 예측기(CSelfPrediction)에 밀기 붙은 입력을 넣고, 밀기 앞 · 중간 · 뒤 번호의 서버 값으로 되돌려 다시 돌려도
//   끝 자리가 처음과 같은지(와이어 float 반올림만큼). 밀기 앞 번호에서 되돌리면 다시 돌리기가 밀기를 다시 넣어야 같아진다
// 근접 연결(컨텐츠 E-3)
//   능력 간격 = 연타 칸(spam_<무기>)의 켜지는 프레임 · 켜짐 칸(meleeActive) ↔ CMeleeAbility(입력 개수로 센 간격)
//   막기 규칙 = 근접 중 발사 · 재장전 입력 · 자동 재장전 막힘, 켜지는 입력의 재장전 끊기(CWeaponCycle). 규칙마다 근접 없는 대조를 같이 본다
//   결과로 고치기 = 서버 근접 결과(OnMeleeResult) · 결과 없이 지나감(AckMelee)으로 고친 예측의 끝 자리 ↔ 서버 판정대로 처음부터 돈 판
// 끝 코드: 0 = 전부 통과, 1 = 불일치, 2 = 입력 오류
#include "AnimClips.h"
#include "AnimPose.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Prediction.h"
#include "Weapon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace mo;

namespace
{
	// 허용 차 — 엔진 쓸기와 해석값의 차는 원통에서 0.002cm 안(e1g1). 몸체(둥근 상자 · 캡슐)는 엔진이 여유를 두고 잴 수 있어 넉넉히(제안값)
	constexpr double kDistTolerance = 0.05;
	constexpr double kImpactTolerance = 0.1;
	constexpr double kSelfTolerance = 1e-6;
	constexpr double kPushStrength = kMeleePushStrength;	// GA_Melee Strength — 밀기 속도 = 이 값 × 맞은 거리 ÷ 350
	// 밀기 — 기록 표가 소수 셋째 자리라 반올림 0.0005 를 넉넉히 덮는 값(제안값). 걸음 대조(MoveCheck)도 이 자릿수다
	constexpr double kPushPosTolerance = 0.01;
	constexpr double kPushVelTolerance = 0.01;
	constexpr double kContactSlack = 1.0;		// 공격자 ↔ 상대 수평 중심 거리가 두 반지름 + 이 값 안이면 닿았다고 본다(닿은 4번은 70.1, 안 닿은 것 중 가장 가까운 것 90.9 — e1g3)
	constexpr int kPushMaxFrames = 120;
	constexpr float kFrameSeconds = 1.f / 60.f;	// Lyra 기록 프레임(t 칸 0.0167 간격)
	// 예측 다시 돌리기 — 와이어 자리 · 속도가 float 라 되돌린 자리가 그만큼 어긋난다(캡슐 중심 수천 cm 의 float 한 칸 ≈ 0.0001). 넉넉히(제안값)
	constexpr double kReplayTolerance = 0.01;
	constexpr uint32_t kReplayInputs = 40;
	constexpr uint32_t kReplayPushSeq = 10;

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

	bool ParseVec(const std::string& text, double out[3])
	{
		return std::sscanf(text.c_str(), "%lf,%lf,%lf", &out[0], &out[1], &out[2]) == 3;
	}

	Vec3 ToVec(const double v[3]) { return Vec3(v[0], v[1], v[2]); }
	double Dist(const Vec3& a, const Vec3& b) { const Vec3 d = a - b; return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); }

	struct Trace
	{
		bool ok = false;
		double hand[3] = {}, fwd[3] = {}, foe[3] = {}, impact[3] = {};
		double foeR = 0, foeHH = 0, dist = 0;
		bool hit = false, cylinder = false, startPen = false;
		int wall = -1;
	};

	struct Part
	{
		std::string bone;
		double p[3], x[3], y[3], z[3];
	};

	struct PartSet
	{
		bool ok = false;
		double at[3] = {};
		double yaw = 0;
		std::vector<Part> parts;
	};

	struct Event
	{
		std::string step;
		bool sameTeam = false;
		Trace prev, now;
		PartSet prevParts, nowParts;
		bool hasSpeed = false;
		double speedAfter = 0;
		size_t row = 0;			// 기록 표에서 능력이 켜진 줄(ev 에 melee_start) — 그 앞 줄이 밀기 시작 상태
	};

	bool ParseTrace(const std::map<std::string, std::string>& kv, Trace& out)
	{
		const auto get = [&](const char* key) -> const std::string* { const auto it = kv.find(key); return it == kv.end() ? nullptr : &it->second; };
		const std::string* hand = get("hand"); const std::string* fwd = get("fwd"); const std::string* foe = get("foe"); const std::string* impact = get("impact");
		const std::string* hit = get("hit"); const std::string* comp = get("comp"); const std::string* dist = get("dist"); const std::string* pen = get("startPen");
		const std::string* wall = get("wall"); const std::string* foeR = get("foeR"); const std::string* foeHH = get("foeHH");
		if (!hand || !fwd || !foe || !impact || !hit || !comp || !dist || !pen || !wall || !foeR || !foeHH) return false;
		if (!ParseVec(*hand, out.hand) || !ParseVec(*fwd, out.fwd) || !ParseVec(*foe, out.foe) || !ParseVec(*impact, out.impact)) return false;
		out.hit = *hit == "1";
		out.cylinder = *comp == "CollisionCylinder";
		out.dist = std::atof(dist->c_str());
		out.startPen = *pen == "1";
		out.wall = std::atoi(wall->c_str());
		out.foeR = std::atof(foeR->c_str());
		out.foeHH = std::atof(foeHH->c_str());
		out.ok = true;
		return true;
	}

	bool ParsePart(const std::map<std::string, std::string>& kv, PartSet& set)
	{
		const auto at = kv.find("at"); const auto yaw = kv.find("yaw"); const auto bone = kv.find("bone");
		const auto p = kv.find("p"); const auto x = kv.find("x"); const auto y = kv.find("y"); const auto z = kv.find("z");
		if (at == kv.end() || yaw == kv.end() || bone == kv.end() || p == kv.end() || x == kv.end() || y == kv.end() || z == kv.end()) return false;
		Part part;
		part.bone = bone->second;
		if (!ParseVec(p->second, part.p) || !ParseVec(x->second, part.x) || !ParseVec(y->second, part.y) || !ParseVec(z->second, part.z)) return false;
		if (!set.ok && !ParseVec(at->second, set.at)) return false;
		set.yaw = std::atof(yaw->second.c_str());
		set.ok = true;
		set.parts.push_back(part);
		return true;
	}

	// 기록 몸체 → 클립 몸체 차례의 자세. 같은 뼈에 요소가 여럿이면 나온 차례대로(기록 도구 · 클립 둘 다 구 → 캡슐 → 상자 차례)
	bool BuildPose(const CAnimClips& clips, const PartSet& set, std::vector<BodyPose>& out)
	{
		const std::vector<ClipBody>& bodies = clips.Bodies();
		const std::vector<ClipBone>& bones = clips.Bones();
		out.assign(bodies.size(), BodyPose());
		std::map<std::string, int> used;
		for (size_t i = 0; i < bodies.size(); ++i)
		{
			const std::string& name = bones[(size_t)bodies[i].bone].name;
			int skip = used[name]++;
			const Part* found = nullptr;
			for (const Part& part : set.parts)
			{
				if (part.bone != name) continue;
				if (skip-- == 0) { found = &part; break; }
			}
			if (!found) return false;
			for (int c = 0; c < 3; ++c)
			{
				out[i].center[c] = found->p[c];
				out[i].axisX[c] = found->x[c];
				out[i].axisY[c] = found->y[c];
				out[i].axisZ[c] = found->z[c];
			}
		}
		return set.parts.size() == bodies.size();
	}

	void Judge(const CMapCollision& map, const CAnimClips& clips, const std::vector<BodyPose>& pose, const PartSet& set, const Trace& capsule, bool sameTeam,
		const double hand[3], const double fwd[3], MeleeResult& out)
	{
		MeleeTarget target;
		target.body.center = ToVec(set.at);
		target.body.yawDegrees = set.yaw;
		target.body.pose = pose.data();
		target.body.shapes = clips.Bodies().data();
		target.body.shapeCount = (uint32_t)clips.Bodies().size();
		target.capsuleRadius = capsule.foeR;
		target.capsuleHalfHeight = capsule.foeHH;
		target.sameTeam = sameTeam;
		ResolveMelee(map, &target, 1, ToVec(hand), ToVec(fwd), out);
	}

	struct Tally
	{
		int compared = 0;
		int fails = 0;
		double maxDist = 0;
		double maxImpact = 0;
	};

	// 기록 도구 쓸기 한 줄과 대조 — 팀은 안 본다(쓸기 단계까지)
	void CompareTrace(const char* label, const Event& e, const Trace& trace, const MeleeResult& mine, Tally& tally)
	{
		++tally.compared;
		const bool found = mine.kind != EMeleeHit::None;
		bool ok = found == trace.hit;
		double distDiff = 0, impactDiff = 0;
		if (ok && found)
		{
			distDiff = std::fabs(mine.distance - trace.dist);
			if (distDiff > kDistTolerance) ok = false;
			if (!trace.startPen)
			{
				impactDiff = Dist(mine.impactPoint, ToVec(trace.impact));
				if (impactDiff > kImpactTolerance) ok = false;
				if (mine.capsule != trace.cylinder) ok = false;
			}
			if ((mine.kind == EMeleeHit::Blocked) != (trace.wall == 1)) ok = false;
			tally.maxDist = std::max(tally.maxDist, distDiff);
			tally.maxImpact = std::max(tally.maxImpact, impactDiff);
		}
		if (!ok)
		{
			++tally.fails;
			std::printf("DIFF %s step=%s lyra(hit=%d cyl=%d dist=%.4f wall=%d pen=%d) mine(kind=%d capsule=%d dist=%.4f) distDiff=%.4f impactDiff=%.4f\n", label, e.step.c_str(),
				trace.hit ? 1 : 0, trace.cylinder ? 1 : 0, trace.dist, trace.wall, trace.startPen ? 1 : 0, (int)mine.kind, mine.capsule ? 1 : 0, mine.distance, distDiff, impactDiff);
		}
	}
}

int main(int argc, char** argv)
{
	if (argc < 5)
	{
		std::printf("사용: MeleeCheck <cprobe.log> <cprobe.csv> <clips.bin> <map_gym.mocl>\n");
		return 2;
	}

	std::vector<char> data;
	CAnimClips clips;
	if (!ReadFile(argv[3], data) || clips.LoadFromMemory(data.data(), data.size()) != EClipLoad::Ok)
	{
		std::printf("MELEECHECK FAIL 클립 파일을 못 읽음: %s\n", argv[3]);
		return 2;
	}
	std::vector<char> mapData;
	CMapCollision map;
	if (!ReadFile(argv[4], mapData) || map.LoadFromMemory(mapData.data(), mapData.size()) != EMapLoad::Ok)
	{
		std::printf("MELEECHECK FAIL 맵을 못 읽음: %s\n", argv[4]);
		return 2;
	}

	// ── 로그 — 근접 한 번 = prev 쓸기 줄로 시작하는 한 덩어리
	std::ifstream log(argv[1]);
	if (!log)
	{
		std::printf("MELEECHECK FAIL 로그를 못 엶: %s\n", argv[1]);
		return 2;
	}
	std::vector<Event> events;
	std::map<std::string, bool> meleeHit;		// 칸 이름 → 능력이 피해를 줬나(MOB_CPROBE_MELEE, 한 번 누르는 칸만)
	bool sameTeam = false;
	int badLines = 0;
	std::string line;
	while (std::getline(log, line))
	{
		const auto after = [&](const char* marker, std::string& rest) { const size_t at = line.find(marker); if (at == std::string::npos) return false; rest = line.substr(at + std::strlen(marker)); return true; };
		std::string rest;
		if (after("MOB_CPROBE_FOE_SAME ", rest)) sameTeam = true;
		else if (after("MOB_CPROBE_FOE ", rest)) sameTeam = false;
		else if (after("MOB_CPROBE_MELEE ", rest))
		{
			const auto kv = KeyValues(rest);
			const auto step = kv.find("step");
			const auto hit = kv.find("hit");
			if (step == kv.end() || hit == kv.end()) ++badLines;
			else meleeHit[step->second] = hit->second == "1";
		}
		else if (after("MOB_CPROBE_MTRACE ", rest))
		{
			const auto kv = KeyValues(rest);
			const auto which = kv.find("which");
			const auto step = kv.find("step");
			if (which == kv.end() || step == kv.end()) { ++badLines; continue; }
			if (which->second == "prev")
			{
				Event e;
				e.step = step->second;
				e.sameTeam = sameTeam;
				if (!ParseTrace(kv, e.prev)) ++badLines;
				events.push_back(e);
			}
			else if (!events.empty() && events.back().step == step->second && !events.back().now.ok)
			{
				if (!ParseTrace(kv, events.back().now)) ++badLines;
			}
			else ++badLines;
		}
		else if (after("MOB_CPROBE_MPART ", rest))
		{
			const auto kv = KeyValues(rest);
			const auto which = kv.find("which");
			if (events.empty() || which == kv.end()) { ++badLines; continue; }
			PartSet& set = which->second == "prev" ? events.back().prevParts : events.back().nowParts;
			if (!ParsePart(kv, set)) ++badLines;
		}
	}

	// ── 기록 표 — 능력이 켜진 프레임(ev 에 melee_start) 다음 프레임의 수평 속도 = 밀기 속도
	std::ifstream csv(argv[2]);
	if (!csv || !std::getline(csv, line))
	{
		std::printf("MELEECHECK FAIL 기록 표를 못 엶: %s\n", argv[2]);
		return 2;
	}
	std::vector<std::string> header;
	{
		std::istringstream in(line);
		std::string cell;
		while (std::getline(in, cell, ',')) header.push_back(cell);
	}
	const auto column = [&](const char* name) { const auto it = std::find(header.begin(), header.end(), std::string(name)); return it == header.end() ? -1 : (int)(it - header.begin()); };
	const int colStep = column("step"), colEv = column("ev"), colSpeed = column("speed2d");
	if (colStep < 0 || colEv < 0 || colSpeed < 0)
	{
		std::printf("MELEECHECK FAIL 기록 표 머리에 step · ev · speed2d 가 없음\n");
		return 2;
	}
	const int colPx = column("px"), colPy = column("py"), colPz = column("pz"), colVx = column("vx"), colVy = column("vy"), colVz = column("vz");
	const int colCrouched = column("crouched"), colFoeX = column("foeX"), colFoeY = column("foeY");
	// 17-d 칸 — walk = 그 줄 프레임에 +X 이동 입력을 넣었나(다음 줄 이동이 먹는다), mode = 언리얼 이동 모드(1 걷기 · 3 낙하). 옛 기록엔 없다(-1)
	const int colWalk = column("walk"), colMode = column("mode");
	constexpr double kUnrealModeFalling = 3.0;		// EMovementMode::MOVE_Falling
	if (colPx < 0 || colPy < 0 || colPz < 0 || colVx < 0 || colVy < 0 || colVz < 0 || colCrouched < 0 || colFoeX < 0 || colFoeY < 0)
	{
		std::printf("MELEECHECK FAIL 기록 표 머리에 밀기 대조 칸(px · py · pz · vx · vy · vz · crouched · foeX · foeY)이 없음\n");
		return 2;
	}
	std::vector<std::vector<std::string>> rows;
	while (std::getline(csv, line))
	{
		std::vector<std::string> cells;
		std::istringstream in(line);
		std::string cell;
		while (std::getline(in, cell, ',')) cells.push_back(cell);
		rows.push_back(cells);
	}
	size_t starts = 0;
	for (size_t r = 0; r + 1 < rows.size(); ++r)
	{
		if ((int)rows[r].size() <= colEv || rows[r][(size_t)colEv].find("melee_start;") == std::string::npos) continue;
		if (starts < events.size() && rows[r][(size_t)colStep] == events[starts].step && (int)rows[r + 1].size() > colSpeed)
		{
			events[starts].hasSpeed = true;
			events[starts].speedAfter = std::atof(rows[r + 1][(size_t)colSpeed].c_str());
			events[starts].row = r;
		}
		++starts;
	}

	if (events.empty() || starts != events.size() || badLines != 0)
	{
		std::printf("MELEECHECK FAIL 입력 — 근접 %zu번(로그) · %zu번(표) · 못 읽은 줄 %d\n", events.size(), starts, badLines);
		return 2;
	}

	// ── 대조
	Tally ability, tracePrev, traceNow;
	const MoveParams moveParams = MakeLyraMoveParams();
	CMoveSim moveSim(map, moveParams);
	int pushCompared = 0, pushFails = 0, pushFrames = 0, pushBlocked = 0;
	int abilityWalkCells = 0;		// 걸으며 칸 — 밀기 몫 속도를 걷던 속도를 빼고 구한 칸
	int pushWalkCells = 0, pushFallCells = 0, pushInputFrames = 0;		// 걸으며 · 낙하로 시작한 칸, 입력을 넣은 프레임 — 0 이면 그 갈래를 안 지나간 것
	double pushMaxPos = 0, pushMaxVel = 0;
	const auto cellOf = [&](const std::vector<std::string>& cells, int col) { return (int)cells.size() > col ? std::atof(cells[(size_t)col].c_str()) : 0.0; };
	int abilityDamage = 0, abilitySameTeam = 0, abilityNone = 0, capsuleFirst = 0, meshFirst = 0;
	int inputFails = 0;
	for (const Event& e : events)
	{
		std::vector<BodyPose> prevPose, nowPose;
		if (!e.prev.ok || !e.now.ok || !e.hasSpeed || !BuildPose(clips, e.prevParts, prevPose) || !BuildPose(clips, e.nowParts, nowPose))
		{
			++inputFails;
			std::printf("INPUT step=%s prev=%d now=%d speed=%d prevParts=%zu nowParts=%zu\n", e.step.c_str(), e.prev.ok ? 1 : 0, e.now.ok ? 1 : 0, e.hasSpeed ? 1 : 0, e.prevParts.parts.size(), e.nowParts.parts.size());
			continue;
		}

		// 능력 — 앞 프레임 손 · 앞 프레임 몸. 캡슐 자리도 앞 프레임
		Trace prevCapsule = e.now;		// 캡슐 크기만 쓴다(자리는 PartSet.at)
		MeleeResult mine;
		Judge(map, clips, prevPose, e.prevParts, prevCapsule, e.sameTeam, e.prev.hand, e.prev.fwd, mine);
		const auto hitIt = meleeHit.find(e.step);
		const bool lyraDamage = hitIt != meleeHit.end() ? hitIt->second : e.speedAfter > 0.0;
		// Lyra 맞은 거리 = 밀기 몫 속도 × 350 ÷ 2200. 걸으며 칸은 밀기 힘이 걷던 속도에 더해지므로(덧붙이기) 힘 없이 같은 입력으로
		// 두 프레임(능력 줄 · 다음 줄) 굴린 속도를 뺀다(17-d — melee_walk_300 873.296 = 400 + 473.296). 입력이 없으면 지금까지와 같은 식
		double lyraPushSpeed = e.speedAfter;
		if (colWalk >= 0 && e.row > 0 && e.row + 1 < rows.size() && cellOf(rows[e.row - 1], colWalk) != 0.0)
		{
			const std::vector<std::string>& before = rows[e.row - 1];
			MoveState np;
			np.location = Vec3(cellOf(before, colPx), cellOf(before, colPy), cellOf(before, colPz));
			np.velocity = Vec3(cellOf(before, colVx), cellOf(before, colVy), cellOf(before, colVz));
			np.lastUpdateVelocity = np.velocity;
			np.mode = colMode >= 0 && cellOf(before, colMode) == kUnrealModeFalling ? EMoveMode::Falling : EMoveMode::Walking;
			np.crouched = np.wantsToCrouch = cellOf(before, colCrouched) != 0.0;
			np.maxSpeed = moveParams.maxWalkSpeed;
			moveSim.RefreshFloor(np);
			for (size_t k = 0; k < 2; ++k) moveSim.Tick(np, cellOf(rows[e.row - 1 + k], colWalk) != 0.0 ? Vec3(1, 0, 0) : Vec3(), kFrameSeconds);
			const std::vector<std::string>& after = rows[e.row + 1];
			lyraPushSpeed = std::hypot(cellOf(after, colVx) - np.velocity.x, cellOf(after, colVy) - np.velocity.y);
			++abilityWalkCells;
		}
		const double lyraDist = lyraPushSpeed * kMeleeDistance / kPushStrength;
		++ability.compared;
		bool ok = (mine.kind == EMeleeHit::Hit) == lyraDamage;
		if (e.sameTeam && mine.kind != EMeleeHit::SameTeam) ok = false;		// 같은 팀 칸은 쓸기에 닿고도 피해가 없어야 한다
		double distDiff = 0;
		if (ok && lyraDamage)
		{
			distDiff = std::fabs(mine.distance - lyraDist);
			ability.maxDist = std::max(ability.maxDist, distDiff);
			if (distDiff > kDistTolerance) ok = false;
		}
		if (lyraDamage) ++abilityDamage;
		else if (e.sameTeam) ++abilitySameTeam;
		else ++abilityNone;
		if (mine.kind != EMeleeHit::None) (mine.capsule ? capsuleFirst : meshFirst)++;
		if (!ok)
		{
			++ability.fails;
			std::printf("DIFF ability step=%s lyra(damage=%d dist=%.4f speed=%.3f same=%d) mine(kind=%d capsule=%d dist=%.4f) distDiff=%.4f\n", e.step.c_str(),
				lyraDamage ? 1 : 0, lyraDist, e.speedAfter, e.sameTeam ? 1 : 0, (int)mine.kind, mine.capsule ? 1 : 0, mine.distance, distDiff);
		}

		// 밀기 — 능력 프레임 앞 줄 = 밀기 직전 상태. 공격자 입력 = 앞 줄 walk 칸(걸으며 칸), 앞 줄이 낙하면 낙하로 시작(뛰며 칸, 17-d)
		if (ok && lyraDamage && mine.kind == EMeleeHit::Hit && e.row > 0)
		{
			const std::vector<std::string>& before = rows[e.row - 1];
			MoveState s;
			s.location = Vec3(cellOf(before, colPx), cellOf(before, colPy), cellOf(before, colPz));
			s.velocity = Vec3(cellOf(before, colVx), cellOf(before, colVy), cellOf(before, colVz));
			s.lastUpdateVelocity = s.velocity;
			const bool fallStart = colMode >= 0 && cellOf(before, colMode) == kUnrealModeFalling;
			s.mode = fallStart ? EMoveMode::Falling : EMoveMode::Walking;
			s.crouched = s.wantsToCrouch = cellOf(before, colCrouched) != 0.0;
			s.maxSpeed = moveParams.maxWalkSpeed;
			moveSim.RefreshFloor(s);
			const Vec3 force = MeleePushForce(ToVec(e.prev.fwd), mine.distance);
			moveSim.ApplyRootForce(s, force, kMeleePushDuration);
			const Vec3 start = s.location;
			Vec3 lyraLast = start;
			int frames = 0;
			bool blocked = false, fail = false;
			double maxPos = 0, maxVel = 0;
			int inputFrames = 0;
			for (size_t r = e.row; r < rows.size() && (int)rows[r].size() > colStep && rows[r][(size_t)colStep] == e.step && frames < kPushMaxFrames; ++r)
			{
				const Vec3 lyraPos(cellOf(rows[r], colPx), cellOf(rows[r], colPy), cellOf(rows[r], colPz));
				const double foeGap = std::hypot(lyraPos.x - cellOf(rows[r], colFoeX), lyraPos.y - cellOf(rows[r], colFoeY));
				if (foeGap <= e.now.foeR + moveParams.capsuleRadius + kContactSlack)
				{
					blocked = true;
					break;
				}
				const bool held = colWalk >= 0 && cellOf(rows[r - 1], colWalk) != 0.0;		// r ≥ e.row > 0
				if (held) ++inputFrames;
				moveSim.Tick(s, held ? Vec3(1, 0, 0) : Vec3(), kFrameSeconds);
				// 재생엔 상대 캡슐이 없다 — 이 이동이 상대 캡슐 안으로 들어가면 Lyra 는 이 프레임에 막혔다. 간격만으로는 자유 프레임(e1g3 melee_480 71.193)과
				// 막힌 프레임(d1q melee_520 71.010)이 겹쳐 위 줄 검사만으로는 못 가른다(17-d)
				if (std::hypot(s.location.x - cellOf(rows[r], colFoeX), s.location.y - cellOf(rows[r], colFoeY)) <= e.now.foeR + moveParams.capsuleRadius + kContactSlack)
				{
					blocked = true;
					break;
				}
				const double posDiff = Dist(s.location, lyraPos);
				const double velDiff = std::hypot(s.velocity.x - cellOf(rows[r], colVx), s.velocity.y - cellOf(rows[r], colVy));
				maxPos = std::max(maxPos, posDiff);
				maxVel = std::max(maxVel, velDiff);
				if (posDiff > kPushPosTolerance || velDiff > kPushVelTolerance) fail = true;
				lyraLast = lyraPos;
				++frames;
				if (cellOf(rows[r], colSpeed) == 0.0) break;	// 멈춘 프레임까지
			}
			if (frames == 0) fail = true;
			++pushCompared;
			if (fail) ++pushFails;
			if (blocked) ++pushBlocked;
			if (inputFrames > 0) ++pushWalkCells;
			if (fallStart) ++pushFallCells;
			pushInputFrames += inputFrames;
			pushFrames += frames;
			pushMaxPos = std::max(pushMaxPos, maxPos);
			pushMaxVel = std::max(pushMaxVel, maxVel);
			std::printf("PUSH %s step=%s dist=%.4f force=%.3f frames=%d input=%d fall=%d blocked=%d maxPosDiff=%.4f maxVelDiff=%.4f slideLyra=%.2f slideMine=%.2f\n", fail ? "DIFF" : "ok", e.step.c_str(),
				mine.distance, std::hypot(force.x, force.y), frames, inputFrames, fallStart ? 1 : 0, blocked ? 1 : 0, maxPos, maxVel, std::hypot(lyraLast.x - start.x, lyraLast.y - start.y), std::hypot(s.location.x - start.x, s.location.y - start.y));
		}

		// 재시험 — 기록 도구가 이 프레임 몸으로 한 쓸기
		MeleeResult withPrevHand, withNowHand;
		Judge(map, clips, nowPose, e.nowParts, e.now, false, e.prev.hand, e.prev.fwd, withPrevHand);
		CompareTrace("tracePrev", e, e.prev, withPrevHand, tracePrev);
		Judge(map, clips, nowPose, e.nowParts, e.now, false, e.now.hand, e.now.fwd, withNowHand);
		CompareTrace("traceNow", e, e.now, withNowHand, traceNow);
	}

	// ── 손 계산 자기 일관 — 무기 셋 × 선 · 앉은 메시 올림
	int selfCompared = 0, selfFails = 0;
	int restFails = 0;		// 기본 자세 손 상수(kMeleeRestHandLocal · kMeleeRestHandCrouchLocal) ↔ 클립 계산값
	double selfMax = 0;
	const int handBone = clips.FindBone("hand_r");
	int handBody = -1;
	for (size_t i = 0; i < clips.Bodies().size(); ++i) if (clips.Bodies()[i].bone == handBone) handBody = (int)i;
	if (handBone < 0 || handBody < 0)
	{
		std::printf("MELEECHECK FAIL 클립에 hand_r 뼈 · 부위가 없음\n");
		return 2;
	}
	{
		const ClipBody& shape = clips.Bodies()[(size_t)handBody];
		const double offset = std::sqrt((double)shape.center[0] * shape.center[0] + (double)shape.center[1] * shape.center[1] + (double)shape.center[2] * shape.center[2]);
		for (int w = 0; w < (int)EWeapon::Count; ++w)
		{
			CAnimPoseEvaluator evaluator;
			if (!evaluator.Init(clips, GetWeaponDef((EWeapon)w).name))
			{
				std::printf("MELEECHECK FAIL 자세 묶음 %s 를 못 엶\n", GetWeaponDef((EWeapon)w).name);
				return 2;
			}
			std::vector<BoneTransform> local(evaluator.BoneCount());
			std::vector<BodyPose> bodies(evaluator.BodyCount());
			AnimPoseInput input;
			evaluator.EvaluateLocal(input, local.data());
			for (const double lift : { 0.0, 25.0 })
			{
				double hand[3];
				evaluator.BodiesFromLocal(local.data(), bodies.data(), lift);
				evaluator.HandRightFromLocal(local.data(), hand, lift);
				const double d = Dist(ToVec(bodies[(size_t)handBody].center), ToVec(hand));
				const double diff = std::fabs(d - offset);
				selfMax = std::max(selfMax, diff);
				++selfCompared;
				if (diff > kSelfTolerance) ++selfFails;
				std::printf("HAND weapon=%s lift=%.0f hand=%.3f,%.3f,%.3f partDist=%.6f offset=%.6f\n", GetWeaponDef((EWeapon)w).name, lift, hand[0], hand[1], hand[2], d, offset);
			}
			// 앉은 기본 자세(앉기 묶음 비중 1 · 앉음 · 메시 올림 0) — 자세가 없을 때 앉은 손(17-d d2). 권총 값은 상수와 견준다
			AnimPoseInput crouchInput;
			crouchInput.crouchBlend = 1.f;
			crouchInput.crouched = true;
			std::vector<BoneTransform> crouchLocal(evaluator.BoneCount());
			evaluator.EvaluateLocal(crouchInput, crouchLocal.data());
			double crouchHand[3];
			evaluator.HandRightFromLocal(crouchLocal.data(), crouchHand, 0.0);
			std::printf("HAND weapon=%s crouch=1 lift=0 hand=%.3f,%.3f,%.3f\n", GetWeaponDef((EWeapon)w).name, crouchHand[0], crouchHand[1], crouchHand[2]);
			if ((EWeapon)w == EWeapon::Pistol)
			{
				double standHand[3];
				evaluator.HandRightFromLocal(local.data(), standHand, 0.0);
				const double standDiff = Dist(ToVec(standHand), ToVec(kMeleeRestHandLocal));
				const double crouchDiff = Dist(ToVec(crouchHand), ToVec(kMeleeRestHandCrouchLocal));
				// 상수는 소수 셋째 자리로 적었다 — 0.001cm 반올림 셋이 모여도 0.0009 아래
				constexpr double kRestTolerance = 0.001;
				restFails += (standDiff > kRestTolerance ? 1 : 0) + (crouchDiff > kRestTolerance ? 1 : 0);
				std::printf("REST stand=%.6f crouch=%.6f\n", standDiff, crouchDiff);
			}
		}
	}

	// ── 예측 다시 돌리기 — 밀기 앞(9) · 넣은 입력(10) · 밀기 중(11 · 12) · 끝난 입력(13) · 한참 뒤(20)에서 서버 값으로 되돌린다
	int replayCompared = 0, replayFails = 0;
	int fixCompared = 0, fixFails = 0;
	double replayMax = 0;
	Vec3 replayForce, replayStart;
	for (const Event& e : events)
	{
		if (e.hasSpeed && e.speedAfter > 0.0 && e.row > 0)
		{
			replayForce = Vec3(e.prev.fwd[0], e.prev.fwd[1], e.prev.fwd[2]) * e.speedAfter;
			const std::vector<std::string>& before = rows[e.row - 1];
			replayStart = Vec3(cellOf(before, colPx), cellOf(before, colPy), cellOf(before, colPz));
			break;
		}
	}
	{
		MatchRules rules;
		CMoveSim replaySim(map, moveParams);		// 밀기 대조의 계수(pushCount)와 섞이지 않게 따로
		const auto runInputs = [&](CSelfPrediction& pred)
		{
			pred.OnSpawn(replayStart.x, replayStart.y, replayStart.z, 0, 0.f);		// 요 = 입력 요(0). 대시 단추가 없어 대시 「앞 입력」은 안 쓴다
			for (uint32_t i = 1; i <= kReplayInputs; ++i)
			{
				wire::Input input = wire::Input();
				if (i == kReplayPushSeq) input.buttons = wire::BUTTON_MELEE;		// 밀기는 근접이 켜진 입력에만 붙는다(E-3)
				pred.AddInput(input, i == kReplayPushSeq ? &replayForce : nullptr);
			}
		};
		// 예측기는 힙에 둔다 — 하나가 약 550KB(입력 고리 1024칸 약 508KB + 무기 쪽 고리 128칸 약 43KB, W-3)라 둘만 스택에 둬도 1MB 를 넘는다(0xC00000FD, 11-19 회귀)
		const std::unique_ptr<CSelfPrediction> straightOwner = std::make_unique<CSelfPrediction>(replaySim, rules, moveParams.maxWalkSpeed);
		CSelfPrediction& straight = *straightOwner;
		runInputs(straight);
		const Vec3 straightEnd = straight.GetMove().location;
		const double straightSlide = std::hypot(straightEnd.x - replayStart.x, straightEnd.y - replayStart.y);
		for (const uint32_t ack : { 9u, 10u, 11u, 12u, 13u, 20u })
		{
			const std::unique_ptr<CSelfPrediction> predOwner = std::make_unique<CSelfPrediction>(replaySim, rules, moveParams.maxWalkSpeed);
			CSelfPrediction& pred = *predOwner;
			runInputs(pred);
			wire::PlayerState server = wire::PlayerState();
			server.lastSeq = ack;
			{
				// 같은 번호의 슬롯 = 서버가 같은 입력을 돌린 값이라고 본다(서버도 같은 Shared 이동). 와이어처럼 float 로 줄인다
				wire::PlayerState probe = wire::PlayerState();
				probe.lastSeq = ack;
				const CSelfPrediction::Slot* slot = pred.FindComparable(probe);
				if (!slot) { ++replayFails; std::printf("REPLAY DIFF ack=%u 슬롯 없음\n", ack); continue; }
				server.x = (float)slot->move.location.x; server.y = (float)slot->move.location.y; server.z = (float)slot->move.location.z;
				server.vx = (float)slot->move.velocity.x; server.vy = (float)slot->move.velocity.y; server.vz = (float)slot->move.velocity.z;
				server.flags = slot->move.mode == EMoveMode::Falling ? wire::STATE_FALLING : 0;
			}
			// 서버가 그 입력을 돌렸으면 근접 결과가 상태보다 먼저 온다(E-3). 같은 힘이라 고칠 것이 없다 — 안 주면 AckMelee 가 밀기를 지운다
			if (ack >= kReplayPushSeq) pred.OnMeleeResult(kReplayPushSeq, true, replayForce);
			const CSelfPrediction::ReconcileResult rr = pred.Reconcile(server, -1.0);		// 허용 차 음수 = 늘 되돌린다
			const Vec3 end = pred.GetMove().location;
			const double diff = Dist(end, straightEnd);
			++replayCompared;
			replayMax = std::max(replayMax, diff);
			const CSelfPrediction::MeleeCounters& mc = pred.GetMeleeCounters();
			const bool fail = !rr.corrected || diff > kReplayTolerance || mc.unconfirmed != 0 || mc.pushHitFixes != 0 || mc.pushForceFixes != 0;
			if (fail) ++replayFails;
			std::printf("REPLAY %s ack=%u corrected=%d endDiff=%.6f slide=%.3f\n", fail ? "DIFF" : "ok", ack, rr.corrected ? 1 : 0, diff, std::hypot(end.x - replayStart.x, end.y - replayStart.y));
		}
		std::printf("MELEE predictReplay compared=%d fails=%d maxEndDiff=%.6f slideStraight=%.3f pushAt=%u inputs=%u pushApplied=%llu\n", replayCompared, replayFails, replayMax, straightSlide, kReplayPushSeq, kReplayInputs, (unsigned long long)replaySim.GetCounters().rootForceApplied);

		// ── 서버 근접 결과로 고치기(E-3) — 내 화면 판정과 서버 판정이 다를 때. 서버 결과(OnMeleeResult)나 결과 없이 지나감(AckMelee)으로 밀기를 고쳐
		//    다시 돌린 끝 자리가 서버 판정대로 처음부터 돈 판(기준)의 끝 자리와 같고, 기준 판 슬롯을 서버 상태로 줘도 되돌리지 않아야 한다
		const Vec3 halfForce = replayForce * 0.5;
		// press = 근접 단추, push = 넘기는 밀기(nullptr = 내 화면에선 빗나감)
		const auto runWith = [&](CSelfPrediction& pred, uint32_t meleeSeq, bool press, const Vec3* push)
		{
			pred.OnSpawn(replayStart.x, replayStart.y, replayStart.z, 0, 0.f);
			for (uint32_t i = 1; i <= kReplayInputs; ++i)
			{
				wire::Input input = wire::Input();
				if (i == meleeSeq && press) input.buttons = wire::BUTTON_MELEE;
				pred.AddInput(input, i == meleeSeq ? push : nullptr);
			}
		};
		enum class EServer { None, Miss, Push };
		const auto fixCase = [&](const char* name, uint32_t meleeSeq, bool press, const Vec3* predPush, EServer server, const Vec3* serverForce, uint32_t ack,
			uint64_t wantHitFixes, uint64_t wantForceFixes, uint64_t wantUnconfirmed, uint64_t wantIgnored)
		{
			// 예측기 하나가 고리 1024칸이라 크다 — 스택에 두면 넘친다(0xC00000FD, 위 「예측기는 힙에 둔다」)
			const std::unique_ptr<CSelfPrediction> refOwner = std::make_unique<CSelfPrediction>(replaySim, rules, moveParams.maxWalkSpeed);
			const std::unique_ptr<CSelfPrediction> predOwner = std::make_unique<CSelfPrediction>(replaySim, rules, moveParams.maxWalkSpeed);
			CSelfPrediction& ref = *refOwner;
			CSelfPrediction& pred = *predOwner;
			runWith(ref, meleeSeq, press, server == EServer::Push ? serverForce : nullptr);
			runWith(pred, meleeSeq, press, predPush);
			const Vec3 refEnd = ref.GetMove().location;
			const double before = Dist(pred.GetMove().location, refEnd);
			if (server != EServer::None) pred.OnMeleeResult(meleeSeq, server == EServer::Push, server == EServer::Push ? *serverForce : Vec3());
			const double afterResult = Dist(pred.GetMove().location, refEnd);
			wire::PlayerState probe = wire::PlayerState();
			probe.lastSeq = ack;
			const CSelfPrediction::Slot* slot = ref.FindComparable(probe);
			wire::PlayerState state = wire::PlayerState();
			state.lastSeq = ack;
			if (slot)
			{
				state.x = (float)slot->move.location.x; state.y = (float)slot->move.location.y; state.z = (float)slot->move.location.z;
				state.vx = (float)slot->move.velocity.x; state.vy = (float)slot->move.velocity.y; state.vz = (float)slot->move.velocity.z;
				state.flags = slot->move.mode == EMoveMode::Falling ? wire::STATE_FALLING : 0;
			}
			const CSelfPrediction::ReconcileResult rr = pred.Reconcile(state, kReplayTolerance);
			const double after = Dist(pred.GetMove().location, refEnd);
			const CSelfPrediction::MeleeCounters& mc = pred.GetMeleeCounters();
			const bool counters = mc.pushHitFixes == wantHitFixes && mc.pushForceFixes == wantForceFixes && mc.unconfirmed == wantUnconfirmed && mc.pushesIgnored == wantIgnored
				&& mc.startMismatches == 0 && mc.rerunsMissed == 0 && mc.resultsNoSlot == 0;
			const bool ok = slot && rr.compared && !rr.corrected && after <= kReplayTolerance && counters
				&& (wantIgnored != 0 || before > kReplayTolerance)						// 다르게 시작해야 고친 게 보인다 — 버린 밀기는 처음부터 같다
				&& (server == EServer::None || afterResult <= kReplayTolerance);		// 서버 결과로 고친 판은 상태를 맞춰 보기 전에 이미 같다
			++fixCompared;
			if (!ok) ++fixFails;
			std::printf("FIX %s %s seq=%u ack=%u before=%.4f afterResult=%.4f after=%.6f corrected=%d hitFixes=%llu forceFixes=%llu unconfirmed=%llu ignored=%llu rerunsMissed=%llu\n",
				ok ? "ok" : "DIFF", name, meleeSeq, ack, before, afterResult, after, rr.corrected ? 1 : 0, (unsigned long long)mc.pushHitFixes, (unsigned long long)mc.pushForceFixes,
				(unsigned long long)mc.unconfirmed, (unsigned long long)mc.pushesIgnored, (unsigned long long)mc.rerunsMissed);
		};
		fixCase("serverPush", kReplayPushSeq, true, nullptr, EServer::Push, &replayForce, 12, 1, 0, 0, 0);		// 내 화면은 빗나감, 서버는 맞음
		fixCase("serverMiss", kReplayPushSeq, true, &replayForce, EServer::Miss, nullptr, 12, 1, 0, 0, 0);		// 내 화면은 맞음, 서버는 빗나감
		fixCase("serverForce", kReplayPushSeq, true, &replayForce, EServer::Push, &halfForce, 12, 0, 1, 0, 0);	// 둘 다 맞음, 맞은 거리가 다름
		fixCase("unconfirmed", kReplayPushSeq, true, &replayForce, EServer::None, nullptr, 12, 0, 0, 1, 0);		// 서버는 안 켰다(결과 없이 지나감)
		fixCase("spawnBase", 1, true, &replayForce, EServer::Miss, nullptr, 3, 1, 0, 0, 0);						// 부활 뒤 첫 입력 — 앞 칸 = 부활 직후 상태
		fixCase("pushWithoutPress", kReplayPushSeq, false, &replayForce, EServer::None, nullptr, 12, 0, 0, 0, 1);	// 근접이 안 켜진 입력의 밀기는 버린다
		std::printf("MELEE predictFix compared=%d fails=%d\n", fixCompared, fixFails);
	}

	// ── 근접 능력 간격(E-3) — 연타 칸(spam_<무기>, 첫 켜짐부터 칸 끝까지 매 프레임 누름)에서 켜지는 프레임 · 켜짐 칸을 CMeleeAbility 와 견준다.
	//    켜짐 칸(meleeActive)은 프레임 끝 값이다 — 능력이 마지막 입력 프레임 안에서 끝나 그 프레임은 이미 0 이라, 내 쪽은 「남은 입력 2 이상」과 견준다
	int spamWeapons = 0, spamFails = 0, spamStarts = 0, spamFrames = 0;
	{
		const int colMeleeActive = column("meleeActive"), colFrame = column("frame");
		if (colMeleeActive < 0 || colFrame < 0)
		{
			std::printf("MELEECHECK FAIL 기록 표 머리에 frame · meleeActive 가 없음\n");
			return 2;
		}
		const size_t need = (size_t)std::max({ colStep, colEv, colMeleeActive, colFrame }) + 1;
		for (int w = 0; w < (int)EWeapon::Count; ++w)
		{
			const WeaponDef& def = GetWeaponDef((EWeapon)w);
			const std::string step = std::string("spam_") + def.name;
			CMeleeAbility spamAbility;
			bool pressing = false;
			int frames = 0, startsLyra = 0, startsMine = 0, diffs = 0;
			std::string firstDiff = "-";
			for (const std::vector<std::string>& row : rows)
			{
				if (row.size() < need || row[(size_t)colStep] != step) continue;
				const bool lyraStart = row[(size_t)colEv].find("melee_start;") != std::string::npos;
				if (!pressing && !lyraStart) continue;
				pressing = true;
				spamAbility.NextInput();
				const bool mineStart = spamAbility.TryActivate(def, kFrameSeconds);
				const bool lyraActive = row[(size_t)colMeleeActive] == "1";
				const bool mineActive = spamAbility.GetInputsLeft() >= 2;
				++frames;
				if (lyraStart) ++startsLyra;
				if (mineStart) ++startsMine;
				if (lyraStart != mineStart || lyraActive != mineActive)
				{
					if (diffs == 0) firstDiff = row[(size_t)colFrame];
					++diffs;
				}
			}
			const bool fail = diffs != 0 || startsLyra < 2;		// 켜짐이 둘 이상이어야 간격을 본다
			++spamWeapons;
			spamStarts += startsLyra;
			spamFrames += frames;
			if (fail) ++spamFails;
			std::printf("SPAM %s weapon=%s inputs=%u frames=%d starts lyra=%d mine=%d diffs=%d firstDiffFrame=%s\n",
				fail ? "DIFF" : "ok", def.name, MeleeAbilityInputs(def, kFrameSeconds), frames, startsLyra, startsMine, diffs, firstDiff.c_str());
		}
	}
	std::printf("MELEE abilitySpam weapons=%d fails=%d starts=%d frames=%d\n", spamWeapons, spamFails, spamStarts, spamFrames);

	// ── 근접 막기 규칙(E-3) — 켜져 있으면 발사 · 재장전 입력 · 자동 재장전이 막히고, 켜지는 입력은 도는 재장전을 끊는다(TagRelationships_ShooterHero).
	//    규칙마다 근접 없이 같은 입력을 준 대조를 같이 본다 — 대조가 안 되면 막힌 게 근접 때문인지 모른다. 규칙은 무기와 상관없어 권총 하나로
	int ruleCompared = 0, ruleFails = 0;
	{
		const WeaponMotion still;
		const auto rule = [&](const char* name, bool ok)
		{
			++ruleCompared;
			if (!ok) ++ruleFails;
			std::printf("RULE %s %s\n", ok ? "ok" : "DIFF", name);
		};
		const auto idle = [&](CWeaponCycle& cycle, int frames, bool meleeActive)
		{
			WeaponInput in;
			in.meleeActive = meleeActive;
			bool started = false;
			for (int i = 0; i < frames; ++i) started = cycle.Tick(kFrameSeconds, in, still).reloadStarted || started;
			return started;
		};
		// 한 발 쏘고 발사 능력이 끝날 때까지 — 재장전을 누를 수 있는 상태
		const auto oneShot = [&](CWeaponCycle& cycle)
		{
			WeaponInput fire;
			fire.fireHeld = true;
			cycle.Tick(kFrameSeconds, fire, still);
			for (int i = 0; i < 600 && cycle.IsFireActive(); ++i) cycle.Tick(kFrameSeconds, WeaponInput(), still);
		};
		{
			CWeaponCycle cycle;
			cycle.Init(EWeapon::Pistol);
			WeaponInput in;
			in.fireHeld = true;
			in.meleeActive = true;
			in.meleeBlocksFire = true;
			const int32_t before = cycle.GetMagazine();
			const WeaponEvents blocked = cycle.Tick(kFrameSeconds, in, still);
			rule("fireBlockedWhileMelee", !blocked.fired && cycle.GetMagazine() == before);
			in.meleeActive = false;
			in.meleeBlocksFire = false;
			rule("fireControl", cycle.Tick(kFrameSeconds, in, still).fired);
		}
		{
			// 근접이 켜지는 입력의 쥔 사격 — Lyra 는 쥔 사격을 누름 능력보다 먼저 켜 한 발 쏜다(L-12-b). 켜진 셈(meleeActive)이어도 누름 전 근접이 없으면 쏜다
			CWeaponCycle cycle;
			cycle.Init(EWeapon::Pistol);
			WeaponInput in;
			in.fireHeld = true;
			in.meleeStarted = true;
			in.meleeActive = true;
			rule("fireFirstWhenMeleeStarts", cycle.Tick(kFrameSeconds, in, still).fired);
		}
		{
			CWeaponCycle cycle;
			cycle.Init(EWeapon::Pistol);
			oneShot(cycle);
			WeaponInput in;
			in.reloadPressed = true;
			in.meleeActive = true;
			const WeaponEvents blocked = cycle.Tick(kFrameSeconds, in, still);
			rule("reloadInputBlockedWhileMelee", !blocked.reloadStarted && !cycle.IsReloading());
			in.meleeActive = false;
			rule("reloadInputControl", cycle.Tick(kFrameSeconds, in, still).reloadStarted);
		}
		{
			// 도는 재장전 — 근접이 켜지는 입력에 끊기고 탄이 안 들어간다. 대조 = 근접 없이 두면 탄이 찬다
			const auto reloading = [&](CWeaponCycle& cycle)
			{
				cycle.Init(EWeapon::Pistol);
				oneShot(cycle);
				WeaponInput in;
				in.reloadPressed = true;
				cycle.Tick(kFrameSeconds, in, still);
			};
			CWeaponCycle cut;
			reloading(cut);
			const int32_t magazine = cut.GetMagazine();
			WeaponInput start;
			start.meleeStarted = true;
			start.meleeActive = true;
			const WeaponEvents cancelled = cut.Tick(kFrameSeconds, start, still);
			idle(cut, 600, false);
			rule("reloadCancelledByMeleeStart", cancelled.reloadCancelledByMelee && !cut.IsReloading() && cut.GetMagazine() == magazine);
			CWeaponCycle full;
			reloading(full);
			const bool wasReloading = full.IsReloading();
			idle(full, 600, false);
			rule("reloadControl", wasReloading && full.GetMagazine() == full.GetDef().magazineSize);
		}
		{
			// 빈 탄창 자동 재장전 — 근접이 켜져 있는 동안은 안 걸리고, 꺼지면 걸린다
			CWeaponCycle cycle;
			cycle.Init(EWeapon::Pistol);
			WeaponInput fire;
			fire.fireHeld = true;
			for (int i = 0; i < 6000 && cycle.GetMagazine() > 0; ++i) cycle.Tick(kFrameSeconds, fire, still);
			const bool startedWhileMelee = idle(cycle, 300, true);
			rule("autoReloadBlockedWhileMelee", cycle.GetMagazine() == 0 && !startedWhileMelee && !cycle.IsReloading());
			rule("autoReloadControl", idle(cycle, 300, false));
		}
	}
	std::printf("MELEE rules compared=%d fails=%d\n", ruleCompared, ruleFails);

	const int fails = ability.fails + tracePrev.fails + traceNow.fails + selfFails + restFails + inputFails + pushFails + replayFails + fixFails + spamFails + ruleFails;
	std::printf("MELEE ability compared=%d fails=%d (damage %d · sameTeam %d · none %d, first touch capsule %d · mesh %d) maxDistDiff=%.4f walkCells=%d\n",
		ability.compared, ability.fails, abilityDamage, abilitySameTeam, abilityNone, capsuleFirst, meshFirst, ability.maxDist, abilityWalkCells);
	std::printf("MELEE tracePrev compared=%d fails=%d maxDistDiff=%.4f maxImpactDiff=%.4f\n", tracePrev.compared, tracePrev.fails, tracePrev.maxDist, tracePrev.maxImpact);
	std::printf("MELEE traceNow compared=%d fails=%d maxDistDiff=%.4f maxImpactDiff=%.4f\n", traceNow.compared, traceNow.fails, traceNow.maxDist, traceNow.maxImpact);
	std::printf("MELEE handSelf compared=%d fails=%d maxDiff=%.9f\n", selfCompared, selfFails, selfMax);
	std::printf("MELEE restHand fails=%d\n", restFails);
	const MoveCounters& mc = moveSim.GetCounters();
	std::printf("MELEE push compared=%d fails=%d frames=%d blocked=%d maxPosDiff=%.4f maxVelDiff=%.4f walkCells=%d fallCells=%d inputFrames=%d fallingIterations=%llu\n", pushCompared, pushFails, pushFrames, pushBlocked, pushMaxPos, pushMaxVel,
		pushWalkCells, pushFallCells, pushInputFrames, (unsigned long long)moveSim.GetCounters().fallingIterations);
	std::printf("MELEE pushCount applied=%llu replaced=%llu prepared=%llu finished=%llu restores=%llu adds=%llu external=%llu apexRefund=%llu\n",
		(unsigned long long)mc.rootForceApplied, (unsigned long long)mc.rootForceReplaced, (unsigned long long)mc.rootForcePrepared, (unsigned long long)mc.rootForceFinished,
		(unsigned long long)mc.rootForceRestores, (unsigned long long)mc.rootForceAdds, (unsigned long long)mc.rootForceExternal, (unsigned long long)mc.rootForceApexRefund);
	std::printf("MELEECHECK %s events=%zu inputFails=%d fails=%d\n", fails == 0 ? "PASS" : "FAIL", events.size(), inputFails, fails);
	return fails == 0 ? 0 : 1;
}
