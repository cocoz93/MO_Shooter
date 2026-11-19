// Lyra 가 기록한 광선·캡슐 결과를 Shared/MapCollision 으로 다시 돌려 대조한다.
// - 왜: 「맞췄다」의 근거는 Lyra 실행값뿐이다. 맵 파일과 기록은 Tools/LyraPilot/record_traces.ps1 이 만든다.
//
//   MapCheck <map.mocl> <trace.csv> [--kill-first-hit] [--shift-z <cm>] [--mismatch <출력.csv>]
//     --kill-first-hit  Lyra 가 맞았다는 첫 광선의 삼각형을 파일에서 지우고 돌린다(시험의 시험 — 불일치가 늘어야 한다)
//     --shift-z         맵을 위로 옮기고 돌린다(시험의 시험 — 거리 차가 커져야 한다)
//     --capsule-pad     캡슐을 살짝 키워 돌린다(진단용 — 스치는 접촉의 허용치를 잰다)
//     --flag-probe      깃발 거름 탐침 — 물체마다 깃발 넷으로 광선 · 기운 캡슐을 대어 깃발 없는 맞음이 나오는지 센다(G-1-a)
//     --axis-probe <n>  기운 축 끼움 시험 — 캡슐 기록 n 개 자리에서 수류탄 캡슐을 일곱 축으로 쓸어 공 줄과 대조한다(G-1-a)
//
//   돌려주는 값: 0 = 끝까지 돌았다(판정은 출력을 보고 한다) · 1 = 읽기 실패
#include "MapCollision.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using mo::Vec3;

namespace
{
	struct Record
	{
		char kind = 0;
		Vec3 a, b;
		bool hit = false;
		bool penetrating = false;
		double distance = 0;
		Vec3 normal;
		std::string actor;
	};

	struct KindStats
	{
		size_t total = 0, bothHit = 0, bothMiss = 0, onlyLyra = 0, onlyMine = 0;
		size_t penetrateMismatch = 0, lyraPenetrating = 0, over01 = 0, over1 = 0;
		double maxDiff = 0, sumDiff = 0;
		std::vector<double> diffs;
		// 캡슐 면 법선 대조(언리얼 ImpactNormal) — 이동의 미끄러짐·바닥 판정이 이 값을 쓴다
		size_t normalCompared = 0, normalOver001 = 0, normalOver01 = 0, normalPenCompared = 0, normalPenOver01 = 0;
		double normalMax = 0;
	};

	bool ReadFile(const char* path, std::vector<uint8_t>& out)
	{
		FILE* f = std::fopen(path, "rb");
		if (!f) return false;
		std::fseek(f, 0, SEEK_END);
		const long long size = _ftelli64(f);
		std::fseek(f, 0, SEEK_SET);
		out.resize((size_t)size);
		const size_t got = std::fread(out.data(), 1, out.size(), f);
		std::fclose(f);
		return got == out.size();
	}

	bool ReadRecords(const char* path, std::vector<Record>& out, double& radius, double& halfHeight)
	{
		FILE* f = std::fopen(path, "r");
		if (!f) return false;
		char line[1024];
		while (std::fgets(line, sizeof(line), f))
		{
			if (line[0] == '#')
			{
				double r, h;
				if (std::sscanf(line, "# capsule r=%lf hh=%lf", &r, &h) == 2)
				{
					radius = r;
					halfHeight = h;
				}
				continue;
			}
			if (std::strncmp(line, "kind,", 5) == 0) continue;

			Record rec;
			char kind;
			int hit, pen;
			int used = 0;
			if (std::sscanf(line, "%c,%lf,%lf,%lf,%lf,%lf,%lf,%d,%d,%lf,%lf,%lf,%lf,%n", &kind,
				&rec.a.x, &rec.a.y, &rec.a.z, &rec.b.x, &rec.b.y, &rec.b.z, &hit, &pen,
				&rec.distance, &rec.normal.x, &rec.normal.y, &rec.normal.z, &used) < 13)
			{
				continue;
			}
			rec.kind = kind;
			rec.hit = hit != 0;
			rec.penetrating = pen != 0;
			rec.actor = line + used;
			while (!rec.actor.empty() && (rec.actor.back() == '\n' || rec.actor.back() == '\r')) rec.actor.pop_back();
			out.push_back(std::move(rec));
		}
		std::fclose(f);
		return true;
	}

	// 파일에서 삼각형 하나를 찾아 넓이 0 으로 만든다. 돌려준 값 = 지운 개수
	int KillTriangle(std::vector<uint8_t>& file, const float* tri)
	{
		int killed = 0;
		uint8_t* cursor = file.data() + 8;
		uint32_t objectCount;
		std::memcpy(&objectCount, cursor, 4);
		cursor = file.data() + 40;
		for (uint32_t i = 0; i < objectCount; ++i)
		{
			uint32_t kind;
			std::memcpy(&kind, cursor + 4, 4);
			cursor += 8;
			if (kind == 1) { cursor += 16; continue; }
			if (kind == 2) { cursor += 28; continue; }
			uint32_t triCount;
			std::memcpy(&triCount, cursor, 4);
			cursor += 4;
			for (uint32_t t = 0; t < triCount; ++t)
			{
				if (std::memcmp(cursor, tri, 36) == 0)
				{
					std::memset(cursor, 0, 36);
					++killed;
				}
				cursor += 36;
			}
		}
		return killed;
	}

	void ShiftZ(std::vector<uint8_t>& file, float amount)
	{
		uint8_t* cursor = file.data() + 8;
		uint32_t objectCount;
		std::memcpy(&objectCount, cursor, 4);
		cursor = file.data() + 40;
		const auto bump = [&](uint8_t* p)
		{
			float z;
			std::memcpy(&z, p, 4);
			z += amount;
			std::memcpy(p, &z, 4);
		};
		for (uint32_t i = 0; i < objectCount; ++i)
		{
			uint32_t kind;
			std::memcpy(&kind, cursor + 4, 4);
			cursor += 8;
			if (kind == 1) { bump(cursor + 8); cursor += 16; continue; }
			if (kind == 2) { bump(cursor + 8); bump(cursor + 20); cursor += 28; continue; }
			uint32_t triCount;
			std::memcpy(&triCount, cursor, 4);
			cursor += 4;
			for (uint32_t t = 0; t < triCount * 3; ++t)
			{
				bump(cursor + 8);
				cursor += 12;
			}
		}
	}

	// 맵 파일의 삼각형 물체 하나 — 탐침용(깃발 · 첫 삼각형 · 꼭짓점 평균)
	struct FileObject
	{
		uint32_t index = 0, flags = 0;
		bool hasTri = false;
		Vec3 a, b, c, mean;
	};

	std::vector<FileObject> ReadObjects(const std::vector<uint8_t>& file)
	{
		std::vector<FileObject> out;
		const uint8_t* cursor = file.data() + 8;
		uint32_t objectCount;
		std::memcpy(&objectCount, cursor, 4);
		cursor = file.data() + 40;
		const auto vertex = [](const uint8_t* p) { float v[3]; std::memcpy(v, p, 12); return Vec3(v[0], v[1], v[2]); };
		for (uint32_t i = 0; i < objectCount; ++i)
		{
			uint32_t flags, kind;
			std::memcpy(&flags, cursor, 4);
			std::memcpy(&kind, cursor + 4, 4);
			cursor += 8;
			if (kind == 1) { cursor += 16; continue; }
			if (kind == 2) { cursor += 28; continue; }
			uint32_t triCount;
			std::memcpy(&triCount, cursor, 4);
			cursor += 4;
			FileObject obj;
			obj.index = i;
			obj.flags = flags;
			Vec3 sum;
			for (uint32_t t = 0; t < triCount; ++t, cursor += 36)
			{
				const Vec3 a = vertex(cursor), b = vertex(cursor + 12), c = vertex(cursor + 24);
				sum = sum + a + b + c;
				const Vec3 e1 = b - a, e2 = c - a;
				const Vec3 cr(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
				if (!obj.hasTri && cr.x * cr.x + cr.y * cr.y + cr.z * cr.z > 1e-4)
				{
					obj.hasTri = true;
					obj.a = a; obj.b = b; obj.c = c;
				}
			}
			if (triCount > 0) obj.mean = sum * (1.0 / (3.0 * triCount));
			out.push_back(obj);
		}
		return out;
	}

	Vec3 Unit(const Vec3& v)
	{
		const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		return len > 1e-12 ? v * (1.0 / len) : Vec3(0, 0, 1);
	}

	// 깃발 거름 탐침(G-1-a) — 물체마다 첫 삼각형 한가운데를 앞뒤로 지나는 광선 · 기운 캡슐을 깃발 넷으로 쏜다
	// - 판정: 돌려준 맞음의 깃발에 질의 깃발이 없으면 wrongFlag(거름이 빠진 것). 깃발이 있는 물체를 못 맞히면 missOwn(다른 물체가 가렸거나 나무에 안 들어감)
	// - 속 찬 물체는 꼭짓점 평균(볼록이라 안쪽)에서 광선을 쏴 「안에서 시작」 갈래를 본다
	// - 겹침: 깃발이 없고 짝 깃발(이동↔수류탄 · 무기↔폭발 선)만 있는 물체 한가운데에 작은 캡슐을 두면 겹치지 않아야 한다
	void FlagProbe(const std::vector<uint8_t>& file, const mo::CMapCollision& map)
	{
		const std::vector<FileObject> objects = ReadObjects(file);
		const uint32_t flagList[4] = { mo::MAP_BLOCK_MOVE, mo::MAP_BLOCK_WEAPON, mo::MAP_BLOCK_GRENADE, mo::MAP_BLOCK_BLAST };
		const char* flagName[4] = { "move", "weapon", "grenade", "blast" };
		const uint32_t partner[4] = { mo::MAP_BLOCK_GRENADE, mo::MAP_BLOCK_BLAST, mo::MAP_BLOCK_MOVE, mo::MAP_BLOCK_WEAPON };
		size_t solids = 0;
		for (const FileObject& obj : objects) solids += (obj.flags & mo::MAP_SOLID) ? 1 : 0;
		std::printf("FLAGPROBE objects=%zu solid=%zu\n", objects.size(), solids);
		for (int f = 0; f < 4; ++f)
		{
			const uint32_t flag = flagList[f];
			size_t has = 0, lacks = 0, oneSided = 0;
			size_t rayOwn = 0, rayMiss = 0, rayWrong = 0, sweepOwn = 0, sweepMiss = 0, sweepWrong = 0;
			size_t insideTried = 0, insideOwn = 0, insideWrong = 0, overlapWrong = 0, lackingHit = 0, lackOverlap = 0, sideHas = 0, sideOwn = 0;
			for (const FileObject& obj : objects)
			{
				if (!obj.hasTri) continue;
				const bool objHas = (obj.flags & flag) != 0;
				has += objHas;
				lacks += !objHas;
				const Vec3 center = (obj.a + obj.b + obj.c) * (1.0 / 3.0);
				const Vec3 n = Unit(Vec3((obj.b - obj.a).y * (obj.c - obj.a).z - (obj.b - obj.a).z * (obj.c - obj.a).y,
					(obj.b - obj.a).z * (obj.c - obj.a).x - (obj.b - obj.a).x * (obj.c - obj.a).z,
					(obj.b - obj.a).x * (obj.c - obj.a).y - (obj.b - obj.a).y * (obj.c - obj.a).x));
				bool rayHitOwn = false, sweepHitOwn = false;
				for (int side = 0; side < 2; ++side)
				{
					const Vec3 from = center + n * (side ? -2.0 : 2.0), to = center + n * (side ? 2.0 : -2.0);
					mo::RayHit ray;
					if (map.Raycast(from, to, flag, ray))
					{
						if (!(ray.flags & flag)) ++rayWrong;
						if (ray.object == obj.index) rayHitOwn = true;
					}
					// 기운 캡슐(축 = 면 법선과 수직인 한 변 방향) — 반지름 1 · 반높이 3
					const Vec3 from2 = center + n * (side ? -5.0 : 5.0), to2 = center + n * (side ? 5.0 : -5.0);
					mo::CapsuleHit sweep;
					if (map.SweepCapsule(from2, to2, Unit(obj.b - obj.a), 1.0, 3.0, flag, sweep))
					{
						if (!(sweep.flags & flag)) ++sweepWrong;
						if (sweep.object == obj.index) sweepHitOwn = true;
					}
				}
				if (objHas) { rayOwn += rayHitOwn; rayMiss += !rayHitOwn; sweepOwn += sweepHitOwn; sweepMiss += !sweepHitOwn; }
				// 한쪽만 물체(이 깃발은 있고 짝 깃발은 없음)를 제 깃발로 맞혔나 — 0 이면 그 물체들이 나무에 안 들어간 것
				if (objHas && !(obj.flags & partner[f])) { ++sideHas; sideOwn += (rayHitOwn || sweepHitOwn); }
				else if (rayHitOwn || sweepHitOwn) ++lackingHit;
				if (obj.flags & mo::MAP_SOLID)
				{
					++insideTried;
					mo::RayHit ray;
					if (map.Raycast(obj.mean, obj.mean + Vec3(0.0, 0.0, 1.0), flag, ray))
					{
						if (!(ray.flags & flag)) ++insideWrong;
						if (objHas && ray.startInside && ray.object == obj.index) ++insideOwn;
					}
				}
				if (!objHas)
				{
					// 깃발 없는 물체 한가운데의 겹침 — 곁에 깃발 있는 물체가 0.5cm 안에 있어도 참이 된다. 짝 깃발 물체를 맵에서 뺀 파일과 수를 대 보면 가른다
					const bool overlap = map.OverlapCapsule(center, 0.5, 0.5, flag);
					lackOverlap += overlap;
					if (obj.flags & partner[f])
					{
						++oneSided;
						overlapWrong += overlap;
					}
				}
			}
			std::printf("FLAGPROBE flag=%s has=%zu lacks=%zu | ray own=%zu miss=%zu wrongFlag=%zu | sweep own=%zu miss=%zu wrongFlag=%zu | lackingHit=%zu | inside tried=%zu own=%zu wrongFlag=%zu | oneSided=%zu overlapWrong=%zu lackOverlap=%zu | sideHas=%zu sideOwn=%zu\n",
				flagName[f], has, lacks, rayOwn, rayMiss, rayWrong, sweepOwn, sweepMiss, sweepWrong, lackingHit, insideTried, insideOwn, insideWrong, oneSided, overlapWrong, lackOverlap, sideHas, sideOwn);
		}
	}

	// 기운 축 끼움 시험(G-1-a) — 기운 캡슐 = 축 위 공들의 합. 안쪽 공(반지름 r)들은 캡슐 안에, 바깥 공(r + 틈)들은 캡슐을 덮는다
	// 그래서 「바깥 공 줄의 첫 닿음 ≤ 캡슐 ≤ 안쪽 공 줄의 첫 닿음」이어야 한다. 공은 축이 없으니 축 처리와 따로 선 기준이다
	void AxisProbe(const std::vector<Record>& records, const mo::CMapCollision& map, size_t want)
	{
		const double r = 5.0, hh = 9.0, axisHalf = hh - r;	// 수류탄 몸 캡슐(플랜 83줄)
		const int kSpheres = 81;
		const double step = 2.0 * axisHalf / (kSpheres - 1);
		const double gap = r - std::sqrt(r * r - 0.25 * step * step) + 1e-6;
		const Vec3 axes[7] = { Vec3(1, 0, 0), Vec3(0, 1, 0), Unit(Vec3(1, 1, 1)), Unit(Vec3(1, -2, 0.5)), Unit(Vec3(0.3, 0.4, -0.866)), Unit(Vec3(0, 1, 1)), Unit(Vec3(-0.6, 0, 0.8)) };
		const uint32_t flags[2] = { mo::MAP_BLOCK_MOVE, mo::MAP_BLOCK_GRENADE };
		std::vector<const Record*> picks;
		size_t capsuleRecords = 0;
		for (const Record& rec : records) capsuleRecords += (rec.kind == 'C' || rec.kind == 'F' || rec.kind == 'A');
		const size_t stride = std::max<size_t>(1, capsuleRecords / std::max<size_t>(1, want));
		size_t seen = 0;
		for (const Record& rec : records)
		{
			if (!(rec.kind == 'C' || rec.kind == 'F' || rec.kind == 'A')) continue;
			if (seen++ % stride == 0 && picks.size() < want) picks.push_back(&rec);
		}
		size_t probes = 0, capHits = 0, innerHits = 0, outerHits = 0, pens = 0, fails = 0;
		double worstAbove = 0, worstBelow = 0;
		double slackInner = 0, slackOuter = 0;	// 끼움이 얼마나 빠듯한가 — 캡슐과 안쪽 · 바깥 공 줄 사이 가장 큰 거리(셋 다 맞았을 때)
		for (const Record* rec : picks)
		{
			const double length = std::sqrt((rec->b - rec->a).x * (rec->b - rec->a).x + (rec->b - rec->a).y * (rec->b - rec->a).y + (rec->b - rec->a).z * (rec->b - rec->a).z);
			for (const Vec3& axis : axes)
			{
				for (uint32_t flag : flags)
				{
					++probes;
					mo::CapsuleHit cap;
					const bool capHit = map.SweepCapsule(rec->a, rec->b, axis, r, hh, flag, cap);
					double inner = 2, outer = 2;	// 첫 닿음 시간(0~1), 2 = 안 닿음
					for (int k = 0; k < kSpheres; ++k)
					{
						const Vec3 off = axis * (-axisHalf + step * k);
						mo::CapsuleHit s;
						if (map.SweepCapsule(rec->a + off, rec->b + off, r, r, flag, s)) inner = std::min(inner, s.time);
						if (map.SweepCapsule(rec->a + off, rec->b + off, r + gap, r + gap, flag, s)) outer = std::min(outer, s.time);
					}
					capHits += capHit;
					innerHits += inner <= 1;
					outerHits += outer <= 1;
					pens += capHit && cap.startPenetrating;
					const double capTime = capHit ? cap.time : 2;
					// 거리로 잰 어긋남 — 캡슐이 안쪽보다 늦거나(above) 바깥보다 이르면(below) 틀림. 1e-3 cm 은 쓸기 계산의 반올림 여유
					const double above = (std::min(capTime, 1.5) - std::min(inner, 1.5)) * length;
					const double below = (std::min(outer, 1.5) - std::min(capTime, 1.5)) * length;
					worstAbove = std::max(worstAbove, above);
					worstBelow = std::max(worstBelow, below);
					if (above > 1e-3 || below > 1e-3) ++fails;
					if (capHit && inner <= 1 && outer <= 1)
					{
						slackInner = std::max(slackInner, (inner - capTime) * length);
						slackOuter = std::max(slackOuter, (capTime - outer) * length);
					}
				}
			}
		}
		std::printf("AXISPROBE probes=%zu records=%zu axes=7 flags=2 spheres=%d gap=%.6f | capHit=%zu innerHit=%zu outerHit=%zu startPen=%zu | fails=%zu worstAbove=%.6f worstBelow=%.6f | slackInner=%.6f slackOuter=%.6f\n",
			probes, picks.size(), kSpheres, gap, capHits, innerHits, outerHits, pens, fails, worstAbove, worstBelow, slackInner, slackOuter);
	}

	double Seconds(std::chrono::steady_clock::time_point from)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
	}

	double ProcessMegabytes()
	{
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS pmc{};
		if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.WorkingSetSize / (1024.0 * 1024.0);
#endif
		return 0;
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("MapCheck <map.mocl> <trace.csv> [--kill-first-hit] [--shift-z <cm>] [--mismatch <out.csv>]\n");
		return 1;
	}
	bool killFirstHit = false;
	double shift = 0;
	double capsulePad = 0;
	const char* mismatchPath = nullptr;
	bool flagProbe = false;
	size_t axisProbe = 0;
	for (int i = 3; i < argc; ++i)
	{
		if (!std::strcmp(argv[i], "--kill-first-hit")) killFirstHit = true;
		else if (!std::strcmp(argv[i], "--shift-z") && i + 1 < argc) shift = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--mismatch") && i + 1 < argc) mismatchPath = argv[++i];
		else if (!std::strcmp(argv[i], "--capsule-pad") && i + 1 < argc) capsulePad = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--flag-probe")) flagProbe = true;
		else if (!std::strcmp(argv[i], "--axis-probe") && i + 1 < argc) axisProbe = (size_t)std::atoll(argv[++i]);
	}

	std::vector<uint8_t> file;
	if (!ReadFile(argv[1], file))
	{
		std::printf("MAP_READ_FAIL %s\n", argv[1]);
		return 1;
	}
	std::vector<Record> records;
	double radius = 35, halfHeight = 90;
	if (!ReadRecords(argv[2], records, radius, halfHeight))
	{
		std::printf("TRACE_READ_FAIL %s\n", argv[2]);
		return 1;
	}
	radius += capsulePad;
	halfHeight += capsulePad;
	std::printf("TRACE records=%zu capsule r=%.3f hh=%.3f\n", records.size(), radius, halfHeight);
	if (records.empty()) return 1;

	if (shift != 0) ShiftZ(file, (float)shift);

	const double memBefore = ProcessMegabytes();
	auto loadStart = std::chrono::steady_clock::now();
	auto map = std::make_unique<mo::CMapCollision>();
	mo::EMapLoad result = map->LoadFromMemory(file.data(), file.size());
	double loadSec = Seconds(loadStart);
	if (result != mo::EMapLoad::Ok)
	{
		std::printf("MAP_LOAD_FAIL code=%d\n", (int)result);
		return 1;
	}

	if (killFirstHit)
	{
		bool done = false;
		for (const Record& rec : records)
		{
			if (rec.kind == 'C' || rec.kind == 'F' || rec.kind == 'A' || !rec.hit) continue;
			mo::RayHit hit;
			if (map->Raycast(rec.a, rec.b, mo::MAP_BLOCK_WEAPON, hit) && std::fabs(hit.distance - rec.distance) < 0.1 && hit.triangle[0] != 0)
			{
				const int killed = KillTriangle(file, hit.triangle);
				std::printf("KILLED triangles=%d at ray a=(%.1f,%.1f,%.1f) dist=%.3f\n", killed, rec.a.x, rec.a.y, rec.a.z, rec.distance);
				done = true;
				break;
			}
		}
		if (!done)
		{
			std::printf("KILL_NOT_APPLIED — 지울 삼각형을 못 찾음\n");
			return 1;
		}
		map = std::make_unique<mo::CMapCollision>();
		loadStart = std::chrono::steady_clock::now();
		result = map->LoadFromMemory(file.data(), file.size());
		loadSec = Seconds(loadStart);
		if (result != mo::EMapLoad::Ok) return 1;
	}

	std::printf("MAP objects=%u triangles=%u degenerate=%u prims=%u\n", map->GetObjectCount(), map->GetTriangleCount(), map->GetDegenerateCount(), map->GetPrimitiveCount());
	std::printf("TREE complex(weapon · blast) tris=%zu nodes=%zu | simple(move · grenade) tris=%zu nodes=%zu\n",
		map->GetTreeTriangleCount(mo::MAP_BLOCK_WEAPON), map->GetTreeNodeCount(mo::MAP_BLOCK_WEAPON),
		map->GetTreeTriangleCount(mo::MAP_BLOCK_MOVE), map->GetTreeNodeCount(mo::MAP_BLOCK_MOVE));
	std::printf("LOAD sec=%.3f treeBytes=%.1fMB processDelta=%.1fMB\n", loadSec, map->GetMemoryBytes() / (1024.0 * 1024.0), ProcessMegabytes() - memBefore);

	FILE* mismatch = mismatchPath ? std::fopen(mismatchPath, "w") : nullptr;
	if (mismatch) std::fprintf(mismatch, "kind,ax,ay,az,bx,by,bz,lyraHit,mineHit,lyraPen,minePen,lyraDist,mineDist,mineFlags,actor\n");

	// 첫 글자가 대문자 R·D·E = 광선, C·F·A = 캡슐
	std::map<char, KindStats> stats;
	std::map<std::string, int> onlyLyraByActor;
	std::map<std::string, int> diffByActor;
	std::map<uint32_t, int> onlyMineByFlags;

	for (const Record& rec : records)
	{
		KindStats& s = stats[rec.kind];
		++s.total;
		const bool isCapsule = rec.kind == 'C' || rec.kind == 'F' || rec.kind == 'A';

		bool mineHit = false;
		bool minePen = false;
		double mineDist = 0;
		uint32_t mineFlags = 0;
		Vec3 mineNormal;
		if (isCapsule)
		{
			mo::CapsuleHit hit;
			mineHit = map->SweepCapsule(rec.a, rec.b, radius, halfHeight, mo::MAP_BLOCK_MOVE, hit);
			if (mineHit) { minePen = hit.startPenetrating; mineDist = hit.distance; mineFlags = hit.flags; mineNormal = hit.impactNormal; }
		}
		else
		{
			mo::RayHit hit;
			mineHit = map->Raycast(rec.a, rec.b, mo::MAP_BLOCK_WEAPON, hit);
			if (mineHit) { minePen = hit.startInside; mineDist = hit.distance; mineFlags = hit.flags; }
		}

		if (rec.penetrating) ++s.lyraPenetrating;
		bool bad = false;
		if (rec.hit && mineHit)
		{
			++s.bothHit;
			const double diff = std::fabs(rec.distance - mineDist);
			s.sumDiff += diff;
			s.maxDiff = std::max(s.maxDiff, diff);
			s.diffs.push_back(diff);
			if (rec.penetrating != minePen) ++s.penetrateMismatch;
			if (diff > 0.1)
			{
				++s.over01;
				bad = true;
				++diffByActor[rec.actor];
			}
			if (diff > 1.0) ++s.over1;

			if (isCapsule && rec.penetrating == minePen)
			{
				const Vec3 d = rec.normal - mineNormal;
				const double normalDiff = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
				if (minePen)
				{
					++s.normalPenCompared;
					if (normalDiff > 0.1) ++s.normalPenOver01;
				}
				else
				{
					++s.normalCompared;
					s.normalMax = std::max(s.normalMax, normalDiff);
					if (normalDiff > 0.01) ++s.normalOver001;
					if (normalDiff > 0.1) ++s.normalOver01;
				}
			}
		}
		else if (rec.hit)
		{
			++s.onlyLyra;
			bad = true;
			++onlyLyraByActor[rec.actor];
		}
		else if (mineHit)
		{
			++s.onlyMine;
			bad = true;
			++onlyMineByFlags[mineFlags];
		}
		else
		{
			++s.bothMiss;
		}

		if (bad && mismatch)
		{
			std::fprintf(mismatch, "%c,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%d,%.4f,%.4f,%u,%s\n", rec.kind,
				rec.a.x, rec.a.y, rec.a.z, rec.b.x, rec.b.y, rec.b.z, rec.hit ? 1 : 0, mineHit ? 1 : 0,
				rec.penetrating ? 1 : 0, minePen ? 1 : 0, rec.distance, mineDist, mineFlags, rec.actor.c_str());
		}
	}
	if (mismatch) std::fclose(mismatch);

	for (auto& pair : stats)
	{
		KindStats& s = pair.second;
		std::sort(s.diffs.begin(), s.diffs.end());
		const double p99 = s.diffs.empty() ? 0 : s.diffs[(size_t)((s.diffs.size() - 1) * 0.99)];
		std::printf("CHECK kind=%c total=%zu bothHit=%zu bothMiss=%zu onlyLyra=%zu onlyMine=%zu lyraPen=%zu penMismatch=%zu maxDiff=%.4f p99Diff=%.5f over0.1=%zu over1=%zu\n",
			pair.first, s.total, s.bothHit, s.bothMiss, s.onlyLyra, s.onlyMine, s.lyraPenetrating, s.penetrateMismatch, s.maxDiff, p99, s.over01, s.over1);
		if (s.normalCompared + s.normalPenCompared > 0)
		{
			std::printf("NORMAL kind=%c compared=%zu over0.01=%zu over0.1=%zu max=%.4f | startPen compared=%zu over0.1=%zu\n",
				pair.first, s.normalCompared, s.normalOver001, s.normalOver01, s.normalMax, s.normalPenCompared, s.normalPenOver01);
		}
	}

	const auto printTop = [](const char* label, const auto& counts)
	{
		std::vector<std::pair<int, std::string>> sorted;
		for (const auto& pair : counts) sorted.push_back({ pair.second, std::string(pair.first) });
		std::sort(sorted.rbegin(), sorted.rend());
		for (size_t i = 0; i < sorted.size() && i < 12; ++i) std::printf("%s %s count=%d\n", label, sorted[i].second.c_str(), sorted[i].first);
	};
	printTop("ONLY_LYRA_ACTOR", onlyLyraByActor);
	printTop("DIFF_ACTOR", diffByActor);
	for (const auto& pair : onlyMineByFlags) std::printf("ONLY_MINE_FLAGS flags=%u count=%d\n", pair.first, pair.second);
	if (flagProbe) FlagProbe(file, *map);
	if (axisProbe > 0) AxisProbe(records, *map, axisProbe);

	// 시간은 대조 계산이 섞이지 않게 따로 잰다
	volatile size_t sink = 0;
	size_t rayCount = 0, sweepCount = 0;
	auto start = std::chrono::steady_clock::now();
	for (const Record& rec : records)
	{
		if (rec.kind == 'C' || rec.kind == 'F' || rec.kind == 'A') continue;
		mo::RayHit hit;
		sink = sink + (map->Raycast(rec.a, rec.b, mo::MAP_BLOCK_WEAPON, hit) ? 1 : 0);
		++rayCount;
	}
	const double raySec = Seconds(start);
	start = std::chrono::steady_clock::now();
	for (const Record& rec : records)
	{
		if (!(rec.kind == 'C' || rec.kind == 'F' || rec.kind == 'A')) continue;
		mo::CapsuleHit hit;
		sink = sink + (map->SweepCapsule(rec.a, rec.b, radius, halfHeight, mo::MAP_BLOCK_MOVE, hit) ? 1 : 0);
		++sweepCount;
	}
	const double sweepSec = Seconds(start);
	std::printf("TIME rays=%zu avg=%.3fus | capsules=%zu avg=%.3fus (sink=%zu)\n",
		rayCount, rayCount ? raySec / rayCount * 1e6 : 0, sweepCount, sweepCount ? sweepSec / sweepCount * 1e6 : 0, (size_t)sink);
	return 0;
}
