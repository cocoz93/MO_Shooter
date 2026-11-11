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
		if (!_bundle || _bundle->clips.size() != ACLIP_COUNT) return false;

		_handR = clips.FindBone("hand_r");
		_gun = clips.FindBone("ik_hand_gun");
		_ikLeft = clips.FindBone("ik_hand_l");
		_upper = clips.FindBone("upperarm_l");
		_lower = clips.FindBone("lowerarm_l");
		_hand = clips.FindBone("hand_l");
		_pelvis = clips.FindBone("pelvis");
		if (_handR < 0 || _gun < 0 || _ikLeft < 0 || _upper < 0 || _lower < 0 || _hand < 0 || _pelvis < 0) return false;

		_clips = &clips;
		_boneCount = clips.Bones().size();
		_a.resize(_boneCount);
		_b.resize(_boneCount);
		_component.resize(_boneCount);
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
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			const float weight = i == 0 ? input.weights[i] * (1.f - fidget) : input.weights[i];
			if (weight > kZeroWeight) add(_bundle->clips[(size_t)ACLIP_IDLE + i], input.times[i], weight);
		}
		if (input.weights[0] * fidget > kZeroWeight) add(fidgetClip, std::min(input.fidgetTime, fidgetClip.length), input.weights[0] * fidget);

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
			const uint32_t index = ACLIP_JUMP_START + (uint32_t)layer.clip;
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

	void CAnimPoseEvaluator::ApplyHandIK(BoneTransform* pose)
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
		RelativeTo(newGun, cs[bones[(size_t)_gun].parent], pose[_gun]);
		RelativeTo(upper, cs[bones[(size_t)_upper].parent], pose[_upper]);
		RelativeTo(lower, upper, pose[_lower]);
		RelativeTo(hand, lower, pose[_hand]);
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
		AccumulateAim(outLocal, ACLIP_AIM_UP, _bundle->aimPitch, input.pitch, input.aimBlend);
		AccumulateAim(outLocal, ACLIP_READY_UP, _bundle->aimPitch + 3, input.pitch, 1.f - input.aimBlend);
		if (_handIK) ApplyHandIK(outLocal);
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

	double CAnimPoseEvaluator::ComputeBoundRadius()
	{
		// 표본 자세의 최대에 더하는 여유. 실제 판은 클립 여럿을 섞고 발 디딤으로 골반을 약 4cm 내린다.
		// 그 몫을 덮으려고 둔 값이다(제 판단) — 실제 경로의 최대는 AnimCheck state 의 STATEBOUND 로 대조한다
		constexpr double kBoundMargin = 10.0;

		const std::vector<ClipBody>& bodies = _clips->Bodies();
		std::vector<BoneTransform> local(_boneCount);
		std::vector<BodyPose> poses(bodies.size());
		double reach = 0;
		const auto measure = [&](AnimPoseInput input)
		{
			for (const float aimBlend : { 0.f, 1.f })
			{
				for (const float pitch : { -90.f, 0.f, 90.f })
				{
					input.aimBlend = aimBlend;
					input.pitch = pitch;
					EvaluateLocal(input, local.data());
					BodiesFromLocal(local.data(), poses.data());
					for (size_t i = 0; i < bodies.size(); ++i) reach = std::max(reach, BodyReach(bodies[i], poses[i]));
				}
			}
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
		for (uint32_t c = ACLIP_JUMP_START; c < ACLIP_RECOVERY; ++c)
		{
			for (uint32_t s = 0; s < clips[c].samples; ++s)
			{
				AnimPoseInput input;
				input.layerCount = 1;
				input.layers[0].clip = (int16_t)(c - ACLIP_JUMP_START);
				input.layers[0].time = sampleTime(clips[c], s);
				input.layers[0].fade = 1.f;
				measure(input);
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
		return reach + kBoundMargin;
	}

	void CAnimPoseEvaluator::BodiesFromLocal(const BoneTransform* local, BodyPose* outBodies)
	{
		BoneTransform* cs = _component.data();
		ComponentFromLocal(local, cs);

		BoneTransform mesh;
		FromKey(_clips->MeshPlacement(), mesh);
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
}
