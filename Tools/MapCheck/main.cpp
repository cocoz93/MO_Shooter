// Lyra 가 기록한 광선·캡슐 결과를 Shared/MapCollision 으로 다시 돌려 대조한다.
// - 왜: 「맞췄다」의 근거는 Lyra 실행값뿐이다. 맵 파일과 기록은 Tools/LyraPilot/record_traces.ps1 이 만든다.
//
//   MapCheck <map.mocl> <trace.csv> [--kill-first-hit] [--shift-z <cm>] [--mismatch <출력.csv>]
//     --kill-first-hit  Lyra 가 맞았다는 첫 광선의 삼각형을 파일에서 지우고 돌린다(시험의 시험 — 불일치가 늘어야 한다)
//     --shift-z         맵을 위로 옮기고 돌린다(시험의 시험 — 거리 차가 커져야 한다)
//     --capsule-pad     캡슐을 살짝 키워 돌린다(진단용 — 스치는 접촉의 허용치를 잰다)
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
	for (int i = 3; i < argc; ++i)
	{
		if (!std::strcmp(argv[i], "--kill-first-hit")) killFirstHit = true;
		else if (!std::strcmp(argv[i], "--shift-z") && i + 1 < argc) shift = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--mismatch") && i + 1 < argc) mismatchPath = argv[++i];
		else if (!std::strcmp(argv[i], "--capsule-pad") && i + 1 < argc) capsulePad = std::atof(argv[++i]);
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
	std::printf("TREE weapon tris=%zu nodes=%zu | move tris=%zu nodes=%zu\n",
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
