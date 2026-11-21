#include "AnimClips.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mo
{
	namespace
	{
		static_assert(sizeof(BoneKey) == 28, "파일의 키 한 칸은 28바이트다");

		class CClipReader
		{
		public:
			CClipReader(const void* data, size_t size) : _cursor(static_cast<const uint8_t*>(data)), _end(_cursor + size) {}

			bool Ok() const { return _ok; }
			size_t Left() const { return (size_t)(_end - _cursor); }

			template <class T>
			T Read()
			{
				T value{};
				Bytes(&value, sizeof(T));
				return value;
			}

			void Bytes(void* out, size_t count)
			{
				if (!_ok || Left() < count)
				{
					_ok = false;
					return;
				}
				std::memcpy(out, _cursor, count);
				_cursor += count;
			}

			void Name(std::string& out)
			{
				const uint8_t length = Read<uint8_t>();
				if (!_ok || Left() < length)
				{
					_ok = false;
					return;
				}
				out.assign(reinterpret_cast<const char*>(_cursor), length);
				_cursor += length;
			}

		private:
			const uint8_t* _cursor;
			const uint8_t* _end;
			bool _ok = true;
		};
	}

	void CAnimClips::Clear()
	{
		_bones.clear();
		_bodies.clear();
		_mesh = BoneKey{};
		_profiles.clear();
		_slots.clear();
		_bundles.clear();
	}

	EClipLoad CAnimClips::LoadFromMemory(const void* data, size_t size)
	{
		Clear();
		const EClipLoad result = Parse(data, size);
		if (result != EClipLoad::Ok) Clear();
		return result;
	}

	EClipLoad CAnimClips::Parse(const void* data, size_t size)
	{
		if (size < 16) return EClipLoad::TooSmall;
		CClipReader reader(data, size);

		char magic[4];
		reader.Bytes(magic, 4);
		if (std::memcmp(magic, "MOAC", 4) != 0) return EClipLoad::BadMagic;
		if (reader.Read<uint32_t>() != kClipPackVersion) return EClipLoad::BadVersion;

		const uint32_t boneCount = reader.Read<uint32_t>();
		if (boneCount == 0 || boneCount > 32767) return EClipLoad::BadBone;
		_bones.resize(boneCount);
		for (uint32_t b = 0; b < boneCount; ++b)
		{
			ClipBone& bone = _bones[b];
			reader.Name(bone.name);
			bone.parent = reader.Read<int16_t>();
			reader.Bytes(&bone.ref, sizeof(bone.ref));
			if (!reader.Ok()) return EClipLoad::Truncated;
			if (bone.parent >= (int)b || bone.parent < -1) return EClipLoad::BadBone;
		}

		reader.Bytes(&_mesh, sizeof(_mesh));
		const uint32_t bodyCount = reader.Read<uint32_t>();
		if (!reader.Ok()) return EClipLoad::Truncated;
		if (bodyCount == 0 || bodyCount > 256) return EClipLoad::BadBody;
		_bodies.resize(bodyCount);
		for (ClipBody& body : _bodies)
		{
			std::string boneName;
			reader.Name(boneName);
			body.kind = reader.Read<uint8_t>();
			body.weakSpot = reader.Read<uint8_t>() != 0;
			reader.Bytes(body.center, sizeof(body.center));
			reader.Bytes(body.q, sizeof(body.q));
			body.a = reader.Read<float>();
			body.b = reader.Read<float>();
			body.c = reader.Read<float>();
			if (!reader.Ok()) return EClipLoad::Truncated;
			body.bone = FindBone(boneName.c_str());
			if (body.bone < 0) return EClipLoad::BadBody;
		}

		// 뼈별 비중 표 — 표마다 남긴 뼈 수만큼
		const uint32_t profileCount = reader.Read<uint32_t>();
		if (!reader.Ok()) return EClipLoad::Truncated;
		if (profileCount > 64) return EClipLoad::BadSlot;
		_profiles.resize(profileCount);
		for (ClipProfile& profile : _profiles)
		{
			reader.Name(profile.name);
			profile.mode = reader.Read<uint8_t>();
			if (!reader.Ok() || (uint64_t)boneCount * sizeof(float) > reader.Left()) return EClipLoad::Truncated;
			if (profile.mode > 2) return EClipLoad::BadSlot;		// 엔진 EBlendProfileMode 는 셋(TimeFactor · WeightFactor · BlendMask)
			profile.weights.resize(boneCount);
			reader.Bytes(profile.weights.data(), boneCount * sizeof(float));
		}

		// 끼움 자리 표
		const uint32_t slotCount = reader.Read<uint32_t>();
		if (!reader.Ok()) return EClipLoad::Truncated;
		if (slotCount > 255) return EClipLoad::BadSlot;
		_slots.resize(slotCount);
		for (size_t s = 0; s < _slots.size(); ++s)
		{
			ClipSlot& slot = _slots[s];
			reader.Name(slot.name);
			const uint8_t mode = reader.Read<uint8_t>();
			slot.profile = reader.Read<int16_t>();
			if (!reader.Ok()) return EClipLoad::Truncated;
			if (mode >= (uint8_t)ESlotMode::Count || slot.profile < -1 || slot.profile >= (int)profileCount) return EClipLoad::BadSlot;
			slot.mode = (ESlotMode)mode;
			for (size_t other = 0; other < s; ++other)
			{
				if (_slots[other].name == slot.name) return EClipLoad::BadSlot;
			}
		}

		const uint32_t bundleCount = reader.Read<uint32_t>();
		if (!reader.Ok()) return EClipLoad::Truncated;
		if (bundleCount == 0 || bundleCount > 64) return EClipLoad::BadBundle;
		_bundles.resize(bundleCount);
		for (ClipBundle& bundle : _bundles)
		{
			reader.Name(bundle.name);
			bundle.sampleHz = reader.Read<uint32_t>();
			reader.Bytes(bundle.aimPitch, sizeof(bundle.aimPitch));
			bundle.groundFriction = reader.Read<float>();
			bundle.brakingFriction = reader.Read<float>();
			bundle.brakingFrictionFactor = reader.Read<float>();
			bundle.brakingDecelerationWalking = reader.Read<float>();
			bundle.gravityZ = reader.Read<float>();
			bundle.separateBraking = reader.Read<uint8_t>() != 0;
			const uint32_t clipCount = reader.Read<uint32_t>();
			if (!reader.Ok()) return EClipLoad::Truncated;
			if (bundle.sampleHz == 0 || clipCount == 0 || clipCount > 1024) return EClipLoad::BadBundle;

			bundle.clips.resize(clipCount);
			for (AnimClip& clip : bundle.clips)
			{
				reader.Name(clip.name);
				clip.length = reader.Read<float>();
				clip.additive = reader.Read<uint8_t>();
				clip.loop = reader.Read<uint8_t>() != 0;
				clip.rootSpeed = reader.Read<float>();
				clip.samples = reader.Read<uint32_t>();

				const uint16_t markerCount = reader.Read<uint16_t>();
				if (!reader.Ok()) return EClipLoad::Truncated;
				clip.markers.resize(markerCount);
				for (ClipMarker& marker : clip.markers)
				{
					marker.time = reader.Read<float>();
					marker.left = reader.Read<uint8_t>() != 0;
				}

				clip.hasCurve = reader.Read<uint8_t>() != 0;
				if (clip.hasCurve)
				{
					clip.curveHz = reader.Read<uint32_t>();
					const uint32_t curveCount = reader.Read<uint32_t>();
					if (!reader.Ok() || curveCount > reader.Left() / sizeof(float)) return EClipLoad::Truncated;
					clip.curve.resize(curveCount);
					reader.Bytes(clip.curve.data(), curveCount * sizeof(float));
				}
				clip.hasLegCurve = reader.Read<uint8_t>() != 0;
				if (clip.hasLegCurve)
				{
					clip.legCurveHz = reader.Read<uint32_t>();
					const uint32_t legCount = reader.Read<uint32_t>();
					if (!reader.Ok() || legCount > reader.Left() / sizeof(float)) return EClipLoad::Truncated;
					clip.legCurve.resize(legCount);
					reader.Bytes(clip.legCurve.data(), legCount * sizeof(float));
				}
				if (!reader.Ok()) return EClipLoad::Truncated;

				// 표본 수는 뽑는 쪽과 같은 float 곱으로 다시 계산해 맞춘다(언리얼 CeilToInt32(float × int) + 1)
				if (!(clip.length > 0.f)) return EClipLoad::BadClip;
				const uint32_t wantSamples = (uint32_t)std::ceil(clip.length * (float)bundle.sampleHz) + 1;
				if (clip.samples != wantSamples || clip.samples < 2) return EClipLoad::BadClip;

				const uint64_t keyBytes = (uint64_t)clip.samples * boneCount * sizeof(BoneKey);
				if (keyBytes > reader.Left()) return EClipLoad::Truncated;
				clip.keys.resize((size_t)clip.samples * boneCount);
				reader.Bytes(clip.keys.data(), (size_t)keyBytes);
			}

			// 몽타주 표 — 줄은 있는 끼움 자리 · 클립만 가리키고, 클립 구간은 그 클립 안이어야 한다
			const uint32_t montageCount = reader.Read<uint32_t>();
			if (!reader.Ok()) return EClipLoad::Truncated;
			if (montageCount > MONTAGE_KIND_COUNT) return EClipLoad::BadMontage;
			bundle.montages.resize(montageCount);
			for (ClipMontage& montage : bundle.montages)
			{
				reader.Name(montage.name);
				montage.kind = reader.Read<uint8_t>();
				reader.Name(montage.group);
				montage.length = reader.Read<float>();
				montage.rate = reader.Read<float>();
				montage.blendIn = reader.Read<float>();
				montage.blendInOption = reader.Read<uint8_t>();
				montage.blendOut = reader.Read<float>();
				montage.blendOutOption = reader.Read<uint8_t>();
				montage.blendOutTrigger = reader.Read<float>();
				montage.autoBlendOut = reader.Read<uint8_t>() != 0;
				montage.blendOutProfile = reader.Read<int16_t>();
				const uint8_t trackCount = reader.Read<uint8_t>();
				if (!reader.Ok()) return EClipLoad::Truncated;
				if (montage.kind >= MONTAGE_KIND_COUNT || bundle.FindMontage(montage.kind) != &montage) return EClipLoad::BadMontage;
				if (!(montage.length > 0.f) || !(montage.rate > 0.f) || !(montage.blendIn >= 0.f) || !(montage.blendOut >= 0.f) || trackCount == 0) return EClipLoad::BadMontage;
				if (montage.blendInOption > kBlendOptionMax || montage.blendOutOption > kBlendOptionMax) return EClipLoad::BadMontage;
				if (montage.blendOutProfile < -1 || montage.blendOutProfile >= (int)profileCount) return EClipLoad::BadMontage;

				montage.tracks.resize(trackCount);
				for (MontageTrack& track : montage.tracks)
				{
					track.slot = reader.Read<uint8_t>();
					track.clip = reader.Read<uint16_t>();
					track.startPos = reader.Read<float>();
					track.clipStart = reader.Read<float>();
					track.clipEnd = reader.Read<float>();
					track.playRate = reader.Read<float>();
					if (!reader.Ok()) return EClipLoad::Truncated;
					if (track.slot >= slotCount || track.clip >= clipCount || !(track.playRate > 0.f)) return EClipLoad::BadMontage;
					const float clipLength = bundle.clips[track.clip].length;
					if (!(track.clipStart >= 0.f) || !(track.clipEnd > track.clipStart) || track.clipEnd > clipLength || !(track.startPos >= 0.f)) return EClipLoad::BadMontage;
				}

				const uint8_t curveCount = reader.Read<uint8_t>();
				if (!reader.Ok()) return EClipLoad::Truncated;
				montage.curves.resize(curveCount);
				for (MontageCurve& curve : montage.curves)
				{
					reader.Name(curve.name);
					curve.owner = reader.Read<int8_t>();
					curve.hz = reader.Read<uint32_t>();
					const uint32_t valueCount = reader.Read<uint32_t>();
					if (!reader.Ok() || valueCount > reader.Left() / sizeof(float)) return EClipLoad::Truncated;
					if (curve.owner < -1 || curve.owner >= (int)trackCount || curve.hz == 0 || valueCount == 0) return EClipLoad::BadMontage;
					curve.values.resize(valueCount);
					reader.Bytes(curve.values.data(), valueCount * sizeof(float));
				}
			}

			// 쏜 뒤 윗몸 덮기 자세
			bundle.overrideClip = reader.Read<int16_t>();
			bundle.overrideSlot = reader.Read<uint8_t>();
			if (!reader.Ok()) return EClipLoad::Truncated;
			if (bundle.overrideClip < -1 || bundle.overrideClip >= (int)clipCount) return EClipLoad::BadMontage;
			if (bundle.overrideClip >= 0 && bundle.overrideSlot >= (int)slotCount) return EClipLoad::BadMontage;
			if (bundle.overrideClip < 0) bundle.overrideSlot = -1;
		}

		char tail[4];
		reader.Bytes(tail, 4);
		if (!reader.Ok()) return EClipLoad::Truncated;
		if (std::memcmp(tail, "MOAE", 4) != 0 || reader.Left() != 0) return EClipLoad::BadTail;
		return EClipLoad::Ok;
	}

	int CAnimClips::FindBone(const char* name) const
	{
		for (size_t b = 0; b < _bones.size(); ++b)
		{
			if (_bones[b].name == name) return (int)b;
		}
		return -1;
	}

	int CAnimClips::FindSlot(const char* name) const
	{
		for (size_t s = 0; s < _slots.size(); ++s)
		{
			if (_slots[s].name == name) return (int)s;
		}
		return -1;
	}

	const ClipBundle* CAnimClips::FindBundle(const char* name) const
	{
		for (const ClipBundle& bundle : _bundles)
		{
			if (bundle.name == name) return &bundle;
		}
		return nullptr;
	}

	size_t CAnimClips::KeyBytes() const
	{
		size_t total = 0;
		for (const ClipBundle& bundle : _bundles)
		{
			for (const AnimClip& clip : bundle.clips) total += clip.keys.size() * sizeof(BoneKey);
		}
		return total;
	}

	void CAnimClips::Sample(const ClipBundle& bundle, const AnimClip& clip, double time, BoneTransform* out) const
	{
		const uint32_t boneCount = (uint32_t)_bones.size();
		const double hz = bundle.sampleHz;
		const double length = clip.length;
		const double t = std::max(0.0, std::min(time, length));
		const uint32_t lo = std::min((uint32_t)(t * hz), clip.samples - 2);
		const double t0 = lo / hz;
		const double t1 = std::min((lo + 1) / hz, length);
		const double span = t1 - t0;
		const double f = span > 1e-9 ? std::max(0.0, std::min((t - t0) / span, 1.0)) : 0.0;

		const BoneKey* a = clip.Keys(lo, boneCount);
		const BoneKey* b = clip.Keys(lo + 1, boneCount);
		for (uint32_t i = 0; i < boneCount; ++i)
		{
			const BoneKey& ka = a[i];
			const BoneKey& kb = b[i];
			BoneTransform& o = out[i];
			for (int c = 0; c < 3; ++c) o.p[c] = (double)ka.p[c] + ((double)kb.p[c] - (double)ka.p[c]) * f;

			double dot = 0;
			for (int c = 0; c < 4; ++c) dot += (double)ka.q[c] * (double)kb.q[c];
			const double sign = dot < 0 ? -1.0 : 1.0;
			double sum = 0;
			for (int c = 0; c < 4; ++c)
			{
				o.q[c] = (double)ka.q[c] + ((double)kb.q[c] * sign - (double)ka.q[c]) * f;
				sum += o.q[c] * o.q[c];
			}
			const double norm = std::sqrt(sum);
			for (int c = 0; c < 4; ++c) o.q[c] /= norm;
		}
	}
}
