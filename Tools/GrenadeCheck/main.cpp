// 수류탄 대조 도구 — Lyra 연습장 기록과 Shared/Projectile 걸음을 맞춰 본다(컨텐츠 G-1).
//
//   GrenadeCheck <map_gym.mocl> <기록 폴더> <태그>... [--verbose] [--pawn-row now|prev]
//
// 기록 = Tools/LyraPilot/record_content_probe.ps1 -Mode grenade(G-0). 태그마다 셋을 읽는다
//   cprobe_<태그>.log      MOB_CPROBE_GSPAWN(생긴 자리 · 회전 · 첫 속도) · GHIT(부딪힘) · GDROP(기록 도구가 옮긴 수류탄)
//   cprobe_<태그>.csv      사람 둘(나 · 상대)의 캡슐 — 수류탄이 맞을 수 있는 사람
//   cprobe_<태그>_gren.csv 수류탄마다 프레임 끝 자리 · 속도 · 회전(나이 0 줄 = 생긴 프레임의 걸음 뒤)
// 날아감(G-1-b) — 수류탄마다 생긴 상태에서 1/60 초씩 걸어 첫 부딪힘 전 프레임까지 자리 · 속도 · 회전을 견주고,
//   첫 부딪힘이 같은 프레임에 같은 것(맵 · 나 · 상대 · 수류탄)에 났는지, 맞은 거리 · 물린 시각 · 법선이 같은지 본다
// 끝 코드: 0 = 전부 통과, 1 = 불일치, 2 = 입력 오류
#include "MapCollision.h"
#include "Projectile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
	constexpr float kFrameSeconds = 1.f / 60.f;	// 기록은 고정 프레임(-UseFixedTimeStep)
	constexpr double kPawnRadius = 35.0;			// Lyra 사람 캡슐 반지름(Movement.h capsuleRadius)
	constexpr double kOwnerReach = 40.0;			// 생긴 자리 = 던진 사람 중심 + 앞 5 + 위 25 — 이 안이면 던진 사람으로 본다
	constexpr int kMaxFrames = 400;

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
		double time = 0, distance = 0;
		Vec3 normal;
		bool startPenetrating = false;
	};

	struct Teleport
	{
		int frame = 0;
		Vec3 location, velocity;
	};

	struct Grenade
	{
		std::string name, step;
		int spawnFrame = 0;
		Vec3 location, velocity;
		Rotator rotation;
		std::map<int, GrenRow> rows;		// 나이 → 줄
		RecordHit firstHit;
		std::vector<Teleport> teleports;
		uint32_t owner = 0;				// kIdMe · kIdFoe · 0(못 찾음)
	};

	struct Totals
	{
		int grenades = 0, frames = 0, fails = 0;
		double maxPos = 0, maxVel = 0, maxRot = 0, maxLaunch = 0;
		int sameKind[4] = {};	// 같음의 종류별 — 맵 · 나 · 상대 · 수류탄
		int same = 0, early = 0, late = 0, kindDiff = 0, noneBoth = 0, ownerUnknown = 0, launchFails = 0;
		int hitCompared = 0, hitFails = 0, meshHits = 0;
		double maxHitDist = 0, maxHitTime = 0, maxHitNormal = 0;
		int rowsRead = 0, spawnsRead = 0, hitsRead = 0;

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
		}
	};

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

		// 로그 — 생김 · 부딪힘 · 옮김
		struct PendingHit { std::string name; RecordHit hit; };
		std::vector<PendingHit> hits;
		struct PendingDrop { std::string name; Teleport tp; };
		std::vector<PendingDrop> drops;
		for (const std::string& line : Lines(logData))
		{
			const size_t at = line.find("MOB_CPROBE_G");
			if (at == std::string::npos) continue;
			const size_t space = line.find(' ', at);
			if (space == std::string::npos) continue;
			const std::string kind = line.substr(at + 11, space - at - 11);
			if (kind != "GSPAWN" && kind != "GHIT" && kind != "GDROP") continue;
			const auto kv = KeyValues(line.substr(space + 1));
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
				h.hit.startPenetrating = kv.count("pen") && kv.at("pen") == "1";
				if (h.name.empty() || h.hit.frame < 0 || !kv.count("normal") || !ParseVec(kv.at("normal"), h.hit.normal))
				{
					std::printf("GRENADECHECK input tag=%s GHIT 줄 꼴 다름\n", tag.c_str());
					return false;
				}
				hits.push_back(h);
				++t.hitsRead;
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
			if (g && g->firstHit.frame < 0) g->firstHit = h.hit;
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
			bool failed = false;
			std::string firstFail;
			bool simHit = false;
			int simHitFrame = -1;
			ProjectileHit simFirst;
			const int recHitFrame = g.firstHit.frame;

			for (int age = 0; age < kMaxFrames; ++age)
			{
				const int frame = g.spawnFrame + age;
				const auto rowIt = g.rows.find(age);
				// 터짐 · 사라짐. 상대에 맞으면 그 걸음 안에서 터져 그 프레임 줄이 이미 숨김이다 — 기록의 첫 부딪힘 프레임은 걷는다
				if (rowIt == g.rows.end() || (rowIt->second.hidden && frame != recHitFrame)) break;

				// 막는 것 — 사람 둘 · 다른 수류탄(이 프레임 걸음 전 자리 = 앞 프레임 끝)
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
						obstacles.push_back(o);
					}
					if (p.foeHp > 0)
					{
						ProjectileObstacle o;
						o.center = p.foe; o.radius = kPawnRadius; o.halfHeight = p.foeHalfHeight;
						o.id = kIdFoe; o.kind = EProjectileHitKind::Pawn; o.owner = g.owner == kIdFoe;
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

				ProjectileHit frameHits[8];
				const size_t n = TickProjectile(map, params, s, kFrameSeconds, obstacles.data(), obstacles.size(), frameHits, 8);
				const bool recHitHere = recHitFrame == frame;

				if (n > 0 || recHitHere)
				{
					if (n > 0)
					{
						simHit = true;
						simHitFrame = frame;
						simFirst = frameHits[0];
					}
					break;	// 부딪힌 프레임의 줄은 튕김 뒤 — G-1-c 몫
				}

				// 부딪힘 전 프레임 대조
				const GrenRow& r = rowIt->second;
				const double dp = Dist(s.location, r.location), dv = Dist(s.velocity, r.velocity), dr = RotDiff(s.rotation, r.rotation);
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

				// 기록 도구가 옮긴 수류탄 — 그 프레임 걸음 뒤 자리 · 속도를 덮는다(회전은 그대로)
				for (const Teleport& tp : g.teleports)
					if (tp.frame == frame)
					{
						s.location = tp.location;
						s.velocity = tp.velocity;
					}
			}

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
	for (const std::string& tag : tags)
		if (!RunTag(map, dir, tag, verbose, pawnRowNow, total)) return 2;

	const bool pass = total.fails == 0 && total.early == 0 && total.late == 0 && total.kindDiff == 0 && total.hitFails == 0
		&& total.launchFails == 0 && total.ownerUnknown == 0 && total.grenades > 0 && total.frames > 0;
	std::printf("GRENADECHECK flight grenades=%d frames=%d fails=%d maxPosDiff=%.6f maxVelDiff=%.6f maxRotDiff=%.6f firstHit same=%d early=%d late=%d kindDiff=%d none=%d\n",
		total.grenades, total.frames, total.fails, total.maxPos, total.maxVel, total.maxRot, total.same, total.early, total.late, total.kindDiff, total.noneBoth);
	std::printf("GRENADECHECK firstHit compared=%d fails=%d maxDistDiff=%.6f maxTimeDiff=%.7f maxNormalDiff=%.6f pawnMeshNotCompared=%d\n",
		total.hitCompared, total.hitFails, total.maxHitDist, total.maxHitTime, total.maxHitNormal, total.meshHits);
	std::printf("GRENADECHECK firstHit kinds world=%d me=%d foe=%d gren=%d\n", total.sameKind[0], total.sameKind[1], total.sameKind[2], total.sameKind[3]);
	std::printf("GRENADECHECK launch grenades=%d fails=%d maxVelDiff=%.6f ownerUnknown=%d (rows=%d spawns=%d hits=%d)\n",
		total.grenades, total.launchFails, total.maxLaunch, total.ownerUnknown, total.rowsRead, total.spawnsRead, total.hitsRead);
	std::printf("GRENADECHECK %s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
