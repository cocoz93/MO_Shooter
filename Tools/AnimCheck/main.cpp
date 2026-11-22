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
//   --wireKeepNew 는 --wire 에서 프로토콜 5 에 더한 칸(몽타주 · 덮기 · 왼손)만 바로 값으로 둔다 — 새 칸의 눈금 몫을 가른다(9단계 조각 4).
//
// 사용: AnimCheck fall <clips.bin> [--seconds=초]
//   묶음마다 1초 서 있다 그 초만큼(기본 12) 떨어지는 입력을 만들어, 갱신마다 쌓기 칸을 패킷 형식으로 한 번 왕복시킨다.
//   반복 클립 칸이 바로 값과 같은 클립 자리(길이로 감은 시각)를 가리키는지 센다 — stuck 이 0 이 아니면 FAIL(L-13-b4).
//   기록 낙하는 1초 안이라 패킷 시각 한도(10.9초)에 못 닿아 입력을 만든다. loopRows = 지나간 반복 칸 수(0 이면 못 본 것이라 FAIL)

#include "AnimClips.h"
#include "AnimPose.h"
#include "AnimState.h"
#include "Movement.h"

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
		case EClipLoad::BadSlot: return "BadSlot";
		case EClipLoad::BadMontage: return "BadMontage";
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
		// 뼈별 비중 표 · 끼움 자리(9단계 3-a) — 비중은 뼈 차례 그대로
		const std::vector<ClipProfile>& profiles = clips.Profiles();
		for (size_t p = 0; p < profiles.size(); ++p)
		{
			std::fprintf(file, "PROFILE,%zu,%s,%d", p, profiles[p].name.c_str(), (int)profiles[p].mode);
			for (const float weight : profiles[p].weights) std::fprintf(file, ",%.9g", weight);
			std::fprintf(file, "\n");
		}
		const std::vector<ClipSlot>& slots = clips.Slots();
		for (size_t s = 0; s < slots.size(); ++s)
		{
			std::fprintf(file, "SLOT,%zu,%s,%d,%s\n", s, slots[s].name.c_str(), (int)slots[s].mode, slots[s].profile < 0 ? "-" : profiles[(size_t)slots[s].profile].name.c_str());
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
				std::fprintf(file, "CLIP,%zu,%s,%.9g,%u,%d,%.9g,%d,%.9g\n", c, clip.name.c_str(), clip.length, clip.samples, (int)clip.additive, clip.rootSpeed, clip.loop ? 1 : 0, clip.rate);
				for (const ClipMarker& marker : clip.markers) std::fprintf(file, "MARK,%zu,%.9g,%s\n", c, marker.time, marker.left ? "L" : "R");
				if (clip.hasCurve)
				{
					std::fprintf(file, "CURVE,%zu,%u,%zu", c, clip.curveHz, clip.curve.size());
					for (const float value : clip.curve) std::fprintf(file, ",%.9g", value);
					std::fprintf(file, "\n");
				}
				if (clip.hasLegCurve)
				{
					std::fprintf(file, "LCURVE,%zu,%u,%zu", c, clip.legCurveHz, clip.legCurve.size());
					for (const float value : clip.legCurve) std::fprintf(file, ",%.9g", value);
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
			for (const ClipMontage& montage : bundle.montages)
			{
				std::fprintf(file, "MONTAGE,%d,%s,%s,%.9g,%.9g,%.9g,%d,%.9g,%d,%.9g,%d,%d\n", (int)montage.kind, montage.name.c_str(), montage.group.c_str(), montage.length, montage.rate,
					montage.blendIn, (int)montage.blendInOption, montage.blendOut, (int)montage.blendOutOption, montage.blendOutTrigger, montage.autoBlendOut ? 1 : 0, (int)montage.blendOutProfile);
				for (const MontageTrack& track : montage.tracks)
				{
					std::fprintf(file, "TRACK,%d,%s,%u,%.9g,%.9g,%.9g,%.9g\n", (int)montage.kind, slots[track.slot].name.c_str(), (unsigned)track.clip, track.startPos, track.clipStart, track.clipEnd, track.playRate);
				}
				for (const MontageCurve& curve : montage.curves)
				{
					std::fprintf(file, "MCURVE,%d,%s,%d,%u,%zu", (int)montage.kind, curve.name.c_str(), curve.owner, curve.hz, curve.values.size());
					for (const float value : curve.values) std::fprintf(file, ",%.9g", value);
					std::fprintf(file, "\n");
				}
			}
			std::fprintf(file, "OVERRIDE,%d,%s\n", bundle.overrideClip, bundle.overrideSlot < 0 ? "-" : slots[(size_t)bundle.overrideSlot].name.c_str());
		}
		std::fclose(file);
		return true;
	}

	int RunClips(int argc, char** argv)
	{
		if (argc < 3)
		{
			std::printf("사용: AnimCheck clips <clips.bin> [--dump=<out.csv>] [--markers=<이름 일부>]\n");
			return 2;
		}
		std::string dumpPath;
		std::string markerName;		// --markers= — 이름에 이 글이 든 클립의 길이 · 발 표시(시각 · 왼발 L / 오른발 R)를 찍는다(꺾기 걸음 따르기 대조, 11-18)
		for (int i = 3; i < argc; ++i)
		{
			if (std::strncmp(argv[i], "--dump=", 7) == 0) dumpPath = argv[i] + 7;
			else if (std::strncmp(argv[i], "--markers=", 10) == 0) markerName = argv[i] + 10;
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
			// 몽타주 표(9단계 3-a) — 파일 차례로 종류:이름 · 줄 수 · 곡선 수
			std::printf("ANIMCLIPS montages bundle=%s count=%zu override=%d", bundle.name.c_str(), bundle.montages.size(), bundle.overrideClip);
			for (const ClipMontage& montage : bundle.montages) std::printf(" %d:%s/tracks=%zu/curves=%zu", (int)montage.kind, montage.name.c_str(), montage.tracks.size(), montage.curves.size());
			std::printf("\n");
			if (markerName.empty()) continue;
			for (const AnimClip& clip : bundle.clips)
			{
				if (clip.name.find(markerName) == std::string::npos) continue;
				std::printf("MARKERS bundle=%s clip=%s length=%.4f", bundle.name.c_str(), clip.name.c_str(), clip.length);
				for (const ClipMarker& marker : clip.markers) std::printf(" %.4f%c", marker.time, marker.left ? 'L' : 'R');
				std::printf("\n");
			}
		}
		std::printf("ANIMCLIPS slots=%zu profiles=%zu\n", clips.Slots().size(), clips.Profiles().size());

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

	// Lyra 기록(animcmp_ev_<무기>_probe.csv)의 몽타주 칸 하나 — 「이름#번호@시각*비중>목표 r속도 b섞기시간;」
	struct ProbeMontage
	{
		std::string name;
		int id = -1;
		float position = 0.f;
		float weight = 0.f;
		float desired = 0.f;
		float rate = 1.f;
		float blendTime = 0.f;
	};

	// Lyra 기록 한 줄에서 3-b 가 쓰는 칸. 줄 f = 갱신 f · 자세 f 뒤 사건 f 까지 반영한 값이다(3-b 기록 읽기, 실측)
	struct ProbeRow
	{
		bool firing = false;
		bool hasLayer = false;			// 기록 머리의 무기 층이 지금 층일 때만 층 변수 칸이 찬다
		float timeSinceFired = 0.f;
		float overrideWeight = 0.f;
		float handLeftAlpha = 1.f;
		float aimBlend = 1.f;
		float upperAdditive = 0.f;
		float disableLeftHand = 0.f;
		int ads = -1;					// 조준 변수 GameplayTag_IsADS(C-3). -1 = 이 칸이 없는 옛 기록
		int onGround = -1;				// IsOnGround. -1 = 칸 없음
		int crouched = -1;				// 앉음 변수 IsCrouching(D-3). -1 = 이 칸이 없는 옛 기록
		std::string locomotion;			// 이동 상태 기계(LocomotionSM)의 지금 상태 이름. 빈 칸 = 칸 없음
		std::string idleStance;			// 무기 층 서기 안 상태 기계(IdleStance) 상태 이름 — Idle · StanceTransition. 빈 칸 = 칸 없음
		std::string weapon;
		std::string events;
		std::vector<ProbeMontage> montages;
	};

	bool ParseProbeMontages(const std::string& cell, std::vector<ProbeMontage>& out)
	{
		size_t pos = 0;
		while (pos < cell.size())
		{
			size_t end = cell.find(';', pos);
			if (end == std::string::npos) end = cell.size();
			const std::string item = cell.substr(pos, end - pos);
			pos = end + 1;
			if (item.empty()) continue;
			const size_t hash = item.find('#');
			const size_t at = item.find('@');
			const size_t star = item.find('*');
			const size_t gt = item.find('>');
			const size_t r = item.find(" r");
			const size_t b = item.find(" b");
			if (hash == std::string::npos || at == std::string::npos || star == std::string::npos || gt == std::string::npos || r == std::string::npos || b == std::string::npos) return false;
			ProbeMontage m;
			m.name = item.substr(0, hash);
			m.id = std::atoi(item.c_str() + hash + 1);
			m.position = std::strtof(item.c_str() + at + 1, nullptr);
			m.weight = std::strtof(item.c_str() + star + 1, nullptr);
			m.desired = std::strtof(item.c_str() + gt + 1, nullptr);
			m.rate = std::strtof(item.c_str() + r + 2, nullptr);
			m.blendTime = std::strtof(item.c_str() + b + 2, nullptr);
			out.push_back(m);
		}
		return true;
	}

	bool ReadProbe(const char* path, std::map<std::pair<int, int>, ProbeRow>& out)
	{
		std::vector<char> data;
		if (!ReadFile(path, data))
		{
			std::printf("STATE FAIL Lyra 기록을 못 읽음: %s\n", path);
			return false;
		}
		std::string text(data.begin(), data.end());
		size_t pos = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
		std::vector<std::string> header;
		int cPass = -1, cFrame = -1, cFiring = -1, cSince = -1, cOverride = -1, cHand = -1, cAim = -1, cUpper = -1, cCurves = -1, cEvents = -1, cMontages = -1, cWeapon = -1;
		int cAds = -1, cGround = -1, cLocomotion = -1;		// C-3 에 더한 칸 — 없어도 읽는다(9단계 기록엔 없다)
		int cCrouch = -1, cStance = -1;						// D-3 에 쓰는 칸 — 없어도 읽는다(앉기 전 기록도 칸은 있다)
		const auto bySuffix = [&](const char* suffix)
		{
			const size_t n = std::strlen(suffix);
			for (size_t i = 0; i < header.size(); ++i)
			{
				if (header[i].size() >= n && header[i].compare(header[i].size() - n, n, suffix) == 0) return (int)i;
			}
			return -1;
		};
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
				cPass = Column(header, "pass");
				cFrame = Column(header, "frame");
				cFiring = bySuffix(".GameplayTag_IsFiring");
				cSince = bySuffix(".TimeSinceFiredWeapon");
				cOverride = bySuffix(".HipFireUpperBodyOverrideWeight");
				cHand = bySuffix(".HandIK_Left_Alpha");
				cAim = bySuffix(".AimOffsetBlendWeight");
				cUpper = bySuffix(".UpperbodyDynamicAdditiveWeight");
				cCurves = Column(header, "curves");
				cEvents = Column(header, "events");
				cMontages = Column(header, "montages");
				cWeapon = Column(header, "weapon");
				cAds = bySuffix(".GameplayTag_IsADS");
				cGround = bySuffix(".IsOnGround");
				cLocomotion = Column(header, "sm:ABP_Mannequin_Base.LocomotionSM");
				cCrouch = bySuffix(".IsCrouching");		// 「ABP_Mannequin_Base.IsCrouching」 — 맞춤 속성 칸 「__CustomProperty_isCrouching_<번호>」는 끝이 달라 안 걸린다
				cStance = bySuffix(".IdleStance");			// 「sm:ABP_<무기>AnimLayers_Feminine.IdleStance」 — 무기 층마다 이름 앞이 다르다
				const int need[] = { cPass, cFrame, cFiring, cSince, cOverride, cHand, cAim, cUpper, cCurves, cEvents, cMontages, cWeapon };
				for (int column : need)
				{
					if (column < 0)
					{
						std::printf("STATE FAIL Lyra 기록에 3-b 칸이 없음(머리 %zu 칸)\n", header.size());
						return false;
					}
				}
				continue;
			}
			if (cells.size() != header.size())
			{
				std::printf("STATE FAIL Lyra 기록 줄 %zu 의 칸 수 %zu, 머리 %zu\n", lineNumber, cells.size(), header.size());
				return false;
			}
			ProbeRow row;
			row.firing = std::atoi(cells[(size_t)cFiring].c_str()) != 0;
			row.timeSinceFired = std::strtof(cells[(size_t)cSince].c_str(), nullptr);
			row.hasLayer = !cells[(size_t)cOverride].empty();
			row.overrideWeight = std::strtof(cells[(size_t)cOverride].c_str(), nullptr);
			row.handLeftAlpha = std::strtof(cells[(size_t)cHand].c_str(), nullptr);
			row.aimBlend = std::strtof(cells[(size_t)cAim].c_str(), nullptr);
			row.upperAdditive = std::strtof(cells[(size_t)cUpper].c_str(), nullptr);
			const std::string& curves = cells[(size_t)cCurves];
			const size_t hand = curves.find("DisableLHandIK=");
			row.disableLeftHand = hand == std::string::npos ? 0.f : std::strtof(curves.c_str() + hand + 15, nullptr);
			row.weapon = cells[(size_t)cWeapon];
			row.events = cells[(size_t)cEvents];
			if (cAds >= 0) row.ads = std::atoi(cells[(size_t)cAds].c_str()) != 0 ? 1 : 0;
			if (cGround >= 0) row.onGround = std::atoi(cells[(size_t)cGround].c_str()) != 0 ? 1 : 0;
			if (cLocomotion >= 0) row.locomotion = cells[(size_t)cLocomotion];
			if (cCrouch >= 0) row.crouched = std::atoi(cells[(size_t)cCrouch].c_str()) != 0 ? 1 : 0;
			if (cStance >= 0) row.idleStance = cells[(size_t)cStance];
			if (!ParseProbeMontages(cells[(size_t)cMontages], row.montages))
			{
				std::printf("STATE FAIL Lyra 기록 줄 %zu 몽타주 칸을 못 읽음: %s\n", lineNumber, cells[(size_t)cMontages].c_str());
				return false;
			}
			const int pass = std::atoi(cells[(size_t)cPass].c_str());
			const int frame = std::atoi(cells[(size_t)cFrame].c_str());
			out[{ pass, frame }] = std::move(row);
		}
		return true;
	}

	// 묶음에서 이름으로 몽타주 종류를 찾는다. 없으면 -1
	int MontageKindByName(const ClipBundle& bundle, const std::string& name)
	{
		for (const ClipMontage& montage : bundle.montages)
		{
			if (montage.name == name) return montage.kind;
		}
		return -1;
	}

	// 앉은 캡슐의 메시 올림(D62 7) = 선 반높이 90 − 앉은 반높이 65(Lyra B_Hero_ShooterMannequin, 컨텐츠 0-3 실측). 이 도구는 이동 인자를 안 읽어 상수로 둔다
	constexpr double kCrouchMeshLiftZ = 25.0;

	int RunState(int argc, char** argv)
	{
		if (argc < 6)
		{
			std::printf("사용: AnimCheck state <clips.bin> <animcmp_<Tag>.csv> <animcmp_<Tag>_moves.csv> <out.csv> [--brakeScale=배율] [--groundAdd=cm] [--wire] [--wireKeepNew] [--probe=<animcmp_ev_<무기>_probe.csv>] [--montDump=<out.csv>] [--montageTest=N] [--poseTest=N] [--slotProfile=<비중 표 이름>] [--overrideProfile=<비중 표 이름>] [--adsLag=N] [--noAds] [--crouchLag=N] [--noCrouch] [--crouchLift=cm] [--stanceFade=넣기프레임:빼기프레임]\n");
			return 2;
		}
		float brakeScale = 1.f;
		float groundAdd = 0.f;
		bool useWire = false;
		bool wireKeepNew = false;
		const char* probePath = nullptr;
		const char* montDumpPath = nullptr;
		const char* plantDumpPath = nullptr;		// --plantDump=<csv> — 갱신마다 발 붙이기 장치 값(L-6-c-2). seq = 그 판 입력 번호(한 줄 앞 갱신이 여럿이면 같은 frame 에 여러 줄)
		const char* stackDumpPath = nullptr;		// --stackDump=<csv> — 갱신마다 점프 · 전환 쌓기 칸(칸 수 · 칸별 클립 · 시각 · 섞기, L-13-b 점프 대조). 줄 꼴은 --plantDump 와 같다
		int montageTest = 0;
		int poseTest = 0;							// 끼움 자리 후보 고르기(CAnimPoseEvaluator::kPoseTest*)
		const char* slotProfileName = nullptr;		// 비중 표가 있는 끼움 자리(덮기 자리 빼고)를 이 표로
		const char* overrideProfileName = nullptr;	// 쏜 뒤 덮기 자리를 이 표로
		int adsLag = 1;								// 조준 입력을 몇 줄 앞 기록에서 가져오나(C-3, 1 = 실제 경로)
		bool noAds = false;							// 조준 입력을 늘 0 으로(일부러 틀림)
		// 앉음 입력(D-3) — 갱신 f 의 앉음 = 기록 줄 f−crouchLag 의 IsCrouching. 앉음은 이동이 바꾸고 같은 프레임 애니 갱신이 읽는다(D62 8) —
		// 기록에서 IsCrouching 이 바뀐 줄에 CrouchStateChange 가 같이 켜진다(animcmp_crouch_<무기>_probe.csv 20줄, 실측) → 0 이 실제 경로. 1 은 일부러 틀림
		int crouchLag = 0;
		bool noCrouch = false;						// 앉음 입력을 늘 0 으로(앉기를 옮기기 전 기준선)
		double crouchLift = kCrouchMeshLiftZ;		// 앉은 메시 올림(일부러 틀림 — 0 · 50)
		int stanceInFrames = -1, stanceOutFrames = -1;	// 서기 안 전환을 넣고 빼는 섞기 후보(1/60초 프레임 수, D62 4 후보 비교). -1 = 안 줌 → 실제 경로(넣기 0 · 빼기 15, D63)
		int tracePass = -1, traceFrom = 0, traceTo = -1;		// --trace=판:처음-끝 — 그 줄들의 상태 · 쌓기 칸을 찍는다(어긋난 곳 찾기용)
		double maxReach = 0;
		size_t poseMontageRows = 0, poseOverrideRows = 0, poseHandRows = 0, poseLegRows = 0, posePlantRows = 0, posePlantPartialRows = 0;		// 자세에 몽타주 · 덮기 · 왼팔 · 다리 IK 비중 · 장치(켬 · 0 과 1 사이)가 실제로 들어간 줄(새 길을 지났는지)
		double poseKindSeconds[3] = {};		// 자세 한 번(EvaluateLocal + BodiesFromLocal) 시간 — 몽타주 있음 · 덮기만 · 둘 다 없음
		size_t poseKindRows[3] = {};
		for (int i = 6; i < argc; ++i)
		{
			if (std::strncmp(argv[i], "--brakeScale=", 13) == 0) brakeScale = std::strtof(argv[i] + 13, nullptr);
			else if (std::strncmp(argv[i], "--groundAdd=", 12) == 0) groundAdd = std::strtof(argv[i] + 12, nullptr);
			else if (std::strcmp(argv[i], "--wire") == 0) useWire = true;
			else if (std::strcmp(argv[i], "--wireKeepNew") == 0) useWire = wireKeepNew = true;
			else if (std::strncmp(argv[i], "--probe=", 8) == 0) probePath = argv[i] + 8;
			else if (std::strncmp(argv[i], "--montDump=", 11) == 0) montDumpPath = argv[i] + 11;
			else if (std::strncmp(argv[i], "--plantDump=", 12) == 0) plantDumpPath = argv[i] + 12;
			else if (std::strncmp(argv[i], "--stackDump=", 12) == 0) stackDumpPath = argv[i] + 12;
			else if (std::strncmp(argv[i], "--montageTest=", 14) == 0) montageTest = std::atoi(argv[i] + 14);
			else if (std::strncmp(argv[i], "--poseTest=", 11) == 0) poseTest = std::atoi(argv[i] + 11);
			else if (std::strncmp(argv[i], "--slotProfile=", 14) == 0) slotProfileName = argv[i] + 14;
			else if (std::strncmp(argv[i], "--overrideProfile=", 18) == 0) overrideProfileName = argv[i] + 18;
			else if (std::strncmp(argv[i], "--adsLag=", 9) == 0) adsLag = std::atoi(argv[i] + 9);
			else if (std::strcmp(argv[i], "--noAds") == 0) noAds = true;
			else if (std::strncmp(argv[i], "--crouchLag=", 12) == 0) crouchLag = std::atoi(argv[i] + 12);
			else if (std::strcmp(argv[i], "--noCrouch") == 0) noCrouch = true;
			else if (std::strncmp(argv[i], "--crouchLift=", 13) == 0) crouchLift = std::strtod(argv[i] + 13, nullptr);
			else if (std::strncmp(argv[i], "--stanceFade=", 13) == 0) std::sscanf(argv[i] + 13, "%d:%d", &stanceInFrames, &stanceOutFrames);
			else if (std::strncmp(argv[i], "--trace=", 8) == 0) std::sscanf(argv[i] + 8, "%d:%d-%d", &tracePass, &traceFrom, &traceTo);
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
		std::vector<PoseRow> rows;
		std::vector<MoveRow> moves;
		if (!ReadPoseRows(argv[3], rows) || !ReadMoves(argv[4], moves)) return 1;
		if (rows.empty() || moves.empty())
		{
			std::printf("STATE FAIL 줄 0 rows=%zu moves=%zu\n", rows.size(), moves.size());
			return 1;
		}
		// 뜬 동안 발 디딤 장치의 땅 — 서버(Room.cpp FillAnimInput)와 같게 메시 자리 z + 캡슐 반높이(L-6-c-3). 기록 이동 csv 에 칸이 없어 여기서 채운다
		for (MoveRow& move : moves) move.input.airGap = clips.MeshPlacement().p[2] + MoveParams{}.capsuleHalfHeight;

		// Lyra 기록을 주면 그 기록의 무기 · 사격 · 몽타주 사건으로 상태를 몰고, 몽타주 칸 · 쏜 뒤 변수를 기록과 비교한다(9단계 3-b)
		std::map<std::pair<int, int>, ProbeRow> probe;
		if (probePath && !ReadProbe(probePath, probe)) return 1;
		const auto probeAt = [&](int pass, int frame) -> const ProbeRow*
		{
			const auto found = probe.find({ pass, frame });
			return found == probe.end() ? nullptr : &found->second;
		};
		std::string bundleName = "rifle";
		if (probePath)
		{
			const ProbeRow* first = probeAt(rows[0].pass, rows[0].frame);
			if (!first)
			{
				std::printf("STATE FAIL Lyra 기록에 첫 줄(pass=%d frame=%d)이 없음\n", rows[0].pass, rows[0].frame);
				return 1;
			}
			bundleName = first->weapon;
		}

		// 묶음마다 자세 계산기 하나 — 상태가 든 묶음의 것으로 자세를 만든다
		const std::vector<ClipBundle>& bundles = clips.Bundles();
		std::vector<CAnimPoseEvaluator> evaluators(bundles.size());
		for (size_t b = 0; b < bundles.size(); ++b)
		{
			if (!evaluators[b].Init(clips, bundles[b].name.c_str()))
			{
				std::printf("STATE FAIL 계산기를 못 만듦: %s\n", bundles[b].name.c_str());
				return 1;
			}
			evaluators[b].SetPoseTestFlags((uint32_t)poseTest);
		}
		// 이름 → 비중 표 번호. 뼈 가림(방식 2)이 아니면 자리 섞기에 못 쓴다
		const auto findProfile = [&](const char* name) -> int
		{
			for (size_t p = 0; p < clips.Profiles().size(); ++p)
			{
				if (clips.Profiles()[p].name == name && clips.Profiles()[p].mode == 2) return (int)p;
			}
			std::printf("STATE FAIL 뼈 가림 비중 표가 없음: %s\n", name);
			return -1;
		};
		if (slotProfileName)
		{
			const int profile = findProfile(slotProfileName);
			if (profile < 0) return 1;
			for (CAnimPoseEvaluator& evaluator : evaluators) evaluator.SetSlotProfileForTest(profile);
		}
		if (overrideProfileName)
		{
			const int profile = findProfile(overrideProfileName);
			if (profile < 0) return 1;
			for (CAnimPoseEvaluator& evaluator : evaluators) evaluator.SetOverrideProfileForTest(profile);
		}
		CAnimState state;
		if (!state.Init(clips, bundleName.c_str()))
		{
			std::printf("STATE FAIL 상태를 못 만듦: %s\n", bundleName.c_str());
			return 1;
		}
		if (brakeScale != 1.f) state.SetBrakingScaleForTest(brakeScale);
		state.SetMontageTestFlags(montageTest);
		if (stanceInFrames >= 0 && stanceOutFrames >= 0) state.SetStanceFadeForTest((float)stanceInFrames / 60.f, (float)stanceOutFrames / 60.f);		// 묶음을 다시 읽어도 안 바뀌는 값
		CAnimPoseEvaluator& firstEvaluator = evaluators[(size_t)state.BundleIndex()];
		std::vector<BoneTransform> plantLocal(firstEvaluator.BoneCount());		// 발 붙이기 장치가 갱신마다 셈하는 자세의 작업 칸(L-6-c-2)
		size_t plantPoses = 0, plantTargetZeroRows = 0;

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
		std::vector<BoneTransform> local(firstEvaluator.BoneCount());
		std::vector<BodyPose> poses(firstEvaluator.BodyCount());

		// 몽타주 · 쏜 뒤 변수 대조(--probe). 문턱 = 패킷 눈금(시각 1/6000초 · 비중 1/255)과 기록이 찍은 자릿수(섞기 시간 · 속도 소수 셋째)
		DiffColumn montPosition{ "montPosition", 1.f / 6000.f };
		DiffColumn montWeight{ "montWeight", 1.f / 255.f };
		DiffColumn montDesired{ "montDesired", 0.5f };
		DiffColumn montBlend{ "montBlendTime", 0.0006f };
		DiffColumn montRate{ "montRate", 0.0006f };
		DiffColumn* const montColumns[] = { &montPosition, &montWeight, &montDesired, &montBlend, &montRate };
		// 쏜 뒤 시간은 기록이 유효숫자 넷만 찍어 1초 넘으면 0.001 까지 반올림된다. 층 변수는 기록 머리의 무기 층일 때만 칸이 찬다
		DiffColumn fireSince{ "timeSinceFired", 0.002f };
		DiffColumn fireOverride{ "overrideWeight", 1.f / 255.f };
		DiffColumn lyraAim{ "lyraAimBlend", 1.f / 255.f };
		DiffColumn handLeft{ "handLeftAlpha", 1.f / 255.f };
		DiffColumn leftCurve{ "disableLeftHand", 1.f / 255.f };
		DiffColumn* const fireColumns[] = { &fireSince, &fireOverride, &lyraAim, &handLeft, &leftCurve };
		size_t montRows = 0, montCountDiff = 0, montNameDiff = 0, births = 0, unknownBirths = 0, failedPlays = 0, lateBirths = 0;
		size_t bundleChanges = 0, multiUpdateRows = 0, udwRows = 0, udwBelow = 0;
		size_t reloadCancels = 0, layerRows = 0, layerSwapRows = 0;		// 재장전 중 쏘기 · 던짐으로 재장전을 먼저 끈 수 · 층 변수를 댄 줄 · 교체 사건 줄이라 뺀 줄
		size_t beforeFireRows = 0;		// 판에서 처음 쏘기 전이라 쏜 뒤 시간 · 덮기 비중을 안 댄 줄
		bool firedSinceReset = false;
		// 조준(C-3) — 갱신 f 의 조준 입력 = 기록 줄 f−adsLag 의 IsADS. Lyra 는 사건 f 에 변수가 바뀌고 갱신 f+1 에 상태 기계가 반응한다(D60 6, 실측) → 1 이 실제 경로.
		// 0 · 2 는 일부러 틀림(한 프레임 이르게 · 늦게), noAds = 조준을 안 넣음
		bool adsSinceReset = false;		// 조준이 들어간 뒤로는 덮기 비중이 조준 갈래 값(0 · 1)에서 시작하므로 처음 쏘기 전이라도 댄다
		size_t adsFedRows = 0, adsBlendRows = 0, adsPartialRows = 0, adsMissingRows = 0;
		size_t crouchFedRows = 0, crouchBlendRows = 0, crouchPartialRows = 0, crouchMissingRows = 0, crouchNoColumnRows = 0;
		// 서기 안 전환 대조(D62 4) — Lyra IdleStance 가 StanceTransition 인 줄 ↔ 우리 쌓기에 서기 안 전환 칸이 있는 줄. 앉기 · 일어서기 종류는 Lyra 쪽 IsCrouching 으로
		size_t stanceRows = 0, stanceBoth = 0, stanceLyraOnly = 0, stanceOursOnly = 0, stanceKindDiff = 0, stanceLyraOnlyIdle = 0, stanceOursOnlyIdle = 0;
		std::vector<std::string> stanceFirst;
		size_t smRows = 0, smDiff = 0, smDiffAds = 0, smDiffHip = 0, smDiffCrouch = 0;
		std::map<std::string, size_t> smPairs;			// 「Lyra 상태 > 우리 상태」 → 줄 수
		std::vector<std::string> smFirst;				// 앞 몇 개 어긋난 줄
		bool smWasDiff = false;
		size_t smRuns = 0;								// 어긋남이 시작된 횟수(연속 줄은 한 번)
		size_t smLagOne = 0;							// 한 프레임 늦게 바꾼 줄(아래 대조 주석)
		std::string smPrevRef;							// 앞 줄 Lyra 상태(공중은 "Air")
		bool hasCountFirst = false;
		size_t countFirstRow = 0;
		std::vector<int> seenIds;
		std::map<std::string, std::vector<float>> birthRates;		// 「무기 종류」 → 기록이 준 속도들
		bool lastHadMontage = false;
		FILE* montDump = nullptr;
		if (montDumpPath && (fopen_s(&montDump, montDumpPath, "wb") != 0 || !montDump))
		{
			std::printf("STATE FAIL 몽타주 덤프를 못 씀: %s\n", montDumpPath);
			std::fclose(file);
			return 1;
		}
		if (montDump) std::fprintf(montDump, "pass,frame,ref,got,sinceRef,sinceGot,ovrRef,ovrGot,aimRef,aimGot,handRef,handGot,curveRef,curveGot\n");
		FILE* plantDump = nullptr;
		if (plantDumpPath && (fopen_s(&plantDump, plantDumpPath, "wb") != 0 || !plantDump))
		{
			std::printf("STATE FAIL 발 붙이기 덤프를 못 씀: %s\n", plantDumpPath);
			std::fclose(file);
			if (montDump) std::fclose(montDump);
			return 1;
		}
		if (plantDump) std::fprintf(plantDump, "pass,frame,seq,footL,footR,ground,target,current,alpha,stepped,offsetZ\n");
		FILE* stackDump = nullptr;
		if (stackDumpPath && (fopen_s(&stackDump, stackDumpPath, "wb") != 0 || !stackDump))
		{
			std::printf("STATE FAIL 쌓기 칸 덤프를 못 씀: %s\n", stackDumpPath);
			std::fclose(file);
			if (montDump) std::fclose(montDump);
			if (plantDump) std::fclose(plantDump);
			return 1;
		}
		if (stackDump)
		{
			std::fprintf(stackDump, "pass,frame,seq,count");
			for (int k = 0; k < kAnimLayerMax; ++k) std::fprintf(stackDump, ",clip%d,time%d,fade%d", k, k, k);
			std::fprintf(stackDump, ",crouchBlend,adsBlend,plantAlpha\n");
		}
		const auto bundleIndexOf = [&](const std::string& name)
		{
			for (size_t b = 0; b < bundles.size(); ++b)
			{
				if (bundles[b].name == name) return (int)b;
			}
			return -1;
		};

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
			const bool newReset = row.reset != currentReset;
			if (newReset)
			{
				state.Reset();
				currentReset = row.reset;
				nextSeq = 0;
				lastHadMontage = false;
				firedSinceReset = false;
				adsSinceReset = false;
				smWasDiff = false;
				smPrevRef.clear();
			}
			const auto found = byReset.find(row.reset);
			if (found == byReset.end() || row.seq >= (int)found->second.size() || row.seq + 1 < nextSeq)
			{
				std::printf("STATE FAIL 줄 %zu reset=%d seq=%d 입력 없음 또는 역행(다음 %d)\n", i, row.reset, row.seq, nextSeq);
				std::fclose(file);
				return 1;
			}

			const ProbeRow* lyra = nullptr;
			if (probePath)
			{
				lyra = probeAt(row.pass, row.frame);
				if (!lyra)
				{
					std::printf("STATE FAIL Lyra 기록에 pass=%d frame=%d 가 없음\n", row.pass, row.frame);
					std::fclose(file);
					return 1;
				}
				// 무기 층은 사건 다음 갱신부터 바뀐다 — 앞 줄(사건 f-1 뒤)의 무기가 지금 묶음과 다르면 이번 갱신 앞에 바꾼다(D46). 판 처음은 그 줄의 무기로 새로 잡는다
				const ProbeRow* before = newReset ? nullptr : probeAt(row.pass, row.frame - 1);
				const std::string& want = before ? before->weapon : lyra->weapon;
				if (want != bundles[(size_t)state.BundleIndex()].name)
				{
					const bool ok = newReset ? state.Init(clips, want.c_str()) : state.ChangeBundle(clips, want.c_str());
					if (!ok)
					{
						std::printf("STATE FAIL 묶음을 못 바꿈: %s (pass=%d frame=%d)\n", want.c_str(), row.pass, row.frame);
						std::fclose(file);
						return 1;
					}
					if (brakeScale != 1.f) state.SetBrakingScaleForTest(brakeScale);		// 묶음을 읽으면 제동 값이 묶음 것으로 돌아간다
					++bundleChanges;
				}
				if (!newReset && row.seq != nextSeq) ++multiUpdateRows;		// 이 줄 앞 갱신이 하나가 아니다 — 그 사이 사건은 기록에 없다
			}
			bool adsIn = false;
			if (lyra && !noAds)
			{
				const ProbeRow* source = probeAt(row.pass, row.frame - adsLag);
				if (!source)
				{
					source = lyra;
					++adsMissingRows;
				}
				adsIn = source->ads > 0;
			}
			bool crouchIn = false;
			if (lyra && !noCrouch)
			{
				const ProbeRow* source = probeAt(row.pass, row.frame - crouchLag);
				if (!source)
				{
					source = lyra;
					++crouchMissingRows;
				}
				if (source->crouched < 0) ++crouchNoColumnRows;
				crouchIn = source->crouched > 0;
			}
			const auto t0 = std::chrono::steady_clock::now();
			for (; nextSeq <= row.seq; ++nextSeq)
			{
				AnimMoveInput input = found->second[(size_t)nextSeq]->input;
				input.groundDistance += groundAdd;
				input.ads = adsIn;
				input.crouched = crouchIn;
				state.Update(input, evaluators[(size_t)state.BundleIndex()], plantLocal.data());
				const CAnimState::PlantView plantView = state.Plant();
				if (plantView.stepped) ++plantPoses;
				if (plantDump)
				{
					AnimPoseInput after;
					state.ToPoseInput(after);
					std::fprintf(plantDump, "%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%d,%.5f\n", row.pass, row.frame, nextSeq, plantView.feet[0], plantView.feet[1], plantView.ground,
						plantView.target, plantView.current, plantView.alpha, plantView.stepped ? 1 : 0, after.plantOffset[2]);
				}
				if (stackDump)
				{
					AnimPoseInput after;
					state.ToPoseInput(after);
					std::fprintf(stackDump, "%d,%d,%d,%d", row.pass, row.frame, nextSeq, after.layerCount);
					for (int k = 0; k < kAnimLayerMax; ++k)
						std::fprintf(stackDump, ",%d,%.5f,%.5f", k < after.layerCount ? (int)after.layers[k].clip : -99, k < after.layerCount ? after.layers[k].time : 0.f, k < after.layerCount ? after.layers[k].fade : 0.f);
					std::fprintf(stackDump, ",%.5f,%.5f,%.5f\n", after.crouchBlend, after.adsBlend, after.plantAlpha);
				}
				++fed;
			}
			if (adsIn)
			{
				++adsFedRows;
				adsSinceReset = true;
			}
			if (state.AdsBlend() > 0.f) ++adsBlendRows;
			if (state.AdsBlend() > 0.f && state.AdsBlend() < 1.f) ++adsPartialRows;
			if (crouchIn) ++crouchFedRows;
			if (state.CrouchBlend() > 0.f) ++crouchBlendRows;
			if (state.CrouchBlend() > 0.f && state.CrouchBlend() < 1.f) ++crouchPartialRows;
			updateSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

			if (lyra)
			{
				// 사건 f — 기록에 처음 나온 몽타주를 기록 차례대로 튼다. 종류는 사건 뒤 무기(새 무기)의 묶음에서 이름으로 찾는다
				const int weaponBundle = bundleIndexOf(lyra->weapon);
				for (const ProbeMontage& m : lyra->montages)
				{
					if (std::find(seenIds.begin(), seenIds.end(), m.id) != seenIds.end()) continue;
					seenIds.push_back(m.id);
					++births;
					if (m.position != 0.f || m.weight != 0.f) ++lateBirths;		// 갓 튼 칸은 시각 0 · 비중 0 이어야 한다 — 아니면 기록 앞에서 튼 것이다
					const int kind = weaponBundle < 0 ? -1 : MontageKindByName(bundles[(size_t)weaponBundle], m.name);
					if (kind < 0)
					{
						if (unknownBirths++ == 0) std::printf("STATEMONT 묶음 %s 에 없는 몽타주 %s (pass=%d frame=%d)\n", lyra->weapon.c_str(), m.name.c_str(), row.pass, row.frame);
						continue;
					}
					// 재장전 중 쏘기 — Lyra 는 쏘기 능력이 재장전 능력을 취소해 재장전 몽타주를 자산 빼기 시간으로 먼저 끄고 쏘기를 튼다(1053 기록, 실측). 서버와 같은 함수(PlayFireMontage)를 부른다
					bool played = false;
					if (kind == MONTAGE_FIRE)
					{
						bool cancelledReload = false;
						played = state.PlayFireMontage(m.rate, weaponBundle, cancelledReload);
						if (cancelledReload) ++reloadCancels;
					}
					else if (kind == MONTAGE_GRENADE)
					{
						// 수류탄도 켤 때 재장전 능력을 끊는다(G-0-c) — 서버 CRoom::PlayAnimEvents 와 같은 함수. 재장전 칸이 자산 빼기 곡선을 지닌 채 빠져 Lyra 와 같은 프레임에 지워진다(G-4-b)
						bool cancelledReload = false;
						played = state.PlayMontageCancellingReload(MONTAGE_GRENADE, m.rate, weaponBundle, cancelledReload);
						if (cancelledReload) ++reloadCancels;
					}
					else
					{
						played = state.PlayMontage((uint8_t)kind, m.rate, weaponBundle);
					}
					if (!played) ++failedPlays;
					char key[64];
					std::snprintf(key, sizeof(key), "%s kind=%d", lyra->weapon.c_str(), kind);
					birthRates[key].push_back(m.rate);
				}
				state.SetFiring(lyra->firing);

				// 이동 상태 대조(C-3) — 기록 줄 f 의 상태 = 갱신 f 뒤 상태. Lyra 점프 상태 다섯(JumpStart · JumpStartLoop · JumpApex · FallLoop · FallLand)은 우리 "Air" 하나다.
				// 「한 프레임 늦음」 = Lyra 가 이 줄에서 막 바꿨고 우리는 앞 줄 Lyra 상태에 아직 있다. 빠지는 시각으로 나갈 때 늘 이렇다(9단계부터 같음 —
				// Lyra 는 바꾼 프레임에 새 상태 비중이 0 이고 다음 프레임부터 오르며, 우리는 한 프레임 뒤에 바꾸면서 같은 갱신에 섞기를 한 칸 민다. 허리 0 4,1 기록 3278, 실측)
				if (!lyra->locomotion.empty())
				{
					++smRows;
					const std::string& raw = lyra->locomotion;
					const bool air = raw.compare(0, 4, "Jump") == 0 || raw.compare(0, 4, "Fall") == 0;
					const std::string ref = air ? std::string("Air") : raw;
					const char* got = state.MoveStateName();
					const bool same = ref == got;
					const bool lagOne = !same && !smWasDiff && !smPrevRef.empty() && smPrevRef != ref && smPrevRef == got;
					if (lagOne) ++smLagOne;
					else if (!same)
					{
						++smDiff;
						if (lyra->ads > 0) ++smDiffAds;
						else ++smDiffHip;
						if (lyra->crouched > 0) ++smDiffCrouch;		// 조준 · 허리와 겹쳐 센다(앉아 조준은 둘 다)
						++smPairs[raw + ">" + got];
						if (!smWasDiff)
						{
							++smRuns;
							if (smFirst.size() < 12)
							{
								char text[200];
								std::snprintf(text, sizeof(text), "(pass=%d frame=%d lyra=%s got=%s ads=%d moveAds=%d adsBlend=%.3f crouch=%d moveCrouch=%d crouchBlend=%.3f)", row.pass, row.frame, raw.c_str(), got, lyra->ads, state.MoveAds() ? 1 : 0, state.AdsBlend(),
									lyra->crouched, state.MoveCrouch() ? 1 : 0, state.CrouchBlend());
								smFirst.push_back(text);
							}
						}
					}
					smWasDiff = !same && !lagOne;
					smPrevRef = ref;
				}
				// 서기 안 전환(D62 4) — Lyra 의 서기 안 상태 기계는 서기 상태가 섞여 나가는 동안에도 상태 이름을 지닌다. 우리 칸도 출발 · 점프 아래에서 돈다 → 이동 상태와 따로 센다
				if (!lyra->idleStance.empty())
				{
					++stanceRows;
					const bool lyraStance = lyra->idleStance == "StanceTransition";
					const int ours = state.StanceLayer();
					const bool lyraIdle = lyra->locomotion == "Idle";
					if (lyraStance && ours != 0)
					{
						++stanceBoth;
						if (ours != (lyra->crouched > 0 ? 1 : 2)) ++stanceKindDiff;
					}
					else if (lyraStance || ours != 0)
					{
						if (lyraStance) ++stanceLyraOnly;
						else ++stanceOursOnly;
						if (lyraStance && lyraIdle) ++stanceLyraOnlyIdle;
						if (!lyraStance && lyraIdle) ++stanceOursOnlyIdle;
						if (stanceFirst.size() < 12)
						{
							char text[160];
							std::snprintf(text, sizeof(text), "(pass=%d frame=%d lyra=%s/%s ours=%d state=%s crouch=%d)", row.pass, row.frame, lyra->idleStance.c_str(), lyra->locomotion.c_str(), ours, state.MoveStateName(), lyra->crouched);
							stanceFirst.push_back(text);
						}
					}
				}

				// 사건 뒤 목록 대조 — 엔진 배열 차례 그대로
				++montRows;
				const int gotCount = state.MontageCount();
				if (gotCount != (int)lyra->montages.size())
				{
					++montCountDiff;
					if (!hasCountFirst)
					{
						hasCountFirst = true;
						countFirstRow = i;
					}
				}
				const int common = std::min(gotCount, (int)lyra->montages.size());
				for (int k = 0; k < common; ++k)
				{
					const CAnimState::MontageView view = state.MontageAt(k);
					const ProbeMontage& m = lyra->montages[(size_t)k];
					if (view.montage->name != m.name) ++montNameDiff;
					montPosition.Add(m.position, view.position, i);
					montWeight.Add(m.weight, view.weight, i);
					montDesired.Add(m.desired, view.desired, i);
					montBlend.Add(m.blendTime, view.blendTime, i);
					montRate.Add(m.rate, view.playRate, i);
				}

				// 쏜 뒤 변수(갱신 f 값). 쏜 뒤 시간 · 덮기 비중은 그 판에서 처음 쏜 다음 줄부터 — 판 시작 값이 서로 다르다
				// (기록은 레벨 시작부터 센다: 권총 판 시작 4.033초 · 덮기 0.0279, 우리는 「안 쏨」 1000초 · 0).
				// 쏜 뒤 시간은 어느 한쪽이 5초 안일 때만 더 본다 — 기록이 유효숫자 4자리라(1662 줄 10.02) 그 위에선 기준 0.002 를 못 맞춘다. 덮기 비중이 가르는 건 0.5초뿐이다
				if (!firedSinceReset) ++beforeFireRows;
				else if (lyra->timeSinceFired < 5.f || state.TimeSinceFired() < 5.f) fireSince.Add(lyra->timeSinceFired, state.TimeSinceFired(), i);
				// 무기를 바꾼 사건 줄은 층 칸에 새 층의 처음 값이 먼저 찍히고, 그 줄 갱신 · 자세는 앞 층이 했다(D46) — 층 변수 대조에서 빼고 수를 센다
				const ProbeRow* previous = newReset ? nullptr : probeAt(row.pass, row.frame - 1);
				const bool swapRow = previous && previous->weapon != lyra->weapon;
				if (lyra->hasLayer && swapRow) ++layerSwapRows;
				if (lyra->hasLayer && !swapRow)
				{
					++layerRows;
					if (firedSinceReset || adsSinceReset) fireOverride.Add(lyra->overrideWeight, state.OverrideWeight(), i);
					lyraAim.Add(lyra->aimBlend, state.AimBlend(), i);
					handLeft.Add(lyra->handLeftAlpha, state.HandLeftAlpha(), i);
				}
				leftCurve.Add(lyra->disableLeftHand, state.DisableLeftHand(), i);
				// 윗몸 덧붙임 비중이 1 이 아닌 줄 — 앞 줄 사건 뒤에 몽타주가 있었는데도 1 이 아니면 그 비중을 옮겨야 한다
				if (lastHadMontage)
				{
					++udwRows;
					if (lyra->upperAdditive < 0.999f) ++udwBelow;
				}
				lastHadMontage = !lyra->montages.empty();
				if (lyra->firing) firedSinceReset = true;		// 이 줄 사건의 발사는 다음 줄 갱신부터 보인다

				if (montDump)
				{
					std::string refCell, gotCell;
					for (const ProbeMontage& m : lyra->montages)
					{
						char cell[160];
						std::snprintf(cell, sizeof(cell), "%s@%.5f*%.4f>%.0f b%.3f;", m.name.c_str(), m.position, m.weight, m.desired, m.blendTime);
						refCell += cell;
					}
					for (int k = 0; k < gotCount; ++k)
					{
						const CAnimState::MontageView view = state.MontageAt(k);
						char cell[160];
						std::snprintf(cell, sizeof(cell), "%s@%.5f*%.4f>%.0f b%.3f;", view.montage->name.c_str(), view.position, view.weight, view.desired, view.blendTime);
						gotCell += cell;
					}
					std::fprintf(montDump, "%d,%d,%s,%s,%.4f,%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.4f,%.4f\n", row.pass, row.frame, refCell.c_str(), gotCell.c_str(),
						lyra->timeSinceFired, state.TimeSinceFired(), lyra->overrideWeight, state.OverrideWeight(), lyra->aimBlend, state.AimBlend(),
						lyra->handLeftAlpha, state.HandLeftAlpha(), lyra->disableLeftHand, state.DisableLeftHand());
				}
			}

			AnimPoseInput got;
			state.ToPoseInput(got);
			{
				const CAnimState::PlantView plantView = state.Plant();
				if (plantView.stepped && plantView.target == 0.f) ++plantTargetZeroRows;
			}
			if (row.pass == tracePass && row.frame >= traceFrom && row.frame <= traceTo)
			{
				std::printf("TRACE pass=%d frame=%d state=%s ads=%d moveAds=%d adsBlend=%.4f crouch=%d moveCrouch=%d crouchBlend=%.4f lyra=%s/%s layers=", row.pass, row.frame, state.MoveStateName(), adsIn ? 1 : 0, state.MoveAds() ? 1 : 0, state.AdsBlend(),
					crouchIn ? 1 : 0, state.MoveCrouch() ? 1 : 0, state.CrouchBlend(), lyra ? lyra->locomotion.c_str() : "-", lyra ? lyra->idleStance.c_str() : "-");
				for (int k = 0; k < got.layerCount; ++k) std::printf("%d@%.4f*%.3f ", got.layers[k].clip, got.layers[k].time, got.layers[k].fade);
				std::printf("walk=");
				for (int k = 0; k < kAnimWalkClips; ++k)
				{
					if (got.weights[k] > 0.001f) std::printf("%d*%.3f@hip%.4f/ads%.4f/crouch%.4f ", k, got.weights[k], got.times[k], got.adsTimes[k], got.crouchTimes[k]);
				}
				std::printf("\n");
			}
			if (useWire)
			{
				wire::AnimStateWire onWire{};
				state.ToWire(onWire);
				const AnimPoseInput exact = got;
				state.FromWire(onWire, got.pitch, got.crouched, got);
				if (wireKeepNew)
				{
					got.montageCount = exact.montageCount;
					for (int k = 0; k < kAnimMontageMax; ++k) got.montages[k] = exact.montages[k];
					got.overrideWeight = exact.overrideWeight;
					got.handLeftAlpha = exact.handLeftAlpha;
					got.legIKAlpha = exact.legIKAlpha;
					got.plantAlpha = exact.plantAlpha;
					for (int k = 0; k < 2; ++k) got.footOffset[k] = exact.footOffset[k];
				}
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

			CAnimPoseEvaluator& evaluator = evaluators[(size_t)state.BundleIndex()];
			if (got.montageCount > 0) ++poseMontageRows;
			if (got.overrideWeight > 0.00001f) ++poseOverrideRows;
			if (got.handLeftAlpha < 1.f) ++poseHandRows;
			if (got.legIKAlpha < 1.f) ++poseLegRows;
			if (got.plantAlpha > 0.f) ++posePlantRows;
			if (got.plantAlpha > 0.f && got.plantAlpha < 1.f) ++posePlantPartialRows;
			const auto poseStart = std::chrono::steady_clock::now();
			evaluator.EvaluateLocal(got, local.data());
			evaluator.BodiesFromLocal(local.data(), poses.data(), got.crouched ? crouchLift : 0.0);
			const double poseSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - poseStart).count();
			const int poseKind = got.montageCount > 0 ? 0 : (got.overrideWeight > 0.00001f ? 1 : 2);
			poseKindSeconds[poseKind] += poseSeconds;
			++poseKindRows[poseKind];
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
		if (montDump) std::fclose(montDump);
		if (plantDump) std::fclose(plantDump);
		if (stackDump) std::fclose(stackDump);

		const double count = (double)rows.size();
		std::printf("STATEOUT rows=%zu moves=%zu fed=%zu lines=%zu resets=%zu brakeScale=%.3f groundAdd=%.1f overflows=%d\n", rows.size(), moves.size(), fed, lines, byReset.size(), brakeScale, groundAdd, state.StackOverflows());
		std::printf("STATETIME updateUs=%.3f\n", fed > 0 ? updateSeconds * 1e6 / (double)fed : 0.0);
		const auto poseUs = [&](int kind) { return poseKindRows[kind] > 0 ? poseKindSeconds[kind] * 1e6 / (double)poseKindRows[kind] : 0.0; };
		std::printf("STATEPOSETIME montageUs=%.2f rows=%zu overrideOnlyUs=%.2f rows=%zu plainUs=%.2f rows=%zu\n", poseUs(0), poseKindRows[0], poseUs(1), poseKindRows[1], poseUs(2), poseKindRows[2]);
		unsigned long long montageMisses = 0, trackOutside = 0, legSolves = 0, legStraight = 0, legUnreachable = 0, plantSolves = 0, plantStretched = 0, plantExtended = 0;
		for (const CAnimPoseEvaluator& evaluator : evaluators)
		{
			montageMisses += evaluator.MontageMisses();
			trackOutside += evaluator.TrackOutside();
			legSolves += evaluator.LegIKSolves();
			legStraight += evaluator.LegIKStraight();
			legUnreachable += evaluator.LegIKUnreachable();
			plantSolves += evaluator.PlantLegSolves();
			plantStretched += evaluator.PlantLegStretched();
			plantExtended += evaluator.PlantLegExtended();
		}
		std::printf("STATEPOSE poseTest=%d slotProfile=%s overrideProfile=%s rows=%zu montageRows=%zu overrideRows=%zu handRows=%zu montageMisses=%llu trackOutside=%llu\n", poseTest, slotProfileName ? slotProfileName : "-", overrideProfileName ? overrideProfileName : "-", rows.size(), poseMontageRows, poseOverrideRows, poseHandRows, montageMisses, trackOutside);
		// 다리 IK(L-12-c) — 비중이 1 보다 작은 줄 · 다리를 고친 수(셈마다 다리 하나씩, 발 붙이기 장치의 셈까지) · 다리가 곧아 엔진이 앞 프레임 굽힘 방향을 읽었을 수 · 안 닿아 곧게 편 수
		std::printf("STATELEGIK rows=%zu partialRows=%zu solves=%llu straight=%llu unreachable=%llu\n", rows.size(), poseLegRows, legSolves, legStraight, legUnreachable);
		// 장치 다리 몫(L-12-e) — 장치가 켜진 줄 · 알파가 0 과 1 사이인 줄 · 다리를 고친 수(셈마다 다리 하나씩, 발 높이 셈 몫은 안 돈다) · 닿는 비율이 늘리기 시작을 넘은 수 · 다리가 곧아 굽힘 쪽을 무릎 조절점으로 잡은 수
		std::printf("STATEPLANTLEG rows=%zu plantRows=%zu partialRows=%zu solves=%llu stretched=%llu extended=%llu length=%.6f/%.6f\n", rows.size(), posePlantRows, posePlantPartialRows, plantSolves, plantStretched, plantExtended, evaluators[0].PlantLegLength(0), evaluators[0].PlantLegLength(1));
		double bound = 0;
		for (CAnimPoseEvaluator& evaluator : evaluators) bound = std::max(bound, evaluator.ComputeBoundRadius(kCrouchMeshLiftZ));
		std::printf("STATEBOUND reach=%.3f bound=%.3f wire=%d\n", maxReach, bound, useWire ? (wireKeepNew ? 2 : 1) : 0);
		// 발 붙이기 장치(L-6-c-2) — poses = 발 높이를 읽으려 셈한 자세(땅 위 · 장치 켜짐 갱신) · targetZeroRows = 두 발 다 쓸기가 땅을 못 맞힌 줄 · bundleMisses 0 이어야 한다
		std::printf("STATEPLANT poses=%zu fed=%zu targetZeroRows=%zu bundleMisses=%llu\n", plantPoses, fed, plantTargetZeroRows, (unsigned long long)state.PlantBundleMisses());
		if (probePath)
		{
			std::printf("STATEMONT rows=%zu countDiff=%zu nameDiff=%zu births=%zu unknownBirths=%zu failedPlays=%zu lateBirths=%zu drops=%d profileStartMisses=%d bundleChanges=%zu multiUpdateRows=%zu test=%d",
				montRows, montCountDiff, montNameDiff, births, unknownBirths, failedPlays, lateBirths, state.MontageDrops(), state.ProfileStartMisses(), bundleChanges, multiUpdateRows, montageTest);
			if (hasCountFirst) std::printf(" countFirst=(pass=%d frame=%d)", rows[countFirstRow].pass, rows[countFirstRow].frame);
			std::printf("\n");
			for (const auto& entry : birthRates)
			{
				float low = entry.second[0], high = entry.second[0];
				for (float rate : entry.second)
				{
					low = std::min(low, rate);
					high = std::max(high, rate);
				}
				std::printf("STATEBIRTH %s count=%zu rate=%.3f~%.3f\n", entry.first.c_str(), entry.second.size(), low, high);
			}
			std::printf("STATEUDW rowsAfterMontage=%zu below1=%zu\n", udwRows, udwBelow);
			std::printf("STATELAYER rows=%zu swapRowsSkipped=%zu reloadCancels=%zu beforeFireRows=%zu\n", layerRows, layerSwapRows, reloadCancels, beforeFireRows);
			std::printf("STATEADS adsLag=%d noAds=%d fedRows=%zu blendRows=%zu partialRows=%zu missingLagRows=%zu\n", adsLag, noAds ? 1 : 0, adsFedRows, adsBlendRows, adsPartialRows, adsMissingRows);
			std::printf("STATECROUCH crouchLag=%d noCrouch=%d lift=%.1f stanceFade=%d:%d fedRows=%zu blendRows=%zu partialRows=%zu missingLagRows=%zu noColumnRows=%zu\n", crouchLag, noCrouch ? 1 : 0, crouchLift, stanceInFrames, stanceOutFrames,
				crouchFedRows, crouchBlendRows, crouchPartialRows, crouchMissingRows, crouchNoColumnRows);
			std::printf("STATESTANCE rows=%zu both=%zu kindDiff=%zu lyraOnly=%zu oursOnly=%zu lyraOnlyIdle=%zu oursOnlyIdle=%zu\n", stanceRows, stanceBoth, stanceKindDiff, stanceLyraOnly, stanceOursOnly, stanceLyraOnlyIdle, stanceOursOnlyIdle);
			for (const std::string& text : stanceFirst) std::printf("STATESTANCEFIRST %s\n", text.c_str());
			std::printf("STATESM rows=%zu lagOne=%zu diff=%zu diffAds=%zu diffHip=%zu diffCrouch=%zu runs=%zu\n", smRows, smLagOne, smDiff, smDiffAds, smDiffHip, smDiffCrouch, smRuns);
			for (const auto& pair : smPairs) std::printf("STATESMPAIR %s rows=%zu\n", pair.first.c_str(), pair.second);
			for (const std::string& text : smFirst) std::printf("STATESMFIRST %s\n", text.c_str());
			const auto print = [&](const char* tag, const DiffColumn* column)
			{
				std::printf("%s %s max=%.6f over=%zu", tag, column->name, column->max, column->over);
				if (column->hasFirst)
				{
					const PoseRow& first = rows[column->firstRow];
					std::printf(" first=(pass=%d frame=%d ref=%.6f got=%.6f)", first.pass, first.frame, column->firstRef, column->firstGot);
				}
				const PoseRow& worst = rows[column->maxRow];
				std::printf(" maxAt=(pass=%d frame=%d)\n", worst.pass, worst.frame);
			};
			for (const DiffColumn* column : montColumns) print("STATEMONTDIFF", column);
			for (const DiffColumn* column : fireColumns) print("STATEFIRE", column);
		}
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
	// L-13-b4 — 긴 낙하 패킷 왕복(머리 주석 「AnimCheck fall」)
	int RunFall(int argc, char** argv)
	{
		if (argc < 3)
		{
			std::printf("사용: AnimCheck fall <clips.bin> [--seconds=초]\n");
			return 2;
		}
		float seconds = 12.f;
		for (int i = 3; i < argc; ++i)
		{
			if (std::strncmp(argv[i], "--seconds=", 10) == 0) seconds = (float)std::atof(argv[i] + 10);
		}
		std::vector<char> data;
		if (!ReadFile(argv[2], data))
		{
			std::printf("FALLWIRE FAIL 클립 파일을 못 읽음: %s\n", argv[2]);
			return 1;
		}
		CAnimClips clips;
		const EClipLoad result = clips.LoadFromMemory(data.data(), data.size());
		if (result != EClipLoad::Ok)
		{
			std::printf("FALLWIRE FAIL result=%s\n", LoadName(result));
			return 1;
		}
		constexpr float kDt = 1.f / 60.f;
		constexpr int kStandFrames = 60;
		constexpr float kSlack = 0.001f;		// 패킷 눈금 1/6000초(반올림 몫 1/12000)의 열두 배 — 넘으면 다른 자리
		const int fallFrames = (int)std::lround(seconds / kDt);
		bool ok = true;
		for (const ClipBundle& bundle : clips.Bundles())
		{
			CAnimState state;
			CAnimPoseEvaluator evaluator;
			if (!state.Init(clips, bundle.name.c_str()) || !evaluator.Init(clips, bundle.name.c_str()))
			{
				std::printf("FALLWIRE FAIL 상태 · 계산기를 못 만듦: %s\n", bundle.name.c_str());
				ok = false;
				continue;
			}
			std::vector<BoneTransform> scratch(evaluator.BoneCount());
			size_t loopRows = 0, stuck = 0, countDiff = 0;
			float maxDiff = 0.f, maxTime = 0.f, firstStuck = -1.f;
			for (int f = 0; f < kStandFrames + fallFrames; ++f)
			{
				const bool falling = f >= kStandFrames;
				AnimMoveInput input;
				input.deltaSeconds = kDt;
				input.falling = falling;
				input.velocity[2] = falling ? -980.0 * (double)(f - kStandFrames) * (double)kDt : 0.0;
				input.groundDistance = falling ? 100000.f : 0.f;	// 땅까지 1km — 내려앉음 클립으로 안 넘어간다
				input.airGap = clips.MeshPlacement().p[2] + MoveParams{}.capsuleHalfHeight;
				state.Update(input, evaluator, scratch.data());
				AnimPoseInput exact, viaWire;
				state.ToPoseInput(exact);
				wire::AnimStateWire onWire{};
				state.ToWire(onWire);
				state.FromWire(onWire, exact.pitch, exact.crouched, viaWire);
				if (viaWire.layerCount != exact.layerCount)
				{
					++countDiff;
					continue;
				}
				for (int k = 0; k < exact.layerCount; ++k)
				{
					if (exact.layers[k].clip < 0) continue;
					const AnimClip& clip = bundle.clips[StackClipIndex(exact.layers[k].clip)];
					if (!clip.loop) continue;
					++loopRows;
					const float length = std::max(clip.length, 0.01f);
					const float a = std::fmod(exact.layers[k].time, length);
					const float b = std::fmod(viaWire.layers[k].time, length);
					float d = std::fabs(a - b);
					d = std::min(d, length - d);		// 길이 끝과 0 은 같은 자리
					maxDiff = std::max(maxDiff, d);
					maxTime = std::max(maxTime, exact.layers[k].time);
					if (d > kSlack)
					{
						++stuck;
						if (firstStuck < 0.f) firstStuck = (float)(f - kStandFrames) * kDt;
					}
				}
			}
			std::printf("FALLWIRE bundle=%s fallSeconds=%.1f frames=%d loopRows=%zu countDiff=%zu maxLayerTime=%.3f maxDiff=%.6f stuck=%zu firstStuck=%.3f\n",
				bundle.name.c_str(), seconds, kStandFrames + fallFrames, loopRows, countDiff, maxTime, maxDiff, stuck, firstStuck);
			if (loopRows == 0 || countDiff > 0 || stuck > 0) ok = false;
		}
		std::printf(ok ? "FALLWIRE PASS\n" : "FALLWIRE FAIL\n");
		return ok ? 0 : 1;
	}
}

int main(int argc, char** argv)
{
	if (argc >= 2 && std::strcmp(argv[1], "clips") == 0) return RunClips(argc, argv);
	if (argc >= 2 && std::strcmp(argv[1], "pose") == 0) return RunPose(argc, argv);
	if (argc >= 2 && std::strcmp(argv[1], "state") == 0) return RunState(argc, argv);
	if (argc >= 2 && std::strcmp(argv[1], "fall") == 0) return RunFall(argc, argv);
	std::printf("사용: AnimCheck clips <clips.bin> [--dump=<out.csv>]\n");
	std::printf("      AnimCheck pose <clips.bin> <animcmp_<Tag>.csv> <out.csv> [--shift=N] [--swaplr] [--noHandIK]\n");
	std::printf("      AnimCheck state <clips.bin> <animcmp_<Tag>.csv> <animcmp_<Tag>_moves.csv> <out.csv> [--brakeScale=배율] [--groundAdd=cm] [--wire]\n");
	std::printf("      AnimCheck fall <clips.bin> [--seconds=초]\n");
	return 2;
}
