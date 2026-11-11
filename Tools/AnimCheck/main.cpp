// 클립 자료 대조 도구 (candidate1-impl-plan.md 1단계)
//
// 사용: AnimCheck clips <clips.bin> [--dump=<out.csv>]
//   파일을 읽어 ANIMCLIPS 줄을 낸다. --dump 를 주면 읽은 값을 한 줄씩 다시 쓴다.
//   Tools/LyraPilot/check_clip_pack.py 가 그 파일과 뽑을 때 쓴 csv 를 전수 대조한다.
//
// 사용: AnimCheck pose <clips.bin> <animcmp_<Tag>.csv> <out.csv> [--shift=N] [--swaplr] [--noHandIK]
//   언리얼이 프레임마다 쓴 자세 입력으로 22부위를 계산해 POSE 줄(언리얼 기록과 같은 형식)을 낸다 (2단계).
//   check_pose.py <animcmp_<Tag>_shapes.csv> <out.csv> --refTag=POSEB 로 대조한다.
//   --shift / --swaplr / --noHandIK 는 대조 도구가 틀림을 잡는지 보는 일부러 틀림이다.
//
// 사용: AnimCheck state <clips.bin> <animcmp_<Tag>.csv> <animcmp_<Tag>_moves.csv> <out.csv> [--brakeScale=배율] [--groundAdd=cm] [--wire]
//   언리얼이 프레임마다 쓴 입력만으로 상태를 만들어 언리얼 상태 칸과 비교하고, 그 상태로 22부위를 계산해 POSE 줄을 낸다.
//   판정은 pose 와 같은 check_pose.py. --brakeScale / --groundAdd 는 일부러 틀림이다.
//   --wire 는 상태를 패킷 형식으로 한 번 왕복시킨 값으로 자세를 계산한다 — 패킷 눈금이 언리얼 대비 얼마를 더하는지 잰다.

#include "AnimClips.h"
#include "AnimPose.h"
#include "AnimState.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace mo;

namespace
{
	const char* LoadName(EClipLoad result)
	{
		switch (result)
		{
		case EClipLoad::Ok: return "Ok";
		case EClipLoad::TooSmall: return "TooSmall";
		case EClipLoad::BadMagic: return "BadMagic";
		case EClipLoad::BadVersion: return "BadVersion";
		case EClipLoad::Truncated: return "Truncated";
		case EClipLoad::BadBone: return "BadBone";
		case EClipLoad::BadBody: return "BadBody";
		case EClipLoad::BadBundle: return "BadBundle";
		case EClipLoad::BadClip: return "BadClip";
		case EClipLoad::BadTail: return "BadTail";
		}
		return "?";
	}

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

	// float 한 칸을 그대로 되돌릴 수 있는 자릿수(%.9g)로 쓴다. 대조는 float 비트로 한다
	void WriteKey(FILE* file, const BoneKey& key)
	{
		std::fprintf(file, ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", key.p[0], key.p[1], key.p[2], key.q[0], key.q[1], key.q[2], key.q[3]);
	}

	bool Dump(const CAnimClips& clips, const char* path)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path, "wb") != 0 || !file) return false;

		const std::vector<ClipBone>& bones = clips.Bones();
		for (size_t b = 0; b < bones.size(); ++b)
		{
			std::fprintf(file, "BONE,%zu,%s,%s", b, bones[b].name.c_str(), bones[b].parent < 0 ? "-" : bones[(size_t)bones[b].parent].name.c_str());
			WriteKey(file, bones[b].ref);
		}
		std::fprintf(file, "MESH");
		WriteKey(file, clips.MeshPlacement());
		for (const ClipBody& body : clips.Bodies())
		{
			std::fprintf(file, "BODY,%s,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", bones[(size_t)body.bone].name.c_str(), (int)body.kind, body.weakSpot ? 1 : 0,
				body.center[0], body.center[1], body.center[2], body.q[0], body.q[1], body.q[2], body.q[3], body.a, body.b, body.c);
		}
		for (const ClipBundle& bundle : clips.Bundles())
		{
			std::fprintf(file, "BUNDLE,%s,%u\n", bundle.name.c_str(), bundle.sampleHz);
			std::fprintf(file, "AIMPITCH,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", bundle.aimPitch[0], bundle.aimPitch[1], bundle.aimPitch[2], bundle.aimPitch[3], bundle.aimPitch[4], bundle.aimPitch[5]);
			std::fprintf(file, "MOVE,groundFriction,%.9g,brakingFriction,%.9g,brakingFrictionFactor,%.9g,brakingDecelerationWalking,%.9g,gravityZ,%.9g,separateBraking,%d\n",
				bundle.groundFriction, bundle.brakingFriction, bundle.brakingFrictionFactor, bundle.brakingDecelerationWalking, bundle.gravityZ, bundle.separateBraking ? 1 : 0);
			for (size_t c = 0; c < bundle.clips.size(); ++c)
			{
				const AnimClip& clip = bundle.clips[c];
				std::fprintf(file, "CLIP,%zu,%s,%.9g,%u,%d,%.9g,%d\n", c, clip.name.c_str(), clip.length, clip.samples, (int)clip.additive, clip.rootSpeed, clip.loop ? 1 : 0);
				for (const ClipMarker& marker : clip.markers) std::fprintf(file, "MARK,%zu,%.9g,%s\n", c, marker.time, marker.left ? "L" : "R");
				if (clip.hasCurve)
				{
					std::fprintf(file, "CURVE,%zu,%u,%zu", c, clip.curveHz, clip.curve.size());
					for (const float value : clip.curve) std::fprintf(file, ",%.9g", value);
					std::fprintf(file, "\n");
				}
				for (uint32_t s = 0; s < clip.samples; ++s)
				{
					const BoneKey* keys = clip.Keys(s, (uint32_t)bones.size());
					for (size_t b = 0; b < bones.size(); ++b)
					{
						std::fprintf(file, "KEY,%zu,%u,%s", c, s, bones[b].name.c_str());
						WriteKey(file, keys[b]);
					}
				}
			}
		}
		std::fclose(file);
		return true;
	}

	int RunClips(int argc, char** argv)
	{
		if (argc < 3)
		{
			std::printf("사용: AnimCheck clips <clips.bin> [--dump=<out.csv>]\n");
			return 2;
		}
		std::string dumpPath;
		for (int i = 3; i < argc; ++i)
		{
			if (std::strncmp(argv[i], "--dump=", 7) == 0) dumpPath = argv[i] + 7;
			else
			{
				std::printf("[오류] 모르는 인자: %s\n", argv[i]);
				return 2;
			}
		}

		std::vector<char> data;
		if (!ReadFile(argv[2], data))
		{
			std::printf("ANIMCLIPS FAIL 파일을 못 읽음: %s\n", argv[2]);
			return 1;
		}
		CAnimClips clips;
		const EClipLoad result = clips.LoadFromMemory(data.data(), data.size());
		if (result != EClipLoad::Ok)
		{
			std::printf("ANIMCLIPS FAIL result=%s\n", LoadName(result));
			return 1;
		}

		for (const ClipBundle& bundle : clips.Bundles())
		{
			uint64_t samples = 0;
			double seconds = 0;
			for (const AnimClip& clip : bundle.clips)
			{
				samples += clip.samples;
				seconds += clip.length;
			}
			std::printf("ANIMCLIPS loaded=%zu bundle=%s bones=%zu hz=%u samples=%llu seconds=%.2f keyBytes=%zu keyMB=%.2f\n",
				bundle.clips.size(), bundle.name.c_str(), clips.Bones().size(), bundle.sampleHz, (unsigned long long)samples, seconds, clips.KeyBytes(), clips.KeyBytes() / (1024.0 * 1024.0));
		}

		if (!dumpPath.empty() && !Dump(clips, dumpPath.c_str()))
		{
			std::printf("ANIMCLIPS FAIL 덤프를 못 씀: %s\n", dumpPath.c_str());
			return 1;
		}
		return 0;
	}

	std::vector<std::string> SplitComma(const std::string& line)
	{
		std::vector<std::string> out;
		size_t start = 0;
		for (;;)
		{
			const size_t comma = line.find(',', start);
			if (comma == std::string::npos)
			{
				out.push_back(line.substr(start));
				return out;
			}
			out.push_back(line.substr(start, comma - start));
			start = comma + 1;
		}
	}

	struct PoseRow
	{
		int pass = 0;
		int frame = 0;
		int part = 0;
		int reset = -1;			// 이 줄의 상태 = 이 판(reset)의 입력을 seq 번호까지 넣은 결과
		int seq = -1;
		AnimPoseInput input;
	};

	// 칸 이름을 번호로. 없으면 -1
	int Column(const std::vector<std::string>& header, const std::string& name)
	{
		for (size_t i = 0; i < header.size(); ++i)
		{
			if (header[i] == name) return (int)i;
		}
		return -1;
	}

	bool ReadPoseRows(const char* path, std::vector<PoseRow>& out)
	{
		std::vector<char> data;
		if (!ReadFile(path, data))
		{
			std::printf("POSEOUT FAIL 상태 파일을 못 읽음: %s\n", path);
			return false;
		}
		std::string text(data.begin(), data.end());
		size_t pos = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;

		std::vector<std::string> header;
		std::vector<std::string> segments;
		int cPass = -1, cFrame = -1, cSegment = -1, cFidget = -1, cRecovery = -1, cAimBlend = -1, cPitch = -1, cLayers = -1, cReset = -1, cSeq = -1;
		int cWeight[kAnimWalkClips] = {};
		int cTime[kAnimWalkClips] = {};
		int cPlant[3] = {};
		int cLayerClip[kAnimLayerMax] = {};
		int cLayerTime[kAnimLayerMax] = {};
		int cLayerFade[kAnimLayerMax] = {};
		size_t lineNumber = 0;
		while (pos < text.size())
		{
			size_t end = text.find('\n', pos);
			if (end == std::string::npos) end = text.size();
			std::string line = text.substr(pos, end - pos);
			pos = end + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()) continue;
			++lineNumber;

			std::vector<std::string> cells = SplitComma(line);
			if (header.empty())
			{
				header = std::move(cells);
				const auto need = [&](const std::string& name, int& where)
				{
					where = Column(header, name);
					if (where < 0) std::printf("POSEOUT FAIL 칸이 없음: %s\n", name.c_str());
					return where >= 0;
				};
				bool ok = need("pass", cPass) && need("frame", cFrame) && need("segment", cSegment) && need("fidget", cFidget) && need("recovery", cRecovery)
					&& need("aimBlend", cAimBlend) && need("pitch", cPitch) && need("layers", cLayers) && need("plantX", cPlant[0]) && need("plantY", cPlant[1]) && need("plantZ", cPlant[2]);
				for (int i = 0; ok && i < kAnimWalkClips; ++i) ok = need("w" + std::to_string(i), cWeight[i]) && need("t" + std::to_string(i), cTime[i]);
				for (int i = 0; ok && i < kAnimLayerMax; ++i) ok = need("c" + std::to_string(i), cLayerClip[i]) && need("tm" + std::to_string(i), cLayerTime[i]) && need("f" + std::to_string(i), cLayerFade[i]);
				if (!ok) return false;
				cReset = Column(header, "reset");		// 입력 기록이 든 판에만 있다. 없으면 state 쪽에서 거부한다
				cSeq = Column(header, "seq");
				continue;
			}
			if (cells.size() != header.size())
			{
				std::printf("POSEOUT FAIL 줄 %zu 의 칸 수 %zu, 머리 %zu\n", lineNumber, cells.size(), header.size());
				return false;
			}

			const auto number = [&](int column) { return std::strtof(cells[(size_t)column].c_str(), nullptr); };
			PoseRow row;
			row.pass = std::atoi(cells[(size_t)cPass].c_str());
			row.frame = std::atoi(cells[(size_t)cFrame].c_str());
			if (cReset >= 0 && cSeq >= 0)
			{
				row.reset = std::atoi(cells[(size_t)cReset].c_str());
				row.seq = std::atoi(cells[(size_t)cSeq].c_str());
			}
			const std::string& segment = cells[(size_t)cSegment];
			int segmentIndex = -1;
			for (size_t i = 0; i < segments.size(); ++i)
			{
				if (segments[i] == segment) segmentIndex = (int)i;
			}
			if (segmentIndex < 0)
			{
				segmentIndex = (int)segments.size();
				segments.push_back(segment);
			}
			row.part = row.pass * 100 + segmentIndex;		// 언리얼 쪽 둘째 칸 = 판*100+경로 조각 번호

			AnimPoseInput& input = row.input;
			for (int i = 0; i < kAnimWalkClips; ++i)
			{
				input.weights[i] = number(cWeight[i]);
				input.times[i] = number(cTime[i]);
			}
			input.fidgetTime = number(cFidget);
			input.recoveryTime = number(cRecovery);
			input.aimBlend = number(cAimBlend);
			input.pitch = number(cPitch);
			for (int c = 0; c < 3; ++c) input.plantOffset[c] = number(cPlant[c]);
			input.layerCount = std::atoi(cells[(size_t)cLayers].c_str());
			if (input.layerCount < 0 || input.layerCount > kAnimLayerMax)
			{
				std::printf("POSEOUT FAIL 줄 %zu layers=%d\n", lineNumber, input.layerCount);
				return false;
			}
			for (int i = 0; i < kAnimLayerMax; ++i)
			{
				input.layers[i].clip = (int16_t)std::atoi(cells[(size_t)cLayerClip[i]].c_str());
				input.layers[i].time = number(cLayerTime[i]);
				input.layers[i].fade = number(cLayerFade[i]);
			}
			out.push_back(row);
		}
		return true;
	}

	int RunPose(int argc, char** argv)
	{
		if (argc < 5)
		{
			std::printf("사용: AnimCheck pose <clips.bin> <animcmp_<Tag>.csv> <out.csv> [--shift=N] [--swaplr] [--noHandIK]\n");
			return 2;
		}
		int shift = 0;
		bool swapLR = false;
		bool noHandIK = false;
		for (int i = 5; i < argc; ++i)
		{
			if (std::strncmp(argv[i], "--shift=", 8) == 0) shift = std::atoi(argv[i] + 8);
			else if (std::strcmp(argv[i], "--swaplr") == 0) swapLR = true;
			else if (std::strcmp(argv[i], "--noHandIK") == 0) noHandIK = true;
			else
			{
				std::printf("[오류] 모르는 인자: %s\n", argv[i]);
				return 2;
			}
		}

		std::vector<char> data;
		if (!ReadFile(argv[2], data))
		{
			std::printf("POSEOUT FAIL 클립 파일을 못 읽음: %s\n", argv[2]);
			return 1;
		}
		CAnimClips clips;
		const EClipLoad result = clips.LoadFromMemory(data.data(), data.size());
		if (result != EClipLoad::Ok)
		{
			std::printf("POSEOUT FAIL result=%s\n", LoadName(result));
			return 1;
		}
		CAnimPoseEvaluator evaluator;
		if (!evaluator.Init(clips, "rifle"))
		{
			std::printf("POSEOUT FAIL 자세 계산기를 못 만듦\n");
			return 1;
		}
		if (noHandIK) evaluator.SetHandIKEnabledForTest(false);

		std::vector<PoseRow> rows;
		if (!ReadPoseRows(argv[3], rows)) return 1;
		if (rows.empty())
		{
			std::printf("POSEOUT FAIL 상태 줄 0\n");
			return 1;
		}

		// 일부러 틀림 — 파일럿 1의 pose_calc.py 와 같은 뜻. 밀기는 같은 판 안에서 N프레임 뒤의 걸음 가중치 · 시각을 쓴다(판 끝은 그대로)
		std::vector<AnimPoseInput> inputs;
		inputs.reserve(rows.size());
		for (size_t i = 0; i < rows.size(); ++i)
		{
			AnimPoseInput input = rows[i].input;
			if (shift != 0)
			{
				const size_t other = std::min(i + (size_t)shift, rows.size() - 1);
				if (rows[other].pass == rows[i].pass)
				{
					std::memcpy(input.weights, rows[other].input.weights, sizeof(input.weights));
					std::memcpy(input.times, rows[other].input.times, sizeof(input.times));
				}
			}
			if (swapLR) std::swap(input.weights[3], input.weights[4]);
			inputs.push_back(input);
		}

		FILE* file = nullptr;
		if (fopen_s(&file, argv[4], "wb") != 0 || !file)
		{
			std::printf("POSEOUT FAIL 출력을 못 씀: %s\n", argv[4]);
			return 1;
		}

		static const char* const kKinds[] = { "sphere", "sphyl", "box" };
		const std::vector<ClipBone>& bones = clips.Bones();
		const std::vector<ClipBody>& bodies = clips.Bodies();
		std::vector<BoneTransform> local(evaluator.BoneCount());
		std::vector<BodyPose> poses(evaluator.BodyCount());
		double evaluateSeconds = 0;
		double bodiesSeconds = 0;
		size_t lines = 0;
		for (size_t i = 0; i < rows.size(); ++i)
		{
			const auto t0 = std::chrono::steady_clock::now();
			evaluator.EvaluateLocal(inputs[i], local.data());
			const auto t1 = std::chrono::steady_clock::now();
			evaluator.BodiesFromLocal(local.data(), poses.data());
			const auto t2 = std::chrono::steady_clock::now();
			evaluateSeconds += std::chrono::duration<double>(t1 - t0).count();
			bodiesSeconds += std::chrono::duration<double>(t2 - t1).count();

			for (size_t b = 0; b < bodies.size(); ++b)
			{
				const ClipBody& body = bodies[b];
				const BodyPose& pose = poses[b];
				std::fprintf(file, "POSE,%d,%d,%s,%s,%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f\n",
					rows[i].part, rows[i].frame, bones[(size_t)body.bone].name.c_str(), kKinds[(int)body.kind], body.weakSpot ? 1 : 0,
					pose.center[0], pose.center[1], pose.center[2],
					pose.axisX[0], pose.axisX[1], pose.axisX[2], pose.axisY[0], pose.axisY[1], pose.axisY[2], pose.axisZ[0], pose.axisZ[1], pose.axisZ[2],
					body.a, body.b, body.c);
				++lines;
			}
		}
		std::fclose(file);

		std::printf("POSEOUT rows=%zu bodies=%zu lines=%zu shift=%d swaplr=%d noHandIK=%d\n", rows.size(), bodies.size(), lines, shift, swapLR ? 1 : 0, noHandIK ? 1 : 0);
		std::printf("POSETIME evaluateUs=%.2f bodiesUs=%.2f totalUs=%.2f\n", evaluateSeconds * 1e6 / rows.size(), bodiesSeconds * 1e6 / rows.size(), (evaluateSeconds + bodiesSeconds) * 1e6 / rows.size());
		return 0;
	}

	struct MoveRow
	{
		int reset = 0;
		int seq = 0;
		AnimMoveInput input;
	};

	bool ReadMoves(const char* path, std::vector<MoveRow>& out)
	{
		std::vector<char> data;
		if (!ReadFile(path, data))
		{
			std::printf("STATE FAIL 입력 파일을 못 읽음: %s\n", path);
			return false;
		}
		std::string text(data.begin(), data.end());
		size_t pos = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;

		static const char* const kNames[] = { "reset", "seq", "dt", "vx", "vy", "vz", "ax", "ay", "az", "accel", "falling", "pitch", "ground", "hasGap", "gap" };
		enum { R, S, DT, VX, VY, VZ, AX, AY, AZ, ACCEL, FALLING, PITCH, GROUND, HASGAP, GAP, COUNT };
		int columns[COUNT] = {};
		std::vector<std::string> header;
		size_t lineNumber = 0;
		while (pos < text.size())
		{
			size_t end = text.find('\n', pos);
			if (end == std::string::npos) end = text.size();
			std::string line = text.substr(pos, end - pos);
			pos = end + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()) continue;
			++lineNumber;

			std::vector<std::string> cells = SplitComma(line);
			if (header.empty())
			{
				header = std::move(cells);
				for (int i = 0; i < COUNT; ++i)
				{
					columns[i] = Column(header, kNames[i]);
					if (columns[i] < 0)
					{
						std::printf("STATE FAIL 입력 칸이 없음: %s\n", kNames[i]);
						return false;
					}
				}
				continue;
			}
			if (cells.size() != header.size())
			{
				std::printf("STATE FAIL 입력 줄 %zu 의 칸 수 %zu, 머리 %zu\n", lineNumber, cells.size(), header.size());
				return false;
			}
			const auto text2 = [&](int k) { return cells[(size_t)columns[k]].c_str(); };
			MoveRow row;
			row.reset = std::atoi(text2(R));
			row.seq = std::atoi(text2(S));
			row.input.deltaSeconds = std::strtof(text2(DT), nullptr);
			for (int c = 0; c < 3; ++c)
			{
				row.input.velocity[c] = std::strtod(text2(VX + c), nullptr);
				row.input.acceleration[c] = std::strtod(text2(AX + c), nullptr);
			}
			row.input.accelerating = std::atoi(text2(ACCEL)) != 0;
			row.input.falling = std::atoi(text2(FALLING)) != 0;
			row.input.pitch = std::strtof(text2(PITCH), nullptr);
			row.input.groundDistance = std::strtof(text2(GROUND), nullptr);
			row.input.hasFloorGap = std::atoi(text2(HASGAP)) != 0;
			row.input.plantGap = std::strtof(text2(GAP), nullptr);
			out.push_back(row);
		}
		return true;
	}

	int HeaviestWalk(const AnimPoseInput& input)
	{
		int best = 0;
		for (int i = 1; i < kAnimWalkClips; ++i)
		{
			if (input.weights[i] > input.weights[best]) best = i;
		}
		return best;
	}

	// 차이의 최대와 처음 기준을 넘은 줄을 함께 남긴다 — 어긋남은 대개 한 곳에서 시작해 뒤로 번지므로 처음 자리가 원인에 가깝다
	struct DiffColumn
	{
		const char* name = "";
		float threshold = 0.f;
		float max = 0.f;
		size_t maxRow = 0;
		size_t over = 0;
		bool hasFirst = false;
		size_t firstRow = 0;
		float firstRef = 0.f;
		float firstGot = 0.f;

		void Add(float ref, float got, size_t row)
		{
			const float diff = std::fabs(ref - got);
			if (diff > max)
			{
				max = diff;
				maxRow = row;
			}
			if (diff > threshold)
			{
				++over;
				if (!hasFirst)
				{
					hasFirst = true;
					firstRow = row;
					firstRef = ref;
					firstGot = got;
				}
			}
		}
	};

	int RunState(int argc, char** argv)
	{
		if (argc < 6)
		{
			std::printf("사용: AnimCheck state <clips.bin> <animcmp_<Tag>.csv> <animcmp_<Tag>_moves.csv> <out.csv> [--brakeScale=배율] [--groundAdd=cm] [--wire]\n");
			return 2;
		}
		float brakeScale = 1.f;
		float groundAdd = 0.f;
		bool useWire = false;
		double maxReach = 0;
		for (int i = 6; i < argc; ++i)
		{
			if (std::strncmp(argv[i], "--brakeScale=", 13) == 0) brakeScale = std::strtof(argv[i] + 13, nullptr);
			else if (std::strncmp(argv[i], "--groundAdd=", 12) == 0) groundAdd = std::strtof(argv[i] + 12, nullptr);
			else if (std::strcmp(argv[i], "--wire") == 0) useWire = true;
			else
			{
				std::printf("[오류] 모르는 인자: %s\n", argv[i]);
				return 2;
			}
		}

		std::vector<char> data;
		if (!ReadFile(argv[2], data))
		{
			std::printf("STATE FAIL 클립 파일을 못 읽음: %s\n", argv[2]);
			return 1;
		}
		CAnimClips clips;
		const EClipLoad result = clips.LoadFromMemory(data.data(), data.size());
		if (result != EClipLoad::Ok)
		{
			std::printf("STATE FAIL result=%s\n", LoadName(result));
			return 1;
		}
		CAnimPoseEvaluator evaluator;
		CAnimState state;
		if (!evaluator.Init(clips, "rifle") || !state.Init(clips, "rifle"))
		{
			std::printf("STATE FAIL 계산기를 못 만듦\n");
			return 1;
		}
		if (brakeScale != 1.f) state.SetBrakingScaleForTest(brakeScale);

		std::vector<PoseRow> rows;
		std::vector<MoveRow> moves;
		if (!ReadPoseRows(argv[3], rows) || !ReadMoves(argv[4], moves)) return 1;
		if (rows.empty() || moves.empty())
		{
			std::printf("STATE FAIL 줄 0 rows=%zu moves=%zu\n", rows.size(), moves.size());
			return 1;
		}

		// 판별 입력을 번호 순으로 묶는다. 번호가 비면 어느 입력이 빠졌는지 모른 채 상태가 어긋나므로 거부한다
		std::map<int, std::vector<const MoveRow*>> byReset;
		for (const MoveRow& move : moves)
		{
			std::vector<const MoveRow*>& list = byReset[move.reset];
			if (move.seq != (int)list.size())
			{
				std::printf("STATE FAIL reset=%d 입력 번호가 이어지지 않음: %d, 기대 %zu\n", move.reset, move.seq, list.size());
				return 1;
			}
			list.push_back(&move);
		}

		FILE* file = nullptr;
		if (fopen_s(&file, argv[5], "wb") != 0 || !file)
		{
			std::printf("STATE FAIL 출력을 못 씀: %s\n", argv[5]);
			return 1;
		}

		static const char* const kKinds[] = { "sphere", "sphyl", "box" };
		const std::vector<ClipBone>& bones = clips.Bones();
		const std::vector<ClipBody>& bodies = clips.Bodies();
		std::vector<BoneTransform> local(evaluator.BoneCount());
		std::vector<BodyPose> poses(evaluator.BodyCount());

		DiffColumn weights{ "weights", 0.001f };
		DiffColumn times{ "times", 0.001f };
		DiffColumn fidget{ "fidget", 0.001f };
		DiffColumn recovery{ "recovery", 0.001f };
		DiffColumn aimBlend{ "aimBlend", 0.001f };
		DiffColumn pitch{ "pitch", 0.001f };
		DiffColumn plant{ "plantZ", 0.001f };
		DiffColumn layerCount{ "layerCount", 0.5f };
		DiffColumn layerClip{ "layerClip", 0.5f };
		DiffColumn layerTime{ "layerTime", 0.001f };
		DiffColumn layerFade{ "layerFade", 0.001f };
		DiffColumn* const columns[] = { &weights, &times, &fidget, &recovery, &aimBlend, &pitch, &plant, &layerCount, &layerClip, &layerTime, &layerFade };

		size_t sameWalk = 0;
		size_t sameTop = 0;
		size_t sameBoth = 0;
		size_t tieWalk = 0;
		size_t realWalk = 0;
		size_t fed = 0;
		size_t lines = 0;
		int currentReset = -1;
		int nextSeq = 0;
		double updateSeconds = 0;
		for (size_t i = 0; i < rows.size(); ++i)
		{
			const PoseRow& row = rows[i];
			if (row.reset < 0 || row.seq < 0)
			{
				std::printf("STATE FAIL 줄 %zu 에 reset · seq 가 없음\n", i);
				std::fclose(file);
				return 1;
			}
			if (row.reset != currentReset)
			{
				state.Reset();
				currentReset = row.reset;
				nextSeq = 0;
			}
			const auto found = byReset.find(row.reset);
			if (found == byReset.end() || row.seq >= (int)found->second.size() || row.seq + 1 < nextSeq)
			{
				std::printf("STATE FAIL 줄 %zu reset=%d seq=%d 입력 없음 또는 역행(다음 %d)\n", i, row.reset, row.seq, nextSeq);
				std::fclose(file);
				return 1;
			}
			const auto t0 = std::chrono::steady_clock::now();
			for (; nextSeq <= row.seq; ++nextSeq)
			{
				AnimMoveInput input = found->second[(size_t)nextSeq]->input;
				input.groundDistance += groundAdd;
				state.Update(input);
				++fed;
			}
			updateSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

			AnimPoseInput got;
			state.ToPoseInput(got);
			if (useWire)
			{
				wire::AnimStateWire onWire{};
				state.ToWire(onWire);
				state.FromWire(onWire, got.pitch, got);
			}
			const AnimPoseInput& ref = row.input;
			for (int k = 0; k < kAnimWalkClips; ++k)
			{
				weights.Add(ref.weights[k], got.weights[k], i);
				// 시각은 비중이 있는 클립만 본다. 비중 0인 클립의 시각은 자세에 안 들어간다
				if (ref.weights[k] > 0.01f) times.Add(ref.times[k], got.times[k], i);
			}
			fidget.Add(ref.fidgetTime, got.fidgetTime, i);
			recovery.Add(ref.recoveryTime, got.recoveryTime, i);
			aimBlend.Add(ref.aimBlend, got.aimBlend, i);
			pitch.Add(ref.pitch, got.pitch, i);
			plant.Add(ref.plantOffset[2], got.plantOffset[2], i);
			layerCount.Add((float)ref.layerCount, (float)got.layerCount, i);
			const int common = std::min(ref.layerCount, got.layerCount);
			for (int k = 0; k < common; ++k)
			{
				layerClip.Add((float)ref.layers[k].clip, (float)got.layers[k].clip, i);
				layerTime.Add(ref.layers[k].time, got.layers[k].time, i);
				layerFade.Add(ref.layers[k].fade, got.layers[k].fade, i);
			}

			// 대각선처럼 두 클립 비중이 같으면 부동소수 끝자리로 1등이 갈린다. 그 동률은 따로 센다
			const int heaviestRef = HeaviestWalk(ref);
			const int heaviestGot = HeaviestWalk(got);
			const bool walk = heaviestRef == heaviestGot;
			if (!walk)
			{
				if (std::fabs(ref.weights[heaviestRef] - ref.weights[heaviestGot]) <= 1e-4f) ++tieWalk;
				else ++realWalk;
			}
			const bool top = ref.layerCount == got.layerCount && (ref.layerCount == 0 || ref.layers[ref.layerCount - 1].clip == got.layers[got.layerCount - 1].clip);
			if (walk) ++sameWalk;
			if (top) ++sameTop;
			if (walk && top) ++sameBoth;

			evaluator.EvaluateLocal(got, local.data());
			evaluator.BodiesFromLocal(local.data(), poses.data());
			for (size_t b = 0; b < bodies.size(); ++b)
			{
				const ClipBody& body = bodies[b];
				const BodyPose& pose = poses[b];
				maxReach = std::max(maxReach, CAnimPoseEvaluator::BodyReach(body, pose));
				std::fprintf(file, "POSE,%d,%d,%s,%s,%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f\n",
					row.part, row.frame, bones[(size_t)body.bone].name.c_str(), kKinds[(int)body.kind], body.weakSpot ? 1 : 0,
					pose.center[0], pose.center[1], pose.center[2],
					pose.axisX[0], pose.axisX[1], pose.axisX[2], pose.axisY[0], pose.axisY[1], pose.axisY[2], pose.axisZ[0], pose.axisZ[1], pose.axisZ[2],
					body.a, body.b, body.c);
				++lines;
			}
		}
		std::fclose(file);

		const double count = (double)rows.size();
		std::printf("STATEOUT rows=%zu moves=%zu fed=%zu lines=%zu resets=%zu brakeScale=%.3f groundAdd=%.1f overflows=%d\n", rows.size(), moves.size(), fed, lines, byReset.size(), brakeScale, groundAdd, state.StackOverflows());
		std::printf("STATETIME updateUs=%.3f\n", fed > 0 ? updateSeconds * 1e6 / (double)fed : 0.0);
		std::printf("STATEBOUND reach=%.3f bound=%.3f wire=%d\n", maxReach, evaluator.ComputeBoundRadius(), useWire ? 1 : 0);
		std::printf("STATESAME heaviestWalk=%.4f%% topLayer=%.4f%% both=%.4f%% walkTieRows=%zu walkRealDiffRows=%zu\n", sameWalk * 100.0 / count, sameTop * 100.0 / count, sameBoth * 100.0 / count, tieWalk, realWalk);
		for (const DiffColumn* column : columns)
		{
			std::printf("STATEDIFF %s max=%.6f over=%zu", column->name, column->max, column->over);
			if (column->hasFirst)
			{
				const PoseRow& first = rows[column->firstRow];
				std::printf(" first=(pass=%d frame=%d part=%d seq=%d ref=%.6f got=%.6f)", first.pass, first.frame, first.part, first.seq, column->firstRef, column->firstGot);
			}
			std::printf("\n");
		}
		return 0;
	}
}

int main(int argc, char** argv)
{
	if (argc >= 2 && std::strcmp(argv[1], "clips") == 0) return RunClips(argc, argv);
	if (argc >= 2 && std::strcmp(argv[1], "pose") == 0) return RunPose(argc, argv);
	if (argc >= 2 && std::strcmp(argv[1], "state") == 0) return RunState(argc, argv);
	std::printf("사용: AnimCheck clips <clips.bin> [--dump=<out.csv>]\n");
	std::printf("      AnimCheck pose <clips.bin> <animcmp_<Tag>.csv> <out.csv> [--shift=N] [--swaplr] [--noHandIK]\n");
	std::printf("      AnimCheck state <clips.bin> <animcmp_<Tag>.csv> <animcmp_<Tag>_moves.csv> <out.csv> [--brakeScale=배율] [--groundAdd=cm] [--wire]\n");
	return 2;
}
