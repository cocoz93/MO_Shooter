#pragma once

#include "AnimClips.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// 애니 상태 → 22부위 자세. 서버 · 화면 · 봇이 이 한 벌을 같이 쓴다(D32).
// - 옮긴 원본 = MOBSimpleAnimInstance.cpp 의 Evaluate 차례(걸음 섞기 → 점프 쌓기 → 착지 충격 → 조준 → 손 붙이기 → 뿌리 → 발 디딤)와 엔진 함수 5개.
//   같은 입력에 언리얼과 같은 자세가 나오는지는 Tools/AnimCheck + check_pose.py 로 대조한다(0.1cm · 0.3도).
// - 9단계 3-b 에서 더한 것 = 착지 충격 뒤에 끼움 자리 넷(통째 · 사격 덧붙임 · 윗몸 · 윗몸 덧붙임) → 쏜 뒤 윗몸 덮기 → 조준, 손 붙이기의 왼팔 비중.
//   자리 하나 = 엔진 SlotEvaluatePose(AnimInstanceProxy.cpp:1885) + 비중 표가 있으면 뼈별 섞기(BlendPosesPerBoneFilter 메시 기준 회전 갈래, AnimationRuntime.cpp:2213).
//   차례 · 섞기 갈래 · 비중 표는 애니 그래프를 읽은 값이 아니다(D43) — 후보를 바꿔 Lyra 원본 기록과의 22부위 차이가 가장 작은 것을 골랐다(제 판단, D50)
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다. 한 객체는 한 스레드에서만 쓴다(작업 공간을 들고 있다).
namespace mo
{
	inline constexpr int kAnimWalkClips = 5;		// 서기 · 앞 · 뒤 · 왼 · 오른
	inline constexpr int kAnimLayerMax = 4;			// 점프 · 전환이 겹칠 때 함께 섞이는 칸 수의 상한
	inline constexpr int kAnimMontageMax = 3;		// 함께 도는 몽타주 수의 상한 — Lyra 기록 세 무기의 동시 최대(D45)

	// 쌓기 한 칸. clip = 점프 5개 + 전환 12개의 번호(ACLIP_JUMP_START 기준), -1 = 걷기 · 서기 자리
	struct AnimLayer
	{
		int16_t clip = -1;
		float time = 0;
		float fade = 1;		// 0~1 아래 칸을 덮은 정도
	};

	// 자세에 들어가는 몽타주 한 칸. 몽타주는 묶음 번호 · 종류로 찾는다(무기를 바꾸는 틱엔 새 무기의 꺼내기가 앞 무기 묶음 중에 돈다)
	struct AnimMontageSlot
	{
		int8_t bundle = -1;		// CAnimClips::Bundles() 차례
		uint8_t kind = 0;		// EMontageKind
		float position = 0;		// 몽타주 시각(초)
		float weight = 0;		// 0~1 몽타주 비중
	};

	// 자세 계산의 입력. 언리얼 쪽 재생 상태(FMOBSimpleAnimProxy)와 칸마다 같은 뜻이다
	struct AnimPoseInput
	{
		float weights[kAnimWalkClips] = { 1.f, 0.f, 0.f, 0.f, 0.f };
		float times[kAnimWalkClips] = {};
		float fidgetTime = -1.f;		// -1 = 대기 몸짓 안 함
		int layerCount = 0;
		AnimLayer layers[kAnimLayerMax];
		float recoveryTime = -1.f;		// -1 = 착지 충격 안 얹음
		float aimBlend = 1.f;			// 1 = 허리 사격 조준만, 0 = 느슨한 조준만
		float pitch = 0.f;
		float plantOffset[3] = {};		// 메시 기준 골반 이동량(cm). 0 = 안 내림
		int montageCount = 0;			// 비중이 엔진 문턱(1e-5)보다 큰 몽타주만 — 엔진도 그 아래는 자세 계산에 안 넣는다
		AnimMontageSlot montages[kAnimMontageMax];
		float overrideWeight = 0.f;		// 쏜 뒤 윗몸 덮기 비중(D45). 0 = 안 덮음
		float handLeftAlpha = 1.f;		// 왼손 붙이기 비중 = 1 − 앞 프레임 몽타주 곡선 DisableLHandIK
	};

	// 몸 기준(캡슐 중심, 요 0) 부위 하나의 자리
	struct BodyPose
	{
		double center[3];
		double axisX[3];
		double axisY[3];
		double axisZ[3];
	};

	class CAnimPoseEvaluator
	{
	public:
		// 클립 자료와 묶음을 고른다. 묶음이 없거나 클립 수가 ACLIP_COUNT 보다 적거나 손 뼈가 없으면 false.
		// 끼움 자리 표에 평가 차례(kSlotOrder)에도 덮기 자리에도 없는 자리가 있거나, 자리의 비중 표가 뼈 가림(방식 2)이 아니어도 false — 몽타주 줄을 말없이 버리지 않으려고
		bool Init(const CAnimClips& clips, const char* bundleName);

		size_t BoneCount() const { return _boneCount; }
		size_t BodyCount() const { return _clips ? _clips->Bodies().size() : 0; }

		void EvaluateLocal(const AnimPoseInput& input, BoneTransform* outLocal);
		void BodiesFromLocal(const BoneTransform* local, BodyPose* outBodies);

		// 두 자세를 뼈별로 섞는다 — 틱 사이 보간(D32). out 은 a · b 와 다른 곳이어야 한다
		void BlendLocal(const BoneTransform* a, const BoneTransform* b, float alpha, BoneTransform* out) const;

		// 부위 하나가 캡슐 중심에서 닿는 거리(크기 포함)
		static double BodyReach(const ClipBody& body, const BodyPose& pose);
		// 이 묶음의 어떤 자세에서도 몸이 캡슐 중심에서 닿을 수 있는 최대 거리(cm) — 탄이 안 닿는 대상의 자세 계산을 건너뛰는 데 쓴다.
		// 전 클립 표본을 돌므로 시작할 때 한 번만 부른다
		double ComputeBoundRadius();

		// 대조 시험의 일부러 틀림용 — 손 붙이기를 끈다
		void SetHandIKEnabledForTest(bool enabled) { _handIK = enabled; }

		// 끼움 자리 후보 고르기용(3-b, AnimCheck --poseTest · --slotProfile · --overrideProfile). 0 · -1 이 실제 경로다(고른 까닭 = D50)
		static constexpr uint32_t kPoseTestLocalBlend = 1;			// 뼈별 섞기를 국소 갈래로(실제 경로는 메시 기준 회전 갈래)
		static constexpr uint32_t kPoseTestUpperAfterAim = 2;		// 윗몸 · 윗몸 덧붙임 자리를 조준 뒤로
		static constexpr uint32_t kPoseTestNoOverride = 4;			// 쏜 뒤 덮기를 끈다
		static constexpr uint32_t kPoseTestChainUpper = 8;			// 윗몸 → 윗몸 덧붙임을 이어 평가한 뒤 뼈별 섞기를 한 번만
		static constexpr uint32_t kPoseTestNoHandAlpha = 16;		// 왼팔 비중을 늘 1 로
		static constexpr uint32_t kPoseTestOverrideEarly = 32;		// 쏜 뒤 덮기를 끼움 자리 앞(착지 충격 바로 뒤)으로(실제 경로는 끼움 자리 뒤 · 조준 앞)
		void SetPoseTestFlags(uint32_t flags) { _poseTest = flags; }
		void SetSlotProfileForTest(int profile) { _slotProfileTest = profile; }			// 비중 표가 있는 끼움 자리(덮기 자리 빼고)를 이 표로(-1 = 파일 그대로)
		void SetOverrideProfileForTest(int profile) { _overrideProfileTest = profile; }	// 쏜 뒤 덮기 자리를 이 표로(-1 = 파일 그대로)

		// 몽타주 칸이 가리키는 묶음 · 종류가 없어 버린 칸 수, 몽타주 시각이 그 줄 구간 밖이라 기준 자세를 쓴 수(엔진도 기준 자세다). 0 이어야 한다
		uint64_t MontageMisses() const { return _montageMisses; }
		uint64_t TrackOutside() const { return _trackOutside; }

	private:
		void BlendWalk(const AnimPoseInput& input, BoneTransform* pose);
		void ApplyJump(const AnimPoseInput& input, BoneTransform* pose);
		void ApplyRecovery(const AnimPoseInput& input, BoneTransform* pose);
		void ApplyOverride(const AnimPoseInput& input, BoneTransform* pose);
		void ApplySlot(const AnimPoseInput& input, int slot, BoneTransform* pose, bool perBone = true);
		const float* SlotMask(int slot) const;
		void BlendPerBone(const BoneTransform* base, const BoneTransform* target, float layerWeight, const float* mask, BoneTransform* out);
		void AccumulateAim(BoneTransform* pose, uint32_t firstClip, const float pitches[3], float pitch, float weight);
		void ApplyHandIK(BoneTransform* pose, float leftAlpha);
		void ApplyPlant(const AnimPoseInput& input, BoneTransform* pose) const;
		void AccumulateAdditive(BoneTransform* pose, const BoneTransform* additive, float weight, uint8_t additiveType);
		void ComponentFromLocal(const BoneTransform* local, BoneTransform* component) const;

		const CAnimClips* _clips = nullptr;
		const ClipBundle* _bundle = nullptr;
		size_t _boneCount = 0;
		bool _handIK = true;
		int _handR = -1;
		int _gun = -1;
		int _ikLeft = -1;
		int _upper = -1;
		int _lower = -1;
		int _hand = -1;
		int _pelvis = -1;
		static constexpr int kSlotOrderCount = 4;
		int _slotOrder[kSlotOrderCount] = { -1, -1, -1, -1 };		// 평가 차례의 끼움 자리 번호(Init 이 이름으로 찾는다, 없으면 -1)
		uint64_t _montageMisses = 0;
		uint64_t _trackOutside = 0;
		uint32_t _poseTest = 0;
		int _slotProfileTest = -1;
		int _overrideProfileTest = -1;
		std::vector<double> _meshRotations;		// 메시 기준 회전 섞기의 작업 공간 — 아래 · 위 · 섞은 회전 × 뼈 수 × 4
		std::vector<BoneTransform> _a;
		std::vector<BoneTransform> _b;
		std::vector<BoneTransform> _base;		// 끼움 자리에 들어온 아래 자세
		std::vector<BoneTransform> _component;
	};
}
