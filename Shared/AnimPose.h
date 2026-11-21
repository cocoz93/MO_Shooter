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
//   그 자리에 빼기 비중 표가 걸린 몽타주(대시 넷이 제 빼기로 빠질 때)가 있으면 SlotEvaluatePoseWithBlendProfiles(:1560)처럼 뼈마다 비중을 따로 낸다(L-6-b · D130)
//   차례 · 섞기 갈래 · 비중 표는 처음엔 애니 그래프를 읽은 값이 아니었다(D43) — 후보를 바꿔 Lyra 원본 기록과의 22부위 차이가 가장 작은 것을 골랐다(제 판단, D50)
// - 컨텐츠 E-2-b 에서 그래프(ABP_Mannequin_Base AnimGraph 핀 연결)를 읽어 차례 · 윗몸 짜임 · 윗몸 비중 표를 그래프대로 바꿨다(D72, 원본 기준).
//   옛 차례는 시험 깃발 kPoseTestD50Order 로 다시 만든다
// - L-12-c 에서 무기 층(FullBody_SkeletalControls)의 다리 IK(LegIK_0 — 엔진 AnimNode_LegIK)를 뿌리 뒤 · 발 디딤 앞에 더했다(원본 기준, D150)
// - L-12-e 에서 발 붙이기 장치(CR_Mannequin_FootPlant)의 다리 몫(Basic IK · 무릎 조절점 · 발 높이 스프링)을 골반 옮김 앞에 더했다(원본 기준, D153)
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다. 한 객체는 한 스레드에서만 쓴다(작업 공간을 들고 있다).
namespace mo
{
	inline constexpr int kAnimWalkClips = 5;		// 서기 · 앞 · 뒤 · 왼 · 오른
	inline constexpr int kAnimLayerMax = 4;			// 점프 · 전환이 겹칠 때 함께 섞이는 칸 수의 상한
	inline constexpr int kAnimMontageMax = 3;		// 함께 도는 몽타주 수의 상한 — Lyra 기록 세 무기의 동시 최대(D45)

	// 쌓기 한 칸. clip = 쌓기 칸 번호 0~42(점프 5 · 허리 전환 12 · 조준 전환 12 · 앉은 전환 12 · 서기 안 앉기 · 일어서기 — 묶음 클립 번호는 AnimClips.h StackClipIndex), -1 = 걷기 · 서기 자리
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
		float blendOut = -1.f;	// 빼기 진행 0~1(엔진 FAlphaBlend 선형 진행) — 빼기 비중 표가 걸린 몽타주가 빠지는 중일 때만, 아니면 -1(L-4 · 와이어 판 14).
								// 끼움 자리가 이 진행 · 몽타주의 빼기 비중 표로 뼈마다 뺀다(ApplySlot, L-6-b · D130)
	};

	// 자세 계산의 입력. 언리얼 쪽 재생 상태(FMOBSimpleAnimProxy)와 칸마다 같은 뜻이다
	struct AnimPoseInput
	{
		float weights[kAnimWalkClips] = { 1.f, 0.f, 0.f, 0.f, 0.f };
		float times[kAnimWalkClips] = {};
		// 조준 묶음(D60) — 서기 · 걸음 i 의 비중을 허리 클립 weights[i] × (1 − adsBlend) 와 조준 클립 weights[i] × adsBlend 로 나눈다
		float adsBlend = 0.f;			// 0 = 허리 클립만, 1 = 조준 클립만
		float adsTimes[kAnimWalkClips] = {};
		// 앉기 묶음(D62 2) — 조준보다 먼저 나눈다. 허리 weights[i] × (1 − adsBlend) × (1 − crouchBlend) · 조준 weights[i] × adsBlend × (1 − crouchBlend) · 앉음 weights[i] × crouchBlend
		float crouchBlend = 0.f;
		float crouchTimes[kAnimWalkClips] = {};
		bool crouched = false;			// 앉았나 — 쏜 뒤 덮기 자세를 앉은 것으로 고른다(D62 5)
		float fidgetTime = -1.f;		// -1 = 대기 몸짓 안 함(대기 몸짓은 허리 서기 몫에만 든다)
		int layerCount = 0;
		AnimLayer layers[kAnimLayerMax];
		float recoveryTime = -1.f;		// -1 = 착지 충격 안 얹음
		float aimBlend = 1.f;			// 1 = 허리 사격 조준만, 0 = 느슨한 조준만
		float pitch = 0.f;
		float plantOffset[3] = {};		// 메시 기준 골반 이동량(cm). 0 = 안 내림
		float plantAlpha = 0.f;			// 발 붙이기 장치 켜짐(엔진 컨트롤 릭 노드 알파, L-12-e) — 0 이면 장치 다리 몫을 안 돌린다. plantOffset 은 이 값을 곱한 것
		float footOffset[2] = {};		// 장치 발 높이 스프링(왼 · 오른, 뿌리 기준 z cm, 알파 곱하기 전 — ProcessFootOffset, L-12-e)
		int montageCount = 0;			// 비중이 엔진 문턱(1e-5)보다 큰 몽타주만 — 엔진도 그 아래는 자세 계산에 안 넣는다
		AnimMontageSlot montages[kAnimMontageMax];
		float overrideWeight = 0.f;		// 쏜 뒤 윗몸 덮기 비중(D45). 0 = 안 덮음
		float handLeftAlpha = 1.f;		// 왼손 붙이기 비중 = 1 − 앞 프레임 몽타주 곡선 DisableLHandIK
		float legIKAlpha = 1.f;			// 다리 IK 비중 = 1 − 앞 프레임 곡선 DisableLegIK(L-12-c — 몽타주 · 점프 클립 곡선, 장치 켜짐과 같은 값)
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
		// 클립 자료와 묶음을 고른다. 묶음이 없거나 클립 수가 ACLIP_COUNT 보다 적거나 손 뼈 · IK 발 뼈(ik_foot_l · ik_foot_r, 클립 판 9 의 L-12-c 뒤 내보내기)가 없으면 false.
		// 끼움 자리 표에 평가 차례(kSlotOrder)에도 덮기 자리에도 없는 자리가 있거나, 자리의 비중 표가 뼈 가림(방식 2)이 아니어도 false — 몽타주 줄을 말없이 버리지 않으려고
		bool Init(const CAnimClips& clips, const char* bundleName);

		size_t BoneCount() const { return _boneCount; }
		size_t BodyCount() const { return _clips ? _clips->Bodies().size() : 0; }

		void EvaluateLocal(const AnimPoseInput& input, BoneTransform* outLocal);
		// meshLiftZ = 메시를 캡슐 중심 기준으로 더 올리는 값(cm) = 선 반높이 − 지금 반높이(D62 7). 앉으면 캡슐 중심이 내려가도 메시는 세상에서 그대로라 몸 기준으로는 올라간다.
		// 자세 계산은 캡슐을 모르므로 부르는 쪽이 넘긴다. 0 = 선 캡슐(앉기 전과 같은 수)
		void BodiesFromLocal(const BoneTransform* local, BodyPose* outBodies, double meshLiftZ = 0.0);
		// 오른손 뼈(hand_r) 자리 — 몸 기준(캡슐 중심, 요 0). meshLiftZ 는 BodiesFromLocal 과 같은 뜻.
		// 근접 쓸기 시작점이다 — GA_Melee 는 GetSocketLocation("hand_r") 인데 메시에 그 이름 소켓이 없어 뼈 자리다(e1g3, 소켓 없음 82번 · 뼈 자리와 차이 0)
		void HandRightFromLocal(const BoneTransform* local, double outPosition[3], double meshLiftZ = 0.0);
		// 두 발 높이 — 부품 공간(메시 기준)에서 발 뼈가 뿌리 뼈보다 얼마나 위인가(cm), [0] 왼발 · [1] 오른발. 발 붙이기 장치(CAnimState, L-6-c-2)가 읽는다.
		// Lyra 장치는 IK 발 뼈(ik_foot_l · ik_foot_r) 자리에서 쓸기를 하는데 여기선 foot_l · foot_r 로 센다 — 그렇게 셈해도 장치 켬 ↔ 끔 기록 차가 같았다(2492프레임 최대 0.001cm, 기록 대조).
		// 그때(L-6-c-2)는 클립에 IK 발 뼈가 없었다. 지금은 다리 IK(L-12-c)가 장치 앞에서 발을 IK 발 뼈로 당긴다 — 장치가 켜지는 비중 1 프레임엔 두 자리가 같다(발이 닿을 때)
		void FootHeightsFromLocal(const BoneTransform* local, double outHeights[2]);
		// 이 계산기가 고른 묶음 — 발 붙이기 장치가 상태의 묶음과 같은 계산기를 받았는지 본다
		const ClipBundle* Bundle() const { return _bundle; }

		// 두 자세를 뼈별로 섞는다 — 틱 사이 보간(D32). out 은 a · b 와 다른 곳이어야 한다
		void BlendLocal(const BoneTransform* a, const BoneTransform* b, float alpha, BoneTransform* out) const;

		// 부위 하나가 캡슐 중심에서 닿는 거리(크기 포함)
		static double BodyReach(const ClipBody& body, const BodyPose& pose);
		// 이 묶음의 어떤 자세에서도 몸이 캡슐 중심에서 닿을 수 있는 최대 거리(cm) — 탄이 안 닿는 대상의 자세 계산을 건너뛰는 데 쓴다.
		// 전 클립 표본을 돌므로 시작할 때 한 번만 부른다. crouchMeshLiftZ = 앉은 캡슐의 메시 올림(BodiesFromLocal 의 meshLiftZ) — 앉기 클립은 올림 0 · 이 값 둘 다로 잰다
		double ComputeBoundRadius(double crouchMeshLiftZ = 0.0);

		// 대조 시험의 일부러 틀림용 — 손 붙이기를 끈다
		void SetHandIKEnabledForTest(bool enabled) { _handIK = enabled; }

		// 끼움 자리 후보 고르기용(3-b, AnimCheck --poseTest · --slotProfile · --overrideProfile). 0 · -1 이 실제 경로다(고른 까닭 = D50 · 차례는 D72)
		static constexpr uint32_t kPoseTestLocalBlend = 1;			// 뼈별 섞기를 국소 갈래로(실제 경로는 메시 기준 회전 갈래)
		static constexpr uint32_t kPoseTestUpperAfterAim = 2;		// 윗몸 · 윗몸 덧붙임 자리를 조준 뒤로(옛 차례 kPoseTestD50Order 에서만)
		static constexpr uint32_t kPoseTestNoOverride = 4;			// 쏜 뒤 덮기를 끈다
		static constexpr uint32_t kPoseTestChainUpper = 8;			// 윗몸 → 윗몸 덧붙임을 이어 평가한 뒤 뼈별 섞기를 한 번만(옛 차례 kPoseTestD50Order 에서만)
		static constexpr uint32_t kPoseTestNoHandAlpha = 16;		// 왼팔 비중을 늘 1 로
		static constexpr uint32_t kPoseTestD50Order = 64;			// 옛 차례(D50) — 통째 → 사격 덧붙임 → 윗몸 → 윗몸 덧붙임, 자리마다 뼈별 섞기. 옛 판과 같게 하려면 --slotProfile=UpperBodyMask 도
		static constexpr uint32_t kPoseTestOverrideLate = 128;		// 쏜 뒤 덮기를 옛 자리(끼움 자리 뒤 · 조준 앞, L-6-a 전 실제 경로)로. 실제 경로는 끼움 자리 앞(착지 충격 바로 뒤).
																	// 옛 값 32(kPoseTestOverrideEarly — 그때 「앞으로」 시험, G-4-b 기록의 --poseTest=32)는 지금 실제 경로라 뺐다 — 32 는 비워 둔다.
																	// L-6-a 앞에 잰 판을 다시 만들려면 그 깃발에 128 을 더한다(예: 옛 차례 64 → 192)
		static constexpr uint32_t kPoseTestUniformBlendOut = 256;	// 빼기 비중 표를 안 쓴다 — 빠지는 몽타주도 모든 뼈를 몽타주 비중으로(L-6-b 전 실제 경로). L-6-b 앞에 잰 판은 256 을 더한다
		static constexpr uint32_t kPoseTestNoLegIK = 512;			// 다리 IK 를 끈다(L-12-c 전 실제 경로). L-12-c 앞에 잰 판은 512 를 더한다
		static constexpr uint32_t kPoseTestNoLegAlpha = 1024;		// 다리 IK 비중을 늘 1 로(곡선 DisableLegIK 를 안 봄)
		static constexpr uint32_t kPoseTestNoPlantLegs = 2048;		// 발 붙이기 장치의 다리 몫을 끈다(L-12-e 전 실제 경로). L-12-e 앞에 잰 판은 2048 을 더한다
		void SetPoseTestFlags(uint32_t flags) { _poseTest = flags; }
		void SetSlotProfileForTest(int profile) { _slotProfileTest = profile; }			// 비중 표가 있는 끼움 자리(덮기 자리 빼고)를 이 표로(-1 = 파일 그대로)
		void SetOverrideProfileForTest(int profile) { _overrideProfileTest = profile; }	// 쏜 뒤 덮기 자리를 이 표로(-1 = 파일 그대로)

		// 몽타주 칸이 가리키는 묶음 · 종류가 없어 버린 칸 수, 몽타주 시각이 그 줄 구간 밖이라 기준 자세를 쓴 수(엔진도 기준 자세다). 0 이어야 한다
		uint64_t MontageMisses() const { return _montageMisses; }
		uint64_t TrackOutside() const { return _trackOutside; }
		// 다리 IK(L-12-c) 셈 — 다리를 고친 수(셈 한 번에 다리 하나씩) · 그중 다리가 곧아 엔진이 앞 프레임 굽힘 방향을 읽었을 수(여기선 안 둔다 — ApplyLegIK 주석) ·
		// IK 발이 다리 길이 밖이라 곧게 편 수
		uint64_t LegIKSolves() const { return _legIKSolves; }
		uint64_t LegIKStraight() const { return _legIKStraight; }
		uint64_t LegIKUnreachable() const { return _legIKUnreachable; }
		// 장치 다리 몫(L-12-e) 셈 — 다리를 고친 수(셈 한 번에 다리 하나씩) · 닿는 비율이 늘리기 시작(1.0)을 넘은 수 · 다리가 곧아 굽힘 쪽을 무릎 조절점으로 잡은 수
		uint64_t PlantLegSolves() const { return _plantLegSolves; }
		uint64_t PlantLegStretched() const { return _plantLegStretched; }
		uint64_t PlantLegExtended() const { return _plantLegExtended; }
		double PlantLegLength(int i) const { return _plantLegLength[i]; }	// 장치 마디 길이(0 허벅지 → 종아리 · 1 종아리 → 발)

	private:
		void BlendWalk(const AnimPoseInput& input, BoneTransform* pose);
		void ApplyJump(const AnimPoseInput& input, BoneTransform* pose);
		void ApplyRecovery(const AnimPoseInput& input, BoneTransform* pose);
		void ApplyOverride(const AnimPoseInput& input, BoneTransform* pose);
		bool ApplySlot(const AnimPoseInput& input, int slot, BoneTransform* pose, bool perBone = true);		// 자세를 바꿨으면 true
		const float* SlotMask(int slot) const;
		void BlendPerBone(const BoneTransform* base, const BoneTransform* target, float layerWeight, const float* mask, BoneTransform* out);
		void AccumulateAim(BoneTransform* pose, uint32_t firstClip, const float pitches[3], float pitch, float weight);
		void ApplyHandIK(BoneTransform* pose, float leftAlpha);
		void ApplyLegIK(BoneTransform* pose, float alpha);
		void ApplyPlantLegs(const AnimPoseInput& input, BoneTransform* pose);
		void ApplyPlant(const AnimPoseInput& input, BoneTransform* pose) const;
		void AccumulateAdditive(BoneTransform* pose, const BoneTransform* additive, float weight, uint8_t additiveType);
		void AccumulateAdditivePerBone(BoneTransform* pose, const BoneTransform* additive, const float* weights, uint8_t additiveType);
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
		int _footLeft = -1;
		int _footRight = -1;
		int _legs[2][4] = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 } };	// 다리 IK 다리 둘(Lyra 정의 차례 — 왼 · 오른) × 발 · 종아리 · 허벅지 · IK 발
		double _plantLegLength[2] = {};		// 장치 Basic IK 마디 길이(허벅지 → 종아리 · 종아리 → 발) — 장치 초기화가 왼다리 처음 자세에서 잰 ThighLength · CalfLength(두 다리 같이)
		static constexpr int kSlotOrderCount = 5;
		int _slotOrder[kSlotOrderCount] = { -1, -1, -1, -1, -1 };		// 평가 차례의 끼움 자리 번호(Init 이 이름으로 찾는다, 없으면 -1)
		uint64_t _montageMisses = 0;
		uint64_t _trackOutside = 0;
		uint64_t _legIKSolves = 0;
		uint64_t _legIKStraight = 0;
		uint64_t _legIKUnreachable = 0;
		uint64_t _plantLegSolves = 0;
		uint64_t _plantLegStretched = 0;
		uint64_t _plantLegExtended = 0;
		uint32_t _poseTest = 0;
		int _slotProfileTest = -1;
		int _overrideProfileTest = -1;
		std::vector<double> _meshRotations;		// 메시 기준 회전 섞기의 작업 공간 — 아래 · 위 · 섞은 회전 × 뼈 수 × 4
		std::vector<BoneTransform> _a;
		std::vector<BoneTransform> _b;
		std::vector<BoneTransform> _base;		// 끼움 자리에 들어온 아래 자세
		std::vector<float> _boneWeights;		// 빼기 비중 표 길의 뼈별 비중 — (몽타주 칸 수 + 아래 자세 + 일반 줄 합) × 뼈 수
		std::vector<BoneTransform> _component;
	};
}
