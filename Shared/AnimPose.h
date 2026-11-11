#pragma once

#include "AnimClips.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// 애니 상태 → 22부위 자세. 서버 · 화면 · 봇이 이 한 벌을 같이 쓴다(D32).
// - 옮긴 원본 = MOBSimpleAnimInstance.cpp 의 Evaluate 차례(걸음 섞기 → 점프 쌓기 → 착지 충격 → 조준 → 손 붙이기 → 뿌리 → 발 디딤)와 엔진 함수 5개.
//   같은 입력에 언리얼과 같은 자세가 나오는지는 Tools/AnimCheck + check_pose.py 로 대조한다(0.1cm · 0.3도).
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다. 한 객체는 한 스레드에서만 쓴다(작업 공간을 들고 있다).
namespace mo
{
	inline constexpr int kAnimWalkClips = 5;		// 서기 · 앞 · 뒤 · 왼 · 오른
	inline constexpr int kAnimLayerMax = 4;			// 점프 · 전환이 겹칠 때 함께 섞이는 칸 수의 상한

	// 쌓기 한 칸. clip = 점프 5개 + 전환 12개의 번호(ACLIP_JUMP_START 기준), -1 = 걷기 · 서기 자리
	struct AnimLayer
	{
		int16_t clip = -1;
		float time = 0;
		float fade = 1;		// 0~1 아래 칸을 덮은 정도
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
		// 클립 자료와 묶음을 고른다. 묶음이 없거나 클립 수가 ACLIP_COUNT 와 다르거나 손 뼈가 없으면 false
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

	private:
		void BlendWalk(const AnimPoseInput& input, BoneTransform* pose);
		void ApplyJump(const AnimPoseInput& input, BoneTransform* pose);
		void ApplyRecovery(const AnimPoseInput& input, BoneTransform* pose);
		void AccumulateAim(BoneTransform* pose, uint32_t firstClip, const float pitches[3], float pitch, float weight);
		void ApplyHandIK(BoneTransform* pose);
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
		std::vector<BoneTransform> _a;
		std::vector<BoneTransform> _b;
		std::vector<BoneTransform> _component;
	};
}
