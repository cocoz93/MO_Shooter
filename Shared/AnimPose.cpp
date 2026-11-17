#include "AnimPose.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mo
{
	namespace
	{
		constexpr float kZeroWeight = 0.00001f;			// 엔진 ZERO_ANIMWEIGHT_THRESH
		constexpr float kRecoveryWeight = 0.65f;		// MOBSimpleAnimInstance.cpp RECOVERY_WEIGHT
		constexpr float kRecoveryBlendSeconds = 0.2f;	// RECOVERY_BLEND_SECONDS
		constexpr float kIdleBreakBlend = 0.4f;			// IDLE_BREAK_BLEND
		constexpr double kSmallNumber = 1.e-8;			// 엔진 UE_SMALL_NUMBER
		constexpr double kKindaSmall = 1.e-4;			// 엔진 UE_KINDA_SMALL_NUMBER / DOUBLE_KINDA_SMALL_NUMBER
		constexpr uint8_t kAdditiveNone = 0;			// AAT_None
		constexpr uint8_t kAdditiveMeshRotation = 2;	// AAT_RotationOffsetMeshSpace
		constexpr uint8_t kProfileBlendMask = 2;		// 엔진 EBlendProfileMode::BlendMask

		// 끼움 자리 이름 표 — 처음엔 이 표 차례대로 얹었다(플랜 「구조」의 차례, 제 판단 · D43). 지금 차례는 EvaluateLocal 이 Lyra 그래프대로 정한다(D72)
		// 다섯째 AdditiveHitReact(맞은 쪽 반응, 컨텐츠 E-2)는 앞 넷과 따로 조준 뒤 · 손 뒤처리 앞에 얹는다(EvaluateLocal) — 차례는 Lyra 그래프 덤프에서 읽었다(원본 기준)
		constexpr const char* kSlotOrder[] = { "FullBody", "FullBodyAdditivePreAim", "UpperBody", "UpperBodyAdditive", "AdditiveHitReact" };

		float Smooth(float x)
		{
			x = std::max(0.f, std::min(x, 1.f));
			return x * x * (3.f - 2.f * x);
		}

		// 언리얼 FQuat 의 a * b 와 같다(b 를 먼저 돌리고 a)
		void QuatMul(const double a[4], const double b[4], double out[4])
		{
			const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
			const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
			const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
			const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
			out[0] = x;
			out[1] = y;
			out[2] = z;
			out[3] = w;
		}

		void QuatInverse(const double q[4], double out[4])
		{
			out[0] = -q[0];
			out[1] = -q[1];
			out[2] = -q[2];
			out[3] = q[3];
		}

		void QuatNormalize(double q[4])
		{
			const double sum = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
			if (sum >= kSmallNumber)
			{
				const double scale = 1.0 / std::sqrt(sum);
				for (int c = 0; c < 4; ++c) q[c] *= scale;
			}
			else
			{
				q[0] = q[1] = q[2] = 0;
				q[3] = 1;
			}
		}

		void Cross(const double a[3], const double b[3], double out[3])
		{
			const double x = a[1] * b[2] - a[2] * b[1];
			const double y = a[2] * b[0] - a[0] * b[2];
			const double z = a[0] * b[1] - a[1] * b[0];
			out[0] = x;
			out[1] = y;
			out[2] = z;
		}

		double Dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		void Rotate(const double q[4], const double v[3], double out[3])
		{
			double t[3];
			Cross(q, v, t);
			for (int c = 0; c < 3; ++c) t[c] *= 2.0;
			double u[3];
			Cross(q, t, u);
			for (int c = 0; c < 3; ++c) out[c] = v[c] + q[3] * t[c] + u[c];
		}

		// 엔진 GetSafeNormal — 길이가 거의 0 이면 0 벡터를 돌려준다
		void SafeNormal(const double v[3], double out[3])
		{
			const double sum = Dot3(v, v);
			if (sum == 1.0)
			{
				std::memcpy(out, v, sizeof(double) * 3);
				return;
			}
			if (sum < kSmallNumber)
			{
				out[0] = out[1] = out[2] = 0;
				return;
			}
			const double scale = 1.0 / std::sqrt(sum);
			for (int c = 0; c < 3; ++c) out[c] = v[c] * scale;
		}

		void Sub(const double a[3], const double b[3], double out[3])
		{
			for (int c = 0; c < 3; ++c) out[c] = a[c] - b[c];
		}

		// 엔진 FQuat::FindBetweenNormals
		void FindBetweenNormals(const double a[3], const double b[3], double out[4])
		{
			const double w = 1.0 + Dot3(a, b);
			if (w >= 1e-6f)
			{
				Cross(a, b, out);
				out[3] = w;
			}
			else
			{
				// 정반대를 볼 때
				const double x = std::abs(a[0]);
				const double y = std::abs(a[1]);
				const double z = std::abs(a[2]);
				const double basisY[3] = { 0, 1, 0 };
				const double basisMinusX[3] = { -1, 0, 0 };
				Cross(a, (x > y && x > z) ? basisY : basisMinusX, out);
				out[3] = 0;
			}
			QuatNormalize(out);
		}

		void BlendOverwrite(BoneTransform* out, const BoneTransform* source, double weight, size_t count)
		{
			for (size_t i = 0; i < count; ++i)
			{
				for (int c = 0; c < 3; ++c) out[i].p[c] = source[i].p[c] * weight;
				for (int c = 0; c < 4; ++c) out[i].q[c] = source[i].q[c] * weight;
			}
		}

		// 엔진 AccumulateWithShortestRotation
		void BlendAccumulate(BoneTransform* out, const BoneTransform* source, double weight, size_t count)
		{
			for (size_t i = 0; i < count; ++i)
			{
				double q[4];
				for (int c = 0; c < 4; ++c) q[c] = source[i].q[c] * weight;
				const double dot = q[0] * out[i].q[0] + q[1] * out[i].q[1] + q[2] * out[i].q[2] + q[3] * out[i].q[3];
				const double sign = dot < 0 ? -1.0 : 1.0;
				for (int c = 0; c < 4; ++c) out[i].q[c] += q[c] * sign;
				for (int c = 0; c < 3; ++c) out[i].p[c] += source[i].p[c] * weight;
			}
		}

		void NormalizeRotations(BoneTransform* pose, size_t count)
		{
			for (size_t i = 0; i < count; ++i) QuatNormalize(pose[i].q);
		}

		// 엔진 FTransform::BlendWith(TransformVectorized.h:463) — base 를 target 쪽으로 alpha 만큼. 이동은 직선, 회전은 VectorLerpQuat(가까운 쪽 부호를 base 에 붙임) + 정규화
		void BlendWith(BoneTransform& base, const BoneTransform& target, float alpha)
		{
			if (alpha <= kZeroWeight) return;
			if (alpha >= 1.f - kZeroWeight)
			{
				base = target;
				return;
			}
			for (int c = 0; c < 3; ++c) base.p[c] = base.p[c] + (target.p[c] - base.p[c]) * alpha;
			const double dot = base.q[0] * target.q[0] + base.q[1] * target.q[1] + base.q[2] * target.q[2] + base.q[3] * target.q[3];
			const double bias = dot >= 0 ? 1.0 : -1.0;
			for (int c = 0; c < 4; ++c) base.q[c] = target.q[c] * alpha + base.q[c] * bias * (1.0 - alpha);
			QuatNormalize(base.q);
		}

		// A * B (A 를 먼저, 그다음 B. 크기는 항상 1 이다)
		void Multiply(const BoneTransform& a, const BoneTransform& b, BoneTransform& out)
		{
			double q[4];
			double p[3];
			QuatMul(b.q, a.q, q);
			Rotate(b.q, a.p, p);
			for (int c = 0; c < 3; ++c) out.p[c] = p[c] + b.p[c];
			std::memcpy(out.q, q, sizeof(q));
		}

		// a 를 other 기준으로 (엔진 GetRelativeTransform, 크기 1)
		void RelativeTo(const BoneTransform& a, const BoneTransform& other, BoneTransform& out)
		{
			double inverse[4];
			QuatInverse(other.q, inverse);
			double q[4];
			QuatMul(inverse, a.q, q);
			double delta[3];
			Sub(a.p, other.p, delta);
			Rotate(inverse, delta, out.p);
			std::memcpy(out.q, q, sizeof(q));
		}

		void FromKey(const BoneKey& key, BoneTransform& out)
		{
			for (int c = 0; c < 3; ++c) out.p[c] = key.p[c];
			for (int c = 0; c < 4; ++c) out.q[c] = key.q[c];
		}

		// 엔진 FVector::FindBestAxisVectors
		void FindBestAxisVectors(const double v[3], double axis1[3], double axis2[3])
		{
			const double nx = std::abs(v[0]);
			const double ny = std::abs(v[1]);
			const double nz = std::abs(v[2]);
			double start[3] = { 0, 0, 1 };
			if (nz > nx && nz > ny)
			{
				start[0] = 1;
				start[2] = 0;
			}
			const double along = Dot3(start, v);
			double temp[3];
			for (int c = 0; c < 3; ++c) temp[c] = start[c] - v[c] * along;
			SafeNormal(temp, axis1);
			Cross(axis1, v, axis2);
		}

		// 엔진 AnimationCore::SolveTwoBoneIK (늘리기 없음) — 위치만 구한다
		void SolveTwoBonePositions(const double root[3], const double joint[3], const double end[3], const double jointTarget[3], const double effector[3], double outJoint[3], double outEnd[3])
		{
			double temp[3];
			Sub(end, joint, temp);
			const double lowerLength = std::sqrt(Dot3(temp, temp));
			Sub(joint, root, temp);
			const double upperLength = std::sqrt(Dot3(temp, temp));

			double desiredDelta[3];
			Sub(effector, root, desiredDelta);
			double desiredLength = std::sqrt(Dot3(desiredDelta, desiredDelta));
			const double maxLength = lowerLength + upperLength;

			double desiredDir[3];
			if (desiredLength < kKindaSmall)
			{
				desiredLength = kKindaSmall;
				desiredDir[0] = 1;
				desiredDir[1] = 0;
				desiredDir[2] = 0;
			}
			else
			{
				SafeNormal(desiredDelta, desiredDir);
			}

			double targetDelta[3];
			Sub(jointTarget, root, targetDelta);
			const double targetLengthSqr = Dot3(targetDelta, targetDelta);

			double planeNormal[3];
			double bendDir[3];
			if (targetLengthSqr < kKindaSmall * kKindaSmall)
			{
				bendDir[0] = 0;
				bendDir[1] = 1;
				bendDir[2] = 0;
				planeNormal[0] = 0;
				planeNormal[1] = 0;
				planeNormal[2] = 1;
			}
			else
			{
				Cross(desiredDir, targetDelta, planeNormal);
				if (Dot3(planeNormal, planeNormal) < kKindaSmall * kKindaSmall)
				{
					FindBestAxisVectors(desiredDir, planeNormal, bendDir);
				}
				else
				{
					const double along = Dot3(targetDelta, desiredDir);
					double bend[3];
					for (int c = 0; c < 3; ++c) bend[c] = targetDelta[c] - along * desiredDir[c];
					const double sum = Dot3(bend, bend);
					if (sum > kSmallNumber)
					{
						const double scale = 1.0 / std::sqrt(sum);
						for (int c = 0; c < 3; ++c) bendDir[c] = bend[c] * scale;
					}
					else
					{
						std::memcpy(bendDir, bend, sizeof(bend));
					}
				}
			}

			std::memcpy(outEnd, effector, sizeof(double) * 3);
			std::memcpy(outJoint, joint, sizeof(double) * 3);
			if (desiredLength >= maxLength)
			{
				for (int c = 0; c < 3; ++c)
				{
					outEnd[c] = root[c] + maxLength * desiredDir[c];
					outJoint[c] = root[c] + upperLength * desiredDir[c];
				}
				return;
			}

			const double twoAB = 2.0 * upperLength * desiredLength;
			const double cosAngle = twoAB != 0.0 ? (upperLength * upperLength + desiredLength * desiredLength - lowerLength * lowerLength) / twoAB : 0.0;
			const bool reverseUpper = cosAngle < 0.0;
			const double angle = std::acos(cosAngle < -1.0 ? -1.0 : (cosAngle < 1.0 ? cosAngle : 1.0));
			const double jointLineDist = upperLength * std::sin(angle);
			const double projSqr = upperLength * upperLength - jointLineDist * jointLineDist;
			double projDist = projSqr > 0.0 ? std::sqrt(projSqr) : 0.0;
			if (reverseUpper) projDist = -projDist;
			for (int c = 0; c < 3; ++c) outJoint[c] = root[c] + projDist * desiredDir[c] + jointLineDist * bendDir[c];
		}
	}

	bool CAnimPoseEvaluator::Init(const CAnimClips& clips, const char* bundleName)
	{
		_clips = nullptr;
		_bundle = clips.FindBundle(bundleName);
		if (!_bundle || _bundle->clips.size() < ACLIP_COUNT) return false;		// 67개 뒤는 몽타주 · 덮기 자세 클립(9단계 3-a, 조준 17개는 C-3, 앉기 20개는 D-3)

		_handR = clips.FindBone("hand_r");
		_gun = clips.FindBone("ik_hand_gun");
		_ikLeft = clips.FindBone("ik_hand_l");
		_upper = clips.FindBone("upperarm_l");
		_lower = clips.FindBone("lowerarm_l");
		_hand = clips.FindBone("hand_l");
		_pelvis = clips.FindBone("pelvis");
		if (_handR < 0 || _gun < 0 || _ikLeft < 0 || _upper < 0 || _lower < 0 || _hand < 0 || _pelvis < 0) return false;

		static_assert(sizeof(kSlotOrder) / sizeof(kSlotOrder[0]) == kSlotOrderCount, "kSlotOrder 와 칸 수가 같아야 한다");
		const std::vector<ClipSlot>& slots = clips.Slots();
		for (int n = 0; n < kSlotOrderCount; ++n) _slotOrder[n] = clips.FindSlot(kSlotOrder[n]);
		for (int s = 0; s < (int)slots.size(); ++s)
		{
			bool known = s == _bundle->overrideSlot;
			for (int n = 0; n < kSlotOrderCount; ++n) known = known || _slotOrder[n] == s;
			if (!known) return false;
			if (slots[(size_t)s].profile >= 0 && clips.Profiles()[(size_t)slots[(size_t)s].profile].mode != kProfileBlendMask) return false;
		}

		_clips = &clips;
		_boneCount = clips.Bones().size();
		_a.resize(_boneCount);
		_b.resize(_boneCount);
		_base.resize(_boneCount);
		_component.resize(_boneCount);
		_meshRotations.resize(_boneCount * 12);
		return true;
	}

	void CAnimPoseEvaluator::BlendWalk(const AnimPoseInput& input, BoneTransform* pose)
	{
		const AnimClip& fidgetClip = _bundle->clips[ACLIP_FIDGET];
		const float fidget = input.fidgetTime < 0.f ? 0.f : Smooth(std::min(input.fidgetTime, fidgetClip.length - input.fidgetTime) / kIdleBreakBlend);

		bool first = true;
		int active = 0;
		const auto add = [&](const AnimClip& clip, double time, float weight)
		{
			_clips->Sample(*_bundle, clip, time, _a.data());
			if (first) BlendOverwrite(pose, _a.data(), weight, _boneCount);
			else BlendAccumulate(pose, _a.data(), weight, _boneCount);
			first = false;
			++active;
		};
		// 허리 묶음 → 대기 몸짓 → 조준 묶음 → 앉기 묶음 차례. 조준 · 앉기 묶음 비중이 0 이면 조준 전 · 앉기 전과 같은 덧셈 차례 · 같은 수다(D60 · D62 2)
		const float crouch = std::max(0.f, std::min(input.crouchBlend, 1.f));
		const float stand = 1.f - crouch;
		const float adsBlend = std::max(0.f, std::min(input.adsBlend, 1.f));
		const float hip = (1.f - adsBlend) * stand;
		const float ads = adsBlend * stand;
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			const float weight = i == 0 ? input.weights[i] * hip * (1.f - fidget) : input.weights[i] * hip;
			if (weight > kZeroWeight) add(_bundle->clips[(size_t)ACLIP_IDLE + i], input.times[i], weight);
		}
		if (input.weights[0] * hip * fidget > kZeroWeight) add(fidgetClip, std::min(input.fidgetTime, fidgetClip.length), input.weights[0] * hip * fidget);
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			const float weight = input.weights[i] * ads;
			if (weight > kZeroWeight) add(_bundle->clips[(size_t)ACLIP_ADS_IDLE + i], input.adsTimes[i], weight);
		}
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			const float weight = input.weights[i] * crouch;
			if (weight > kZeroWeight) add(_bundle->clips[(size_t)ACLIP_CROUCH_IDLE + i], input.crouchTimes[i], weight);
		}

		if (active == 0)
		{
			for (size_t b = 0; b < _boneCount; ++b) FromKey(_clips->Bones()[b].ref, pose[b]);
		}
		else if (active > 1)
		{
			NormalizeRotations(pose, _boneCount);
		}
	}

	void CAnimPoseEvaluator::ApplyJump(const AnimPoseInput& input, BoneTransform* pose)
	{
		if (input.layerCount == 0) return;

		float layerWeights[kAnimLayerMax] = {};
		float remain = 1.f;
		for (int i = input.layerCount - 1; i >= 0; --i)
		{
			const float alpha = i == 0 ? 1.f : Smooth(input.layers[i].fade);
			layerWeights[i] = remain * alpha;
			remain -= layerWeights[i];
		}

		// 차례는 언리얼 ApplyJump 와 같다 — 쌓기 칸 차례로 클립, 걷기 · 서기 자리는 맨 뒤에 한 번
		int used[kAnimLayerMax];
		int usedCount = 0;
		float baseWeight = 0.f;
		for (int i = 0; i < input.layerCount; ++i)
		{
			if (layerWeights[i] <= kZeroWeight) continue;
			if (input.layers[i].clip < 0) baseWeight += layerWeights[i];
			else used[usedCount++] = i;
		}
		if (usedCount == 0) return;

		float sum = 0.f;
		for (int n = 0; n < usedCount; ++n) sum += layerWeights[used[n]];
		if (baseWeight > kZeroWeight) sum += baseWeight;

		std::memcpy(_b.data(), pose, sizeof(BoneTransform) * _boneCount);
		for (int n = 0; n < usedCount; ++n)
		{
			const AnimLayer& layer = input.layers[used[n]];
			const uint32_t index = StackClipIndex(layer.clip);
			const AnimClip& clip = _bundle->clips[index];
			const float length = std::max(clip.length, 0.01f);
			const float time = clip.loop ? std::fmod(layer.time, length) : std::min(layer.time, length);
			_clips->Sample(*_bundle, clip, time, _a.data());
			const float weight = layerWeights[used[n]] / sum;
			if (n == 0) BlendOverwrite(pose, _a.data(), weight, _boneCount);
			else BlendAccumulate(pose, _a.data(), weight, _boneCount);
		}
		if (baseWeight > kZeroWeight) BlendAccumulate(pose, _b.data(), baseWeight / sum, _boneCount);
		if (usedCount + (baseWeight > kZeroWeight ? 1 : 0) > 1) NormalizeRotations(pose, _boneCount);
	}

	void CAnimPoseEvaluator::AccumulateAdditive(BoneTransform* pose, const BoneTransform* additive, float weight, uint8_t additiveType)
	{
		if (weight <= kZeroWeight) return;

		const bool meshRotation = additiveType == kAdditiveMeshRotation;
		const std::vector<ClipBone>& bones = _clips->Bones();
		if (meshRotation)
		{
			for (size_t b = 1; b < _boneCount; ++b) QuatMul(pose[(size_t)bones[b].parent].q, pose[b].q, pose[b].q);
		}

		const bool full = weight >= 1.f - kZeroWeight;
		for (size_t b = 0; b < _boneCount; ++b)
		{
			double delta[4];
			if (full)
			{
				for (int c = 0; c < 4; ++c) delta[c] = additive[b].q[c] * weight;
				for (int c = 0; c < 3; ++c) pose[b].p[c] += additive[b].p[c] * weight;
			}
			else
			{
				// 항등에서 weight 만큼 — 엔진 BlendFromIdentityAndAccumulate, FTransform::Blend, FQuat::FastLerp
				const double bias = additive[b].q[3] >= 0 ? 1.0 : -1.0;
				delta[0] = additive[b].q[0] * weight;
				delta[1] = additive[b].q[1] * weight;
				delta[2] = additive[b].q[2] * weight;
				delta[3] = additive[b].q[3] * weight + bias * (1.f - weight);
				QuatNormalize(delta);
				for (int c = 0; c < 3; ++c) pose[b].p[c] += additive[b].p[c] * weight;
			}
			QuatMul(delta, pose[b].q, pose[b].q);
		}

		if (meshRotation)
		{
			for (size_t b = _boneCount - 1; b > 0; --b)
			{
				double inverse[4];
				QuatInverse(pose[(size_t)bones[b].parent].q, inverse);
				QuatMul(inverse, pose[b].q, pose[b].q);
			}
		}
		NormalizeRotations(pose, _boneCount);
	}

	void CAnimPoseEvaluator::ApplyRecovery(const AnimPoseInput& input, BoneTransform* pose)
	{
		const AnimClip& clip = _bundle->clips[ACLIP_RECOVERY];
		if (input.recoveryTime < 0.f || clip.additive == kAdditiveNone) return;
		const float weight = kRecoveryWeight * Smooth(std::min(input.recoveryTime, clip.length - input.recoveryTime) / kRecoveryBlendSeconds);
		if (weight <= kZeroWeight) return;
		_clips->Sample(*_bundle, clip, std::min(input.recoveryTime, clip.length), _a.data());
		AccumulateAdditive(pose, _a.data(), weight, clip.additive);
	}

	const float* CAnimPoseEvaluator::SlotMask(int slot) const
	{
		const int profile = _clips->Slots()[(size_t)slot].profile;
		if (profile < 0) return nullptr;
		const int test = slot == _bundle->overrideSlot ? _overrideProfileTest : _slotProfileTest;
		return _clips->Profiles()[(size_t)(test >= 0 ? test : profile)].weights.data();
	}

	// 엔진 BlendPosesPerBoneFilter(AnimationRuntime.cpp:1870) 섞을 자세 하나 — 뼈 비중 = 층 비중 × 뼈 가림 값(UpdateDesiredBoneWeight, :1821), 0~1 로 자른다.
	// 실제 경로는 메시 기준 회전 갈래(:2213~2257) — 이동 · 크기는 국소로 섞고 회전만 부모까지 곱한 메시 기준으로 섞는다. 국소 갈래(:2362~2388)보다 Lyra 기록에 가깝다(D50).
	// 시험 깃발 kPoseTestLocalBlend 이면 국소 갈래. out 은 base · target 과 같은 곳이어도 된다
	void CAnimPoseEvaluator::BlendPerBone(const BoneTransform* base, const BoneTransform* target, float layerWeight, const float* mask, BoneTransform* out)
	{
		const bool meshRotation = (_poseTest & kPoseTestLocalBlend) == 0;
		const std::vector<ClipBone>& bones = _clips->Bones();
		double* sourceRot = _meshRotations.data();
		double* targetRot = sourceRot + _boneCount * 4;
		double* blendRot = targetRot + _boneCount * 4;
		for (size_t b = 0; b < _boneCount; ++b)
		{
			const float weight = std::max(0.f, std::min(layerWeight * (mask ? mask[b] : 1.f), 1.f));
			const BoneTransform baseAtom = base[b];
			const BoneTransform targetAtom = target[b];
			BoneTransform blendAtom = baseAtom;
			if (weight >= 1.f - kZeroWeight) blendAtom = targetAtom;
			else if (weight > kZeroWeight) BlendWith(blendAtom, targetAtom, weight);
			if (meshRotation)
			{
				const int parent = bones[b].parent;
				double* s = sourceRot + b * 4;
				double* t = targetRot + b * 4;
				double* m = blendRot + b * 4;
				if (parent >= 0)
				{
					QuatMul(sourceRot + (size_t)parent * 4, baseAtom.q, s);
					QuatMul(targetRot + (size_t)parent * 4, targetAtom.q, t);
				}
				else
				{
					std::memcpy(s, baseAtom.q, sizeof(double) * 4);
					std::memcpy(t, targetAtom.q, sizeof(double) * 4);
				}
				if (weight <= kZeroWeight) std::memcpy(m, s, sizeof(double) * 4);
				else if (weight >= 1.f - kZeroWeight) std::memcpy(m, t, sizeof(double) * 4);
				else
				{
					// FQuat::FastLerp + Normalize
					const double dot = s[0] * t[0] + s[1] * t[1] + s[2] * t[2] + s[3] * t[3];
					const double bias = dot >= 0 ? 1.0 : -1.0;
					for (int c = 0; c < 4; ++c) m[c] = t[c] * weight + s[c] * bias * (1.0 - weight);
					QuatNormalize(m);
				}
				if (parent >= 0)
				{
					double inverse[4];
					QuatInverse(blendRot + (size_t)parent * 4, inverse);
					QuatMul(inverse, m, blendAtom.q);
					QuatNormalize(blendAtom.q);
				}
			}
			out[b] = blendAtom;
		}
	}

	// 쏜 뒤 윗몸 덮기 — 덮는 자세(그 클립 시각 0)를 비중 × 자리 비중 표로 뼈마다 섞는다. 앉았으면 앉은 덮기 자세(D62 5 — Lyra UpdateHipFireRaiseWeaponPose)
	void CAnimPoseEvaluator::ApplyOverride(const AnimPoseInput& input, BoneTransform* pose)
	{
		if (input.overrideWeight <= kZeroWeight || _bundle->overrideClip < 0 || _bundle->overrideSlot < 0) return;
		const size_t clip = input.crouched ? (size_t)ACLIP_CROUCH_OVERRIDE : (size_t)_bundle->overrideClip;
		_clips->Sample(*_bundle, _bundle->clips[clip], 0.0, _a.data());
		BlendPerBone(pose, _a.data(), input.overrideWeight, SlotMask(_bundle->overrideSlot), pose);
	}

	// 끼움 자리 하나. 엔진 GetSlotWeight(AnimInstanceProxy.cpp:2103) · SlotEvaluatePose(:1885) 를 옮겼다 —
	// 그 자리에 줄이 있는 몽타주의 비중 합이 1 을 넘으면 나누고, 일반 줄은 비중대로 섞은 뒤 남은 몫(1 − 일반 비중 합)을 아래 자세로 채우고, 덧붙임 줄은 그 위에 비중만큼 얹는다.
	// 자리에 비중 표가 있으면 그 결과를 아래 자세와 뼈별로 섞는다(뼈별 섞기 노드의 층 비중은 1 로 둔다 — 그래프를 안 읽어 모름, 제 판단)
	bool CAnimPoseEvaluator::ApplySlot(const AnimPoseInput& input, int slot, BoneTransform* pose, bool perBone)
	{
		if (slot < 0) return false;

		struct Entry
		{
			const ClipBundle* bundle;
			const AnimClip* clip;
			double time;		// 줄 클립 시각. 음수 = 줄 구간 밖(엔진은 기준 자세)
			float weight;
		};
		Entry entries[kAnimMontageMax];
		int count = 0;
		float total = 0.f;
		float nonAdditive = 0.f;
		const std::vector<ClipBundle>& bundles = _clips->Bundles();
		for (int m = 0; m < input.montageCount && m < kAnimMontageMax; ++m)
		{
			const AnimMontageSlot& view = input.montages[m];
			const ClipMontage* montage = view.bundle >= 0 && view.bundle < (int)bundles.size() ? bundles[(size_t)view.bundle].FindMontage(view.kind) : nullptr;
			if (!montage)
			{
				if (slot == _slotOrder[0]) ++_montageMisses;		// 자리마다 세면 한 칸이 넷으로 불어난다 — 첫 자리에서만 센다
				continue;
			}
			const MontageTrack* track = nullptr;
			for (const MontageTrack& candidate : montage->tracks)
			{
				if (candidate.slot == slot)
				{
					track = &candidate;
					break;
				}
			}
			if (!track) continue;

			// 엔진 FAnimTrack::GetAnimationPose(AnimCompositeBase.cpp:535) — 몽타주 시각을 줄 길이로 자르고, 줄 구간 안이면 클립 시각 = 시작 + (시각 − 줄 시작) × 재생 속도(:37).
			// 클립 자체 재생 배율은 1 로 본다 — 15개 몽타주 길이가 전부 「(클립 끝 − 시작) ÷ 줄 재생 속도」와 같다(3-a 덤프, 계산값)
			const ClipBundle& bundle = bundles[(size_t)view.bundle];
			const AnimClip& clip = bundle.clips[track->clip];
			const float rate = std::abs(track->playRate) < 1.e-8f ? 1.f : track->playRate;
			const float trackLength = (track->clipEnd - track->clipStart) / std::abs(rate);
			const float clamped = std::max(0.f, std::min(view.position, track->startPos + trackLength));
			double time = -1.0;
			if (clamped >= track->startPos && clamped <= track->startPos + trackLength)
			{
				const float delta = clamped - track->startPos;
				time = rate > 0.f ? track->clipStart + delta * rate : track->clipEnd + delta * rate;
			}
			else
			{
				++_trackOutside;
			}
			entries[count++] = { &bundle, &clip, time, view.weight };
			total += view.weight;
			if (clip.additive == kAdditiveNone) nonAdditive += view.weight;
		}
		if (count == 0) return false;

		float slotWeight = total;
		if (slotWeight > 1.f)
		{
			nonAdditive /= slotWeight;
			slotWeight = 1.f;
		}
		if (slotWeight <= kZeroWeight) return false;
		const float sourceWeight = std::max(0.f, std::min(1.f - nonAdditive, 1.f));
		const bool normalize = total > 1.f + kZeroWeight;

		const auto sample = [this](const Entry& entry, BoneTransform* out)
		{
			if (entry.time >= 0.0) _clips->Sample(*entry.bundle, *entry.clip, entry.time, out);
			else for (size_t b = 0; b < _boneCount; ++b) FromKey(_clips->Bones()[b].ref, out[b]);
		};

		std::memcpy(_base.data(), pose, sizeof(BoneTransform) * _boneCount);
		int blended = 0;
		for (int e = 0; e < count; ++e)
		{
			if (entries[e].clip->additive != kAdditiveNone) continue;
			const float weight = normalize ? entries[e].weight / total : entries[e].weight;
			sample(entries[e], _a.data());
			if (blended == 0) BlendOverwrite(pose, _a.data(), weight, _boneCount);
			else BlendAccumulate(pose, _a.data(), weight, _boneCount);
			++blended;
		}
		if (blended > 0)
		{
			if (sourceWeight > kZeroWeight)
			{
				BlendAccumulate(pose, _base.data(), sourceWeight, _boneCount);
				++blended;
			}
			if (blended > 1) NormalizeRotations(pose, _boneCount);
		}
		for (int e = 0; e < count; ++e)
		{
			if (entries[e].clip->additive == kAdditiveNone) continue;
			const float weight = normalize ? entries[e].weight / total : entries[e].weight;
			sample(entries[e], _a.data());
			AccumulateAdditive(pose, _a.data(), weight, entries[e].clip->additive);
		}

		const float* mask = SlotMask(slot);
		if (perBone && mask) BlendPerBone(_base.data(), pose, 1.f, mask, pose);
		return true;
	}

	void CAnimPoseEvaluator::AccumulateAim(BoneTransform* pose, uint32_t firstClip, const float pitches[3], float pitch, float weight)
	{
		const AnimClip& center = _bundle->clips[firstClip + 1];
		if (weight <= kZeroWeight || center.additive == kAdditiveNone) return;
		const int other = pitch >= pitches[1] ? 0 : 2;
		const float span = pitches[other] - pitches[1];
		const float alpha = std::abs(span) > (float)kSmallNumber ? std::max(0.f, std::min((pitch - pitches[1]) / span, 1.f)) : 0.f;

		_clips->Sample(*_bundle, center, 0.0, _a.data());
		_clips->Sample(*_bundle, _bundle->clips[firstClip + other], 0.0, _b.data());
		const float weightOne = 1.f - alpha;
		BlendOverwrite(_a.data(), _a.data(), weightOne, _boneCount);
		BlendAccumulate(_a.data(), _b.data(), 1.f - weightOne, _boneCount);
		NormalizeRotations(_a.data(), _boneCount);
		AccumulateAdditive(pose, _a.data(), weight, center.additive);
	}

	void CAnimPoseEvaluator::ComponentFromLocal(const BoneTransform* local, BoneTransform* component) const
	{
		const std::vector<ClipBone>& bones = _clips->Bones();
		component[0] = local[0];
		for (size_t b = 1; b < _boneCount; ++b) Multiply(local[b], component[(size_t)bones[b].parent], component[b]);
	}

	// 총 뼈는 늘 오른손 자리로 옮기고, 왼팔은 leftAlpha 만큼만 당긴다(몽타주 곡선 DisableLHandIK, D45).
	// 비중은 엔진 FCSPose::LocalBlendCSBoneTransforms(BonePose.h:769) — 문턱 아래면 손대지 않고, 1 이면 그대로 두고, 그 사이면 고친 뼈의 부모 기준값을 옛 값 쪽으로 1 − 비중만큼 섞는다
	void CAnimPoseEvaluator::ApplyHandIK(BoneTransform* pose, float leftAlpha)
	{
		BoneTransform* cs = _component.data();
		ComponentFromLocal(pose, cs);

		const std::vector<ClipBone>& bones = _clips->Bones();
		BoneTransform ikLeftInGun;
		RelativeTo(cs[_ikLeft], cs[_gun], ikLeftInGun);
		const BoneTransform newGun = cs[_handR];
		BoneTransform newIkLeft;
		Multiply(ikLeftInGun, newGun, newIkLeft);

		BoneTransform upper = cs[_upper];
		BoneTransform lower = cs[_lower];
		BoneTransform hand = cs[_hand];
		double outJoint[3];
		double outEnd[3];
		SolveTwoBonePositions(upper.p, lower.p, hand.p, lower.p, newIkLeft.p, outJoint, outEnd);

		// 위팔 — 어깨 자리는 그대로, 팔꿈치 쪽으로 돌린다
		{
			double oldDirection[3], newDirection[3], temp[3], delta[4];
			Sub(lower.p, upper.p, temp);
			SafeNormal(temp, oldDirection);
			Sub(outJoint, upper.p, temp);
			SafeNormal(temp, newDirection);
			FindBetweenNormals(oldDirection, newDirection, delta);
			QuatMul(delta, upper.q, upper.q);
		}
		// 아래팔 — 팔꿈치 자리를 옮기고, 손 쪽으로 돌린다
		{
			double oldDirection[3], newDirection[3], temp[3], delta[4];
			Sub(hand.p, lower.p, temp);
			SafeNormal(temp, oldDirection);
			Sub(outEnd, outJoint, temp);
			SafeNormal(temp, newDirection);
			FindBetweenNormals(oldDirection, newDirection, delta);
			QuatMul(delta, lower.q, lower.q);
			std::memcpy(lower.p, outJoint, sizeof(outJoint));
		}
		// 손 — 회전은 그대로 두고 자리만 옮긴다
		std::memcpy(hand.p, outEnd, sizeof(outEnd));

		// 고친 뼈의 부모 기준값만 다시 구한다. 자식은 부모 기준값이 그대로라 따라온다(엔진 SafeSetCSBoneTransforms)
		// 총 뼈와 왼팔은 서로의 조상이 아니라(총 뼈는 뿌리 쪽 IK 뼈 아래) 따로 넣어도 비중 1 의 결과는 한 번에 넣은 것과 같다
		RelativeTo(newGun, cs[bones[(size_t)_gun].parent], pose[_gun]);
		if (leftAlpha < kZeroWeight) return;
		const BoneTransform oldUpper = pose[_upper];
		const BoneTransform oldLower = pose[_lower];
		const BoneTransform oldHand = pose[_hand];
		RelativeTo(upper, cs[bones[(size_t)_upper].parent], pose[_upper]);
		RelativeTo(lower, upper, pose[_lower]);
		RelativeTo(hand, lower, pose[_hand]);
		if (leftAlpha >= 1.f - kZeroWeight) return;
		const float back = 1.f - leftAlpha;
		BlendWith(pose[_upper], oldUpper, back);
		BlendWith(pose[_lower], oldLower, back);
		BlendWith(pose[_hand], oldHand, back);
	}

	void CAnimPoseEvaluator::ApplyPlant(const AnimPoseInput& input, BoneTransform* pose) const
	{
		const bool zero = std::abs(input.plantOffset[0]) <= kKindaSmall && std::abs(input.plantOffset[1]) <= kKindaSmall && std::abs(input.plantOffset[2]) <= kKindaSmall;
		if (zero || _clips->Bones()[(size_t)_pelvis].parent != 0) return;
		const double offset[3] = { input.plantOffset[0], input.plantOffset[1], input.plantOffset[2] };
		double inverse[4];
		QuatInverse(pose[0].q, inverse);
		double local[3];
		Rotate(inverse, offset, local);
		for (int c = 0; c < 3; ++c) pose[_pelvis].p[c] += local[c];
	}

	void CAnimPoseEvaluator::EvaluateLocal(const AnimPoseInput& input, BoneTransform* outLocal)
	{
		BlendWalk(input, outLocal);
		ApplyJump(input, outLocal);
		ApplyRecovery(input, outLocal);
		// 차례 = Lyra ABP_Mannequin_Base AnimGraph 핀 연결(D72 — C:\tmp\mob\e2\graph_chain.py 로 따라감, 원본 기준):
		//   윗몸 섞기(LayeredBoneBlend_2) → 사격 덧붙임(FullBodyAdditivePreAim) → 조준 층(FullBody_Aiming) → 맞은 쪽 반응(AdditiveHitReact)
		//   → 덧붙임 층(FullBodyAdditives, 안 옮김) → 통째(FullBody) → 손 · 발 뒤처리. 쏜 뒤 덮기는 Lyra 그래프에 없는 이 파일 몫이라(D45) 자리를 D50 대로 둔다(사격 덧붙임 뒤 · 조준 앞)
		// 옛 차례(D50 — 통째 → 사격 덧붙임 → 윗몸 → 윗몸 덧붙임, 자리마다 뼈별 섞기)는 시험 깃발 kPoseTestD50Order. 시험 깃발은 후보 판을 다시 만들 때만
		const bool d50 = (_poseTest & kPoseTestD50Order) != 0;
		const bool overrideOn = (_poseTest & kPoseTestNoOverride) == 0;
		const bool overrideEarly = (_poseTest & kPoseTestOverrideEarly) != 0;
		const bool upperAfterAim = d50 && (_poseTest & kPoseTestUpperAfterAim) != 0;
		const auto applyUpperD50 = [&]()
		{
			if ((_poseTest & kPoseTestChainUpper) == 0 || _slotOrder[2] < 0)
			{
				ApplySlot(input, _slotOrder[2], outLocal);		// 윗몸(UpperBody)
				ApplySlot(input, _slotOrder[3], outLocal);		// 윗몸 덧붙임(UpperBodyAdditive)
				return;
			}
			std::memcpy(_b.data(), outLocal, sizeof(BoneTransform) * _boneCount);
			ApplySlot(input, _slotOrder[2], outLocal, false);
			ApplySlot(input, _slotOrder[3], outLocal, false);
			const float* mask = SlotMask(_slotOrder[2]);
			if (mask) BlendPerBone(_b.data(), outLocal, 1.f, mask, outLocal);
		};
		if (overrideOn && overrideEarly) ApplyOverride(input, outLocal);
		if (d50)
		{
			ApplySlot(input, _slotOrder[0], outLocal);		// 통째(FullBody)
			ApplySlot(input, _slotOrder[1], outLocal);		// 사격 덧붙임(FullBodyAdditivePreAim)
			if (!upperAfterAim) applyUpperD50();
		}
		else
		{
			// LayeredBoneBlend_2(가림 = 윗몸 자리 비중 표 — 그래프는 UpperBodyLowerBodySplitMask · 메시 기준 회전) 바탕 = 걸음 + UpperBodyAdditive 자리(ApplyAdditive_1, 가림 없음),
			// 섞을 쪽 = 걸음 위 UpperBody 자리. 그래서 윗몸 덧붙임은 가림이 1 보다 작은 뼈(골반 · 허리 · 빗장)에만 남는다. 두 자리가 다 비면 걸음 그대로라 건너뛴다
			// ApplyAdditive_1 의 비중(UpperbodyDynamicAdditiveWeight — 몽타주 중 · 땅 위면 1, 아니면 초당 6 으로 0 에 다가감, 그래프 원본 기준)은 안 옮겼다 — 늘 1(D72 남은 차이)
			if (_slotOrder[2] >= 0)
			{
				std::memcpy(_b.data(), outLocal, sizeof(BoneTransform) * _boneCount);
				const bool upper = ApplySlot(input, _slotOrder[2], _b.data(), false);		// 윗몸(UpperBody)
				const bool additive = ApplySlot(input, _slotOrder[3], outLocal, false);	// 윗몸 덧붙임(UpperBodyAdditive)
				if (upper || additive) BlendPerBone(outLocal, _b.data(), 1.f, SlotMask(_slotOrder[2]), outLocal);
			}
			else
			{
				ApplySlot(input, _slotOrder[3], outLocal, false);
			}
			ApplySlot(input, _slotOrder[1], outLocal);		// 사격 덧붙임(FullBodyAdditivePreAim)
		}
		if (overrideOn && !overrideEarly) ApplyOverride(input, outLocal);
		AccumulateAim(outLocal, ACLIP_AIM_UP, _bundle->aimPitch, input.pitch, input.aimBlend);
		AccumulateAim(outLocal, ACLIP_READY_UP, _bundle->aimPitch + 3, input.pitch, 1.f - input.aimBlend);
		if (upperAfterAim) applyUpperD50();
		ApplySlot(input, _slotOrder[4], outLocal);		// 맞은 쪽 반응(AdditiveHitReact, 컨텐츠 E-2)
		if (!d50) ApplySlot(input, _slotOrder[0], outLocal);		// 통째(FullBody) — 부활 몽타주가 반응까지 덮는다(Lyra 와 같다, E-2-b spawn_hit 조각)
		if (_handIK) ApplyHandIK(outLocal, (_poseTest & kPoseTestNoHandAlpha) != 0 ? 1.f : input.handLeftAlpha);
		FromKey(_clips->Bones()[0].ref, outLocal[0]);
		ApplyPlant(input, outLocal);
	}

	void CAnimPoseEvaluator::BlendLocal(const BoneTransform* a, const BoneTransform* b, float alpha, BoneTransform* out) const
	{
		BlendOverwrite(out, a, 1.0 - alpha, _boneCount);
		BlendAccumulate(out, b, alpha, _boneCount);
		NormalizeRotations(out, _boneCount);
	}

	double CAnimPoseEvaluator::BodyReach(const ClipBody& body, const BodyPose& pose)
	{
		const double fromCenter = std::sqrt(pose.center[0] * pose.center[0] + pose.center[1] * pose.center[1] + pose.center[2] * pose.center[2]);
		switch (body.kind)		// EBodyShape(Shared/Weapon.h) 차례 — Sphere, Sphyl, Box
		{
		case 0: return fromCenter + body.a;
		case 1: return fromCenter + body.a + body.b * 0.5;
		default: return fromCenter + 0.5 * std::sqrt((double)body.a * body.a + (double)body.b * body.b + (double)body.c * body.c);
		}
	}

	double CAnimPoseEvaluator::ComputeBoundRadius(double crouchMeshLiftZ)
	{
		// 표본 자세의 최대에 더하는 여유. 실제 판은 클립 여럿을 섞고 발 디딤으로 골반을 약 4cm 내린다.
		// 그 몫을 덮으려고 둔 값이다(제 판단) — 실제 경로의 최대는 AnimCheck state 의 STATEBOUND 로 대조한다
		constexpr double kBoundMargin = 10.0;

		const std::vector<ClipBody>& bodies = _clips->Bodies();
		std::vector<BoneTransform> local(_boneCount);
		std::vector<BodyPose> poses(bodies.size());
		double reach = 0;
		const auto measureAt = [&](AnimPoseInput input, double meshLiftZ)
		{
			for (const float aimBlend : { 0.f, 1.f })
			{
				for (const float pitch : { -90.f, 0.f, 90.f })
				{
					input.aimBlend = aimBlend;
					input.pitch = pitch;
					EvaluateLocal(input, local.data());
					BodiesFromLocal(local.data(), poses.data(), meshLiftZ);
					for (size_t i = 0; i < bodies.size(); ++i) reach = std::max(reach, BodyReach(bodies[i], poses[i]));
				}
			}
		};
		const auto measure = [&](const AnimPoseInput& input) { measureAt(input, 0.0); };
		// 앉기 클립(D62 7) — 앉은 캡슐(올림 crouchMeshLiftZ)과 선 캡슐(올림 0) 둘 다. 앉기 전환 첫 자세는 선 자세인데 캡슐은 이미 앉았고, 일어서기 전환은 그 반대다
		const auto measureCrouched = [&](const AnimPoseInput& input)
		{
			measureAt(input, 0.0);
			if (crouchMeshLiftZ != 0.0) measureAt(input, crouchMeshLiftZ);
		};
		const auto sampleTime = [this](const AnimClip& clip, uint32_t sample) { return std::min((float)((double)sample / _bundle->sampleHz), clip.length); };

		const std::vector<AnimClip>& clips = _bundle->clips;
		for (uint32_t c = 0; c < (uint32_t)kAnimWalkClips; ++c)
		{
			for (uint32_t s = 0; s < clips[c].samples; ++s)
			{
				AnimPoseInput input;
				for (float& weight : input.weights) weight = 0.f;
				input.weights[c] = 1.f;
				input.times[c] = sampleTime(clips[c], s);
				measure(input);
			}
		}
		// 조준 서기 · 걸음 넷(D60)
		for (uint32_t c = 0; c < (uint32_t)kAnimWalkClips; ++c)
		{
			const AnimClip& clip = clips[ACLIP_ADS_IDLE + c];
			for (uint32_t s = 0; s < clip.samples; ++s)
			{
				AnimPoseInput input;
				for (float& weight : input.weights) weight = 0.f;
				input.weights[c] = 1.f;
				input.adsBlend = 1.f;
				input.adsTimes[c] = sampleTime(clip, s);
				measure(input);
			}
		}
		// 앉은 서기 · 걸음 넷(D62)
		for (uint32_t c = 0; c < (uint32_t)kAnimWalkClips; ++c)
		{
			const AnimClip& clip = clips[ACLIP_CROUCH_IDLE + c];
			for (uint32_t s = 0; s < clip.samples; ++s)
			{
				AnimPoseInput input;
				for (float& weight : input.weights) weight = 0.f;
				input.weights[c] = 1.f;
				input.crouchBlend = 1.f;
				input.crouched = true;
				input.crouchTimes[c] = sampleTime(clip, s);
				measureCrouched(input);
			}
		}
		// 쌓기 칸 전부 — 점프 · 허리 전환 · 조준 전환 · 앉은 전환 · 서기 안 앉기 · 일어서기
		for (int stack = 0; stack < kStackClipCount; ++stack)
		{
			const AnimClip& clip = clips[StackClipIndex(stack)];
			for (uint32_t s = 0; s < clip.samples; ++s)
			{
				AnimPoseInput input;
				input.layerCount = 1;
				input.layers[0].clip = (int16_t)stack;
				input.layers[0].time = sampleTime(clip, s);
				input.layers[0].fade = 1.f;
				if (stack >= kStackAdsEnd) measureCrouched(input);
				else measure(input);
			}
		}
		for (uint32_t s = 0; s < clips[ACLIP_RECOVERY].samples; ++s)
		{
			AnimPoseInput input;
			input.recoveryTime = sampleTime(clips[ACLIP_RECOVERY], s);
			measure(input);
		}
		for (uint32_t s = 0; s < clips[ACLIP_FIDGET].samples; ++s)
		{
			AnimPoseInput input;
			input.fidgetTime = sampleTime(clips[ACLIP_FIDGET], s);
			measure(input);
		}
		// 몽타주(9단계 3-b) — 서기 위에 몽타주 하나를 비중 1 로, 시각은 표본 간격마다. 덮기 · 왼손 비중은 끝값 0 · 1 을 다 본다.
		// 모든 묶음의 몽타주 전부(조각 4) — 무기를 바꾸면 서버는 그 틱에 묶음을 바꾸고(D46), 앞 무기의 쏘기 · 재장전은 새 묶음 평가기 위에서 빠지는 중으로 남는다.
		// 아래 서기는 허리 · 조준 · 앉음 셋(D60 — 조준한 채 쏜다, D62 — 앉은 채 쏜다 · 앉은 덮기 자세)
		const std::vector<ClipBundle>& bundles = _clips->Bundles();
		for (size_t m = 0; m < bundles.size(); ++m)
		{
			for (const ClipMontage& montage : bundles[m].montages)
			{
				const uint32_t samples = (uint32_t)std::ceil((double)montage.length * _bundle->sampleHz) + 1;
				for (uint32_t s = 0; s < samples; ++s)
				{
					for (int stance = 0; stance < 3; ++stance)		// 0 = 허리, 1 = 조준, 2 = 앉음
					{
						for (const float overrideWeight : { 0.f, 1.f })
						{
							for (const float handLeftAlpha : { 0.f, 1.f })
							{
								AnimPoseInput input;
								input.adsBlend = stance == 1 ? 1.f : 0.f;
								input.crouchBlend = stance == 2 ? 1.f : 0.f;
								input.crouched = stance == 2;
								input.montageCount = 1;
								input.montages[0].bundle = (int8_t)m;
								input.montages[0].kind = montage.kind;
								input.montages[0].position = std::min((float)((double)s / _bundle->sampleHz), montage.length);
								input.montages[0].weight = 1.f;
								input.overrideWeight = overrideWeight;
								input.handLeftAlpha = handLeftAlpha;
								if (stance == 2) measureCrouched(input);
								else measure(input);
							}
						}
					}
				}
			}
		}
		return reach + kBoundMargin;
	}

	void CAnimPoseEvaluator::BodiesFromLocal(const BoneTransform* local, BodyPose* outBodies, double meshLiftZ)
	{
		BoneTransform* cs = _component.data();
		ComponentFromLocal(local, cs);

		BoneTransform mesh;
		FromKey(_clips->MeshPlacement(), mesh);
		if (meshLiftZ != 0.0) mesh.p[2] += meshLiftZ;		// 0 이면 손대지 않는다 — 앉기 전과 비트까지 같게
		const std::vector<ClipBody>& bodies = _clips->Bodies();
		for (size_t i = 0; i < bodies.size(); ++i)
		{
			const ClipBody& body = bodies[i];
			BoneTransform world;
			Multiply(cs[(size_t)body.bone], mesh, world);

			const double center[3] = { body.center[0], body.center[1], body.center[2] };
			double rotated[3];
			Rotate(world.q, center, rotated);
			for (int c = 0; c < 3; ++c) outBodies[i].center[c] = rotated[c] + world.p[c];

			const double bodyQ[4] = { body.q[0], body.q[1], body.q[2], body.q[3] };
			double q[4];
			QuatMul(world.q, bodyQ, q);
			const double x[3] = { 1, 0, 0 };
			const double y[3] = { 0, 1, 0 };
			const double z[3] = { 0, 0, 1 };
			Rotate(q, x, outBodies[i].axisX);
			Rotate(q, y, outBodies[i].axisY);
			Rotate(q, z, outBodies[i].axisZ);
		}
	}

	// BodiesFromLocal 과 같은 곱(뼈 성분 자리 × 메시 자리) — 부위의 뼈 쪽 몫만
	void CAnimPoseEvaluator::HandRightFromLocal(const BoneTransform* local, double outPosition[3], double meshLiftZ)
	{
		BoneTransform* cs = _component.data();
		ComponentFromLocal(local, cs);
		BoneTransform mesh;
		FromKey(_clips->MeshPlacement(), mesh);
		if (meshLiftZ != 0.0) mesh.p[2] += meshLiftZ;
		BoneTransform world;
		Multiply(cs[(size_t)_handR], mesh, world);
		for (int c = 0; c < 3; ++c) outPosition[c] = world.p[c];
	}
}
